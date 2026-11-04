//! Explicitly enabled PC service owns polling/cache; device epochs gate only publication.
use super::*;
use std::{
    collections::HashMap,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc, Mutex, OnceLock,
    },
};
#[path = "pc_fetch_lifecycle.rs"]
mod lifecycle;
use lifecycle::{Budget, Session};
static WORKERS: Budget = Budget::new();
static SHUTDOWN: AtomicBool = AtomicBool::new(false);
struct Job {
    session: Session<Value>,
    signature: String,
}
fn jobs() -> &'static Mutex<HashMap<String, Arc<Job>>> {
    static J: OnceLock<Mutex<HashMap<String, Arc<Job>>>> = OnceLock::new();
    J.get_or_init(Default::default)
}
fn now() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}
pub fn fingerprint(d: &crate::app_services::Declaration, runtime: &Value) -> String {
    json!([d, runtime]).to_string()
}
pub fn stop(id: &str) {
    if let Ok(mut j) = jobs().lock() {
        if let Some(job) = j.remove(id) {
            job.session.cancel();
        }
    }
}
pub fn detach(id: &str, epoch: u64) {
    if let Ok(j) = jobs().try_lock() {
        if let Some(job) = j.get(id) {
            job.session.detach(epoch);
        }
    }
}
pub fn shutdown() {
    SHUTDOWN.store(true, Ordering::SeqCst);
    if let Ok(mut j) = jobs().lock() {
        for (_, job) in j.drain() {
            job.session.cancel();
        }
    }
}
// The generic service API treats an absent setting as enabled for legacy apps.
// Background fetch specifically requires a real user-written enable preference.
fn explicitly_enabled(dir: &Path) -> bool {
    dir.is_dir()
        && dir.join("service-settings.json").is_file()
        && crate::component_service_settings::enabled(dir).unwrap_or(false)
}
fn grants(dir: &Path) -> Result<HashSet<String>, String> {
    let file = match std::fs::File::open(dir.join("fetch-hosts.json")) {
        Ok(f) => f,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(HashSet::new()),
        Err(_) => return Err("无法读取网络白名单".into()),
    };
    let mut raw = Vec::new();
    file.take(4097)
        .read_to_end(&mut raw)
        .map_err(|_| "无法读取网络白名单")?;
    if raw.len() > 4096 {
        return Err("网络白名单过大".into());
    }
    let v: Vec<String> = serde_json::from_slice(&raw).map_err(|_| "网络白名单损坏")?;
    if v.len() > 16 || v.iter().any(|h| !host_ok(h)) {
        return Err("网络白名单无效".into());
    }
    Ok(v.into_iter().collect())
}
fn eligible(id: &str, dir: &Path, signature: &str, sources: &[Source]) -> bool {
    if SHUTDOWN.load(Ordering::SeqCst) || !explicitly_enabled(dir) {
        return false;
    }
    let Ok(hosts) = grants(dir) else {
        return false;
    };
    sources.iter().any(|s| allowed_url(&s.url, &hosts).is_ok())
        && crate::component_app::fetch_declaration_current(id, signature)
}
pub fn request(
    id: &str,
    dir: &Path,
    d: &crate::app_services::Declaration,
    runtime: &Value,
    input: &Value,
) -> Result<Value, String> {
    if SHUTDOWN.load(Ordering::SeqCst) {
        return Err("PC 服务正在退出".into());
    }
    let op = input["operation"].as_str().unwrap_or("status");
    if op == "stop" {
        crate::component_service_settings::set_enabled(dir, false)?;
        stop(id);
        return Ok(json!({"enabled":false,"service":"fetch"}));
    }
    if op == "configure" {
        if input.get("action").is_some() || input.get("_deviceEpoch").is_some() {
            return Err("设备动作不可修改网络白名单".into());
        }
        let hosts: Vec<String> =
            serde_json::from_value(input["hosts"].clone()).map_err(|_| "需要精确主机列表")?;
        if hosts.len() > 16 || hosts.iter().any(|h| !host_ok(h)) {
            return Err("网络白名单无效".into());
        }
        crate::component_app::atomic(&dir.join("fetch-hosts.json"), &hosts)?;
        stop(id);
    }
    if !matches!(op, "status" | "enable" | "configure" | "action") {
        return Err("不支持的 fetch 操作".into());
    }
    if let Some(action) = input["action"].as_str() {
        if !d.actions.contains_key(action) {
            return Err("未声明动作".into());
        }
    }
    let wire = json!({"schemaVersion":1,"engine":ENGINE,"service":"fetch","actions":d.actions,"content":d.content});
    let sources = parse_declaration(&serde_json::to_vec(&wire).map_err(|_| "声明无效")?, runtime)?;
    let signature = fingerprint(d, runtime);
    let generation = input["_deviceEpoch"].as_u64();
    let hosts = grants(dir).unwrap_or_default();
    let enabled = explicitly_enabled(dir);
    let granted = sources.iter().any(|s| allowed_url(&s.url, &hosts).is_ok());
    let actions: Vec<_> = d.actions.keys().cloned().collect();
    let initial = json!({"allowedHosts":hosts,"enabled":enabled,"service":"fetch","variables":{},"data":null,"permissions":crate::app_services::permissions(d),"ui":{"mode":0,"context":"fetch","actions":actions},"values":{"headline":if !enabled{"服务未启用"}else if !granted{"等待网络授权"}else{"等待数据"},"note":"仅显式启用并允许主机后后台周期拉取；隐藏不停止缓存"}});
    if !enabled || !granted {
        stop(id);
        return Ok(initial);
    }
    let mut registry = jobs().lock().map_err(|_| "fetch 生命周期不可用")?;
    if registry
        .get(id)
        .is_some_and(|j| j.signature != signature || !j.session.active())
    {
        if let Some(j) = registry.remove(id) {
            j.session.cancel();
        }
    }
    if let Some(job) = registry.get(id) {
        if let Some(g) = generation {
            job.session.attach(g);
        }
        return Ok(job.session.cached());
    }
    registry.retain(|_, j| j.session.active());
    let permit = WORKERS.acquire().ok_or("fetch 工作线程已满")?;
    let job = Arc::new(Job {
        session: Session::new(initial.clone(), generation),
        signature: signature.clone(),
    });
    registry.insert(id.into(), job.clone());
    drop(registry);
    let id = id.to_owned();
    let failed_id = id.clone();
    let dir: PathBuf = dir.into();
    let runtime = runtime.clone();
    let template = initial.clone();
    let spawned=std::thread::Builder::new().name("pc-fetch".into()).spawn(move||{
        let _permit=permit;let mut caches:Vec<Cache>=sources.iter().map(|_|Cache::default()).collect();
        loop {
        let hosts=grants(&dir).unwrap_or_default();
        let committed=job.session.cycle(||eligible(&id,&dir,&signature,&sources)&&grants(&dir).is_ok_and(|current|current==hosts),||{
            let at=now();let mut variables=Map::new();let mut data=Value::Null;let mut error=false;
            for(source,cache)in sources.iter().zip(&mut caches){
                if !job.session.active()||!eligible(&id,&dir,&signature,&sources){break;}
                // Cache::poll rechecks per-source grants and the minimum interval.
                if cache.poll(source,&runtime,&hosts,at).is_err(){cache.error=Some("上游或白名单错误".into());}error|=cache.error.is_some();
                if source.map.get("rows").is_some(){data=cache.snapshot(source,at);if allowed_url(&source.url,&hosts).is_err(){data["status"]="error".into();data["message"]="网络白名单未允许此来源".into();data["rows"]=json!([]);}}
                if cache.fetched_ms.is_some_and(|t|at.saturating_sub(t)<=source.ttl_ms())&&allowed_url(&source.url,&hosts).is_ok(){variables.extend(cache.vars.clone());}
            }
            let mut out=template.clone();out["allowedHosts"]=json!(hosts);out["variables"]=json!(variables);out["data"]=data;out["values"]=json!({"headline":if error{"上游错误"}else{"数据已更新"},"note":if error{"已有缓存保留，过期后停止提供变量"}else{"PC 后台周期更新"}});out
        },|epoch,out|crate::component_app::publish_fetch(&id,epoch,out));
            if !committed{break;}
            std::thread::sleep(Duration::from_secs(1));
        }
        job.session.cancel();
    });
    if spawned.is_err() {
        stop(&failed_id);
        return Err("无法启动 fetch 工作线程".into());
    }
    Ok(initial)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn no_implicit_background_enable_or_implicit_host_grant() {
        let dir = tempfile::tempdir().unwrap();
        assert!(!explicitly_enabled(dir.path()));
        assert!(grants(dir.path()).unwrap().is_empty());
        crate::component_service_settings::set_enabled(dir.path(), true).unwrap();
        assert!(explicitly_enabled(dir.path()));
        assert!(grants(dir.path()).unwrap().is_empty());
        crate::component_service_settings::set_enabled(dir.path(), false).unwrap();
        assert!(!explicitly_enabled(dir.path()));
    }
}
