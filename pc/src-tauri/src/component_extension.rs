//! Declarative PC companions. No package code, filesystem access, or raw IPC.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;

#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Extension {
    pub schema_version: u8,
    pub engine: String,
    pub permissions: Permissions,
    pub search: Search,
    pub panel: Panel,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub discovery: Option<Discovery>,
}
/// Package-owned public catalogue policy; no executable provider code.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Discovery {
    pub endpoints: Vec<String>,
    pub location_endpoint: String,
    pub city_field: String,
    pub country_field: String,
    pub country: String,
    pub aliases: BTreeMap<String, String>,
    pub station_region: String,
    pub popularity: String,
    pub votes: String,
    pub national_count: usize,
    pub local_supplement: usize,
    pub refresh_sec: u64,
    pub auto_play_on_enter: bool,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Permissions {
    pub network_hosts: Vec<String>,
    pub public_audio: bool,
    pub storage: bool,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Search {
    pub endpoint: String,
    pub query_param: String,
    /// Optional, package-owned discovery query. Never plays audio automatically.
    pub browse_query: Option<String>,
    pub params: BTreeMap<String, String>,
    pub items_path: String,
    pub fields: Fields,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Fields {
    pub id: String,
    pub title: String,
    pub url: String,
    pub fallback_url: Option<String>,
    pub homepage: Option<String>,
    pub codec: Option<String>,
    pub hls: Option<String>,
    pub available: Option<String>,
}
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Panel {
    pub title: String,
    pub description: String,
    pub search_placeholder: String,
    pub attribution: String,
    pub attribution_url: String,
    pub sections: Vec<String>,
}
fn short(s: &str, max: usize) -> bool {
    !s.trim().is_empty() && s.len() <= max && !s.chars().any(char::is_control)
}
fn pointer(s: &str) -> bool {
    s.len() <= 128 && (s.is_empty() || s.starts_with('/')) && !s.chars().any(char::is_control)
}
pub fn decode_surface_image(im:&Value,bytes:&[u8])->Result<image::RgbaImage,String>{
    use image::ImageDecoder;
    let jpeg=im["encoding"]=="jpeg";
    if bytes.len()>if jpeg {1024*1024}else{128*1024}{return Err("PNG 超过资源大小限制".into());}
    let decoder=image::codecs::png::PngDecoder::new(std::io::Cursor::new(bytes)).map_err(|e|e.to_string())?;
    let (w,h)=decoder.dimensions();
    if w as u64!=im["frame_width"].as_u64().unwrap_or(0)||h as u64!=im["frame_height"].as_u64().unwrap_or(0)||(if jpeg {(w,h)!=(640,480)}else{w>512||h>240||w*h>98304}){
        return Err("PNG 尺寸与 surface 声明不符".into());
    }
    let rgba=image::DynamicImage::from_decoder(decoder).map(|i|i.to_rgba8()).map_err(|e|e.to_string())?;
    if jpeg&&rgba.pixels().any(|p|p[3]!=255){return Err("全屏 JPEG 背景必须不透明".into());}Ok(rgba)
}
pub fn validate_surface(s: &Value) -> Result<(), String> {
    if s["engine"]=="p4-app-surface-v1" { return validate_app_surface(s,None); }
    let error = || "设备媒体布局格式无效或超过安全边界".to_string();
    let num = |o: &Value, k: &str, lo: u64, hi: u64| {
        o[k].as_u64()
            .filter(|n| *n >= lo && *n <= hi)
            .ok_or_else(error)
    };
    let keys = |o: &Value, allowed: &[&str]| {
        o.as_object()
            .is_some_and(|m| m.keys().all(|k| allowed.contains(&k.as_str())))
    };
    let touch=s.get("input").is_some();
    if touch&&s["input"]!="p4-media-touch-v1"{return Err(error());}
    if !keys(s, &["background", "elements", "images","input"]) {
        return Err(error());
    }
    num(s, "background", 0, 65535)?;
    let nodes = s["elements"]
        .as_array()
        .filter(|a| !a.is_empty() && a.len() <= if touch{32}else{20})
        .ok_or_else(error)?;
    let mut image_ids=std::collections::HashMap::new();
    let mut pixels=0;
    if let Some(images)=s.get("images"){
        let a=images.as_array().filter(|a|!a.is_empty()&&a.len()<=4).ok_or_else(error)?;
        for im in a {
            if !keys(im,&["id","asset","frame_width","frame_height","frames","fps"]){return Err(error());}
            let id=im["id"].as_str().filter(|id|!id.is_empty()&&id.len()<=15&&id.bytes().all(|c|c.is_ascii_lowercase()||c.is_ascii_digit()||c==b'_'||c==b'-')).ok_or_else(error)?;
            let asset=im["asset"].as_str().ok_or_else(error)?;
            let name=asset.strip_prefix("assets/").ok_or_else(error)?;
            if !name.ends_with(".png")||name.len()<5||name.contains("..")||!name.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c)){return Err(error());}
            let w=num(im,"frame_width",8,512)?;let h=num(im,"frame_height",8,240)?;
            num(im,"frames",1,1)?;num(im,"fps",1,1)?;
            if image_ids.insert(id,(w,h)).is_some(){return Err(error());}
            pixels+=w*h;if pixels>98304{return Err(error());}
        }
    }
    for n in nodes {
        if !keys(
            n,
            &[
                "type", "x", "y", "w", "h", "color", "page", "scale", "radius", "bind", "text", "image", "rotate_ms","action",
            ],
        ) {
            return Err(error());
        }
        if n.get("action").is_some()&&(!touch||!matches!(n["action"].as_str(),Some("media.toggle"|"media.queue"|"media.previous"|"media.next"|"media.up"|"media.down"|"media.select"))){return Err(error());}
        if !matches!(
            n["type"].as_str(),
            Some("panel" | "text" | "meter" | "ticks" | "list" | "transport" | "image")
        ) {
            return Err(error());
        }
        if n["type"]!="image"&&(n.get("image").is_some()||n.get("rotate_ms").is_some()){return Err(error());}
        let x = num(n, "x", 0, 639)?;
        let y = num(n, "y", 0, 479)?;
        let w = num(n, "w", 1, 640 - x)?;
        let h = num(n, "h", 1, 480 - y)?;
        num(n, "color", 0, 65535)?;
        if n["type"]=="image" {
            let id=n["image"].as_str().ok_or_else(error)?;
            let (iw,ih)=image_ids.get(id).ok_or_else(error)?;
            if n.get("rotate_ms").is_some(){num(n,"rotate_ms",8000,60000)?;if w!=h||w>320||iw!=ih{return Err(error());}}
        }
        let scale = if n.get("scale").is_some() {
            num(n, "scale", 1, 3)?
        } else {
            1
        };
        if n.get("page").is_some() {
            num(n, "page", 0, 2)?;
        }
        if n.get("radius").is_some() {
            num(n, "radius", 0, 24.min(w / 2).min(h / 2))?;
        }
        if n.get("bind").is_some()
            && !matches!(
                n["bind"].as_str(),
                Some(
                    "title"
                        | "subtitle"
                        | "status"
                        | "message"
                        | "volume"
                        | "index"
                        | "count"
                        | "level"
                )
            )
        {
            return Err(error());
        }
        if let Some(t) = n.get("text") {
            if t.as_str()
                .is_none_or(|s| s.len() > 120 || s.chars().any(char::is_control))
            {
                return Err(error());
            }
        }
        if n["type"] == "text"
            && ((n.get("text").is_some() == n.get("bind").is_some()) || h < 32 * scale)
        {
            return Err(error());
        }
        if n["type"] == "meter" && !matches!(n["bind"].as_str(), Some("volume" | "level")) {
            return Err(error());
        }
        if n["type"] == "list" && h < 48 {
            return Err(error());
        }
    }
    Ok(())
}

