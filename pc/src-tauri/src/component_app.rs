//! Validated app host. Device requests and UI operations share one serialized path.
use crate::app_services::{self,Declaration};
use serde::{Deserialize,Serialize};
use serde_json::{json,Value};
use std::{collections::VecDeque,path::{Path,PathBuf},sync::{Mutex,OnceLock,mpsc},io::Write};
use tauri::Emitter;
#[derive(Clone,Default,PartialEq,Serialize,Deserialize)]
#[serde(default)]
struct State{
    selected:usize,collection:bool,favorites:Vec<String>,trip:Option<Trip>,visits:u32,
    trip_candidate:Option<usize>,
    city:crate::app_weather::Location,weather:crate::app_weather::Cache,details:bool,
    auto_city:bool,city_from_ip:bool,geo:crate::app_weather::GeoCache,
    city_select:bool,city_candidate:usize,
    knowledge:crate::knowledge_library::Reading,
    sound_volume:Option<u32>,sound_timer:usize,
    todos:crate::component_todos::Reading,
    calendar:crate::component_calendar::Reading,
}
#[derive(Clone,PartialEq,Serialize,Deserialize)]
struct Trip{id:String,started:i64,arrival:i64}
#[derive(Default)]struct DeviceEpoch{generation:u64,board:String,id:String,instance:String,visible:bool}
impl DeviceEpoch{
    fn observe(&mut self,board:&str,id:&str,instance:&str,visible:bool)->u64{
        if self.board!=board||self.id!=id||self.instance!=instance||self.visible!=visible{self.generation=self.generation.wrapping_add(1);self.board=board.into();self.id=id.into();self.instance=instance.into();self.visible=visible;}
        self.generation
    }
    fn accepts(&self,generation:u64)->bool{self.visible&&self.generation==generation}
}
struct Host{root:PathBuf,usb:crate::usb_serial::UsbSerialManager,app:tauri::AppHandle,gate:Mutex<()>,events:mpsc::SyncSender<Value>,seen:Mutex<VecDeque<String>>,device_epoch:Mutex<DeviceEpoch>,entry_epoch:Mutex<Option<u64>>,narration:Mutex<crate::knowledge_narration::Narration>}
static HOST:OnceLock<Host>=OnceLock::new();
// Retry readiness failures, not paid generation. A successful hand-off is final
// for this connection. Slow/cold startup must not permanently consume the edge.
#[derive(Default)]
struct ConnectionPreparation{board:String,done:bool,attempt:u32,next:u64}
impl ConnectionPreparation{
    fn observe(&mut self,board:&str,now:u64)->bool{
        if self.board==board{return false;}
        *self=Self{board:board.into(),next:now+2,..Default::default()};true
    }
    fn due(&self,now:u64)->bool{!self.board.is_empty()&&!self.done&&now>=self.next}
    fn finish(&mut self,ok:bool,now:u64){self.done=ok;self.attempt=self.attempt.saturating_add(1);self.next=now+(2u64.saturating_pow(self.attempt.min(6))).min(60);}
}
#[cfg(test)]mod connection_preparation_tests{
    use super::*;
    #[test]fn cold_start_failures_retry_then_success_consumes_only_one_connection(){
        let mut s=ConnectionPreparation::default();assert!(s.observe("board-a",0));assert!(!s.due(0));assert!(s.due(2));
        for now in [2,4,8,16,32,64,124,184]{assert!(s.due(now));s.finish(false,now);assert!(!s.due(now));assert!(!s.observe("board-a",now));}
        assert!(s.due(244));s.finish(true,244);assert!(!s.due(1000));
        assert!(s.observe("",1001));assert!(!s.due(2000));assert!(s.observe("board-a",2001));assert!(s.due(2003));
    }
    #[test]fn switching_devices_resets_readiness_and_attempts(){
        let mut s=ConnectionPreparation::default();s.observe("a",0);s.finish(true,2);s.observe("b",3);
        assert_eq!(s.attempt,0);assert!(!s.done);assert!(!s.due(4));assert!(s.due(5));
    }
}
fn prepare_connected_news(h:&Host,board:&str)->Result<Value,&'static str>{
    let inventory=h.usb.query_widget_inventory(board).map_err(|_|"inventory_unavailable")?;
    if inventory["complete"]!=true{return Err("inventory_incomplete");}
    let mut unresolved=false;
    for item in inventory["items"].as_array().into_iter().flatten(){
        let live=h.usb.status();if !live.connected||live.board_device_id!=board{return Err("connection_changed");}
        let Some(id)=item["id"].as_str().filter(|id|crate::component_extension::valid_id(id))else{continue;};
        match crate::component_service_settings::enabled(&h.root.join(id)){
            Ok(false)=>continue,Err(_)=>{unresolved=true;continue;},Ok(true)=>{}
        }
        match resolve(id){
            Ok((_,d,_)) if d.service=="news"=>{
                crate::component_news::prefetch(id,&d).map_err(|_|"news_not_ready")?;
                return Ok(inventory);
            },
            Err(_)=>unresolved=true,
            _=>{}
        }
    }
    if unresolved{Err("library_not_ready")}else{Ok(inventory)}
}
pub(crate) fn news_epoch_active(id:&str,generation:u64)->bool{
    HOST.get().is_some_and(|h|{let board=h.usb.status();let e=h.device_epoch.lock().unwrap();board.connected&&e.board==board.board_device_id&&e.id==id&&e.accepts(generation)})
}
pub fn configure(root:PathBuf,usb:crate::usb_serial::UsbSerialManager,app:tauri::AppHandle){
    crate::component_news::configure_cache(root.join("component-apps"));
    crate::component_audio::configure_speech_cache(root.join("news-speech-cache"));
    crate::component_voice::configure_preparation(root.join("news-speech-cache").join("last-pet-voice.json"));
    let (tx,rx)=mpsc::sync_channel::<Value>(24);
    if HOST.set(Host{root:root.join("component-apps"),usb,app,gate:Mutex::new(()),events:tx,seen:Mutex::new(VecDeque::new()),device_epoch:Mutex::new(DeviceEpoch::default()),entry_epoch:Mutex::new(None),narration:Mutex::new(Default::default())}).is_err(){return;}
    std::thread::Builder::new().name("component-app-host".into()).spawn(move||while let Ok(event)=rx.recv(){process_event(event);}).expect("component host worker");
    // Independent from USB edges: startup, long-running PCs crossing midnight,
    // and retry/backoff all run without opening the component. Discovery is
    // bounded and infrequent; generation is asynchronous, never on the USB lane.
    std::thread::Builder::new().name("daily-news-preparation".into()).spawn(move||{
        std::thread::sleep(std::time::Duration::from_secs(5));
        let mut declared=Vec::new();let mut scanned=std::time::Instant::now()-std::time::Duration::from_secs(301);
        loop{
            let h=HOST.get().expect("configured host");
            if !crate::component_audio::voice_busy()&&!crate::component_audio::active_for_exit(){
                if scanned.elapsed().as_secs()>=300{
                    if let Ok(home)=crate::get_home_dir(){if let Ok(library)=crate::component_library::list(&home){
                        declared=library.components.into_iter().filter(|e|e.valid&&e.pc_application.as_ref().is_some_and(|a|a["service"]=="news"))
                            .take(8).filter_map(|e|resolve(&e.id).ok().map(|(_,d,_)|(e.id,d))).collect::<Vec<_>>();
                    }}scanned=std::time::Instant::now();
                }
                for(id,d)in &declared{if crate::component_service_settings::enabled(&h.root.join(id)).unwrap_or(false){let _=crate::component_news::prefetch(id,d);}else{crate::component_news::disable(id);}}
                crate::component_news::prepare_audio(&h.usb.status().board_device_id);
            }
            std::thread::sleep(std::time::Duration::from_secs(15));
        }
    }).expect("daily news worker");
    std::thread::Builder::new().name("component-connect-preparation".into()).spawn(move||{
        let mut preparation=ConnectionPreparation::default();
        let clock=std::time::Instant::now();
        loop {
            let h=HOST.get().expect("configured host");let board=h.usb.status();
            let now=clock.elapsed().as_secs();
            let connected=if board.connected{board.board_device_id.as_str()}else{""};
            if preparation.observe(connected,now){
                // Do not erase an entry already received by the USB worker on
                // the first connected observation. Only a real disconnect ends it.
                if connected.is_empty(){crate::component_news::disconnected();}
                crate::realtime_chat_log::record("","news_connection",json!({"state":if connected.is_empty(){"disconnected"}else{"waiting_ready"}}));
            }
            if preparation.due(now)&&!crate::realtime_chat::status().active{
                let result=prepare_connected_news(h,connected);
                preparation.finish(result.is_ok(),clock.elapsed().as_secs());
                crate::realtime_chat_log::record("","news_connection_preparation",json!({"ok":result.is_ok(),"retries":preparation.attempt,"reason":result.as_ref().err().copied().unwrap_or("scheduled")}));
                if let Ok(inventory)=result{
                        if board.capabilities["widgetResourceIntegrity"]=="p4-resource-integrity-v1" {
                            for item in inventory["items"].as_array().into_iter().flatten(){
                                if !matches!(item["resourceState"].as_str(),Some("bootstrap"|"damaged"))||crate::realtime_chat::status().active{continue;}
                                let Some(id)=item["id"].as_str()else{continue;};
                                match crate::component_library::bundled_repair_source(id){
                                    Ok(Some(entry))=>{
                                        let result=h.usb.repair_widget_resources(connected,id,Path::new(&entry.path));
                                        let _=h.app.emit("component-resource-recovery",json!({"componentId":id,"ok":result.is_ok(),"error":result.err()}));
                                    },
                                    Ok(None)=>{},
                                    Err(_)=>eprintln!("[components] bundled recovery source unavailable; original data retained"),
                                }
                            }
                        }
                }
            }
            std::thread::sleep(std::time::Duration::from_secs(1));
        }
    }).expect("component connection worker");
}
fn atomic(path:&Path,value:&impl Serialize)->Result<(),String>{
    let parent=path.parent().ok_or("组件存储位置无效")?;std::fs::create_dir_all(parent).map_err(|_|"无法创建组件本地存储")?;
    let mut f=tempfile::NamedTempFile::new_in(parent).map_err(|_|"无法准备组件存储")?;
    let raw=serde_json::to_vec(value).map_err(|_|"组件状态无效")?;
    f.write_all(&raw).and_then(|_|f.as_file().sync_all()).map_err(|_|"无法写入组件状态")?;
    f.persist(path).map_err(|_|"无法保存组件状态")?;Ok(())
}
fn load(path:&Path)->Result<State,String>{
    let mut state:State=match std::fs::read(path){Ok(bytes) if bytes.len()<=1048576=>serde_json::from_slice(&bytes).map_err(|_|"组件记录损坏；请备份后联系开发者恢复")?,Ok(_)=>return Err("组件记录超过安全上限".into()),Err(e)if e.kind()==std::io::ErrorKind::NotFound=>State::default(),Err(_)=>return Err("无法读取组件本地记录".into())};
    if state.favorites.len()>64||state.favorites.iter().any(|s|s.is_empty()||s.len()>64)||state.trip.as_ref().is_some_and(|t|t.id.is_empty()||t.id.len()>64||t.started<0||t.started>253402214400||t.arrival<=t.started||t.arrival.saturating_sub(t.started)>86400){return Err("组件记录超出有效范围，未覆盖原文件".into());}
    state.selected%=64;state.sound_timer%=3;state.sound_volume=state.sound_volume.map(|v|v.min(100));
    let mut seen=std::collections::HashSet::new();state.favorites.retain(|id|seen.insert(id.clone()));
    Ok(state)
}
fn resolve(id:&str)->Result<(String,std::sync::Arc<Declaration>,Value),String>{
    type Parsed=Vec<(String,std::sync::Arc<Declaration>)>;
    static PARSED:OnceLock<Mutex<Parsed>>=OnceLock::new();
    if !crate::component_extension::valid_id(id){return Err("组件 ID 无效".into());}
    let entry=crate::component_library::latest(&crate::get_home_dir()?,id)?.ok_or("电脑未安装此组件，请在组件中心恢复或导入同一组件")?;
    if !entry.valid{return Err("组件校验未通过".into());}
    let root=Path::new(&entry.path);let hash=crate::component_library::package_content_hash(root)?;
    if hash!=entry.version_hash{return Err("组件内容已变化，请重新导入".into());}
    let runtime=entry.app_runtime.ok_or("组件没有应用界面")?;
    let cache=PARSED.get_or_init(Default::default);
    if let Some(d)=cache.lock().map_err(|_|"组件声明缓存不可用")?.iter().find(|(h,_)|h==&hash).map(|(_,d)|d.clone()){
        return Ok((hash,d,runtime));
    }
    let bytes=std::fs::read(root.join("pc/application.json")).map_err(|_|"组件没有 PC 应用服务")?;
    let d=std::sync::Arc::new(app_services::parse(&bytes,&runtime)?);
    let mut cache=cache.lock().map_err(|_|"组件声明缓存不可用")?;
    if cache.len()>=8{cache.remove(0);}
    cache.push((hash.clone(),d.clone()));
    Ok((hash,d,runtime))
}
fn pending_todo(rows:&[crate::widget_data::DataRow])->Option<String>{
    rows.iter().find(|r|r.tone>=0).map(|r|format!("{} {}",r.label,r.value))
}
fn defaults(runtime:&Value)->Value{
    let mut value=json!({});for k in ["eyebrow","headline","metricValue","metricLabel","metricUnit","badge","note","footer"]{value[k]=runtime["dashboard"][k].as_str().unwrap_or("").into();}value
}
fn merge(target:&mut Value,source:Value){if let Some(o)=source.as_object(){for (k,v)in o{target[k]=v.clone();}}}
fn bounded(values:&mut Value){for(k,max)in [("eyebrow",95),("headline",159),("metricValue",63),("metricLabel",95),("metricUnit",31),("badge",15),("note",159),("footer",159)]{values[k]=crate::widget_data::bounded_text(values[k].as_str().unwrap_or(""),max).into();}}
fn settle_trip(s:&mut State,d:&Declaration,now:i64)->bool{
    let Some(t)=s.trip.clone()else{return false;};
    // Clock rollback or wildly shifted local time cannot mint duplicate rewards.
    if now<t.started-300{return false;}
    if now>=t.arrival{
        if d.content.iter().any(|c|c["id"]==t.id)&&!s.favorites.contains(&t.id){s.favorites.push(t.id.clone());}
        s.visits=s.visits.saturating_add(1);s.trip=None;s.trip_candidate=None;
        // Keep an explicitly selected next destination; otherwise show the
        // postcard from this arrival. Never replace a queued destination.
        s.collection=d.content.get(s.selected%d.content.len().max(1)).is_some_and(|c|c["id"]==t.id);return true;
    }false
}
fn next(s:&mut State,count:usize,back:bool){s.selected=(s.selected%count.max(1)+if back{count.saturating_sub(1)}else{1})%count.max(1);}
fn controls(id:&str,dir:&Path,d:&Declaration,s:&State)->Result<Value,String>{
    if d.service=="todos" {
        let allowed=s.todos.ui["operations"].as_array();
        let actions:Vec<&str>=d.actions.iter().filter(|(_,op)|allowed.is_some_and(|a|a.iter().any(|v|v.as_str()==Some(op.as_str())))).map(|(key,_)|key.as_str()).collect();
        return Ok(json!({"mode":s.todos.ui["mode"],"actions":actions,"context":s.todos.ui["context"]}));
    }
    let (mode,allowed,context)=match d.service.as_str(){
        "books"=>{
            let mut allowed=vec!["random","category"];
            let info=crate::knowledge_library::info(&s.knowledge,&d.content);
            if s.knowledge.choosing||s.knowledge.page>0{allowed.push("previous");}
            if s.knowledge.choosing||s.knowledge.page.saturating_add(1)<info["pages"].as_u64().unwrap_or(0)as usize{allowed.push("next");}
            (if s.knowledge.choosing{3}else{0},Some(allowed),String::new())
        },
        "calendar"=>(if s.calendar.details{1}else{0},Some(crate::component_calendar::operations(&s.calendar,&d.content,chrono::Utc::now().timestamp())),String::new()),
        "notes"=>{let (mode,allowed,token)=crate::component_notes::controls(id,dir)?;(mode,Some(allowed),token)},
        "weather"=>(if s.city_select{3}else{0},Some(if s.city_select{vec!["choose_city","refresh","todo","previous","next"]}else{vec!["choose_city","refresh","todo","details"]}),String::new()),
        "journey"=>(if s.trip_candidate.is_some(){3}else if s.collection{5}else if s.trip.is_some(){1}else{0},
            Some(if s.trip.is_some()&&s.trip_candidate.is_none(){vec!["collection","previous","next"]}else{vec!["depart","collection","previous","next"]}),String::new()),
        _=>(0,None,String::new()),
    };
    let actions:Vec<&str>=d.actions.iter().filter(|(_,op)|allowed.as_ref().is_none_or(|a|a.contains(&op.as_str()))).map(|(key,_)|key.as_str()).collect();
    Ok(json!({"mode":mode,"actions":actions,"context":context}))
}
fn apply(id:&str,dir:&Path,d:&Declaration,s:&mut State,op:&str,input:&Value,board:&str,notify:&dyn Fn(Value)->Result<(),String>,usb:&crate::usb_serial::UsbSerialManager,values:&mut Value)->Result<(),String>{
    let now=chrono::Utc::now().timestamp();
    match d.service.as_str(){
        "calendar"=>{
            crate::component_calendar::apply(&mut s.calendar,&d.content,op,now)?;
            merge(values,crate::component_calendar::view(&s.calendar,&d.content,now)?);
        },
        "journey"=>{
            settle_trip(s,d,now);
            match op{
                "previous"|"next" if s.trip.is_some()&&!s.collection=>{
                    let count=d.content.len();let at=s.trip_candidate.unwrap_or(s.selected)%count;
                    s.trip_candidate=Some((at+if op=="previous"{count-1}else{1})%count);s.collection=false;
                },
                "previous"=>next(s,d.content.len(),true),"next"=>next(s,d.content.len(),false),
                "collection"=>{if s.trip_candidate.take().is_some(){s.collection=false;}else{s.collection=!s.collection;}},
                "depart"=>{
                    if s.trip.is_none(){let c=&d.content[s.selected%d.content.len()];s.trip=Some(Trip{id:c["id"].as_str().unwrap().into(),started:now,arrival:now+c["minutes"].as_i64().unwrap()*60});s.collection=false;}
                    else if let Some(candidate)=s.trip_candidate.take(){s.selected=candidate%d.content.len();s.collection=false;}
                },_=>{}
            }
            let card=&d.content[s.selected%d.content.len()];
            if let Some(candidate)=s.trip_candidate{
                let c=&d.content[candidate%d.content.len()];
                values["eyebrow"]="选择下一站 · 当前旅程继续".into();values["headline"]=c["title"].clone();values["metricValue"]=format!("{} 分钟",c["minutes"]).into();
                values["note"]="确认后保存为下一站；不会取消或重置当前旅程。抵达后再出发。".into();
            }else if s.collection{
                let earned=s.favorites.iter().any(|id|card["id"]==*id);
                values["eyebrow"]=if earned{"旅行带回 · 原创明信片"}else{"默认明信片 · 目的地预览"}.into();
                values["headline"]=card["title"].clone();
                values["metricValue"]=format!("已带回 {}/{}",s.favorites.len(),d.content.len()).into();
                // Preview art/text is package content, never a fabricated visit
                // or a mutation of the user's earned collection.
                values["note"]=card["text"].clone();
            }
            else if let Some(t)=&s.trip{
                values["eyebrow"]="虚拟旅程 · 电脑计时".into();values["headline"]=d.content.iter().find(|c|c["id"]==t.id).map(|c|c["title"].clone()).unwrap_or("旅行中".into());
                values["metricValue"]=format!("{} 分钟",((t.arrival-now).max(0)+59)/60).into();values["note"]=if now<t.started-300{"电脑时间发生变化，请先核对时钟".into()}else if card["id"]!=t.id{format!("下一站：{}；当前旅程继续，抵达后可再出发。",card["title"].as_str().unwrap_or(""))}else{"离开组件仍在旅行；左右可预选下一站".into()}.into();
            }else{values["eyebrow"]="选一个想去的地方".into();values["headline"]=card["title"].clone();values["metricValue"]=format!("{} 分钟",card["minutes"]).into();values["note"]="不需要 GPS；左右选择原创虚拟目的地".into();}
            values["badge"]=format!("{}/{}",s.trip_candidate.unwrap_or(s.selected)%d.content.len()+1,d.content.len()).into();
            values["metricLabel"]=if s.trip_candidate.is_some(){"确认下一站"}else if s.trip.is_some(){"旅行进行中"}else if s.collection{"前往此地"}else{"就去这里"}.into();values["metricUnit"]=if s.trip_candidate.is_some(){"取消选地"}else if s.collection{"返回旅程"}else{"明信片"}.into();
        },
        "knowledge"|"books" if d.is_library()=>{
            s.knowledge.include_all=d.service=="books";
            if d.service=="books"&&input["_entered"]==true{s.knowledge.choosing=false;}
            let was_playing=crate::component_audio::playing(id);
            if op!="status"&&!(d.auto_read&&matches!(op,"previous"|"next")&&!s.knowledge.choosing){crate::component_audio::stop(Some(id));}
            crate::knowledge_library::apply(&mut s.knowledge,&d.content,op);
            if op=="favorite"&&!s.knowledge.choosing{
                let key=s.knowledge.article.clone();
                if s.favorites.contains(&key){s.favorites.retain(|id|id!=&key);}
                else if s.favorites.len()<64{s.favorites.push(key);}else{return Err("最多收藏 64 条，请先取消一条收藏".into());}
            }
            if op=="speak"&&!s.knowledge.choosing&&!was_playing{
                let volume=(crate::voice_chat_settings::realtime_volume()?.clamp(0.0,1.0)*100.0)as u32;
                crate::component_audio::start(id,usb,crate::component_audio::Source::Speech(crate::knowledge_library::speech_page(&s.knowledge,&d.content)),volume)?;
            }
            merge(values,crate::knowledge_library::view(&s.knowledge,&d.content,if d.actions.values().any(|v|v=="favorite"){&s.favorites}else{&[]},crate::component_audio::playing(id)));
            let audio=crate::component_audio::status(id);if audio["phase"]=="error"{values["metricValue"]=audio["error"].clone();}
        },
        "knowledge"=>{
            match op{"previous"=>{crate::component_audio::stop(Some(id));next(s,d.content.len(),true)},"next"=>{crate::component_audio::stop(Some(id));next(s,d.content.len(),false)},"favorite"=>{let id=d.content[s.selected%d.content.len()]["id"].as_str().unwrap().to_owned();if s.favorites.contains(&id){s.favorites.retain(|s|s!=&id);}else{s.favorites.push(id);}},"speak"=>{if crate::component_audio::playing(id){crate::component_audio::stop(Some(id));}else{let c=&d.content[s.selected%d.content.len()];let volume=(crate::voice_chat_settings::realtime_volume()?.clamp(0.0,1.0)*100.0)as u32;crate::component_audio::start(id,usb,crate::component_audio::Source::Speech(format!("{}。{}",c["title"].as_str().unwrap(),c["text"].as_str().unwrap())),volume)?;}},_=>{}}
            let c=&d.content[s.selected%d.content.len()];values["eyebrow"]=format!("{} · {}/{}",c["source"].as_str().unwrap(),s.selected%d.content.len()+1,d.content.len()).into();values["headline"]=c["title"].clone();values["note"]=c["text"].clone();values["metricValue"]="".into();values["metricUnit"]=if s.favorites.iter().any(|id|c["id"]==*id){"取消收藏"}else{"收藏"}.into();
            values["metricLabel"]=if crate::component_audio::playing(id){"停止朗读"}else{"读给我听"}.into();let audio=crate::component_audio::status(id);if audio["phase"]=="error"{values["note"]=audio["error"].clone();}
        },
        "desktop-tasks"=>{
            match op{"previous"=>s.selected=s.selected.saturating_sub(1),"next"=>s.selected=(s.selected+1)%64,"details"=>s.details=!s.details,
                "voice"=>{if crate::realtime_chat::status().active{return Err("实时对话已开启，直接说出补充要求即可".into());}if board.is_empty(){return Err("请先连接设备".into());}notify(json!({"boardDeviceId":board,"context":"app","at":now*1000}))?;},_=>{}}
            let (agent,_)=crate::resolve_usb_inject_target(board);merge(values,crate::desktop_agent::widget_status(board,&agent,s.selected,s.details));
            values["metricUnit"]=if s.details{"隐藏内容"}else{"查看进展"}.into();
        },
        "weather"=>{
            let p=d.location.as_ref().unwrap_or(crate::location_profile::legacy());
            let picker=p.text.contains_key("cityButton")&&d.actions.values().any(|v|v=="choose_city");
            let was_select=s.city_select;
            if picker&&matches!(op,"choose_city"|"previous"|"next"){
                if !s.city_select{s.city_candidate=p.cities.iter().position(|c|c.city==s.city.city).unwrap_or(0);}
                s.city_select=true;s.collection=false;
                let count=p.cities.len();
                s.city_candidate%=count;
                if op=="previous"{s.city_candidate=(s.city_candidate+count-1)%count;}
                if op=="next"{s.city_candidate=(s.city_candidate+1)%count;}
            }
            if picker&&was_select&&op=="refresh"{
                s.city=p.cities[s.city_candidate%p.cities.len()].clone();
                s.auto_city=false;s.city_from_ip=false;s.city_select=false;s.collection=false;s.weather=Default::default();
            }
            if picker&&was_select&&op=="todo"{s.city_select=false;}
            if op=="configure"{let city:crate::app_weather::Location=serde_json::from_value(input["city"].clone()).map_err(|_|"城市格式无效")?;if !city.valid(){return Err("城市和经纬度无效".into());}s.city=city;s.auto_city=false;s.city_from_ip=false;s.collection=false;s.city_select=false;s.weather=Default::default();}
            if op=="locate"{s.auto_city=true;s.collection=false;s.city_select=false;}
            if s.auto_city||!s.city.valid(){
                s.auto_city=true;crate::app_weather::locate_background(p,&mut s.geo,now);
                if s.geo.location.valid(){if s.city!=s.geo.location{s.city=s.geo.location.clone();s.weather=Default::default();}s.city_from_ip=true;}
            }
            if op=="todo"&&!(picker&&was_select){s.collection=!s.collection;}if op=="details"{s.details=!s.details;}
            if s.city.valid(){crate::app_weather::refresh_background(p,&s.city,&mut s.weather,now);}
            let mut vars=vec![("city",s.city.city.clone()),("mode",p.text(if s.city_from_ip{"autoMode"}else{"manualMode"}))];
            if let Some((temp,condition,at))=crate::app_weather::current(p,&s.weather,now){
                vars.extend([("value",format!("{temp:.0}")),("condition",condition),("time",at)]);
                for(slot,key)in [("eyebrow","readyEyebrow"),("headline","readyHeadline"),("metricValue","readyValue")]{values[slot]=p.render(key,&vars).into();}
                values["note"]=if !s.weather.error.is_empty(){s.weather.error.clone()}else{p.text("sourceNote")}.into();
                if s.details{values["note"]=p.render("detailsNote",&vars).into();}
                if op=="refresh"&&s.weather.error.is_empty(){values["note"]=p.text("cachedNote").into();}
            }else{
                values["headline"]=p.render(if s.city.valid(){if s.weather.error==p.text("loadingNote"){"loadingHeadline"}else{"failedHeadline"}}else if s.geo.error==p.text("locatingNote"){"locatingHeadline"}else{"chooseHeadline"},&vars).into();
                values["metricValue"]=p.text("emptyValue").into();
                values["note"]=if s.city.valid(){s.weather.error.clone()}else{s.geo.error.clone()}.into();
                if s.city.valid(){values["eyebrow"]=p.render("cityEyebrow",&vars).into();}
            }
            if s.collection{let todo=crate::todos::snapshot();values["headline"]=p.text("todoTitle").into();values["metricValue"]="".into();values["note"]=pending_todo(&todo.rows).unwrap_or_else(||p.text("todoEmpty")).into();}
            if picker{
                // Location is independent of forecast success and remains visible in every view.
                values["eyebrow"]=if s.city.valid(){p.render("cityButton",&vars)}else{p.text("emptyCityButton")}.into();
                values["metricLabel"]=p.text(if s.city_select{"pickerConfirm"}else{"refreshButton"}).into();
                values["metricUnit"]=p.text(if s.city_select{"pickerCancel"}else{"todoButton"}).into();
                if s.city_select{
                    let index=s.city_candidate%p.cities.len();
                    values["headline"]=p.cities[index].city.clone().into();
                    values["metricValue"]=format!("{}/{}",index+1,p.cities.len()).into();
                    values["note"]=p.text("pickerNote").into();
                }
            }
        },
        "notes"=>merge(values,crate::component_notes::execute(id,op,input,dir,usb)?),
        "todos"=>merge(values,crate::component_todos::execute(&mut s.todos,op,input)?),
        "soundscape"=>{
            let was_playing=crate::component_audio::playing(id);let volume=s.sound_volume.unwrap_or(35).min(100);
            let chosen=match op{"rain"=>Some(0),"sea"=>Some(1),"fire"=>Some(2),"insects"=>Some(3),_=>None};
            let changed=chosen.is_some_and(|n|n!=s.selected%4);
            if let Some(n)=chosen{s.selected=n;}
            match op{"previous"=>next(s,4,true),"next"=>next(s,4,false),"timer"=>s.sound_timer=(s.sound_timer+1)%3,"up"=>s.sound_volume=Some((volume+5).min(100)),"down"=>s.sound_volume=Some(volume.saturating_sub(5)),_=>{}}
            if op=="toggle"&&was_playing{crate::component_audio::stop(Some(id));}
            else if op=="toggle"||was_playing&&(matches!(op,"previous"|"next")||changed){crate::component_audio::start(id,usb,crate::component_audio::Source::Sound{kind:s.selected%4,minutes:[15,30,60][s.sound_timer%3]},s.sound_volume.unwrap_or(35))?;}
            if op=="timer"&&was_playing{crate::component_audio::sound_timer(id,[15,30,60][s.sound_timer%3]);}
            crate::component_audio::volume(id,s.sound_volume.unwrap_or(35));let audio=crate::component_audio::status(id);
            values["headline"]=["窗边细雨","海边潮汐","暖炉微火","安静虫鸣"][s.selected%4].into();values["eyebrow"]=format!("原创合成声景 · {} 分钟渐止",[15,30,60][s.sound_timer%3]).into();values["metricValue"]=format!("{}%",s.sound_volume.unwrap_or(35)).into();
            values["metricLabel"]=if crate::component_audio::playing(id){"停止播放"}else{"开始播放"}.into();values["metricUnit"]=format!("{} 分钟",[15,30,60][s.sound_timer%3]).into();
            values["note"]=match audio["phase"].as_str().unwrap_or("idle"){"error"=>audio["error"].clone(),"preparing"|"buffering"=>"正在准备设备播放".into(),"playing"=>format!("剩余约 {} 分钟 · 定时为本轮总时长 · 退出即停止",audio["remainingSeconds"].as_u64().unwrap_or(0).div_ceil(60)).into(),_=>"左右换声景重新计时；上下调音量；退出即停止。".into()};
        },
        _=>return Err("此宿主服务仍在接入，不会假装执行成功".into())
    }Ok(())
}
fn request(id:&str,mut input:Value)->Result<Value,String>{
    let h=HOST.get().ok_or("应用服务尚未初始化")?;let _lock=h.gate.lock().map_err(|_|"组件服务暂不可用")?;
    if input.get("_deviceEpoch").is_some()&&!input["_deviceEpoch"].as_u64().is_some_and(|e|h.device_epoch.lock().unwrap().accepts(e)){return Err("组件已退出或切换，此次操作未执行".into());}
    let (_version,d,runtime)=resolve(id).inspect_err(|_|{crate::component_audio::stop(Some(id));crate::component_notes::cancel(Some(id));})?;let dir=h.root.join(id);
    let generation=input["_deviceEpoch"].as_u64();
    // Real device entry returns to today exactly once. PC previews and status
    // polls must not undo a date the user is browsing.
    let new_entry=generation.is_some()&&generation!=*h.entry_epoch.lock().unwrap();
    if d.service=="news"&&new_entry{input["_entered"]=true.into();}
    if d.service=="calendar"&&new_entry&&input["operation"]=="status"{input["operation"]="today".into();}
    if d.service=="books"&&new_entry&&input["operation"]=="status"{input["operation"]="random".into();input["_entered"]=true.into();}
    let mut result=request_validated(id,&dir,&d,&runtime,input,&h.usb,&|payload|h.app.emit(crate::realtime_chat::REQUEST_EVENT,payload).map_err(|_|"无法打开实时对话".into()))?;
    if new_entry{*h.entry_epoch.lock().unwrap()=generation;}
    // Only trusted device visibility events may auto-play. Desktop previews and
    // polling never do. Hold the epoch lock through start so an exit always
    // cancels a job started concurrently with that exit.
    if d.auto_read {
        if let Some(generation)=generation{
            let epoch=h.device_epoch.lock().unwrap();
            // Initial device status may arrive before the desktop resolves its
            // current appearance. Wait without consuming the first narration.
            let eligible=epoch.accepts(generation)&&epoch.id==id;
            let desired=result["permissions"]["autoRead"]==true;
            let voice_ready=crate::component_voice::ready(&epoch.board);
            let busy=crate::component_audio::voice_busy();
            if eligible&&desired&&(!voice_ready||busy)&&!crate::component_audio::playing(id){
                crate::component_audio::report(id,"waiting",if busy{"等待当前录音或实时对话结束"}else{"等待当前宠物音色就绪"});
            }
            if d.service=="news"&&eligible&&desired&&(!voice_ready||busy){
                let audio=crate::component_audio::status(id);
                if audio["speechId"]!=result["knowledge"]["current"]["id"]{
                    crate::component_news::playback_report(id,"waiting",if busy{"语音正在使用，结束后开始播报"}else{"正在确认当前宠物音色"});
                }else if audio["phase"]=="idle"{
                    crate::component_news::playback_report(id,"interrupted","播报已被语音操作中断，可按播报重新开始");
                }
            }
            if eligible&&voice_ready&&(!busy||!desired){
                let mut narration=h.narration.lock().unwrap();
                let phase=crate::component_audio::status(id);
                if let Some(source)=narration.next_source(generation,&result,phase["phase"].as_str().unwrap_or("idle")){
                    let start=crate::voice_chat_settings::realtime_volume().and_then(|v|crate::component_audio::start(id,&h.usb,source,(v.clamp(0.0,1.0)*100.0)as u32));
                    match start{
                        Ok(())=>{if d.service=="news"{crate::component_news::playback_report(id,"preparing","");}},
                        Err(error)=>{
                            crate::component_audio::report(id,if error=="语音正在使用，请结束后再播放"{"waiting"}else{"error"},&error);
                            if error=="语音正在使用，请结束后再播放"{narration.defer_start();if d.service=="news"{crate::component_news::playback_report(id,"waiting","语音正在使用，结束后开始播报");}}
                            else{narration.stop();if d.service=="news"{crate::component_news::playback_report(id,"error",&error);}result["values"]["metricValue"]=crate::widget_data::bounded_text(&error,63).into();}
                        }
                    }
                }
                if d.service=="news"{
                    let audio=crate::component_audio::status(id);
                    if desired&&audio["speechId"]==result["knowledge"]["current"]["id"]{
                        match audio["phase"].as_str().unwrap_or("idle"){
                            "error"=>crate::component_news::playback_report(id,"error",audio["error"].as_str().unwrap_or("播报失败，请重试")),
                            "idle"=>crate::component_news::playback_report(id,"interrupted","播报已中断，可按播报重新开始"),
                            "playing"=>crate::component_news::playback_report(id,"playing",""),
                            "buffering"=>crate::component_news::playback_report(id,"buffering","正在缓冲语音"),_=>{}
                        }
                    }
                    if narration.completed(){crate::component_news::finished(id);crate::component_news::playback_report(id,"completed","");}
                }
            }
        }
    }
    if d.service=="news"{
        let playback=crate::component_news::playback_status(id);
        match playback["phase"].as_str().unwrap_or("idle"){
            "error"=>{result["values"]["badge"]="播报失败".into();result["values"]["metricUnit"]="重试播报".into();},
            "waiting"=>{result["values"]["badge"]="等待播放".into();},
            "interrupted"=>{result["values"]["metricUnit"]="重新播报".into();},_=>{}
        }
        result["news"]["playback"]=playback;
    }
    if d.auto_read&&matches!(d.service.as_str(),"books"|"knowledge"){
        result["playback"]=crate::component_audio::playback_status(id);
    }
    Ok(result)
}
fn request_validated(id:&str,dir:&Path,d:&Declaration,runtime:&Value,input:Value,usb:&crate::usb_serial::UsbSerialManager,notify:&dyn Fn(Value)->Result<(),String>)->Result<Value,String>{
    let operation=input["operation"].as_str().ok_or("缺少组件操作")?;
    if operation=="disable"{crate::component_news::disable(id);crate::component_audio::stop(Some(id));crate::component_notes::cancel(Some(id));crate::component_service_settings::set_enabled(&dir,false)?;return Ok(json!({"enabled":false}));}
    if operation=="enable"{crate::component_service_settings::set_enabled(&dir,true)?;}
    let enabled=crate::component_service_settings::enabled(&dir)?;
    let mut values=defaults(&runtime);
    if !enabled&&operation!="enable"{
        crate::component_notes::cancel(Some(id));
        crate::component_audio::stop(Some(id));
        if operation!="status"{return Err("组件服务已手动停用，可在 PC 组件详情重新开启".into());}
        values["headline"]="服务已停用".into();values["metricValue"]="".into();values["note"]="可在 PC 组件详情重新开启；本机记录保留".into();
        return Ok(json!({"enabled":false,"values":values,"permissions":app_services::permissions(d),"service":d.service}));
    }
    let action=if let Some(action)=input["action"].as_str(){d.actions.get(action).ok_or("动作未在资源包声明")?.as_str()}else{operation};
    if (d.is_library()||matches!(d.service.as_str(),"todos"|"calendar"))&&!matches!(action,"status"|"enable")&&!d.actions.values().any(|v|v==action){return Err("动作未在资源包声明".into());}
    if !matches!(action,"status"|"enable")&&!app_services::operations(&d.service).contains(&action)&&!(d.service=="weather"&&matches!(action,"configure"|"locate"))&&!(d.service=="notes"&&action=="edit"){return Err("不支持的组件操作".into());}
    if d.service=="news"{
        if !matches!(action,"status"|"enable")&&!d.actions.values().any(|v|v==action){return Err("动作未在资源包声明".into());}
        if matches!(action,"refresh"|"toggle"|"previous"|"next"){crate::component_audio::stop(Some(id));}
        let mut result=crate::component_news::request_with_surface(id,d,action,input["_deviceEpoch"].as_u64(),input["_entered"]==true,&runtime["surface"])?;
        if action=="toggle"{crate::component_news::playback_report(id,if result["knowledge"]["choosing"]==true{"stopped"}else{"waiting"},"");}
        bounded(&mut result["values"]);
        return Ok(result);
    }
    let path=dir.join("state.json");let mut state=load(&path)?;let before=state.clone();
    let board=usb.status().board_device_id;
    apply(id,&dir,&d,&mut state,action,&input,&board,notify,usb,&mut values)?;
    if state!=before{atomic(&path,&state)?;}
    bounded(&mut values);
    let library=d.is_library();
    let knowledge=if library{crate::knowledge_library::info(&state.knowledge,&d.content)}else{Value::Null};
    // Shuffle bags stay private; never ship the thousand-card corpus on each poll.
    let mut public_state=serde_json::to_value(&state).map_err(|_|"组件状态无法序列化")?;
    if library{public_state["knowledge"].as_object_mut().unwrap().remove("bags");}
    Ok(json!({"enabled":true,"values":values,"ui":controls(id,dir,d,&state)?,"permissions":app_services::permissions(d),"service":d.service,"state":public_state,"content":if library||d.service=="calendar"{&[][..]}else{d.content.as_slice()},"knowledge":knowledge,"locationProfile":if d.service=="weather"{serde_json::to_value(d.location.as_ref().unwrap_or(crate::location_profile::legacy())).unwrap_or(Value::Null)}else{Value::Null},"captureDiagnostics":if d.service=="notes"{crate::component_notes::diagnostics(id)}else{Value::Null},"notes":if d.service=="notes"&&input["includeNotes"]==true{crate::component_notes::list(&dir)?}else{json!([])}}))
}
/// Exercise production host logic with isolated user storage; never launch a desktop app.
#[cfg(test)]
pub(crate) fn test_request(package:&Path,root:&Path,input:Value,usb:&crate::usb_serial::UsbSerialManager)->Result<Value,String>{
    let entry=crate::component_library::inspect(package)?;
    if !entry.valid{return Err(format!("invalid package: {:?}",entry.validation_errors));}
    let runtime=entry.app_runtime.ok_or("missing application runtime")?;
    let d=app_services::parse(&std::fs::read(package.join("pc/application.json")).map_err(|e|e.to_string())?,&runtime)?;
    request_validated(&entry.id,&root.join(&entry.id),&d,&runtime,input,usb,&|_|Err("hardware acceptance deliberately excludes microphone and task dispatch".into()))
}
pub fn device_event(event:&Value){
    let Some(h)=HOST.get()else{return;};let board=h.usb.status();
    if !board.connected||event["boardDeviceId"]!=board.board_device_id{return;}
    let Some(id)=event["componentId"].as_str().filter(|s|crate::component_extension::valid_id(s))else{return;};
    let Some(instance)=event["instance"].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|b|b.is_ascii_hexdigit()||b==b'-'))else{return;};
    if event["sequence"].as_u64().is_none_or(|n|n==0||n>u32::MAX as u64){return;}
    let Some(visible)=event["visible"].as_bool()else{return;};
    let generation=h.device_epoch.lock().unwrap().observe(&board.board_device_id,id,instance,visible);
    // Visibility/heartbeats cannot wait behind filesystem or network work.
    crate::component_audio::heartbeat(id,visible);
    crate::component_news::observe(id,visible);
    if !visible{crate::component_notes::stop_recording(id);return;}
    let mut queued=event.clone();queued["_deviceEpoch"]=generation.into();
    if h.events.try_send(queued).is_err()&&event["action"]!="status"{
        let _=h.usb.try_send_app_snapshot(&board.board_device_id,&json!({"componentId":id,"instance":instance,"sequence":event["sequence"],"ui":{"mode":7,"actions":[]},"values":{"headline":"操作未接收","metricValue":"","note":"操作队列繁忙，请稍后再按；未自动重试"}}));
    }
}
fn process_event(event:Value){
    let Some(h)=HOST.get()else{return;};let board=h.usb.status();
    if !board.connected||event["boardDeviceId"]!=board.board_device_id{return;}
    let Some(id)=event["componentId"].as_str().filter(|id|crate::component_extension::valid_id(id))else{return;};
    let Some(instance)=event["instance"].as_str().filter(|s|!s.is_empty()&&s.len()<=31&&s.bytes().all(|c|c.is_ascii_hexdigit()||c==b'-'))else{return;};
    let Some(sequence)=event["sequence"].as_u64().filter(|n|*n>0&&*n<=u32::MAX as u64)else{return;};
    let Some(action)=event["action"].as_str().filter(|s|s.len()<=47)else{return;};
    if !event["_deviceEpoch"].as_u64().is_some_and(|e|h.device_epoch.lock().unwrap().accepts(e)){return;}
    if action!="status"{
        if event["visible"]!=true||crate::realtime_chat::status().active{return;}
        let key=format!("{}:{id}:{instance}:{sequence}",board.board_device_id);let mut seen=h.seen.lock().unwrap();if seen.contains(&key){return;}seen.push_back(key);if seen.len()>128{seen.pop_front();}
    }
    let input=if action=="status"{json!({"operation":"status","_deviceEpoch":event["_deviceEpoch"]})}else{json!({"operation":"action","action":action,"_deviceEpoch":event["_deviceEpoch"],"confirm":event["confirmed"]==true,"context":event["context"]})};
    let (mut values,ui)=match request(id,input){Ok(v)=>(v["values"].clone(),v.get("ui").cloned().unwrap_or(json!({"mode":7,"actions":[]}))),Err(e)=>(json!({"headline":"操作未完成","metricValue":"","note":e}),json!({"mode":7,"actions":[]}))};bounded(&mut values);
    if !event["_deviceEpoch"].as_u64().is_some_and(|e|h.device_epoch.lock().unwrap().accepts(e)){return;}
    let _=h.usb.send_app_snapshot(&board.board_device_id,&json!({"componentId":id,"instance":instance,"sequence":sequence,"values":values,"ui":ui}));
}
#[tauri::command]
pub async fn component_app_request(component_id:String,input:Value)->Result<Value,String>{
    if input.as_object().is_none_or(|o|o.keys().any(|k|k.starts_with('_'))){return Err("组件请求包含保留字段".into());}
    tauri::async_runtime::spawn_blocking(move||request(&component_id,input)).await.map_err(|_|"组件请求中断")?
}

