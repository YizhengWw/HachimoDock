//! Shared cloud transport regressions. No real credentials in deterministic tests.
use super::*;
use tokio_tungstenite::tungstenite::{client::IntoClientRequest, handshake::derive_accept_key, Message};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use futures_util::{SinkExt, StreamExt};

fn direct(transport: CloudTransport) -> reqwest::Client {
    build_cloud_client(&Settings { mode: ProxyMode::Direct, ..Default::default() }, Some(Duration::from_secs(3)), transport).unwrap()
}

#[test]
fn all_cloud_transports_use_the_configured_proxy_without_leaking_credentials() {
    tauri::async_runtime::block_on(async {
        for transport in [CloudTransport::Api, CloudTransport::Download, CloudTransport::WebSocket] {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let address = listener.local_addr().unwrap();
            let worker = tokio::spawn(async move {
                let (mut stream, _) = listener.accept().await.unwrap();
                let mut buf = [0u8; 4096]; let size = stream.read(&mut buf).await.unwrap();
                stream.write_all(b"HTTP/1.1 407 Proxy Authentication Required\r\nContent-Length: 0\r\nConnection: close\r\n\r\n").await.unwrap();
                String::from_utf8_lossy(&buf[..size]).to_string()
            });
            let settings = Settings { mode: ProxyMode::Manual, proxy_url:format!("http://{address}"), ..Default::default() };
            let client = build_cloud_client(&settings, Some(Duration::from_secs(3)), transport).unwrap();
            let error = client.get("https://service.example.invalid/test?token=private-query")
                .header("X-Api-Key", "private-header").send().await.unwrap_err();
            let message = connection_error(&error);
            assert!(!message.contains("private") && !message.contains(&address.to_string()));
            let connect = tokio::time::timeout(Duration::from_secs(4), worker).await.unwrap().unwrap();
            assert!(connect.starts_with("CONNECT service.example.invalid:443 "));
            assert!(!connect.contains("private"));
        }
    });
}

#[test]
fn websocket_upgrade_validates_handshake_and_exchanges_frames() {
    tauri::async_runtime::block_on(async {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("ws://{}/speech", listener.local_addr().unwrap());
        let server = tokio::spawn(async move {
            let (stream, _) = listener.accept().await.unwrap();
            let mut ws = tokio_tungstenite::accept_async(stream).await.unwrap();
            assert_eq!(ws.next().await.unwrap().unwrap(), Message::Text("hello".into()));
            ws.send(Message::Binary(vec![1, 2, 3].into())).await.unwrap();
        });
        let (mut ws, _) = tokio::time::timeout(Duration::from_secs(3), upgrade_websocket(&direct(CloudTransport::WebSocket), url.into_client_request().unwrap())).await.unwrap().unwrap();
        ws.send(Message::Text("hello".into())).await.unwrap();
        assert_eq!(ws.next().await.unwrap().unwrap(), Message::Binary(vec![1, 2, 3].into()));
        server.await.unwrap();
    });
}

#[test]
fn websocket_rejects_bad_accept_and_unsolicited_extensions() {
    tauri::async_runtime::block_on(async {
        for invalid_accept in [true, false] {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let request = format!("ws://{}/speech", listener.local_addr().unwrap()).into_client_request().unwrap();
            let accept = if invalid_accept { "invalid".into() } else { derive_accept_key(request.headers()["sec-websocket-key"].as_bytes()) };
            let server = tokio::spawn(async move {
                let (mut stream, _) = listener.accept().await.unwrap();
                let mut buf = [0u8; 4096]; stream.read(&mut buf).await.unwrap();
                let extra = if invalid_accept { "" } else { "Sec-WebSocket-Extensions: permessage-deflate\r\n" };
                stream.write_all(format!("HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Accept: {accept}\r\n{extra}\r\n").as_bytes()).await.unwrap();
            });
            let error = upgrade_websocket(&direct(CloudTransport::WebSocket), request).await.unwrap_err();
            assert!(error.contains("握手校验失败"));
            server.await.unwrap();
        }
    });
}

