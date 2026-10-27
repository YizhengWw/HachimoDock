//! Public media DNS, including fake-IP proxy networks. Never connect to the fake
//! address: resolve over authenticated HTTPS, validate, then pin the real answer.
use std::{
    collections::HashMap,
    io::Read,
    net::{IpAddr, SocketAddr, ToSocketAddrs},
    sync::{mpsc, Mutex, OnceLock},
    time::{Duration, Instant},
};

type Answers = Result<Vec<IpAddr>, String>;
type Job = (String, mpsc::SyncSender<Answers>);
static SYSTEM: OnceLock<mpsc::SyncSender<Job>> = OnceLock::new();
static CACHE: OnceLock<Mutex<HashMap<String, (Instant, Vec<IpAddr>)>>> = OnceLock::new();

fn fake_ip(ip: IpAddr) -> bool {
    matches!(ip, IpAddr::V4(v) if v.octets()[0] == 198 && (18..=19).contains(&v.octets()[1]))
}
pub(crate) fn public_name(host: &str) -> bool {
    host.len() <= 253
        && host.contains('.')
        && !host.ends_with('.')
        && ![
            "local",
            "localhost",
            "internal",
            "lan",
            "home",
            "test",
            "invalid",
            "onion",
        ]
        .iter()
        .any(|s| host.ends_with(&format!(".{s}")))
        && host.split('.').all(|s| {
            !s.is_empty()
                && s.len() <= 63
                && !s.starts_with('-')
                && !s.ends_with('-')
                && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-')
        })
}
fn system(host: &str) -> Answers {
    // One bounded worker, not one detached DNS thread per stream/segment. A stuck
    // OS resolver cannot create unbounded workers or block cancellation forever.
    let sender = SYSTEM.get_or_init(|| {
        let (tx, rx) = mpsc::sync_channel::<Job>(8);
        std::thread::spawn(move || {
            for (host, reply) in rx {
                let result = (host.as_str(), 443)
                    .to_socket_addrs()
                    .map(|a| a.map(|s| s.ip()).collect())
                    .map_err(|_| "系统域名解析失败".into());
                let _ = reply.try_send(result);
            }
        });
        tx
    });
    let (tx, rx) = mpsc::sync_channel(1);
    sender
        .try_send((host.into(), tx))
        .map_err(|_| "系统域名解析繁忙")?;
    rx.recv_timeout(Duration::from_secs(3))
        .map_err(|_| "系统域名解析超时")?
}
fn decode(host: &str, body: &[u8]) -> Result<(Vec<IpAddr>, u64), String> {
    let value: serde_json::Value =
        serde_json::from_slice(body).map_err(|_| "安全域名解析响应无效")?;
    let question = if value["Question"].is_array() {
        &value["Question"][0]
    } else {
        &value["Question"]
    };
    if value["Status"] != 0
        || value["TC"] == true
        || question["name"].as_str().map(|s| s.trim_end_matches('.')) != Some(host)
        || question["type"] != 1
    {
        return Err("安全域名解析失败".into());
    }
    let mut ttl = 60;
    let mut ips = Vec::new();
    for answer in value["Answer"].as_array().ok_or("电台没有公网地址")? {
        if answer["type"] != 1 {
            continue;
        }
        let ip: IpAddr = answer["data"]
            .as_str()
            .ok_or("解析地址无效")?
            .parse()
            .map_err(|_| "解析地址无效")?;
        if !ip.is_ipv4() || !crate::media_network::public_ip(ip) {
            return Err("已拒绝非公网电台地址".into());
        }
        if ips.len() >= 32 {
            return Err("解析地址数量超限".into());
        }
        ttl = ttl.min(answer["TTL"].as_u64().unwrap_or(0));
        ips.push(ip);
    }
    if ips.is_empty() {
        return Err("电台没有公网 IPv4 地址".into());
    }
    Ok((ips, ttl))
}
fn secure(host: &str) -> Result<(Vec<IpAddr>, u64), String> {
    // Public DNS only; no account ID/key, ECS subnet or device identifiers.
    // Regional first answer avoids distant CDN edges for domestic broadcasters.
    // A private answer is a hard rejection, never a reason to try another server.
    for (resolver, ip, path) in [
        ("dns.alidns.com", [223, 5, 5, 5], "/resolve"),
        ("cloudflare-dns.com", [1, 1, 1, 1], "/dns-query"),
    ] {
        if let Ok(body) = secure_body(host, resolver, ip, path) {
            return decode(host, &body);
        }
    }
    Err("电台域名解析失败，请检查网络或代理的 DNS 设置".into())
}
fn secure_body(host: &str, resolver: &str, ip: [u8; 4], path: &str) -> Result<Vec<u8>, String> {
    // Fixed public bootstrap prevents a fake OS answer from breaking the DNS
    // service itself; TLS verification still uses the resolver hostname.
    let client = reqwest::blocking::Client::builder()
        .no_proxy()
        .resolve(resolver, SocketAddr::from((ip, 443)))
        .redirect(reqwest::redirect::Policy::none())
        .connect_timeout(Duration::from_secs(4))
        .timeout(Duration::from_secs(6))
        .build()
        .map_err(|_| "安全域名解析初始化失败")?;
    let response = client
        .get(format!("https://{resolver}{path}"))
        .query(&[("name", host), ("type", "A")])
        .header("Accept", "application/dns-json")
        .send()
        .and_then(|r| r.error_for_status())
        .map_err(|_| "电台域名解析失败，请检查网络或代理的 DNS 设置")?;
    let mut body = Vec::new();
    response
        .take(16385)
        .read_to_end(&mut body)
        .map_err(|_| "安全域名解析响应中断")?;
    if body.len() > 16384 {
        return Err("安全域名解析响应超限".into());
    }
    Ok(body)
}
pub fn resolve(host: &str, port: u16) -> Result<Vec<SocketAddr>, String> {
    let host = host
        .trim_start_matches('[')
        .trim_end_matches(']')
        .to_ascii_lowercase();
    if let Ok(ip) = host.parse::<IpAddr>() {
        return if crate::media_network::public_ip(ip) {
            Ok(vec![SocketAddr::new(ip, port)])
        } else {
            Err("已拒绝非公网电台地址".into())
        };
    }
    if !public_name(&host) {
        return Err("已拒绝本地域名或无效电台域名".into());
    }
    let cache = CACHE.get_or_init(Default::default);
    if let Some((until, ips)) = cache.lock().unwrap().get(&host) {
        if *until > Instant::now() {
            return Ok(ips.iter().map(|ip| SocketAddr::new(*ip, port)).collect());
        }
    }
    let (ips, ttl) = match system(&host) {
        Ok(ips) if !ips.is_empty() && ips.iter().all(|ip| crate::media_network::public_ip(*ip)) => {
            (ips, 30)
        }
        Ok(ips)
            if ips
                .iter()
                .any(|ip| !crate::media_network::public_ip(*ip) && !fake_ip(*ip)) =>
        {
            return Err("已拒绝非公网电台地址".into())
        }
        _ => secure(&host)?,
    };
    let mut cache = cache.lock().unwrap();
    cache.retain(|_, (until, _)| *until > Instant::now());
    if cache.len() >= 128 {
        cache.clear();
    }
    cache.insert(
        host,
        (Instant::now() + Duration::from_secs(ttl), ips.clone()),
    );
    Ok(ips
        .into_iter()
        .map(|ip| SocketAddr::new(ip, port))
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn private_literal_and_local_names_never_resolve() {
        for h in [
            "127.0.0.1",
            "[::1]",
            "198.18.0.4",
            "10.0.0.1",
            "router",
            "printer.local",
            "a.internal",
            "a.onion",
        ] {
            assert!(resolve(h, 443).is_err(), "{h}");
        }
        assert!(resolve("1.1.1.1", 443).is_ok());
    }
    #[test]
    fn secure_answers_are_bounded_public_and_question_matched() {
        let mut value = serde_json::json!({"Status":0,"Question":[{"name":"radio.example.com","type":1}],"Answer":[{"type":1,"TTL":900,"data":"1.1.1.1"}]});
        assert_eq!(
            decode("radio.example.com", &serde_json::to_vec(&value).unwrap())
                .unwrap()
                .1,
            60
        );
        assert!(decode("other.example.com", &serde_json::to_vec(&value).unwrap()).is_err());
        for ip in ["198.18.0.4", "127.0.0.1", "169.254.169.254", "10.1.2.3"] {
            value["Answer"][0]["data"] = ip.into();
            assert!(decode("radio.example.com", &serde_json::to_vec(&value).unwrap()).is_err());
        }
    }
}
