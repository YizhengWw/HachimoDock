//! data-only writer ownership, no grants, effects or IO.
use serde_json::Value;
use std::collections::BTreeSet;
pub const ENGINE:&str="p4-variable-owners-v1";
pub struct Plan {pub mixed:bool,pub host:BTreeSet<String>}
fn bad()->String{"变量所有权无效或写入者冲突；已有数据保留".into()}
fn name(s:&str)->bool{!s.is_empty()&&s.len()<=31&&s.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c))}
fn refs(v:&Value,owned:&BTreeSet<String>,depth:usize)->Result<(),String>{
    if depth>16{return Err(bad());}
    match v {
        Value::Array(a)=>for v in a{refs(v,owned,depth+1)?},
        Value::Object(o)=>for(k,v)in o{
            if ["var","fmt_mmss","fmt_hms","pct_of","of_max"].contains(&k.as_str())
                &&v.as_str().is_some_and(|s|owned.contains(s)){return Err(bad());}
            refs(v,owned,depth+1)?;
        },_=>{}
    }Ok(())
}
pub fn parse(r:&Value)->Result<Plan,String>{
    let vars=r["vars"].as_object().filter(|o|o.len()<=8).ok_or_else(bad)?;
    let Some(p)=r.get("ownership")else{
        return Ok(Plan{mixed:false,host:if r["host"]==true{vars.keys().cloned().collect()}else{BTreeSet::new()}});
    };
    if !p.as_object().is_some_and(|o|o.len()==2&&o.contains_key("engine")&&o.contains_key("host"))
        ||p["engine"]!=ENGINE||r["host"]!=true||r["engine"]!="p4-bounded-runtime-v4"
        ||r["host_context"]!="p4-host-context-v1"||r["input"]["engine"]!="p4-input-v1"
        ||r["surface"]["engine"]!="p4-app-surface-v1"||r["surface"]["bindings"]!="p4-variable-bindings-v1"
        ||["local","scene","game","media","instrument","data"].iter().any(|k|r.get(*k).is_some())
        ||vars.is_empty()||vars.iter().any(|(k,v)|!name(k)||!matches!(v["type"].as_str(),Some("int"|"string"))){return Err(bad());}
    let names=p["host"].as_array().filter(|a|!a.is_empty()&&a.len()<=vars.len()).ok_or_else(bad)?;
    for key in ["form","persist","program","dashboard"]{if let Some(v)=r.get(key){if !v.is_object(){return Err(bad());}}}
    let mut host=BTreeSet::new();
    for v in names{
        let k=v.as_str().filter(|k|name(k)&&vars.contains_key(*k)).ok_or_else(bad)?;
        if !host.insert(k.to_owned()){return Err(bad());}
    }
    let owns=|v:&Value|v.as_str().is_some_and(|k|host.contains(k));
    for key in ["transitions","tick"]{if let Some(value)=r.get(key){
        for rule in value.as_array().ok_or_else(bad)?{
            let object=rule.as_object().ok_or_else(bad)?;
            if rule.get("then").is_some_and(|v|!v.is_object()){return Err(bad());}
            if key=="transitions"&&rule["on"].as_str().is_some_and(|s|s.starts_with("host."))
                &&(object.len()!=2||!object.contains_key("from")||!object.contains_key("on")||rule["from"]!="*"){return Err(bad());}
            refs(rule,&host,0)?;
            for body in [rule,&rule["then"]]{for op in ["set","inc"]{if let Some(writes)=body.get(op){
                if writes.as_object().ok_or_else(bad)?.keys().any(|k|host.contains(k)){return Err(bad());}
            }}}
        }
    }}
    refs(&r["dashboard"],&host,0)?;
    if let Some(v)=r["persist"].get("vars"){
        if v.as_array().ok_or_else(bad)?.iter().any(owns){return Err(bad());}
    }
    let f=&r["form"];
    if f.get("keyboard").is_some_and(|v|!v.is_object()){return Err(bad());}
    if ["focus_var","active_var"].iter().any(|k|owns(&f[*k]))
        ||["index_var","label_var"].iter().any(|k|owns(&f["keyboard"][*k])){return Err(bad());}
    if let Some(v)=f.get("fields"){
        if v.as_array().ok_or_else(bad)?.iter().any(|f|!f.is_object()||owns(&f["var"])||owns(&f["draft_var"])){return Err(bad());}
    }
    if let Some(p)=r.get("program"){
        if !p["outputs"].as_array().is_some_and(|a|a.len()==8&&!a.iter().any(owns)){return Err(bad());}
    }
    Ok(Plan{mixed:true,host})
}
pub fn require_capability(r:&Value,caps:&Value)->Result<(),String>{
    if r.get("ownership").is_some()&&caps["widgetVariableOwners"]!=ENGINE{
        return Err("设备未开放宿主与设备变量隔离；未安装不兼容资源".into());
    }Ok(())
}
pub fn mixed(r:&Value)->Result<bool,String>{
    if r.get("ownership").is_none(){return Ok(false);}Ok(parse(r)?.mixed)
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn shared_writer_plan_rejects_collisions_and_requires_exact_capability(){
        let s:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/variable-owners-v1.json")).unwrap();
        let r=&s["runtime"];let p=parse(r).unwrap();assert!(p.mixed&&p.host==BTreeSet::from(["step".into()]));
        for patch in s["reject"].as_array().unwrap(){
            let mut bad=r.clone();let(parent,key)=patch["path"].as_str().unwrap().rsplit_once('/').unwrap();
            let at=bad.pointer_mut(parent).unwrap();if let Some(a)=at.as_array_mut(){a[key.parse::<usize>().unwrap()]=patch["value"].clone();}else{at[key]=patch["value"].clone();}
            assert!(parse(&bad).is_err(),"{patch}");
        }
        assert!(require_capability(r,&Value::Null).is_err());
        assert!(require_capability(r,&serde_json::json!({"widgetVariableOwners":true})).is_err());
        require_capability(r,&serde_json::json!({"widgetVariableOwners":ENGINE})).unwrap();
        let mut old=r.clone();old.as_object_mut().unwrap().remove("ownership");
        assert!(!parse(&old).unwrap().mixed&&parse(&old).unwrap().host.len()==2);
    }
}
