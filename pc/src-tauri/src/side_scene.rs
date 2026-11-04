//! Strict opt-in tile-world declaration. No scripts, networking or game IDs.
use serde::Deserialize;
use serde_json::Value;
use std::collections::HashSet;
pub const ENGINE:&str="p4-side-scene-v1";
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Tile{flags:u8,#[serde(default)]reward:u16,#[serde(default)]replacement:u8}
fn yes()->bool{true}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Actor{
    id:String,behavior:String,contact:String,x:u16,y:u16,width:u8,height:u8,skin:u8,
    #[serde(default)]speed:i16,#[serde(default)]range_min:u16,#[serde(default)]range_max:u16,
    #[serde(default)]value:u16,#[serde(default)]vertical:bool,#[serde(default)]solid:bool,
    #[serde(default)]stompable:bool,#[serde(default="yes")]edge_turn:bool,
}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Physics{run_speed:u16,acceleration:u16,friction:u16,gravity:u16,jump_speed:u16,terminal_speed:u16,lives:u8}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Actions{left:String,right:String,jump:String,pause:String}
fn sixteen()->u8{16}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Skin{idle:Vec<u8>,walk:Vec<u8>,air:Vec<u8>,hurt:Vec<u8>,fps:u8,#[serde(default="sixteen")]width:u8,#[serde(default="sixteen")]height:u8}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Art{image:String,columns:u8,rows:u8,scale:u8,x:u16,y:u16,w:u16,h:u16,sky:u16,tiles:Vec<u8>,skins:Vec<Skin>}
#[derive(Deserialize)]#[serde(deny_unknown_fields)]
struct Scene{engine:String,width:u8,height:u8,rows:Vec<String>,tiles:Vec<Tile>,actors:Vec<Actor>,physics:Physics,actions:Actions,art:Art,active_state:String,result_state:String,win_state:String,score_var:String,coins_var:String,lives_var:String,checkpoint_var:String}
fn name(s:&str,n:usize)->bool{!s.is_empty()&&s.len()<=n&&s.bytes().all(|c|c.is_ascii_alphanumeric()||b"._-".contains(&c))}
fn tile_at(s:&Scene,x:u16,y:u16)->Option<u8>{s.rows.get(y as usize)?.as_bytes().get(x as usize).and_then(|b|match b{b'0'..=b'9'=>Some(b-b'0'),b'a'..=b'f'=>Some(b-b'a'+10),_=>None})}
pub fn validate_runtime(r:&Value)->Result<(),String>{
    let bad=||"横版场景需要有界地图、匹配图集、类型安全状态与输入；不允许脚本或未知字段".to_string();
    let s:Scene=serde_json::from_value(r["scene"].clone()).map_err(|_|bad())?;
    if s.engine!=ENGINE||r["engine"]!="p4-bounded-runtime-v4"||!r["tick"].as_array().is_some_and(Vec::is_empty)
        ||["host","game","data","media","instrument","local","lifecycle","pages"].iter().any(|k|r.get(k).is_some())
        ||r["surface"]["engine"]!="p4-app-surface-v1"||r["surface"]["graphics"]!="p4-app-graphics-v1"
        ||["game_view","scene_view","data_view"].iter().any(|k|r["surface"].get(k).is_some())
        ||!(20..=160).contains(&s.width)||!(8..=16).contains(&s.height)||s.tiles.is_empty()||s.tiles.len()>16||s.actors.is_empty()||s.actors.len()>24||s.rows.len()!=s.height as usize{return Err(bad());}
    for t in &s.tiles{if t.flags>15||t.replacement as usize>=s.tiles.len()||t.reward>10000||t.flags&2!=0&&t.flags&5!=0{return Err(bad());}}
    if s.tiles[0].flags!=0||s.tiles[0].reward!=0{return Err(bad());}
    if s.tiles.iter().any(|t|t.reward>0&&s.tiles[t.replacement as usize].reward>0){return Err(bad());}
    for (y,row)in s.rows.iter().enumerate(){if row.len()!=s.width as usize||!row.bytes().enumerate().all(|(x,_)|tile_at(&s,x as u16,y as u16).is_some_and(|v|(v as usize)<s.tiles.len())){return Err(bad());}}
    let p=&s.physics;if !(32..=240).contains(&p.run_speed)||!(100..=2000).contains(&p.acceleration)||!(100..=2400).contains(&p.friction)||!(200..=1800).contains(&p.gravity)||!(100..=600).contains(&p.jump_speed)||!(240..=640).contains(&p.terminal_speed)||!(1..=9).contains(&p.lives){return Err(bad());}
    let mut ids=HashSet::new();let mut controlled=0;
    for a in &s.actors{
        if !name(&a.id,15)||!ids.insert(&a.id)||!(1..=32).contains(&a.width)||!(1..=32).contains(&a.height)||u32::from(a.x)+u32::from(a.width)>u32::from(s.width)*16||u32::from(a.y)+u32::from(a.height)>u32::from(s.height)*16||a.skin>7||a.value>10000||!(-160..=160).contains(&a.speed)||a.range_min>2560||a.range_max>2560||!["none","damage","collect","goal","checkpoint"].contains(&a.contact.as_str()){return Err(bad());}
        match a.behavior.as_str(){
            "controlled"=>{controlled+=1;if a.contact!="none"||a.solid||a.speed!=0{return Err(bad());}for y in a.y/16..=(a.y+u16::from(a.height)-1)/16{for x in a.x/16..=(a.x+u16::from(a.width)-1)/16{if s.tiles[tile_at(&s,x,y).ok_or_else(bad)? as usize].flags&1!=0{return Err(bad());}}}},
            "static"=>if a.speed!=0||a.solid{return Err(bad());},
            "walker"|"platform"=>{
                let(position,extent,size)=if a.vertical{(a.y,u16::from(s.height)*16,a.height)}else{(a.x,u16::from(s.width)*16,a.width)};
                if a.speed==0||a.range_min>a.range_max||position<a.range_min||position>a.range_max||u32::from(a.range_max)+u32::from(size)>u32::from(extent)||a.behavior=="walker"&&(a.vertical||a.solid)||a.behavior=="platform"&&(!a.solid||a.contact!="none"){return Err(bad());}
            },_=>return Err(bad()),
        }
        let _=(a.stompable,a.edge_turn);
    }
    if controlled!=1{return Err(bad());}
    let states=r["states"].as_array().ok_or_else(bad)?;
    let names=[&s.active_state,&s.result_state,&s.win_state];let mut distinct=HashSet::new();
    for name in names{if !crate_name(name,23)||!distinct.insert(name)||!states.iter().any(|v|v==name){return Err(bad());}}
    let variables=[&s.score_var,&s.coins_var,&s.lives_var,&s.checkpoint_var];let mut distinct=HashSet::new();
    for (index,key)in variables.iter().enumerate(){if !name(key,31)||!distinct.insert(*key)||r["vars"][*key]["type"]!="int"{return Err(bad());}
        if let Some(bounds)=r["limits"].get(*key){let maximum=[1000000000,65535,i64::from(p.lives),s.actors.len()as i64][index];if !bounds[0].as_i64().is_some_and(|n|n<=0)||!bounds[1].as_i64().is_some_and(|n|n>=maximum){return Err(bad());}}
    }
    let actions=[&s.actions.left,&s.actions.right,&s.actions.jump,&s.actions.pause];let mut unique=HashSet::new();
    if actions.iter().any(|a|!name(a,47)||!unique.insert(*a))||r["input"]["engine"]!="p4-input-v1"||r["input"]["hold"]!=s.actions.jump{return Err(bad());}
    let ts=r["transitions"].as_array().ok_or_else(bad)?;let mut declared=HashSet::new();let mut pauses=Vec::new();
    for t in ts{let action=t["on"].as_str().ok_or_else(bad)?;declared.insert(action);if !actions.iter().any(|a|a.as_str()==action)||!t.as_object().is_some_and(|m|m.keys().all(|k|["from","on","to"].contains(&k.as_str()))){return Err(bad());}
        if action==s.actions.pause&&t["from"]==s.active_state{let pause=t["to"].as_str().filter(|v|states.iter().any(|s|s==v)&&!names.iter().any(|s|s.as_str()==*v)).ok_or_else(bad)?;pauses.push(pause);}
    }
    if declared.len()!=4||pauses.len()!=1||!ts.iter().any(|t|t["on"]==s.actions.pause&&t["from"]==pauses[0]&&t["to"]==s.active_state){return Err(bad());}
    let initial=r["initial_state"].as_str().ok_or_else(bad)?;
    if !states.iter().any(|s|s==initial)||names.iter().any(|s|initial==s.as_str())||initial==pauses[0]{return Err(bad());}
    let a=&s.art;let frames=u16::from(a.columns)*u16::from(a.rows);
    if !name(&a.image,15)||!(1..=32).contains(&a.columns)||!(1..=15).contains(&a.rows)||frames>256||!(1..=3).contains(&a.scale)||a.x>=640||a.y>=480||a.w<160||u32::from(a.x)+u32::from(a.w)>640||a.h<128||u32::from(a.y)+u32::from(a.h)>480||a.tiles.len()!=s.tiles.len()||a.tiles.iter().any(|v|u16::from(*v)>=frames)||a.skins.is_empty()||a.skins.len()>8{return Err(bad());}
    let image=r["surface"]["images"].as_array().and_then(|v|v.iter().find(|im|im["id"]==a.image)).ok_or_else(bad)?;
    if u32::from(a.h)<u32::from(s.height)*16*u32::from(a.scale){return Err(bad());}
    if image.get("encoding").is_some()||image["frame_width"]!=u16::from(a.columns)*16||image["frame_height"]!=u16::from(a.rows)*16||s.actors.iter().any(|e|e.skin as usize>=a.skins.len()){return Err(bad());}
    for skin in &a.skins{if !(1..=20).contains(&skin.fps)||!(8..=32).contains(&skin.width)||!(8..=32).contains(&skin.height){return Err(bad());}for list in [&skin.idle,&skin.walk,&skin.air,&skin.hurt]{if list.is_empty()||list.len()>8||list.iter().any(|n|u16::from(*n)>=frames){return Err(bad());}}}
    let _=a.sky;Ok(())
}
fn crate_name(s:&str,n:usize)->bool{name(s,n)}
pub fn validate_buttons(r:&Value,buttons:&Value)->Result<(),String>{
    let s=&r["scene"];for (action,event)in [("left","knob.rotate_ccw"),("right","knob.rotate_cw"),("jump","button.sw1.short_press"),("pause","button.sw2.short_press")]{if !buttons.as_array().is_some_and(|v|v.iter().any(|b|b["action"]==s["actions"][action]&&b["event"]==event)){return Err("横版场景需要左右摇杆、跳跃与暂停的实际输入映射".into());}}Ok(())
}

#[cfg(test)]mod tests{
    use super::*;
    #[test]#[ignore="explicit authored side scene, no device/audio"]
    fn authored_side_package_is_strict(){
        let root=std::path::PathBuf::from(std::env::var("PET_SIDE_PACKAGE").unwrap());let r:Value=serde_json::from_slice(&std::fs::read(root.join("runtime/widget.json")).unwrap()).unwrap();validate_runtime(&r).unwrap();
        let b:Value=serde_json::from_slice(&std::fs::read(root.join("buttons.json")).unwrap()).unwrap();validate_buttons(&r,&b).unwrap();
        for (pointer,value)in [("/scene/width",serde_json::json!(161)),("/scene/physics/lives",serde_json::json!(true)),("/scene/actors/0/behavior",serde_json::json!("script")),("/scene/art/skins/0/idle",serde_json::json!([]))]{let mut bad=r.clone();*bad.pointer_mut(pointer).unwrap()=value;assert!(validate_runtime(&bad).is_err(),"{pointer}");}
    }
}