#[test]
fn api_redirects_stop_while_downloads_follow_and_limit_loops() {
    tauri::async_runtime::block_on(async {
        for (transport, loop_redirect) in [(CloudTransport::Api, false), (CloudTransport::Download, false), (CloudTransport::Download, true)] {
            let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
            let url = format!("http://{}", listener.local_addr().unwrap());
            let server = tokio::spawn(async move {
                loop {
                    let (mut stream, _) = listener.accept().await.unwrap();
                    let mut buf = [0u8; 4096]; let n = stream.read(&mut buf).await.unwrap();
                    let response = if !loop_redirect && String::from_utf8_lossy(&buf[..n]).contains("GET /final ") {
                        "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nConnection: close\r\n\r\nPNG"
                    } else { "HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" };
                    stream.write_all(response.as_bytes()).await.unwrap();
                }
            });
            let response = direct(transport).get(&url).send().await;
            if loop_redirect { assert!(response.unwrap_err().is_redirect()); }
            else {
                let response = response.unwrap();
                match transport {
                    CloudTransport::Api => assert_eq!(response.status(), 302),
                    _ => assert_eq!(response.text().await.unwrap(), "PNG"),
                }
            }
            server.abort();
        }
    });
}

#[test]
#[ignore = "internal authenticated model catalog + asset download; no generation tasks"]
fn live_generation_transports() {
    let key = crate::internal_credentials::ark_key().expect("requires internal build");
    tauri::async_runtime::block_on(async {
        let response = crate::http_request_text("https://ark.cn-beijing.volces.com/api/v3/models".into(), Some("GET".into()),
            Some(serde_json::json!({"Authorization":format!("Bearer {key}")}).to_string()), None, Some(15000)).await.unwrap();
        assert_eq!(response.status, 200);
        let json: serde_json::Value = serde_json::from_str(&response.body).unwrap();
        let rows = json["data"].as_array().unwrap();
        assert!(rows.iter().any(|row| row["id"].as_str().is_some_and(|id| id.contains("seedance") && id.contains("fast"))));
        println!("PASS authenticated model catalog: {} rows, Fast model present", rows.len());
        let bytes = crate::download_bytes("https://portal.volccdn.com/obj/volcfe/misc/favicon.png".into()).await.unwrap();
        assert!(bytes.starts_with(b"\x89PNG\r\n\x1a\n"));
        println!("PASS cloud binary download: {} bytes", bytes.len());
    });
}

#[test]
fn desktop_http_bridge_preserves_post_headers_body_and_status() {
    tauri::async_runtime::block_on(async {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let url = format!("http://{}/task", listener.local_addr().unwrap());
        let server = tokio::spawn(async move {
            let (mut stream, _) = listener.accept().await.unwrap();
            let mut buf = Vec::new();
            loop {
                let mut chunk = [0u8; 1024]; let n = stream.read(&mut chunk).await.unwrap();
                assert!(n > 0); buf.extend_from_slice(&chunk[..n]);
                if buf.ends_with(b"{\"model\":\"fixture\"}") { break; }
            }
            let request = String::from_utf8(buf).unwrap();
            assert!(request.starts_with("POST /task "));
            assert!(request.to_lowercase().contains("authorization: bearer fixture-key"));
            stream.write_all(b"HTTP/1.1 202 Accepted\r\nContent-Length: 11\r\nConnection: close\r\n\r\n{\"id\":\"ok\"}").await.unwrap();
        });
        let response = crate::http_request_text(url, Some("POST".into()), Some("{\"Authorization\":\"Bearer fixture-key\",\"Content-Type\":\"application/json\"}".into()), Some("{\"model\":\"fixture\"}".into()), Some(3000)).await.unwrap();
        assert!(response.ok); assert_eq!(response.status, 202); assert_eq!(response.body, "{\"id\":\"ok\"}");
        server.await.unwrap();
    });
}

#[test]
fn community_certificate_file_is_feature_gated_and_ephemeral() {
    let file = community_ca_file().unwrap();
    assert_eq!(file.is_some(), cfg!(feature = "internal-network"));
    if let Some(file) = file {
        let path = file.path().to_path_buf();
        assert!(!certificates(&fs::read_to_string(&path).unwrap()).unwrap().is_empty());
        #[cfg(unix)] {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        }
        drop(file);
        assert!(!path.exists());
    }
}
