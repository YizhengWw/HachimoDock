//! r150 graphics capabilities: the p4-graphics-v2 quota profile, p4-keyframe-bg-v1
//! backgrounds, p4-particles-v1 emitters and p4-bodies-v1 negotiation. Data only:
//! every ceiling mirrors pc/skills/petui/scripts/app_surface.py and the firmware.
use serde_json::Value;
pub const GRAPHICS_V2:&str="p4-graphics-v2";
pub const PARTICLES_ENGINE:&str="p4-particles-v1";
pub const KEYFRAME_ENGINE:&str="p4-keyframe-bg-v1";
pub const BODIES_ENGINE:&str="p4-bodies-v1";
pub const PARTICLE_OPS:[&str;6]=["lte","lt","gte","gt","eq","ne"];
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub struct Limits{pub images:usize,pub image_pixels:u64,pub image_bytes:usize,pub fonts:usize,pub font_chars:usize,pub font_width:u64,pub scene_sprites:usize,pub sprite_pixels:i64,pub keyframes:u64}
pub const V1:Limits=Limits{images:4,image_pixels:98304,image_bytes:128*1024,fonts:2,font_chars:49,font_width:240,scene_sprites:4,sprite_pixels:4096,keyframes:1};
pub const V2:Limits=Limits{images:6,image_pixels:196608,image_bytes:256*1024,fonts:4,font_chars:98,font_width:512,scene_sprites:8,sprite_pixels:16384,keyframes:4};
/// Each keyframe body stays within one P4J1 (100000 B); the whole set within 400 KB of transfer.
pub const KEYFRAME_BYTES:usize=100000;
pub const KEYFRAME_SET_BYTES:usize=400000;
pub fn declared(runtime:Option<&Value>)->bool{runtime.is_some_and(|r|r["graphics"]==GRAPHICS_V2)}
/// p4-graphics-v2 ceilings apply only when the package names the profile.
pub fn limits(runtime:Option<&Value>)->Limits{if declared(runtime){V2}else{V1}}
pub fn validate_runtime(runtime:&Value)->Result<(),String>{
    if let Some(g)=runtime.get("graphics"){
        if g!=GRAPHICS_V2||runtime["engine"]!="p4-bounded-runtime-v4"{return Err("graphics 只支持 p4-graphics-v2 且需要 p4-bounded-runtime-v4".into());}
    }
    validate_bodies(runtime)?;
    Ok(())
}
/// (frames, fps) of a keyframe background declared on images[0], None for a static one.
pub fn keyframes(surface:&Value)->Option<(u64,u64)>{
    let im=surface["images"].as_array()?.first()?;
    if im["encoding"]!="jpeg"{return None;}
    let frames=im.get("frames").and_then(Value::as_u64).unwrap_or(1);
    if frames<=1{return None;}
    Some((frames,im.get("fps").and_then(Value::as_u64).unwrap_or(1)))
}
/// Source PNG of frame k>=2: assets/bg.png -> assets/bg.2.png. Frame 1 is the declared asset.
pub fn keyframe_asset(asset:&str,frame:u64)->String{
    if frame<2{asset.to_string()}else{format!("{}.{frame}.png",asset.strip_suffix(".png").unwrap_or(asset))}
}
/// Pack id of frame k>=2 on the device: "<id>.k" (declared ids stay <= 12 chars).
pub fn keyframe_id(id:&str,frame:u64)->String{if frame<2{id.to_string()}else{format!("{id}.{frame}")}}
/// Validate images[0].frames/fps against the profile; a static background always passes.
pub fn validate_keyframes(surface:&Value,runtime:Option<&Value>)->Result<(),String>{
    let fail=||"关键帧背景需要 graphics=p4-graphics-v2、首张 JPEG 背景、2-4 帧、1-2 fps 且 id 不超过 12 字符".to_string();
    let Some(im)=surface["images"].as_array().and_then(|a|a.first())else{return Ok(());};
    let frames=im.get("frames").and_then(Value::as_u64).ok_or_else(fail)?;
    let fps=im.get("fps").and_then(Value::as_u64).ok_or_else(fail)?;
    if im["encoding"]!="jpeg"||frames==1{return if frames==1&&fps==1{Ok(())}else{Err(fail())};}
    let max=limits(runtime).keyframes;
    if frames<2||frames>max||fps<1||fps>2||im["id"].as_str().is_none_or(|s|s.len()>12){return Err(fail());}
    Ok(())
}
/// Validate image budgets before adapting an app surface to a legacy node validator.
/// JPEG backgrounds have their own bounded storage; palette pixels are summed separately.
pub fn validate_image_budget(surface:&Value,runtime:Option<&Value>)->Result<u64,String>{
    let fail=||"图形图片数量、尺寸或解码像素预算超限".to_string();
    let Some(images)=surface.get("images")else{return Ok(0);};
    let quota=limits(runtime);
    let images=images.as_array().filter(|a|!a.is_empty()&&a.len()<=quota.images).ok_or_else(fail)?;
    validate_keyframes(surface,runtime)?;
    let mut pixels=0u64;
    for (i,image) in images.iter().enumerate(){
        let w=image["frame_width"].as_u64().ok_or_else(fail)?;
        let h=image["frame_height"].as_u64().ok_or_else(fail)?;
        if image.get("encoding").is_some(){
            if i!=0||image["encoding"]!="jpeg"||w!=640||h!=480{return Err(fail());}
        }else{
            if !(8..=512).contains(&w)||!(8..=240).contains(&h)||w*h>98304{return Err(fail());}
            if image["frames"]!=1||image["fps"]!=1{return Err(fail());}
            pixels=pixels.checked_add(w*h).ok_or_else(fail)?;
            if pixels>quota.image_pixels{return Err(fail());}
        }
    }
    Ok(pixels)
}
fn int(v:&Value,lo:i64,hi:i64)->Option<i64>{v.as_i64().filter(|n|(lo..=hi).contains(n))}
/// p4-particles-v1: 1-2 bounded decorative emitters; runtime (when given) owns action/var/page references.
pub fn validate_particles(surface:&Value,runtime:Option<&Value>)->Result<(),String>{
    let fail=||"particles 需要 1-2 个有界装饰发射器：矩形内、count/budget ≤64、已声明动作或 tick 触发、整数变量守卫".to_string();
    let Some(emitters)=surface.get("particles")else{return Ok(());};
    if surface["graphics"]!="p4-app-graphics-v1"{return Err(fail());}
    let emitters=emitters.as_array().filter(|a|(1..=2).contains(&a.len())).ok_or_else(fail)?;
    let required=["x","y","w","h","count","life_ms","speed","spread","gravity","color","shape","trigger","budget"];
    for e in emitters{
        let o=e.as_object().ok_or_else(fail)?;
        if !o.keys().all(|k|required.contains(&k.as_str())||k=="when"||k=="page")||!required.iter().all(|k|o.contains_key(*k)){return Err(fail());}
        let x=int(&e["x"],0,639).ok_or_else(fail)?;let y=int(&e["y"],0,479).ok_or_else(fail)?;
        int(&e["w"],1,640-x).ok_or_else(fail)?;int(&e["h"],1,480-y).ok_or_else(fail)?;
        let count=int(&e["count"],1,64).ok_or_else(fail)?;
        int(&e["life_ms"],100,4000).ok_or_else(fail)?;int(&e["speed"],1,240).ok_or_else(fail)?;int(&e["spread"],0,360).ok_or_else(fail)?;
        int(&e["gravity"],-240,240).ok_or_else(fail)?;int(&e["color"],0,65535).ok_or_else(fail)?;int(&e["budget"],1,count).ok_or_else(fail)?;
        if !matches!(e["shape"].as_str(),Some("dot"|"spark")){return Err(fail());}
        let page=if e.get("page").is_some(){int(&e["page"],0,4).ok_or_else(fail)?}else{4};
        if page!=4&&runtime.is_some_and(|r|page>=r["pages"].as_array().map_or(1,|a|a.len().max(1)) as i64){return Err(fail());}
        let t=e["trigger"].as_object().filter(|t|t.len()==1).ok_or_else(fail)?;
        if let Some(tick)=t.get("tick"){int(tick,50,2000).ok_or_else(fail)?;}
        else{
            let a=t.get("action").and_then(Value::as_str).filter(|s|!s.is_empty()&&s.len()<=47&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            if runtime.is_some_and(|r|!r["transitions"].as_array().is_some_and(|ts|ts.iter().any(|tr|tr["on"]==a))){return Err(fail());}
        }
        if let Some(w)=e.get("when"){
            let w=w.as_object().filter(|w|w.len()==3&&w.contains_key("var")&&w.contains_key("op")&&w.contains_key("value")).ok_or_else(fail)?;
            if surface["bindings"]!="p4-variable-bindings-v1"||!w["op"].as_str().is_some_and(|op|PARTICLE_OPS.contains(&op)){return Err(fail());}
            let var=w["var"].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))).ok_or_else(fail)?;
            int(&w["value"],-1_000_000_000,1_000_000_000).ok_or_else(fail)?;
            if runtime.is_some_and(|r|r["vars"][var]["type"]!="int"){return Err(fail());}
        }
    }
    Ok(())
}
/// Install-time negotiation: every new feature needs its hello string; packages without them keep installing.
pub fn require_capability(runtime:&Value,capabilities:&Value)->Result<(),String>{
    if runtime.get("graphics").is_some()&&capabilities["widgetGraphicsV2"]!=GRAPHICS_V2{return Err("设备未开放 p4-graphics-v2 图形配额；未安装不兼容资源".into());}
    if runtime["surface"].get("particles").is_some()&&capabilities["widgetParticles"]!=PARTICLES_ENGINE{return Err("设备未开放装饰粒子能力 p4-particles-v1；未安装不兼容资源".into());}
    if keyframes(&runtime["surface"]).is_some()&&capabilities["widgetKeyframeBackground"]!=KEYFRAME_ENGINE{return Err("设备未开放关键帧背景能力 p4-keyframe-bg-v1；未安装不兼容资源".into());}
    if (runtime["scene"].get("bodies").is_some()||runtime["scene"].get("gravity").is_some())&&capabilities["widgetBodies"]!=BODIES_ENGINE{return Err("设备未开放刚体场景能力 p4-bodies-v1；未安装不兼容资源".into());}
    Ok(())
}
#[cfg(test)]mod tests{
    use super::*;
    fn fixture(name:&str)->Value{serde_json::from_str(&std::fs::read_to_string(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../skills/petui/contracts").join(name)).unwrap()).unwrap()}
    #[test]fn graphics_profile_limits_follow_the_declaration(){
        assert_eq!(limits(None),V1);assert_eq!(limits(Some(&serde_json::json!({"graphics":"p4-graphics-v2"}))),V2);
        assert_eq!(limits(Some(&serde_json::json!({"quota":"p4-quota-v2"}))),V1);
        let f=fixture("graphics-v2.json");
        for s in f["valid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_ok(),"{s}");}
        for s in f["invalid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_err(),"{s}");}
        for s in f["validOnlyWithProfile"].as_array().unwrap(){
            assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_ok(),"{s}");
            assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtimeV1"])).is_err(),"{s}");
            assert!(crate::component_extension::validate_app_surface(s,None).is_err(),"{s}");
        }
        assert!(validate_runtime(&serde_json::json!({"engine":"p4-bounded-runtime-v4","graphics":"p4-graphics-v2"})).is_ok());
        assert!(validate_runtime(&serde_json::json!({"engine":"p4-bounded-runtime-v3","graphics":"p4-graphics-v2"})).is_err());
        assert!(validate_runtime(&serde_json::json!({"engine":"p4-bounded-runtime-v4","graphics":"p4-graphics-v3"})).is_err());
    }
    #[test]fn eight_scene_bindings_are_profile_scoped(){
        let f=fixture("graphics-v2.json");let v=&f["sceneViewBindings"];
        assert!(crate::component_extension::validate_app_surface(&v["valid8"],Some(&f["runtime"])).is_ok());
        assert!(crate::component_extension::validate_app_surface(&v["valid8"],Some(&f["runtimeV1"])).is_err());
        assert!(crate::component_extension::validate_app_surface(&v["invalid9"],Some(&f["runtime"])).is_err());
        assert!(crate::component_extension::validate_app_surface(&v["valid4"],Some(&f["runtimeV1"])).is_ok());
    }
    #[test]fn image_budget_contract_vectors(){
        let f=fixture("graphics-v2.json");
        for s in f["valid"].as_array().unwrap(){assert!(validate_image_budget(s,Some(&f["runtime"])).is_ok());}
        for s in f["validOnlyWithProfile"].as_array().unwrap(){
            if s["images"].as_array().unwrap().len()>V1.images{assert!(validate_image_budget(s,None).is_err());}
        }
        let mut s=f["valid"][0].clone();
        s["images"][0]["frame_width"]=639.into();assert!(validate_image_budget(&s,Some(&f["runtime"])).is_err());
        let mut s=f["valid"][2].clone();
        assert_eq!(validate_image_budget(&s,Some(&f["runtime"])).unwrap(),192000);
        s["images"][1]["frame_width"]=512.into();s["images"][1]["frame_height"]=240.into();
        assert!(validate_image_budget(&s,Some(&f["runtime"])).is_err());
    }
    #[test]fn particle_vectors_and_negotiation(){
        let f=fixture("particles-v1.json");
        for s in f["valid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_ok(),"{s}");}
        for s in f["invalid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_err(),"{s}");}
        let runtime=serde_json::json!({"graphics":"p4-graphics-v2","surface":f["valid"][0].clone(),"scene":{}});
        let full=serde_json::json!({"widgetGraphicsV2":"p4-graphics-v2","widgetParticles":"p4-particles-v1","widgetKeyframeBackground":"p4-keyframe-bg-v1","widgetBodies":"p4-bodies-v1"});
        assert!(require_capability(&runtime,&full).is_ok());
        for missing in ["widgetGraphicsV2","widgetParticles"]{let mut c=full.clone();c[missing]=Value::Null;assert!(require_capability(&runtime,&c).is_err(),"{missing}");}
        assert!(require_capability(&serde_json::json!({"surface":{}}),&serde_json::json!({})).is_ok());
    }
    #[test]fn keyframe_vectors_assets_and_negotiation(){
        let f=fixture("keyframe-bg-v1.json");
        for s in f["valid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_ok(),"{s}");assert!(keyframes(s).is_some()||s["images"][0]["frames"]==1);}
        for s in f["invalid"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtime"])).is_err(),"{s}");}
        for s in f["validOnlyWithProfile"].as_array().unwrap(){assert!(crate::component_extension::validate_app_surface(s,Some(&f["runtimeV1"])).is_err(),"{s}");}
        for a in f["assets"].as_array().unwrap(){
            assert_eq!(keyframe_asset(a["asset"].as_str().unwrap(),a["frame"].as_u64().unwrap()),a["source"]);
            assert_eq!(keyframe_id(a["id"].as_str().unwrap(),a["frame"].as_u64().unwrap()),a["packId"]);
        }
        let animated=f["valid"].as_array().unwrap().iter().find(|s|keyframes(s).is_some()).unwrap();
        let runtime=serde_json::json!({"graphics":"p4-graphics-v2","surface":animated.clone()});
        assert!(require_capability(&runtime,&serde_json::json!({"widgetGraphicsV2":"p4-graphics-v2","widgetKeyframeBackground":"p4-keyframe-bg-v1"})).is_ok());
        assert!(require_capability(&runtime,&serde_json::json!({"widgetGraphicsV2":"p4-graphics-v2"})).is_err());
        assert!(require_capability(&serde_json::json!({"scene":{"bodies":[]}}),&serde_json::json!({})).is_err());
    }
}

/// The bodies extension changes physics, never the legacy scene/entity budgets.
pub fn validate_bodies(runtime:&Value)->Result<(),String>{
    let fail=||"刚体场景需要开放网格、有界唯一刚体、整数重力和 impulse 操作".to_string();
    let scene=&runtime["scene"];
    let ops:Vec<&Value>=scene["rules"].as_array().into_iter().flatten().flat_map(|r|r["do"].as_array().into_iter().flatten()).collect();
    let Some(bodies)=scene.get("bodies")else{
        return if scene.get("gravity").is_some()||ops.iter().any(|o|o["op"]=="impulse"){Err(fail())}else{Ok(())};
    };
    if runtime["engine"]!="p4-bounded-runtime-v4"||scene.get("motion").is_some()||int(&scene["tick_ms"],20,200).is_none(){return Err(fail());}
    let grid=scene["grid"].as_object().filter(|g|g.len()==2&&g.contains_key("width")&&g.contains_key("height")).ok_or_else(fail)?;
    if int(&grid["width"],4,16).is_none()||int(&grid["height"],4,16).is_none(){return Err(fail());}
    if scene.get("gravity").is_some_and(|g|int(g,-64,64).is_none()){return Err(fail());}
    let entities=scene["entities"].as_array().filter(|a|(1..=12).contains(&a.len())).ok_or_else(fail)?;
    let mut ids=std::collections::BTreeSet::new();
    for e in entities{let id=e["id"].as_str().ok_or_else(fail)?;if !ids.insert(id){return Err(fail());}}
    let bodies=bodies.as_array().filter(|a|(1..=8).contains(&a.len())).ok_or_else(fail)?;
    let mut seen=std::collections::BTreeSet::new();
    for b in bodies{
        let b=b.as_object().filter(|o|o.keys().all(|k|["entity","mass","restitution","static","rolling"].contains(&k.as_str()))).ok_or_else(fail)?;
        let id=b.get("entity").and_then(Value::as_str).ok_or_else(fail)?;
        if !seen.insert(id){return Err(fail());}
        let e=entities.iter().find(|e|e["id"]==id).ok_or_else(fail)?;
        if b.get("mass").is_some_and(|v|int(v,1,64).is_none())||b.get("restitution").is_some_and(|v|int(v,0,100).is_none())
          ||["static","rolling"].iter().any(|k|b.get(*k).is_some_and(|v|!v.is_boolean()))
          ||e.get("bounds").is_some_and(|v|v!="clamp")||e.get("collidable").is_some_and(|v|v!=true){return Err(fail());}
        if b.get("rolling")==Some(&Value::Bool(true))&&!matches!(e.get("shape").and_then(Value::as_str).unwrap_or("rect"),"rect"|"circle"|"ball"|"coin"|"capsule"|"triangle"|"diamond"){return Err(fail());}
        if b.get("static")==Some(&Value::Bool(true))&&["vx","vy"].iter().any(|k|e.get(*k).is_some_and(|v|v!=0)){return Err(fail());}
    }
    for e in entities{if !seen.contains(e["id"].as_str().unwrap())&&["vx","vy"].iter().any(|k|e.get(*k).is_some_and(|v|v!=0)){return Err(fail());}}
    for o in ops{
        if matches!(o["op"].as_str(),Some("move"|"place"|"velocity"|"accelerate"|"bounce"|"charge"|"launch"|"cancel")){return Err(fail());}
        if o["op"]=="impulse"{
            if o.as_object().is_none_or(|o|o.len()!=4||o.keys().any(|k|!["op","entity","dx","dy"].contains(&k.as_str())))
               ||o["entity"].as_str().is_none_or(|id|!seen.contains(id))||int(&o["dx"],-64,64).is_none()||int(&o["dy"],-64,64).is_none()||(o["dx"]==0&&o["dy"]==0){return Err(fail());}
        }
    }
    Ok(())
}

#[cfg(test)]mod bodies_tests{
    use super::*;
    #[test]fn schema_matches_shared_bodies_vectors(){
        let f:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/bodies-v1.json")).unwrap();
        for scene in f["valid"].as_array().unwrap(){let mut r=f["runtime"].clone();r["scene"]=scene.clone();assert!(validate_runtime(&r).is_ok());}
        for scene in f["invalid"].as_array().unwrap(){let mut r=f["runtime"].clone();r["scene"]=scene.clone();assert!(validate_runtime(&r).is_err(),"{scene}");}
    }
}