pub fn validate_app_persistence(runtime:&Value)->Result<(),String>{
    let Some(p)=runtime.get("persist") else{return Ok(());};
    let error=||"组件存档声明无效，需要变量子集及完整安全恢复映射".to_string();
    if runtime["surface"]["engine"]!="p4-app-surface-v1" || !p.as_object().is_some_and(|m|m.keys().all(|k|["version","vars","states","page"].contains(&k.as_str()))&&m.contains_key("version")&&m.contains_key("vars")&&m.contains_key("states")){return Err(error());}
    if let Some(page)=p.get("page"){if !page.is_boolean()||(page==true&&!runtime["pages"].as_array().is_some_and(|a|!a.is_empty())){return Err(error());}}
    if !p["version"].as_u64().is_some_and(|n|(1..=65535).contains(&n)){return Err(error());}
    let vars=p["vars"].as_array().filter(|a|!a.is_empty()&&a.len()<=8).ok_or_else(error)?;
    let mut unique=std::collections::HashSet::new();
    for v in vars {let name=v.as_str().ok_or_else(error)?;if runtime["vars"][name]["type"]!="int"||!unique.insert(name){return Err(error());}}
    let states=runtime["states"].as_array().ok_or_else(error)?;
    let mapping=p["states"].as_object().filter(|m|m.len()==states.len()).ok_or_else(error)?;
    for state in states{let key=state.as_str().ok_or_else(error)?;let value=mapping.get(key).ok_or_else(error)?;if !states.contains(value){return Err(error());}}
    Ok(())
}

fn validate_motion_runtime(r:&Value)->Result<(),String>{
    let bad=||"连续运动需要有界参数、静态平台、落地角色及独立的按住蓄力动作".to_string();
    let rules=r["scene"]["rules"].as_array().map(Vec::as_slice).unwrap_or(&[]);
    let Some(m)=r["scene"].get("motion") else {
        if rules.iter().any(|x|x["on"]=="landed"||x["do"].as_array().is_some_and(|a|a.iter().any(|o|o["op"]=="charge"))){return Err(bad());}
        return Ok(());
    };
    #[derive(Deserialize)] #[serde(deny_unknown_fields)]
    struct Motion {engine:String,body:String,platforms:Vec<String>,gravity:u8,lift:u8,speed:[u8;2],charge_ms:u16,charge_var:String,track:Option<Track>}
    #[derive(Deserialize)] #[serde(deny_unknown_fields)]
    struct Track {engine:String,camera_x:u8,gap:[u8;2],width:[u8;2],tones:Vec<u8>}
    if m.get("track").is_some_and(Value::is_null){return Err(bad());}
    let m:Motion=serde_json::from_value(m.clone()).map_err(|_|bad())?;
    let es=r["scene"]["entities"].as_array().ok_or_else(bad)?;
    let body=es.iter().find(|e|e["id"]==m.body).ok_or_else(bad)?;
    let n=|v:&Value,k:&str,default:i64|v[k].as_i64().unwrap_or(default);
    let platforms:Vec<_>=m.platforms.iter().filter_map(|p|es.iter().find(|e|e["id"]==*p)).collect();
    let hold=r["input"]["hold"].as_str().filter(|s|!s.is_empty()).ok_or_else(bad)?;
    if m.engine!="p4-motion-v1"||!(4..=64).contains(&m.gravity)||!(2..=16).contains(&m.lift)
        ||!(300..=3000).contains(&m.charge_ms)||m.speed[0]<1||m.speed[1]<m.speed[0]||m.speed[1]>24
        ||platforms.len()!=m.platforms.len()||!(1..=11).contains(&platforms.len())
        ||m.platforms.iter().collect::<std::collections::HashSet<_>>().len()!=platforms.len()||m.platforms.contains(&m.body)
        ||r["vars"][&m.charge_var]["type"]!="int"||r["scene"]["score_var"]==m.charge_var||r["lifecycle"]["record_var"]==m.charge_var
        ||r["scene"].get("sprites").is_some()||r["scene"]["grid"].get("solid_tones").is_some()
        ||r["input"].get("drag").is_some()||es.iter().any(|e|n(e,"vx",0)!=0||n(e,"vy",0)!=0)
        ||body["active"]==false||!platforms.iter().any(|p|p["active"]!=false&&n(body,"y",0)+n(body,"height",1)==n(p,"y",0)
            &&2*n(p,"x",0)<=2*n(body,"x",0)+n(body,"width",1)&&2*n(body,"x",0)+n(body,"width",1)<2*(n(p,"x",0)+n(p,"width",1))){return Err(bad());}
    if let Some(t)=&m.track {
        if t.engine!="p4-platform-track-v1"||!(4..=8).contains(&platforms.len())||es.len()!=platforms.len()+1
          ||t.camera_x<2||i64::from(t.camera_x)>n(&r["scene"]["grid"],"width",0)/2
          ||t.gap[0]<1||t.gap[1]<t.gap[0]||t.gap[1]>6||t.width[0]<2||t.width[1]<t.width[0]||t.width[1]>4
          ||(u32::from(t.gap[1])+u32::from(t.width[1]))*u32::from(m.gravity)>2*u32::from(m.lift)*u32::from(m.speed[1])
          ||!(2..=4).contains(&t.tones.len())||t.tones.iter().any(|v|!(1..=4).contains(v))
          ||t.tones.iter().collect::<std::collections::HashSet<_>>().len()!=t.tones.len()
          ||platforms.iter().any(|p|p["active"]==false||n(p,"y",0)!=n(body,"y",0)+n(body,"height",1)){return Err(bad());}
    }
    let mut charge=false;
    for rule in rules {
        if matches!(rule["on"].as_str(),Some("tick"|"collision"|"blocked")){return Err(bad());}
        if rule["on"]=="landed"&&(rule["entity"]!=m.body||!m.platforms.iter().any(|p|rule["with"]==*p)){return Err(bad());}
        for op in rule["do"].as_array().ok_or_else(bad)? {
            if matches!(op["op"].as_str(),Some("move"|"velocity"|"accelerate"|"bounce")){return Err(bad());}
            if m.track.is_some()&&matches!(op["op"].as_str(),Some("place"|"show"|"hide")){return Err(bad());}
            if op["op"]=="charge"{if op["entity"]!=m.body||rule["on"]!=hold{return Err(bad());}charge=true;}
        }
    }
    if !charge{return Err(bad());}Ok(())
}

