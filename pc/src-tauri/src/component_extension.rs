//! Declarative PC companions. No package code, filesystem access, or raw IPC.
use serde::{Deserialize, Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
#[path = "app_form.rs"]
pub(crate) mod app_form;
#[path = "app_canvas.rs"]
pub(crate) mod app_canvas;
#[path = "app_texts.rs"]
pub(crate) mod app_texts;

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
    /// Rows of the package-bundled `pc/catalog.json`, loaded from the formal
    /// library copy at service start. Never part of the declaration itself.
    #[serde(skip)]
    pub catalog_rows: Vec<Value>,
}
/// Package-owned public catalogue policy; no executable provider code.
#[derive(Clone, Debug, Deserialize, Serialize)]
#[serde(deny_unknown_fields, rename_all = "camelCase")]
pub struct Discovery {
    /// 1–3 HTTPS mirrors; empty only when `search.catalog` bundles the rows.
    #[serde(default)]
    pub endpoints: Vec<String>,
    /// Empty disables city estimation (national list only).
    #[serde(default)]
    pub location_endpoint: String,
    pub city_field: String,
    pub country_field: String,
    pub country: String,
    pub aliases: BTreeMap<String, String>,
    /// City → region used when rows carry a province-level `stationRegion`.
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub region_aliases: BTreeMap<String, String>,
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
    /// HTTPS GET endpoint; empty only when `catalog` bundles the rows offline.
    #[serde(default)]
    pub endpoint: String,
    /// `pc/catalog.json`: a bounded JSON array shipped inside the package and
    /// read through the same field mapping. No network, no code.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub catalog: Option<String>,
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
    validate_surface_with_budget(s,crate::app_graphics::V1)
}
fn validate_surface_with_budget(s:&Value,quota:crate::app_graphics::Limits)->Result<(),String>{
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
        let a=images.as_array().filter(|a|!a.is_empty()&&a.len()<=quota.images).ok_or_else(error)?;
        for im in a {
            if !keys(im,&["id","asset","frame_width","frame_height","frames","fps"]){return Err(error());}
            let id=im["id"].as_str().filter(|id|!id.is_empty()&&id.len()<=15&&id.bytes().all(|c|c.is_ascii_lowercase()||c.is_ascii_digit()||c==b'_'||c==b'-')).ok_or_else(error)?;
            let asset=im["asset"].as_str().ok_or_else(error)?;
            let name=asset.strip_prefix("assets/").ok_or_else(error)?;
            if !name.ends_with(".png")||name.len()<5||name.contains("..")||!name.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c)){return Err(error());}
            let w=num(im,"frame_width",8,512)?;let h=num(im,"frame_height",8,240)?;
            num(im,"frames",1,1)?;num(im,"fps",1,1)?;
            if image_ids.insert(id,(w,h)).is_some(){return Err(error());}
            if w*h>98304{return Err(error());}
            pixels+=w*h;if pixels>quota.image_pixels{return Err(error());}
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
    let v3=runtime["quota"]=="p4-quota-v3";
    let count=if v3{32}else if runtime["quota"]=="p4-quota-v2"{16}else{8};
    let mut worst=128usize+if p["page"]==true{64}else{0}+if runtime["alarms"].as_array().is_some_and(|a|!a.is_empty()){268}else{0};
    let vars=p["vars"].as_array().filter(|a|!a.is_empty()&&a.len()<=count).ok_or_else(error)?;
    let mut unique=std::collections::HashSet::new();
    for v in vars {
        let name=v.as_str().ok_or_else(error)?;
        if name.is_empty()||name.len()>31||!name.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))||!unique.insert(name){return Err(error());}
        let kind=runtime["vars"][name]["type"].as_str().ok_or_else(error)?;
        let value_bytes=if kind=="int"{11}else if kind=="string"{
            let field=if matches!(runtime["form"]["engine"].as_str(),Some("p4-app-form-v2"|"p4-app-form-v3")){
                runtime["form"]["fields"].as_array().and_then(|a|a.iter().find(|f|f["kind"]=="text"&&f["var"]==name))
            }else{None};
            if !v3&&field.is_none(){return Err(error());}
            let cap=if let Some(f)=field{f["max_bytes"].as_u64().filter(|n|(1..=63).contains(n)).ok_or_else(error)? as usize}else{127};
            2+6*cap
        }else{return Err(error());};
        worst+=name.len()+4+value_bytes;
    }
    let budget=if v3{2303}else{1023};
    if worst>budget{return Err(format!("persist 最坏 JSON 转义尺寸 {worst} 超过存档预算 {budget} 字节"));}
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
    crate::app_graphics::validate_runtime(runtime)?;
    cells_shape(runtime)?;
    crate::app_shell::validate(runtime)?;
    crate::app_data_rows::validate(runtime)?;
    crate::widget_notify::validate(runtime)?;
    if runtime.get("ownership").is_some(){crate::app_variable_owners::parse(runtime)?;}
    crate::app_component_program::validate(runtime)?;
    validate_navigation(runtime)?;
    app_form::validate_runtime(runtime)?;
    crate::app_longtext::validate_derived_writers(runtime)?;
    app_texts::validate_runtime(runtime)?;
    if runtime.get("host_context").is_some()&&(runtime["host_context"]!="p4-host-context-v1"||runtime["host"]!=true||runtime["input"]["engine"]!="p4-input-v1"){return Err("宿主动作上下文声明无效".into());}
    if runtime["scene"]["engine"]==crate::side_scene::ENGINE{crate::side_scene::validate_runtime(runtime)?;}
    validate_local_tools(runtime)?;
    validate_app_persistence(runtime)?;
    validate_motion_runtime(runtime)?;
    if runtime["transitions"].as_array().is_some_and(|ts|ts.len()>if runtime.get("input").is_some(){if runtime["quota"]=="p4-quota-v3"{48}else if runtime["quota"]=="p4-quota-v2"{32}else{24}}else{12}){
        return Err("组件状态转换超出预算".into());
    }
    if let Some(input)=runtime.get("input"){
        let bad=||"组件输入策略必须使用 p4-input-v1 及已声明的有界动作".to_string();
        let p=input.as_object().ok_or_else(bad)?;
        if input["engine"]!="p4-input-v1" || p.keys().any(|k|!["engine","repeat","press","swipe","confirm","drag","cooldown","hold"].contains(&k.as_str())){return Err(bad());}
        let transitions=runtime["transitions"].as_array().ok_or_else(bad)?;
        let known=|v:&Value|v.as_str().is_some_and(|s|transitions.iter().any(|t|t["on"]==s));
        if let Some(h)=p.get("hold"){
            if !known(h)||(runtime["scene"]["motion"]["engine"]!="p4-motion-v1"&&runtime["scene"]["engine"]!=crate::side_scene::ENGINE)||p.contains_key("drag")
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
    let rules=runtime["transitions"].as_array().into_iter().flatten().chain(runtime["transitions"].as_array().into_iter().flatten().filter_map(|t|t.get("else"))).chain(runtime["tick"].as_array().into_iter().flatten()).chain(runtime["tick"].as_array().into_iter().flatten().filter_map(|t|t.get("then")));
    for rule in rules{for op in ["set","inc"]{if let Some(values)=rule.get(op){
        for (name,value) in values.as_object().ok_or_else(error)?{
            let kind=runtime["vars"][name]["type"].as_str().ok_or_else(error)?;
            // `set {var}` copies another int; p4-logic-v1 also allows `inc {var}` (add another int).
            if value.is_object()&&(op=="set"||(op=="inc"&&runtime["logic"]=="p4-logic-v1")){
                let source=value["var"].as_str().ok_or_else(error)?;
                if value.as_object().unwrap().len()!=1||kind!="int"||runtime["vars"][source]["type"]!="int"{return Err(error());}
            }else if kind=="int"{
                if !value.as_i64().is_some_and(|n|(-1_000_000_000..=1_000_000_000).contains(&n)){return Err(error());}
            }else if kind!="string"||op!="set"||!value.as_str().is_some_and(|s|s.len()<=63){return Err(error());}
        }
    }}}
    Ok(())
}

fn validate_navigation(r:&Value)->Result<(),String>{
    let rules=r["transitions"].as_array();
    if r.get("navigation").is_none()&&!rules.is_some_and(|ts|ts.iter().any(|t|t.get("from_page").is_some()||t["page"].is_object())){return Ok(());}
    let bad=||"页面导航需要 p4-app-navigation-v1、本地应用、统一输入和有效页面操作".to_string();
    let max_pages=if r["quota"]=="p4-quota-v3" {12} else if r["quota"]=="p4-quota-v2" {6} else {4};
    let pages=r["pages"].as_array().filter(|a|(2..=max_pages).contains(&a.len())).ok_or_else(bad)?;
    let mut ids=std::collections::HashSet::new();
    for p in pages {let id=p["id"].as_str().filter(|s|!s.is_empty()&&s.len()<=23&&s.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c))).ok_or_else(bad)?;if !ids.insert(id){return Err(bad());}}
    if r["navigation"]!="p4-app-navigation-v1"||r.get("host").is_some()&&!crate::app_variable_owners::mixed(r)?||r.get("local").is_some()||r["surface"]["engine"]!="p4-app-surface-v1"||r["input"]["engine"]!="p4-input-v1"
        ||["game","scene","media","instrument","data"].iter().any(|k|r.get(*k).is_some()){return Err(bad());}
    let known=|v:&Value|v.is_string()&&pages.iter().any(|p|p["id"]==*v);
    for t in rules.ok_or_else(bad)? {
        if t.get("from_page").is_some_and(|v|!known(v)){return Err(bad());}
        if let Some(p)=t.get("page"){
            if let Some(o)=p.as_object(){
                if o.len()!=1{return Err(bad());}
                let (op,target)=o.iter().next().unwrap();
                if (op=="back"&&target!=true)||(op!="back"&&(!["push","replace","reset"].contains(&op.as_str())||!known(target))){return Err(bad());}
            }else if !known(p)&&p!="next"&&p!="prev"{return Err(bad());}
        }
    }
    Ok(())
}

