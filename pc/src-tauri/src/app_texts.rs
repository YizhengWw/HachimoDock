//! Bounded package text table, separate from surface longtext documents.
use serde_json::Value;
fn bad()->String{"text_at 需要合法 p4-texts-v1 文本池、int 索引及唯一文本来源".into()}
pub(crate) fn validate_runtime(runtime:&Value)->Result<(),String>{
    let Some(value)=runtime.get("texts") else{return Ok(())};
    let texts=value.as_array().filter(|a|(1..=64).contains(&a.len())).ok_or_else(bad)?;
    if runtime["engine"]!="p4-bounded-runtime-v4"{return Err(bad());}
    let mut used=texts.len()-1;
    for value in texts{
        let text=value.as_str().filter(|s|s.len()<=120&&!s.chars().any(|c|c<='\u{1f}'||c=='\u{7f}')).ok_or_else(bad)?;
        used+=text.len();
    }
    if used>4096{return Err(bad());}Ok(())
}
pub(crate) fn validate_reference(value:&Value,runtime:&Value)->Result<(),String>{
    validate_runtime(runtime)?;
    if !runtime["texts"].as_array().is_some_and(|a|!a.is_empty())
        ||!value.as_str().is_some_and(|name|runtime["vars"][name]["type"]=="int"){return Err(bad());}
    Ok(())
}
pub(crate) fn validate_node(node:&Value,runtime:Option<&Value>)->Result<(),String>{
    if !matches!(node["type"].as_str(),Some("text"|"button"))
        ||["text","bind","var_bind","field_bind","key_for","row_bind"].iter().any(|k|node.get(*k).is_some())
        ||!node["text_at"].as_str().is_some_and(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))){return Err(bad());}
    if let Some(runtime)=runtime{validate_reference(&node["text_at"],runtime)?;}Ok(())
}
pub(crate) fn require_capability(runtime:&Value,caps:&Value)->Result<(),String>{
    if runtime.get("texts").is_some()&&caps["widgetTexts"]!="p4-texts-v1"{return Err("需要 widgetTexts=p4-texts-v1，未安装不兼容资源".into());}Ok(())
}
