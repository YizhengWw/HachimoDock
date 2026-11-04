//! One bounded host-fed collection block. No IO, credentials or source records.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::BTreeSet;

pub const ENGINE: &str = "p4-host-collection-v1";
pub fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    if runtime["surface"].get("collection_view").is_some() && capabilities["widgetHostCollection"] != ENGINE {
        return Err("设备未开放宿主虚拟列表能力；未安装不兼容资源".into());
    }
    Ok(())
}
fn bad() -> String { "集合控件需要有界布局、稳定来源和匹配的行操作；原数据保留".into() }
fn name(s: &str, max: usize) -> bool {
    !s.is_empty() && s.len() <= max && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b))
}
fn text(s: &str, nonempty: bool) -> bool {
    (!nonempty || !s.is_empty()) && s.len() <= 63 && !s.chars().any(char::is_control)
}
// Native cJSON and JS see mathematical JSON numbers, not integer lexemes.
// Keep these new bounded fields consistent without converting bools/strings.
fn integer(v: Value) -> Option<i64> {
    v.as_i64().or_else(||v.as_f64().filter(|n|n.is_finite()&&n.fract()==0.0&&(-1e9..=1e9).contains(n)).map(|n|n as i64))
}
fn u16_number<'de,D:serde::Deserializer<'de>>(d:D)->Result<u16,D::Error>{
    integer(Value::deserialize(d)?).and_then(|n|u16::try_from(n).ok()).ok_or_else(||serde::de::Error::custom("bounded integer required"))
}
fn u8_number<'de,D:serde::Deserializer<'de>>(d:D)->Result<u8,D::Error>{
    integer(Value::deserialize(d)?).and_then(|n|u8::try_from(n).ok()).ok_or_else(||serde::de::Error::custom("bounded integer required"))
}
fn i16_number<'de,D:serde::Deserializer<'de>>(d:D)->Result<i16,D::Error>{
    integer(Value::deserialize(d)?).and_then(|n|i16::try_from(n).ok()).ok_or_else(||serde::de::Error::custom("bounded integer required"))
}
fn colors<'de,D:serde::Deserializer<'de>>(d:D)->Result<[u16;6],D::Error>{
    let values=Vec::<Value>::deserialize(d)?;
    if values.len()!=6{return Err(serde::de::Error::custom("six colors required"));}
    let mut out=[0u16;6];
    for (i,v) in values.into_iter().enumerate(){out[i]=integer(v).and_then(|n|u16::try_from(n).ok()).ok_or_else(||serde::de::Error::custom("bounded color required"))?;}
    Ok(out)
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    engine: String,
    pub source: String,
    #[serde(deserialize_with="u16_number")] x: u16,
    #[serde(deserialize_with="u16_number")] y: u16,
    #[serde(deserialize_with="u16_number")] w: u16,
    #[serde(deserialize_with="u16_number")] row_height: u16,
    #[serde(deserialize_with="u8_number")] gap: u8,
    #[serde(deserialize_with="u8_number")]
    pub visible: u8,
    #[serde(deserialize_with="u8_number")] page: u8,
    pub actions: Vec<String>,
    #[serde(deserialize_with="colors")] colors: [u16; 6],
}
impl Config {
    pub fn parse(value: &Value) -> Result<Self, String> {
        let c: Self = serde_json::from_value(value.clone()).map_err(|_| bad())?;
        let height = u32::from(c.visible) * u32::from(c.row_height)
            + u32::from(c.visible.saturating_sub(1)) * u32::from(c.gap);
        if c.engine != ENGINE || !name(&c.source, 16) || !(1..=8).contains(&c.visible)
            || c.x >= 640 || c.y >= 480 || c.w < 128 || u32::from(c.x) + u32::from(c.w) > 640
            || !(48..=96).contains(&c.row_height) || c.gap > 16 || height + u32::from(c.y) > 480
            || c.page > 4 || c.actions.len() != usize::from(c.visible)
            || c.actions.iter().any(|a| !a.strip_prefix("host.").is_some_and(|s| name(s, 42)))
            || c.actions.iter().collect::<BTreeSet<_>>().len() != c.actions.len() {
            return Err(bad());
        }
        Ok(c)
    }
    pub fn validate_surface(&self, surface: &Value, runtime: Option<&Value>) -> Result<(), String> {
        let elements = surface["elements"].as_array().ok_or_else(bad)?;
        if surface["engine"] != "p4-app-surface-v1" || surface["graphics"] != "p4-app-graphics-v1"
            || elements.is_empty() || elements.len() + usize::from(self.visible) * 3 > 32
            || ["game_view","scene_view","data_view","input"].iter().any(|k|surface.get(*k).is_some()) {
            return Err(bad());
        }
        let height = u64::from(self.visible) * u64::from(self.row_height)
            + u64::from(self.visible - 1) * u64::from(self.gap);
        let overlaps = |v: &Value| -> bool {
            let Some(x) = v["x"].as_u64() else { return true; };
            let Some(y) = v["y"].as_u64() else { return true; };
            let Some(w) = v["w"].as_u64() else { return true; };
            let Some(h) = v["h"].as_u64() else { return true; };
            u64::from(self.x) < x.saturating_add(w) && u64::from(self.x) + u64::from(self.w) > x
                && u64::from(self.y) < y.saturating_add(h) && u64::from(self.y) + height > y
        };
        if surface.get("viewport").is_some_and(overlaps)
            || surface["viewports"]["items"].as_array().is_some_and(|vs| vs.iter().any(overlaps))
            || elements.iter().any(|n| n.get("action").is_some() && n.get("view").is_none()
                && n.get("in_view").is_none() && overlaps(n)) {
            return Err(bad());
        }
        if let Some(r) = runtime {
            if r["host"] != true || r["host_context"] != "p4-host-context-v1"
                || r["input"]["engine"] != "p4-input-v1"
                || self.page != 4 && usize::from(self.page) >= r["pages"].as_array().map_or(1, Vec::len)
                || self.actions.iter().any(|a| !r["transitions"].as_array()
                    .is_some_and(|ts| ts.iter().any(|t| t["on"] == *a))) {
                return Err(bad());
            }
        }
        Ok(())
    }
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields)]
struct Row {
    present: bool, key: String, label: String, detail: String,
    #[serde(deserialize_with="i16_number")] index: i16,
    selected: bool, truncated: bool,
}
#[derive(Clone, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
struct Snapshot {
    engine: String, source: String,
    #[serde(deserialize_with="u8_number")] visible: u8,
    #[serde(deserialize_with="u16_number")] count: u16,
    #[serde(deserialize_with="u16_number")] offset: u16,
    #[serde(deserialize_with = "selection")]
    selection_key: Option<String>,
    rows: Vec<Row>,
}
fn selection<'de, D: serde::Deserializer<'de>>(d: D) -> Result<Option<String>, D::Error> {
    Option::<String>::deserialize(d)
}
pub fn validate_snapshot(v: &Value) -> Result<(), String> {
    let s: Snapshot = serde_json::from_value(v.clone()).map_err(|_| bad())?;
    if s.engine != ENGINE || !name(&s.source, 16) || !(1..=8).contains(&s.visible) || s.count > 128
        || s.rows.len() != usize::from(s.visible) || s.selection_key.as_ref().is_some_and(|s| !text(s, true)) {
        return Err(bad());
    }
    let last = s.count.saturating_sub(1) / u16::from(s.visible) * u16::from(s.visible);
    if s.offset > last || s.offset % u16::from(s.visible) != 0 { return Err(bad()); }
    let mut keys = BTreeSet::new();
    for (i, row) in s.rows.iter().enumerate() {
        let index = usize::from(s.offset) + i;
        if row.present != (index < usize::from(s.count)) { return Err(bad()); }
        if row.present {
            if !text(&row.key, true) || !text(&row.label, false) || !text(&row.detail, false)
                || !keys.insert(&row.key) || row.index != index as i16
                || row.selected != (s.selection_key.as_deref() == Some(row.key.as_str())) { return Err(bad()); }
        } else if !row.key.is_empty() || !row.label.is_empty() || !row.detail.is_empty()
            || row.index != -1 || row.selected || row.truncated { return Err(bad()); }
    }
    Ok(())
}
pub fn project(source: &str, visible: u8, model: &Value) -> Result<Value, String> {
    let v = json!({"engine":ENGINE,"source":source,"visible":visible,"count":model["count"],
        "offset":model["offset"],"selectionKey":model["selectionKey"],"rows":model["rows"]});
    validate_snapshot(&v)?;
    Ok(v)
}
/// Existing app/snapshot budget is unchanged. Never split a publication, trim
/// identities, leak full records into diagnostics or repeat the source action.
pub fn bounded_envelope(payload: Value) -> Result<Value, String> {
    if !payload["componentId"].as_str().is_some_and(|s|name(s,47))
        || !payload["instance"].as_str().is_some_and(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_hexdigit()||b==b'-'))
        || !payload["sequence"].as_u64().is_some_and(|n|(1..=u64::from(u32::MAX)).contains(&n)) { return Err(bad()); }
    if let Some(v) = payload.get("collectionView") { validate_snapshot(v)?; }
    if serde_json::to_vec(&payload).map_err(|_| bad())?.len() <= 2048 { return Ok(payload); }
    Ok(json!({"componentId":payload["componentId"],"instance":payload["instance"],"sequence":payload["sequence"],
        "values":{"headline":"列表暂不可显示","note":"可见快照超出传输预算；记录已保留，未重发操作"},
        "ui":{"mode":7,"actions":[]}}))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn suite()->Value{serde_json::from_str(include_str!("../../skills/petui/contracts/host-collection-v1.json")).unwrap()}
    fn patched(value:&Value,patch:&Value)->Value{
        let mut out=value.clone();let (parent,key)=patch["path"].as_str().unwrap().rsplit_once('/').unwrap();
        let target=if parent.is_empty(){&mut out}else{out.pointer_mut(parent).unwrap()};
        if let Some(a)=target.as_array_mut(){a[key.parse::<usize>().unwrap()]=patch["value"].clone();}else{target[key]=patch["value"].clone();}out
    }
    #[test]
    fn host_collection_shared_contract_refuses_invalid_packets(){
        let s=suite();assert!(Config::parse(&s["config"]).is_ok());assert!(validate_snapshot(&s["snapshot"]).is_ok());
        for p in s["configReject"].as_array().unwrap(){assert!(Config::parse(&patched(&s["config"],p)).is_err(),"{p}");}
        for p in s["snapshotReject"].as_array().unwrap(){assert!(validate_snapshot(&patched(&s["snapshot"],p)).is_err(),"{p}");}
        for field in ["selectionKey","rows","source"]{let mut m=s["snapshot"].clone();m.as_object_mut().unwrap().remove(field);assert!(validate_snapshot(&m).is_err());}
        let mut c=s["config"].clone();c["visible"]=json!(2.0);c["x"]=json!(20.0);c["colors"][0]=json!(65535.0);assert!(Config::parse(&c).is_ok());
        let mut m=s["snapshot"].clone();m["count"]=json!(3.0);m["rows"][0]["label"]=json!("🙂".repeat(15));assert!(validate_snapshot(&m).is_ok());
        m["rows"][0]["label"]=json!("🙂".repeat(16));assert!(validate_snapshot(&m).is_err());
    }
    #[test]
    fn host_collection_requires_explicit_capability_and_layout_ownership(){
        let s=suite();let mut r=s["runtimeBase"].clone();r["surface"]["collection_view"]=s["config"].clone();
        let c=Config::parse(&s["config"]).unwrap();c.validate_surface(&r["surface"],Some(&r)).unwrap();
        for cap in [json!({}),json!({"widgetHostCollection":true}),json!({"widgetHostCollection":"future"})]{assert!(require_capability(&r,&cap).is_err());}
        require_capability(&r,&json!({"widgetHostCollection":ENGINE})).unwrap();require_capability(&s["runtimeBase"],&json!({})).unwrap();
        r["host_context"]=Value::Null;assert!(c.validate_surface(&r["surface"],Some(&r)).is_err());
        r["surface"]["elements"].as_array_mut().unwrap().push(json!({"type":"button","x":30,"y":110,"w":100,"h":48,"action":"host.other"}));
        assert!(c.validate_surface(&r["surface"],None).is_err());
    }
    #[test]
    fn host_collection_projection_never_exports_private_selected_record(){
        let s=suite();let mut m=s["snapshot"].clone();m["selected"]=json!({"value":{"private":"private-sentinel"}});
        m["records"]=json!([{"private":"private-sentinel"}]);let p=project("items",2,&m).unwrap();
        assert_eq!(p,s["snapshot"]);assert!(!p.to_string().contains("private-sentinel"));
        let empty=json!({"present":false,"key":"","label":"","detail":"","index":-1,"selected":false,"truncated":false});
        m["offset"]=json!(2);m["rows"]=json!([{"present":true,"key":"gamma","label":"第三项","detail":"","index":2,"selected":false,"truncated":false},empty]);
        let last=project("items",2,&m).unwrap();assert_eq!(last["selectionKey"],"beta");
        m["count"]=json!(0);m["offset"]=json!(0);m["rows"]=json!([empty.clone(),empty]);assert!(project("items",2,&m).is_ok());
    }
    #[test]
    fn host_collection_transport_budget_is_atomic_and_error_has_no_actions_or_records(){
        let s=suite();let mut p=json!({"componentId":"test","instance":"ab-12","sequence":1,"collectionView":s["snapshot"],"variables":{"count":3},"ui":{"mode":0,"context":"abc","actions":["host.pick_a"]}});
        assert_eq!(bounded_envelope(p.clone()).unwrap(),p);
        p["privateData"]=json!("private-sentinel".repeat(200));let err=bounded_envelope(p.clone()).unwrap();
        assert!(err.to_string().len()<2048);assert_eq!(err["ui"]["mode"],7);assert_eq!(err["ui"]["actions"],json!([]));
        assert!(err.get("variables").is_none()&&err.get("collectionView").is_none());assert!(!err.to_string().contains("private-sentinel"));
        p["sequence"]=json!(0);assert!(bounded_envelope(p).is_err());
    }
}
