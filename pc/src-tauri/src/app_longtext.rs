//! p4-longtext-v1: deterministic long-text layout (identical to
//! firmware/main/pet_p4_text_flow.h) plus the surface.texts / longtext / figure
//! declarations. The metric model is normative: scalars >= U+2E80 advance one
//! font size, everything else half a font size rounded up.
use serde_json::Value;
pub const ENGINE: &str = "p4-longtext-v1";
pub const CAPABILITY: &str = "widgetLongText";
pub const MAX_BYTES: usize = 4096;
pub const MAX_LINES: usize = 256;
pub const LINE_BYTES: usize = 120;
fn bad() -> String { "长文需要 p4-longtext-v1：≤2 段 ≤4096 字节文本、整数页码变量、行高内的 longtext 节点与图片+说明的 figure".into() }
pub fn valid_text(text: &str) -> bool {
    (1..=MAX_BYTES).contains(&text.len()) && !text.chars().any(|c| (c.is_control() && c != '\n') || ('\u{7f}'..'\u{a0}').contains(&c))
}
fn wide(c: char) -> bool { c as u32 >= 0x2e80 }
fn advance(c: char, font: usize) -> usize { if wide(c) { font } else { font.div_ceil(2) } }
/// Greedy wrap into lines; returns (lines, truncated). Byte-identical to the C layout.
pub fn lines(text: &str, width: usize, font: usize, max_lines: usize) -> (Vec<String>, bool) {
    let raw = text.as_bytes(); let mut out = Vec::new(); let mut truncated = false;
    if raw.is_empty() || width < font || font < 10 { return (out, false); }
    let scalars: Vec<(usize, usize, char)> = text.char_indices().map(|(i, c)| (i, c.len_utf8(), c)).collect();
    let total = raw.len();
    let emit = |out: &mut Vec<String>, truncated: &mut bool, start: usize, mut end: usize| {
        while end > start && raw[end - 1] == b' ' { end -= 1; }
        if out.len() < max_lines { out.push(text[start..end].to_string()); } else { *truncated = true; }
    };
    let (mut line_start, mut line_width, mut last_break, mut k) = (0usize, 0usize, 0usize, 0usize);
    let mut i = 0usize;
    while i < total {
        let (_, n, c) = scalars[k];
        if c == '\n' { emit(&mut out, &mut truncated, line_start, i); i += n; k += 1; line_start = i; line_width = 0; last_break = 0; if truncated { break; } continue; }
        let w = advance(c, font);
        if wide(c) && i > line_start { last_break = i; }
        if (line_width + w > width || i + n - line_start > LINE_BYTES) && i > line_start {
            let cut = if last_break > line_start { last_break } else { i };
            emit(&mut out, &mut truncated, line_start, cut);
            if truncated { break; }
            line_start = cut;
            while line_start < total && raw[line_start] == b' ' { line_start += 1; }
            line_width = 0; last_break = 0;
            let mut j = line_start;
            while j < i { let (_, m, ch) = scalars.iter().find(|s| s.0 == j).copied().unwrap(); line_width += advance(ch, font); j += m; }
            if line_start > i { i = line_start; k = scalars.iter().position(|s| s.0 >= line_start).unwrap_or(scalars.len()); }
            // Recheck this scalar after a word break: the retained prefix can
            // still overflow the byte budget even if the pixel width fits.
            continue;
        }
        line_width += w; i += n; k += 1;
        if c == ' ' || wide(c) { last_break = i; }
    }
    if !truncated && (i > line_start || out.is_empty()) { emit(&mut out, &mut truncated, line_start, i); }
    (out, truncated)
}
pub fn per_page(h: usize, line_height: usize) -> usize { if line_height == 0 { 1 } else { (h / line_height).max(1) } }
pub fn pages(count: usize, per: usize) -> usize { if per == 0 { 1 } else { count.div_ceil(per).max(1) } }
pub fn page_index(value: i64, count: usize) -> usize { if count == 0 { 0 } else { value.rem_euclid(count as i64) as usize } }
pub fn declared(surface: &Value) -> bool {
    surface.get("texts").is_some() || surface["elements"].as_array().is_some_and(|a| a.iter().any(|n| matches!(n["type"].as_str(), Some("longtext" | "figure"))))
}
pub fn require_capability(runtime: &Value, capabilities: &Value) -> Result<(), String> {
    // The existing native install hook covers both bounded text profiles.
    crate::component_extension::app_texts::require_capability(runtime,capabilities)?;
    if declared(&runtime["surface"]) && capabilities[CAPABILITY] != ENGINE { return Err("需要长文分页与图文能力 p4-longtext-v1，请先升级固件；未安装不兼容资源".into()); }
    Ok(())
}
/// count_var is a derived native writer, distinct from form/VM/HOST writers.
pub fn validate_derived_writers(runtime:&Value)->Result<(),String>{
    let mut counts=std::collections::HashSet::new();
    let form=&runtime["form"];
    for node in runtime["surface"]["elements"].as_array().into_iter().flatten().filter(|n|n["type"]=="longtext"){
        let Some(value)=node.get("count_var") else{continue};
        let name=value.as_str().filter(|s|runtime["vars"][*s]["type"]=="int").ok_or_else(bad)?;
        let form_role=["focus_var","active_var"].iter().any(|k|form[*k]==name)
            ||form["fields"].as_array().is_some_and(|a|a.iter().any(|f|f["var"]==name||f["draft_var"]==name))
            ||form["keyboard"].as_object().is_some_and(|o|o.iter().any(|(k,v)|k.ends_with("_var")&&v==name));
        let program=runtime["program"]["outputs"].as_array().is_some_and(|a|a.iter().any(|v|v==name));
        let host=runtime["host"]==true&&(runtime.get("ownership").is_none()||runtime["ownership"]["host"].as_array().is_some_and(|a|a.iter().any(|v|v==name)));
        if node["page_var"]==name||!counts.insert(name)||form_role||program||host{return Err("longtext count_var 必须是独立 derived writer，不能与 form/PROGRAM/HOST 或另一 count_var 冲突".into());}
    }
    // Count mirrors may be read by guest logic, but only native layout writes
    // them. Validate every effect body, including branches not taken at boot.
    if !counts.is_empty() {
        let rules=runtime["transitions"].as_array().into_iter().flatten()
            .flat_map(|t|std::iter::once(t).chain(t.get("else")))
            .chain(runtime["tick"].as_array().into_iter().flatten()
                .flat_map(|t|std::iter::once(t).chain(t.get("then"))));
        for rule in rules {for op in ["set","inc"] {
            if rule[op].as_object().is_some_and(|writes|writes.keys().any(|name|counts.contains(name.as_str()))) {
                return Err("longtext count_var 是只读 derived writer，不能作为 guest set/inc 目标".into());
            }
        }}
    }
    Ok(())
}
/// Validate surface.texts; returns the text ids.
pub fn validate_texts(surface: &Value) -> Result<Vec<String>, String> {
    let Some(texts) = surface.get("texts") else { return Ok(vec![]); };
    if surface["bindings"] != "p4-variable-bindings-v1" { return Err(bad()); }
    let list = texts.as_array().filter(|a| (1..=2).contains(&a.len())).ok_or_else(bad)?;
    let mut ids = Vec::new();
    for t in list {
        let o = t.as_object().filter(|o| o.len() == 2).ok_or_else(bad)?;
        let id = o.get("id").and_then(Value::as_str).filter(|s| (1..=15).contains(&s.len()) && s.bytes().all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b"-_".contains(&b))).ok_or_else(bad)?;
        let text = o.get("text").and_then(Value::as_str).filter(|s| valid_text(s)).ok_or_else(bad)?; let _ = text;
        if ids.iter().any(|x: &String| x == id) { return Err(bad()); }
        ids.push(id.to_string());
    }
    Ok(ids)
}
/// Validate one longtext node; returns the number of standard line nodes it expands into.
pub fn validate_longtext(n: &Value, texts: &[String], used: &mut Vec<String>, runtime: Option<&Value>) -> Result<usize, String> {
    let allowed = ["type","x","y","w","h","color","page","text_id","page_var","count_var","font_size","line_height","align","weight","state_mask","host_mask","show"];
    if !n.as_object().is_some_and(|o| o.keys().all(|k| allowed.contains(&k.as_str()))) { return Err(bad()); }
    let id = n["text_id"].as_str().filter(|s| texts.iter().any(|t| t == s)).ok_or_else(bad)?;
    if used.iter().any(|u| u == id) { return Err(bad()); }
    used.push(id.to_string());
    let font = n.get("font_size").map_or(Some(16), Value::as_u64).filter(|v| (10..=96).contains(v)).ok_or_else(bad)? as usize;
    let lh = n.get("line_height").map_or(Some(((font * 5 + 3) / 4) as u64), Value::as_u64).filter(|v| *v as usize >= font && *v <= 120).ok_or_else(bad)? as usize;
    let w = n["w"].as_u64().ok_or_else(bad)? as usize; let h = n["h"].as_u64().ok_or_else(bad)? as usize;
    if h < lh || w < font || w > 40 * font || h / lh > 24 { return Err(bad()); }
    let page_var = n["page_var"].as_str().filter(|s| !s.is_empty() && s.len() <= 31).ok_or_else(bad)?;
    let count_var = n.get("count_var").map(|v| v.as_str().filter(|s| !s.is_empty() && s.len() <= 31 && *s != page_var).ok_or_else(bad)).transpose()?;
    if let Some(r) = runtime { for v in std::iter::once(page_var).chain(count_var) { if r["vars"][v]["type"] != "int" { return Err(bad()); } } }
    Ok(per_page(h, lh))
}
/// Validate one figure node (palette image above a one-line caption); expands into 2 nodes.
pub fn validate_figure(n: &Value, surface: &Value) -> Result<usize, String> {
    let allowed = ["type","x","y","w","h","color","page","image","caption","font_size","line_height","align","weight","state_mask","host_mask","show","radius"];
    if !n.as_object().is_some_and(|o| o.keys().all(|k| allowed.contains(&k.as_str()))) { return Err(bad()); }
    let image = surface["images"].as_array().and_then(|a| a.iter().find(|im| im["id"] == n["image"])).ok_or_else(bad)?;
    if image.get("encoding").is_some() { return Err(bad()); }
    let caption = n["caption"].as_str().filter(|s| s.len() <= 120 && !s.chars().any(char::is_control)).ok_or_else(bad)?; let _ = caption;
    let font = n.get("font_size").map_or(Some(16), Value::as_u64).filter(|v| (10..=96).contains(v)).ok_or_else(bad)? as usize;
    let lh = n.get("line_height").map_or(Some(((font * 5 + 3) / 4) as u64), Value::as_u64).filter(|v| *v as usize >= font && *v <= 120).ok_or_else(bad)? as usize;
    if (n["h"].as_u64().ok_or_else(bad)? as usize) < lh + 8 { return Err(bad()); }
    Ok(2)
}
#[cfg(test)]
mod tests {
    use super::*;
    fn suite() -> Value { serde_json::from_str(include_str!("../../skills/petui/contracts/longtext-v1.json")).unwrap() }
    #[test]
    fn text_flow_matches_shared_layout_vectors() {
        let s = suite();
        for case in s["flow"].as_array().unwrap() {
            let (rows, truncated) = lines(case["text"].as_str().unwrap(), case["width"].as_u64().unwrap() as usize, case["font_size"].as_u64().unwrap() as usize, MAX_LINES);
            let expected: Vec<String> = case["lines"].as_array().unwrap().iter().map(|v| v.as_str().unwrap().to_string()).collect();
            assert_eq!(rows, expected, "{case}"); assert_eq!(truncated, case["truncated"] == true);
        }
        for case in s["paging"].as_array().unwrap() {
            let per = per_page(case["h"].as_u64().unwrap() as usize, case["line_height"].as_u64().unwrap() as usize);
            assert_eq!(per as u64, case["per_page"].as_u64().unwrap());
            let total = pages(case["lines"].as_u64().unwrap() as usize, per);
            assert_eq!(total as u64, case["pages"].as_u64().unwrap());
            assert_eq!(page_index(case["value"].as_i64().unwrap(), total) as u64, case["page"].as_u64().unwrap());
        }
        let (rows, truncated) = lines(&"字".repeat(4096 / 3), 16, 16, 4); assert_eq!(rows.len(), 4); assert!(truncated);
        assert!(valid_text("a\nb") && !valid_text("a\tb") && !valid_text("") && !valid_text(&"x".repeat(4097)));
    }
    #[test]
    fn longtext_declarations_follow_the_shared_contract() {
        let s = suite(); let r = &s["runtime"];
        crate::component_extension::validate_app_runtime(r).unwrap();
        crate::component_extension::validate_app_surface(&r["surface"], Some(r)).unwrap();
        for case in s["invalid"].as_array().unwrap() {
            let mut bad = r.clone(); let path = case["path"].as_array().unwrap(); let mut parent = &mut bad;
            for key in &path[..path.len() - 1] { parent = if let Some(k) = key.as_str() { &mut parent[k] } else { &mut parent[key.as_u64().unwrap() as usize] }; }
            let key = path.last().unwrap(); if let Some(k) = key.as_str() { parent[k] = case["value"].clone(); } else { parent[key.as_u64().unwrap() as usize] = case["value"].clone(); }
            assert!(crate::component_extension::validate_app_runtime(&bad).and_then(|_| crate::component_extension::validate_app_surface(&bad["surface"], Some(&bad))).is_err(), "{case}");
        }
        assert!(require_capability(r, &serde_json::json!({})).is_err());
        require_capability(r, &serde_json::json!({"widgetLongText":ENGINE})).unwrap();
    }
}
