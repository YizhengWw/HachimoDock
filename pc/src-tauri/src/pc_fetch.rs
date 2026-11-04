//! Package-owned pc-fetch-v1 service. No device I/O and no implicit host grants.
use reqwest::{blocking::Client, redirect::Policy, Url};
use serde::Deserialize;
use serde_json::{json, Map, Value};
use std::{
    collections::{BTreeMap, HashSet},
    io::Read,
    net::{IpAddr, SocketAddr},
    time::Duration,
};
pub const ENGINE: &str = "pc-fetch-v1";
pub const CAPABILITY: &str = "widgetPcFetch";
#[derive(Clone, Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Source {
    pub id: String,
    pub url: String,
    #[serde(default = "get")]
    pub method: String,
    #[serde(default)]
    pub headers: BTreeMap<String, String>,
    pub interval_sec: u64,
    pub ttl_sec: Option<u64>,
    pub max_bytes: Option<usize>,
    pub map: Value,
}
fn get() -> String {
    "GET".into()
}
fn ident(s: &str, n: usize) -> bool {
    !s.is_empty()
        && s.len() <= n
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}
fn host_ok(s: &str) -> bool {
    s.len() <= 253
        && s.contains('.')
        && s.parse::<std::net::IpAddr>().is_err()
        && s.split('.').all(|p| {
            !p.is_empty()
                && p.len() <= 63
                && !p.starts_with('-')
                && !p.ends_with('-')
                && p.bytes()
                    .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
        })
}
fn public_ip(ip: IpAddr) -> bool {
    match ip {
        IpAddr::V4(v) => {
            let [a, b, c, _] = v.octets();
            !(v.is_private()
                || v.is_loopback()
                || v.is_link_local()
                || v.is_unspecified()
                || v.is_broadcast()
                || v.is_documentation()
                || a == 0
                || a >= 224
                || (a == 100 && (64..=127).contains(&b))
                || (a == 198 && (b == 18 || b == 19))
                || (a == 192 && b == 0 && c == 0))
        }
        IpAddr::V6(v) => {
            let s = v.segments();
            (s[0] & 0xe000) == 0x2000
                && !(s[0] == 0x2001 && (s[1] <= 0x01ff || s[1] == 0x0db8))
                && s[0] != 0x2002
                && !(s[0] == 0x3fff && s[1] <= 0x0fff)
        }
    }
}
pub fn allowed_url(raw: &str, hosts: &HashSet<String>) -> Result<Url, String> {
    let u = Url::parse(raw).map_err(|_| "invalid URL")?;
    let host = u.host_str().ok_or("missing host")?;
    if raw.len() > 2048
        || u.scheme() != "https"
        || !u.username().is_empty()
        || u.password().is_some()
        || u.fragment().is_some()
        || u.port_or_known_default() != Some(443)
        || !host_ok(host)
        || !hosts.contains(host)
    {
        return Err("host not allowed".into());
    }
    if u.query_pairs().any(|(k, _)| {
        [
            "key",
            "api_key",
            "apikey",
            "token",
            "access_token",
            "password",
            "secret",
            "authorization",
            "sig",
            "signature",
        ]
        .contains(&k.to_ascii_lowercase().as_str())
    }) {
        return Err("credential query forbidden".into());
    }
    Ok(u)
}
fn pointer_ok(s: &str) -> bool {
    if s.len() > 128 || !(s.is_empty() || s.starts_with('/')) {
        return false;
    }
    let mut chars = s.chars();
    while let Some(c) = chars.next() {
        if c == '~' && !matches!(chars.next(), Some('0' | '1')) {
            return false;
        }
    }
    true
}
impl Source {
    pub fn ttl_ms(&self) -> u64 {
        self.ttl_sec
            .unwrap_or(self.interval_sec.saturating_mul(3))
            .saturating_mul(1000)
    }
    fn limit(&self) -> usize {
        self.max_bytes.unwrap_or(65536)
    }
    pub fn validate(&self, runtime: &Value, hosts: &HashSet<String>) -> Result<(), String> {
        allowed_url(&self.url, hosts)?;
        if !ident(&self.id, 16)
            || self.method != "GET"
            || !(60..=86400).contains(&self.interval_sec)
            || !(self.interval_sec..=604800).contains(&(self.ttl_ms() / 1000))
            || !(1024..=65536).contains(&self.limit())
            || self.headers.len() > 4
        {
            return Err("invalid source bounds".into());
        }
        for (k, v) in &self.headers {
            if v.is_empty()
                || v.len() > 120
                || v.chars().any(char::is_control)
                || !match k.as_str() {
                    "Accept" => matches!(v.as_str(), "application/json" | "text/plain"),
                    "Accept-Language" | "User-Agent" => true,
                    "Cache-Control" => v == "no-cache",
                    _ => false,
                }
            {
                return Err("invalid header".into());
            }
        }
        let m = self
            .map
            .as_object()
            .filter(|m| m.len() == 1)
            .ok_or("invalid mapping")?;
        if let Some(vars) = m.get("vars") {
            let vars = vars
                .as_object()
                .filter(|v| (1..=8).contains(&v.len()))
                .ok_or("invalid vars")?;
            if runtime["host"] != true
                || runtime["surface"]["bindings"] != "p4-variable-bindings-v1"
                || runtime.get("persist").is_some()
                || runtime["tick"].as_array().is_some_and(|a| !a.is_empty())
            {
                return Err("invalid host runtime".into());
            }
            for (k, p) in vars {
                if !matches!(runtime["vars"][k]["type"].as_str(), Some("int" | "string"))
                    || !p.as_str().is_some_and(pointer_ok)
                {
                    return Err("invalid var binding".into());
                }
            }
        } else if let Some(rows) = m.get("rows") {
            let rows = rows.as_object().ok_or("invalid rows")?;
            if !["items", "id", "label"]
                .iter()
                .all(|k| rows.contains_key(*k))
                || rows.iter().any(|(k, p)| {
                    !["items", "id", "label", "value", "detail", "meta", "tone"]
                        .contains(&k.as_str())
                        || !p.as_str().is_some_and(pointer_ok)
                })
                || runtime["data"]["source"] != format!("fetch.{}", self.id)
            {
                return Err("invalid rows binding".into());
            }
        } else {
            return Err("unknown mapping".into());
        }
        Ok(())
    }
}
#[derive(Debug, Default)]
pub struct Cache {
    pub attempted_ms: Option<u64>,
    pub fetched_ms: Option<u64>,
    pub rows: Option<Value>,
    pub vars: Map<String, Value>,
    pub error: Option<String>,
}
fn text(v: &Value, max: usize) -> String {
    let s = match v {
        Value::String(s) => s.clone(),
        Value::Number(_) | Value::Bool(_) => v.to_string(),
        _ => String::new(),
    };
    let mut out = String::new();
    for c in s.chars().filter(|c| !c.is_control()) {
        if out.len() + c.len_utf8() > max {
            break;
        }
        out.push(c);
    }
    out
}
pub fn project(
    doc: &Value,
    source: &Source,
    runtime: &Value,
) -> Result<(Option<Value>, Map<String, Value>), String> {
    let mut vars = Map::new();
    if let Some(m) = source.map["vars"].as_object() {
        for (name, p) in m {
            let v = doc
                .pointer(p.as_str().ok_or("invalid pointer")?)
                .ok_or("missing binding")?;
            let mapped = if runtime["vars"][name]["type"] == "int" {
                let n = v
                    .as_i64()
                    .or_else(|| v.as_str().and_then(|s| s.trim().parse().ok()))
                    .or_else(|| {
                        v.as_f64()
                            .filter(|f| f.fract() == 0.0 && f.abs() <= 1e9)
                            .map(|f| f as i64)
                    })
                    .filter(|n| (-1_000_000_000..=1_000_000_000).contains(n))
                    .ok_or("invalid integer binding")?;
                json!(n)
            } else if v.is_string() || v.is_number() || v.is_boolean() {
                json!(text(v, 63))
            } else {
                return Err("invalid string binding".into());
            };
            vars.insert(name.clone(), mapped);
        }
        return Ok((None, vars));
    }
    let m = &source.map["rows"];
    let items = doc
        .pointer(m["items"].as_str().ok_or("invalid items pointer")?)
        .and_then(Value::as_array)
        .ok_or("items not array")?;
    let mut rows = Vec::new();
    let mut ids = HashSet::new();
    for item in items {
        let mut row = Map::new();
        for (field, limit) in [
            ("id", 23),
            ("label", 36),
            ("value", 24),
            ("detail", 24),
            ("meta", 63),
        ] {
            let v = m[field]
                .as_str()
                .and_then(|p| item.pointer(p))
                .unwrap_or(&Value::Null);
            row.insert(field.into(), json!(text(v, limit)));
        }
        let id = row["id"].as_str().unwrap();
        if id.is_empty() || !ids.insert(id.to_owned()) {
            continue;
        }
        let tone = m["tone"]
            .as_str()
            .and_then(|p| item.pointer(p))
            .and_then(|v| {
                v.as_f64()
                    .or_else(|| v.as_str().and_then(|s| s.trim().parse::<f64>().ok()))
            })
            .unwrap_or(0.0);
        row.insert(
            "tone".into(),
            json!(if tone > 0.0 {
                1
            } else if tone < 0.0 {
                -1
            } else {
                0
            }),
        );
        rows.push(Value::Object(row));
        if rows.len() == 20 {
            break;
        }
    }
    Ok((Some(json!(rows)), vars))
}
impl Cache {
    pub fn due(&self, source: &Source, now: u64) -> bool {
        self.attempted_ms
            .is_none_or(|t| now.saturating_sub(t) >= source.interval_sec.saturating_mul(1000))
    }
    /// Sync worker-thread entry; callers must re-read the PC-owned allowlist each call.
    /// Redirects and proxy inheritance are disabled. Successful mappings replace cache atomically.
    pub fn poll(
        &mut self,
        source: &Source,
        runtime: &Value,
        hosts: &HashSet<String>,
        now: u64,
    ) -> Result<bool, String> {
        self.poll_with_resolver(source,runtime,hosts,now,|host|crate::media_dns::system_addresses(host,443))
    }
    fn poll_with_resolver(&mut self,source:&Source,runtime:&Value,hosts:&HashSet<String>,now:u64,
        resolve:impl FnOnce(&str)->Result<Vec<SocketAddr>,String>)->Result<bool,String>{
        source.validate(runtime, hosts)?; // also enforced on cache hits / revoked grants
        if !self.due(source, now) {
            return Ok(false);
        }
        self.attempted_ms = Some(now);
        let result = (|| {
            let url = allowed_url(&source.url, hosts)?;
            let host = url.host_str().ok_or("missing host")?;
            let addresses=resolve(host)?;
            if addresses.is_empty() || addresses.iter().any(|a| !public_ip(a.ip())) {
                return Err("non-public DNS address".into());
            }
            let client = Client::builder()
                .redirect(Policy::none())
                .no_proxy()
                .resolve_to_addrs(host, &addresses)
                .timeout(Duration::from_secs(15))
                .build()
                .map_err(|_| "client error")?;
            let mut request = client.get(url);
            for (k, v) in &source.headers {
                request = request.header(k, v);
            }
            let response = request.send().map_err(|_| "network error")?;
            if !response.status().is_success() {
                return Err("upstream HTTP error".into());
            }
            let mut bytes = Vec::new();
            response
                .take(source.limit() as u64 + 1)
                .read_to_end(&mut bytes)
                .map_err(|_| "response read error")?;
            if bytes.len() > source.limit() {
                return Err("response too large".into());
            }
            let doc = serde_json::from_slice(&bytes).map_err(|_| "invalid JSON")?;
            project(&doc, source, runtime)
        })();
        match result {
            Ok((rows, vars)) => {
                self.rows = rows;
                self.vars = vars;
                self.fetched_ms = Some(now);
                self.error = None;
                Ok(true)
            }
            Err(e) => {
                self.error = Some(e.clone());
                Err(e)
            }
        }
    }
    pub fn snapshot(&self, source: &Source, now: u64) -> Value {
        let stale = self
            .fetched_ms
            .is_some_and(|t| now.saturating_sub(t) > source.ttl_ms());
        let (status, message) = if stale {
            ("error", "数据已过期")
        } else if self.error.is_some() && self.fetched_ms.is_none() {
            ("error", "上游错误")
        } else if self.fetched_ms.is_none() {
            ("empty", "等待数据")
        } else if self
            .rows
            .as_ref()
            .and_then(Value::as_array)
            .is_none_or(|r| r.is_empty())
        {
            ("empty", "暂无数据")
        } else {
            ("ok", "")
        };
        json!({"schema":1,"source":format!("fetch.{}",source.id),"ttlMs":source.ttl_ms().clamp(5000,300000),"status":status,"message":message,"rows":self.rows.clone().unwrap_or(json!([]))})
    }
}
/// Validate the entire declaration before allocating per-source state.
pub fn parse(
    bytes: &[u8],
    runtime: &Value,
    hosts: &HashSet<String>,
) -> Result<Vec<Source>, String> {
    if bytes.len() > 16384 {
        return Err("declaration too large".into());
    }
    let d: Value = serde_json::from_slice(bytes).map_err(|_| "invalid declaration")?;
    let o = d.as_object().ok_or("invalid declaration")?;
    if o.keys().any(|k| {
        !["schemaVersion", "engine", "service", "actions", "content"].contains(&k.as_str())
    }) || d["schemaVersion"] != 1
        || d["engine"] != ENGINE
        || d["service"] != "fetch"
    {
        return Err("invalid declaration".into());
    }
    let sources: Vec<Source> =
        serde_json::from_value(d["content"].clone()).map_err(|_| "invalid sources")?;
    if !(1..=4).contains(&sources.len()) {
        return Err("invalid source count".into());
    }
    let mut ids = HashSet::new();
    let mut bound = HashSet::new();
    let mut rows = 0;
    for source in &sources {
        source.validate(runtime, hosts)?;
        if !ids.insert(source.id.as_str()) {
            return Err("duplicate source".into());
        }
        if let Some(vars) = source.map["vars"].as_object() {
            for k in vars.keys() {
                if !bound.insert(k.as_str()) {
                    return Err("duplicate variable writer".into());
                }
            }
        }
        if source.map.get("rows").is_some() {
            rows += 1;
        }
    }
    if rows > 1
        || (!bound.is_empty()
            && runtime["vars"].as_object().is_none_or(|v| {
                v.len() != bound.len() || v.keys().any(|k| !bound.contains(k.as_str()))
            }))
    {
        return Err("conflicting bindings".into());
    }
    if let Some(host) = runtime.get("host") {
        if host != true {
            return Err("invalid host mode".into());
        }
    }
    if let Some(actions) = d.get("actions") {
        let actions = actions
            .as_object()
            .filter(|a| a.len() <= 4)
            .ok_or("invalid actions")?;
        for (a, op) in actions {
            let name = a.strip_prefix("host.").ok_or("invalid host action")?;
            if name.is_empty()
                || name.len() > 32
                || !name.bytes().all(|c| c.is_ascii_lowercase() || c == b'_')
                || !op
                    .as_str()
                    .is_some_and(|op| sources.iter().any(|s| op == format!("{}:refresh", s.id)))
                || !runtime["transitions"].as_array().is_some_and(|ts| {
                    ts.iter().any(|t| {
                        t.as_object().is_some_and(|o| o.len() == 2)
                            && t["from"] == "*"
                            && t["on"] == a.as_str()
                    })
                })
            {
                return Err("invalid refresh action".into());
            }
        }
    }
    Ok(sources)
}
#[cfg(test)]
mod tests {
    use super::*;
    fn fixture() -> (Source, Value, HashSet<String>) {
        (serde_json::from_value(json!({"id":"feed","url":"https://example.org/feed","interval_sec":60,"map":{"rows":{"items":"/items","id":"/id","label":"/label"}}})).unwrap(),json!({"host":true,"data":{"source":"fetch.feed"}}),HashSet::from(["example.org".into()]))
    }
    #[test]
    fn whitelist_and_network_boundaries() {
        let (_, _, hosts) = fixture();
        for url in [
            "http://example.org/",
            "https://evil.example.org/",
            "https://example.org:444/",
            "https://u:p@example.org/",
            "https://example.org/#x",
            "https://127.0.0.1/",
            "https://example.org/?%74oken=secret",
        ] {
            assert!(allowed_url(url, &hosts).is_err(), "{url}");
        }
        for ip in [
            "127.0.0.1",
            "10.0.0.1",
            "100.64.0.1",
            "169.254.169.254",
            "198.18.0.1",
            "::1",
            "::ffff:127.0.0.1",
            "2001:db8::1",
        ] {
            assert!(!public_ip(ip.parse().unwrap()), "{ip}");
        }
        assert!(public_ip("8.8.8.8".parse().unwrap()));
        assert!(allowed_url("https://example.org/feed", &hosts).is_ok());
    }
    #[test]
    fn rows_utf8_dedup_limit_and_pointer() {
        let (mut s, r, h) = fixture();
        s.validate(&r, &h).unwrap();
        s.map["rows"]["label"] = json!("/a~1b");
        let items: Vec<_> = (0..25)
            .map(|i| json!({"id":i,"a/b":"中".repeat(30)}))
            .collect();
        let (rows, _) = project(&json!({"items":items}), &s, &r).unwrap();
        let rows = rows.unwrap();
        assert_eq!(rows.as_array().unwrap().len(), 20);
        assert_eq!(rows[0]["label"].as_str().unwrap().len(), 36);
        let (rows, _) =
            project(&json!({"items":[{"id":"x"},{"id":"x"},{"id":""}]}), &s, &r).unwrap();
        assert_eq!(rows.unwrap().as_array().unwrap().len(), 1);
        assert!(project(&json!({}), &s, &r).is_err());
        s.map["rows"]["id"] = json!("/~2");
        assert!(s.validate(&r, &h).is_err());
    }
    #[test]
    fn bounded_dns_failure_preserves_cache_and_all_answers_still_require_public_addresses(){
        let (source,runtime,hosts)=fixture();let mut cache=Cache::default();
        cache.vars.insert("old".into(),json!(7));cache.fetched_ms=Some(1);
        let failed=cache.poll_with_resolver(&source,&runtime,&hosts,100,|_|Err("DNS deadline".into()));
        assert_eq!(failed.unwrap_err(),"DNS deadline");assert_eq!(cache.fetched_ms,Some(1));assert_eq!(cache.vars["old"],7);
        cache.attempted_ms=None;
        let rejected=cache.poll_with_resolver(&source,&runtime,&hosts,200,|_|Ok(vec![
            "93.184.216.34:443".parse().unwrap(),"127.0.0.1:443".parse().unwrap()]));
        assert_eq!(rejected.unwrap_err(),"non-public DNS address"); // never constructs/sends HTTP
        let mut called=false;
        assert!(cache.poll_with_resolver(&source,&runtime,&HashSet::new(),300,|_|{called=true;Ok(vec![])}).is_err());
        assert!(!called); // declaration is not a runtime host grant
    }
    #[test]
    fn variable_atomic_projection_and_cache_states() {
        let (mut s, _, h) = fixture();
        let r = json!({"host":true,"surface":{"bindings":"p4-variable-bindings-v1"},"vars":{"n":{"type":"int"},"s":{"type":"string"}}});
        s.map = json!({"vars":{"n":"/n","s":"/s"}});
        s.validate(&r, &h).unwrap();
        assert_eq!(
            project(&json!({"n":"42","s":"a\nb"}), &s, &r).unwrap().1["s"],
            "ab"
        );
        assert!(project(&json!({"n":true,"s":"x"}), &s, &r).is_err());
        let mut c = Cache::default();
        assert_eq!(c.snapshot(&s, 0)["message"], "等待数据");
        c.error = Some("network error".into());
        assert_eq!(c.snapshot(&s, 0)["status"], "error");
        c.rows = Some(json!([{"id":"x"}]));
        c.fetched_ms = Some(100);
        c.attempted_ms = Some(100);
        assert!(!c.due(&s, 60099));
        assert!(c.due(&s, 60100));
        assert_eq!(c.snapshot(&s, 180101)["message"], "数据已过期");
        assert!(c.poll(&s, &r, &HashSet::new(), 101).is_err());
        assert_eq!(c.attempted_ms, Some(100));
    }
    #[test]
    fn declaration_rejects_conflicting_writers_and_unknown_fields() {
        let (s, r, h) = fixture();
        let mut d = json!({"schemaVersion":1,"engine":ENGINE,"service":"fetch","content":[{"id":s.id,"url":s.url,"interval_sec":60,"map":s.map}]});
        assert!(parse(&serde_json::to_vec(&d).unwrap(), &r, &h).is_ok());
        let duplicate = d["content"][0].clone();
        d["content"].as_array_mut().unwrap().push(duplicate);
        assert!(parse(&serde_json::to_vec(&d).unwrap(), &r, &h).is_err());
        d["content"].as_array_mut().unwrap().pop();
        d["content"][0]["headers"] = json!({"Authorization":"Bearer secret"});
        assert!(parse(&serde_json::to_vec(&d).unwrap(), &r, &h).is_err());
    }
}
/// Import-time validation only. URL hosts here describe requested permissions;
/// this set MUST NOT be used as the user's runtime allowlist.
pub fn parse_declaration(bytes: &[u8], runtime: &Value) -> Result<Vec<Source>, String> {
    if bytes.len() > 16384 {
        return Err("declaration too large".into());
    }
    let d: Value = serde_json::from_slice(bytes).map_err(|_| "invalid declaration")?;
    let hosts: HashSet<String> = d["content"]
        .as_array()
        .into_iter()
        .flatten()
        .filter_map(|s| s["url"].as_str().and_then(|s| Url::parse(s).ok()))
        .filter_map(|u| u.host_str().map(str::to_owned))
        .collect();
    parse(bytes, runtime, &hosts)
}
#[path = "pc_fetch_host.rs"]
pub mod host;
