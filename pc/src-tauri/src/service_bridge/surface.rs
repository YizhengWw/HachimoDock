use super::*;
pub const DEVICE_ENGINE:&str="p4-service-bridge-v1";
pub fn normalize(s:&Value,normalized:&mut Value,runtime:Option<&Value>)->Result<()>{
    let declared=s.get("services").is_some();
    if declared&&(s["services"]!=DEVICE_ENGINE||s["engine"]!="p4-app-surface-v1"||s["graphics"]!="p4-app-graphics-v1"||s["bindings"]!="p4-variable-bindings-v1"||runtime.is_some_and(|r|r["host"]!=true||r["host_context"]!="p4-host-context-v1"||r["input"]["engine"]!="p4-input-v1")){return Err(fail());}
    if runtime.is_some_and(|r|if declared{!r["host_contract"].as_str().is_some_and(|s|s.len()==64&&s.bytes().all(|c|c.is_ascii_hexdigit()&&!c.is_ascii_uppercase()))}else{r.get("host_contract").is_some()}){return Err(fail());}
    normalized.as_object_mut().ok_or_else(fail)?.remove("services");
    let Some(nodes)=normalized["elements"].as_array_mut()else{return Err(fail())};
    for n in nodes {
        if let Some(b)=n.as_object_mut().ok_or_else(fail)?.remove("image_bind"){
            if !declared||n["type"]!="image"||!b.as_str().is_some_and(|s|name(s,15))||["tile","motion","rotate_ms"].iter().any(|k|n.get(*k).is_some())||s["images"].as_array().is_some_and(|a|a.iter().any(|im|im["id"]==n["image"]&&im.get("encoding").is_some())){return Err(fail());}
        }
        let param=n.as_object_mut().unwrap().remove("value_param");
        let lo=n.as_object_mut().unwrap().remove("value_min");let hi=n.as_object_mut().unwrap().remove("value_max");
        if let Some(param)=param {
            if !declared||n["type"]!="meter"||!param.as_str().is_some_and(|s|name(s,31))||!n["action"].as_str().is_some_and(|a|a.starts_with("host.")&&name(a,47))||runtime.is_some_and(|r|r["input"]["confirm"].get(n["action"].as_str().unwrap_or("")).is_some()){return Err(fail());}
            let low=lo.as_ref().map_or(Some(0),Value::as_i64).filter(|n|(-1_000_000_000..=1_000_000_000).contains(n)).ok_or_else(fail)?;
            if !hi.as_ref().map_or(Some(100),Value::as_i64).is_some_and(|n|n>low&&n<=1_000_000_000){return Err(fail());}
        }else if lo.is_some()||hi.is_some(){return Err(fail());}
    }Ok(())
}
pub fn validate_event_data(v:&Value)->bool{
    v.as_object().is_some_and(|m|m.len()<=4&&m.iter().all(|(k,v)|name(k,31)&&(scalar(v,"int")||scalar(v,"string")||scalar(v,"bool"))))
}
