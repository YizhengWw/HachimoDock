//! p4-data-rows-v1: generic paged / filtered / selectable projection of a bounded
//! PC-fed data list (<=20 rows, 5 per page). Same rules as
//! firmware/main/pet_p4_data_rows.h and pc/skills/petui/scripts/data_rows.py.
use crate::widget_data::DataRow;
use serde::{Deserialize, Serialize};
use serde_json::Value;
pub const ENGINE: &str = "p4-data-rows-v1";
pub const CAPABILITY: &str = "widgetDataRows";
pub const REFRESH: &str = "data.refresh";
pub const PAGE: usize = 5;
const FIELDS: [&str; 5] = ["label", "value", "detail", "meta", "tone"];
pub const BIND_FIELDS: [&str; 4] = ["label", "value", "detail", "meta"];
fn bad() -> String { "通用数据行需要 p4-data-rows-v1：data_view、整数筛选/选择变量、data.refresh 保留动作与 row_bind 文本节点".into() }
fn ident(s: &str) -> bool { !s.is_empty() && s.len() <= 31 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) }
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct Filter { pub var: String, pub field: String, pub values: Vec<String> }
#[derive(Clone, Debug, Default, Serialize, Deserialize, PartialEq)]
pub struct Config { pub select_var: Option<String>, pub filters: Vec<Filter>, pub refresh_action: Option<String> }
pub fn extended(data: &Value) -> bool { ["select_var", "filters", "refresh_action"].iter().any(|k| data.get(*k).is_some()) }
pub fn declared(runtime: &Value) -> bool {
    let s = &runtime["surface"];
    extended(&runtime["data"]) || s["data_view"]["layout"] == "rows" || s["elements"].as_array().is_some_and(|a| a.iter().any(|n| n.get("row_bind").is_some()))
}
pub fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    if declared(runtime) && capabilities[CAPABILITY] != ENGINE { return Err("需要通用数据行能力 p4-data-rows-v1，请先升级固件；未安装不兼容资源".into()); }
    Ok(())
}
/// Strict data extension (select_var, filters, refresh_action).
pub fn config(data: &Value, runtime: Option<&Value>) -> Result<Config, String> {
    let o = data.as_object().filter(|o| o.contains_key("source") && o.contains_key("page_var") && o.keys().all(|k| ["source","page_var","select_var","filters","refresh_action"].contains(&k.as_str()))).ok_or_else(bad)?;
    let page_var = o["page_var"].as_str().unwrap_or("");
    let int_var = |name: &str| -> Result<(), String> {
        if !ident(name) || name == page_var { return Err(bad()); }
        if let Some(r) = runtime { if r["vars"][name]["type"] != "int" { return Err(bad()); } }
        Ok(())
    };
    let mut out = Config::default();
    if let Some(v) = o.get("select_var") { let name = v.as_str().ok_or_else(bad)?; int_var(name)?; out.select_var = Some(name.into()); }
    if let Some(v) = o.get("refresh_action") {
        if v != REFRESH { return Err(bad()); }
        if let Some(r) = runtime { if !r["transitions"].as_array().is_some_and(|ts| ts.iter().any(|t| t["on"] == REFRESH)) { return Err(bad()); } }
        out.refresh_action = Some(REFRESH.into());
    }
    if let Some(filters) = o.get("filters") {
        let list = filters.as_array().filter(|a| (1..=4).contains(&a.len())).ok_or_else(bad)?;
        for f in list {
            let fo = f.as_object().filter(|m| m.len() == 3 && ["var","field","values"].iter().all(|k| m.contains_key(*k))).ok_or_else(bad)?;
            let var = fo["var"].as_str().ok_or_else(bad)?; int_var(var)?;
            if out.filters.iter().any(|x| x.var == var) { return Err(bad()); }
            let field = fo["field"].as_str().filter(|s| FIELDS.contains(s)).ok_or_else(bad)?;
            let values = fo["values"].as_array().filter(|a| (1..=8).contains(&a.len())).ok_or_else(bad)?;
            let mut norm = Vec::new();
            for v in values {
                if field == "tone" && v.as_i64().is_some_and(|n| (-1..=1).contains(&n)) { norm.push(v.as_i64().unwrap().to_string()); }
                else { norm.push(v.as_str().filter(|s| (1..=24).contains(&s.len()) && !s.chars().any(char::is_control)).ok_or_else(bad)?.to_string()); }
            }
            out.filters.push(Filter { var: var.into(), field: field.into(), values: norm });
        }
    }
    Ok(out)
}
/// Package-level rules for every rows-related declaration.
pub fn validate(runtime: &Value) -> Result<(), String> {
    if !declared(runtime) { return Ok(()); }
    let s = &runtime["surface"];
    if !runtime["data"].is_object() || !s["data_view"].is_object() || s["bindings"] != "p4-variable-bindings-v1" { return Err(bad()); }
    if extended(&runtime["data"]) { config(&runtime["data"], Some(runtime))?; }
    for n in s["elements"].as_array().into_iter().flatten().filter(|n| n.get("row_bind").is_some()) {
        if !n["row_bind"].as_str().is_some_and(|f| BIND_FIELDS.contains(&f)) || !matches!(n["type"].as_str(), Some("text" | "button")) || ["text","bind","var_bind","field_bind","key_for"].iter().any(|k| n.get(*k).is_some()) { return Err(bad()); }
    }
    Ok(())
}
pub fn field(row: &DataRow, name: &str) -> String {
    match name { "label" => row.label.clone(), "value" => row.value.clone(), "detail" => row.detail.clone(), "meta" => row.meta.clone(), "tone" => row.tone.to_string(), _ => String::new() }
}
fn matches(row: &DataRow, f: &Filter, value: i64) -> bool {
    if value <= 0 { return true; }
    if value as usize > f.values.len() { return false; }
    field(row, &f.field) == f.values[value as usize - 1]
}
/// Visible rows (source order) for the current int filter values.
pub fn filter_rows<'a>(rows: &'a [DataRow], cfg: &Config, values: &dyn Fn(&str) -> i64) -> Vec<&'a DataRow> {
    rows.iter().take(20).filter(|r| cfg.filters.iter().all(|f| matches(r, f, values(&f.var)))).collect()
}
pub fn pages(count: usize) -> usize { count.div_ceil(PAGE).max(1) }
pub fn page_index(count: usize, value: i64) -> usize { value.rem_euclid(pages(count) as i64) as usize }
pub fn select(count: usize, value: i64) -> Option<usize> { if count == 0 { None } else if value < 0 { Some(0) } else { Some((value as usize).min(count - 1)) } }
#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    fn suite() -> Value { serde_json::from_str(include_str!("../../skills/petui/contracts/data-rows-v1.json")).unwrap() }
    #[test]
    fn rows_projection_matches_shared_vectors() {
        let s = suite(); let rows: Vec<DataRow> = serde_json::from_value(s["rows"].clone()).unwrap();
        let cfg: Config = serde_json::from_value(s["config"].clone()).unwrap();
        for case in s["cases"].as_array().unwrap() {
            let values = &case["values"]; let get = |name: &str| values[name].as_i64().unwrap_or(0);
            let visible = filter_rows(&rows, &cfg, &get);
            assert_eq!(visible.iter().map(|r| json!(r.id)).collect::<Vec<_>>(), case["visible"].as_array().unwrap().clone(), "{case}");
            let page = page_index(visible.len(), get("page")); assert_eq!(page as u64, case["page"].as_u64().unwrap());
            assert_eq!(pages(visible.len()) as u64, case["pages"].as_u64().unwrap());
            let slice: Vec<Value> = visible.iter().skip(page * PAGE).take(PAGE).map(|r| json!(r.id)).collect();
            assert_eq!(slice, case["rows"].as_array().unwrap().clone());
            let selected = select(visible.len(), get("sel")).map(|i| json!(visible[i].id)).unwrap_or(Value::Null);
            assert_eq!(selected, case["selected"]);
        }
    }
    #[test]
    fn rows_declaration_matches_shared_contract_and_capability_gate() {
        let s = suite(); let r = &s["runtime"];
        validate(r).unwrap(); crate::component_extension::validate_app_runtime(r).unwrap(); crate::component_extension::validate_app_surface(&r["surface"], Some(r)).unwrap();
        assert_eq!(config(&r["data"], Some(r)).unwrap(), serde_json::from_value::<Config>(s["config"].clone()).unwrap());
        for case in s["invalid"].as_array().unwrap() {
            let mut bad = r.clone(); let path = case["path"].as_array().unwrap(); let mut parent = &mut bad;
            for key in &path[..path.len() - 1] { parent = if let Some(k) = key.as_str() { &mut parent[k] } else { &mut parent[key.as_u64().unwrap() as usize] }; }
            let key = path.last().unwrap(); if let Some(k) = key.as_str() { parent[k] = case["value"].clone(); } else { parent[key.as_u64().unwrap() as usize] = case["value"].clone(); }
            assert!(validate(&bad).and_then(|_| crate::component_extension::validate_app_surface(&bad["surface"], Some(&bad))).is_err(), "{case}");
        }
        assert!(require_capability(r, &json!({})).is_err()); require_capability(r, &json!({"widgetDataRows":ENGINE})).unwrap();
        require_capability(&json!({"data":{"source":"stocks.watchlist","page_var":"p"},"surface":{}}), &json!({})).unwrap();
    }
}
