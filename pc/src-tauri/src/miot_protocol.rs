//! MIoCo cloud protocol adapted from Xiaomi miloco-miot 2026.8.6 (cloud.py/const.py).
//! Copyright (C) 2025 Xiaomi Corporation. See licenses/Xiaomi-Miloco-LICENSE.md.
//! Native implementation: no Python runtime, localhost server, camera library or credentials bundled.
use aes::cipher::{block_padding::Pkcs7, BlockDecryptMut, BlockEncryptMut, KeyIvInit};
use base64::{engine::general_purpose::STANDARD, Engine};
use rand::{rngs::OsRng, RngCore};
use rsa::{pkcs8::DecodePublicKey, Pkcs1v15Encrypt, RsaPublicKey};
use serde_json::{json, Value};
use std::time::Duration;

pub const CLIENT_ID: &str = "2882303761520431603";
pub const REDIRECT: &str = "https://mico.api.mijia.tech/login_redirect";
pub const HOST: &str = "https://mico.api.mijia.tech";
const PUBLIC_KEY: &str = "-----BEGIN PUBLIC KEY-----\nMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAzH220YGgZOlXJ4eSleFb\nBeylq4qHsVNzhPTUTy/caDb4a3GzqH6SX4GiYRilZZZrjjU2ckkr8GM66muaIuJw\nr8ZB9SSY3Hqwo32tPowpyxobTN1brmqGK146X6JcFWK/QiUYVXZlcHZuMgXLlWyn\nzTMVl2fq7wPbzZwOYFxnSRh8YEnXz6edHAqJqLEqZMP00bNFBGP+yc9xmc7ySSyw\nOgW/muVzfD09P2iWhl3x8N+fBBWpuI5HjvyQuiX8CZg3xpEeCV8weaprxMxR0epM\n3l7T6rJuPXR1D7yhHaEQj2+dyrZTeJO8D8SnOgzV5j4bp1dTunlzBXGYVjqDsRhZ\nqQIDAQAB\n-----END PUBLIC KEY-----";

async fn body(mut response: reqwest::Response) -> Result<Vec<u8>, String> {
    if !response.status().is_success() {
        return Err(format!(
            "家居服务返回 HTTP {}，请检查网络或重新授权",
            response.status().as_u16()
        ));
    }
    let mut bytes = Vec::new();
    while let Some(chunk) = response.chunk().await.map_err(|_| "家居服务响应中断")? {
        if bytes.len() + chunk.len() > 4 * 1024 * 1024 {
            return Err("家居服务响应超出安全上限".into());
        }
        bytes.extend_from_slice(&chunk);
    }
    Ok(bytes)
}

pub async fn token(data: Value) -> Result<Value, String> {
    let response = crate::llm_network::client(Duration::from_secs(25))?
        .get(format!("{HOST}/app/v2/mico/oauth/get_token"))
        .query(&[("data", data.to_string())])
        .send()
        .await
        .map_err(|_| "账号授权请求失败，请检查网络后重新授权")?;
    let value: Value =
        serde_json::from_slice(&body(response).await?).map_err(|_| "授权响应格式错误")?;
    if value["code"] != 0
        || value["result"]["access_token"]
            .as_str()
            .unwrap_or("")
            .is_empty()
        || value["result"]["refresh_token"]
            .as_str()
            .unwrap_or("")
            .is_empty()
    {
        return Err("授权未完成或已失效，请重新连接账号".into());
    }
    Ok(value["result"].clone())
}

