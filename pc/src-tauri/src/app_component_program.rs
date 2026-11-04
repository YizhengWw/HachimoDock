//! Data-only component program schema. No execution, files, grants or providers.
use serde_json::Value;
use std::collections::BTreeSet;
pub const ENGINE:&str="p4-component-program-v1";
fn bad()->String{"组件程序需要有界纯整数指令、明确输入和唯一输出所有权；已有数据保留".into()}
fn exact(v:&Value,keys:&[&str])->bool{v.as_object().is_some_and(|o|o.len()==keys.len()&&keys.iter().all(|k|o.contains_key(*k)))}
fn number(v:&Value,lo:i64,hi:i64)->bool{v.as_f64().is_some_and(|n|n.is_finite()&&n.fract()==0.0&&n>=lo as f64&&n<=hi as f64)}
fn name(v:&str,max:usize)->bool{!v.is_empty()&&v.len()<=max&&v.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))}
pub fn image(p:&Value)->Result<(),String>{
    let v2=p["engine"]=="p4-integer-program-v2";
    if !exact(p,&["engine","schema","cells","fuel","code"])||(!v2&&p["engine"]!="p4-integer-program-v1")||!number(&p["schema"],1,u32::MAX as i64)||!number(&p["cells"],0,if v2{256}else{64})||!number(&p["fuel"],1,if v2{16384}else{4096}){return Err(bad());}
    let code=p["code"].as_array().filter(|a|!a.is_empty()&&a.len()<=if v2{1024}else{256}).ok_or_else(bad)?;
    for row in code{
        let a=row.as_array().filter(|a|a.len()==5).ok_or_else(bad)?;
        if !a[1..4].iter().all(|n|number(n,0,255))||!number(&a[4],i32::MIN as i64,i32::MAX as i64){return Err(bad());}
        let (x,y,z,n)=(a[1].as_f64().unwrap(),a[2].as_f64().unwrap(),a[3].as_f64().unwrap(),a[4].as_f64().unwrap());
        let valid=match a[0].as_str(){
            Some("HALT")=>x==0.0&&y==0.0&&z==0.0&&n==0.0,
            Some("CONST")=>x<16.0&&y==0.0&&z==0.0,
            Some("MOVE"|"LOAD"|"STORE")=>x<16.0&&y<16.0&&z==0.0&&n==0.0,
            Some("ADD"|"SUB"|"MUL"|"DIV"|"MOD"|"LT"|"EQ")=>x<16.0&&y<16.0&&z<16.0&&n==0.0,
            Some(op @ ("JUMP"|"JZ"))=>x<16.0&&(op!="JUMP"||x==0.0)&&y==0.0&&z==0.0&&n>=0.0&&n<(code.len() as f64),
            Some("EVENT")=>x<16.0&&y==0.0&&z==0.0&&(0.0..4.0).contains(&n),
            Some("TEXT") if v2=>x<16.0&&y<16.0&&z==0.0&&n==0.0,
            Some("CELLGET"|"CELLSET") if v2=>x<16.0&&y<16.0&&z<16.0&&n==0.0,
            Some("OUTPUT")=>x<8.0&&y<16.0&&z==0.0&&n==0.0,_=>false
        };if !valid{return Err(bad());}
    }Ok(())
}
pub fn require_capability(runtime:&Value,caps:&Value)->Result<(),String>{
    require_cells_texts_capability(runtime,caps)?;
    crate::app_variable_owners::require_capability(runtime,caps)?;
    if runtime.get("program").is_some()&&!((caps["widgetProgram"]==runtime["program"]["engine"])||(runtime["program"]["engine"]==ENGINE&&caps["widgetProgram"]=="p4-component-program-v2")||caps["widgetPrograms"].as_array().is_some_and(|a|a.contains(&runtime["program"]["engine"]))){return Err("设备未开放受限组件程序；未安装不兼容资源".into());}Ok(())
}
/// Call for every runtime, including packages without a program declaration.
pub fn require_cells_texts_capability(runtime:&Value,caps:&Value)->Result<(),String>{
    for (field,key,engine) in [("cells","widgetCells","p4-cells-v1"),("texts","widgetTexts","p4-texts-v1")]{
        if runtime.get(field).is_some()&&caps[key]!=engine{return Err(format!("设备未声明 {key}={engine}，未安装不兼容资源"));}
    }
    Ok(())
}
pub fn validate(r:&Value)->Result<(),String>{
    let Some(p)=r.get("program")else{return Ok(());};
    let owners=crate::app_variable_owners::parse(r)?;
    if !exact(p,&["engine","image","outputs","actions","timers"])||(p["engine"]!=ENGINE&&p["engine"]!="p4-component-program-v2")||r["engine"]!="p4-bounded-runtime-v4"
        ||r["surface"]["engine"]!="p4-app-surface-v1"||r["input"]["engine"]!="p4-input-v1"
        ||r.get("host").is_some()&&!owners.mixed
        ||["local","scene","game","media","instrument","data"].iter().any(|k|r.get(*k).is_some()){return Err(bad());}
    image(&p["image"])?;
    let image_v2=p["image"]["engine"]=="p4-integer-program-v2";
    if image_v2&&p["engine"]!="p4-component-program-v2"{return Err(bad());}
    let grid=r.get("cells").and_then(|g|Some((g["cols"].as_u64()?,g["rows"].as_u64()?)));
    let uses_grid=p["image"]["code"].as_array().unwrap().iter().any(|i|matches!(i[0].as_str(),Some("CELLGET"|"CELLSET")));
    if r.get("cells").is_some()||uses_grid{
        if !image_v2||!grid.is_some_and(|(c,h)|(1..=16).contains(&c)&&(1..=16).contains(&h)&&c*h<=p["image"]["cells"].as_u64().unwrap_or(0)){return Err(bad());}
    }
    if r.get("texts").is_some()&&(!image_v2||!r["texts"].as_array().is_some_and(|a|(1..=64).contains(&a.len()))){return Err(bad());}
    let vars=r["vars"].as_object().filter(|v|!v.is_empty()&&v.len()<=8).ok_or_else(bad)?;
    if vars.iter().any(|(k,v)|!name(k,31)||!matches!(v["type"].as_str(),Some("int"|"string"))){return Err(bad());}
    let outputs=p["outputs"].as_array().filter(|a|a.len()==8).ok_or_else(bad)?;let mut owned=BTreeSet::new();
    for k in outputs{if k.is_null(){continue;}let k=k.as_str().filter(|s|name(s,31)&&vars.get(*s).is_some_and(|v|v["type"]=="int")).ok_or_else(bad)?;if !owned.insert(k){return Err(bad());}}
    if owned.is_empty(){return Err(bad());}
    for (k,v) in vars{
        if v["type"]=="int"{if let Some(bounds)=r["limits"].get(k){
            let a=bounds.as_array().filter(|a|a.len()==2).ok_or_else(bad)?;
            if !number(&a[0],-1000000000,1000000000)||!number(&a[1],-1000000000,1000000000)||a[0].as_f64()>a[1].as_f64(){return Err(bad());}
        }}
    }
    for key in ["transitions","tick"]{if let Some(rules)=r[key].as_array(){for rule in rules{for body in [rule,&rule["then"],&rule["else"]]{for op in ["set","inc"]{if body[op].as_object().is_some_and(|o|o.keys().any(|k|owned.contains(k.as_str()))){return Err(bad());}}}}}}
    if r["persist"]["vars"].as_array().is_some_and(|a|a.iter().any(|v|v.as_str().is_some_and(|s|owned.contains(s)))){return Err(bad());}
    let form=&r["form"];
    if ["focus_var","active_var"].iter().any(|k|form[*k].as_str().is_some_and(|s|owned.contains(s)))
        ||["index_var","label_var"].iter().any(|k|form["keyboard"][*k].as_str().is_some_and(|s|owned.contains(s)))
        ||form["fields"].as_array().is_some_and(|a|a.iter().any(|f|["var","draft_var"].iter().any(|k|f[*k].as_str().is_some_and(|s|owned.contains(s))))){return Err(bad());}
    let actions=p["actions"].as_object().filter(|a|!a.is_empty()&&a.len()<=24).ok_or_else(bad)?;
    let rules=r["transitions"].as_array().ok_or_else(bad)?;
    for (action,words) in actions{
        if form["actions"].as_object().is_some_and(|a|a.values().any(|v|v==action)){return Err(bad());}
        if !name(action,47)||action.starts_with("host."){return Err(bad());}
        let matched=rules.iter().filter(|t|t["on"]==*action).collect::<Vec<_>>();
        if matched.is_empty()||matched.iter().any(|t|!exact(t,&["from","on"])||t["from"]!="*"){return Err(bad());}
        let words=words.as_array().filter(|a|a.len()<=4).ok_or_else(bad)?;
        for word in words{if !number(word,i32::MIN as i64,i32::MAX as i64)&&!(exact(word,&["var"])&&word["var"].as_str().is_some_and(|s|vars.get(s).is_some_and(|v|v["type"]=="int"))){return Err(bad());}}
    }
    let timers=p["timers"].as_array().filter(|a|a.len()<=8).ok_or_else(bad)?;let mut ids=BTreeSet::new();
    for t in timers{
        if !exact(t,&["id","delayMs","intervalMs","words"])||!number(&t["id"],1,65535)||!number(&t["delayMs"],0,u32::MAX as i64)||!number(&t["intervalMs"],0,u32::MAX as i64)
            ||t["intervalMs"].as_f64().is_some_and(|n|n>0.0&&n<100.0)||!t["words"].as_array().is_some_and(|a|a.len()<=4&&a.iter().all(|n|number(n,i32::MIN as i64,i32::MAX as i64))){return Err(bad());}
        if !ids.insert(t["id"].as_f64().unwrap() as u32){return Err(bad());}
    }Ok(())
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn v2_descriptor_vectors_and_v1_image_ceilings(){
        let s:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/component-program-v2.json")).unwrap();
        validate(&s["runtime"]).unwrap();
        for patch in s["reject"].as_array().unwrap(){
            if patch["scope"]=="package"{continue;}
            let mut r=s["runtime"].clone();let(parent,key)=patch["path"].as_str().unwrap().rsplit_once('/').unwrap();
            let at=r.pointer_mut(parent).unwrap();if let Some(a)=at.as_array_mut(){a[key.parse::<usize>().unwrap()]=patch["value"].clone();}else{at[key]=patch["value"].clone();}
            assert!(validate(&r).is_err(),"{patch}");
        }
        let mut v1=s["runtime"]["program"]["image"].clone();v1["engine"]="p4-integer-program-v1".into();
        assert!(image(&v1).is_err());
        require_capability(&s["runtime"],&serde_json::json!({"widgetProgram":"p4-component-program-v2","widgetCells":"p4-cells-v1","widgetTexts":"p4-texts-v1"})).unwrap();
        assert!(require_capability(&s["runtime"],&serde_json::json!({"widgetProgram":ENGINE})).is_err());
    }
    #[test]fn host_inputs_need_disjoint_explicit_writers_and_both_capabilities(){
        let s:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/variable-owners-v1.json")).unwrap();let r=&s["runtime"];
        validate(r).unwrap();
        for patch in s["reject"].as_array().unwrap(){
            let mut bad=r.clone();let(parent,key)=patch["path"].as_str().unwrap().rsplit_once('/').unwrap();let at=bad.pointer_mut(parent).unwrap();
            if let Some(a)=at.as_array_mut(){a[key.parse::<usize>().unwrap()]=patch["value"].clone();}else{at[key]=patch["value"].clone();}
            assert!(validate(&bad).is_err(),"{patch}");
        }
        assert!(require_capability(r,&serde_json::json!({"widgetProgram":ENGINE})).is_err());
        require_capability(r,&serde_json::json!({"widgetProgram":ENGINE,"widgetVariableOwners":crate::app_variable_owners::ENGINE})).unwrap();
        let mut legacy=r.clone();legacy.as_object_mut().unwrap().remove("ownership");assert!(validate(&legacy).is_err());
    }
    #[test]fn component_program_shared_contract_ownership_and_capability(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/component-program-v1.json")).unwrap();let r=&suite["runtime"];validate(r).unwrap();
        for p in suite["reject"].as_array().unwrap(){let mut bad=r.clone();let(parent,key)=p["path"].as_str().unwrap().rsplit_once('/').unwrap();let o=bad.pointer_mut(parent).unwrap();if let Some(a)=o.as_array_mut(){a[key.parse::<usize>().unwrap()]=p["value"].clone();}else{o[key]=p["value"].clone();}assert!(validate(&bad).is_err(),"{p}");}
        assert!(require_capability(r,&serde_json::json!({})).is_err());require_capability(r,&serde_json::json!({"widgetProgram":ENGINE})).unwrap();
        let mut old=r.clone();old.as_object_mut().unwrap().remove("program");validate(&old).unwrap();require_capability(&old,&Value::Null).unwrap();
        let mut feedback=r.clone();feedback["program"]["outputs"][1]=Value::String("step".into());validate(&feedback).unwrap();
        for key in ["focus_var","active_var"]{let mut bad=r.clone();bad["form"]=serde_json::json!({(key):"count"});assert!(validate(&bad).is_err());}
        for key in ["index_var","label_var"]{let mut bad=r.clone();bad["form"]=serde_json::json!({"keyboard":{(key):"count"}});assert!(validate(&bad).is_err());}
    }
    #[test]fn mixed_form_navigation_and_program_keep_disjoint_ownership(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/component-program-form-v1.json")).unwrap();
        let r=&suite["runtime"];validate(r).unwrap();
        for key in ["focus","editing","keyIndex"]{let mut bad=r.clone();bad["program"]["outputs"][0]=Value::String(key.into());assert!(validate(&bad).is_err());}
        assert!(require_capability(r,&serde_json::json!({"widgetAppFormV2":"p4-app-form-v2"})).is_err());
    }
}
