//! Bounded data-only draw declarations. No provider, credentials or pixel pointer.
use serde_json::Value;
use std::collections::BTreeSet;
pub const ENGINE:&str="p4-canvas-v1";
pub const ENGINE_V2:&str="p4-canvas-v2";
/// canvas.md cost table: (commands, names, spans, pooled polyline points).
fn budget(engine:&str)->(usize,usize,u64,usize){if engine==ENGINE_V2{(64,16,8192,64)}else{(16,8,2048,0)}}
fn fields(v:&Value,keys:&[&str])->bool{
    v.as_object().is_some_and(|o|o.len()==keys.len()&&o.keys().all(|k|keys.contains(&k.as_str())))
}
fn number(v:&Value,lo:u64,hi:u64)->Option<u64>{v.as_u64().filter(|n|(lo..=hi).contains(n))}
fn operand(v:&Value,lo:u64,hi:u64,names:&mut BTreeSet<String>,limit:usize,runtime:Option<&Value>)->bool{
    if number(v,lo,hi).is_some(){return true;}
    if !fields(v,&["var"]){return false;}
    let Some(name)=v["var"].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b)))else{return false;};
    names.insert(name.into());names.len()<=limit&&runtime.is_none_or(|r|r["vars"][name]["type"]=="int")
}
pub fn validate(c:&Value,runtime:Option<&Value>)->Result<(),String>{
    let fail=||"绘图声明、整数绑定或绘制预算无效；未修改已有组件".to_string();
    let engine=c["engine"].as_str().filter(|e|*e==ENGINE||*e==ENGINE_V2).ok_or_else(fail)?;
    let v2=engine==ENGINE_V2;let (max_commands,max_names,max_spans,max_points)=budget(engine);
    if !(fields(c,&["engine","x","y","w","h","commands"])||fields(c,&["engine","x","y","w","h","commands","page"])){return Err(fail());}
    if let Some(page)=c.get("page"){
        let page=number(page,0,4).ok_or_else(fail)?;
        if page!=4&&runtime.is_some_and(|r|page>=r["pages"].as_array().map_or(1,|a|a.len().max(1)) as u64){return Err(fail());}
    }
    let x=number(&c["x"],0,639).ok_or_else(fail)?;let y=number(&c["y"],0,479).ok_or_else(fail)?;
    let w=number(&c["w"],1,320).ok_or_else(fail)?;let h=number(&c["h"],1,240).ok_or_else(fail)?;
    if x+w>640||y+h>480{return Err(fail());}
    let commands=c["commands"].as_array().filter(|a|(1..=max_commands).contains(&a.len())).ok_or_else(fail)?;
    let radius=w.min(h)/2;let side=w.max(h);
    let mut names=BTreeSet::new();let mut cost=0u64;let mut points=0usize;
    for p in commands{
        let op=p["op"].as_str().unwrap_or("");
        let keys:&[&str]=match op{
            "line"=>&["op","x","y","x2","y2","color"],"rect"=>&["op","x","y","w","h","color"],"circle"=>&["op","x","y","r","color"],
            "polyline" if v2=>&["op","points","color"],"arc" if v2=>&["op","x","y","r","start","end","color"],"ring" if v2=>&["op","x","y","r","inner","color"],
            _=>return Err(fail())};
        if !fields(p,keys)||number(&p["color"],0,65535).is_none(){return Err(fail());}
        let mut bound=|v:&Value,lo:u64,hi:u64|operand(v,lo,hi,&mut names,max_names,runtime);
        if op=="polyline"{
            let pts=p["points"].as_array().filter(|a|(3..=16).contains(&a.len())).ok_or_else(fail)?;
            points+=pts.len();if points>max_points{return Err(fail());}
            for pt in pts{
                let pair=pt.as_array().filter(|a|a.len()==2).ok_or_else(fail)?;
                if !bound(&pair[0],0,w-1)||!bound(&pair[1],0,h-1){return Err(fail());}
            }
            cost+=(pts.len() as u64-1)*side;
        }else{
            if !bound(&p["x"],0,w-1)||!bound(&p["y"],0,h-1){return Err(fail());}
            let ok=match op{
                "line"=>{cost+=side;bound(&p["x2"],0,w-1)&&bound(&p["y2"],0,h-1)},
                "rect"=>{cost+=1;bound(&p["w"],1,w)&&bound(&p["h"],1,h)},
                "circle"=>{let ok=bound(&p["r"],1,radius);cost+=2*number(&p["r"],1,radius).unwrap_or(radius)+1;ok},
                "arc"=>{let ok=bound(&p["r"],1,radius)&&bound(&p["start"],0,359)&&bound(&p["end"],0,359);cost+=6*number(&p["r"],1,radius).unwrap_or(radius)+8;ok},
                _=>{
                    let ok=bound(&p["r"],1,radius)&&radius>=1&&bound(&p["inner"],0,radius-1);
                    let inner_ok=match (number(&p["r"],1,radius),number(&p["inner"],0,radius.saturating_sub(1))){(Some(r),Some(i))=>i<r,_=>true};
                    cost+=4*number(&p["r"],1,radius).unwrap_or(radius)+2;ok&&inner_ok
                }
            };
            if !ok{return Err(fail());}
        }
        if cost>max_spans{return Err(fail());}
    }
    Ok(())
}
/// A v1 declaration installs on a v1 or v2 device; a v2 declaration needs widgetCanvas=p4-canvas-v2.
pub fn capability_accepts(engine:&str,capability:&Value)->bool{
    if engine==ENGINE_V2{capability==ENGINE_V2}else{capability==ENGINE||capability==ENGINE_V2}
}
pub fn require_capability(runtime:&Value,capabilities:&Value)->Result<(),String>{
    if let Some(canvas)=runtime["surface"].get("canvas"){
        if !capability_accepts(canvas["engine"].as_str().unwrap_or(""),&capabilities["widgetCanvas"]){
            return Err("设备未开放有界绘图能力；未安装不兼容资源".into());
        }
    }
    Ok(())
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn shared_declarations(){
        let f:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/canvas-v1.json")).unwrap();
        for c in f["valid"].as_array().unwrap(){assert!(validate(c,Some(&f["runtime"])).is_ok(),"{c}");}
        for c in f["invalid"].as_array().unwrap(){assert!(validate(c,Some(&f["runtime"])).is_err(),"{c}");}
        assert!(require_capability(&serde_json::json!({"surface":{"canvas":{}}}),&serde_json::json!({})).is_err());
    }
    #[test]fn shared_v2_declarations_and_capability_negotiation(){
        let f:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/canvas-v2.json")).unwrap();
        for c in f["valid"].as_array().unwrap(){assert!(validate(c,Some(&f["runtime"])).is_ok(),"{c}");}
        for c in f["invalid"].as_array().unwrap(){assert!(validate(c,Some(&f["runtime"])).is_err(),"{c}");}
        let v1=serde_json::json!({"surface":{"canvas":{"engine":ENGINE}}});let v2=serde_json::json!({"surface":{"canvas":{"engine":ENGINE_V2}}});
        for (runtime,device,ok) in [(&v1,ENGINE,true),(&v1,ENGINE_V2,true),(&v2,ENGINE_V2,true),(&v2,ENGINE,false),(&v2,"",false),(&v1,"",false)]{
            assert_eq!(require_capability(runtime,&serde_json::json!({"widgetCanvas":device})).is_ok(),ok,"{runtime} on {device}");
        }
        // A 65th command, a 17th name and a span overflow are refused before any device sees them.
        let mut c=f["valid"][2].clone();c["commands"].as_array_mut().unwrap().push(serde_json::json!({"op":"rect","x":0,"y":0,"w":1,"h":1,"color":1}));assert!(validate(&c,None).is_err());
        let mut c=f["valid"][0].clone();c["w"]=320.into();c["h"]=240.into();c["commands"]=Value::Array(vec![serde_json::json!({"op":"polyline","points":(0..16).map(|i|serde_json::json!([i,0])).collect::<Vec<_>>(),"color":1});3]);assert!(validate(&c,None).is_err());
    }
    #[test]fn hostile_values_costs_and_page_references_fail_without_panicking(){
        let f:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/canvas-v1.json")).unwrap();
        for key in ["x","y","w","h","commands","page"]{
            for value in [Value::Null,serde_json::json!(true),serde_json::json!(-1),serde_json::json!(1.5),serde_json::json!(1e300),serde_json::json!([]),serde_json::json!({"var":"x"})]{
                let mut c=f["valid"][0].clone();c[key]=value;assert!(validate(&c,Some(&f["runtime"])).is_err(),"{key}: {c}");
            }
        }
        let mut c=f["valid"][0].clone();c["page"]=0.into();assert!(validate(&c,Some(&f["runtime"])).is_ok());
        c["page"]=3.into();assert!(validate(&c,Some(&f["runtime"])).is_err());
        c["page"]=4.into();c["w"]=320.into();c["commands"]=Value::Array(vec![serde_json::json!({"op":"line","x":0,"y":0,"x2":1,"y2":1,"color":0});7]);assert!(validate(&c,None).is_err());
        let v2:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/canvas-v2.json")).unwrap();
        for key in ["points","r","start","end","inner","color"]{
            for value in [Value::Null,serde_json::json!(true),serde_json::json!(-1),serde_json::json!(1.5),serde_json::json!(1e300),serde_json::json!({"var":"label"})]{
                for mut c in v2["valid"][0]["commands"].as_array().unwrap().iter().cloned(){
                    if c.get(key).is_none(){continue;}
                    c[key]=value.clone();let mut d=v2["valid"][0].clone();d["commands"]=Value::Array(vec![c]);
                    assert!(validate(&d,Some(&v2["runtime"])).is_err(),"{key}: {d}");
                }
            }
        }
    }
}