#[cfg(test)]mod tests{use super::*;#[test]fn journey_reward_is_idempotent_and_survives_exit(){let d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v1".into(),service:"journey".into(),location:None,actions:Default::default(),content:vec![json!({"id":"coast"})]};let mut s=State{trip:Some(Trip{id:"coast".into(),started:100,arrival:160}),..Default::default()};assert!(!settle_trip(&mut s,&d,159));assert!(settle_trip(&mut s,&d,161));assert!(!settle_trip(&mut s,&d,200));assert_eq!(s.visits,1);assert_eq!(s.favorites,vec!["coast"]);}
#[test]fn weather_never_recommends_completed_todos(){
    let row=|tone|crate::widget_data::DataRow{id:"test".into(),label:"测试".into(),value:"10/20".into(),detail:String::new(),meta:String::new(),tone};
    assert_eq!(pending_todo(&[]),None);assert_eq!(pending_todo(&[row(-1)]),None);
    assert_eq!(pending_todo(&[row(-1),row(1)]),Some("测试 10/20".into()));
}
#[test]fn snapshots_bound_utf8_not_bytes_mid_character(){let mut v=json!({"headline":"好".repeat(90),"note":"私密\n文本"});bounded(&mut v);assert_eq!(v["headline"].as_str().unwrap().len(),159);assert_eq!(v["note"],"私密文本");}
#[test]#[ignore="completed knowledge package; isolated storage, no hardware or speech"]
fn knowledge_package_host_persists_selection_and_keeps_poll_small(){
    let package=PathBuf::from(std::env::var("PET_KNOWLEDGE_PACKAGE").unwrap());let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let first=test_request(&package,root.path(),json!({"operation":"status"}),&usb).unwrap();
    assert_eq!(first["knowledge"]["total"],1000);assert_eq!(first["content"],json!([]));
    assert!(first["state"]["knowledge"].get("bags").is_none());assert!(serde_json::to_vec(&first).unwrap().len()<16000);
    let runtime:Value=serde_json::from_slice(&std::fs::read(package.join("runtime/widget.json")).unwrap()).unwrap();
    let d=app_services::parse(&std::fs::read(package.join("pc/application.json")).unwrap(),&runtime).unwrap();
    let dir=root.path().join("hachimi-learn");
    let request=|operation:&str|request_validated("hachimi-learn",&dir,&d,&runtime,json!({"operation":operation}),&usb,&|_|Err("not allowed".into())).unwrap();
    assert_eq!(request("status")["knowledge"],first["knowledge"]);
    let next=request("next");assert_eq!(next["knowledge"]["page"],1);
    let saved=std::fs::read(dir.join("state.json")).unwrap();assert_eq!(request("status")["knowledge"],next["knowledge"]);assert_eq!(std::fs::read(dir.join("state.json")).unwrap(),saved);
    request("category");request("next");let cancel=request("category");assert_eq!(cancel["knowledge"]["current"]["id"],first["knowledge"]["current"]["id"]);
    request("category");request("next");let changed=request("random");assert_ne!(changed["knowledge"]["category"],first["knowledge"]["category"]);
    assert_eq!(changed["knowledge"]["page"],0);let id=changed["knowledge"]["current"]["id"].clone();
    if d.actions.values().any(|v|v=="favorite"){
        let fav=request("favorite");assert!(fav["state"]["favorites"].as_array().unwrap().contains(&id));
        let unfav=request("favorite");assert!(!unfav["state"]["favorites"].as_array().unwrap().contains(&id));
    }else{
        assert!(request_validated("hachimi-learn",&dir,&d,&runtime,json!({"operation":"favorite"}),&usb,&|_|Err("not allowed".into())).is_err());
    }
    let disabled=request("disable");assert_eq!(disabled["enabled"],false);assert_eq!(request("status")["enabled"],false);
    let enabled=request("enable");assert_eq!(enabled["knowledge"]["current"]["id"],id);assert_eq!(enabled["enabled"],true);
}
#[test]fn persisted_indices_are_bounded_without_rewriting_private_records(){
    let dir=tempfile::tempdir().unwrap();let p=dir.path().join("state.json");
    atomic(&p,&State{selected:usize::MAX,sound_timer:usize::MAX,sound_volume:Some(u32::MAX),favorites:vec!["coast".into(),"coast".into()],..Default::default()}).unwrap();
    let before=std::fs::read(&p).unwrap();let s=load(&p).unwrap();assert!(s.selected<64&&s.sound_timer<3);assert_eq!(s.sound_volume,Some(100));assert_eq!(s.favorites.len(),1);assert_eq!(std::fs::read(&p).unwrap(),before);
    atomic(&p,&State{trip:Some(Trip{id:"coast".into(),started:i64::MIN,arrival:i64::MAX}),..Default::default()}).unwrap();assert!(load(&p).is_err());
}
#[test]fn queued_actions_expire_on_exit_switch_and_reentry(){
    let mut e=DeviceEpoch::default();let first=e.observe("board","notes","1234",true);assert!(e.accepts(first));
    assert_eq!(e.observe("board","notes","1234",true),first);
    e.observe("board","notes","1234",false);assert!(!e.accepts(first));
    let second=e.observe("board","notes","1234",true);assert!(e.accepts(second));assert!(!e.accepts(first));
    e.observe("board","sounds","5678",true);assert!(!e.accepts(second));
}
#[test]fn manual_weather_city_survives_status_and_late_auto_result(){
    let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v1".into(),service:"weather".into(),location:None,actions:Default::default(),content:vec![]};
    let city=crate::app_weather::Location{city:"北京".into(),lat:39.912,lon:116.401};
    crate::app_weather::seed_forecast_test(&city);
    let auto=crate::app_weather::Location{city:"上海".into(),lat:31.23,lon:121.47};
    atomic(&root.path().join("state.json"),&State{auto_city:true,geo:crate::app_weather::GeoCache{location:auto,next:i64::MAX,..Default::default()},..Default::default()}).unwrap();
    let call=|input|request_validated("weather-test",root.path(),&d,&json!({"dashboard":{}}),input,&usb,&|_|Err("no dispatch".into())).unwrap();
    let saved=call(json!({"operation":"configure","city":city}));
    assert_eq!(saved["state"]["city"]["city"],"北京");assert_eq!(saved["state"]["auto_city"],false);
    assert!(saved["values"]["headline"].as_str().unwrap().contains("北京"));
    for _ in 0..3{let next=call(json!({"operation":"status"}));assert_eq!(next["state"]["city"]["city"],"北京");assert!(!next["values"]["headline"].as_str().unwrap().contains("先选"));}
    assert_eq!(load(&root.path().join("state.json")).unwrap().city.city,"北京");
}
#[test]fn device_journey_selection_departure_and_duplicate_do_not_restart_trip(){
    let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v1".into(),service:"journey".into(),location:None,actions:[("host.next".into(),"next".into()),("host.previous".into(),"previous".into()),("host.primary".into(),"depart".into())].into(),content:vec![json!({"id":"coast","title":"海岸","text":"海边","minutes":15}),json!({"id":"forest","title":"森林","text":"林中","minutes":30})]};
    let call=|action:&str|request_validated("journey-test",root.path(),&d,&json!({"dashboard":{}}),json!({"operation":"action","action":action}),&usb,&|_|Err("no dispatch".into())).unwrap();
    assert_eq!(call("host.next")["values"]["headline"],"森林");
    let started=call("host.primary")["state"]["trip"].clone();
    assert_eq!(started["id"],"forest");
    let active=call("host.primary");
    assert!(!active["ui"]["actions"].as_array().unwrap().contains(&json!("host.primary")));
    assert_eq!(active["state"]["trip"],started);
    let candidate=call("host.previous");assert_eq!(candidate["values"]["headline"],"海岸");
    assert!(candidate["ui"]["actions"].as_array().unwrap().contains(&json!("host.primary")));
    assert_eq!(candidate["values"]["metricLabel"],"确认下一站");
    assert_eq!(candidate["state"]["selected"],1); // browsing is not confirmation
    let confirmed=call("host.primary");assert_eq!(confirmed["state"]["trip"],started);
    assert_eq!(confirmed["state"]["selected"],0);
    assert_eq!(confirmed["values"]["headline"],"森林");
    assert!(confirmed["values"]["note"].as_str().unwrap().contains("下一站：海岸"));
    call("host.next");
    let cancelled=request_validated("journey-test",root.path(),&d,&json!({"dashboard":{}}),json!({"operation":"collection"}),&usb,&|_|Err("no dispatch".into())).unwrap();
    assert_eq!(cancelled["state"]["trip"],started);assert_eq!(cancelled["state"]["selected"],0);
    assert_eq!(cancelled["state"]["trip_candidate"],Value::Null);
    let cards=request_validated("journey-test",root.path(),&d,&json!({"dashboard":{}}),json!({"operation":"collection"}),&usb,&|_|Err("no dispatch".into())).unwrap();
    assert_eq!(cards["state"]["collection"],true);
    let browsed=call("host.next");
    assert_eq!(browsed["state"]["collection"],true);
    assert_eq!(browsed["state"]["trip_candidate"],Value::Null);
    assert_eq!(browsed["state"]["trip"],started);
    call("host.previous");
    request_validated("journey-test",root.path(),&d,&json!({"dashboard":{}}),json!({"operation":"collection"}),&usb,&|_|Err("no dispatch".into())).unwrap();
    let mut saved=load(&root.path().join("state.json")).unwrap();
    let arrival=saved.trip.as_ref().unwrap().arrival;
    assert!(settle_trip(&mut saved,&d,arrival));assert_eq!(saved.selected,0);
    assert!(!saved.collection);assert_eq!(saved.favorites,vec!["forest"]);
    assert!(!settle_trip(&mut saved,&d,arrival+1));assert_eq!(saved.visits,1);
}
#[test]fn journey_default_postcards_do_not_fabricate_visits(){
    let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v1".into(),service:"journey".into(),location:None,actions:Default::default(),content:vec![json!({"id":"coast","title":"海岸","text":"一张默认海岸明信片","minutes":15}),json!({"id":"forest","title":"森林","text":"一张默认森林明信片","minutes":30})]};
    let mut state=State::default();let mut values=json!({});
    apply("postcard-test",root.path(),&d,&mut state,"collection",&json!({}),"",&|_|Ok(()),&usb,&mut values).unwrap();
    assert_eq!(values["note"],"一张默认海岸明信片");
    assert!(values["eyebrow"].as_str().unwrap().contains("默认"));
    assert!(state.favorites.is_empty());assert_eq!(state.visits,0);
    apply("postcard-test",root.path(),&d,&mut state,"next",&json!({}),"",&|_|Ok(()),&usb,&mut values).unwrap();
    assert_eq!(values["note"],"一张默认森林明信片");
    assert!(state.favorites.is_empty());assert_eq!(state.visits,0);
    state.favorites.push("forest".into());
    apply("postcard-test",root.path(),&d,&mut state,"status",&json!({}),"",&|_|Ok(()),&usb,&mut values).unwrap();
    assert!(values["eyebrow"].as_str().unwrap().contains("带回"));
    assert_eq!(state.favorites,vec!["forest"]);
}
#[test]fn resource_only_weather_update_changes_copy_but_preserves_city(){
    let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let mut d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v2".into(),service:"weather".into(),location:Some(crate::location_profile::legacy().clone()),actions:Default::default(),content:vec![]};
    let call=|d:&Declaration,input|request_validated("weather-package-upgrade",root.path(),d,&json!({"dashboard":{}}),input,&usb,&|_|Err("no dispatch".into())).unwrap();
    call(&d,json!({"operation":"configure","city":{"city":"北京","lat":39.904,"lon":116.407}}));
    let p=d.location.as_mut().unwrap();
    p.cities=vec![crate::app_weather::Location{city:"青岛".into(),lat:36.067,lon:120.383}];
    p.text.insert("failedHeadline".into(),"{city} · 资源包的新提示".into());
    p.forecast.request.endpoint="https://forecast.example.com/new".into();
    p.validate().unwrap();
    let result=call(&d,json!({"operation":"status"}));
    assert_eq!(result["state"]["city"]["city"],"北京");
    assert_eq!(result["values"]["headline"],"北京 · 资源包的新提示");
    assert_eq!(result["locationProfile"]["cities"][0]["city"],"青岛");
    assert!(result["permissions"]["networkHosts"].as_array().unwrap().contains(&json!("forecast.example.com")));
    call(&d,json!({"operation":"disable"}));
    let before=std::fs::read(root.path().join("state.json")).unwrap();
    let status=call(&d,json!({"operation":"status"}));assert_eq!(status["enabled"],false);
    assert_eq!(before,std::fs::read(root.path().join("state.json")).unwrap());
}
#[test]fn weather_device_picker_confirms_cancels_wraps_and_preserves_location(){
    let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let d=Declaration{schema_version:1,auto_read:false,engine:"pc-app-services-v2".into(),service:"weather".into(),location:Some(crate::location_profile::legacy().clone()),actions:[
        ("host.choose".into(),"choose_city".into()),("host.previous".into(),"previous".into()),("host.next".into(),"next".into()),
        ("host.primary".into(),"refresh".into()),("host.secondary".into(),"todo".into())].into(),content:vec![]};
    let call=|input|request_validated("weather-picker",root.path(),&d,&json!({"dashboard":{}}),input,&usb,&|_|Err("no dispatch".into())).unwrap();
    assert_eq!(call(json!({"operation":"status"}))["values"]["eyebrow"],"选择城市");
    call(json!({"operation":"configure","city":{"city":"北京","lat":39.904,"lon":116.407}}));
    let action=|a|call(json!({"operation":"action","action":a}));
    assert_eq!(action("host.choose")["values"]["headline"],"北京");
    let browsing=action("host.next");
    assert_eq!(browsing["values"]["headline"],"上海");assert_eq!(browsing["state"]["city"]["city"],"北京");
    assert_eq!(call(json!({"operation":"status"}))["values"]["metricLabel"],"确认城市");
    let cancel=action("host.secondary");assert_eq!(cancel["state"]["city"]["city"],"北京");assert_eq!(cancel["state"]["collection"],false);
    action("host.choose");action("host.next");
    let saved=action("host.primary");
    assert_eq!(saved["state"]["city"]["city"],"上海");assert_eq!(saved["state"]["city_select"],false);
    assert_eq!(saved["values"]["eyebrow"],"上海 · 切换");
    assert_eq!(call(json!({"operation":"status"}))["values"]["eyebrow"],"上海 · 切换");
    action("host.previous");assert_eq!(action("host.previous")["values"]["headline"],"天津");
    assert_eq!(action("host.next")["values"]["headline"],"北京");
    action("host.secondary");
    assert_eq!(action("host.secondary")["values"]["eyebrow"],"上海 · 切换"); // todo view keeps location
    call(json!({"operation":"configure","city":{"city":"自定义城市","lat":20.1,"lon":110.2}}));
    action("host.choose");let unchanged=action("host.secondary");
    assert_eq!(unchanged["state"]["city"]["city"],"自定义城市");
}
#[test]
#[ignore="Requires the current authored packages; isolated state, no microphone, speaker, network or task dispatch"]
fn authored_host_services_complete_local_cycles(){
    let packages=PathBuf::from(std::env::var("PET_APP_PACKAGE_DIR").unwrap());let root=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
    let run=|id:&str,input:Value|test_request(&packages.join(id),root.path(),input,&usb);
    for id in ["hachimi-travel","hachimi-weather","hachimi-notes","hachimi-learn","hachimi-sounds"]{
        let initial=run(id,json!({"operation":"status","includeNotes":true})).unwrap();assert_eq!(initial["enabled"],true);
        assert!(initial["values"]["headline"].as_str().is_some_and(|s|!s.is_empty()));
        assert_eq!(run(id,json!({"operation":"disable"})).unwrap()["enabled"],false);
        assert_eq!(run(id,json!({"operation":"status"})).unwrap()["enabled"],false);
        assert!(run(id,json!({"operation":"action","action":"undeclared"})).is_err());
        assert_eq!(run(id,json!({"operation":"enable"})).unwrap()["enabled"],true);
    }
    let id="hachimi-travel";let first=run(id,json!({"operation":"depart"})).unwrap();let started=first["state"]["trip"].clone();
    run(id,json!({"operation":"next"})).unwrap();assert_eq!(run(id,json!({"operation":"depart"})).unwrap()["state"]["trip"],started);
    let path=root.path().join(id).join("state.json");let mut state=load(&path).unwrap();let now=chrono::Utc::now().timestamp();state.trip.as_mut().unwrap().started=now-1000;state.trip.as_mut().unwrap().arrival=now-1;atomic(&path,&state).unwrap();
    let arrived=run(id,json!({"operation":"status"})).unwrap();assert_eq!(arrived["state"]["visits"],1);assert_eq!(arrived["state"]["favorites"],json!(["coast"]));assert_eq!(run(id,json!({"operation":"status"})).unwrap()["state"]["visits"],1);
    let id="hachimi-learn";let old=run(id,json!({"operation":"status"})).unwrap();
    assert!(run(id,json!({"operation":"favorite"})).is_err()); // removed UI must not leave a hidden command
    let new=run(id,json!({"operation":"random"})).unwrap();
    assert_ne!(new["state"]["knowledge"]["article"],old["state"]["knowledge"]["article"]);
    assert!(new["content"].as_array().unwrap().is_empty()); // no thousand-card IPC payload
    assert!(new["state"]["knowledge"].get("bags").is_none());
    let page=run(id,json!({"operation":"next"})).unwrap();assert_eq!(page["state"]["knowledge"]["page"],1);
    assert_eq!(run(id,json!({"operation":"previous"})).unwrap()["state"]["knowledge"]["page"],0);
    let category=run(id,json!({"operation":"category"})).unwrap();assert_eq!(category["state"]["knowledge"]["choosing"],true);
    run(id,json!({"operation":"next"})).unwrap();
    assert_eq!(run(id,json!({"operation":"random"})).unwrap()["state"]["knowledge"]["choosing"],false);
    let id="hachimi-sounds";assert_eq!(run(id,json!({"operation":"next"})).unwrap()["values"]["headline"],"海边潮汐");
    assert_eq!(run(id,json!({"operation":"up"})).unwrap()["values"]["metricValue"],"40%");
    assert_eq!(run(id,json!({"operation":"timer"})).unwrap()["values"]["metricUnit"],"30 分钟");
    assert_eq!(run(id,json!({"operation":"status"})).unwrap()["state"]["sound_volume"],40);
    for (op,title) in [("rain","窗边细雨"),("sea","海边潮汐"),("fire","暖炉微火"),("insects","安静虫鸣")]{
        let state=run(id,json!({"operation":"action","action":format!("host.{op}")})).unwrap();
        assert_eq!(state["values"]["headline"],title);
        assert_eq!(state["values"]["metricUnit"],"30 分钟");
        assert!(!crate::component_audio::playing(id)); // selection alone is not consent to playback
    }
    let id="hachimi-notes";assert!(run(id,json!({"operation":"save"})).is_err());assert!(run(id,json!({"operation":"todo"})).is_err());assert!(run(id,json!({"operation":"delete"})).unwrap()["values"]["headline"].as_str().unwrap().contains("确认"));
    assert!(!root.path().join(id).join("notes.json").exists());assert_eq!(crate::component_notes::diagnostics(id),Value::Null);
    assert!(!run("hachimi-weather",json!({"operation":"status"})).unwrap()["values"]["headline"].as_str().unwrap().contains("先选"));
    assert!(run("hachimi-weather",json!({"operation":"configure","city":{"city":"bad","lat":91,"lon":0}})).is_err());
    assert!(!crate::component_audio::playing("hachimi-sounds"));assert!(!crate::component_audio::playing("hachimi-learn"));
}
}
