//! Shared public-network media transport. Package adapters supply declarative mappings.
use reqwest::{
    blocking::{Client, Response},
    Url,
};
use serde::{Deserialize, Serialize};
use std::{
    collections::HashMap,
    io::{Read, Write},
    net::{IpAddr, SocketAddr},
    sync::{Mutex, OnceLock},
    time::{Duration, Instant},
};

#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct Station {
    pub id: String,
    pub name: String,
    pub city: String,
    pub homepage: String,
    pub codec: String,
    pub hls: bool,
    #[serde(default)]
    pub url: String,
}
pub fn public_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(ip) => {
            let o = ip.octets();
            !ip.is_private()
                && !ip.is_loopback()
                && !ip.is_link_local()
                && !ip.is_unspecified()
                && !ip.is_broadcast()
                && !ip.is_documentation()
                && o[0] != 0
                && o[0] < 224
                && !(o[0] == 100 && (64..=127).contains(&o[1]))
                && !(o[0] == 192 && o[1] == 0 && o[2] == 0)
                && !(o[0] == 198 && (18..=19).contains(&o[1]))
        }
        IpAddr::V6(ip) => {
            let s = ip.segments();
            ip.to_ipv4_mapped().map(public_v4).unwrap_or(
                s[0] & 0xe000 == 0x2000
                    && !(s[0] == 0x2001 && (s[1] == 0xdb8 || s[1] == 0))
                    && s[0] != 0x2002,
            )
        }
    }
}
fn public_v4(ip: std::net::Ipv4Addr) -> bool {
    public_ip(IpAddr::V4(ip))
}
pub fn valid_url(raw: &str) -> Result<Url, String> {
    let u = Url::parse(raw).map_err(|_| "电台地址无效")?;
    if raw.len() > 2048
        || !matches!(u.scheme(), "http" | "https")
        || !u.username().is_empty()
        || u.password().is_some()
        || u.host_str().is_none()
        || u.fragment().is_some()
        || !matches!(u.port_or_known_default(), Some(80 | 443))
    {
        return Err("电台地址不受支持".into());
    }
    Ok(u)
}
pub fn get(raw: &str) -> Result<Response, String> {
    get_scoped(raw, None)
}
fn pinned_client(host: &str, addresses: &[SocketAddr]) -> Result<Client, String> {
    // Reuse TLS connections for HLS segments. Cache identity includes every
    // checked DNS answer; a changed answer cannot reuse an unchecked endpoint.
    static CLIENTS: OnceLock<Mutex<HashMap<String, (Instant, Client)>>> = OnceLock::new();
    let key = format!("{host}:{addresses:?}");
    let mut clients = CLIENTS.get_or_init(Default::default).lock().unwrap();
    clients.retain(|_, (at, _)| at.elapsed() < Duration::from_secs(60));
    if let Some((_, client)) = clients.get(&key) {
        return Ok(client.clone());
    }
    let client = Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .resolve_to_addrs(host, addresses)
        .connect_timeout(Duration::from_secs(5))
        .timeout(Duration::from_secs(15))
        .pool_idle_timeout(Duration::from_secs(15))
        .user_agent("HachimoDock-Radio/1.0")
        .build()
        .map_err(|_| "电台网络初始化失败")?;
    if clients.len() >= 32 {
        clients.clear();
    }
    clients.insert(key, (Instant::now(), client.clone()));
    Ok(client)
}
fn get_scoped(raw: &str, hosts: Option<&[String]>) -> Result<Response, String> {
    let mut url = valid_url(raw)?;
    for _ in 0..5 {
        let host = url.host_str().ok_or("电台缺少域名")?;
        if hosts.is_some_and(|hosts| url.scheme() != "https" || !hosts.iter().any(|h| h == host)) {
            return Err("请求超出组件声明的域名权限".into());
        }
        let addresses = crate::media_dns::resolve(host, url.port_or_known_default().unwrap())?;
        // Pin the checked answers; do not let redirects, proxies or a second DNS
        // lookup turn an untrusted catalogue entry into a local-network request.
        let client = pinned_client(host, &addresses)?;
        let response = client
            .get(url.clone())
            .header("Icy-MetaData", "0")
            .send()
            .map_err(|error| {
                #[cfg(test)]
                eprintln!("media connection diagnostic ({host}): {error:?}");
                if error.is_timeout() {
                    "电台连接超时，请换台或检查网络"
                } else {
                    "电台连接失败，请检查网络或证书配置"
                }
            })?;
        if response.status().is_redirection() {
            let location = response
                .headers()
                .get("location")
                .and_then(|v| v.to_str().ok())
                .ok_or("电台跳转无效")?;
            url = valid_url(url.join(location).map_err(|_| "电台跳转无效")?.as_str())?;
            continue;
        }
        if !response.status().is_success() {
            return Err(format!(
                "电台暂不可用（HTTP {}）",
                response.status().as_u16()
            ));
        }
        return Ok(response);
    }
    Err("电台跳转次数过多".into())
}
pub(crate) fn scoped_json(raw: &str, hosts: &[String], max: usize) -> Result<serde_json::Value, String> {
    let response=get_scoped(raw,Some(hosts))?;
    let mut data=Vec::new();
    response.take(max as u64+1).read_to_end(&mut data).map_err(|_|"目录响应中断")?;
    if data.len()>max {return Err("目录响应超过安全上限".into());}
    serde_json::from_slice(&data).map_err(|_|"目录 JSON 无效".into())
}
fn bytes(raw: &str, max: usize) -> Result<(Url, Vec<u8>), String> {
    let response = get(raw)?;
    let url = response.url().clone();
    let mut out = vec![];
    response
        .take(max as u64 + 1)
        .read_to_end(&mut out)
        .map_err(|_error| {
            #[cfg(test)]
            eprintln!("media body diagnostic ({url}): {_error:?}");
            "电台数据接收中断"
        })?;
    if out.len() > max {
        return Err("电台数据超过安全上限".into());
    }
    Ok((url, out))
}
pub fn search(
    extension: &crate::component_extension::Extension,
    city: &str,
) -> Result<Vec<Station>, String> {
    let city = city.trim();
    if city.is_empty() || city.chars().count() > 30 || city.chars().any(char::is_control) {
        return Err("请输入城市或电台名称（最多 30 字）".into());
    }
    extension.validate()?;
    let mut url = valid_url(&extension.search.endpoint)?;
    url.query_pairs_mut()
        .append_pair(&extension.search.query_param, city)
        .extend_pairs(&extension.search.params);
    let response = get_scoped(url.as_str(), Some(&extension.permissions.network_hosts))?;
    let mut data = Vec::new();
    response
        .take(512 * 1024 + 1)
        .read_to_end(&mut data)
        .map_err(|_| "搜索响应中断")?;
    if data.len() > 512 * 1024 {
        return Err("搜索响应超过安全上限".into());
    }
    let rows: serde_json::Value = serde_json::from_slice(&data).map_err(|_| "目录格式无效")?;
    if rows
        .pointer(&extension.search.items_path)
        .and_then(serde_json::Value::as_array)
        .is_none()
    {
        return Err("搜索结果与组件字段映射不符".into());
    }
    Ok(extension.map_results(&rows, city))
}
#[derive(Debug)]
pub struct Playlist {
    pub variant: Option<Url>,
    pub segments: Vec<(u64, Url)>,
    pub ended: bool,
}
pub fn playlist(base: &Url, text: &str) -> Result<Playlist, String> {
    if !text.trim_start().starts_with("#EXTM3U") {
        return Err("直播列表无效".into());
    }
    let mut out = Playlist {
        variant: None,
        segments: vec![],
        ended: false,
    };
    let mut sequence = 0u64;
    let mut variant = false;
    for line in text.lines().map(str::trim).filter(|l| !l.is_empty()) {
        if line.starts_with("#EXT-X-KEY:") && line != "#EXT-X-KEY:METHOD=NONE"
            || line.starts_with("#EXT-X-MAP:")
            || line.starts_with("#EXT-X-BYTERANGE:")
            || line == "#EXT-X-DISCONTINUITY"
        {
            return Err("该电台使用暂不支持的直播格式，请换台".into());
        }
        if let Some(n) = line.strip_prefix("#EXT-X-MEDIA-SEQUENCE:") {
            sequence = n.parse().map_err(|_| "直播序号无效")?;
        } else if line.starts_with("#EXT-X-STREAM-INF:") {
            variant = true;
        } else if line == "#EXT-X-ENDLIST" {
            out.ended = true;
        } else if !line.starts_with('#') {
            let u = valid_url(base.join(line).map_err(|_| "直播片段地址无效")?.as_str())?;
            if variant {
                if out.variant.is_none() {
                    out.variant = Some(u);
                }
                variant = false;
            } else {
                if out.segments.len() >= 100 {
                    return Err("直播列表过大".into());
                }
                out.segments.push((sequence, u));
                sequence = sequence.checked_add(1).ok_or("直播序号溢出")?;
            }
        }
    }
    Ok(out)
}
pub fn feed(
    station: &Station,
    mut pipe: impl Write,
    alive: impl Fn() -> bool,
) -> Result<(), String> {
    if !station.hls {
        // Async body reads have an inactivity timeout, not a total-duration cap.
        return tokio::runtime::Builder::new_current_thread()
            .enable_all()
            .build()
            .map_err(|_| "直播网络初始化失败")?
            .block_on(async {
                let mut url = valid_url(&station.url)?;
                for _ in 0..5 {
                    let host = url.host_str().ok_or("电台缺少域名")?;
                    let name = host.to_owned();
                    let port = url.port_or_known_default().unwrap();
                    let addresses =
                        tokio::task::spawn_blocking(move || crate::media_dns::resolve(&name, port))
                            .await
                            .map_err(|_| "电台域名解析任务失败")??;
                    let client = reqwest::Client::builder()
                        .no_proxy()
                        .redirect(reqwest::redirect::Policy::none())
                        .resolve_to_addrs(host, &addresses)
                        .connect_timeout(Duration::from_secs(5))
                        .user_agent("HachimoDock-Radio/1.0")
                        .build()
                        .map_err(|_| "电台网络初始化失败")?;
                    let mut response = tokio::time::timeout(
                        Duration::from_secs(10),
                        client.get(url.clone()).header("Icy-MetaData", "0").send(),
                    )
                    .await
                    .map_err(|_| "电台连接超时")?
                    .map_err(|_| "电台连接失败")?;
                    if response.status().is_redirection() {
                        let next = response
                            .headers()
                            .get("location")
                            .and_then(|h| h.to_str().ok())
                            .ok_or("电台跳转无效")?;
                        url = valid_url(url.join(next).map_err(|_| "电台跳转无效")?.as_str())?;
                        continue;
                    }
                    if !response.status().is_success() {
                        return Err("电台服务暂不可用".into());
                    }
                    while alive() {
                        let chunk = tokio::time::timeout(Duration::from_secs(10), response.chunk())
                            .await
                            .map_err(|_| "直播接收超时")?
                            .map_err(|_| "直播接收中断")?
                            .ok_or("直播连接已结束")?;
                        pipe.write_all(&chunk).map_err(|_| "直播解码已结束")?;
                    }
                    return Ok(());
                }
                Err("电台跳转次数过多".into())
            });
    }
    let mut url = valid_url(&station.url)?;
    let mut last = None;
    let mut depth = 0;
    while alive() {
        let (base, data) = bytes(url.as_str(), 256 * 1024)?;
        let p = playlist(
            &base,
            std::str::from_utf8(&data).map_err(|_| "直播列表编码无效")?,
        )?;
        if let Some(v) = p.variant {
            depth += 1;
            if depth > 2 {
                return Err("直播列表嵌套过多".into());
            }
            url = v;
            continue;
        }
        let start = if last.is_none() {
            p.segments.len().saturating_sub(3)
        } else {
            0
        };
        for (sequence, segment) in p.segments.into_iter().skip(start) {
            if !alive() {
                return Ok(());
            }
            if last.is_some_and(|n| sequence <= n) {
                continue;
            }
            let (_, data) = bytes(segment.as_str(), 4 * 1024 * 1024)?;
            pipe.write_all(&data).map_err(|_| "直播解码已结束")?;
            last = Some(sequence);
        }
        if p.ended {
            return Err("电台已结束直播".into());
        }
        for _ in 0..10 {
            if !alive() {
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(100));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn blocks_local_and_credentials() {
        for ip in [
            "127.0.0.1",
            "10.0.0.1",
            "169.254.169.254",
            "100.64.0.1",
            "::1",
            "::ffff:127.0.0.1",
            "fc00::1",
        ] {
            assert!(!public_ip(ip.parse().unwrap()));
        }
        for u in [
            "file:///tmp/x",
            "http://a:b@example.com/",
            "http://example.com:8080/",
            "ftp://example.com/",
        ] {
            assert!(valid_url(u).is_err());
        }
        assert!(public_ip("8.8.8.8".parse().unwrap()));
    }
    #[test]
    fn hls_sequences_and_relative_paths() {
        let base = Url::parse("https://example.com/live/index.m3u8").unwrap();
        let p = playlist(
            &base,
            "#EXTM3U\n#EXT-X-MEDIA-SEQUENCE:42\n#EXTINF:5,\na.ts\n#EXTINF:5,\nb.ts",
        )
        .unwrap();
        assert_eq!(p.segments[1].0, 43);
        assert_eq!(p.segments[0].1.as_str(), "https://example.com/live/a.ts");
        assert!(playlist(&base, "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128\nx.ts").is_err());
        assert!(playlist(&base, "#EXTM3U\nfile:///etc/passwd").is_err());
    }
    #[test]
    #[ignore = "Uses the public directory and a broadcaster; decodes in memory, no playback or saved audio"]
    fn live_hls_decode() {
        use std::{
            process::{Command, Stdio},
            sync::{
                atomic::{AtomicBool, Ordering},
                mpsc, Arc,
            },
        };
        let extension = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        let stations = search(&extension, "北京").expect("directory");
        let station = stations
            .into_iter()
            .find(|s| s.hls && s.url.contains("rbc.cn"))
            .expect("public broadcaster HLS");
        let ffmpeg = std::env::var("RADIO_TEST_FFMPEG").unwrap_or_else(|_| "ffmpeg".into());
        let mut child = Command::new(ffmpeg)
            .args([
                "-hide_banner",
                "-loglevel",
                "error",
                "-nostdin",
                "-protocol_whitelist",
                "pipe",
                "-i",
                "pipe:0",
                "-vn",
                "-ac",
                "1",
                "-ar",
                "48000",
                "-f",
                "s16le",
                "pipe:1",
            ])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap();
        let input = child.stdin.take().unwrap();
        let mut output = child.stdout.take().unwrap();
        let alive = Arc::new(AtomicBool::new(true));
        let flag = alive.clone();
        let (errors, err) = mpsc::channel();
        let network = std::thread::spawn(move || {
            let r = feed(&station, input, || flag.load(Ordering::SeqCst));
            let _ = errors.send(r);
        });
        let (tx, rx) = mpsc::channel();
        std::thread::spawn(move || {
            let mut pcm = vec![0; 48000 * 2 * 4];
            let result = output.read_exact(&mut pcm);
            let peak = pcm
                .chunks_exact(2)
                .map(|s| i16::from_le_bytes([s[0], s[1]]).unsigned_abs())
                .max()
                .unwrap_or(0);
            let _ = tx.send((result.is_ok(), peak));
        });
        let decoded = rx.recv_timeout(Duration::from_secs(35));
        alive.store(false, Ordering::SeqCst);
        let _ = child.kill();
        let _ = child.wait();
        let _ = network.join();
        let detail = err.try_recv();
        assert!(
            decoded.is_ok_and(|(ok, peak)| ok && peak > 0),
            "No decoded audio: {detail:?}"
        );
    }
}