pub fn validate_app_runtime(runtime:&Value)->Result<(),String>{
    validate_local_tools(runtime)?;
    validate_app_persistence(runtime)?;
    validate_motion_runtime(runtime)?;
    if runtime["transitions"].as_array().is_some_and(|ts|ts.len()>if runtime.get("input").is_some(){24}else{12}){
        return Err("组件状态转换超出预算".into());
    }
    if let Some(input)=runtime.get("input"){
        let bad=||"组件输入策略必须使用 p4-input-v1 及已声明的有界动作".to_string();
        let p=input.as_object().ok_or_else(bad)?;
        if input["engine"]!="p4-input-v1" || p.keys().any(|k|!["engine","repeat","press","swipe","confirm","drag","cooldown","hold"].contains(&k.as_str())){return Err(bad());}
        let transitions=runtime["transitions"].as_array().ok_or_else(bad)?;
        let known=|v:&Value|v.as_str().is_some_and(|s|transitions.iter().any(|t|t["on"]==s));
        if let Some(h)=p.get("hold"){
            if !known(h)||runtime["scene"]["motion"]["engine"]!="p4-motion-v1"||p.contains_key("drag")
                ||["repeat","press"].iter().any(|k|p.get(*k).and_then(Value::as_array).is_some_and(|a|a.contains(h)))
                ||["confirm","cooldown"].iter().any(|k|p.get(*k).and_then(Value::as_object).is_some_and(|o|o.contains_key(h.as_str().unwrap_or("")))){return Err(bad());}
        }
        for key in ["repeat","press"]{
            if let Some(v)=p.get(key){
                let a=v.as_array().filter(|a|a.len()<=8).ok_or_else(bad)?;
                let mut seen=std::collections::HashSet::new();
                for v in a{
                    if !known(v)||!seen.insert(v.as_str().unwrap()){return Err(bad());}
                    if key=="press" && !(runtime.get("instrument").is_some() && v=="instrument.strike")
                        && (runtime["game"]["actions"].get("flap").is_none()||v!=&runtime["game"]["actions"]["flap"]){return Err(bad());}
                }
            }
        }
        if let Some(s)=p.get("swipe"){
            let s=s.as_object().ok_or_else(bad)?;
            if s.iter().any(|(k,v)|!["left","right","up","down"].contains(&k.as_str())||!known(v)){return Err(bad());}
        }
        if let Some(c)=p.get("confirm"){
            let c=c.as_object().filter(|c|c.len()<=3).ok_or_else(bad)?;
            if c.iter().any(|(k,v)|!known(&Value::String(k.clone()))||!v.as_str().is_some_and(|s|!s.is_empty()&&s.len()<=120)){return Err(bad());}
        }
        if let Some(c)=p.get("cooldown"){
            let c=c.as_object().filter(|c|c.len()<=3).ok_or_else(bad)?;
            if c.iter().any(|(k,v)|!known(&Value::String(k.clone()))||!v.as_u64().is_some_and(|n|(250..=60000).contains(&n)))
                ||!c.is_empty()&&(runtime["host"]==true||runtime.get("media").is_some()||runtime.get("instrument").is_some()){return Err(bad());}
        }
        if let Some(d)=p.get("drag"){
            if !d.as_object().is_some_and(|o|o.len()==2&&o.contains_key("entity")&&o.contains_key("axis"))
                ||!matches!(d["axis"].as_str(),Some("x"|"y"|"xy"))||!d["entity"].as_str().is_some_and(|s|!s.is_empty()&&s.len()<=15)
                ||runtime["surface"]["scene_view"]["engine"]!="p4-scene-surface-v1"
                ||!runtime["scene"]["entities"].as_array().is_some_and(|es|es.iter().any(|e|e["id"]==d["entity"]&&e.get("bounds").is_none_or(|b|b=="clamp"))){return Err(bad());}
        }
    }
    let error=||"应用运行时的变量范围或赋值无效".to_string();
    if let Some(life)=runtime.get("lifecycle"){
        let bad=||"游戏生命周期必须声明有效暂停动作及独立的已保存成绩变量".to_string();
        let g=runtime.get("game").or_else(||runtime.get("scene")).ok_or_else(bad)?;
        let active=g.get("playing_state").or_else(||g.get("active_state")).ok_or_else(bad)?;
        let result=g.get("result_state").ok_or_else(bad)?;
        let pause=life["pause_action"].as_str().filter(|s|!s.is_empty()).ok_or_else(bad)?;
        if !life.as_object().is_some_and(|o|o.keys().all(|k|matches!(k.as_str(),"pause_action"|"record_var")))
            ||!runtime["transitions"].as_array().is_some_and(|ts|ts.iter().any(|t|
                t["on"]==pause && &t["from"]==active && t.get("to").is_some_and(|to|
                    to!=active && to!=result && runtime["states"].as_array().is_some_and(|s|s.contains(to))
                    && ts.iter().any(|back|back["on"]==pause && &back["from"]==to && &back["to"]==active)))){return Err(bad());}
        if let Some(record)=life.get("record_var"){
            let name=record.as_str().ok_or_else(bad)?;
            if runtime["vars"][name]["type"]!="int"||record==&g["score_var"]
                ||!runtime["persist"]["vars"].as_array().is_some_and(|s|s.contains(record)){return Err(bad());}
        }
    }
    let app=runtime["surface"]["engine"]=="p4-app-surface-v1";
    if let Some(limits)=runtime.get("limits"){
        if !app{return Err(error());}let limits=limits.as_object().ok_or_else(error)?;
        for (name,range) in limits{
            let a=range.as_array().filter(|a|a.len()==2).ok_or_else(error)?;
            let lo=a[0].as_i64().ok_or_else(error)?;let hi=a[1].as_i64().ok_or_else(error)?;
            let init=runtime["vars"][name]["init"].as_i64().unwrap_or(0);
            if runtime["vars"][name]["type"]!="int"||lo < -1_000_000_000||hi > 1_000_000_000||lo>init||hi<init{return Err(error());}
        }
    }
    if !app{return Ok(());}
    let rules=runtime["transitions"].as_array().into_iter().flatten().chain(runtime["tick"].as_array().into_iter().flatten()).chain(runtime["tick"].as_array().into_iter().flatten().filter_map(|t|t.get("then")));
    for rule in rules{for op in ["set","inc"]{if let Some(values)=rule.get(op){
        for (name,value) in values.as_object().ok_or_else(error)?{
            let kind=runtime["vars"][name]["type"].as_str().ok_or_else(error)?;
            if value.is_object()&&op=="set"{
                let source=value["var"].as_str().ok_or_else(error)?;
                if value.as_object().unwrap().len()!=1||kind!="int"||runtime["vars"][source]["type"]!="int"{return Err(error());}
            }else if kind=="int"{
                if !value.as_i64().is_some_and(|n|(-1_000_000_000..=1_000_000_000).contains(&n)){return Err(error());}
            }else if kind!="string"||op!="set"||!value.as_str().is_some_and(|s|s.len()<=63){return Err(error());}
        }
    }}}
    Ok(())
}