pub async fn post(access_token: &str, path: &str, data: Value) -> Result<Value, String> {
    let mut key = [0_u8; 16];
    OsRng.fill_bytes(&mut key);
    let public = RsaPublicKey::from_public_key_pem(PUBLIC_KEY).map_err(|_| "家居协议初始化失败")?;
    let secret = public
        .encrypt(&mut OsRng, Pkcs1v15Encrypt, &key)
        .map_err(|_| "家居协议初始化失败")?;
    let encrypted = cbc::Encryptor::<aes::Aes128>::new((&key).into(), (&key).into())
        .encrypt_padded_vec_mut::<Pkcs7>(data.to_string().as_bytes());
    let response = crate::llm_network::client(Duration::from_secs(25))?
        .post(format!("{HOST}{path}"))
        .header("Content-Type", "text/plain")
        .header("User-Agent", "mico/docker")
        .header("X-Client-BizId", "micoapi")
        .header("X-Encrypt-Type", "1")
        .header("X-Client-AppId", CLIENT_ID)
        .header("X-Client-Secret", STANDARD.encode(secret))
        .header("Authorization", format!("Bearer{access_token}"))
        .body(STANDARD.encode(encrypted))
        .send()
        .await
        .map_err(|_| "家居请求未取得结果，请检查设备状态；控制操作不会自动重试")?;
    let bytes = body(response).await?;
    let cipher = STANDARD.decode(bytes).map_err(|_| "家居服务响应校验失败")?;
    let plain = cbc::Decryptor::<aes::Aes128>::new((&key).into(), (&key).into())
        .decrypt_padded_vec_mut::<Pkcs7>(&cipher)
        .map_err(|_| "家居服务响应校验失败")?;
    let result: Value = serde_json::from_slice(&plain).map_err(|_| "家居服务响应格式错误")?;
    if result["code"] != 0 {
        return Err(format!(
            "家居服务拒绝请求（代码 {}）",
            result["code"].as_i64().unwrap_or(-1)
        ));
    }
    result
        .get("result")
        .cloned()
        .ok_or_else(|| "家居服务缺少执行结果".into())
}

pub async fn spec(urn: &str) -> Result<Value, String> {
    if !urn.starts_with("urn:miot-spec-v2:device:") || urn.len() > 512 {
        return Err("设备尚未提供可用的标准能力描述".into());
    }
    let response = crate::llm_network::client(Duration::from_secs(20))?
        .get("https://miot-spec.org/miot-spec-v2/instance")
        .query(&[("type", urn)])
        .send()
        .await
        .map_err(|_| "无法获取设备能力，请稍后刷新")?;
    let result: Value =
        serde_json::from_slice(&body(response).await?).map_err(|_| "设备能力格式错误")?;
    if !result["services"].is_array() {
        return Err("该设备暂未开放标准控制能力".into());
    }
    Ok(result)
}

pub async fn urn_by_model(model: &str) -> Result<String, String> {
    if model.is_empty()
        || model.len() > 160
        || !model
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'-' | b'_'))
    {
        return Err("设备型号无法解析".into());
    }
    // Public MIOT catalog endpoint also used by upstream; not a company intranet interface.
    let response = crate::llm_network::client(Duration::from_secs(20))?
        .get("https://miot-spec.org/internal/urn-by-model-version")
        .query(&[("model", model), ("version", "0")])
        .send()
        .await
        .map_err(|_| "无法查询此型号的设备能力")?;
    let value: Value =
        serde_json::from_slice(&body(response).await?).map_err(|_| "设备型号响应无效")?;
    value["urn"]
        .as_str()
        .filter(|s| s.starts_with("urn:miot-spec-v2:device:"))
        .map(str::to_string)
        .ok_or("此型号暂未开放标准设备能力".into())
}

pub fn auth_url(device_id: &str, state: &str) -> String {
    let mut url = reqwest::Url::parse("https://account.xiaomi.com/oauth2/authorize").unwrap();
    url.query_pairs_mut().extend_pairs(&[
        ("client_id", CLIENT_ID),
        ("redirect_uri", REDIRECT),
        ("response_type", "code"),
        ("device_id", device_id),
        ("state", state),
        ("skip_confirm", "false"),
    ]);
    url.to_string()
}

pub fn check_result(result: &Value) -> Result<(), String> {
    let rows = result
        .as_array()
        .cloned()
        .unwrap_or_else(|| vec![result.clone()]);
    if rows.is_empty() {
        return Err("设备未返回结果，状态尚未确认".into());
    }
    for row in rows {
        if row["code"] != json!(0) {
            return Err(format!(
                "设备未完成操作（代码 {}），请在米家确认状态",
                row["code"].as_i64().unwrap_or(-1)
            ));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn protocol_key_is_public_and_valid() {
        assert!(RsaPublicKey::from_public_key_pem(PUBLIC_KEY).is_ok());
    }
    #[test]
    fn business_failure_is_not_success() {
        assert!(check_result(&json!([{"code":0}])).is_ok());
        for value in [json!([]), json!({}), json!([{"code":-704042011}])] {
            assert!(check_result(&value).is_err());
        }
    }
}
