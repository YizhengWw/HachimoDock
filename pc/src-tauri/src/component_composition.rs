//! Bounded service composition. No component IDs, scripts, arbitrary IPC or credentials.
use crate::app_services::Declaration;
use crate::component_runtime_lifecycle::{enter_owner, Resource, ResourceOwner};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::BTreeMap,
    path::{Path, PathBuf},
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc, Arc, Mutex, OnceLock,
    },
};

pub const ENGINE: &str = "pc-app-composition-v1";
pub const ENGINE_V2: &str = "pc-app-composition-v2";
pub const ENGINE_V3: &str = "pc-app-composition-v3";
pub fn is_engine(engine: &str) -> bool {
    matches!(engine, ENGINE | ENGINE_V2 | ENGINE_V3)
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Plan {
    sources: BTreeMap<String, Source>,
    bindings: BTreeMap<String, Binding>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    workflows: BTreeMap<String, Vec<Step>>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    async_workflows: BTreeMap<String, Vec<AsyncStep>>,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(untagged)]
enum AsyncStep {
    Get(GetStep),
    Local(Step),
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct GetStep {
    get: PublicGet,
    result: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct PublicGet {
    endpoint: String,
    hosts: Vec<String>,
    pointer: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Step {
    target: String,
    #[serde(
        default,
        deserialize_with = "present_string",
        skip_serializing_if = "Option::is_none"
    )]
    pointer: Option<String>,
    #[serde(
        default,
        deserialize_with = "present_value",
        skip_serializing_if = "Option::is_none"
    )]
    value: Option<Value>,
    #[serde(
        default,
        deserialize_with = "present_value",
        skip_serializing_if = "Option::is_none"
    )]
    when: Option<Value>,
}
fn present_value<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<Value>, D::Error> {
    Value::deserialize(d).map(Some)
}
fn present_string<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    String::deserialize(d).map(Some)
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
enum Source {
    Constant {
        value: Value,
    },
    Clock {},
    Counter {
        initial: i32,
        min: i32,
        max: i32,
        step: i32,
    },
    Timer {
        seconds: u32,
    },
    Ledger {
        unit: String,
        target: u16,
        capacity: u16,
        label: String,
    },
    Computed {
        expression: Value,
    },
    Collection {
        expression: Value,
        view: crate::component_collection::Config,
    },
    Document {
        initial: Value,
    },
    Workflow {
        workflow: String,
    },
    JsonGet {
        endpoint: String,
        hosts: Vec<String>,
        refresh_sec: u32,
    },
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Binding {
    source: String,
    pointer: String,
    fallback: Value,
}
#[derive(Default, Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Saved {
    version: u8,
    models: BTreeMap<String, Model>,
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    receipts: Vec<Receipt>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    runs: BTreeMap<String, Run>,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Run {
    id: String,
    specification: String,
    attempt: u32,
    phase: String,
    event: Value,
    results: BTreeMap<String, Value>,
    steps: Vec<Checkpoint>,
    error: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Checkpoint {
    phase: String,
    attempts: u32,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Receipt {
    id: String,
    fingerprint: String,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
enum Model {
    Counter {
        value: i32,
    },
    Timer {
        remaining_ms: i64,
        started_ms: Option<i64>,
    },
    Ledger {
        data: crate::component_ledger::Ledger,
    },
    Document {
        value: Value,
    },
    Collection {
        view: crate::component_collection::State,
    },
}
fn name(s: &str, n: usize) -> bool {
    !s.is_empty()
        && s.len() <= n
        && s.bytes()
            .all(|c| c.is_ascii_alphanumeric() || b"._-".contains(&c))
}
fn scalar(v: &Value, kind: &str) -> bool {
    match kind {
        "int" => v
            .as_i64()
            .is_some_and(|n| (-1_000_000_000..=1_000_000_000).contains(&n)),
        "string" => v
            .as_str()
            .is_some_and(|s| s.len() <= 63 && !s.chars().any(char::is_control)),
        _ => false,
    }
}
fn plan(d: &Declaration) -> Result<Plan, String> {
    if d.content.len() != 1 {
        return Err("组合声明需要一个有界计划".into());
    }
    serde_json::from_value(d.content[0].clone())
        .map_err(|_| "组合声明包含未知字段或无效类型".into())
}
fn operations(source: &Source) -> &'static [&'static str] {
    match source {
        Source::Counter { .. } => &["increment", "decrement", "reset"],
        Source::Timer { .. } => &["start", "pause", "reset"],
        Source::Ledger { .. } => &["append", "remove_latest", "previous", "next"],
        Source::JsonGet { .. } => &["refresh"],
        Source::Document { .. } => &["set", "append", "remove"],
        Source::Collection { .. } => crate::component_collection::OPERATIONS,
        Source::Workflow { .. } => &["start", "resume", "cancel"],
        _ => &[],
    }
}
fn route<'a>(p: &'a Plan, route: &'a str) -> Option<(&'a str, &'a str)> {
    let (alias, op) = route.split_once(':')?;
    if alias == "flow" && p.workflows.contains_key(op) {
        return Some((alias, op));
    }
    operations(p.sources.get(alias)?)
        .contains(&op)
        .then_some((alias, op))
}
pub fn validate(d: &Declaration, r: &Value) -> Result<(), String> {
    let bad = || "组合应用需要有界数据源、精确变量绑定和已声明动作".to_string();
    let writers=crate::app_variable_owners::parse(r)?;
    if !is_engine(&d.engine)
        || d.schema_version != 1
        || d.service != "composition"
        || d.auto_read
        || d.location.is_some()
        || r["host"] != true
        || r["surface"]["bindings"] != "p4-variable-bindings-v1"
        || !writers.mixed&&(r.get("persist").is_some()||!r["tick"].as_array().is_some_and(Vec::is_empty))
        || r["dashboard"]
            .as_object()
            .is_some_and(|o| !writers.mixed&&o.values().any(|v| !v.is_string()))
    {
        return Err(bad());
    }
    let p = plan(d)?;
    let v2 = d.engine != ENGINE;
    let v3 = d.engine == ENGINE_V3;
    if writers.mixed&&!v2{return Err("宿主与设备字段混用需要组合 v2 的精确动作上下文".into());}
    if v2 && (r["host_context"] != "p4-host-context-v1" || r["input"]["engine"] != "p4-input-v1") {
        return Err("组合 v2 需要带版本上下文的宿主动作与统一输入".into());
    }
    if !v2
        && (d.content[0].get("workflows").is_some()
            || p.sources
                .values()
                .any(|s| matches!(s, Source::Computed { .. } | Source::Document { .. } | Source::Collection { .. })))
    {
        return Err("规则、文档与流程需要显式使用组合 v2".into());
    }
    if !v3
        && (d.content[0].get("asyncWorkflows").is_some()
            || p.sources
                .values()
                .any(|s| matches!(s, Source::Workflow { .. })))
    {
        return Err("持久异步流程需要显式使用组合 v3".into());
    }
    let vars = r["vars"]
        .as_object()
        .filter(|v| !v.is_empty() && v.len() <= 8)
        .ok_or_else(bad)?;
    if p.sources.is_empty()
        || p.sources.len() > if v2 { 8 } else { 4 }
        || p.bindings.keys().cloned().collect::<std::collections::BTreeSet<_>>() != writers.host
        || d.actions.len()
            > if r["input"]["engine"] == "p4-input-v1" {
                16
            } else {
                8
            }
    {
        return Err(bad());
    }
    for (alias, source) in &p.sources {
        if !name(alias, 16)
            || !v2
                && !p.bindings.values().any(|b| b.source == *alias)
                && !d
                    .actions
                    .values()
                    .any(|a| a.split_once(':').is_some_and(|(source, _)| source == alias))
        {
            return Err(bad());
        }
        match source {
            Source::Constant { value } => {
                if serde_json::to_vec(value).map_err(|_| bad())?.len() > 4096 {
                    return Err(bad());
                }
            }
            Source::Counter {
                initial,
                min,
                max,
                step,
            } => {
                if *min < -1_000_000_000
                    || *max > 1_000_000_000
                    || min > max
                    || initial < min
                    || initial > max
                    || *step < 1
                    || *step > 1_000_000_000
                {
                    return Err(bad());
                }
            }
            Source::Timer { seconds } => {
                if !(1..=86400).contains(seconds) {
                    return Err(bad());
                }
            }
            Source::JsonGet {
                endpoint,
                hosts,
                refresh_sec,
            } => {
                let u = crate::media_network::valid_url(endpoint)?;
                if u.scheme() != "https"
                    || u.port_or_known_default() != Some(443)
                    || !u.username().is_empty()
                    || u.password().is_some()
                    || u.fragment().is_some()
                    || hosts.is_empty()
                    || hosts.len() > 4
                    || !hosts.iter().all(|h| {
                        h.len() <= 253
                            && h.contains('.')
                            && h.parse::<std::net::IpAddr>().is_err()
                            && h.split('.').all(|s| {
                                !s.is_empty()
                                    && s.len() <= 63
                                    && !s.starts_with('-')
                                    && !s.ends_with('-')
                                    && s.bytes().all(|c| {
                                        c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-'
                                    })
                            })
                    })
                    || !u
                        .host_str()
                        .is_some_and(|h| hosts.iter().any(|allowed| allowed == h))
                    || !(30..=86400).contains(refresh_sec)
                    || u.query_pairs().any(|(k, _)| {
                        [
                            "key",
                            "api_key",
                            "apikey",
                            "token",
                            "access_token",
                            "password",
                            "secret",
                            "authorization",
                        ]
                        .contains(&k.to_ascii_lowercase().as_str())
                    })
                {
                    return Err(bad());
                }
            }
            Source::Clock {} => {}
            Source::Workflow { workflow } => {
                if !p.async_workflows.contains_key(workflow) {
                    return Err("流程状态源引用未声明异步流程".into());
                }
            }
            Source::Computed { expression } => {
                crate::component_expr::validate(expression, &p.sources.keys().cloned().collect())?;
            }
            Source::Collection { expression, view } => {
                crate::component_collection::validate(view)?;
                validate_collection_dependencies(&p,expression,0)?;
            }
            Source::Document { initial } => {
                if !crate::component_expr::bounded(initial) {
                    return Err(bad());
                }
            }
            Source::Ledger {
                unit,
                target,
                capacity,
                label,
            } => {
                if !crate::component_ledger::valid(&crate::component_ledger::Config {
                    unit,
                    target: *target,
                    capacity: *capacity,
                    label,
                }) {
                    return Err(bad());
                }
                for (action, target) in &d.actions {
                    if target == &format!("{alias}:remove_latest")
                        && !r["input"]["confirm"][action].is_string()
                    {
                        return Err("移除最新记录必须有明确确认".into());
                    }
                }
            }
        }
    }
    validate_workflows(&p, d, r)?;
    validate_async_workflows(&p, d)?;
    source_order(&p)?;
    if let Some(collection)=r["surface"].get("collection_view"){
        let config=crate::app_host_collection::Config::parse(collection)?;
        config.validate_surface(&r["surface"],Some(r))?;
        if d.engine==ENGINE || !matches!(p.sources.get(&config.source),Some(Source::Collection{view,..}) if view.page_size==config.visible)
            || config.actions.iter().enumerate().any(|(slot,action)|d.actions.get(action)!=Some(&format!("{}:row_{slot}",config.source))) {
            return Err(bad());
        }
    }
    for (key, b) in &p.bindings {
        if !name(key, 31)
            || !p.sources.contains_key(&b.source)
            || !pointer(&b.pointer)
            || !scalar(
                &b.fallback,
                vars.get(key).and_then(|v| v["type"].as_str()).unwrap_or(""),
            )
            || !within_limits(r, key, &b.fallback)
        {
            return Err(bad());
        }
    }
    let all_rules = r["transitions"].as_array().ok_or_else(bad)?;
    let rules:Vec<_>=all_rules.iter().filter(|v|!writers.mixed||v["on"].as_str().is_some_and(|s|s.starts_with("host."))).collect();
    let unique: std::collections::BTreeSet<_> =
        rules.iter().filter_map(|v| v["on"].as_str()).collect();
    if rules.len() != d.actions.len()
        || unique.len() != d.actions.len()
        || d.actions.iter().any(|(a, target)| {
            !a.strip_prefix("host.").is_some_and(|s| {
                !s.is_empty()
                    && s.len() <= 32
                    && s.bytes().all(|c| c.is_ascii_lowercase() || c == b'_')
            }) || route(&p, target).is_none()
                || target
                    .split(':')
                    .next()
                    .is_some_and(|a| matches!(p.sources.get(a), Some(Source::Document { .. })))
        })
        || rules.iter().any(|v| {
            v.as_object().is_none_or(|v| v.len() != 2)
                || v["from"] != "*"
                || !v["on"].as_str().is_some_and(|a| d.actions.contains_key(a))
        })
    {
        return Err(bad());
    }
    Ok(())
}
fn validate_collection_dependencies(p:&Plan,expression:&Value,depth:usize)->Result<(),String>{
    if depth>8{return Err("集合依赖超出预算".into());}
    fn transient(v:&Value)->bool{
        match v{
            Value::Object(o)=>o.get("ref").and_then(Value::as_str).is_some_and(|s|s=="/event"||s.starts_with("/event/"))||o.values().any(transient),
            Value::Array(a)=>a.iter().any(transient),_=>false,
        }
    }
    if transient(expression){return Err("集合条目不能依赖瞬时动作数据".into());}
    let known=p.sources.keys().cloned().collect();
    for dependency in crate::component_expr::validate(expression,&known)?{
        match &p.sources[&dependency]{
            Source::Document{..}|Source::Constant{..}=>{},
            Source::Computed{expression}=>validate_collection_dependencies(p,expression,depth+1)?,
            _=>return Err("集合视图仅接受文档、常量及其确定性计算，不绑定时钟或在途网络结果".into()),
        }
    }
    Ok(())
}
fn source_order(p: &Plan) -> Result<Vec<String>, String> {
    let known = p.sources.keys().cloned().collect();
    let mut waiting = BTreeMap::new();
    for (alias, source) in &p.sources {
        let deps = if let Source::Computed { expression } | Source::Collection { expression,.. } = source {
            crate::component_expr::validate(expression, &known)?
        } else {
            Default::default()
        };
        waiting.insert(alias.clone(), deps);
    }
    let mut order = Vec::new();
    while !waiting.is_empty() {
        let next = waiting
            .iter()
            .find(|(_, deps)| deps.iter().all(|d| order.contains(d)))
            .map(|(name, _)| name.clone())
            .ok_or("计算数据源存在循环依赖")?;
        waiting.remove(&next);
        order.push(next);
    }
    Ok(order)
}
fn validate_workflows(p: &Plan, d: &Declaration, r: &Value) -> Result<(), String> {
    let known = p.sources.keys().cloned().collect();
    if d.engine != ENGINE && p.sources.contains_key("flow") || p.workflows.len() > 8 {
        return Err("流程名称或数量无效".into());
    }
    for (name, steps) in &p.workflows {
        if !self::name(name, 16)
            || steps.is_empty()
            || steps.len() > 8
            || !d.actions.values().any(|a| a == &format!("flow:{name}"))
        {
            return Err("流程需要已声明动作与 1–8 个本地步骤".into());
        }
        let mut destructive = false;
        for step in steps {
            let (alias, op) = route(p, &step.target)
                .filter(|(alias, _)| *alias != "flow")
                .ok_or("流程目标无效")?;
            let source = &p.sources[alias];
            if matches!(source, Source::JsonGet { .. } | Source::Workflow { .. } | Source::Collection { .. }) {
                return Err("本地事务流程不允许网络副作用，尚未提交请求".into());
            }
            if let Source::Document { initial } = source {
                let ptr = step
                    .pointer
                    .as_deref()
                    .filter(|p| pointer(p))
                    .ok_or("文档步骤需要静态 JSON Pointer")?;
                if (op == "remove") != step.value.is_none() || op == "remove" && ptr.is_empty() {
                    return Err("文档步骤值或路径无效".into());
                }
                destructive |= op == "remove"
                    || op == "set"
                        && (ptr.is_empty()
                            || initial
                                .pointer(ptr)
                                .is_some_and(|v| v.is_array() || v.is_object()));
            } else if step.pointer.is_some() || step.value.is_some() {
                return Err("此服务不接受文档路径或值".into());
            }
            destructive |= op == "remove_latest" || op == "reset";
            for expression in step.value.iter().chain(step.when.iter()) {
                crate::component_expr::validate(expression, &known)?;
            }
        }
        if destructive
            && d.actions.iter().any(|(a, t)| {
                t == &format!("flow:{name}")
                    && !r["input"]["confirm"][a]
                        .as_str()
                        .is_some_and(|s| !s.trim().is_empty())
            })
        {
            return Err("移除、整体覆盖和重置流程需要明确确认".into());
        }
    }
    Ok(())
}
fn document_step(value: &mut Value, step: &Step, computed: Option<Value>) -> Result<(), String> {
    let ptr = step.pointer.as_deref().ok_or("缺少文档路径")?;
    let op = step.target.split_once(':').ok_or("缺少操作")?.1;
    if op == "set" || op == "append" {
        let target = value
            .pointer_mut(ptr)
            .ok_or("文档路径不存在，未修改原数据")?;
        let new = computed.ok_or("缺少文档值")?;
        if op == "set" {
            *target = new;
        } else {
            let rows = target.as_array_mut().ok_or("追加目标不是集合")?;
            if rows.len() >= 128 {
                return Err("集合达到容量，原记录保留".into());
            }
            rows.push(new);
        }
    } else if op == "remove" {
        let (parent, key) = ptr.rsplit_once('/').ok_or("文档路径无效")?;
        let key = key.replace("~1", "/").replace("~0", "~");
        let target = value.pointer_mut(parent).ok_or("文档父路径不存在")?;
        match target {
            Value::Array(a) => {
                let index = key
                    .parse::<usize>()
                    .ok()
                    .filter(|i| i.to_string() == key && *i < a.len())
                    .ok_or("集合索引无效")?;
                a.remove(index);
            }
            Value::Object(o) => {
                o.remove(&key).ok_or("文档字段不存在")?;
            }
            _ => return Err("移除目标无效".into()),
        }
    } else {
        return Err("未知文档操作".into());
    }
    if !crate::component_expr::bounded(value) {
        return Err("文档超出数据预算，原记录未覆盖".into());
    }
    Ok(())
}
fn validate_public_get(get: &PublicGet) -> Result<(), String> {
    let bad = || "异步网络步骤仅接受有界公开 HTTPS GET".to_string();
    let u = crate::media_network::valid_url(&get.endpoint)?;
    if get.endpoint.len() > 2048
        || !pointer(&get.pointer)
        || u.scheme() != "https"
        || u.port_or_known_default() != Some(443)
        || !u.username().is_empty()
        || u.password().is_some()
        || u.fragment().is_some()
        || get.hosts.is_empty()
        || get.hosts.len() > 4
        || !get.hosts.iter().all(|h| {
            h.len() <= 253
                && h.contains('.')
                && h.parse::<std::net::IpAddr>().is_err()
                && h.split('.').all(|s| {
                    !s.is_empty()
                        && s.len() <= 63
                        && !s.starts_with('-')
                        && !s.ends_with('-')
                        && s.bytes()
                            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-')
                })
        })
        || !u
            .host_str()
            .is_some_and(|h| get.hosts.iter().any(|allowed| allowed == h))
        || u.query_pairs().any(|(k, _)| {
            [
                "key",
                "api_key",
                "apikey",
                "token",
                "access_token",
                "password",
                "secret",
                "authorization",
            ]
            .contains(&k.to_ascii_lowercase().as_str())
        })
    {
        return Err(bad());
    }
    Ok(())
}
fn validate_async_workflows(p: &Plan, d: &Declaration) -> Result<(), String> {
    if p.async_workflows.len() > 4 {
        return Err("异步流程最多 4 条".into());
    }
    let known = p.sources.keys().cloned().collect();
    for (flow, steps) in &p.async_workflows {
        let aliases: Vec<_> = p
            .sources
            .iter()
            .filter_map(|(a, s)| {
                matches!(s, Source::Workflow { workflow } if workflow == flow).then_some(a)
            })
            .collect();
        if !name(flow, 16)
            || steps.is_empty()
            || steps.len() > 8
            || aliases.len() != 1
            || !["start", "resume", "cancel"].iter().all(|op| {
                d.actions
                    .values()
                    .any(|target| target == &format!("{}:{op}", aliases[0]))
            })
        {
            return Err("异步流程需要唯一状态源、开始/恢复/取消动作与 1–8 步".into());
        }
        let mut outputs = std::collections::BTreeSet::new();
        for step in steps {
            match step {
                AsyncStep::Get(step) => {
                    validate_public_get(&step.get)?;
                    if !name(&step.result, 16) || !outputs.insert(&step.result) {
                        return Err("异步 GET 结果名无效或重复".into());
                    }
                }
                AsyncStep::Local(step) => {
                    let (alias, op) = route(p, &step.target).ok_or("异步本地步骤目标无效")?;
                    match p.sources.get(alias) {
                        Some(Source::Counter { .. }) if matches!(op, "increment" | "decrement") => {
                            if step.pointer.is_some() || step.value.is_some() {
                                return Err("计数步骤不接受值或路径".into());
                            }
                        }
                        Some(Source::Document { initial }) if matches!(op, "append" | "set") => {
                            let ptr = step
                                .pointer
                                .as_deref()
                                .filter(|s| pointer(s) && !s.is_empty())
                                .ok_or("异步文档步骤需要非根静态路径")?;
                            if step.value.is_none()
                                || op == "set"
                                    && initial
                                        .pointer(ptr)
                                        .is_some_and(|v| v.is_array() || v.is_object())
                            {
                                return Err(
                                    "异步流程不支持移除或整体覆盖，使用已确认本地事务".into()
                                );
                            }
                        }
                        _ => return Err("异步流程只支持公开 GET、计数及非破坏性文档更新".into()),
                    }
                    for expression in step.value.iter().chain(step.when.iter()) {
                        crate::component_expr::validate(expression, &known)?;
                    }
                }
            }
        }
    }
    Ok(())
}
fn pointer(s: &str) -> bool {
    if s.len() > 128 || !s.is_empty() && !s.starts_with('/') {
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
fn within_limits(r: &Value, key: &str, value: &Value) -> bool {
    match r["limits"].get(key) {
        None => true,
        Some(bounds) => value.as_i64().is_some_and(|v| {
            bounds[0]
                .as_i64()
                .zip(bounds[1].as_i64())
                .is_some_and(|(lo, hi)| lo <= v && v <= hi)
        }),
    }
}
pub fn permissions(d: &Declaration) -> Value {
    let hosts = plan(d)
        .map(|p| {
            p.sources
                .values()
                .filter_map(|s| {
                    if let Source::JsonGet { hosts, .. } = s {
                        Some(hosts.clone())
                    } else {
                        None
                    }
                })
                .flatten()
                .chain(
                    p.async_workflows
                        .values()
                        .flatten()
                        .filter_map(|step| {
                            if let AsyncStep::Get(step) = step {
                                Some(step.get.hosts.clone())
                            } else {
                                None
                            }
                        })
                        .flatten(),
                )
                .collect::<Vec<_>>()
        })
        .unwrap_or_default();
    json!({"storage":true,"networkHosts":hosts,"microphone":false,"deviceAudio":false,"speechProvider":false,"composition":true,"durableWorkflows":d.engine==ENGINE_V3,"authenticatedWrites":false})
}

#[derive(Clone, Default)]
struct Cached {
    resources: ResourceOwner,
    owner: String,
    value: Value,
    error: bool,
    pending: bool,
    last: u64,
    generation: u64,
}
struct Job {
    resources: ResourceOwner,
    key: String,
    owner: String,
    endpoint: String,
    hosts: Vec<String>,
    generation: u64,
    workflow: Option<WorkflowJob>,
}
struct Network {
    cache: Arc<Mutex<BTreeMap<String, Cached>>>,
    tx: mpsc::SyncSender<Job>,
}
fn finish(cache: &mut BTreeMap<String, Cached>, job: &Job, result: Result<Value, String>) {
    if !job.resources.is_current() { return; }
    if let Some(v) = cache
        .get_mut(&job.key)
        .filter(|v| v.owner == job.owner && v.generation == job.generation)
    {
        v.pending = false;
        v.error = result.is_err();
        if let Ok(value) = result {
            v.value = value;
        }
    }
}
fn with_public_network(resources: &ResourceOwner, fetch: impl FnOnce() -> Result<Value, String>) -> Result<Value, String> {
    let permit = resources.acquire(Resource::PublicNetwork)?;
    permit.ensure_current()?;
    let result = fetch();
    permit.ensure_current()?;
    result
}
fn network() -> &'static Network {
    static POOL: OnceLock<Network> = OnceLock::new();
    POOL.get_or_init(|| {
        let cache: Arc<Mutex<BTreeMap<String, Cached>>> = Default::default();
        let (tx, rx) = mpsc::sync_channel::<Job>(8);
        let rx = Arc::new(Mutex::new(rx));
        for index in 0..2 {
            let cache = cache.clone();
            let rx = rx.clone();
            std::thread::Builder::new()
                .name(format!("plugin-json-{index}"))
                .spawn(move || loop {
                    let job = match rx.lock().unwrap().recv() {
                        Ok(j) => j,
                        Err(_) => break,
                    };
                    if let Some(workflow) = job.workflow {
                        let _ = execute_workflow(&workflow, &|get| {
                            workflow_fetch(&workflow.owner, get)
                        });
                        retire_workflow(&workflow);
                        continue;
                    }
                    if !cache.lock().unwrap().get(&job.key).is_some_and(|v| {
                        v.pending && v.owner == job.owner && v.generation == job.generation
                    }) {
                        continue;
                    }
                    let result = with_public_network(&job.resources, || {
                        crate::media_network::scoped_json_https(&job.endpoint, &job.hosts, 65536)
                    });
                    finish(&mut cache.lock().unwrap(), &job, result);
                })
                .expect("bounded plugin JSON worker");
        }
        Network { cache, tx }
    })
}
fn fetched(
    id: &str,
    alias: &str,
    source: &Source,
    now: u64,
    refresh: bool,
) -> (Value, &'static str) {
    cached(network(), id, alias, source, now, refresh)
}
fn cached(
    pool: &Network,
    id: &str,
    alias: &str,
    source: &Source,
    now: u64,
    refresh: bool,
) -> (Value, &'static str) {
    let Source::JsonGet {
        endpoint,
        hosts,
        refresh_sec,
    } = source
    else {
        return (Value::Null, "error");
    };
    let key = format!(
        "{id}:{alias}:{:x}",
        Sha256::digest(serde_json::to_vec(source).unwrap())
    );
    let mut cache = pool.cache.lock().unwrap();
    if !cache.contains_key(&key) && cache.len() >= 32 {
        if let Some(old) = cache
            .iter()
            .filter(|(_, v)| !v.pending)
            .min_by_key(|(_, v)| v.last)
            .map(|(k, _)| k.clone())
        {
            if let Some(retired) = cache.remove(&old) { retired.resources.cancel(); }
        } else {
            return (Value::Null, "busy");
        }
    }
    let value = cache.entry(key.clone()).or_insert_with(|| Cached {
        owner: id.into(),
        ..Default::default()
    });
    let delay = if refresh { 15 } else { *refresh_sec as u64 };
    if !value.pending && (value.last == 0 || now.saturating_sub(value.last) >= delay) {
        static GENERATION: AtomicU64 = AtomicU64::new(1);
        value.generation = GENERATION.fetch_add(1, Ordering::Relaxed);
        value.pending = true;
        value.last = now;
        if pool
            .tx
            .try_send(Job {
                resources: value.resources.clone(),
                key,
                owner: id.into(),
                endpoint: endpoint.clone(),
                hosts: hosts.clone(),
                generation: value.generation,
                workflow: None,
            })
            .is_err()
        {
            value.pending = false;
            value.error = true;
        }
    }
    (
        value.value.clone(),
        if value.pending {
            "preparing"
        } else if value.error {
            "error"
        } else {
            "ready"
        },
    )
}
// A process-wide revision fences requests resolved before any successful formal
// package mutation. It is constant-space; unrelated concurrent requests may
// need an explicit retry, but their existing background jobs are not cancelled.
static LIBRARY_REVISION: AtomicU64 = AtomicU64::new(1);
thread_local!{static RESOLVED_REVISION: std::cell::Cell<Option<u64>> = const {std::cell::Cell::new(None)};}
pub(crate) struct ResolutionScope(Option<u64>);
impl Drop for ResolutionScope{fn drop(&mut self){RESOLVED_REVISION.with(|r|r.set(self.0));}}
pub(crate) fn resolution_scope()->ResolutionScope{
    let revision=LIBRARY_REVISION.load(Ordering::Acquire);
    ResolutionScope(RESOLVED_REVISION.with(|r|r.replace(Some(revision))))
}
fn ensure_library_revision()->Result<(),String>{
    if RESOLVED_REVISION.with(|r|r.get().is_some_and(|v|v!=LIBRARY_REVISION.load(Ordering::Acquire))){
        return Err("组件库已变化，请刷新后重新操作；旧动作未执行".into());
    }
    Ok(())
}
fn revoke_component_jobs(id:&str)->Vec<WorkflowJob>{
    if let Some(pool)=NETWORK_ACCESS.get(){
        pool.cache.lock().unwrap().retain(|_,v|{if v.owner==id{v.resources.cancel();false}else{true}});
    }
    let mut active=active_workflows().lock().unwrap();
    let jobs:Vec<_>=active.values().filter(|j|j.owner==id).cloned().collect();
    for job in &jobs{job.resources.cancel();}
    active.retain(|_,j|j.owner!=id);
    jobs
}
fn checkpoint_retired(jobs:Vec<WorkflowJob>){
    let _scope=enter_owner(&ResourceOwner::fresh());
    for job in jobs{
        if let Ok(mut state)=load(&job.path(),131072){
            if let Some(run)=state.runs.get_mut(&job.flow).filter(|r|job.matches(r)){
                cancel_run(run);
                let _=save_state(&job.path(),&state,131072);
            }
        }
    }
}
/// Formal library mutations hold their existing library lock, then this record
/// lock. Workers never take the library lock. Only successful rename revokes;
/// the physical network lease stays in its worker until the GET really returns.
pub(crate) fn library_mutation<T>(id:&str,rename:impl FnOnce()->Result<T,String>)->Result<T,String>{
    let _transaction=record_transaction().lock().map_err(|_|"记录事务暂不可用")?;
    if LIBRARY_REVISION.load(Ordering::Acquire)==u64::MAX{return Err("组件库版本计数已耗尽".into());}
    let result=rename()?;
    LIBRARY_REVISION.fetch_add(1,Ordering::AcqRel);
    checkpoint_retired(revoke_component_jobs(id));
    Ok(result)
}
pub fn disable(id: &str) {
    let jobs=revoke_component_jobs(id);
    if let Ok(_guard)=record_transaction().lock(){checkpoint_retired(jobs);}
}
// A separate non-initializing handle makes disabling a package side-effect free.
static NETWORK_ACCESS: OnceLock<&'static Network> = OnceLock::new();
fn network_now() -> u64 {
    static CLOCK: OnceLock<std::time::Instant> = OnceLock::new();
    CLOCK
        .get_or_init(std::time::Instant::now)
        .elapsed()
        .as_secs()
        .saturating_add(1)
}

fn record_transaction() -> &'static Mutex<()> {
    static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
    LOCK.get_or_init(|| Mutex::new(()))
}
fn admit_workflow_get(recent: &mut BTreeMap<String, u64>, key: String, now: u64) -> bool {
    if recent
        .get(&key)
        .is_some_and(|at| now.saturating_sub(*at) < 15)
    {
        return false;
    }
    recent.retain(|_, at| now.saturating_sub(*at) < 15);
    if recent.len() >= 32 {
        return false;
    }
    recent.insert(key, now);
    true
}
fn workflow_fetch(owner: &str, get: &PublicGet) -> Result<Value, String> {
    static RECENT: OnceLock<Mutex<BTreeMap<String, u64>>> = OnceLock::new();
    let key = format!(
        "{:x}",
        Sha256::digest(crate::json_canonical::canonical_vec(&(owner, &get.endpoint, &get.hosts)).unwrap())
    );
    if !admit_workflow_get(
        &mut RECENT.get_or_init(Default::default).lock().unwrap(),
        key,
        network_now(),
    ) {
        return Err("workflow_rate_limited".into());
    }
    crate::media_network::scoped_json_https(&get.endpoint, &get.hosts, 65536)
}
fn save_state(path: &Path, state: &Saved, maximum: usize) -> Result<(), String> {
    if serde_json::to_vec(state).map_err(|_| "记录无法保存")?.len() > maximum {
        return Err("组合记录达到总容量，原数据未覆盖".into());
    }
    crate::component_app::atomic(path, state)
}
fn done(step: &Checkpoint) -> bool {
    matches!(step.phase.as_str(), "completed" | "skipped")
}
fn complete_checkpoint(run: &mut Run, index: usize, applied: bool) -> bool {
    run.steps[index].phase = if applied { "completed" } else { "skipped" }.into();
    let completed = run.steps.iter().all(done);
    if completed {
        // Terminal phase and the last effect/result must share one durable
        // replacement. A later "mark completed" write leaves a crash window.
        run.phase = "completed".into();
        run.error.clear();
    }
    completed
}
fn running(run: &Run) -> bool {
    matches!(run.phase.as_str(), "queued" | "running")
}
fn cancel_run(run: &mut Run) {
    run.phase = "cancelled".into();
    run.error.clear();
    if let Some(step) = run.steps.iter_mut().find(|s| !done(s)) {
        step.phase = "cancelled".into();
    }
}
fn fail_run(run: &mut Run, error: &str) {
    run.phase = "failed".into();
    run.error = error.into();
    if let Some(step) = run.steps.iter_mut().find(|s| !done(s)) {
        step.phase = "failed".into();
    }
}
fn workflow_value(run: Option<&Run>) -> Value {
    let Some(run) = run else {
        return json!({"phase":"idle","progress":0,"step":0,"error":""});
    };
    let completed = run.steps.iter().filter(|s| done(s)).count();
    json!({"phase":run.phase,"progress":completed*100/run.steps.len(),"step":completed,"error":run.error})
}
fn specification(d: &Declaration, r: &Value) -> String {
    format!("{:x}", Sha256::digest(crate::json_canonical::canonical_vec(&(d, r)).unwrap()))
}
#[derive(Clone)]
struct WorkflowJob {
    resources: ResourceOwner,
    owner: String,
    dir: PathBuf,
    flow: String,
    run_id: String,
    attempt: u32,
    declaration: Declaration,
    runtime: Value,
}
impl WorkflowJob {
    fn path(&self) -> PathBuf {
        self.dir.join("composition-v3-state.json")
    }
    fn key(&self) -> String {
        format!(
            "{}:{}:{}:{}",
            self.dir.display(),
            self.flow,
            self.run_id,
            self.attempt
        )
    }
    fn matches(&self, run: &Run) -> bool {
        run.id == self.run_id
            && run.attempt == self.attempt
            && run.specification == specification(&self.declaration, &self.runtime)
            && running(run)
    }
}
fn active_workflows() -> &'static Mutex<BTreeMap<String, WorkflowJob>> {
    static ACTIVE: OnceLock<Mutex<BTreeMap<String, WorkflowJob>>> = OnceLock::new();
    ACTIVE.get_or_init(|| Mutex::new(BTreeMap::new()))
}
fn active(job: &WorkflowJob) -> bool {
    job.resources.is_current() && active_workflows().lock().unwrap().get(&job.key())
        .is_some_and(|current| current.resources.same_owner(&job.resources))
}
fn register_workflow(job: &WorkflowJob) {
    if let Some(previous) = active_workflows().lock().unwrap().insert(job.key(), job.clone()) {
        if !previous.resources.same_owner(&job.resources) { previous.resources.cancel(); }
    }
}
fn retire_workflow(job: &WorkflowJob) {
    let mut active = active_workflows().lock().unwrap();
    if active.get(&job.key()).is_some_and(|current| current.resources.same_owner(&job.resources)) {
        active.remove(&job.key());
    }
    // A reused event ID/key is not authority over a newer physical owner.
    job.resources.cancel();
}
fn enqueue_workflow(job: WorkflowJob) -> bool {
    let pool = network();
    let _ = NETWORK_ACCESS.set(pool);
    // The same two workers and eight waiting slots serve ordinary JSON polls
    // and workflows, so adding workflows does not double the network budget.
    register_workflow(&job);
    let cleanup = job.clone();
    if pool
        .tx
        .try_send(Job {
            resources: job.resources.clone(),
            key: String::new(),
            owner: job.owner.clone(),
            endpoint: String::new(),
            hosts: vec![],
            generation: 0,
            workflow: Some(job),
        })
        .is_err()
    {
        retire_workflow(&cleanup);
        return false;
    }
    true
}
fn recover_workflows(dir: &Path, state: &mut Saved, spec: &str) {
    let active = active_workflows().lock().unwrap();
    for (flow, run) in &mut state.runs {
        // Legacy workers committed the last checkpoint before their separate
        // terminal-phase write. Reconcile that proven result, never execute a
        // step. Definitions/results have already been validated by the caller.
        // Other failures, cancellations and unknown GET results stay unchanged.
        if run.specification == spec
            && run.steps.iter().all(done)
            && (running(run) && run.error.is_empty()
                || run.phase == "failed" && run.error == "interrupted")
        {
            run.phase = "completed".into();
            run.error.clear();
            continue;
        }
        if running(run)
            && (run.specification != spec
                || !active
                    .values()
                    .any(|j| j.resources.is_current() && j.dir == dir && j.flow == *flow && j.matches(run)))
        {
            fail_run(
                run,
                if run.specification != spec {
                    "definition_changed"
                } else {
                    "interrupted"
                },
            );
        }
    }
}
fn validate_run_definitions(p: &Plan, state: &Saved, spec: &str) -> Result<(), String> {
    for (flow, run) in &state.runs {
        if run.specification != spec {
            continue;
        }
        let steps = p
            .async_workflows
            .get(flow)
            .ok_or("存档流程定义不兼容，原数据保留")?;
        if steps.len() != run.steps.len() {
            return Err("存档步骤数量不兼容，原数据保留".into());
        }
        let expected: std::collections::BTreeSet<_> = steps
            .iter()
            .zip(&run.steps)
            .filter_map(|(step, checkpoint)| {
                if let AsyncStep::Get(get) = step {
                    (checkpoint.phase == "completed").then_some(get.result.as_str())
                } else {
                    None
                }
            })
            .collect();
        if expected != run.results.keys().map(String::as_str).collect() {
            return Err("存档结果与步骤检查点不一致，原数据保留".into());
        }
    }
    Ok(())
}
// Local workflow expressions intentionally do not initiate periodic GETs. Their
// only network inputs are explicitly checkpointed /event/results/<name> values.
// As with status/v2 snapshots, one incompatible source is unavailable (null),
// not a failure of every independent step. A step's own target and expression
// are still checked below; saved incompatible data is never reset or replaced.
fn workflow_sources(
    p: &Plan,
    state: &mut Saved,
    event: &Value,
) -> Result<BTreeMap<String, Value>, String> {
    let mut values = BTreeMap::new();
    let known = p.sources.keys().cloned().collect();
    for alias in source_order(p)? {
        let value = match &p.sources[&alias] {
            Source::JsonGet { .. } => Value::Null,
            Source::Computed { expression } => crate::component_expr::eval(
                expression,
                &json!({"sources":values,"event":event}),
                &known,
            ).unwrap_or(Value::Null),
            source @ Source::Collection { .. } => collection_value(&alias,source,state,&values,event,None).unwrap_or(Value::Null),
            source => source_value(
                &alias,
                source,
                state,
                chrono::Utc::now().timestamp_millis(),
                None,
            ).unwrap_or(Value::Null),
        };
        values.insert(alias, value);
    }
    Ok(values)
}
fn apply_async_local(p: &Plan, state: &mut Saved, flow: &str, step: &Step) -> Result<bool, String> {
    let run = &state.runs[flow];
    let mut event = run.event.clone();
    event["results"] = serde_json::to_value(&run.results).unwrap();
    let values = workflow_sources(p, state, &event)?;
    let ctx = json!({"sources":values,"event":event});
    let known = p.sources.keys().cloned().collect();
    if let Some(condition) = &step.when {
        if !crate::component_expr::eval(condition, &ctx, &known)?
            .as_bool()
            .ok_or("条件必须是布尔值")?
        {
            return Ok(false);
        }
    }
    let (alias, op) = route(p, &step.target).ok_or("本地目标无效")?;
    if let Source::Document { .. } = &p.sources[alias] {
        let computed = step
            .value
            .as_ref()
            .map(|e| crate::component_expr::eval(e, &ctx, &known))
            .transpose()?;
        let Some(Model::Document { value }) = state.models.get_mut(alias) else {
            return Err("文档不可用".into());
        };
        if !crate::component_expr::bounded(value) {
            return Err("既有文档超出预算，原数据保留".into());
        }
        if op == "set"
            && value
                .pointer(step.pointer.as_deref().unwrap_or(""))
                .is_some_and(|v| v.is_array() || v.is_object())
        {
            return Err("异步流程不能覆盖既有集合".into());
        }
        document_step(value, step, computed)?;
    } else {
        source_value(
            alias,
            &p.sources[alias],
            state,
            chrono::Utc::now().timestamp_millis(),
            Some(op),
        )?;
    }
    Ok(true)
}
fn execute_workflow(
    job: &WorkflowJob,
    fetch: &dyn Fn(&PublicGet) -> Result<Value, String>,
) -> Result<(), String> {
    // Durable runs are independent of device visibility. Only their explicit
    // cancel/disable/retirement revokes this scope; reentry cannot revive it.
    let _scope = enter_owner(&job.resources);
    let p = plan(&job.declaration)?;
    let steps = &p.async_workflows[&job.flow];
    for _ in 0..=steps.len() {
        let guard = record_transaction().lock().map_err(|_| "记录锁不可用")?;
        let mut state = load(&job.path(), 131072)?;
        validate_run_definitions(&p, &state, &specification(&job.declaration, &job.runtime))?;
        if !active(job) || !state.runs.get(&job.flow).is_some_and(|r| job.matches(r)) {
            return Ok(());
        }
        let Some(index) = state.runs[&job.flow].steps.iter().position(|s| !done(s)) else {
            state.runs.get_mut(&job.flow).unwrap().phase = "completed".into();
            save_state(&job.path(), &state, 131072)?;
            return Ok(());
        };
        let run = state.runs.get_mut(&job.flow).unwrap();
        run.phase = "running".into();
        run.steps[index].attempts += 1;
        match &steps[index] {
            AsyncStep::Local(step) => {
                let mut candidate = state.clone();
                match apply_async_local(&p, &mut candidate, &job.flow, step) {
                    Ok(applied) => {
                        let completed = complete_checkpoint(candidate.runs.get_mut(&job.flow).unwrap(), index, applied);
                        // The model mutation and checkpoint share ONE replacement.
                        // Neither side can become visible without the other.
                        if save_state(&job.path(), &candidate, 131072).is_ok() {
                            if completed { return Ok(()); }
                            continue;
                        }
                    }
                    Err(_) => {}
                }
                fail_run(state.runs.get_mut(&job.flow).unwrap(), "local_step_failed");
                save_state(&job.path(), &state, 131072)?;
                return Ok(());
            }
            AsyncStep::Get(step) => {
                run.steps[index].phase = "running".into();
                save_state(&job.path(), &state, 131072)?;
                drop(guard);
                let result = with_public_network(&job.resources, || fetch(&step.get)).and_then(|value| {
                    value
                        .pointer(&step.get.pointer)
                        .filter(|v| crate::component_expr::bounded(v))
                        .cloned()
                        .ok_or("结果路径缺失或超预算".into())
                });
                let _guard = record_transaction().lock().map_err(|_| "记录锁不可用")?;
                let mut state = load(&job.path(), 131072)?;
                if !active(job) || !state.runs.get(&job.flow).is_some_and(|r| job.matches(r)) {
                    return Ok(());
                }
                let limited = result
                    .as_ref()
                    .is_err_and(|error| error == "workflow_rate_limited");
                match result {
                    Ok(value) => {
                        let mut candidate = state.clone();
                        let run = candidate.runs.get_mut(&job.flow).unwrap();
                        run.results.insert(step.result.clone(), value);
                        let completed = complete_checkpoint(run, index, true);
                        if save_state(&job.path(), &candidate, 131072).is_ok() {
                            if completed { return Ok(()); }
                            continue;
                        }
                    }
                    Err(_) => {}
                }
                fail_run(
                    state.runs.get_mut(&job.flow).unwrap(),
                    if limited {
                        "rate_limited"
                    } else {
                        "get_step_failed"
                    },
                );
                save_state(&job.path(), &state, 131072)?;
                return Ok(());
            }
        }
    }
    Err("异步流程步骤预算耗尽".into())
}

fn load(path: &Path, maximum: usize) -> Result<Saved, String> {
    use std::io::Read;
    let bytes: std::io::Result<Vec<u8>> = (|| {
        let file = std::fs::File::open(path)?;
        let mut bytes = Vec::new();
        file.take((maximum + 1) as u64).read_to_end(&mut bytes)?;
        Ok(bytes)
    })();
    match bytes {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(Saved {
            version: 1,
            ..Default::default()
        }),
        Ok(bytes) if bytes.len() <= maximum => {
            let s: Saved =
                serde_json::from_slice(&bytes).map_err(|_| "组合应用存档损坏，原数据未覆盖")?;
            let unique: std::collections::BTreeSet<_> =
                s.receipts.iter().map(|r| r.id.as_str()).collect();
            if s.version != 1
                || s.models.len() > 64
                || s.receipts.len() > 128
                || unique.len() != s.receipts.len()
                || s.receipts.iter().any(|r| {
                    !event_id(&r.id)
                        || r.fingerprint.len() != 64
                        || !r.fingerprint.bytes().all(|c| c.is_ascii_hexdigit())
                })
                || s.runs.len() > 4
                || s.runs.values().filter(|r| running(r)).count() > 1
                || s.runs.iter().any(|(flow, run)| {
                    !name(flow, 16)
                        || !event_id(&run.id)
                        || run.specification.len() != 64
                        || !run.specification.bytes().all(|c| c.is_ascii_hexdigit())
                        || !(1..=8).contains(&run.attempt)
                        || !matches!(
                            run.phase.as_str(),
                            "queued" | "running" | "completed" | "failed" | "cancelled"
                        )
                        || !run.event.is_object()
                        || !crate::component_expr::bounded(&run.event)
                        || run.results.len() > 8
                        || run
                            .results
                            .iter()
                            .any(|(k, v)| !name(k, 16) || !crate::component_expr::bounded(v))
                        || !matches!(
                            run.error.as_str(),
                            "" | "interrupted"
                                | "definition_changed"
                                | "queue_full"
                                | "local_step_failed"
                                | "get_step_failed"
                                | "rate_limited"
                        )
                        || run.steps.is_empty()
                        || run.steps.len() > 8
                        || run.error.len() > 32
                        || run.steps.iter().any(|s| {
                            s.attempts > run.attempt
                                || done(s) && s.attempts == 0
                                || !matches!(
                                    s.phase.as_str(),
                                    "pending"
                                        | "running"
                                        | "completed"
                                        | "skipped"
                                        | "failed"
                                        | "cancelled"
                                )
                        })
                        || run.steps.windows(2).any(|s| !done(&s[0]) && done(&s[1]))
                        || run
                            .steps
                            .iter()
                            .filter(|s| {
                                matches!(s.phase.as_str(), "running" | "failed" | "cancelled")
                            })
                            .count()
                            > 1
                        || run.phase == "completed" && !run.steps.iter().all(done)
                })
            {
                return Err("组合应用存档版本或大小无效，原数据未覆盖".into());
            }
            Ok(s)
        }
        _ => Err("无法读取组合应用存档，原数据未覆盖".into()),
    }
}
fn source_value(
    alias: &str,
    source: &Source,
    s: &mut Saved,
    now: i64,
    op: Option<&str>,
) -> Result<Value, String> {
    let bad = || "组合服务配置与既有存档不兼容，未覆盖数据".to_string();
    if matches!(
        source,
        Source::Counter { .. }
            | Source::Timer { .. }
            | Source::Ledger { .. }
            | Source::Document { .. }
    ) && !s.models.contains_key(alias)
        && s.models.len() >= 64
    {
        return Err("组合存档达到配额，既有数据未覆盖".into());
    }
    match source {
        Source::Workflow { workflow } => {
            if op.is_some() {
                return Err("异步流程操作由持久调度器执行".into());
            }
            Ok(workflow_value(s.runs.get(workflow)))
        }
        Source::Computed { .. } | Source::Collection { .. } => Err("计算/集合源由有界依赖调度器执行".into()),
        Source::Document { initial } => {
            if op.is_some() {
                return Err("文档写入必须由声明流程提供路径和值".into());
            }
            let model = s
                .models
                .entry(alias.into())
                .or_insert_with(|| Model::Document {
                    value: initial.clone(),
                });
            let Model::Document { value } = model else {
                return Err(bad());
            };
            if !crate::component_expr::bounded(value) {
                return Err(bad());
            }
            Ok(value.clone())
        }
        Source::Constant { value } => Ok(value.clone()),
        Source::Clock {} => {
            use chrono::{Datelike, TimeZone, Timelike};
            let t = chrono::Local
                .timestamp_millis_opt(now)
                .single()
                .ok_or("系统时间无效")?;
            Ok(
                json!({"iso":t.to_rfc3339(),"unix_ms":now,"hour":t.hour(),"minute":t.minute(),"second":t.second(),"year":t.year(),"month":t.month(),"day":t.day()}),
            )
        }
        Source::Counter {
            initial,
            min,
            max,
            step,
        } => {
            if op == Some("reset") {
                s.models
                    .insert(alias.into(), Model::Counter { value: *initial });
            }
            let m = s
                .models
                .entry(alias.into())
                .or_insert(Model::Counter { value: *initial });
            let Model::Counter { value } = m else {
                return Err(bad());
            };
            if *value < *min || *value > *max {
                return Err(bad());
            }
            let delta = match op {
                Some("increment") => *step as i64,
                Some("decrement") => -(*step as i64),
                _ => 0,
            };
            if op == Some("reset") {
                *value = *initial;
            } else {
                *value = (*value as i64 + delta).clamp(*min as i64, *max as i64) as i32;
            }
            Ok(json!({"value":value}))
        }
        Source::Timer { seconds } => {
            let max = *seconds as i64 * 1000;
            if op == Some("reset") {
                s.models.insert(
                    alias.into(),
                    Model::Timer {
                        remaining_ms: max,
                        started_ms: None,
                    },
                );
            }
            let m = s.models.entry(alias.into()).or_insert(Model::Timer {
                remaining_ms: max,
                started_ms: None,
            });
            let Model::Timer {
                remaining_ms,
                started_ms,
            } = m
            else {
                return Err(bad());
            };
            if !(0..=max).contains(remaining_ms)
                || started_ms.is_some_and(|t| t < 0 || t > now + 300000)
            {
                return Err(bad());
            }
            let remaining = started_ms.map_or(*remaining_ms, |at| {
                remaining_ms
                    .saturating_sub(now.saturating_sub(at).max(0))
                    .max(0)
            });
            match op {
                Some("pause") => {
                    *remaining_ms = remaining;
                    *started_ms = None;
                }
                Some("reset") => {
                    *remaining_ms = max;
                    *started_ms = None;
                }
                Some("start") if remaining > 0 && started_ms.is_none() => {
                    *started_ms = Some(now);
                }
                _ => {}
            }
            let remaining = started_ms.map_or(*remaining_ms, |at| {
                remaining_ms
                    .saturating_sub(now.saturating_sub(at).max(0))
                    .max(0)
            });
            if remaining == 0 {
                *remaining_ms = 0;
                *started_ms = None;
            }
            Ok(
                json!({"seconds":(remaining+999)/1000,"progress":((max-remaining)*100/max).clamp(0,100),"phase":if remaining==0{"finished"}else if started_ms.is_some(){"running"}else{"paused"}}),
            )
        }
        Source::JsonGet { .. } => Err("网络源由有界后台执行器处理".into()),
        Source::Ledger {
            unit,
            target,
            capacity,
            label,
        } => {
            let model = s
                .models
                .entry(alias.into())
                .or_insert_with(|| Model::Ledger {
                    data: crate::component_ledger::empty(),
                });
            let Model::Ledger { data } = model else {
                return Err(bad());
            };
            crate::component_ledger::apply(
                &crate::component_ledger::Config {
                    unit,
                    target: *target,
                    capacity: *capacity,
                    label,
                },
                data,
                now,
                op,
            )
        }
    }
}
fn event_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 120
        && id
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b":_-".contains(&b))
}
fn collection_value(alias:&str,source:&Source,state:&mut Saved,
    values:&BTreeMap<String,Value>,event:&Value,op:Option<&str>)->Result<Value,String>{
    let Source::Collection{expression,view}=source else{return Err("无效集合源".into());};
    let known=values.keys().cloned().collect();
    let items=crate::component_expr::eval(expression,&json!({"sources":values,"event":event}),&known)?;
    let mut model=match state.models.get(alias){
        Some(Model::Collection{view})=>view.clone(),
        Some(_)=>return Err("集合视图与既有存档不兼容，原数据保留".into()),
        None if state.models.len()>=64=>return Err("组合存档达到配额，原数据保留".into()),
        None=>crate::component_collection::State::default(),
    };
    let value=crate::component_collection::project(view,&items,&mut model,op)?;
    state.models.insert(alias.into(),Model::Collection{view:model});
    Ok(value)
}
fn snapshot_sources(
    id: &str,
    p: &Plan,
    state: &mut Saved,
    now: i64,
    selected: Option<(&str, &str)>,
    event: &Value,
) -> Result<(BTreeMap<String, Value>, BTreeMap<String, &'static str>), String> {
    let known = p.sources.keys().cloned().collect();
    let mut values = BTreeMap::new();
    let mut status = BTreeMap::new();
    for alias in source_order(p)? {
        let source = &p.sources[&alias];
        let op = selected.filter(|(a, _)| *a == alias).map(|(_, op)| op);
        if matches!(source, Source::JsonGet { .. }) {
            let pool = network();
            let _ = NETWORK_ACCESS.set(pool);
            let (v, phase) = fetched(id, &alias, source, network_now(), op == Some("refresh"));
            values.insert(alias.clone(), v);
            status.insert(alias, phase);
            continue;
        }
        let value = if matches!(source,Source::Collection{..}){
            collection_value(&alias,source,state,&values,event,op)
        } else if let Source::Computed { expression } = source {
            crate::component_expr::eval(
                expression,
                &json!({"sources":values,"event":event}),
                &known,
            )
        } else {
            source_value(&alias, source, state, now, op)
        };
        match value {
            Ok(v) => {
                values.insert(alias.clone(), v);
                status.insert(alias, "ready");
            }
            Err(e) if op.is_some() => return Err(e),
            Err(_) => {
                values.insert(alias.clone(), Value::Null);
                status.insert(alias, "error");
            }
        }
    }
    Ok((values, status))
}
pub fn request(
    id: &str,
    dir: &Path,
    d: &Declaration,
    r: &Value,
    input: &Value,
) -> Result<Value, String> {
    request_with_dispatch(id, dir, d, r, input, &enqueue_workflow)
}
fn request_with_dispatch(
    id: &str,
    dir: &Path,
    d: &Declaration,
    r: &Value,
    input: &Value,
    dispatch: &dyn Fn(WorkflowJob) -> bool,
) -> Result<Value, String> {
    // UI commands and device events run on separate threads. Serialize the
    // bounded local read/modify/atomic-replace transaction so neither can
    // overwrite a newer record. Public GET runs only in the background pool;
    // microphone, audio, models and desktop tasks never acquire this lock.
    let _transaction = record_transaction()
        .lock()
        .map_err(|_| "记录事务暂不可用，原数据未修改")?;
    ensure_library_revision()?;
    let p = plan(d)?;
    let v2 = d.engine != ENGINE;
    let v3 = d.engine == ENGINE_V3;
    if v2 {
        validate(d, r)?;
    }
    let operation = input["operation"].as_str().unwrap_or("");
    let selected = if operation == "action" {
        let action = input["action"].as_str().ok_or("缺少组合动作")?;
        Some(route(&p, d.actions.get(action).ok_or("动作未在资源包声明")?).ok_or("组合动作无效")?)
    } else if matches!(operation, "status" | "enable") {
        None
    } else {
        return Err("不支持的组合操作".into());
    };
    let path = dir.join(if v3 {
        "composition-v3-state.json"
    } else if v2 {
        "composition-v2-state.json"
    } else {
        "composition-state.json"
    });
    let mut state = if v3 && !path.exists() && dir.join("composition-v2-state.json").exists() {
        load(&dir.join("composition-v2-state.json"), 65536)?
    } else if v2 && !path.exists() {
        load(&dir.join("composition-state.json"), 16384)?
    } else {
        load(
            &path,
            if v3 {
                131072
            } else if v2 {
                65536
            } else {
                16384
            },
        )?
    };
    let before = serde_json::to_value(&state).unwrap();
    if v3 {
        validate_run_definitions(&p, &state, &specification(d, r))?;
        recover_workflows(dir, &mut state, &specification(d, r));
    }
    let context = |s: &Saved| {
        format!(
            "{:x}",
            Sha256::digest(if v2 {
                serde_json::to_vec(&(s, d, r)).unwrap()
            } else {
                serde_json::to_vec(&(s, &d.content)).unwrap()
            })
        )
    };
    let event = input.get("data").cloned().unwrap_or(json!({}));
    if v2 && !crate::component_expr::bounded(&event) {
        return Err("动作数据超出预算，未执行".into());
    }
    if v3 && (!event.is_object() || event.get("results").is_some()) {
        return Err("异步动作数据必须为有界对象，results 为宿主保留字段".into());
    }
    let fingerprint = format!(
        "{:x}",
        Sha256::digest(crate::json_canonical::canonical_vec(&(d, r, &input["action"], &event)).unwrap())
    );
    let receipt_id = if v2 && operation == "action" {
        Some(
            input
                .get("_eventId")
                .or_else(|| input.get("eventId"))
                .and_then(Value::as_str)
                .filter(|s| event_id(s))
                .ok_or("组合 v2 动作缺少稳定请求编号，未执行")?,
        )
    } else {
        None
    };
    let duplicate = if let Some(id) = receipt_id {
        if let Some(previous) = state.receipts.iter().find(|r| r.id == id) {
            if previous.fingerprint != fingerprint {
                return Err("请求编号已被不同动作使用，未执行".into());
            }
            true
        } else {
            false
        }
    } else {
        false
    };
    if v2
        && operation == "action"
        && !duplicate
        && input["context"].as_str() != Some(context(&state).as_str())
    {
        return Err("操作内容版本已变化，请刷新后重试；未执行或重发".into());
    }
    if operation == "action"
        && !duplicate
        && r["input"]["confirm"]
            .get(input["action"].as_str().unwrap_or(""))
            .is_some()
        && (input["confirm"] != true || input["context"].as_str() != Some(context(&state).as_str()))
    {
        return Err("确认已过期或尚未确认，组合动作未执行".into());
    }
    let now = chrono::Utc::now().timestamp_millis();
    let effective = selected.filter(|_| !duplicate);
    let workflow_selected = effective.and_then(|(alias, op)| {
        if let Some(Source::Workflow { workflow }) = p.sources.get(alias) {
            Some((workflow.as_str(), op))
        } else {
            None
        }
    });
    if v3 && state.runs.values().any(running) && effective.is_some() && workflow_selected.is_none()
    {
        return Err("异步流程进行中，本地变更暂被锁定；可取消后操作".into());
    }
    let mut submitted = None;
    let mut cancelled = None;
    if let Some((flow, op)) = workflow_selected {
        match op {
            "start" => {
                if !state.runs.contains_key(flow) && state.runs.len() >= 4 {
                    return Err("流程历史已达 4 个名称，原检查点保留".into());
                }
                if state.runs.values().any(running)
                    || state
                        .runs
                        .get(flow)
                        .is_some_and(|run| run.phase == "failed")
                {
                    return Err("流程正在执行或需要先恢复/取消失败任务".into());
                }
                state.runs.insert(
                    flow.into(),
                    Run {
                        id: receipt_id.unwrap().into(),
                        specification: specification(d, r),
                        attempt: 1,
                        phase: "queued".into(),
                        event: event.clone(),
                        results: BTreeMap::new(),
                        error: String::new(),
                        steps: p.async_workflows[flow]
                            .iter()
                            .map(|_| Checkpoint {
                                phase: "pending".into(),
                                attempts: 0,
                            })
                            .collect(),
                    },
                );
            }
            "resume" => {
                if state.runs.values().any(running) {
                    return Err("已有异步流程正在执行".into());
                }
                let run = state
                    .runs
                    .get_mut(flow)
                    .filter(|run| {
                        run.phase == "failed"
                            && run.specification == specification(d, r)
                            && run.attempt < 8
                    })
                    .ok_or("只能恢复同版本的失败流程；最多 8 次尝试")?;
                run.attempt += 1;
                run.phase = "queued".into();
                run.error.clear();
                if let Some(step) = run.steps.iter_mut().find(|s| !done(s)) {
                    step.phase = "pending".into();
                }
            }
            "cancel" => {
                let run = state
                    .runs
                    .get_mut(flow)
                    .filter(|run| running(run) || run.phase == "failed")
                    .ok_or("没有可取消流程")?;
                cancelled = Some((flow.to_string(), run.id.clone(), run.attempt));
                cancel_run(run);
            }
            _ => return Err("未知异步流程操作".into()),
        }
        if op != "cancel" {
            let run = &state.runs[flow];
            let job = WorkflowJob {
                resources: ResourceOwner::fresh(),
                owner: id.into(),
                dir: dir.into(),
                flow: flow.into(),
                run_id: run.id.clone(),
                attempt: run.attempt,
                declaration: d.clone(),
                runtime: r.clone(),
            };
            // The transaction lock keeps workers from reading before the final
            // receipt/run save. Failed persistence revokes this job below.
            if dispatch(job.clone()) {
                submitted = Some(job);
            } else {
                fail_run(state.runs.get_mut(flow).unwrap(), "queue_full");
            }
        }
    }
    let mut trace = Vec::new();
    if let Some(("flow", flow)) = effective {
        let known = p.sources.keys().cloned().collect();
        for step in &p.workflows[flow] {
            let (values, _) = snapshot_sources(id, &p, &mut state, now, None, &event)?;
            let ctx = json!({"sources":values,"event":event});
            if let Some(condition) = &step.when {
                let allowed = crate::component_expr::eval(condition, &ctx, &known)?
                    .as_bool()
                    .ok_or("流程条件不是布尔值")?;
                if !allowed {
                    trace.push(json!({"target":step.target,"phase":"skipped"}));
                    continue;
                }
            }
            let (alias, op) = route(&p, &step.target).ok_or("流程目标无效")?;
            if matches!(p.sources[alias], Source::Document { .. }) {
                let computed = step
                    .value
                    .as_ref()
                    .map(|e| crate::component_expr::eval(e, &ctx, &known))
                    .transpose()?;
                let Some(Model::Document { value }) = state.models.get_mut(alias) else {
                    return Err("文档源暂不可用，流程未提交".into());
                };
                if op == "set"
                    && value
                        .pointer(step.pointer.as_deref().unwrap_or(""))
                        .is_some_and(|v| v.is_array() || v.is_object())
                    && !r["input"]["confirm"][input["action"].as_str().unwrap_or("")]
                        .as_str()
                        .is_some_and(|s| !s.trim().is_empty())
                {
                    return Err("整体替换记录集合需要声明并执行确认，原记录未修改".into());
                }
                document_step(value, step, computed)?;
            } else {
                source_value(alias, &p.sources[alias], &mut state, now, Some(op))?;
            }
            trace.push(json!({"target":step.target,"phase":"applied"}));
        }
    }
    let (sources, status) = snapshot_sources(
        id,
        &p,
        &mut state,
        now,
        effective.filter(|(alias, _)| {
            *alias != "flow" && !matches!(p.sources.get(*alias), Some(Source::Workflow { .. }))
        }),
        &event,
    )?;
    if let Some(id) = receipt_id.filter(|_| !duplicate) {
        state.receipts.push(Receipt {
            id: id.into(),
            fingerprint,
        });
        if state.receipts.len() > 128 {
            state.receipts.remove(0);
        }
    }
    let mut variables = serde_json::Map::new();
    let mut errors = Vec::new();
    for (key, b) in &p.bindings {
        let raw = sources.get(&b.source).and_then(|s| s.pointer(&b.pointer));
        let kind = r["vars"][key]["type"].as_str().unwrap_or("");
        let projected = raw.and_then(|v| {
            if kind == "string" && v.is_number() {
                Some(Value::String(v.to_string()))
            } else {
                Some(v.clone())
            }
        });
        let value = match projected.filter(|v| scalar(v, kind) && within_limits(r, key, v)) {
            Some(v) => v,
            None => {
                errors.push(key.clone());
                b.fallback.clone()
            }
        };
        variables.insert(key.clone(), value);
    }
    if before != serde_json::to_value(&state).unwrap() {
        if let Err(error) = save_state(
            &path,
            &state,
            if v3 {
                131072
            } else if v2 {
                65536
            } else {
                16384
            },
        ) {
            if let Some(job) = submitted {
                retire_workflow(&job);
            }
            return Err(error);
        }
    }
    // The cancellation checkpoint is durable before revoking the worker. The
    // existing transaction lock prevents any interleaved step commit here.
    if let Some((flow, run_id, attempt)) = cancelled {
        let mut active = active_workflows().lock().unwrap();
        active.retain(|_, job| {
            let matches = job.dir == dir && job.flow == flow && job.run_id == run_id && job.attempt == attempt;
            if matches { job.resources.cancel(); }
            !matches
        });
    }
    let mut values = json!({});
    for k in [
        "eyebrow",
        "headline",
        "metricValue",
        "metricLabel",
        "metricUnit",
        "badge",
        "note",
        "footer",
    ] {
        values[k] = r["dashboard"][k].as_str().unwrap_or("").into();
    }
    if !errors.is_empty() || status.values().any(|v| *v != "ready") {
        values["note"] = "部分数据等待更新或暂不可用；已有记录保留，未自动重置".into();
    }
    let actions: Vec<_> = d
        .actions
        .iter()
        .filter(|(_, target)| {
            route(&p, target).is_some_and(|(alias, op)| {
                if let Some(Source::Workflow { workflow }) = p.sources.get(alias) {
                    let run = state.runs.get(workflow);
                    return match op {
                        "start" => {
                            !state.runs.values().any(running)
                                && !run.is_some_and(|r| r.phase == "failed")
                                && (run.is_some() || state.runs.len() < 4)
                        }
                        "resume" => {
                            !state.runs.values().any(running)
                                && run.is_some_and(|run| {
                                    run.phase == "failed"
                                        && run.attempt < 8
                                        && run.specification == specification(d, r)
                                })
                        }
                        "cancel" => run.is_some_and(|r| running(r) || r.phase == "failed"),
                        _ => false,
                    };
                }
                if v3 && state.runs.values().any(running) {
                    return false;
                }
                if matches!(p.sources.get(alias),Some(Source::Collection{..})){
                    return status.get(alias)==Some(&"ready")&&sources.get(alias)
                        .is_some_and(|value|crate::component_collection::enabled(value,op));
                }
                if alias == "flow" {
                    return p.workflows[op].iter().all(|s| {
                        s.target
                            .split_once(':')
                            .is_some_and(|(a, _)| status.get(a) == Some(&"ready"))
                    });
                }
                let ready = status.get(alias) != Some(&"error")
                    || op == "reset"
                    || matches!(p.sources[alias], Source::JsonGet { .. });
                if let (
                    Source::Ledger {
                        unit,
                        target,
                        capacity,
                        label,
                    },
                    Some(Model::Ledger { data }),
                ) = (&p.sources[alias], state.models.get(alias))
                {
                    ready
                        && crate::component_ledger::allowed(
                            &crate::component_ledger::Config {
                                unit,
                                target: *target,
                                capacity: *capacity,
                                label,
                            },
                            data,
                            now,
                            op,
                        )
                } else {
                    ready
                }
            })
        })
        .map(|(action, _)| action)
        .collect();
    let mut response=json!({"enabled":true,"service":"composition","variables":variables,"values":values,"ui":{"mode":0,"actions":actions,"context":context(&state)},"permissions":permissions(d),"sources":status,"bindingErrors":errors,"workflowSteps":trace,"workflows":state.runs.iter().map(|(name, run)| (name.clone(),json!({"state":workflow_value(Some(run)),"steps":run.steps}))).collect::<BTreeMap<_,_>>(),"duplicate":duplicate});
    if let Some(collection)=r["surface"].get("collection_view"){
        let config=crate::app_host_collection::Config::parse(collection)?;
        if status.get(&config.source)==Some(&"ready") {
            if let Some(model)=sources.get(&config.source){
                response["collectionView"]=crate::app_host_collection::project(&config.source,config.visible,model)?;
            }
        }
    }
    Ok(response)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn host_collection_contract()->(Declaration,Value){
        let s:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/host-collection-v1.json")).unwrap();
        let mut r=s["runtimeBase"].clone();r["surface"]["collection_view"]=s["config"].clone();
        let mut d=s["declarationBase"].clone();d["content"][0]["sources"]["records"]["initial"]=s["records"].clone();
        (crate::app_services::parse(&serde_json::to_vec(&d).unwrap(),&r).unwrap(),r)
    }
    #[test]
    fn host_collection_real_source_projection_pages_selection_and_dedup(){
        let (d,r)=host_collection_contract();let dir=tempfile::tempdir().unwrap();
        let req=|v:&Value|request("host-list",dir.path(),&d,&r,v).unwrap();
        let first=req(&json!({"operation":"status"}));assert_eq!(first["variables"],json!({"count":3}));
        let view=&first["collectionView"];crate::app_host_collection::validate_snapshot(view).unwrap();
        assert_eq!(view["rows"][0]["label"],"第一项");assert!(view.get("selected").is_none());
        let op=json!({"operation":"action","action":"host.pick_b","eventId":"choose-beta","context":first["ui"]["context"]});
        let chosen=req(&op);assert_eq!(chosen["collectionView"]["selectionKey"],"beta");assert_eq!(req(&op)["duplicate"],true);
        let next=req(&json!({"operation":"action","action":"host.next","eventId":"page-next","context":chosen["ui"]["context"]}));
        assert_eq!(next["collectionView"]["offset"],2);assert_eq!(next["collectionView"]["rows"][0]["key"],"gamma");
        assert_eq!(next["collectionView"]["rows"][1]["present"],false);assert_eq!(next["collectionView"]["selectionKey"],"beta");
        assert!(!next["ui"]["actions"].as_array().unwrap().contains(&json!("host.pick_b")));
        let path=dir.path().join("composition-v2-state.json");let before=std::fs::read(&path).unwrap();
        assert!(request("host-list",dir.path(),&d,&r,&json!({"operation":"action","action":"host.pick_a","eventId":"stale","context":first["ui"]["context"]})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(),before);assert_eq!(req(&json!({"operation":"status"}))["collectionView"],next["collectionView"]);
        std::fs::write(&path,b"unreadable preserved fixture").unwrap();assert!(request("host-list",dir.path(),&d,&r,&json!({"operation":"status"})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(),b"unreadable preserved fixture");
    }
    #[test]
    fn host_collection_rejects_unowned_source_and_mismatched_row_commands(){
        let (d,r)=host_collection_contract();let value=serde_json::to_value(&d).unwrap();
        for (pointer,bad) in [("/content/0/sources/items/view/page_size",json!(3)),("/actions/host.pick_b",json!("items:row_0")),("/content/0/sources/items/kind",json!("computed"))]{
            let mut candidate=value.clone();*candidate.pointer_mut(pointer).unwrap()=bad;
            assert!(crate::app_services::parse(&serde_json::to_vec(&candidate).unwrap(),&r).is_err());
        }
        let mut absent=r.clone();absent["surface"]["collection_view"]["source"]=json!("missing");assert!(validate(&d,&absent).is_err());
    }
    #[test]
    fn collection_shared_contract_and_real_stable_key_requests(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/collection-v1.json")).unwrap();
        let runtime=suite["runtime"].clone();
        let declaration=crate::app_services::parse(&serde_json::to_vec(&suite["declaration"]).unwrap(),&runtime).unwrap();
        for case in suite["rejections"].as_array().unwrap(){
            let mut bad=suite["declaration"].clone();
            let (parent,key)=case["pointer"].as_str().unwrap().rsplit_once('/').unwrap();
            bad.pointer_mut(parent).unwrap()[key]=case["value"].clone();
            assert!(crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(),&runtime).is_err(),"{case}");
        }
        let dir=tempfile::tempdir().unwrap();
        let req=|input:&Value|request("collection-contract",dir.path(),&declaration,&runtime,input).unwrap();
        let initial=req(&json!({"operation":"status"}));
        assert_eq!(initial["variables"],suite["initial"]);assert_eq!(initial["bindingErrors"],json!([]));
        assert!(!initial["ui"]["actions"].as_array().unwrap().contains(&json!("host.previous")));
        let action=json!({"operation":"action","action":"host.row_b","eventId":"select-b","context":initial["ui"]["context"]});
        let selected=req(&action);assert_eq!(selected["variables"]["chosen"],"B");
        let path=dir.path().join("composition-v2-state.json");
        let mut saved=load(&path,65536).unwrap();
        let Model::Document{value}=saved.models.get_mut("records").unwrap() else{panic!()};
        *value=json!([{"id":"c","label":"C"},{"id":"a","label":"A"},{"id":"b","label":"B"}]);
        save_state(&path,&saved,65536).unwrap();
        let reordered=req(&json!({"operation":"status"}));
        assert_eq!(reordered["variables"]["rowa"],"C");assert_eq!(reordered["variables"]["chosen"],"B");
        let before=std::fs::read(&path).unwrap();
        assert!(request("collection-contract",dir.path(),&declaration,&runtime,
            &json!({"operation":"action","action":"host.row_a","eventId":"stale-row","context":selected["ui"]["context"]})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(),before);
        assert_eq!(req(&action)["duplicate"],true);
        let page=req(&json!({"operation":"action","action":"host.next","eventId":"next-page","context":reordered["ui"]["context"]}));
        assert_eq!(page["variables"]["rowa"],"B");assert_eq!(page["variables"]["rowb"],"");
        assert_eq!(page["bindingErrors"],json!([]));
        assert!(!page["ui"]["actions"].as_array().unwrap().contains(&json!("host.row_b")));
        assert!(!page["ui"]["actions"].as_array().unwrap().contains(&json!("host.next")));
        let persisted=load(&path,65536).unwrap();
        let Model::Collection{view}=persisted.models.get("list").unwrap() else{panic!()};
        assert_eq!(view.selected.as_deref(),Some("b"));assert_eq!(view.offset,2);
        assert_eq!(req(&json!({"operation":"status"}))["variables"],page["variables"]);
    }
    #[test]
    fn collection_incompatible_saved_view_is_preserved_without_blocking_document(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/collection-v1.json")).unwrap();
        let r=&suite["runtime"];let d=crate::app_services::parse(&serde_json::to_vec(&suite["declaration"]).unwrap(),r).unwrap();
        let dir=tempfile::tempdir().unwrap();request("collection-mismatch",dir.path(),&d,r,&json!({"operation":"status"})).unwrap();
        let path=dir.path().join("composition-v2-state.json");let mut saved=load(&path,65536).unwrap();
        saved.models.insert("list".into(),Model::Counter{value:42});save_state(&path,&saved,65536).unwrap();
        let before=std::fs::read(&path).unwrap();
        let status=request("collection-mismatch",dir.path(),&d,r,&json!({"operation":"status"})).unwrap();
        assert_eq!(status["sources"]["list"],"error");assert_eq!(status["sources"]["records"],"ready");
        assert!(request("collection-mismatch",dir.path(),&d,r,&json!({"operation":"action","action":"host.row_a","eventId":"no-reset","context":status["ui"]["context"]})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(),before);
    }
    fn v3_contract() -> (Declaration, Value) {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/composition-v3.json"
        ))
        .unwrap();
        let runtime = suite["runtime"].clone();
        let d = crate::app_services::parse(
            &serde_json::to_vec(&suite["declaration"]).unwrap(),
            &runtime,
        )
        .unwrap();
        (d, runtime)
    }
    fn capture_job(queue: &Mutex<Vec<WorkflowJob>>, job: WorkflowJob) -> bool {
        register_workflow(&job);
        queue.lock().unwrap().push(job);
        true
    }
    fn run_captured(
        queue: &Mutex<Vec<WorkflowJob>>,
        fetch: &dyn Fn(&PublicGet) -> Result<Value, String>,
    ) -> WorkflowJob {
        let job = queue.lock().unwrap().remove(0);
        execute_workflow(&job, fetch).unwrap();
        retire_workflow(&job);
        job
    }
    fn phase2_package(path:&Path,id:&str,version:&str){
        std::fs::create_dir_all(path.join("runtime")).unwrap();std::fs::create_dir_all(path.join("assets")).unwrap();
        for (name,value) in [
            ("component.json",json!({"id":id,"name":"Lifecycle fixture","version":version,"kind":"tool","description":"test"})),
            ("negative-screen.json",json!({"dashboard":{"title":"Lifecycle fixture"}})),
            ("buttons.json",json!([])),("runtime/widget.json",json!({"schema_version":1})),("share.json",json!({"title":"Lifecycle fixture"}))]{
            std::fs::write(path.join(name),serde_json::to_vec(&value).unwrap()).unwrap();
        }
        std::fs::write(path.join("assets/.keep"),b"").unwrap();
    }
    #[test]
    fn phase2_formal_delete_and_replace_retire_late_get_without_early_lease_release(){
        use crate::component_library::{publish_source,delete,DeleteLibraryComponentInput};
        for replace in [false,true]{
            let id=if replace{"phase2-library-replace"}else{"phase2-library-delete"};
            let root=tempfile::tempdir().unwrap();let home=root.path().join("home");let source=root.path().join("source");
            phase2_package(&source,id,"1.0.0");let published=publish_source(&home,&source).unwrap();
            let (d,r)=v3_contract();let dir=root.path().join("state");std::fs::create_dir_all(&dir).unwrap();
            let queue=Mutex::new(Vec::new());let dispatch=|job|capture_job(&queue,job);
            let first=request_with_dispatch(id,&dir,&d,&r,&json!({"operation":"status"}),&dispatch).unwrap();
            request_with_dispatch(id,&dir,&d,&r,&json!({"operation":"action","action":"host.start","eventId":"phase2-start","context":first["ui"]["context"]}),&dispatch).unwrap();
            let mut job=queue.lock().unwrap().remove(0);
            let observer=ResourceOwner::isolated_for_test([1,4]);job.resources=observer.sibling_for_test();register_workflow(&job);
            let (ready_tx,ready_rx)=std::sync::mpsc::channel();let (release_tx,release_rx)=std::sync::mpsc::channel();
            let physical=job.clone();
            let worker=std::thread::spawn(move||{
                let calls=std::sync::atomic::AtomicUsize::new(0);
                execute_workflow(&physical,&|_|{
                    assert_eq!(calls.fetch_add(1,Ordering::SeqCst),0,"no subsequent GET");
                    ready_tx.send(()).unwrap();
                    release_rx.recv_timeout(std::time::Duration::from_secs(5)).unwrap();
                    Ok(json!({"record":{"title":"must not commit"}}))
                })
            });
            ready_rx.recv_timeout(std::time::Duration::from_secs(5)).unwrap();
            assert_eq!(observer.used_for_test(Resource::PublicNetwork),1);
            if replace{phase2_package(&source,id,"1.0.1");publish_source(&home,&source).unwrap();}
            else{delete(&home,DeleteLibraryComponentInput{path:published.path.clone()}).unwrap();}
            assert!(!job.resources.is_current());
            assert_eq!(observer.used_for_test(Resource::PublicNetwork),1,"revocation is not physical completion");
            assert!(observer.try_acquire(Resource::PublicNetwork).is_err());
            let checkpoint=std::fs::read(job.path()).unwrap();
            release_tx.send(()).unwrap();worker.join().unwrap().unwrap();
            assert_eq!(std::fs::read(job.path()).unwrap(),checkpoint);
            let saved=load(&job.path(),131072).unwrap();assert_eq!(saved.runs["fetch"].phase,"cancelled");assert!(saved.runs["fetch"].results.is_empty());
            assert_eq!(observer.used_for_test(Resource::PublicNetwork),0);
            assert!(observer.try_acquire(Resource::PublicNetwork).is_ok());retire_workflow(&job);
        }
    }
    #[test]
    fn phase2_library_revision_rejects_stale_resolved_request_and_failed_mutation_keeps_owner(){
        let (d,r)=v3_contract();let dir=tempfile::tempdir().unwrap();let queue=Mutex::new(Vec::new());let dispatch=|job|capture_job(&queue,job);
        let id="phase2-library-failure";
        let first=request_with_dispatch(id,dir.path(),&d,&r,&json!({"operation":"status"}),&dispatch).unwrap();
        request_with_dispatch(id,dir.path(),&d,&r,&json!({"operation":"action","action":"host.start","eventId":"phase2-failure","context":first["ui"]["context"]}),&dispatch).unwrap();
        let job=queue.lock().unwrap().remove(0);let before=std::fs::read(job.path()).unwrap();
        assert!(library_mutation(id,||Err::<(),String>("injected rename failure".into())).is_err());
        assert!(active(&job));assert_eq!(std::fs::read(job.path()).unwrap(),before);
        {
            let _resolved=resolution_scope();
            library_mutation("phase2-other-component",||Ok(())).unwrap();
            assert!(request_with_dispatch(id,dir.path(),&d,&r,&json!({"operation":"status"}),&dispatch).unwrap_err().contains("组件库已变化"));
        }
        assert!(active(&job),"unrelated mutation must not stop durable background work");
        execute_workflow(&job,&|_|Ok(json!({"record":{"title":"kept"}}))).unwrap();retire_workflow(&job);
        assert_eq!(load(&job.path(),131072).unwrap().runs["fetch"].phase,"completed");
    }
    #[test]
    fn v3_shared_contract_and_rejections() {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/composition-v3.json"
        ))
        .unwrap();
        let r = &suite["runtime"];
        assert!(
            crate::app_services::parse(&serde_json::to_vec(&suite["declaration"]).unwrap(), r)
                .is_ok()
        );
        for case in suite["rejections"].as_array().unwrap() {
            let mut bad = suite["declaration"].clone();
            let (parent, key) = case["pointer"].as_str().unwrap().rsplit_once('/').unwrap();
            bad.pointer_mut(parent).unwrap()[key] = case["value"].clone();
            assert!(
                crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(), r).is_err(),
                "{case}"
            );
        }
    }
    #[test]
    fn v3_real_get_admission_is_monotonic_bounded_and_does_not_retry() {
        let mut recent = BTreeMap::new();
        assert!(admit_workflow_get(&mut recent, "same".into(), 1));
        assert!(!admit_workflow_get(&mut recent, "same".into(), 15));
        assert!(admit_workflow_get(&mut recent, "same".into(), 16));
        for index in 0..31 {
            assert!(admit_workflow_get(
                &mut recent,
                format!("other-{index}"),
                16
            ));
        }
        assert!(!admit_workflow_get(&mut recent, "full".into(), 16));
        assert_eq!(recent.len(), 32);
        assert!(admit_workflow_get(&mut recent, "full".into(), 31));
        assert_eq!(recent.len(), 1);
    }
    #[test]
    fn v3_public_request_reaches_actual_bounded_worker_without_network() {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/workflow-local-v3.json"
        ))
        .unwrap();
        let r = suite["runtime"].clone();
        let d = crate::app_services::parse(&serde_json::to_vec(&suite["declaration"]).unwrap(), &r)
            .unwrap();
        assert_eq!(permissions(&d)["networkHosts"], json!([]));
        let dir = tempfile::tempdir().unwrap();
        let mut last = request(
            "workflow-thread-test",
            dir.path(),
            &d,
            &r,
            &json!({"operation":"status"}),
        )
        .unwrap();
        assert_eq!(last["variables"], suite["expected"]["initial"]);
        for (index, expected) in ["first_completion", "second_completion"].iter().enumerate() {
            let action = json!({"operation":"action","action":"host.start","eventId":format!("thread-start-{index}"),"context":last["ui"]["context"]});
            let queued = request("workflow-thread-test", dir.path(), &d, &r, &action).unwrap();
            assert_eq!(queued["variables"]["phase"], "queued");
            for _ in 0..200 {
                last = request(
                    "workflow-thread-test",
                    dir.path(),
                    &d,
                    &r,
                    &json!({"operation":"status"}),
                )
                .unwrap();
                if last["variables"]["phase"] == "completed" {
                    break;
                }
                std::thread::sleep(std::time::Duration::from_millis(5));
            }
            assert_eq!(last["variables"], suite["expected"][*expected]);
            assert_eq!(
                request("workflow-thread-test", dir.path(), &d, &r, &action).unwrap()["duplicate"],
                true
            );
        }
    }
    #[test]
    fn v3_real_request_path_checkpoints_failure_only_resume_and_deduplicates() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| {
            request_with_dispatch("workflow-test", dir.path(), &d, &r, input, &dispatch)
        };
        let status = json!({"operation":"status"});
        let first = req(&status).unwrap();
        assert_eq!(first["variables"]["phase"], "idle");
        let action = json!({"operation":"action","action":"host.start","eventId":"start-1","context":first["ui"]["context"],"data":{"selection":"public"}});
        let queued = req(&action).unwrap();
        assert_eq!(queued["variables"]["phase"], "queued");
        assert_eq!(queued["ui"]["actions"], json!(["host.cancel"]));
        assert!(req(&json!({"operation":"action","action":"host.increment","eventId":"conflict-1","context":queued["ui"]["context"]})).is_err());
        assert!(req(&json!({"operation":"action","action":"host.start","eventId":"same-component-2","context":queued["ui"]["context"]})).is_err());
        assert_eq!(req(&action).unwrap()["duplicate"], true);
        let mut reused = action.clone();
        reused["data"]["selection"] = "different".into();
        assert!(req(&reused).is_err());
        run_captured(
            &queue,
            &|_| Err("injected failure, no real provider".into()),
        );
        let failed = req(&status).unwrap();
        assert_eq!(
            failed["variables"],
            json!({"phase":"failed","points":1,"progress":25})
        );
        assert_eq!(
            failed["workflows"]["fetch"]["steps"][0]["phase"],
            "completed"
        );
        // Reopening/status does not enqueue or refetch anything.
        assert_eq!(req(&status).unwrap()["variables"], failed["variables"]);
        assert!(queue.lock().unwrap().is_empty());
        req(&json!({"operation":"action","action":"host.resume","eventId":"resume-1","context":failed["ui"]["context"]})).unwrap();
        let job = run_captured(&queue, &|_| {
            Ok(json!({"record":{"title":"fixture","value":7}}))
        });
        let completed = req(&status).unwrap();
        assert_eq!(
            completed["variables"],
            json!({"phase":"completed","points":2,"progress":100})
        );
        let saved = load(&job.path(), 131072).unwrap();
        let Model::Document { value } = &saved.models["records"] else {
            panic!()
        };
        assert_eq!(value["rows"], json!([{"title":"fixture","value":7}]));
        assert_eq!(saved.runs["fetch"].steps[0].attempts, 1);
        assert_eq!(saved.runs["fetch"].steps[1].attempts, 2);
        assert_eq!(saved.runs["fetch"].steps[2].attempts, 1);
        assert!(!completed.to_string().contains("fixture")); // result payload stays private
        assert!(req(&json!({"operation":"action","action":"host.resume","eventId":"resume-completed","context":completed["ui"]["context"]})).is_err());
        assert_eq!(req(&action).unwrap()["duplicate"], true);
        assert_eq!(req(&status).unwrap()["variables"]["points"], 2);
    }
    #[test]
    fn v3_cancel_during_get_revokes_late_results_and_preserves_completed_effects() {
        let (d, r) = v3_contract();
        for by_disable in [false, true] {
            let dir = tempfile::tempdir().unwrap();
            let queue = Mutex::new(Vec::new());
            let dispatch = |job| capture_job(&queue, job);
            let owner = if by_disable {
                "workflow-disable-test"
            } else {
                "workflow-cancel-test"
            };
            let req =
                |input: &Value| request_with_dispatch(owner, dir.path(), &d, &r, input, &dispatch);
            let first = req(&json!({"operation":"status"})).unwrap();
            req(&json!({"operation":"action","action":"host.start","eventId":"cancel-start","context":first["ui"]["context"]})).unwrap();
            run_captured(&queue, &|_| {
                if by_disable {
                    disable(owner);
                } else {
                    let current = req(&json!({"operation":"status"})).unwrap();
                    req(&json!({"operation":"action","action":"host.cancel","eventId":"cancel-inflight","context":current["ui"]["context"]})).unwrap();
                }
                Ok(json!({"record":{"title":"late"}}))
            });
            let current = req(&json!({"operation":"status"})).unwrap();
            assert_eq!(
                current["variables"],
                json!({"phase":"cancelled","points":1,"progress":25})
            );
            let saved = load(&dir.path().join("composition-v3-state.json"), 131072).unwrap();
            assert!(saved.runs["fetch"].results.is_empty());
            let Model::Document { value } = &saved.models["records"] else {
                panic!()
            };
            assert_eq!(value["rows"], json!([]));
        }
    }
    #[test]
    fn v3_resource_owner_outlives_device_exit_but_cancelled_run_cannot_reenter() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let request = |input: &Value| request_with_dispatch("resource-workflow", dir.path(), &d, &r, input, &dispatch);
        let device = ResourceOwner::fresh();
        {
            let _device = enter_owner(&device);
            let first = request(&json!({"operation":"status"})).unwrap();
            request(&json!({"operation":"action","action":"host.start","eventId":"resource-start","context":first["ui"]["context"]})).unwrap();
        }
        device.cancel();
        let first_job = queue.lock().unwrap()[0].clone();
        assert!(first_job.resources.is_current(), "hide is not durable workflow cancellation");
        let completed = {
            let _stale_device = enter_owner(&device);
            run_captured(&queue, &|_| Ok(json!({"record":{"title":"local fixture"}})))
        };
        assert!(!completed.resources.is_current(), "completed physical run retires its owner");
        let status = request(&json!({"operation":"status"})).unwrap();
        assert_eq!(status["variables"]["phase"], "completed");
        request(&json!({"operation":"action","action":"host.start","eventId":"resource-new","context":status["ui"]["context"]})).unwrap();
        let next = queue.lock().unwrap()[0].clone();
        first_job.resources.cancel();
        assert!(next.resources.is_current());
        let status = request(&json!({"operation":"status"})).unwrap();
        request(&json!({"operation":"action","action":"host.cancel","eventId":"resource-cancel","context":status["ui"]["context"]})).unwrap();
        assert!(!next.resources.is_current());
        let before = std::fs::read(next.path()).unwrap();
        run_captured(&queue, &|_| panic!("cancelled queued job must not invoke even an injected GET"));
        assert_eq!(std::fs::read(next.path()).unwrap(), before);
    }
    #[test]
    fn v3_resource_budget_is_shared_with_polling_until_cancelled_physical_get_finishes() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let first = request_with_dispatch("resource-shared", dir.path(), &d, &r, &json!({"operation":"status"}), &dispatch).unwrap();
        request_with_dispatch("resource-shared", dir.path(), &d, &r,
            &json!({"operation":"action","action":"host.start","eventId":"shared-start","context":first["ui"]["context"]}), &dispatch).unwrap();
        let poll_owner = ResourceOwner::isolated_for_test([2, 4]);
        let third = poll_owner.sibling_for_test();
        let mut job = queue.lock().unwrap().remove(0);
        job.resources = poll_owner.sibling_for_test();
        register_workflow(&job);
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        let (release_tx, release_rx) = std::sync::mpsc::channel();
        let poll = poll_owner.clone();
        // The exact boundary used by periodic JSON workers, with injected IO.
        let worker = std::thread::spawn(move || with_public_network(&poll, || {
            ready_tx.send(()).unwrap();
            release_rx.recv_timeout(std::time::Duration::from_secs(3)).unwrap();
            Ok(json!({"late":true}))
        }));
        ready_rx.recv_timeout(std::time::Duration::from_secs(3)).unwrap();
        execute_workflow(&job, &|_| {
            assert_eq!(poll_owner.used_for_test(Resource::PublicNetwork), 2);
            poll_owner.cancel();
            assert_eq!(poll_owner.used_for_test(Resource::PublicNetwork), 2);
            assert!(third.try_acquire(Resource::PublicNetwork).is_err());
            assert!(job.resources.is_current());
            Ok(json!({"record":{"title":"synthetic"}}))
        }).unwrap();
        assert_eq!(load(&job.path(), 131072).unwrap().runs["fetch"].phase, "completed");
        retire_workflow(&job);
        assert_eq!(poll_owner.used_for_test(Resource::PublicNetwork), 1);
        release_tx.send(()).unwrap();
        assert!(worker.join().unwrap().is_err(), "cancelled GET result is not published");
        assert_eq!(poll_owner.used_for_test(Resource::PublicNetwork), 0);
        assert!(third.try_acquire(Resource::PublicNetwork).is_ok());
    }
    #[test]
    fn v3_resource_late_completion_cannot_retire_a_new_owner_with_the_same_run_key() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let first = request_with_dispatch("resource-reused-key", dir.path(), &d, &r, &json!({"operation":"status"}), &dispatch).unwrap();
        request_with_dispatch("resource-reused-key", dir.path(), &d, &r,
            &json!({"operation":"action","action":"host.start","eventId":"same-key","context":first["ui"]["context"]}), &dispatch).unwrap();
        let old = queue.lock().unwrap().remove(0);
        let mut replacement = old.clone();
        replacement.resources = ResourceOwner::fresh();
        assert_eq!(old.key(), replacement.key());
        execute_workflow(&old, &|_| {
            // Model a later logical run reusing the bounded receipt's ID/key:
            // fresh checkpoints, while earlier committed local data survives.
            let _transaction = record_transaction().lock().unwrap();
            let mut state = load(&old.path(), 131072).unwrap();
            let run = state.runs.get_mut("fetch").unwrap();
            run.phase = "queued".into();
            for step in &mut run.steps { step.phase = "pending".into(); step.attempts = 0; }
            save_state(&old.path(), &state, 131072).unwrap();
            register_workflow(&replacement);
            assert!(!old.resources.is_current());
            Ok(json!({"record":{"title":"stale"}}))
        }).unwrap();
        retire_workflow(&old);
        assert!(active(&replacement));
        assert!(replacement.resources.is_current());
        assert!(load(&old.path(), 131072).unwrap().runs["fetch"].results.is_empty());
        execute_workflow(&replacement, &|_| Ok(json!({"record":{"title":"fresh"}}))).unwrap();
        let saved = load(&replacement.path(), 131072).unwrap();
        assert_eq!(saved.runs["fetch"].phase, "completed");
        assert!(matches!(saved.models["count"], Model::Counter { value: 3 }));
        let Model::Document { value } = &saved.models["records"] else { panic!() };
        assert_eq!(value["rows"][0]["title"], "fresh");
        retire_workflow(&replacement);
    }
    #[test]
    fn v3_restart_at_get_checkpoint_requires_explicit_resume() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| {
            request_with_dispatch(
                "workflow-restart-test",
                dir.path(),
                &d,
                &r,
                input,
                &dispatch,
            )
        };
        let first = req(&json!({"operation":"status"})).unwrap();
        req(&json!({"operation":"action","action":"host.start","eventId":"restart-start","context":first["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| {
            active_workflows()
                .lock()
                .unwrap()
                .retain(|_, job| job.dir != dir.path());
            Ok(json!({"record":{"title":"lost-result"}}))
        });
        // Simulated process loss after the persisted running marker: no result
        // checkpoint exists, but the earlier local increment is durable.
        let disk = load(&dir.path().join("composition-v3-state.json"), 131072).unwrap();
        assert_eq!(disk.runs["fetch"].phase, "running");
        let reopened = req(&json!({"operation":"status"})).unwrap();
        assert_eq!(reopened["variables"]["phase"], "failed");
        assert_eq!(
            reopened["workflows"]["fetch"]["state"]["error"],
            "interrupted"
        );
        assert_eq!(reopened["variables"]["points"], 1);
        assert!(queue.lock().unwrap().is_empty());
        req(&json!({"operation":"action","action":"host.resume","eventId":"restart-resume","context":reopened["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| Ok(json!({"record":{"title":"retried"}})));
        assert_eq!(
            req(&json!({"operation":"status"})).unwrap()["variables"]["points"],
            2
        );
    }
    #[test]
    fn v3_restart_after_last_checkpoint_reconciles_without_replaying_any_step() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| request_with_dispatch("workflow-terminal-restart", dir.path(), &d, &r, input, &dispatch);
        let first = req(&json!({"operation":"status"})).unwrap();
        let start = json!({"operation":"action","action":"host.start","eventId":"terminal-start","context":first["ui"]["context"]});
        req(&start).unwrap();
        let job = run_captured(&queue, &|_| Ok(json!({"record":{"title":"preserved"}})));
        let completed = load(&job.path(), 131072).unwrap();
        // Older workers saved the last completed checkpoint before a separate
        // terminal-phase save. Reproduce precisely that valid on-disk snapshot,
        // including the old recovery path which had labelled it interrupted.
        for (phase, error) in [("running", ""), ("failed", "interrupted")] {
            let mut interrupted = completed.clone();
            let run = interrupted.runs.get_mut("fetch").unwrap();
            run.phase = phase.into();
            run.error = error.into();
            assert!(run.steps.iter().all(done));
            save_state(&job.path(), &interrupted, 131072).unwrap();
            let reopened = req(&json!({"operation":"status"})).unwrap();
            assert_eq!(reopened["variables"], json!({"phase":"completed","progress":100,"points":2}));
            assert_eq!(reopened["workflows"]["fetch"]["state"]["error"], "");
            assert!(queue.lock().unwrap().is_empty());
            let saved = load(&job.path(), 131072).unwrap();
            assert_eq!(serde_json::to_value(&saved.models).unwrap(), serde_json::to_value(&completed.models).unwrap());
            assert_eq!(saved.runs["fetch"].results, completed.runs["fetch"].results);
            assert_eq!(saved.runs["fetch"].attempt, 1);
            assert_eq!(req(&start).unwrap()["duplicate"], true);
        }
    }
    #[test]
    fn v3_terminal_checkpoint_covers_local_skipped_and_get_without_promoting_unknown() {
        for kind in ["local", "skipped", "get"] {
            let (mut d, r) = v3_contract();
            d.content[0]["asyncWorkflows"]["fetch"] = match kind {
                "get" => json!([d.content[0]["asyncWorkflows"]["fetch"][1].clone()]),
                "skipped" => json!([{"target":"count:increment","when":false}]),
                _ => json!([{"target":"count:increment"}]),
            };
            validate(&d, &r).unwrap();
            let dir = tempfile::tempdir().unwrap();
            let queue = Mutex::new(Vec::new());
            let dispatch = |job| capture_job(&queue, job);
            let req = |input: &Value| request_with_dispatch("workflow-terminal-kinds", dir.path(), &d, &r, input, &dispatch);
            let first = req(&json!({"operation":"status"})).unwrap();
            req(&json!({"operation":"action","action":"host.start","eventId":"terminal-kinds","context":first["ui"]["context"]})).unwrap();
            let job = queue.lock().unwrap().remove(0);
            let mut checkpoint = load(&job.path(), 131072).unwrap().runs["fetch"].clone();
            checkpoint.phase = "running".into();
            checkpoint.steps[0].attempts = 1;
            assert!(complete_checkpoint(&mut checkpoint, 0, kind != "skipped"));
            assert_eq!(checkpoint.phase, "completed", "last checkpoint is terminal before save");
            execute_workflow(&job, &|_| Ok(json!({"record":{"title":"synthetic"}}))).unwrap();
            retire_workflow(&job);
            let mut saved = load(&job.path(), 131072).unwrap();
            assert_eq!(saved.runs["fetch"].phase, "completed");
            assert_eq!(saved.runs["fetch"].steps[0].phase, if kind == "skipped" { "skipped" } else { "completed" });
            assert!(matches!(saved.models["count"], Model::Counter { value } if value == i32::from(kind == "local")));
            // A failure code is not proof of success, even with a suspicious
            // terminal-looking checkpoint. Likewise a changed definition must
            // not be promoted by the legacy-recovery exception.
            saved.runs.get_mut("fetch").unwrap().phase = "failed".into();
            saved.runs.get_mut("fetch").unwrap().error = "get_step_failed".into();
            save_state(&job.path(), &saved, 131072).unwrap();
            assert_eq!(req(&json!({"operation":"status"})).unwrap()["variables"]["phase"], "failed");
            saved.runs.get_mut("fetch").unwrap().phase = "running".into();
            saved.runs.get_mut("fetch").unwrap().error.clear();
            saved.runs.get_mut("fetch").unwrap().specification = "0".repeat(64);
            save_state(&job.path(), &saved, 131072).unwrap();
            let changed = req(&json!({"operation":"status"})).unwrap();
            assert_eq!(changed["workflows"]["fetch"]["state"]["error"], "definition_changed");
            assert!(queue.lock().unwrap().is_empty());
        }
    }
    #[test]
    fn v3_failed_admission_save_revokes_the_queued_worker_and_preserves_old_file() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| request_with_dispatch("workflow-save-full", dir.path(), &d, &r, input, &dispatch);
        req(&json!({"operation":"status"})).unwrap();
        let path = dir.path().join("composition-v3-state.json");
        let mut state = load(&path, 131072).unwrap();
        // Fill private historical models within their own bounded document
        // budget. Never evict them to make room for a new async request.
        for index in 0..17 {
            let rows = vec![Value::String("x".repeat(500)); 15];
            state.models.insert(format!("history-{index}"), Model::Document { value: json!({"rows":rows}) });
        }
        save_state(&path, &state, 131072).unwrap();
        let current = req(&json!({"operation":"status"})).unwrap();
        let before = std::fs::read(&path).unwrap();
        let result = req(&json!({"operation":"action","action":"host.start","eventId":"save-full-start","context":current["ui"]["context"],"data":{"input":"x".repeat(500),"other":"y".repeat(500),"third":"z".repeat(500),"fourth":"w".repeat(500)}}));
        assert!(result.is_err(), "new run must exceed the total quota");
        assert_eq!(std::fs::read(&path).unwrap(), before);
        let job = queue.lock().unwrap().remove(0);
        assert!(!active(&job));
        assert!(!job.resources.is_current());
        execute_workflow(&job, &|_| panic!("uncommitted run cannot make a request")).unwrap();
        retire_workflow(&job);
        assert_eq!(std::fs::read(&path).unwrap(), before);
    }
    #[test]
    fn v3_completed_get_is_not_replayed_when_later_local_step_fails() {
        let (mut d, r) = v3_contract();
        d.content[0]["sources"]["records"]["initial"]["rows"] = json!(vec![0; 128]);
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| {
            request_with_dispatch(
                "workflow-local-fail-test",
                dir.path(),
                &d,
                &r,
                input,
                &dispatch,
            )
        };
        let initial = req(&json!({"operation":"status"})).unwrap();
        req(&json!({"operation":"action","action":"host.start","eventId":"full-start","context":initial["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| Ok(json!({"record":"fixture"})));
        let failed = req(&json!({"operation":"status"})).unwrap();
        assert_eq!(
            failed["variables"],
            json!({"phase":"failed","progress":50,"points":1})
        );
        req(&json!({"operation":"action","action":"host.resume","eventId":"full-resume","context":failed["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| panic!("completed GET must never be replayed"));
        assert_eq!(
            req(&json!({"operation":"status"})).unwrap()["variables"],
            failed["variables"]
        );
    }
    #[test]
    fn v3_incompatible_later_source_does_not_block_independent_earlier_steps() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let req = |input: &Value| request_with_dispatch("workflow-source-isolation", dir.path(), &d, &r, input, &dispatch);
        req(&json!({"operation":"status"})).unwrap();
        let path = dir.path().join("composition-v3-state.json");
        let mut state = load(&path, 131072).unwrap();
        // A saved alias from an older package is not the current document type.
        // It is needed by step 3, not by the earlier independent counter/GET.
        state.models.insert("records".into(), Model::Counter { value: 77 });
        save_state(&path, &state, 131072).unwrap();
        let first = req(&json!({"operation":"status"})).unwrap();
        req(&json!({"operation":"action","action":"host.start","eventId":"source-start","context":first["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| Ok(json!({"record":{"title":"preserved-result"}})));
        let failed = req(&json!({"operation":"status"})).unwrap();
        assert_eq!(failed["variables"], json!({"phase":"failed","progress":50,"points":1}));
        assert_eq!(failed["workflows"]["fetch"]["steps"][2]["phase"], "failed");
        let mut state = load(&path, 131072).unwrap();
        assert!(matches!(state.models["records"], Model::Counter { value: 77 }), "incompatible saved data is not reset");
        // Simulate an explicit external migration in this isolated fixture;
        // the engine itself must never fabricate or reset that user record.
        state.models.insert("records".into(), Model::Document { value: json!({"rows":[]}) });
        save_state(&path, &state, 131072).unwrap();
        let current = req(&json!({"operation":"status"})).unwrap();
        req(&json!({"operation":"action","action":"host.resume","eventId":"source-resume","context":current["ui"]["context"]})).unwrap();
        run_captured(&queue, &|_| panic!("completed GET is preserved, not replayed"));
        let saved = load(&path, 131072).unwrap();
        assert_eq!(saved.runs["fetch"].phase, "completed");
        assert_eq!(saved.runs["fetch"].steps[0].attempts, 1);
        assert_eq!(saved.runs["fetch"].steps[1].attempts, 1);
        assert_eq!(saved.runs["fetch"].steps[2].attempts, 2);
        assert!(matches!(saved.models["count"], Model::Counter { value: 2 }));
        let Model::Document { value } = &saved.models["records"] else { panic!() };
        assert_eq!(value["rows"], json!([{"title":"preserved-result"}]));
    }
    #[test]
    fn v3_unavailable_current_target_or_expression_still_fails_without_effects() {
        for broken in ["target", "expression", "document_budget"] {
            let (mut d, r) = v3_contract();
            if broken == "expression" {
                d.content[0]["asyncWorkflows"]["fetch"][0]["when"] = json!({"ref":"/sources/records/available"});
            } else if broken == "document_budget" {
                d.content[0]["asyncWorkflows"]["fetch"][0] = json!({"target":"records:set","pointer":"/old","value":"replacement"});
            }
            validate(&d, &r).unwrap();
            let dir = tempfile::tempdir().unwrap();
            let queue = Mutex::new(Vec::new());
            let dispatch = |job| capture_job(&queue, job);
            let req = |input: &Value| request_with_dispatch("workflow-target-isolation", dir.path(), &d, &r, input, &dispatch);
            req(&json!({"operation":"status"})).unwrap();
            let path = dir.path().join("composition-v3-state.json");
            let mut state = load(&path, 131072).unwrap();
            let (alias, invalid) = match broken {
                "target" => ("count", Model::Document { value: json!({"old":"retained"}) }),
                "expression" => ("records", Model::Counter { value: 77 }),
                _ => ("records", Model::Document { value: json!({"old":"x".repeat(513)}) }),
            };
            state.models.insert(alias.into(), invalid);
            save_state(&path, &state, 131072).unwrap();
            let first = req(&json!({"operation":"status"})).unwrap();
            req(&json!({"operation":"action","action":"host.start","eventId":"unavailable-start","context":first["ui"]["context"]})).unwrap();
            run_captured(&queue, &|_| panic!("invalid local first step cannot reach GET"));
            let saved = load(&path, 131072).unwrap();
            assert_eq!(saved.runs["fetch"].phase, "failed");
            assert_eq!(saved.runs["fetch"].error, "local_step_failed");
            assert_eq!(saved.runs["fetch"].steps[0].phase, "failed");
            assert!(saved.runs["fetch"].results.is_empty());
            assert_eq!(serde_json::to_value(saved.models).unwrap(), serde_json::to_value(state.models).unwrap());
        }
    }
    #[test]
    fn v3_queue_failure_migration_and_corruption_preserve_previous_data() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let legacy = br#"{"version":1,"models":{"count":{"kind":"counter","value":9}}}"#;
        std::fs::write(dir.path().join("composition-v2-state.json"), legacy).unwrap();
        let req = |input: &Value| {
            request_with_dispatch("workflow-queue-test", dir.path(), &d, &r, input, &|_| false)
        };
        let initial = req(&json!({"operation":"status"})).unwrap();
        assert_eq!(initial["variables"]["points"], 9);
        let failed = req(&json!({"operation":"action","action":"host.start","eventId":"queue-start","context":initial["ui"]["context"]})).unwrap();
        assert_eq!(
            failed["variables"],
            json!({"phase":"failed","progress":0,"points":9})
        );
        assert_eq!(failed["workflows"]["fetch"]["state"]["error"], "queue_full");
        assert_eq!(
            std::fs::read(dir.path().join("composition-v2-state.json")).unwrap(),
            legacy
        );
        let path = dir.path().join("composition-v3-state.json");
        std::fs::write(&path, b"corrupted").unwrap();
        assert!(req(&json!({"operation":"status"})).is_err());
        assert_eq!(std::fs::read(path).unwrap(), b"corrupted");
    }
    #[test]
    fn v3_definition_changes_and_forged_checkpoints_cannot_resume_or_execute() {
        let (d, r) = v3_contract();
        let dir = tempfile::tempdir().unwrap();
        let queue = Mutex::new(Vec::new());
        let dispatch = |job| capture_job(&queue, job);
        let first = request_with_dispatch(
            "workflow-version-test",
            dir.path(),
            &d,
            &r,
            &json!({"operation":"status"}),
            &dispatch,
        )
        .unwrap();
        request_with_dispatch("workflow-version-test", dir.path(), &d, &r,
            &json!({"operation":"action","action":"host.start","eventId":"version-start","context":first["ui"]["context"]}), &dispatch).unwrap();
        let mut changed = d.clone();
        changed.content[0]["asyncWorkflows"]["fetch"][0]["target"] = "count:decrement".into();
        let stopped = request_with_dispatch(
            "workflow-version-test",
            dir.path(),
            &changed,
            &r,
            &json!({"operation":"status"}),
            &dispatch,
        )
        .unwrap();
        assert_eq!(stopped["variables"]["phase"], "failed");
        assert_eq!(
            stopped["workflows"]["fetch"]["state"]["error"],
            "definition_changed"
        );
        assert!(!stopped["ui"]["actions"]
            .as_array()
            .unwrap()
            .contains(&json!("host.resume")));
        run_captured(&queue, &|_| {
            panic!("old-definition job must not issue a GET")
        });
        let path = dir.path().join("composition-v3-state.json");
        let mut state = load(&path, 131072).unwrap();
        state.runs.get_mut("fetch").unwrap().specification = specification(&changed, &r);
        state.runs.get_mut("fetch").unwrap().steps.pop();
        save_state(&path, &state, 131072).unwrap();
        let bytes = std::fs::read(&path).unwrap();
        assert!(request_with_dispatch(
            "workflow-version-test",
            dir.path(),
            &changed,
            &r,
            &json!({"operation":"status"}),
            &dispatch
        )
        .is_err());
        assert_eq!(std::fs::read(path).unwrap(), bytes);
    }
    fn v2_contract() -> (Declaration, Value) {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/composition-v2.json"
        ))
        .unwrap();
        let runtime = suite["runtime"].clone();
        let d = crate::app_services::parse(
            &serde_json::to_vec(&suite["declaration"]).unwrap(),
            &runtime,
        )
        .unwrap();
        (d, runtime)
    }
    #[test]
    fn mixed_writer_composition_binds_only_host_fields_and_preserves_native_state(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/variable-owners-v1.json")).unwrap();
        let mut r=suite["runtime"].clone();
        r["transitions"].as_array_mut().unwrap().push(json!({"from":"*","on":"host.increment"}));
        let declaration=json!({"schemaVersion":1,"engine":"pc-app-composition-v2","service":"composition",
            "actions":{"host.increment":"source:increment"},"content":[{"sources":{"source":{"kind":"counter","initial":2,"min":1,"max":10,"step":1}},
            "bindings":{"step":{"source":"source","pointer":"/value","fallback":2}}}]});
        let d=crate::app_services::parse(&serde_json::to_vec(&declaration).unwrap(),&r).unwrap();
        let dir=tempfile::tempdir().unwrap();
        let before=r.clone();
        let initial=request("owned-case",dir.path(),&d,&r,&json!({"operation":"status"})).unwrap();
        assert_eq!(initial["variables"],json!({"step":2}));
        let action=json!({"operation":"action","action":"host.increment","eventId":"owned-1","context":initial["ui"]["context"]});
        let result=request("owned-case",dir.path(),&d,&r,&action).unwrap();
        assert_eq!(result["variables"],json!({"step":3}));assert_eq!(r,before);
        let duplicate=request("owned-case",dir.path(),&d,&r,&action).unwrap();
        assert_eq!(duplicate["variables"],json!({"step":3}));
        assert!(request("owned-case",dir.path(),&d,&r,&json!({"operation":"action","action":"add","eventId":"owned-2","context":result["ui"]["context"]})).is_err());
        let mut wrong=declaration.clone();wrong["content"][0]["bindings"]["count"]=json!({"source":"source","pointer":"/value","fallback":0});
        assert!(crate::app_services::parse(&serde_json::to_vec(&wrong).unwrap(),&r).is_err());
        let mut missing=declaration.clone();missing["content"][0]["bindings"]=json!({});
        assert!(crate::app_services::parse(&serde_json::to_vec(&missing).unwrap(),&r).is_err());
        let mut old=declaration;old["engine"]=json!(ENGINE);
        assert!(crate::app_services::parse(&serde_json::to_vec(&old).unwrap(),&r).is_err());
    }
    #[test]
    fn v2_contract_rejects_cycles_bad_steps_and_missing_context() {
        let suite: Value = serde_json::from_str(include_str!(
            "../../skills/petui/contracts/composition-v2.json"
        ))
        .unwrap();
        let r = &suite["runtime"];
        let good = &suite["declaration"];
        assert!(crate::app_services::parse(&serde_json::to_vec(good).unwrap(), r).is_ok());
        for c in suite["rejections"].as_array().unwrap() {
            let mut bad = good.clone();
            let (parent, key) = c["pointer"].as_str().unwrap().rsplit_once('/').unwrap();
            bad.pointer_mut(parent).unwrap()[key] = c["value"].clone();
            assert!(
                crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(), r).is_err(),
                "{c}"
            );
        }
        let mut bad = r.clone();
        bad["input"].as_object_mut().unwrap().remove("confirm");
        assert!(crate::app_services::parse(&serde_json::to_vec(good).unwrap(), &bad).is_err());
    }
    #[test]
    fn v2_local_workflow_is_atomic_deduplicated_and_survives_restart() {
        let (d, r) = v2_contract();
        let dir = tempfile::tempdir().unwrap();
        let status = json!({"operation":"status"});
        let initial = request("alpha", dir.path(), &d, &r, &status).unwrap();
        assert_eq!(initial["variables"]["total"], 0);
        let action = json!({"operation":"action","action":"host.save","eventId":"event-1","context":initial["ui"]["context"],"data":{"category":"food","cents":200}});
        let saved = request("alpha", dir.path(), &d, &r, &action).unwrap();
        assert_eq!(saved["variables"], json!({"total":200,"points":1}));
        assert_eq!(saved["workflowSteps"].as_array().unwrap().len(), 2);
        let again = request("alpha", dir.path(), &d, &r, &action).unwrap();
        assert_eq!(again["duplicate"], true);
        assert_eq!(again["variables"], saved["variables"]);
        let mut collision = action.clone();
        collision["data"]["cents"] = 300.into();
        assert!(request("alpha", dir.path(), &d, &r, &collision).is_err());
        let mut stale = action.clone();
        stale["eventId"] = "event-2".into();
        assert!(request("alpha", dir.path(), &d, &r, &stale).is_err());
        let mut next = stale.clone();
        next["context"] = saved["ui"]["context"].clone();
        next["data"]["category"] = "book".into();
        let second = request("alpha", dir.path(), &d, &r, &next).unwrap();
        assert_eq!(second["variables"], json!({"total":200,"points":2}));
        let reopened = request("alpha", dir.path(), &d, &r, &status).unwrap();
        assert_eq!(reopened["variables"], second["variables"]);
        let remove = json!({"operation":"action","action":"host.remove","eventId":"event-3","context":second["ui"]["context"],"confirm":true});
        assert_eq!(
            request("alpha", dir.path(), &d, &r, &remove).unwrap()["variables"]["total"],
            0
        );
        let other = request("beta", tempfile::tempdir().unwrap().path(), &d, &r, &status).unwrap();
        assert_eq!(other["variables"]["total"], 0);
    }
    #[test]
    fn v2_failed_later_step_rolls_back_and_keeps_original_v1_data() {
        let (d, r) = v2_contract();
        let dir = tempfile::tempdir().unwrap();
        let legacy = b"{\"version\":1,\"models\":{\"growth\":{\"kind\":\"counter\",\"value\":9}}}";
        std::fs::write(dir.path().join("composition-state.json"), legacy).unwrap();
        let initial = request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        assert_eq!(initial["variables"]["points"], 9);
        assert_eq!(
            std::fs::read(dir.path().join("composition-state.json")).unwrap(),
            legacy
        );
        let mut fail = d.clone();
        fail.content[0]["workflows"]["save"][1]["when"] =
            json!({"op":"gt","args":[{"op":"div","args":[1,0]},0]});
        validate(&fail, &r).unwrap();
        let status = request(
            "alpha",
            dir.path(),
            &fail,
            &r,
            &json!({"operation":"status"}),
        )
        .unwrap();
        let path = dir.path().join("composition-v2-state.json");
        let before = std::fs::read(&path).unwrap();
        let action = json!({"operation":"action","action":"host.save","eventId":"failed-1","context":status["ui"]["context"],"data":{"category":"food","cents":200}});
        assert!(request("alpha", dir.path(), &fail, &r, &action).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        std::fs::write(&path, b"corrupted").unwrap();
        assert!(request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), b"corrupted");
    }
    #[test]
    fn v2_replay_outside_receipt_window_and_changed_actions_are_rejected() {
        let (mut d, mut r) = v2_contract();
        d.content[0]["workflows"] = json!({});
        d.actions = BTreeMap::from([("host.increment".into(), "growth:increment".into())]);
        r["transitions"] = json!([{"from":"*","on":"host.increment"}]);
        let dir = tempfile::tempdir().unwrap();
        let mut current =
            request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        let initial = current["ui"]["context"].clone();
        for i in 0..130 {
            current=request("alpha",dir.path(),&d,&r,&json!({"operation":"action","action":"host.increment","eventId":format!("test-{i}"),"context":current["ui"]["context"]})).unwrap();
        }
        assert_eq!(current["variables"]["points"], 130);
        assert!(request("alpha",dir.path(),&d,&r,&json!({"operation":"action","action":"host.increment","eventId":"test-0","context":initial})).is_err());
        d.actions
            .insert("host.increment".into(), "growth:decrement".into());
        assert!(request("alpha",dir.path(),&d,&r,&json!({"operation":"action","action":"host.increment","eventId":"changed-route","context":current["ui"]["context"]})).is_err());
    }
    #[test]
    fn v2_explicit_null_is_data_not_an_omitted_step() {
        let (mut d, r) = v2_contract();
        d.content[0]["workflows"]["save"][0]["value"] = Value::Null;
        validate(&d, &r).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let first = request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        let next=request("alpha",dir.path(),&d,&r,&json!({"operation":"action","action":"host.save","eventId":"null-value","context":first["ui"]["context"]})).unwrap();
        assert_eq!(next["variables"], json!({"total":0,"points":0}));
        let saved = load(&dir.path().join("composition-v2-state.json"), 65536).unwrap();
        let Model::Document { value } = &saved.models["records"] else {
            panic!()
        };
        assert_eq!(value["rows"], json!([null]));
    }
    #[test]
    fn oversized_document_store_is_rejected_without_overwrite() {
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("composition-v2-state.json");
        let raw = vec![b' '; 65537];
        std::fs::write(&path, &raw).unwrap();
        assert!(load(&path, 65536).is_err());
        assert_eq!(std::fs::metadata(&path).unwrap().len(), 65537);
    }
    #[test]
    fn replacing_saved_collections_cannot_bypass_confirmation_with_a_new_initial_shape() {
        let (mut d, mut r) = v2_contract();
        d.content[0]["sources"]["records"]["initial"] = json!({"rows":0});
        d.content[0]["workflows"]["save"] =
            json!([{"target":"records:set","pointer":"/rows","value":{"literal":[]}}]);
        validate(&d, &r).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let path = dir.path().join("composition-v2-state.json");
        std::fs::write(&path,serde_json::to_vec(&json!({"version":1,"models":{"records":{"kind":"document","value":{"rows":[{"category":"food","cents":200}]}}}})).unwrap()).unwrap();
        let status = request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        let before = std::fs::read(&path).unwrap();
        let mut action = json!({"operation":"action","action":"host.save","eventId":"bulk-1","context":status["ui"]["context"],"confirm":true});
        assert!(request("alpha", dir.path(), &d, &r, &action).is_err());
        assert_eq!(std::fs::read(&path).unwrap(), before);
        r["input"]["confirm"]["host.save"] = "替换全部记录？".into();
        let next = request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        action["context"] = next["ui"]["context"].clone();
        action["confirm"] = false.into();
        assert!(request("alpha", dir.path(), &d, &r, &action).is_err());
        action["confirm"] = true.into();
        assert_eq!(
            request("alpha", dir.path(), &d, &r, &action).unwrap()["variables"]["total"],
            0
        );
    }
    fn contract() -> Value {
        serde_json::from_str(include_str!(
            "../../skills/petui/contracts/composition-v1.json"
        ))
        .unwrap()
    }
    fn declaration(value: &Value) -> Declaration {
        serde_json::from_value(value.clone()).unwrap()
    }
    #[test]
    fn shared_contract_and_rejections() {
        let suite = contract();
        let r = &suite["runtime"];
        let d = &suite["declaration"];
        assert!(crate::app_services::parse(&serde_json::to_vec(d).unwrap(), r).is_ok());
        for case in suite["rejections"].as_array().unwrap() {
            let mut bad = d.clone();
            let pointer = case["pointer"].as_str().unwrap();
            // An unknown field may not exist yet; add only its final object key.
            let (parent, key) = pointer.rsplit_once('/').unwrap();
            bad.pointer_mut(parent)
                .unwrap()
                .as_object_mut()
                .unwrap()
                .insert(key.into(), case["value"].clone());
            assert!(
                crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(), r).is_err(),
                "{pointer}"
            );
        }
        let mut bad = r.clone();
        bad["transitions"][1] = bad["transitions"][0].clone();
        assert!(validate(&declaration(d), &bad).is_err());
    }
    #[test]
    fn typed_projection_persistence_confirmation_and_isolation() {
        let suite = contract();
        let mut r = suite["runtime"].clone();
        let d = declaration(&suite["declaration"]);
        let dir = tempfile::tempdir().unwrap();
        let other = tempfile::tempdir().unwrap();
        let status = json!({"operation":"status"});
        let initial = request("alpha", dir.path(), &d, &r, &status).unwrap();
        assert_eq!(initial["variables"]["caption"], "Focus");
        r["input"] = json!({"engine":"p4-input-v1","confirm":{"host.increment":"Increment?"}});
        let mut action = json!({"operation":"action","action":"host.increment"});
        assert!(request("alpha", dir.path(), &d, &r, &action).is_err());
        action["confirm"] = true.into();
        action["context"] = initial["ui"]["context"].clone();
        let result = request("alpha", dir.path(), &d, &r, &action).unwrap();
        assert_eq!(result["variables"]["count"], 1);
        assert!(request("alpha", dir.path(), &d, &r, &action).is_err()); // stale confirm cannot repeat action
        assert_eq!(
            request("alpha", dir.path(), &d, &r, &status).unwrap()["variables"]["count"],
            1
        );
        assert_eq!(
            request("beta", other.path(), &d, &r, &status).unwrap()["variables"]["count"],
            0
        );
        let mut changed = d.clone();
        changed.content[0]["bindings"]["caption"]["pointer"] = "/missing".into();
        let value = request("alpha", dir.path(), &changed, &r, &status).unwrap();
        assert_eq!(value["variables"]["caption"], "Unavailable");
        assert_eq!(value["bindingErrors"], json!(["caption"]));
        let file = dir.path().join("composition-state.json");
        std::fs::write(&file, b"corrupted").unwrap();
        assert!(request("alpha", dir.path(), &d, &r, &status).is_err());
        assert_eq!(std::fs::read(file).unwrap(), b"corrupted");
    }
    #[test]
    fn counters_and_timers_are_independent_and_bounded() {
        let mut s = Saved {
            version: 1,
            ..Default::default()
        };
        let c = Source::Counter {
            initial: 0,
            min: 0,
            max: 2,
            step: 1,
        };
        let t = Source::Timer { seconds: 3 };
        for _ in 0..5 {
            source_value("count", &c, &mut s, 1000, Some("increment")).unwrap();
        }
        assert_eq!(
            source_value("count", &c, &mut s, 1000, None).unwrap()["value"],
            2
        );
        source_value("focus", &t, &mut s, 1000, Some("start")).unwrap();
        assert_eq!(
            source_value("focus", &t, &mut s, 2000, Some("pause")).unwrap()["seconds"],
            2
        );
        assert_eq!(
            source_value("focus", &t, &mut s, 999999, None).unwrap()["seconds"],
            2
        );
        source_value("focus", &t, &mut s, 1000000, Some("start")).unwrap();
        assert_eq!(
            source_value("focus", &t, &mut s, 1003000, None).unwrap()["phase"],
            "finished"
        );
        assert_eq!(
            source_value("count", &c, &mut s, 1003000, None).unwrap()["value"],
            2
        );
    }
    #[test]
    fn bounded_fetch_queue_deduplicates_preserves_success_and_rejects_stale_results() {
        let (tx, rx) = mpsc::sync_channel(8);
        let pool = Network {
            cache: Default::default(),
            tx,
        };
        let source = Source::JsonGet {
            endpoint: "https://example.com/data".into(),
            hosts: vec!["example.com".into()],
            refresh_sec: 60,
        };
        assert_eq!(
            cached(&pool, "alpha", "public", &source, 100, false).1,
            "preparing"
        );
        let first = rx.try_recv().unwrap();
        cached(&pool, "alpha", "public", &source, 101, true);
        assert!(rx.try_recv().is_err());
        finish(
            &mut pool.cache.lock().unwrap(),
            &first,
            Ok(json!({"value":42})),
        );
        assert_eq!(
            cached(&pool, "alpha", "public", &source, 110, false),
            (json!({"value":42}), "ready")
        );
        cached(&pool, "alpha", "public", &source, 115, true);
        let failed = rx.try_recv().unwrap();
        finish(
            &mut pool.cache.lock().unwrap(),
            &failed,
            Err("unavailable".into()),
        );
        assert_eq!(
            cached(&pool, "alpha", "public", &source, 116, false),
            (json!({"value":42}), "error")
        );
        pool.cache.lock().unwrap().clear();
        cached(&pool, "alpha", "public", &source, 117, false);
        let new = rx.try_recv().unwrap();
        finish(
            &mut pool.cache.lock().unwrap(),
            &first,
            Ok(json!({"value":999})),
        );
        assert_eq!(
            cached(&pool, "alpha", "public", &source, 118, false),
            (Value::Null, "preparing")
        );
        finish(
            &mut pool.cache.lock().unwrap(),
            &new,
            Ok(json!({"value":3})),
        );
        assert_eq!(
            cached(&pool, "alpha", "public", &source, 119, false).0,
            json!({"value":3})
        );
        for index in 0..40 {
            cached(
                &pool,
                &format!("owner-{index}"),
                "public",
                &source,
                120,
                false,
            );
        }
        assert!(pool.cache.lock().unwrap().len() <= 32);
        assert_eq!(rx.try_iter().count(), 8); // bounded queue; no network threads in this test
    }
    #[test]
    fn incompatible_source_preserves_record_without_blocking_other_sources() {
        let suite = contract();
        let mut r = suite["runtime"].clone();
        let mut d = declaration(&suite["declaration"]);
        d.actions
            .insert("host.reset".into(), "counter:reset".into());
        r["transitions"]
            .as_array_mut()
            .unwrap()
            .push(json!({"from":"*","on":"host.reset"}));
        let dir = tempfile::tempdir().unwrap();
        request(
            "alpha",
            dir.path(),
            &d,
            &r,
            &json!({"operation":"action","action":"host.increment"}),
        )
        .unwrap();
        d.content[0]["sources"]["counter"]["max"] = 0.into();
        validate(&d, &r).unwrap();
        let status = request("alpha", dir.path(), &d, &r, &json!({"operation":"status"})).unwrap();
        assert_eq!(status["sources"]["counter"], "error");
        assert_eq!(status["sources"]["timer"], "ready");
        assert_eq!(status["variables"]["remaining"], 60);
        assert_eq!(status["variables"]["count"], 0);
        assert!(status["ui"]["actions"]
            .as_array()
            .unwrap()
            .contains(&json!("host.reset")));
        assert!(!status["ui"]["actions"]
            .as_array()
            .unwrap()
            .contains(&json!("host.increment")));
        assert!(matches!(
            load(&dir.path().join("composition-state.json"), 16384)
                .unwrap()
                .models["counter"],
            Model::Counter { value: 1 }
        ));
        assert!(request(
            "alpha",
            dir.path(),
            &d,
            &r,
            &json!({"operation":"action","action":"host.increment"})
        )
        .is_err());
        let reset = request(
            "alpha",
            dir.path(),
            &d,
            &r,
            &json!({"operation":"action","action":"host.reset"}),
        )
        .unwrap();
        assert_eq!(reset["sources"]["counter"], "ready");
        assert_eq!(reset["variables"]["count"], 0);
    }
    #[test]
    fn concurrent_record_commands_do_not_overwrite_each_other() {
        let suite: Value =
            serde_json::from_str(include_str!("../../skills/petui/contracts/ledger-v1.json"))
                .unwrap();
        let r = &suite["runtime"];
        let mut value = suite["declaration"].clone();
        value["content"][0]["sources"]["records"]["unit"] = "event".into();
        let d = declaration(&value);
        validate(&d, r).unwrap();
        let dir = tempfile::tempdir().unwrap();
        let mut counts = std::thread::scope(|scope| {
            let jobs: Vec<_> = (0..24)
                .map(|_| {
                    scope.spawn(|| {
                        request(
                            "test",
                            dir.path(),
                            &d,
                            r,
                            &json!({"operation":"action","action":"host.mark"}),
                        )
                        .unwrap()["variables"]["count"]
                            .as_u64()
                            .unwrap()
                    })
                })
                .collect();
            jobs.into_iter()
                .map(|job| job.join().unwrap())
                .collect::<Vec<_>>()
        });
        counts.sort_unstable();
        assert_eq!(counts, (1..=24).collect::<Vec<_>>());
        assert_eq!(
            request("test", dir.path(), &d, r, &json!({"operation":"status"})).unwrap()
                ["variables"]["count"],
            24
        );
    }
    #[test]
    fn ledger_shared_contract_persistence_idempotence_and_stale_removal() {
        let suite: Value =
            serde_json::from_str(include_str!("../../skills/petui/contracts/ledger-v1.json"))
                .unwrap();
        let r = &suite["runtime"];
        let good = &suite["declaration"];
        let d = declaration(good);
        validate(&d, r).unwrap();
        for case in suite["rejections"].as_array().unwrap() {
            let mut bad = good.clone();
            bad["content"][0]["sources"]["records"][case["field"].as_str().unwrap()] =
                case["value"].clone();
            assert!(crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(), r).is_err());
        }
        let dir = tempfile::tempdir().unwrap();
        let op = json!({"operation":"action","action":"host.mark"});
        let once = request("test", dir.path(), &d, r, &op).unwrap();
        assert_eq!(once["variables"]["count"], 1);
        assert_eq!(
            request("test", dir.path(), &d, r, &op).unwrap()["variables"]["count"],
            1
        );
        assert!(!once["ui"]["actions"]
            .as_array()
            .unwrap()
            .contains(&json!("host.mark")));
        let undo = json!({"operation":"action","action":"host.undo","confirm":true,"context":once["ui"]["context"]});
        assert_eq!(
            request("test", dir.path(), &d, r, &undo).unwrap()["variables"]["count"],
            0
        );
        assert!(request("test", dir.path(), &d, r, &undo).is_err());
        let status = json!({"operation":"status"});
        assert_eq!(
            request("test", dir.path(), &d, r, &status).unwrap()["variables"]["count"],
            0
        );
        std::fs::write(
            dir.path().join("composition-state.json"),
            b"broken private record",
        )
        .unwrap();
        assert!(request("test", dir.path(), &d, r, &op).is_err());
        assert_eq!(
            std::fs::read(dir.path().join("composition-state.json")).unwrap(),
            b"broken private record"
        );
    }
}