#[cfg(test)]
mod navigation_tests {
    use super::*;
    #[test]
    fn navigation_contract_and_negative_profiles(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/app-navigation-v1.json")).unwrap();
        let r=suite["runtime"].clone();assert!(validate_app_runtime(&r).is_ok());
        for (pointer,value) in [
            ("/navigation",Value::Null),("/host",Value::Bool(true)),
            ("/transitions/0/from_page",serde_json::json!("missing")),
            ("/transitions/0/page",serde_json::json!({"push":"missing"})),
            ("/transitions/0/page",serde_json::json!({"back":false})),
            ("/transitions/0/page",serde_json::json!({"back":true,"push":"home"})),
            ("/transitions/0/page",serde_json::json!(["home"])),
            ("/transitions/0/page",Value::Null),
            ("/pages/1/id",serde_json::json!("home")),
        ] {
            let mut bad=r.clone();
            if pointer=="/host"{bad["host"]=value;}else{*bad.pointer_mut(pointer).unwrap()=value;}
            assert!(validate_app_runtime(&bad).is_err(),"accepted {pointer}: {bad}");
        }
        let mut legacy=r;legacy.as_object_mut().unwrap().remove("navigation");
        assert!(validate_app_runtime(&legacy).is_err());
        legacy["transitions"]=serde_json::json!([{ "from":"*","on":"next","page":"next" }]);
        assert!(validate_app_runtime(&legacy).is_ok());
    }
}

