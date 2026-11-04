//! Data-only bridge of the native installed-program controller. No providers,
//! filesystem, timers, grants or retry loop; preserve unknown outcomes.
use serde::Serialize;
use serde_json::{json,Value};
use std::collections::BTreeSet;
pub const ENGINE:&str="p4-program-background-v2";
pub const INSPECT:&str="p4-program-inspect-v1";
fn bad()->String{"后台程序状态未通过核对，现有数据保留".into()}
fn name(s:&str)->bool{!s.is_empty()&&s.len()<=47&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))}
pub fn valid_widget_id(s:&str)->bool{!s.is_empty()&&s.len()<=47&&s.as_bytes()[0].is_ascii_lowercase()&&s.bytes().all(|b|b.is_ascii_lowercase()||b.is_ascii_digit()||b"_-".contains(&b))}
fn boot(s:&str)->bool{s.len()==16&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))}
fn serial(s:&str)->bool{s.parse::<u64>().is_ok_and(|n|n>0&&n.to_string()==s)}
fn text<'a>(p:&'a Value,k:&str)->Result<&'a str,String>{p[k].as_str().ok_or_else(bad)}
fn boolean(p:&Value,k:&str)->Result<bool,String>{p[k].as_bool().ok_or_else(bad)}
fn uint(p:&Value,k:&str)->Result<u32,String>{p[k].as_u64().and_then(|n|u32::try_from(n).ok()).ok_or_else(bad)}
pub fn supported(caps:&Value)->bool{
    let known=|s:&str|matches!(s,"p4-component-program-v1"|"p4-component-program-v2");
    let Some(primary)=caps["widgetProgram"].as_str().filter(|s|known(s))else{return false;};
    if let Some(versions)=caps.get("widgetPrograms"){
        let Some(versions)=versions.as_array().filter(|a|!a.is_empty()&&a.len()<=2)else{return false;};
        let mut seen=BTreeSet::new();
        for version in versions{
            let Some(version)=version.as_str().filter(|s|known(s))else{return false;};
            if !seen.insert(version){return false;}
        }
        if !seen.contains(primary){return false;}
    }
    caps["widgetProgramBackground"]==ENGINE
      &&caps["widgetProgramInspect"]==INSPECT
      &&caps["widgetProgramBackgroundMaxActive"].as_u64().is_some_and(|n|(1..=4).contains(&n))
}
#[derive(Clone,Debug,Serialize)]#[serde(rename_all="camelCase")]
pub struct Entry{
    pub widget_id:String,pub registration:String,pub live:bool,pub stopped:bool,
    pub checkpoint_pending:bool,pub revision:u32,pub phase:u32,pub last_step_us:u32,pub max_step_us:u32,
}
#[derive(Clone,Debug,Serialize)]#[serde(rename_all="camelCase")]
pub struct Snapshot{pub boot_id:String,pub items:Vec<Entry>}
pub fn snapshot(p:&Value,board:&str,expected_boot:Option<&str>)->Result<Snapshot,String>{
    if !name(board)||p["boardDeviceId"]!=board||p["engine"]!=ENGINE||p["ok"]!=true{return Err(bad());}
    let epoch=text(p,"bootId")?;if !boot(epoch)||expected_boot.is_some_and(|b|b!=epoch){return Err(bad());}
    let rows=p["items"].as_array().filter(|a|a.len()<=4).ok_or_else(bad)?;
    let mut ids=BTreeSet::new();let mut serials=BTreeSet::new();let mut items=Vec::with_capacity(rows.len());
    for row in rows{
        let id=text(row,"widgetId")?;let registration=text(row,"registration")?;
        if !valid_widget_id(id)||!serial(registration)||!ids.insert(id)||!serials.insert(registration){return Err(bad());}
        let live=boolean(row,"live")?;let stopped=boolean(row,"stopped")?;
        let last=uint(row,"lastStepUs")?;let max=uint(row,"maxStepUs")?;let phase=uint(row,"phase")?;
        if (live&&stopped)||last>max||phase>u8::MAX as u32{return Err(bad());}
        items.push(Entry{widget_id:id.into(),registration:registration.into(),live,stopped,
          checkpoint_pending:boolean(row,"checkpointPending")?,revision:uint(row,"revision")?,phase,last_step_us:last,max_step_us:max});
    }
    Ok(Snapshot{boot_id:epoch.into(),items})
}
#[derive(Clone,Debug)]
pub struct Installed{pub widget_id:String,pub boot_id:String,sums:[String;3]}
pub fn installed(p:&Value,board:&str,id:&str)->Result<Installed,String>{
    let state=snapshot(p,board,None)?;let content=&p["installedContent"];
    if !valid_widget_id(id)||content["widgetId"]!=id{return Err(bad());}
    let mut sums=Vec::with_capacity(3);
    for key in ["widgetChecksum","buttonsChecksum","spritesChecksum"]{
        let s=text(content,key)?;if s.len()!=8||!s.bytes().all(|b|b.is_ascii_hexdigit()){return Err(bad());}sums.push(s.to_ascii_lowercase());
    }
    Ok(Installed{widget_id:id.into(),boot_id:state.boot_id,sums:sums.try_into().map_err(|_|bad())?})
}
pub fn start_payload(content:&Installed)->Value{
    json!({"widgetId":content.widget_id,"bootId":content.boot_id,"widgetChecksum":content.sums[0],"buttonsChecksum":content.sums[1],"spritesChecksum":content.sums[2]})
}
pub fn stop_payload(id:&str,epoch:&str,registration:&str)->Result<Value,String>{
    if !valid_widget_id(id)||!boot(epoch)||!serial(registration){return Err(bad());}
    Ok(json!({"widgetId":id,"bootId":epoch,"registration":registration}))
}
#[derive(Debug,Serialize)]#[serde(rename_all="camelCase")]
pub struct Outcome{pub confirmed:bool,pub code:&'static str,pub snapshot:Option<Snapshot>}
impl Outcome{
    pub fn unknown()->Self{Self{confirmed:false,code:"result_unknown_no_replay",snapshot:None}}
    pub fn started(p:&Value,board:&str,content:&Installed)->Self{
        match snapshot(p,board,Some(&content.boot_id)){
            Ok(s) if s.items.iter().any(|e|e.widget_id==content.widget_id&&e.live&&!e.stopped)=>Self{confirmed:true,code:"running",snapshot:Some(s)},
            _=>Self::unknown(),
        }
    }
    pub fn stopped(p:&Value,board:&str,id:&str,epoch:&str,registration:&str)->Self{
        match snapshot(p,board,Some(epoch)){
            Ok(s) if s.items.iter().any(|e|e.widget_id==id&&e.registration==registration&&!e.live&&e.stopped)=>Self{confirmed:true,code:"stopped",snapshot:Some(s)},
            _=>Self::unknown(),
        }
    }
}
#[cfg(test)]mod tests{
 use super::*;
 fn response()->Value{json!({"ok":true,"boardDeviceId":"board-test","engine":ENGINE,"bootId":"0123456789abcdef",
   "items":[{"widgetId":"focus","registration":"18446744073709551615","live":true,"stopped":false,"checkpointPending":true,"revision":9,"phase":1,"lastStepUs":110,"maxStepUs":210}],
   "installedContent":{"widgetId":"focus","widgetChecksum":"ffffffff","buttonsChecksum":"00000001","spritesChecksum":"00000000"}})}
 #[test]fn exact_boot_content_and_u64_serial_are_preserved_without_js_rounding(){
   let p=response();let content=installed(&p,"board-test","focus").unwrap();let payload=start_payload(&content);
   assert_eq!(payload["bootId"],p["bootId"]);assert_eq!(payload["widgetChecksum"],"ffffffff");assert_eq!(payload.as_object().unwrap().len(),5);
   assert!(Outcome::started(&p,"board-test",&content).confirmed);
   assert!(snapshot(&p,"another-board",None).is_err());assert!(snapshot(&p,"board-test",Some("1123456789abcdef")).is_err());
   for invalid in ["0","00","01","-1","+1","1 ","18446744073709551616"]{assert!(stop_payload("focus","0123456789abcdef",invalid).is_err());}
   assert_eq!(stop_payload("focus","0123456789abcdef","18446744073709551615").unwrap()["registration"],"18446744073709551615");
 }
 #[test]fn malformed_or_partial_state_never_confirms_or_reflects_device_extensions(){
   for (path,value) in [("/items/0/revision",json!(1.5)),("/items/0/maxStepUs",json!(109)),("/items/0/phase",json!(256)),("/items/0/stopped",json!(true)),("/items/0/live",json!(1)),("/bootId",json!("0123456789abcdeF")),("/ok",json!(false))]{
     let mut p=response();*p.pointer_mut(path).unwrap()=value;assert!(snapshot(&p,"board-test",None).is_err(),"{path}");
   }
   let mut p=response();let duplicate=p["items"][0].clone();p["items"].as_array_mut().unwrap().push(duplicate);assert!(snapshot(&p,"board-test",None).is_err());
   let mut p=response();p["apiKey"]=json!("do-not-reflect");p["items"][0]["privatePath"]=json!("do-not-reflect");
   let sanitized=serde_json::to_string(&snapshot(&p,"board-test",None).unwrap()).unwrap();assert!(!sanitized.contains("do-not-reflect"));
   p["ok"]=json!(false);p["error"]=json!("do-not-reflect");assert!(!snapshot(&p,"board-test",None).unwrap_err().contains("do-not-reflect"));
 }
 #[test]fn stop_confirms_only_same_owner_and_pending_is_not_disk_durability(){
   let mut p=response();p["items"][0]["live"]=json!(false);p["items"][0]["stopped"]=json!(true);
   let o=Outcome::stopped(&p,"board-test","focus","0123456789abcdef","18446744073709551615");assert!(o.confirmed);assert!(o.snapshot.unwrap().items[0].checkpoint_pending);
   assert!(!Outcome::stopped(&p,"board-test","focus","0123456789abcdef","1").confirmed);
   assert!(!Outcome::stopped(&p,"board-test","focus","1123456789abcdef","18446744073709551615").confirmed);
 }
 #[test]fn gates_are_closed_for_legacy_or_missing_inspection(){
   let mut caps=json!({"widgetProgram":"p4-component-program-v1","widgetProgramBackground":ENGINE,"widgetProgramInspect":INSPECT,"widgetProgramBackgroundMaxActive":1});
   assert!(supported(&caps));caps["widgetProgramInspect"]=Value::Null;assert!(!supported(&caps));assert!(!supported(&Value::Null));
   assert!(valid_widget_id(&"a".repeat(47)));assert!(!valid_widget_id(&"a".repeat(48)));
   for invalid in ["Upper","has.dot","1first","../file",""]{assert!(!valid_widget_id(invalid));}
 }
 #[test]fn r158_v2_negotiation_accepts_only_known_consistent_scalars_and_lists(){
   // Relevant projection of the actual r158 hello; no device/serial I/O.
   let caps=json!({"widgetProgram":"p4-component-program-v2","widgetPrograms":["p4-component-program-v1","p4-component-program-v2"],
     "widgetProgramBackground":ENGINE,"widgetProgramInspect":INSPECT,"widgetProgramBackgroundMaxActive":1,
     "widgetProgramMaxFuel":4096,"widgetProgramAggregateFuel":4096,"widgetProgramParent":"p4-program-parent-v1"});
   assert!(supported(&caps));
   let mut scalar=caps.clone();scalar.as_object_mut().unwrap().remove("widgetPrograms");assert!(supported(&scalar));
   let mut v1=caps.clone();v1["widgetProgram"]=json!("p4-component-program-v1");assert!(supported(&v1));
   for (key,value) in [("widgetProgram",Value::Null),("widgetProgram",json!(["p4-component-program-v1","p4-component-program-v2"])),
     ("widgetProgram",json!("p4-component-program-v3")),("widgetPrograms",Value::Null),("widgetPrograms",json!([])),
     ("widgetPrograms",json!("p4-component-program-v2")),("widgetPrograms",json!(["p4-component-program-v1"])),
     ("widgetPrograms",json!(["p4-component-program-v2","p4-component-program-v2"])),
     ("widgetPrograms",json!(["p4-component-program-v2","p4-component-program-v3"])),("widgetPrograms",json!([true,"p4-component-program-v2"])),
     ("widgetProgramBackground",json!("p4-program-background-v1")),("widgetProgramInspect",Value::Null),
     ("widgetProgramBackgroundMaxActive",json!(0)),("widgetProgramBackgroundMaxActive",json!(5)),
     ("widgetProgramBackgroundMaxActive",json!("1")),("widgetProgramBackgroundMaxActive",json!(1.5))]{
       let mut invalid=caps.clone();invalid[key]=value;assert!(!supported(&invalid),"{key}: {}",invalid[key]);
   }
 }
}
