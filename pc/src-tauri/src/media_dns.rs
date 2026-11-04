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
struct Job { host:String, reply:mpsc::SyncSender<Answers>, deadline:Instant }
struct SystemResolver { sender:mpsc::SyncSender<Job> }
static SYSTEM: OnceLock<Result<SystemResolver,String>> = OnceLock::new();
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
impl SystemResolver {
    fn new(resolve:impl Fn(&str)->Answers+Send+'static)->Result<Self,String>{
        let (sender,rx)=mpsc::sync_channel::<Job>(8);
        // Exactly one resolver worker, even if the OS resolver never returns.
        // Callers time out and release their own service permits independently.
        std::thread::Builder::new().name("media-system-dns".into()).spawn(move||{
            for job in rx {
                // Do not perform stale DNS work after its caller timed out.
                if Instant::now()>=job.deadline {continue;}
                let result=resolve(&job.host);
                if Instant::now()<job.deadline {let _=job.reply.try_send(result);}
            }
        }).map_err(|_|"系统域名解析工作线程不可用".to_string())?;
        Ok(Self{sender})
    }
    fn request(&self,host:&str,timeout:Duration)->Answers{
        let (reply,rx)=mpsc::sync_channel(1);
        self.sender.try_send(Job{host:host.into(),reply,deadline:Instant::now()+timeout})
            .map_err(|_|"系统域名解析繁忙".to_string())?;
        rx.recv_timeout(timeout).map_err(|_|"系统域名解析超时".to_string())?
    }
}
fn system(host: &str) -> Answers {
    let resolver=SYSTEM.get_or_init(||SystemResolver::new(|host|{
        let answers:Vec<_>=(host,443).to_socket_addrs().map_err(|_|"系统域名解析失败".to_string())?
            .take(33).map(|a|a.ip()).collect();
        if answers.len()>32{return Err("系统域名解析地址数量超限".into());}
        Ok(answers)
    })).as_ref().map_err(Clone::clone)?;
    resolver.request(host,Duration::from_secs(3))
}
/// Bounded system DNS only. This does not add DoH fallback, host permissions,
/// proxy use or implicit address acceptance to pc-fetch. Its caller must check
/// every answer against its existing public-IP policy before pinning it.
pub(crate) fn system_addresses(host:&str,port:u16)->Result<Vec<SocketAddr>,String>{
    system(host).map(|ips|ips.into_iter().map(|ip|SocketAddr::new(ip,port)).collect())
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
    fn timed_out_system_dns_keeps_one_worker_and_skips_expired_queued_requests(){
        use std::sync::{Arc,atomic::{AtomicUsize,Ordering}};
        let calls=Arc::new(AtomicUsize::new(0));let count=calls.clone();
        let (entered_tx,entered_rx)=mpsc::channel();let (release_tx,release_rx)=mpsc::channel();
        let resolver=SystemResolver::new(move|host|{
            count.fetch_add(1,Ordering::SeqCst);
            if host=="blocked.example"{entered_tx.send(()).unwrap();release_rx.recv().unwrap();}
            Ok(vec!["93.184.216.34".parse().unwrap()])
        }).unwrap();
        assert!(resolver.request("blocked.example",Duration::from_secs(1)).is_err());
        entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        // Timeouts do not spawn replacements. At most eight pending DNS jobs,
        // with further attempts failing closed instead of growing a queue.
        for _ in 0..12 {assert!(resolver.request("expired.example",Duration::from_millis(2)).is_err());}
        assert_eq!(calls.load(Ordering::SeqCst),1);
        release_tx.send(()).unwrap();
        let deadline=Instant::now()+Duration::from_secs(2);
        loop {
            if resolver.request("fresh.example",Duration::from_millis(50)).is_ok(){break;}
            assert!(Instant::now()<deadline,"bounded resolver did not recover");
            std::thread::yield_now();
        }
        assert_eq!(calls.load(Ordering::SeqCst),2); // stale queue never resolves
    }
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