/// The historical quota/maxLocalItems advertisement does not prove mask safety.
/// Require the explicit repaired-mask contract before installing any deck >16.
pub fn require_local_tools_capability(r: &Value, capabilities: &Value) -> Result<(), String> {
    let Some(local) = r.get("local") else { return Ok(()); };
    if capabilities["widgetLocalTools"] != "p4-local-tools-v1" {
        return Err("需要离线内容池与真实日期能力，请先升级设备固件；未安装不兼容资源".into());
    }
    let extended = local["kind"] == "deck"
        && local["groups"].as_array().is_some_and(|groups| groups.iter().any(|group|
            group["items"].as_array().is_some_and(|items| items.len() > 16)));
    if extended && capabilities["widgetLocalToolsMasks"] != "p4-local-masks-v2" {
        return Err("超过 16 项的离线内容池需要 p4-local-masks-v2 安全存档能力，请先升级设备固件；未写入组件".into());
    }
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
                if !(2..=if r["quota"]=="p4-quota-v3"{64}else if r["quota"]=="p4-quota-v2"{24}else{16}).contains(&items.len()){return None;}
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
        let wide=deck&&groups.iter().any(|g|g["items"].as_array().is_some_and(|items|items.len()>24));
        if wide&&r["quota"]!="p4-quota-v3"{return None;}
        let count=if wide{13}else{7};
        let vars:Vec<String>=(0..count).map(|i|format!("local{i}")).collect();let v=r["vars"].as_object()?;
        if v.len()!=count||!vars.iter().all(|k|v.get(k).is_some_and(|v|v["type"]=="int")){return None;}
        let saved:serde_json::Map<String,Value>=states.iter().map(|s|(s.to_string(),Value::String("ready".into()))).collect();
        if r["persist"]!=serde_json::json!({"version":if wide{2}else{1},"vars":vars,"states":saved}){return None;}
        let ts=r["transitions"].as_array()?;let mut seen=std::collections::HashSet::new();
        if ts.len()!=actions.len(){return None;}
        for t in ts{let on=t["on"].as_str()?;if !keys(t,&["on","from"])||t["from"]!="*"||!actions.contains_key(on)||!seen.insert(on){return None;}}
        Some(())
    };
    check().ok_or_else(||"离线工具必须遵循 p4-local-tools-v1 的内容池、日期和存档契约".into())
}
#[cfg(test)]
mod local_tools_mask_boundary_tests {
    use super::*;
    fn fixture(count:usize)->Value {
        let wide=count>24;let names:Vec<_>=(0..if wide{13}else{7}).map(|i|format!("local{i}")).collect();
        let vars:serde_json::Map<String,Value>=names.iter().map(|n|(n.clone(),serde_json::json!({"type":"int","init":0}))).collect();
        let states=["ready","busy","result","editing","empty","offline"];
        let saved:serde_json::Map<String,Value>=states.iter().map(|s|(s.to_string(),Value::String("ready".into()))).collect();
        serde_json::json!({"quota":if wide{"p4-quota-v3"}else{"p4-quota-v2"},"local":{"engine":"p4-local-tools-v1","kind":"deck","groups":[{"title":"g","items":(0..count).map(|i|serde_json::json!({"title":format!("item{i}"),"note":"n"})).collect::<Vec<_>>() }],"actions":{"local.draw":"draw","local.close":"close"},"reveal_ms":300,"idle":"i","busy":"b","hint":"h"},"states":states,"initial_state":"ready","surface":{"engine":"p4-app-surface-v1"},"input":{"engine":"p4-input-v1"},"vars":vars,"persist":{"version":if wide{2}else{1},"vars":names,"states":saved},"transitions":[{"on":"local.draw","from":"*"},{"on":"local.close","from":"*"}]})
    }
    #[test]
    fn shared_local_masks_capability_vectors() {
        let vectors:Value=serde_json::from_str(include_str!("../../../firmware/tests/fixtures/local_tools_masks_capabilities.json")).unwrap();
        for (index,case) in vectors["vectors"].as_array().unwrap().iter().enumerate() {
            let runtime=if case["kind"].is_null(){serde_json::json!({})}else{
                let groups:Vec<_>=case["counts"].as_array().unwrap().iter().map(|n|
                    serde_json::json!({"items":vec![serde_json::json!({});n.as_u64().unwrap() as usize]})).collect();
                serde_json::json!({"local":{"kind":case["kind"],"groups":groups}})
            };
            assert_eq!(require_local_tools_capability(&runtime,&case["capabilities"]).is_ok(),case["allowed"].as_bool().unwrap(),"vector={index}");
        }
    }
    #[test]
    fn extended_decks_require_exact_mask_capability_not_quota_or_advertised_count() {
        let legacy=serde_json::json!({"widgetLocalTools":"p4-local-tools-v1","widgetQuota":"p4-quota-v3","widgetQuotas":["p4-quota-v2","p4-quota-v3"],"maxLocalItems":64});
        let mut safe=legacy.clone();safe["widgetLocalToolsMasks"]=serde_json::json!("p4-local-masks-v2");
        for count in [16,17,24,25,32,33,64] {
            let r=fixture(count);
            assert_eq!(require_local_tools_capability(&r,&legacy).is_ok(),count<=16,"count={count}");
            assert!(require_local_tools_capability(&r,&safe).is_ok());
            for wrong in [serde_json::Value::Null,serde_json::json!(true),serde_json::json!("p4-local-masks-v1"),serde_json::json!(["p4-local-masks-v2"])] {
                let mut caps=legacy.clone();caps["widgetLocalToolsMasks"]=wrong;
                assert_eq!(require_local_tools_capability(&r,&caps).is_ok(),count<=16);
            }
        }
        let mut second=fixture(16);second["local"]["groups"].as_array_mut().unwrap().push(fixture(17)["local"]["groups"][0].clone());
        assert!(require_local_tools_capability(&second,&legacy).is_err());
        let mut countdown=fixture(64);countdown["local"]["kind"]=serde_json::json!("countdown");
        assert!(require_local_tools_capability(&countdown,&legacy).is_ok());
        let mut missing_base=safe.clone();missing_base.as_object_mut().unwrap().remove("widgetLocalTools");
        assert!(require_local_tools_capability(&fixture(16),&missing_base).is_err());
        assert!(require_local_tools_capability(&serde_json::json!({}),&serde_json::json!({})).is_ok());
    }
    #[test]
    fn local_masks_preserve_legacy_and_require_complete_wide_save_contract(){
        for count in [16,17,24,32,33,64] {
            let r=fixture(count);assert!(validate_local_tools(&r).is_ok(),"count={count}");
            assert!(validate_app_persistence(&r).is_ok());
            let mut bad=r.clone();bad["persist"]["version"]=serde_json::json!(if count>24{1}else{2});assert!(validate_local_tools(&bad).is_err());
            if count>24 {
                let mut bad=r.clone();bad["vars"].as_object_mut().unwrap().remove("local12");assert!(validate_local_tools(&bad).is_err());
                let mut bad=r.clone();bad["quota"]=serde_json::json!("p4-quota-v2");assert!(validate_local_tools(&bad).is_err());
            }
        }
        assert!(validate_local_tools(&fixture(65)).is_err());
    }
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
    validate_surface_with_budget(&normalized,crate::app_graphics::limits(runtime))
}
// Cells keep the same integer-grid and atlas contract as the native parser.
// Only their drawing projection is normalized for the legacy geometry validator.
fn cells_shape(runtime:&Value)->Result<Option<(u64,u64,u64)>,String>{
    let bad=||"格子状态需要 p4-cells-v1、1–16 的行列及 2/4 bit 格值".to_string();
    let Some(c)=runtime.get("cells") else{return Ok(None)};
    if runtime["engine"]!="p4-bounded-runtime-v4"||c["engine"]!="p4-cells-v1"
        ||!c.as_object().is_some_and(|o|o.keys().all(|k|["engine","cols","rows","bits","init"].contains(&k.as_str())))
        ||["game","scene","local","media","instrument","data","host"].iter().any(|k|runtime.get(*k).is_some()){return Err(bad());}
    let cols=c["cols"].as_u64().filter(|n|(1..=16).contains(n)).ok_or_else(bad)?;
    let rows=c["rows"].as_u64().filter(|n|(1..=16).contains(n)).ok_or_else(bad)?;
    let bits=c["bits"].as_u64().filter(|n|[2,4].contains(n)).ok_or_else(bad)?;
    if let Some(init)=c.get("init"){
        let init=init.as_array().filter(|a|a.len()==rows as usize).ok_or_else(bad)?;
        for row in init{
            if !row.as_str().is_some_and(|row|row.len()==cols as usize&&row.bytes().all(|c|(c as char).to_digit(16).is_some_and(|n|n<(1u32<<bits)))){return Err(bad());}
        }
    }
    Ok(Some((cols,rows,bits)))
}
fn validate_cells_node(n:&Value,s:&Value,runtime:Option<&Value>,bindings:bool)->Result<(),String>{
    let bad=||"格子绘图需要有效网格、整格尺寸、普通图集及整数光标变量".to_string();
    if !bindings||["tile","action","motion","rotate_ms","text","bind","var_bind","text_at","field_bind","key_for","row_bind"].iter().any(|k|n.get(*k).is_some()){return Err(bad());}
    let shape=if let Some(r)=runtime{Some(cells_shape(r)?.ok_or_else(bad)?)}else{None};
    let w=n["w"].as_u64().filter(|n|*n>0).ok_or_else(bad)?;
    let h=n["h"].as_u64().filter(|n|*n>0).ok_or_else(bad)?;
    if shape.is_some_and(|(cols,rows,_)|w%cols!=0||h%rows!=0||w/cols<4||h/rows<4){return Err(bad());}
    if n.get("image").is_some(){
        let image=n["image"].as_str().ok_or_else(bad)?;
        let im=s["images"].as_array().and_then(|a|a.iter().find(|im|im["id"]==image)).ok_or_else(bad)?;
        let columns=n["columns"].as_u64().filter(|n|(2..=shape.map_or(16,|(_,_,bits)|1u64<<bits)).contains(n)).ok_or_else(bad)?;
        if im.get("encoding").is_some()||!im["frame_width"].as_u64().is_some_and(|w|w%columns==0&&w/columns>=4){return Err(bad());}
    }else if n.get("columns").is_some(){return Err(bad());}
    if let Some(cursor)=n.get("cursor"){
        if !cursor.as_object().is_some_and(|o|o.len()==3&&o.keys().all(|k|["x_var","y_var","color"].contains(&k.as_str())))
            ||!cursor["color"].as_u64().is_some_and(|n|n<=65535){return Err(bad());}
        for key in ["x_var","y_var"]{
            let var=cursor[key].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c))).ok_or_else(bad)?;
            if runtime.is_some_and(|r|r["vars"][var]["type"]!="int"){return Err(bad());}
        }
    }
    Ok(())
}
fn validate_app_graphics(s:&Value,runtime:Option<&Value>)->Result<(),String>{
    let fail=||"应用图形需要受控图标、字号、按钮与 640×480 静态背景".to_string();
    if s["graphics"]!="p4-app-graphics-v1"||s["engine"]!="p4-app-surface-v1"{return Err(fail());}
    crate::app_graphics::validate_image_budget(s,runtime)?;
    let icons=["home","leaf","desk","agent","compass","sun","mic","book","sound","spark","heart","pause","play","check","plus","list","arrow","cloud","clock","stretch"];
    let shell=s["shell"]==crate::app_shell::ENGINE;
    if s.get("shell").is_some() && (!shell || s["bindings"]!="p4-variable-bindings-v1") {return Err(fail());}
    if let Some(tabs)=s.get("tabs") {crate::app_shell::validate_tabs(tabs,s,runtime)?;}
    let texts=crate::app_longtext::validate_texts(s)?;
    let mut used_texts=Vec::new();
    let mut expanded=s["tabs"]["items"].as_array().map_or(0,|a|a.len());
    let mut normalized=s.clone();crate::service_bridge::surface::normalize(s,&mut normalized,runtime)?;normalized.as_object_mut().ok_or_else(fail)?.remove("graphics");
    for key in ["shell","tabs","texts"] {normalized.as_object_mut().unwrap().remove(key);}
    // r150: p4-graphics-v2 ceilings, keyframe backgrounds and particle emitters follow the declared profile.
    let limits=crate::app_graphics::limits(runtime);
    crate::app_graphics::validate_particles(s,runtime)?;normalized.as_object_mut().unwrap().remove("particles");
    crate::app_graphics::validate_keyframes(s,runtime)?;
    if let Some(collection)=normalized.as_object_mut().unwrap().remove("collection_view"){
        crate::app_host_collection::Config::parse(&collection)?.validate_surface(s,runtime)?;
    }
    if let Some(canvas)=normalized.as_object_mut().unwrap().remove("canvas"){
        if s["bindings"]!="p4-variable-bindings-v1"{return Err(fail());}
        app_canvas::validate(&canvas,runtime)?;
    }
    let variable_bindings=match normalized.as_object_mut().unwrap().remove("bindings"){
        None=>false,Some(v)if v=="p4-variable-bindings-v1"=>true,_=>return Err(fail()),
    };
    let viewport=normalized.as_object_mut().unwrap().remove("viewport");
    let viewports=normalized.as_object_mut().unwrap().remove("viewports");
    let mut declared:Vec<&Value>=Vec::new();
    if let Some(multi)=&viewports{
        if viewport.is_some()||multi["engine"]!="p4-app-viewports-v1"||!multi.as_object().is_some_and(|m|m.len()==2&&m.keys().all(|k|["engine","items"].contains(&k.as_str()))){return Err(fail());}
        let items=multi["items"].as_array().filter(|a|(1..=3).contains(&a.len())).ok_or_else(fail)?;
        declared.extend(items.iter());
    }else if let Some(v)=&viewport{declared.push(v);}
    for (i,v) in declared.iter().enumerate(){
        let kind=if viewports.is_some(){"id"}else{"engine"};
        if !variable_bindings||!v.as_object().is_some_and(|m|m.len()==7&&m.keys().all(|k|[kind,"x","y","w","h","content_height","offset_var"].contains(&k.as_str())))||["game_view","scene_view","data_view"].iter().any(|k|s.get(k).is_some()){return Err(fail());}
        if viewports.is_none()&&v["engine"]!="p4-app-viewport-v1"{return Err(fail());}
        let x=v["x"].as_u64().filter(|v|*v<640).ok_or_else(fail)?;let y=v["y"].as_u64().filter(|v|*v<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|(64..=640-x).contains(&w))||!v["h"].as_u64().is_some_and(|h|(64..=480-y).contains(&h))||!v["content_height"].as_u64().is_some_and(|h|(v["h"].as_u64().unwrap()..=2048).contains(&h)){return Err(fail());}
        let key=v["offset_var"].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
        if runtime.is_some_and(|r|r["input"]["engine"]!="p4-input-v1"||r["vars"][key]["type"]!="int"||["scene","game","data","media","instrument"].iter().any(|k|r.get(*k).is_some())){return Err(fail());}
        if viewports.is_some(){
            let id=v["id"].as_str().filter(|s|!s.is_empty()&&s.len()<=15&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            for previous in &declared[..i]{
                let px=previous["x"].as_u64().unwrap();let py=previous["y"].as_u64().unwrap();
                if previous["id"]==id||previous["offset_var"]==key||(x<px+previous["w"].as_u64().unwrap()&&x+v["w"].as_u64().unwrap()>px&&y<py+previous["h"].as_u64().unwrap()&&y+v["h"].as_u64().unwrap()>py){return Err(fail());}
            }
        }
    }
    if let Some(v)=normalized.as_object_mut().unwrap().remove("scene_view"){
        if ["game_view","data_view"].iter().any(|k|s.get(k).is_some())||!v.as_object().is_some_and(|o|o.len()==7&&o.keys().all(|k|["engine","x","y","w","h","colors","sprites"].contains(&k.as_str())))||v["engine"]!="p4-scene-surface-v1"{return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=64&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=64&&h<=480-y)||!v["colors"].as_array().is_some_and(|a|a.len()==5&&a.iter().all(|c|c.as_u64().is_some_and(|v|v<=65535))){return Err(fail());}
        if let Some(r)=runtime{if r.get("scene").is_none()||r["scene"]["engine"]=="p4-touch-slice-v1"||["host","data","game","media","instrument"].iter().any(|k|r.get(k).is_some())||r["scene"].get("sprites").is_some()||r["scene"]["entities"].as_array().is_some_and(|a|a.iter().any(|e|e.get("sprite").is_some()))||["rows","solid_tones"].iter().any(|k|r["scene"]["grid"].get(k).is_some()){return Err(fail());}}
        let mut seen=std::collections::HashSet::new();
        for sp in v["sprites"].as_array().filter(|a|a.len()<=limits.scene_sprites).ok_or_else(fail)?{
            if !sp.as_object().is_some_and(|o|o.len()==4&&o.keys().all(|k|["entity","image","columns","fps"].contains(&k.as_str()))){return Err(fail());}
            let entity=sp["entity"].as_str().filter(|s|!s.is_empty()&&s.len()<=15&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if !seen.insert(entity)||runtime.is_some_and(|r|!r["scene"]["entities"].as_array().is_some_and(|a|a.iter().any(|e|e["id"]==entity))){return Err(fail());}
            let im=s["images"].as_array().and_then(|a|a.iter().find(|im|im["id"]==sp["image"])).ok_or_else(fail)?;
            let columns=sp["columns"].as_u64().filter(|n|(1..=8).contains(n)).ok_or_else(fail)?;
            if im.get("encoding").is_some()||!im["frame_width"].as_u64().is_some_and(|w|w%columns==0&&w/columns>=8)||!sp["fps"].as_u64().is_some_and(|n|(1..=20).contains(&n)){return Err(fail());}
        }
    }
    if let Some(v)=normalized.as_object_mut().unwrap().remove("data_view"){
        if s.get("game_view").is_some()||!v.as_object().is_some_and(|o|o.len()==7&&o.keys().all(|k|["engine","x","y","w","h","layout","colors"].contains(&k.as_str())))||v["engine"]!="p4-data-surface-v1"||!matches!(v["layout"].as_str(),Some("todos"|"metrics"|"rows")){return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=320&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=300&&h<=480-y)||!v["colors"].as_array().is_some_and(|a|a.len()==5&&a.iter().all(|c|c.as_u64().is_some_and(|v|v<=65535))){return Err(fail());}
        if let Some(r)=runtime{if r.get("data").is_none()||["scene","game","media","instrument"].iter().any(|k|r.get(k).is_some()) || r.get("host").is_some_and(|h|h!=true){return Err(fail());}}
    }
    if let Some(v)=normalized.as_object_mut().unwrap().remove("game_view"){
        if !v.as_object().is_some_and(|o|o.len()==5&&o.keys().all(|k|["engine","x","y","w","h"].contains(&k.as_str())))||v["engine"]!="p4-game-surface-v1"{return Err(fail());}
        let x=v["x"].as_u64().filter(|n|*n<640).ok_or_else(fail)?;
        let y=v["y"].as_u64().filter(|n|*n<480).ok_or_else(fail)?;
        if !v["w"].as_u64().is_some_and(|w|w>=64&&w<=640-x)||!v["h"].as_u64().is_some_and(|h|h>=64&&h<=480-y){return Err(fail());}
        if let Some(r)=runtime{if !matches!(r["game"]["type"].as_str(),Some("snake"|"blocks"|"flappy"|"gomoku"))||["host","scene","data","media","instrument"].iter().any(|k|r.get(k).is_some()){return Err(fail());}}
    }
    let typography=s.get("typography").is_some();
    if typography&&s["typography"]!="p4-app-type-v1"{return Err(fail());}
    let interaction=s.get("interaction").is_some();
    if interaction&&(!typography||s["interaction"]!="p4-app-interaction-v1"){return Err(fail());}
    normalized.as_object_mut().unwrap().remove("interaction");
    normalized.as_object_mut().unwrap().remove("typography");
    if let Some(fonts)=normalized.as_object_mut().unwrap().remove("fonts"){
        if !typography{return Err(fail());}
        let fonts=fonts.as_array().filter(|a|!a.is_empty()&&a.len()<=limits.fonts).ok_or_else(fail)?;
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
            if im.get("encoding").is_some()||w==0||h==0||w>limits.font_width||h>240||w%cw!=0||h%ch!=0{return Err(fail());}
            let chars:Vec<char>=f["chars"].as_str().ok_or_else(fail)?.chars().collect();
            let metrics=f["metrics"].as_array().ok_or_else(fail)?;
            if chars.is_empty()||chars.len()>limits.font_chars||chars.len() as u64>(w/cw)*(h/ch)||metrics.len()!=chars.len(){return Err(fail());}
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
        let images_array=images.as_array_mut().filter(|a|!a.is_empty()&&a.len()<=limits.images).ok_or_else(fail)?;
        for (i,im) in images_array.iter_mut().enumerate(){
            if let Some(encoding)=im.as_object_mut().ok_or_else(fail)?.remove("encoding"){
                if encoding!="jpeg"||i!=0||im["frame_width"]!=640||im["frame_height"]!=480{return Err(fail());}
                // Keyframe frames/fps were checked above; the legacy validator only knows frames=fps=1.
                im["frames"]=1.into();im["fps"]=1.into();
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
    let node_cap=if runtime.is_some_and(|r|r["quota"]=="p4-quota-v3"){64}else if runtime.is_some_and(|r|r["quota"]=="p4-quota-v2"){48}else{32};
    let nodes=normalized["elements"].as_array_mut().filter(|a|!a.is_empty()&&a.len()<=node_cap).ok_or_else(fail)?;
    for (i,n) in nodes.iter_mut().enumerate(){
        let mut t=n["type"].as_str().ok_or_else(fail)?.to_owned();
        if let Some(page)=n.get("page") {
            let page=page.as_u64().filter(|p|*p<=if shell {11} else {4}).ok_or_else(fail)?;
            if runtime.is_some_and(|r| (shell || page!=4) && page as usize>=r["pages"].as_array().map_or(1,|p|p.len().max(1))) {return Err(fail());}
            // All real page indices have been checked; project to legacy geometry validation.
            n["page"]=serde_json::json!(if shell {page.min(3)} else {page});
        }
        if let Some(field)=n.get("row_bind") {
            if !variable_bindings || !field.as_str().is_some_and(|f|crate::app_data_rows::BIND_FIELDS.contains(&f)) || !matches!(t.as_str(),"text"|"button") || ["text","bind","var_bind","field_bind","key_for"].iter().any(|k|n.get(*k).is_some()) || s.get("data_view").is_none() || runtime.is_some_and(|r|r.get("data").is_none()) {return Err(fail());}
            n.as_object_mut().unwrap().remove("row_bind");n["bind"]="headline".into();
        }
        if n.get("text_at").is_some() {
            if !variable_bindings {return Err(fail());}
            app_texts::validate_node(n,runtime)?;
            n.as_object_mut().unwrap().remove("text_at");n["bind"]="headline".into();
        }
        if t=="longtext" || t=="figure" {
            if !variable_bindings {return Err(fail());}
            // Validate the original declaration before projecting it to legacy geometry.
            let original=&s["elements"][i];
            let count=if t=="longtext" {crate::app_longtext::validate_longtext(original,&texts,&mut used_texts,runtime)?} else {crate::app_longtext::validate_figure(original,s)?};
            expanded+=count-1;
            for key in ["text_id","page_var","count_var","caption","image"] {n.as_object_mut().unwrap().remove(key);}
            n["type"]="text".into();n["text"]="".into();t="text".into();
        } else if ["text_id","page_var","count_var","caption"].iter().any(|k|n.get(*k).is_some()) {return Err(fail());}
        if t=="cells"{validate_cells_node(&s["elements"][i],s,runtime,variable_bindings)?;}
        else if n.get("columns").is_some()||n.get("cursor").is_some(){return Err(fail());}
        app_form::validate_field_node(n,runtime)?;
        if n.as_object_mut().ok_or_else(fail)?.remove("field_bind").is_some(){n["bind"]="headline".into();}
        let inside=n.as_object_mut().ok_or_else(fail)?.remove("in_view");
        let named=n.as_object_mut().ok_or_else(fail)?.remove("view");
        if inside.is_some()&&named.is_some(){return Err(fail());}
        let node_view=if let Some(id)=&named{
            let id=id.as_str().ok_or_else(fail)?;
            Some(declared.iter().copied().find(|v|viewports.is_some()&&v["id"]==id).ok_or_else(fail)?)
        }else if let Some(inside)=inside{Some(viewport.as_ref().filter(|_|inside==true).ok_or_else(fail)?)}else{None};
        if let Some(v)=node_view{
            let x=n["x"].as_u64().ok_or_else(fail)?;let y=n["y"].as_u64().ok_or_else(fail)?;
            let w=n["w"].as_u64().ok_or_else(fail)?;let h=n["h"].as_u64().ok_or_else(fail)?;
            if !x.checked_add(w).is_some_and(|end|end<=v["w"].as_u64().unwrap())||!y.checked_add(h).is_some_and(|end|end<=v["content_height"].as_u64().unwrap())||h>480||n.get("motion").is_some()||s["images"].as_array().is_some_and(|a|a.iter().any(|im|im["id"]==n["image"]&&im.get("encoding").is_some())){return Err(fail());}
            n["y"]=0.into();
        }
        if let Some(binding)=n.as_object_mut().ok_or_else(fail)?.remove("var_bind"){
            let name=binding.as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if !variable_bindings||!matches!(t.as_str(),"text"|"button"|"meter")||["bind","text","key_for"].iter().any(|k|n.get(*k).is_some()){return Err(fail());}
            if let Some(r)=runtime{
                if !matches!(r["vars"][name]["type"].as_str(),Some("int"|"string"))||t=="meter"&&r["vars"][name]["type"]!="int"{return Err(fail());}
            }
            // Reuse the existing geometry/typography checks after validating
            // the typed variable reference; do not create a permissive path.
            n["bind"]=serde_json::json!(if t=="meter"{"progress"}else{"headline"});
        }
        if let Some(mask)=n.as_object_mut().ok_or_else(fail)?.remove("state_mask"){
            let max_mask=if runtime.is_some_and(|r|r["quota"]=="p4-quota-v3"){65535}else{63};
            let m=mask.as_u64().filter(|m|(1..=max_mask).contains(m)).ok_or_else(fail)?;
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
        if t=="cells"{
            for field in ["image","columns","cursor"]{n.as_object_mut().unwrap().remove(field);}
            n["type"]="panel".into();
        }
        if matches!(t.as_str(),"text"|"icon"|"button"){
            // Content/typography was checked above; use the old geometry checks
            // without imposing its legacy 32px line box on 18px keycaps.
            n["type"]="panel".into();
        }
    }
    if nodes.len()+expanded>node_cap {return Err(fail());}
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
pub const CATALOG_PATH: &str = "pc/catalog.json";
pub const CATALOG_MAX_BYTES: usize = 512 * 1024;
pub const CATALOG_MAX_ROWS: usize = 2000;
/// Same key as media_catalog: trimmed, lower-case, no whitespace, no trailing 市.
pub(crate) fn catalog_key(s: &str) -> String {
    s.trim().trim_end_matches('市').chars().filter(|c| !c.is_whitespace()).flat_map(char::to_lowercase).collect()
}
impl Extension {
    /// Bundled catalogs are authoritative, including for old favorites/history.
    /// Match both ID and URL so a reused station ID cannot authorize another feed.
    pub fn allows_station(&self, station: &crate::media_network::Station) -> bool {
        self.search.catalog.is_none() || self.catalog_rows.iter().any(|row| {
            self.map_results(&serde_json::json!([row]), "")
                .iter().any(|s|s.id==station.id && s.url==station.url)
        })
    }
    /// Flat JSON array of objects with text/number/bool values only; bounded size.
    pub fn validate_catalog_bytes(bytes: &[u8]) -> Result<Vec<Value>, String> {
        if bytes.len() > CATALOG_MAX_BYTES {
            return Err("包内电台目录超过 512 KiB".into());
        }
        let value: Value = serde_json::from_slice(bytes).map_err(|_| "包内电台目录不是有效 JSON")?;
        let rows = value.as_array().ok_or("包内电台目录必须是对象数组")?;
        if rows.is_empty() || rows.len() > CATALOG_MAX_ROWS {
            return Err(format!("包内电台目录需要 1-{CATALOG_MAX_ROWS} 条"));
        }
        for row in rows {
            let object = row.as_object().ok_or("包内电台目录的条目必须是对象")?;
            if object.len() > 16 {
                return Err("包内电台目录的条目字段过多".into());
            }
            for (key, field) in object {
                if !short(key, 40) || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_') {
                    return Err("包内电台目录的字段名无效".into());
                }
                match field {
                    Value::String(s) if s.len() > 2048 || s.chars().any(char::is_control) => {
                        return Err("包内电台目录的文本字段无效".into())
                    }
                    Value::String(_) | Value::Number(_) | Value::Bool(_) | Value::Null => {}
                    _ => return Err("包内电台目录的条目只能包含文本、数字和布尔值".into()),
                }
            }
        }
        Ok(rows.clone())
    }
    /// Load the declared bundled catalog from the formal package directory.
    pub fn load_catalog(&mut self, package_dir: &std::path::Path) -> Result<(), String> {
        if self.search.catalog.is_none() {
            return Ok(());
        }
        let bytes = std::fs::read(package_dir.join(CATALOG_PATH)).map_err(|_| "无法读取包内电台目录")?;
        self.catalog_rows = Self::validate_catalog_bytes(&bytes)?;
        Ok(())
    }
    /// Offline search over bundled rows: title contains the query (or its city
    /// alias), or the row's region equals the query / its region alias.
    pub fn catalog_search(&self, query: &str) -> Vec<Value> {
        let key = catalog_key(query);
        let d = self.discovery.as_ref();
        let lookup = |map: &BTreeMap<String, String>| {
            map.get(query.trim())
                .or_else(|| map.get(query.trim().trim_end_matches('市')))
                .map(|s| catalog_key(s))
                .unwrap_or_default()
        };
        let city = d.map(|d| lookup(&d.aliases)).unwrap_or_default();
        let region = d.map(|d| lookup(&d.region_aliases)).unwrap_or_default();
        let f = &self.search.fields;
        let mut rows: Vec<&Value> = self
            .catalog_rows
            .iter()
            .filter(|r| {
                let title = catalog_key(r.pointer(&f.title).and_then(Value::as_str).unwrap_or(""));
                let row_region = d
                    .map(|d| catalog_key(r.pointer(&d.station_region).and_then(Value::as_str).unwrap_or("")))
                    .unwrap_or_default();
                !key.is_empty()
                    && (title.contains(&key)
                        || (!city.is_empty() && title.contains(&city))
                        || row_region == key
                        || (!region.is_empty() && row_region == region))
            })
            .collect();
        if let Some(d) = d {
            rows.sort_by_key(|r| {
                std::cmp::Reverse((
                    r.pointer(&d.popularity).and_then(Value::as_u64).unwrap_or(0),
                    r.pointer(&d.votes).and_then(Value::as_u64).unwrap_or(0),
                ))
            });
        }
        rows.into_iter().take(200).cloned().collect()
    }
    pub fn validate(&self) -> Result<(), String> {
        let fail = || Err("PC 扩展声明无效：需要受控联网、媒体、存储及列表面板".into());
        if self.schema_version != 1
            || self.engine != "pc-declarative-media-v1"
            || !self.permissions.public_audio
            || !self.permissions.storage
        {
            return fail();
        }
        let offline = self.search.catalog.is_some();
        if self.search.catalog.as_deref().is_some_and(|path| path != CATALOG_PATH) {
            return fail();
        }
        let hosts = &self.permissions.network_hosts;
        if (hosts.is_empty() && !offline)
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
        if offline {
            // Bundled rows: no remote search endpoint and rows must be a root array.
            if !self.search.endpoint.is_empty() || !self.search.items_path.is_empty() {
                return fail();
            }
        } else {
            let endpoint = crate::media_network::valid_url(&self.search.endpoint)?;
            if endpoint.scheme() != "https"
                || endpoint.query().is_some()
                || !hosts
                    .iter()
                    .any(|h| Some(h.as_str()) == endpoint.host_str())
            {
                return fail();
            }
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
            // Offline catalogs carry no mirrors; remote discovery needs 1–3 and a location service.
            let mirrors_ok = if offline { d.endpoints.is_empty() } else { !d.endpoints.is_empty() && d.endpoints.len() <= 3 && !d.location_endpoint.is_empty() };
            if !self.search.items_path.is_empty() || !mirrors_ok || !(1..=100).contains(&d.national_count)
                || !(1..=30).contains(&d.local_supplement) || !(3600..=604800).contains(&d.refresh_sec)
                || d.country.len()!=2 || !d.country.bytes().all(|c|c.is_ascii_uppercase())
                || d.aliases.len()>80 || d.aliases.iter().any(|(a,b)|!short(a,80)||!short(b,80))
                || d.region_aliases.len()>80 || d.region_aliases.iter().any(|(a,b)|!short(a,80)||!short(b,80))
                || [&d.city_field,&d.country_field,&d.station_region,&d.popularity,&d.votes].iter().any(|s|!pointer(s)) { return fail(); }
            for raw in d.endpoints.iter().chain((!d.location_endpoint.is_empty()).then_some(&d.location_endpoint)) {
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
    fn scroll_viewport_is_bounded_and_requires_typed_input() {
        let r=serde_json::json!({"input":{"engine":"p4-input-v1"},"vars":{"scroll":{"type":"int","init":0}},"surface":{"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","bindings":"p4-variable-bindings-v1","background":0,"viewport":{"engine":"p4-app-viewport-v1","x":16,"y":64,"w":608,"h":352,"content_height":720,"offset_var":"scroll"},"elements":[{"type":"text","x":20,"y":600,"w":200,"h":32,"color":65535,"text":"History","in_view":true}]}});
        assert!(validate_app_surface(&r["surface"],Some(&r)).is_ok());
        for (path,value) in [
            ("/surface/viewport/engine",serde_json::json!("unknown")),("/surface/viewport/h",serde_json::json!(true)),
            ("/surface/viewport/content_height",serde_json::json!(2049)),("/surface/viewport/offset_var",serde_json::json!("missing")),
            ("/surface/elements/0/in_view",serde_json::json!(false)),("/surface/elements/0/y",serde_json::json!(710)),
            ("/surface/elements/0/x",serde_json::json!(u64::MAX)),("/surface/elements/0/y",serde_json::json!(u64::MAX)),
            ("/vars/scroll/type",serde_json::json!("string")),("/input/engine",serde_json::json!("unknown"))
        ] {let mut bad=r.clone();*bad.pointer_mut(path).unwrap()=value;assert!(validate_app_surface(&bad["surface"],Some(&bad)).is_err(),"{path}");}
        let mut bad=r.clone();bad["scene"]=serde_json::json!({});assert!(validate_app_surface(&bad["surface"],Some(&bad)).is_err());
    }
    #[test]
    fn multiple_scroll_viewports_shared_contract_and_strict_ownership() {
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/app-viewports-v1.json")).unwrap();
        let surface=&suite["surface"];
        let runtime=serde_json::json!({"vars":{"left_offset":{"type":"int"},"right_offset":{"type":"int"}},"input":{"engine":"p4-input-v1"},"transitions":[{"on":"left.row"},{"on":"right.row"},{"on":"fixed"}]});
        assert!(validate_app_surface(surface,Some(&runtime)).is_ok());
        let mut host=runtime.clone();host["host"]=true.into();assert!(validate_app_surface(surface,Some(&host)).is_ok());
        for case in suite["invalid"].as_array().unwrap(){
            let mut bad=surface.clone();*bad.pointer_mut(case["pointer"].as_str().unwrap()).unwrap()=case["value"].clone();
            assert!(validate_app_surface(&bad,Some(&runtime)).is_err(),"{}",case["pointer"]);
        }
        for key in ["scene","game","data","media","instrument"]{let mut bad=runtime.clone();bad[key]=serde_json::json!({});assert!(validate_app_surface(surface,Some(&bad)).is_err(),"{key}");}
        for (key,value) in [("in_view",serde_json::json!(true)),("motion",serde_json::json!("float")),("unknown",serde_json::json!(true))]{let mut bad=surface.clone();bad["elements"][0][key]=value;assert!(validate_app_surface(&bad,Some(&runtime)).is_err());}
        let mut bad=surface.clone();bad["viewport"]=serde_json::json!({});assert!(validate_app_surface(&bad,Some(&runtime)).is_err());
        let mut bad=surface.clone();bad["viewports"]["items"][0]["unknown"]=true.into();assert!(validate_app_surface(&bad,Some(&runtime)).is_err());
        let mut bad=runtime.clone();bad["vars"]["right_offset"]["type"]="string".into();assert!(validate_app_surface(surface,Some(&bad)).is_err());
        let mut bad=runtime.clone();bad["input"]["engine"]="unknown".into();assert!(validate_app_surface(surface,Some(&bad)).is_err());
        let mut three=surface.clone();three["viewports"]["items"].as_array_mut().unwrap().push(serde_json::json!({"id":"bottom","x":16,"y":400,"w":608,"h":64,"content_height":2048,"offset_var":"third_offset"}));
        assert!(validate_app_surface(&three,None).is_ok());
        let item=three["viewports"]["items"][2].clone();three["viewports"]["items"].as_array_mut().unwrap().push(item);assert!(validate_app_surface(&three,None).is_err());
        let mut adjacent=surface.clone();adjacent["viewports"]["items"][1]["x"]=304.into();assert!(validate_app_surface(&adjacent,None).is_ok());
    }
    #[test]
    fn named_surface_variables_require_capability_schema_and_declared_types(){
        let mut r=serde_json::json!({"vars":{"label":{"type":"string","init":""},"level":{"type":"int","init":0}},"surface":{"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","bindings":"p4-variable-bindings-v1","background":0,"elements":[{"type":"text","x":20,"y":20,"w":200,"h":32,"color":65535,"var_bind":"label"},{"type":"meter","x":20,"y":60,"w":200,"h":12,"color":65535,"var_bind":"level"}]}});
        assert!(validate_app_surface(&r["surface"],Some(&r)).is_ok());
        for (pointer,value) in [("/surface/elements/0/var_bind",serde_json::json!("missing")),("/surface/elements/1/var_bind",serde_json::json!("label")),("/surface/bindings",serde_json::json!("unknown"))]{let mut bad=r.clone();*bad.pointer_mut(pointer).unwrap()=value;assert!(validate_app_surface(&bad["surface"],Some(&bad)).is_err());}
        r["surface"]["elements"][0]["text"]="duplicate".into();assert!(validate_app_surface(&r["surface"],Some(&r)).is_err());
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
        e.search.fields.url = "/url_resolved".into();
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
    fn bundled_catalog_is_declared_bounded_and_searchable_offline() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/city-radio");
        // This case needs the actual bundled/offline declaration. Keep the
        // separate remote media-sample fixture for undeclared-host regressions.
        let bundled = || parse(include_bytes!("../../builtin-clawpkgs/city-radio/pc/extension.json")).unwrap();
        let mut e = bundled();
        assert_eq!(e.search.catalog.as_deref(), Some(CATALOG_PATH));
        assert!(e.search.endpoint.is_empty() && e.discovery.as_ref().unwrap().endpoints.is_empty());
        e.load_catalog(&root).unwrap();
        // Offline search: title match, English alias, province alias; never a network call.
        let nanjing = e.catalog_search("南京");
        assert!(nanjing.iter().any(|r| r["name"].as_str().unwrap().starts_with("江苏")));
        assert!(!e.catalog_search("Shanghai").is_empty());
        assert!(e.catalog_search("不存在的电台xyz").is_empty());
        let mapped = e.map_results(&Value::Array(e.catalog_search("新闻")), "新闻");
        assert!(!mapped.is_empty() && mapped.len() <= 20);
        assert!(e.allows_station(&mapped[0]));
        let mut stale=mapped[0].clone();stale.url="https://example.com/old-feed".into();
        assert!(!e.allows_station(&stale));
        let mut remote=e.clone();remote.search.catalog=None;
        assert!(remote.allows_station(&stale));
        // Declaration bounds: a remote endpoint or mirrors next to a bundled catalog are rejected.
        let mut bad = bundled();
        bad.search.endpoint = "https://ipwho.is/search".into();
        assert!(bad.validate().is_err());
        let mut bad = bundled();
        bad.discovery.as_mut().unwrap().endpoints = vec!["https://ipwho.is/list".into()];
        assert!(bad.validate().is_err());
        let mut bad = bundled();
        bad.search.catalog = Some("pc/other.json".into());
        assert!(bad.validate().is_err());
        // Row bounds mirror the clawpkg gate.
        assert!(Extension::validate_catalog_bytes(b"[]").is_err());
        assert!(Extension::validate_catalog_bytes(br#"[{"id":"a","nested":{"x":1}}]"#).is_err());
        assert!(Extension::validate_catalog_bytes(br#"[{"bad key":"a"}]"#).is_err());
        assert!(Extension::validate_catalog_bytes(r#"[{"id":"a","name":"台","url":"http://example.com/a","rank":1,"hls":false,"available":true}]"#.as_bytes()).is_ok());
        let huge = vec![b' '; CATALOG_MAX_BYTES + 1];
        assert!(Extension::validate_catalog_bytes(&huge).is_err());
        // A package that declares the catalog but omits the file is invalid; the real package is valid.
        assert!(crate::clawpkg::validate_clawpkg_at_path(&root).unwrap().ok);
        let temp = tempfile::tempdir().unwrap();
        for name in ["component.json", "negative-screen.json", "buttons.json", "share.json", "runtime/widget.json", "pc/extension.json", "assets/.keep"] {
            let path = temp.path().join(name);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::copy(root.join(name), path).unwrap();
        }
        let result = crate::clawpkg::validate_clawpkg_at_path(temp.path()).unwrap();
        assert!(result.errors.iter().any(|e| e.contains("pc/catalog.json")), "{:?}", result.errors);
    }
    #[test]
    fn surface_and_package_validation_reject_cross_owner_and_private_files() {
        let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../tests/fixtures/media-sample");
        assert!(extension().search.catalog.is_none(), "remote fixture must keep remote host-policy coverage");
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

#[cfg(test)]
mod persistence_budget_tests {
    #[test]
    fn all_profile_subsets_fit_the_worst_escaped_storage_budget() {
        let vectors:serde_json::Value=serde_json::from_str(include_str!("../../skills/petui/contracts/store-budget-v1.json")).unwrap();
        for case in vectors["cases"].as_array().unwrap(){
            assert_eq!(super::validate_app_persistence(&case["runtime"]).is_ok(),case["ok"].as_bool().unwrap(),"{}",case["name"]);
        }
    }
}