fn validate_local_tools(r:&Value)->Result<(),String>{
    let Some(c)=r.get("local") else{return Ok(())};
    let check=||->Option<()>{
        let keys=|v:&Value,names:&[&str]|v.as_object().is_some_and(|o|o.len()==names.len()&&o.keys().all(|k|names.contains(&k.as_str())));
        let text=|v:&Value,n:usize|v.as_str().is_some_and(|s|!s.is_empty()&&s.len()<n&&s.bytes().all(|b|b>=32&&b!=127));
        if !keys(c,&["engine","kind","groups","actions","reveal_ms","idle","busy","hint"])||c["engine"]!="p4-local-tools-v1"{return None;}
        let deck=c["kind"]=="deck";if !deck&&c["kind"]!="countdown"{return None;}
        if !(300..=2000).contains(&c["reveal_ms"].as_i64()?)||!["idle","busy","hint"].iter().all(|k|text(&c[k],145)){return None;}
        let groups=c["groups"].as_array()?;if !(1..=3).contains(&groups.len()){return None;}
        for g in groups{
            if !text(&g["title"],31){return None;}
            if deck{
                if !keys(g,&["title","items"]){return None;}let items=g["items"].as_array()?;
                if !(2..=16).contains(&items.len()){return None;}
                let mut titles=std::collections::HashSet::new();
                for i in items{if !keys(i,&["title","note"])||!text(&i["title"],61)||!text(&i["note"],145)||!titles.insert(i["title"].as_str()?){return None;}}
            }else if !keys(g,&["title","kind"])||!matches!(g["kind"].as_str(),Some("weekdays"|"weekend"|"date")){return None;}
        }
        let actions=c["actions"].as_object()?;if !(2..=8).contains(&actions.len()){return None;}
        let mut ops=std::collections::HashSet::new();
        for (k,v) in actions{if k.is_empty()||k.len()>47||!k.bytes().all(|b|b.is_ascii_lowercase()||b.is_ascii_digit()||b"._-".contains(&b))||!ops.insert(v.as_str()?){return None;}}
        if deck{if !ops.contains("draw")||ops.iter().any(|o|!["draw","exclude","reset","close","previous","next"].contains(o))||ops.contains("exclude")&&!ops.contains("reset")||groups.len()>1&&(!ops.contains("previous")||!ops.contains("next")){return None;}}
        else if ops.len()!=6||!["previous","next","edit","cancel","up","down"].iter().all(|o|ops.contains(o)){return None;}
        let states=["ready","busy","result","editing","empty","offline"];
        if r["states"]!=serde_json::json!(states)||r["initial_state"]!="ready"||r["surface"]["engine"]!="p4-app-surface-v1"||r["input"]["engine"]!="p4-input-v1"||["game","scene","host","media","instrument","data","pages"].iter().any(|k|r.get(k).is_some())||r.get("tick").is_some_and(|v|v.as_array().is_none_or(|a|!a.is_empty())){return None;}
        let vars:Vec<String>=(0..7).map(|i|format!("local{i}")).collect();let v=r["vars"].as_object()?;
        if v.len()!=7||!vars.iter().all(|k|v.get(k).is_some_and(|v|v["type"]=="int")){return None;}
        let saved:serde_json::Map<String,Value>=states.iter().map(|s|(s.to_string(),Value::String("ready".into()))).collect();
        if r["persist"]!=serde_json::json!({"version":1,"vars":vars,"states":saved}){return None;}
        let ts=r["transitions"].as_array()?;let mut seen=std::collections::HashSet::new();
        if ts.len()!=actions.len(){return None;}
        for t in ts{let on=t["on"].as_str()?;if !keys(t,&["on","from"])||t["from"]!="*"||!actions.contains_key(on)||!seen.insert(on){return None;}}
        Some(())
    };
    check().ok_or_else(||"离线工具必须遵循 p4-local-tools-v1 的内容池、日期和存档契约".into())
}
/// Generic tool layouts share image/geometry budgets with media, but bind only
/// to bounded dashboard slots. No arbitrary object paths or expressions.
pub fn validate_app_surface(s:&Value,runtime:Option<&Value>)->Result<(),String>{
    if s.get("graphics").is_some(){return validate_app_graphics(s,runtime);}
    let fail=||"通用应用布局、页面、动作或数据绑定无效".to_string();
    if s["engine"]!="p4-app-surface-v1"{return Err(fail());}
    let mut normalized=s.clone();normalized.as_object_mut().ok_or_else(fail)?.remove("engine");
    let nodes=normalized["elements"].as_array_mut().ok_or_else(fail)?;
    for n in nodes {
        if !matches!(n["type"].as_str(),Some("panel"|"text"|"meter"|"image"))||n.get("rotate_ms").is_some(){return Err(fail());}
        for key in ["opacity","opacity_end"]{
            if let Some(v)=n.as_object_mut().ok_or_else(fail)?.remove(key){
                if n["type"]!="panel"||!v.as_u64().is_some_and(|v|v<=255){return Err(fail());}
            }
        }
        if let Some(b)=n.get("bind"){
            if !matches!(b.as_str(),Some("title"|"eyebrow"|"headline"|"metricLabel"|"metricValue"|"metricUnit"|"badge"|"note"|"footer"|"progress")){return Err(fail());}
            if n["type"]=="meter"&&b!="progress"{return Err(fail());}
            n["bind"]=serde_json::json!(if n["type"]=="meter"{"level"}else{"title"});
        }
        if let Some(p)=n.get("page"){
            let page=p.as_u64().filter(|p|*p<=4).ok_or_else(fail)?;
            if let Some(r)=runtime{let count=r["pages"].as_array().map_or(1,|p|p.len().max(1));if page!=4&&page as usize>=count{return Err(fail());}}
            n["page"]=serde_json::json!(if page==4{2}else{page.min(1)});
        }
        if let Some(a)=n.as_object_mut().ok_or_else(fail)?.remove("action"){
            let action=a.as_str().filter(|a|!a.is_empty()&&a.len()<=47&&a.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if let Some(r)=runtime{if !r["transitions"].as_array().is_some_and(|rules|rules.iter().any(|r|r["on"]==action)){return Err(fail());}}
        }
        if let Some(m)=n.as_object_mut().ok_or_else(fail)?.remove("motion"){
            let y=n["y"].as_u64().ok_or_else(fail)?;let h=n["h"].as_u64().ok_or_else(fail)?;
            if m!="float"||n["type"]!="image"||y<4||y.saturating_add(h)>476{return Err(fail());}
        }
    }
    validate_surface(&normalized)
}
fn validate_app_graphics(s:&Value,runtime:Option<&Value>)->Result<(),String>{
    let fail=||"应用图形需要受控图标、字号、按钮与 640×480 静态背景".to_string();
    if s["graphics"]!="p4-app-graphics-v1"||s["engine"]!="p4-app-surface-v1"{return Err(fail());}
    let icons=["home","leaf","desk","agent","compass","sun","mic","book","sound","spark","heart","pause","play","check","plus","list","arrow","cloud","clock","stretch"];
    let mut normalized=s.clone();normalized.as_object_mut().ok_or_else(fail)?.remove("graphics");
    if let Some(v)=normalized.as_object_mut().unwrap().remove("scene_view"){
        if ["game_view","data_view"].iter().any(|k|s.get(k).is_some())||!v.as_object().is_some_and(|o|o.len()==7&&o.keys().all(|k|["engine","x","y","w","h","colors","sprites"].contains(&k.as_str())))||v["engine"]!="p4-scene-surface-v1"{return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=64&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=64&&h<=480-y)||!v["colors"].as_array().is_some_and(|a|a.len()==5&&a.iter().all(|c|c.as_u64().is_some_and(|v|v<=65535))){return Err(fail());}
        if let Some(r)=runtime{if r.get("scene").is_none()||r["scene"]["engine"]=="p4-touch-slice-v1"||["host","data","game","media","instrument"].iter().any(|k|r.get(k).is_some())||r["scene"].get("sprites").is_some()||r["scene"]["entities"].as_array().is_some_and(|a|a.iter().any(|e|e.get("sprite").is_some()))||["rows","solid_tones"].iter().any(|k|r["scene"]["grid"].get(k).is_some()){return Err(fail());}}
        let mut seen=std::collections::HashSet::new();
        for sp in v["sprites"].as_array().filter(|a|a.len()<=4).ok_or_else(fail)?{
            if !sp.as_object().is_some_and(|o|o.len()==4&&o.keys().all(|k|["entity","image","columns","fps"].contains(&k.as_str()))){return Err(fail());}
            let entity=sp["entity"].as_str().filter(|s|!s.is_empty()&&s.len()<=15&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if !seen.insert(entity)||runtime.is_some_and(|r|!r["scene"]["entities"].as_array().is_some_and(|a|a.iter().any(|e|e["id"]==entity))){return Err(fail());}
            let im=s["images"].as_array().and_then(|a|a.iter().find(|im|im["id"]==sp["image"])).ok_or_else(fail)?;
            let columns=sp["columns"].as_u64().filter(|n|(1..=8).contains(n)).ok_or_else(fail)?;
            if im.get("encoding").is_some()||!im["frame_width"].as_u64().is_some_and(|w|w%columns==0&&w/columns>=8)||!sp["fps"].as_u64().is_some_and(|n|(1..=20).contains(&n)){return Err(fail());}
        }
    }
    if let Some(v)=normalized.as_object_mut().unwrap().remove("data_view"){
        if s.get("game_view").is_some()||!v.as_object().is_some_and(|o|o.len()==7&&o.keys().all(|k|["engine","x","y","w","h","layout","colors"].contains(&k.as_str())))||v["engine"]!="p4-data-surface-v1"||!matches!(v["layout"].as_str(),Some("todos"|"metrics")){return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=320&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=300&&h<=480-y)||!v["colors"].as_array().is_some_and(|a|a.len()==5&&a.iter().all(|c|c.as_u64().is_some_and(|v|v<=65535))){return Err(fail());}
        if let Some(r)=runtime{if r.get("data").is_none()||["host","scene","game","media","instrument"].iter().any(|k|r.get(k).is_some()){return Err(fail());}}
    }
    if let Some(v)=normalized.as_object_mut().unwrap().remove("game_view"){
        if !v.as_object().is_some_and(|o|o.len()==5&&o.keys().all(|k|["engine","x","y","w","h"].contains(&k.as_str())))||v["engine"]!="p4-game-surface-v1"{return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=64&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=64&&h<=480-y){return Err(fail());}
        if let Some(r)=runtime{if !matches!(r["game"]["type"].as_str(),Some("snake"|"blocks"|"flappy"))||["host","scene","data","media","instrument"].iter().any(|k|r.get(k).is_some()){return Err(fail());}}
    }
    let typography=s.get("typography").is_some();
    if typography&&s["typography"]!="p4-app-type-v1"{return Err(fail());}
    let interaction=s.get("interaction").is_some();
    if interaction&&(!typography||s["interaction"]!="p4-app-interaction-v1"){return Err(fail());}
    normalized.as_object_mut().unwrap().remove("interaction");
    normalized.as_object_mut().unwrap().remove("typography");
    if let Some(fonts)=normalized.as_object_mut().unwrap().remove("fonts"){
        if !typography{return Err(fail());}
        let fonts=fonts.as_array().filter(|a|!a.is_empty()&&a.len()<=2).ok_or_else(fail)?;
        let mut seen=std::collections::HashSet::new();let mut images_seen=std::collections::HashSet::new();
        for f in fonts{
            if !f.as_object().is_some_and(|o|(3..=4).contains(&o.len())&&o.keys().all(|k|["image","chars","metrics","cell"].contains(&k.as_str()))){return Err(fail());}
            let (cw,ch)=if let Some(cell)=f.get("cell"){
                let a=cell.as_array().filter(|a|a.len()==2).ok_or_else(fail)?;
                let cw=a[0].as_u64().filter(|v|[16,32,48,64].contains(v)).ok_or_else(fail)?;
                let ch=a[1].as_u64().filter(|v|[32,48,64].contains(v)&&*v>=cw).ok_or_else(fail)?;
                (cw,ch)
            }else{(32,32)};
            let image=f["image"].as_str().ok_or_else(fail)?;
            if !images_seen.insert(image){return Err(fail());}
            let im=s["images"].as_array().and_then(|a|a.iter().find(|im|im["id"]==image)).ok_or_else(fail)?;
            let w=im["frame_width"].as_u64().ok_or_else(fail)?;let h=im["frame_height"].as_u64().ok_or_else(fail)?;
            if im.get("encoding").is_some()||w==0||h==0||w>240||h>240||w%cw!=0||h%ch!=0{return Err(fail());}
            let chars:Vec<char>=f["chars"].as_str().ok_or_else(fail)?.chars().collect();
            let metrics=f["metrics"].as_array().ok_or_else(fail)?;
            if chars.is_empty()||chars.len()>49||chars.len() as u64>(w/cw)*(h/ch)||metrics.len()!=chars.len(){return Err(fail());}
            for (c,m) in chars.into_iter().zip(metrics){
                if c.is_control()||c as u32>65535||!seen.insert(c){return Err(fail());}
                let a=m.as_array().filter(|a|a.len()==3).ok_or_else(fail)?;
                let adv=a[0].as_u64().filter(|v|(4..=32).contains(v)).ok_or_else(fail)?;
                let top=a[1].as_u64().filter(|v|*v<=32).ok_or_else(fail)?;
                let bottom=a[2].as_u64().filter(|v|*v>=top&&*v<=32).ok_or_else(fail)?;let _=(adv,bottom);
            }
        }
    }
    let mut background=None;
    if let Some(images)=normalized.get_mut("images"){
        let images_array=images.as_array_mut().filter(|a|!a.is_empty()&&a.len()<=4).ok_or_else(fail)?;
        for (i,im) in images_array.iter_mut().enumerate(){
            if let Some(encoding)=im.as_object_mut().ok_or_else(fail)?.remove("encoding"){
                if encoding!="jpeg"||i!=0||im["frame_width"]!=640||im["frame_height"]!=480{return Err(fail());}
                background=Some(im["id"].as_str().ok_or_else(fail)?.to_owned());
                // Validate its identifier/path with the legacy validator before
                // excluding its separately budgeted pixels from palette images.
                let mut check=im.clone();check["frame_width"]=8.into();check["frame_height"]=8.into();
                validate_surface(&serde_json::json!({"background":0,"images":[check],"elements":[{"type":"image","x":0,"y":0,"w":8,"h":8,"color":0,"image":im["id"]}]}))?;
            }
        }
        if let Some(bg)=&background{
            let a=images.as_array_mut().unwrap();
            if a.iter().filter(|im|im["id"].as_str()==Some(bg)).count()!=1{return Err(fail());}
            a.remove(0);
        }
    }
    if normalized["images"].as_array().is_some_and(|a|a.is_empty()){normalized.as_object_mut().unwrap().remove("images");}
    let nodes=normalized["elements"].as_array_mut().filter(|a|!a.is_empty()&&a.len()<=32).ok_or_else(fail)?;
    for (i,n) in nodes.iter_mut().enumerate(){
        let t=n["type"].as_str().ok_or_else(fail)?.to_owned();
        if let Some(mask)=n.as_object_mut().ok_or_else(fail)?.remove("state_mask"){
            let m=mask.as_u64().filter(|m|(1..=63).contains(m)).ok_or_else(fail)?;
            if runtime.is_some_and(|r|r["input"]["engine"]!="p4-input-v1"||r["states"].as_array().is_none_or(|s|(m>>s.len())!=0))
                ||(t=="image"&&background.as_deref()==n["image"].as_str()){return Err(fail());}
        }
        if let Some(mask)=n.as_object_mut().ok_or_else(fail)?.remove("host_mask"){
            mask.as_u64().filter(|m|(1..=255).contains(m)).ok_or_else(fail)?;
            if runtime.is_some_and(|r|r["host"]!=true||r["input"]["engine"]!="p4-input-v1")
                ||(t=="image"&&background.as_deref()==n["image"].as_str()){return Err(fail());}
        }
        for key in ["tile","show"]{
            if let Some(spec)=n.as_object_mut().ok_or_else(fail)?.remove(key){
                if !interaction{return Err(fail());}
                let o=spec.as_object().ok_or_else(fail)?;
                for field in ["var","max_var"]{
                    if field=="max_var"&&!o.contains_key(field){continue;}
                    let name=spec[field].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
                    if runtime.is_some_and(|r|r["vars"][name]["type"]!="int"){return Err(fail());}
                }
                if key=="show"{
                    if o.len()!=3||!o.keys().all(|k|["var","min","max"].contains(&k.as_str())){return Err(fail());}
                    let lo=spec["min"].as_i64().filter(|v|(-1_000_000_000..=1_000_000_000).contains(v)).ok_or_else(fail)?;
                    if !spec["max"].as_i64().is_some_and(|v|v>=lo&&v<=1_000_000_000){return Err(fail());}
                }else{
                    if t!="image"||!o.keys().all(|k|["columns","rows","row","var","max_var"].contains(&k.as_str())){return Err(fail());}
                    let cols=spec["columns"].as_u64().filter(|v|(1..=8).contains(v)).ok_or_else(fail)?;
                    let rows=spec["rows"].as_u64().filter(|v|(1..=8).contains(v)).ok_or_else(fail)?;
                    if !spec["row"].as_u64().is_some_and(|v|v<rows){return Err(fail());}
                    let im=s["images"].as_array().and_then(|a|a.iter().find(|im|im["id"]==n["image"])).ok_or_else(fail)?;
                    let w=im["frame_width"].as_u64().ok_or_else(fail)?;let h=im["frame_height"].as_u64().ok_or_else(fail)?;
                    if im.get("encoding").is_some()||w%cols!=0||h%rows!=0||w/cols<8||h/rows<8{return Err(fail());}
                }
                if t=="image"&&background.as_deref()==n["image"].as_str(){return Err(fail());}
            }
        }
        if let Some(key)=n.as_object_mut().ok_or_else(fail)?.remove("key_for"){
            let key=key.as_str().filter(|k|!k.is_empty()&&k.len()<=47&&k.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if !typography||t!="button"||n.get("action").is_some()||n.get("bind").is_some(){return Err(fail());}
            if let Some(r)=runtime{if !r["transitions"].as_array().is_some_and(|a|a.iter().any(|v|v["on"]==key)){return Err(fail());}}
        }
        if n.get("opacity").is_some()&&!matches!(t.as_str(),"panel"|"button"){return Err(fail());}
        if n.get("opacity_end").is_some()&&t!="panel"{return Err(fail());}
        let h=n["h"].as_u64().ok_or_else(fail)?;let w=n["w"].as_u64().ok_or_else(fail)?;
        let font=n.get("font_size").map_or(Some(16),Value::as_u64).filter(|v|(10..=96).contains(v)).ok_or_else(fail)?;
        let line=n.get("line_height").map_or(Some((font*5+3)/4),Value::as_u64).filter(|v|*v>=font&&*v<=120).ok_or_else(fail)?;
        let _=line;
        for (k,lo,hi) in [("padding",0,32),("gap",0,24),("icon_size",8,48),("foreground",0,65535),("stroke",0,65535),("stroke_width",0,2)]{
            if n.get(k).is_some()&&!n[k].as_u64().is_some_and(|v|(lo..=hi).contains(&v)){return Err(fail());}
        }
        if n.get("weight").is_some()&&!matches!(n["weight"].as_u64(),Some(400|600)){return Err(fail());}
        if n.get("align").is_some()&&!matches!(n["align"].as_str(),Some("left"|"center"|"right")){return Err(fail());}
        if n.get("valign").is_some()&&!matches!(n["valign"].as_str(),Some("top"|"center"|"bottom")){return Err(fail());}
        if n.get("gradient").is_some()&&(t!="panel"||!matches!(n["gradient"].as_str(),Some("horizontal"|"vertical"))){return Err(fail());}
        if let Some(icon)=n.get("icon"){if !matches!(t.as_str(),"icon"|"button")||!icon.as_str().is_some_and(|v|icons.contains(&v)){return Err(fail());}}
        if t=="icon"&&(n.get("icon").is_none()||w>48||h>48){return Err(fail());}
        if matches!(t.as_str(),"text"|"button")&&((n.get("text").is_some()==n.get("bind").is_some())||h<font){return Err(fail());}
        if t=="button"&&(n.get("padding").and_then(Value::as_u64).unwrap_or(16)*2>=w||(n.get("icon").is_some()&&n.get("icon_size").and_then(Value::as_u64).unwrap_or(20)>h)){return Err(fail());}
        if t=="image"&&background.as_deref()==n["image"].as_str(){
            if i!=0||n["x"]!=0||n["y"]!=0||w!=640||h!=480||n.get("page").is_some_and(|p|p!=4)||n.get("action").is_some()||n.get("motion").is_some(){return Err(fail());}
            n["type"]="panel".into();n.as_object_mut().unwrap().remove("image");
        }
        if t=="button"&&n.get("opacity_end").is_some(){return Err(fail());}
        for k in ["font_size","line_height","align","valign","padding","gap","icon_size","weight","foreground","stroke","stroke_width","icon","gradient"]{n.as_object_mut().unwrap().remove(k);}
        if matches!(t.as_str(),"text"|"icon"|"button"){
            // Content/typography was checked above; use the old geometry checks
            // without imposing its legacy 32px line box on 18px keycaps.
            n["type"]="panel".into();
        }
    }
    let all=nodes.clone();
    for chunk in all.chunks(20){normalized["elements"]=serde_json::json!(chunk);validate_app_surface(&normalized,runtime)?;}
    Ok(())
}
pub fn valid_id(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 44
        && s.as_bytes()[0].is_ascii_lowercase()
        && s.bytes()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || b"_-".contains(&c))
}
pub fn parse(bytes: &[u8]) -> Result<Extension, String> {
    if bytes.len() > 16384 {
        return Err("PC 扩展配置超过 16 KiB".into());
    }
    let e: Extension = serde_json::from_slice(bytes).map_err(|_| "PC 扩展格式或字段无效")?;
    e.validate()?;
    Ok(e)
}
impl Extension {
    pub fn validate(&self) -> Result<(), String> {
        let fail = || Err("PC 扩展声明无效：需要受控联网、媒体、存储及列表面板".into());
        if self.schema_version != 1
            || self.engine != "pc-declarative-media-v1"
            || !self.permissions.public_audio
            || !self.permissions.storage
        {
            return fail();
        }
        let hosts = &self.permissions.network_hosts;
        if hosts.is_empty()
            || hosts.len() > 4
            || hosts.iter().any(|h| {
                h.len() > 253
                    || h.parse::<std::net::IpAddr>().is_ok()
                    || !h.contains('.')
                    || h.split('.').any(|s| {
                        s.is_empty()
                            || s.starts_with('-')
                            || s.ends_with('-')
                            || !s
                                .bytes()
                                .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-')
                    })
            })
        {
            return fail();
        }
        let endpoint = crate::media_network::valid_url(&self.search.endpoint)?;
        if endpoint.scheme() != "https"
            || endpoint.query().is_some()
            || !hosts
                .iter()
                .any(|h| Some(h.as_str()) == endpoint.host_str())
        {
            return fail();
        }
        let safe_param =
            |s: &str| short(s, 40) && s.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'_');
        if self
            .search
            .browse_query
            .as_ref()
            .is_some_and(|q| !short(q, 120) || q.chars().count() > 30)
            || !safe_param(&self.search.query_param)
            || self.search.params.len() > 12
            || self
                .search
                .params
                .iter()
                .any(|(k, v)| !safe_param(k) || !short(v, 128) || k == &self.search.query_param)
        {
            return fail();
        }
        let f = &self.search.fields;
        if let Some(d) = &self.discovery {
            if !self.search.items_path.is_empty() || d.endpoints.is_empty() || d.endpoints.len()>3 || !(1..=100).contains(&d.national_count)
                || !(1..=30).contains(&d.local_supplement) || !(3600..=604800).contains(&d.refresh_sec)
                || d.country.len()!=2 || !d.country.bytes().all(|c|c.is_ascii_uppercase())
                || d.aliases.len()>80 || d.aliases.iter().any(|(a,b)|!short(a,80)||!short(b,80))
                || [&d.city_field,&d.country_field,&d.station_region,&d.popularity,&d.votes].iter().any(|s|!pointer(s)) { return fail(); }
            for raw in d.endpoints.iter().chain(std::iter::once(&d.location_endpoint)) {
                let u=crate::media_network::valid_url(raw)?;
                if u.scheme()!="https" || !hosts.iter().any(|h|Some(h.as_str())==u.host_str()) { return fail(); }
            }
        }
        if [&self.search.items_path, &f.id, &f.title, &f.url]
            .iter()
            .any(|s| !pointer(s))
            || [&f.fallback_url, &f.homepage, &f.codec, &f.hls, &f.available]
                .iter()
                .any(|s| s.as_ref().is_some_and(|s| !pointer(s)))
        {
            return fail();
        }
        let p = &self.panel;
        if !short(&p.title, 120)
            || !short(&p.description, 1200)
            || !short(&p.search_placeholder, 180)
            || !short(&p.attribution, 120)
        {
            return fail();
        }
        crate::media_network::valid_url(&p.attribution_url)?;
        let mut seen = std::collections::HashSet::new();
        if p.sections.is_empty()
            || p.sections.len() > 5
            || p.sections.iter().any(|s| {
                !matches!(
                    s.as_str(),
                    "search" | "library" | "history" | "volume" | "sleep"
                ) || !seen.insert(s)
            })
        {
            return fail();
        }
        Ok(())
    }
    /// Mapping only: no expressions, interpolation, scripts or executable filters.
    pub fn map_results(&self, value: &Value, query: &str) -> Vec<crate::media_network::Station> {
        let Some(rows) = value
            .pointer(&self.search.items_path)
            .and_then(Value::as_array)
        else {
            return vec![];
        };
        let f = &self.search.fields;
        let mut seen = std::collections::HashSet::new();
        rows.iter()
            .take(1000)
            .filter_map(|r| {
                let text = |p: &str| r.pointer(p).and_then(Value::as_str).unwrap_or("");
                let optional = |p: &Option<String>| p.as_deref().map(text).unwrap_or("");
                if f.available.as_ref().is_some_and(|p| {
                    !matches!(r.pointer(p), Some(Value::Bool(true)))
                        && r.pointer(p).and_then(Value::as_i64) != Some(1)
                }) {
                    return None;
                }
                let id = text(&f.id);
                let title = text(&f.title).trim();
                if !short(id, 80) || !short(title, 240) || !seen.insert(id.to_string()) {
                    return None;
                }
                let raw = if text(&f.url).is_empty() {
                    optional(&f.fallback_url)
                } else {
                    text(&f.url)
                };
                let url = crate::media_network::valid_url(raw).ok()?;
                let hls = f.hls.as_ref().is_some_and(|p| {
                    r.pointer(p) == Some(&Value::Bool(true))
                        || r.pointer(p).and_then(Value::as_i64) == Some(1)
                });
                Some(crate::media_network::Station {
                    id: id.into(),
                    name: title.chars().take(50).collect(),
                    city: query.into(),
                    url: url.into(),
                    homepage: optional(&f.homepage).chars().take(2048).collect(),
                    codec: optional(&f.codec).chars().take(12).collect(),
                    hls,
                })
            })
            .take(20)
            .collect()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn platform_track_has_bounded_native_schema_and_no_teleport_rules(){
        let good:Value=serde_json::from_str(include_str!("../../new-components/cloud-hop/runtime/widget.json")).unwrap();
        assert!(validate_app_runtime(&good).is_ok());
        for (key,v) in [("camera_x",serde_json::json!(0)),("gap",serde_json::json!([2,255])),("width",serde_json::json!([2,8])),("tones",serde_json::json!([1,1])),("engine",serde_json::json!("unknown"))]{
            let mut bad=good.clone();bad["scene"]["motion"]["track"][key]=v;assert!(validate_app_runtime(&bad).is_err(),"{key}");
        }
        let mut bad=good.clone();bad["scene"]["motion"]["track"]=Value::Null;assert!(validate_app_runtime(&bad).is_err());
        let mut bad=good;bad["scene"]["rules"][1]["do"]=serde_json::json!([{"op":"place","entity":"puppy","x":2,"y":4}]);assert!(validate_app_runtime(&bad).is_err());
    }
    #[test]
    fn application_graphics_reject_unknown_icons_and_unbounded_layouts(){
        let good=serde_json::json!({"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","background":0,"images":[{"id":"background","asset":"assets/background.png","frame_width":640,"frame_height":480,"frames":1,"fps":1,"encoding":"jpeg"}],"elements":[{"type":"image","x":0,"y":0,"w":640,"h":480,"color":0,"image":"background"},{"type":"button","x":26,"y":366,"w":126,"h":48,"color":23000,"text":"摸摸头","font_size":17,"icon":"heart","padding":18,"foreground":65535}]});
        assert!(validate_app_surface(&good,None).is_ok());
        for (key,value) in [("icon",serde_json::json!("external-svg")),("font_size",serde_json::json!(97)),("line_height",serde_json::json!(12)),("padding",serde_json::json!(64)),("weight",serde_json::json!(500)),("gradient",serde_json::json!("vertical")),("opacity_end",serde_json::json!(12))]{
            let mut bad=good.clone();bad["elements"][1][key]=value;assert!(validate_app_surface(&bad,None).is_err(),"{key}");
        }
        let mut bad=good.clone();bad["elements"][0]["page"]=0.into();assert!(validate_app_surface(&bad,None).is_err());
        let mut bad=good.clone();bad["images"][0]["frame_width"]=320.into();assert!(validate_app_surface(&bad,None).is_err());
        let mut bad=good.clone();bad.as_object_mut().unwrap().remove("graphics");assert!(validate_app_surface(&bad,None).is_err());
    }
    #[test]
    fn application_references_limits_and_save_schema_are_bounded(){
        let mut r=serde_json::json!({"states":["ready","run","paused"],"pages":[{"id":"a"}],"vars":{"duration":{"type":"int","init":1500},"left":{"type":"int","init":1500}},"transitions":[{"on":"go","from":"ready","to":"run","set":{"left":{"var":"duration"}}}],"tick":[],"limits":{"duration":[300,3600]},"persist":{"version":1,"vars":["left"],"states":{"ready":"ready","run":"paused","paused":"paused"},"page":true},"surface":{"engine":"p4-app-surface-v1","background":65535,"elements":[{"type":"text","x":0,"y":0,"w":200,"h":32,"color":0,"bind":"headline","action":"go"}]}});
        assert!(validate_app_runtime(&r).is_ok());assert!(validate_app_surface(&r["surface"],Some(&r)).is_ok());
        r["limits"]["duration"]=serde_json::json!([1600,3600]);assert!(validate_app_runtime(&r).is_err());r["limits"]["duration"]=serde_json::json!([300,3600]);
        r["transitions"][0]["set"]["left"]=serde_json::json!({"var":"secret"});assert!(validate_app_runtime(&r).is_err());
        r["surface"]["elements"][0]["action"]="shell".into();assert!(validate_app_surface(&r["surface"],Some(&r)).is_err());
    }
    #[test]
    #[ignore = "Requires authored package artifacts supplied via PET_APP_PACKAGE_DIR"]
    fn generated_application_packages_pass_native_import_and_image_budget(){
        let dir=std::path::PathBuf::from(std::env::var("PET_APP_PACKAGE_DIR").expect("supply a generated package directory"));
        let mut count=0;
        for entry in std::fs::read_dir(dir).unwrap(){let root=entry.unwrap().path();if !root.is_dir()||!root.join("component.json").is_file(){continue;}
            let result=crate::clawpkg::validate_clawpkg_at_path(&root).unwrap();assert!(result.ok,"{}: {:?}",root.display(),result.errors);
            let r:Value=serde_json::from_slice(&std::fs::read(root.join("runtime/widget.json")).unwrap()).unwrap();
            assert!(validate_app_runtime(&r).is_ok());let mut total=0;
            for im in r["surface"]["images"].as_array().into_iter().flatten(){
                let png=std::fs::read(root.join(im["asset"].as_str().unwrap())).unwrap();let image=decode_surface_image(im,&png).unwrap();
                let packed=if im["encoding"]=="jpeg"{crate::surface_image::encode_background(&image).unwrap()}else{crate::surface_image::encode(&image)};
                assert!(packed.len()<=100000);total+=packed.len();
                if let Ok(output)=std::env::var("PET_APP_ENCODED_DIR"){
                    let output=std::path::Path::new(&output).join(root.file_name().unwrap());std::fs::create_dir_all(&output).unwrap();
                    std::fs::write(output.join(format!("{}.p4i",im["id"].as_str().unwrap())),&packed).unwrap();
                    if im["encoding"]=="jpeg"{
                        let decoded=image::load_from_memory(&packed[12..]).unwrap().to_rgb8();assert_eq!(decoded.dimensions(),(640,480));
                        let mse=image.pixels().zip(decoded.pixels()).map(|(a,b)|(0..3).map(|c|(f64::from(a[c])-f64::from(b[c])).powi(2)).sum::<f64>()).sum::<f64>()/(640.0*480.0*3.0);
                        assert!(mse.sqrt()<12.0,"JPEG error {}",mse.sqrt());decoded.save(output.join("background.png")).unwrap();
                        eprintln!("  full-resolution JPEG RGB RMSE {:.2}",mse.sqrt());
                    }
                }
            }
            assert!(total<=128*1024);eprintln!("{}: native validation passed; device images {} bytes",root.file_name().unwrap().to_string_lossy(),total);count+=1;
        }assert!(count>0);
    }
    fn extension() -> Extension {
        parse(include_bytes!(
            "../../tests/fixtures/media-sample/pc/extension.json"
        ))
        .unwrap()
    }
    #[test]
    fn maps_package_fields_without_a_provider_branch() {
        let mut e = extension();
        e.search.items_path = "/items".into();
        e.search.fields.id = "/key".into();
        e.search.fields.title = "/label".into();
        e.search.fields.available = None;
        let rows=e.map_results(&serde_json::json!({"items":[{"key":"test","label":"Example","url_resolved":"https://example.com/live"}]}),"test");
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0].id, "test");
    }
    #[test]
    fn rejects_undeclared_hosts_scripts_and_paths() {
        let mut e = extension();
        e.search.endpoint = "https://elsewhere.example/search".into();
        assert!(e.validate().is_err());
        let mut v = serde_json::to_value(extension()).unwrap();
        v["script"] = Value::String("fetch('/secret')".into());
        assert!(parse(&serde_json::to_vec(&v).unwrap()).is_err());
        for id in ["../other", "/absolute", "a/b", "UPPER", ""] {
            assert!(!valid_id(id));
        }
    }
    #[test]
    fn surface_and_package_validation_reject_cross_owner_and_private_files() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../tests/fixtures/media-sample");
        let valid = crate::clawpkg::validate_clawpkg_at_path(&root).unwrap();
        assert!(valid.ok, "{:?}", valid.errors);
        let temp = tempfile::tempdir().unwrap();
        for name in [
            "component.json",
            "negative-screen.json",
            "buttons.json",
            "share.json",
            "runtime/widget.json",
            "pc/extension.json",
            "assets/.keep",
            "assets/checker.png",
            "assets/stripe.png",
        ] {
            let path = temp.path().join(name);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::copy(root.join(name), path).unwrap();
        }
        let path = temp.path().join("runtime/widget.json");
        let mut runtime: Value = serde_json::from_slice(&std::fs::read(&path).unwrap()).unwrap();
        assert!(validate_surface(&runtime["surface"]).is_ok());
        runtime["media"]["source"] = Value::String("pc.other-component".into());
        std::fs::write(&path, serde_json::to_vec(&runtime).unwrap()).unwrap();
        assert!(
            !crate::clawpkg::validate_clawpkg_at_path(temp.path())
                .unwrap()
                .ok
        );
        runtime["media"]["source"] = Value::String("pc.media-sample".into());
        runtime["surface"]["elements"][0]["x"] = Value::from(640);
        assert!(validate_surface(&runtime["surface"]).is_err());
        std::fs::copy(root.join("runtime/widget.json"), &path).unwrap();
        std::fs::write(temp.path().join("pc/library.json"), "{}").unwrap();
        assert!(
            !crate::clawpkg::validate_clawpkg_at_path(temp.path())
                .unwrap()
                .ok
        );
    }
}
