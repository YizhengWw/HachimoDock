use super::*;
use crate::{app_services::Declaration, music_player::LocalPlayer, usb_serial::UsbSerialManager};
use std::{cell::RefCell,collections::HashMap,path::Path,sync::{Arc,Mutex,OnceLock,atomic::{AtomicBool,Ordering}}};

#[derive(Clone,Deserialize)]
#[serde(tag="kind",rename_all="kebab-case",deny_unknown_fields)]
enum Source {
    Service {declaration:Value,runtime:Value,#[serde(default)] status:Option<Status>,#[serde(default)] parameters:BTreeMap<String,BTreeMap<String,String>>},
    LocalMedia {},
}
#[derive(Clone,Deserialize)]
#[serde(deny_unknown_fields)]
struct Status{pointer:String,phases:BTreeMap<String,String>}
fn source(v:&Value)->Result<Source>{serde_json::from_value(v.clone()).map_err(|_|fail())}
fn declaration(d:&Value,r:&Value)->Result<Declaration>{
    // A service cannot recursively install a broker or add automatic narration.
    if d["engine"]==ENGINE||d["autoRead"]==true{return Err(fail());}
    crate::app_services::parse(&serde_json::to_vec(d).map_err(|_|fail())?,r)
}
fn media_methods()->BTreeMap<String,Method>{
    let mut out=BTreeMap::new();
    for method in ["toggle","pause","resume","stop","previous","next","volume_up","volume_down","cycle_mode"]{out.insert(method.into(),Method{arguments:BTreeMap::new(),confirmation:false});}
    for (method,field,kind) in [("play","key","string"),("remove","key","string"),("seek","positionMs","int"),("seek_percent","percent","int"),("volume","volume","int"),("mode","mode","string")]{
        out.insert(method.into(),Method{arguments:[(field.into(),kind.into())].into(),confirmation:method=="remove"});
    }
    out
}
pub struct Catalog;
impl Provider for Catalog {
    fn validate(&self,v:&Value)->Result<()>{match source(v)?{
        Source::LocalMedia{}=>Ok(()),
        Source::Service{declaration:d,runtime:r,parameters,status}=>{
            let d=declaration(&d,&r)?;
            if status.as_ref().is_some_and(|s|!pointer(&s.pointer)||s.phases.is_empty()||s.phases.len()>12||s.phases.iter().any(|(k,v)|!text(k,31)||!PHASES.contains(&v.as_str()))){return Err(fail());}
            if parameters.values().any(|fields|!fields.is_empty()){return Err("此原生服务尚未注册带参数方法".into());}
            if parameters.iter().any(|(method,fields)|!d.actions.contains_key(method)||fields.len()>4||fields.iter().any(|(k,v)|!name(k,31)||!matches!(v.as_str(),"int"|"string"|"bool"))){return Err(fail());}
            Ok(())
        }
    }}
    fn permissions(&self,v:&Value)->Result<Value>{match source(v)?{
        Source::LocalMedia{}=>Ok(json!({"storage":true,"localAudioImport":true,"deviceAudio":true,"networkHosts":[],"microphone":false,"speechProvider":false})),
        Source::Service{declaration:d,runtime:r,..}=>Ok(crate::app_services::permissions(&declaration(&d,&r)?)),
    }}
    fn methods(&self,v:&Value)->Result<BTreeMap<String,Method>>{match source(v)?{
        Source::LocalMedia{}=>Ok(media_methods()),
        Source::Service{declaration:d,runtime:r,parameters,..}=>{
            let d=declaration(&d,&r)?;
            Ok(d.actions.keys().map(|a|(a.clone(),Method{arguments:parameters.get(a).cloned().unwrap_or_default(),confirmation:r["input"]["confirm"].get(a).is_some()})).collect())
        }
    }}
    fn snapshot(&self,_:&str,_:&Value)->Result<Snapshot>{Err(fail())}
    fn command(&self,_:&str,_:&Value,_:&str,_:&Value,_:&str,_:&str,_:bool)->Result<()>{Err(fail())}
}

struct Player {definition:String,player:LocalPlayer}
static VIEWS:OnceLock<Mutex<HashMap<String,std::sync::Arc<Mutex<String>>>>>=OnceLock::new();
fn view(id:&str)->Result<std::sync::Arc<Mutex<String>>>{let mut all=VIEWS.get_or_init(Default::default).lock().map_err(|_|fail())?;if !all.contains_key(id)&&all.len()>=64{return Err("服务视图达到上限".into());}Ok(all.entry(id.into()).or_default().clone())}
static PLAYERS:OnceLock<Mutex<HashMap<String,Player>>>=OnceLock::new();
fn players()->&'static Mutex<HashMap<String,Player>>{PLAYERS.get_or_init(Default::default)}
pub struct Adapter<'a>{pub id:&'a str,pub dir:&'a Path,pub usb:&'a UsbSerialManager,pub notify:&'a dyn Fn(Value)->Result<()>,pub views:RefCell<BTreeMap<String,String>>,pub revoked:Option<&'a AtomicBool>}
impl Adapter<'_> {
    fn current(&self)->Result<()>{if self.revoked.is_some_and(|r|r.load(Ordering::Acquire)){Err("服务视图已撤销；操作未重放".into())}else{Ok(())}}
    fn player(&self,alias:&str,d:&Value)->Result<LocalPlayer>{
        self.current()?;
        let key=format!("{}:{alias}",self.id);let signature=digest(d)?;
        {let all=players().lock().map_err(|_|fail())?;if let Some(old)=all.get(&key){if old.definition==signature{return Ok(old.player.clone());}}if !all.contains_key(&key)&&all.len()>=16{return Err("媒体服务实例达到上限".into());}}
        // No registry lock spans filesystem work or any provider operation.
        let player=LocalPlayer::open(self.id,self.dir.join("services").join(alias),self.usb.clone(),view(self.id)?)?;
        let mut all=players().lock().map_err(|_|fail())?;
        if let Some(old)=all.get(&key){if old.definition==signature{return Ok(old.player.clone());}old.player.interrupt();}
        if !all.contains_key(&key)&&all.len()>=16{return Err("媒体服务实例达到上限".into());}
        all.insert(key,Player{definition:signature,player:player.clone()});Ok(player)
    }
    fn service_call(&self,alias:&str,d:&Value,r:&Value,input:Value)->Result<Value>{
        let d=declaration(d,r)?;
        crate::component_app::request_validated(self.id,&self.dir.join("services").join(alias),&d,r,input,self.usb,self.notify)
    }
}
fn native_snapshot(v:Value)->Result<Snapshot>{
    let observation=v["ui"]["context"].as_str().filter(|s|name(s,64)).map(str::to_owned).unwrap_or(digest(&v)?);
    let actions=v["ui"]["actions"].as_array().map(|a|a.iter().filter_map(Value::as_str).map(str::to_owned).collect()).unwrap_or_default();
    let phase=if v["enabled"]==false{"unavailable"}else{"ready"};Ok(Snapshot{value:v,observation,actions,phase:phase.into()})
}
impl Provider for Adapter<'_>{
    fn validate(&self,v:&Value)->Result<()>{Catalog.validate(v)}
    fn permissions(&self,v:&Value)->Result<Value>{Catalog.permissions(v)}
    fn methods(&self,v:&Value)->Result<BTreeMap<String,Method>>{Catalog.methods(v)}
    fn snapshot(&self,alias:&str,v:&Value)->Result<Snapshot>{self.current()?;match source(v)?{
        Source::Service{declaration:d,runtime:r,status,..}=>{
            let mut snapshot=native_snapshot(self.service_call(alias,&d,&r,json!({"operation":"status","includeNotes":true}))?)?;
            if let Some(status)=status{
                snapshot.phase=snapshot.value.pointer(&status.pointer).and_then(Value::as_str).and_then(|p|status.phases.get(p)).cloned().unwrap_or_else(||"unavailable".into());
                if snapshot.phase=="unavailable"{snapshot.actions.clear();}
            }Ok(snapshot)
        },
        Source::LocalMedia{}=>{
            let player=self.player(alias,v)?;let stamp=player.view_stamp();self.views.borrow_mut().insert(alias.into(),stamp.clone());
            let mut v=player.snapshot();
            // Display projections have the bridge's 63-byte scalar/row bounds;
            // preserve the full private library title and never leak its paths.
            v["title"]=crate::widget_data::bounded_text(v["current"]["title"].as_str().unwrap_or("尚未选择歌曲"),63).into();
            v["artist"]=crate::widget_data::bounded_text(v["current"]["artist"].as_str().unwrap_or("本地曲库"),63).into();
            let position=v["positionMs"].as_u64().unwrap_or(0);let duration=v["durationMs"].as_u64().unwrap_or(0);
            v["progressPercent"]=if duration==0{0}else{position.min(duration).saturating_mul(100)/duration}.into();
            v["modeLabel"]=match v["mode"].as_str(){Some("single")=>"单曲循环",Some("loop")=>"列表循环",Some("shuffle")=>"随机播放",_=>"顺序播放"}.into();
            v["stateLabel"]=match v["state"].as_str(){Some("playing")=>"正在播放",Some("paused")=>"已暂停",Some("buffering"|"loading")=>"准备音频",Some("ended")=>"播放完成",Some("interrupted")=>"播放已中断",Some("error")=>"播放失败",_=>"等待播放"}.into();
            if let Some(rows)=v["queue"].as_array_mut(){for row in rows{
                for field in ["title","artist"]{row[field]=crate::widget_data::bounded_text(row[field].as_str().unwrap_or(""),63).into();}
            }}
            let observation=digest(&(stamp,&v["generation"],&v["state"],&v["current"],&v["mode"],&v["volume"],&v["queue"]))?;
            let mut actions: BTreeSet<_>=["volume","mode","volume_up","volume_down","cycle_mode"].into_iter().map(str::to_owned).collect();
            if v["queue"].as_array().is_some_and(|q|!q.is_empty()){
                actions.extend(["play","remove"].into_iter().map(str::to_owned));
                let has_current=v["current"].is_object();
                if has_current{actions.extend(["toggle","stop","previous","next"].into_iter().map(str::to_owned));}
                match v["state"].as_str(){
                    Some("playing"|"buffering")=>{actions.insert("pause".into());},
                    Some("paused")=>{actions.insert("resume".into());},_=>{}
                }
                if has_current&&v["durationMs"].as_u64().is_some_and(|n|n>0){actions.extend(["seek","seek_percent"].into_iter().map(str::to_owned));}
            }
            let phase=match v["state"].as_str(){Some("playing")=>"running",Some("paused")=>"paused",Some("ended")=>"completed",Some("error")=>"failed",Some("interrupted")=>"interrupted",Some("loading"|"queued"|"buffering")=>"preparing",_=>"idle"};
            Ok(Snapshot{value:v.clone(),observation,actions,phase:phase.into()})
        }
    }}
    fn image(&self,alias:&str,slot:&str,value:&Value)->Result<ImageRef>{
        if let Some(data)=value.as_str(){assets::register(self.id,&format!("{alias}.{slot}"),data)}else{let r=ImageRef::parse(value)?;assets::preview(self.id,&r)?;Ok(r)}
    }
    fn command(&self,alias:&str,v:&Value,method:&str,args:&Value,observation:&str,event_id:&str,confirmed:bool)->Result<()>{
        let latest=self.snapshot(alias,v)?;
        if latest.observation!=observation||!latest.actions.contains(method){return Err(fail());}
        self.current()?;
        match source(v)?{
            Source::Service{declaration:d,runtime:r,..}=>{
                self.service_call(alias,&d,&r,json!({"operation":"action","action":method,"data":args,"context":latest.value["ui"]["context"],"eventId":event_id,"_eventId":event_id,"confirm":confirmed}))?;
            }
            Source::LocalMedia{}=>{
                let stamp=self.views.borrow().get(alias).cloned().ok_or_else(fail)?;
                let (method,args)=match method{
                    "volume_up"|"volume_down"=>{
                        let n=latest.value["volume"].as_i64().filter(|v|(0..=100).contains(v)).ok_or_else(fail)?;
                        ("volume",json!({"volume":(n+if method=="volume_up"{5}else{-5}).clamp(0,100)}))
                    },
                    "cycle_mode"=>{
                        let next=match latest.value["mode"].as_str(){Some("sequence")=>"single",Some("single")=>"loop",Some("loop")=>"shuffle",_=>"sequence"};
                        ("mode",json!({"mode":next}))
                    },
                    _=>(method,args.clone())
                };
                self.player(alias,v)?.command_observed(method,&args,&stamp)?;
            }
        }Ok(())
    }
}

struct Cached{definition:String,session:Mutex<Session>,revoked:AtomicBool}
impl Cached {
    fn revoke(&self){self.revoked.store(true,Ordering::Release);}
}
// The registry protects only bounded membership. Provider/USB/filesystem work
// holds one session mutex, never the registry or a reader-thread wait.
static SESSIONS:OnceLock<Mutex<HashMap<String,Arc<Cached>>>>=OnceLock::new();
fn session_for(id:&str,scope:&str,session_key:&str,signature:&str)->Result<(Arc<Cached>,bool)>{
        let mut sessions=SESSIONS.get_or_init(Default::default).lock().map_err(|_|fail())?;
        let prefix=format!("{id}:");
        let retire=sessions.iter().any(|(key,s)|key.starts_with(&prefix)&&s.definition!=signature);
        sessions.retain(|key,s|{
            let replaced=key.starts_with(&prefix)&&s.definition!=signature;
            let old_device=scope.starts_with("device.")&&key.starts_with(&format!("{id}:device."))&&key!=session_key;
            if replaced||old_device{s.revoke();false}else{true}
        });
        if !sessions.contains_key(session_key)&&sessions.len()>=32{return Err("服务桥实例达到上限".into());}
        let session=sessions.entry(session_key.to_owned()).or_insert_with(||Arc::new(Cached{
            definition:signature.to_owned(),session:Mutex::new(Session::enter(1)),revoked:AtomicBool::new(false)
        })).clone();
        Ok((session,retire))
}
pub fn parse(d:&Declaration,r:&Value)->Result<Plan>{
    if d.schema_version!=1||d.engine!=ENGINE||d.service!="bridge"||d.auto_read||d.location.is_some()||d.content.len()!=1{return Err(fail());}
    let plan=Plan::parse(&d.content[0])?;plan.validate(r,&Catalog)?;
    // Notes capture is keyed by component ID, not by the provider's storage
    // directory. Two aliases would otherwise share a draft across libraries.
    if plan.services.values().filter(|v|matches!(source(v),Ok(Source::Service{declaration:d,..}) if d["service"]=="notes")).count()>1{return Err("同一服务桥最多一个笔记提供器".into());}
    if r["surface"]["services"]==surface::DEVICE_ENGINE&&r["host_contract"]!=contract::package(d,r)?{return Err("PC 服务与设备布局签名不一致，请重新绑定服务契约".into());}
    if plan.services.values().any(|v|matches!(source(v),Ok(Source::LocalMedia{})))&&r["surface"]["services"]!=super::surface::DEVICE_ENGINE{return Err("本地媒体需要带视图身份的服务桥能力".into());}
    if d.actions.len()!=plan.commands.len()||plan.commands.iter().any(|(a,c)|d.actions.get(a)!=Some(&format!("{}:{}",c.service,c.method))){return Err(fail());}
    // Multiple views may observe the same audio owner. Two independently
    // instantiated audio providers cannot silently contend under one identity.
    if plan.services.values().filter(|d|Catalog.permissions(d).is_ok_and(|p|p["deviceAudio"]==true)).count()>1{return Err("同一服务桥最多一个音频提供器".into());}
    Ok(plan)
}
pub fn permissions(d:&Declaration)->Result<Value>{
    let plan=Plan::parse(d.content.first().ok_or_else(fail)?)?;
    let mut out=serde_json::Map::new();let mut hosts=BTreeSet::new();
    for source in plan.services.values(){let p=Catalog.permissions(source)?;for(k,v)in p.as_object().ok_or_else(fail)?{
        if k=="networkHosts"{for h in v.as_array().ok_or_else(fail)?{hosts.insert(h.as_str().ok_or_else(fail)?.to_owned());}}
        else if let Some(value)=v.as_bool(){let prior=out.get(k).and_then(Value::as_bool).unwrap_or(false);out.insert(k.clone(),(prior||value).into());}
    }}out.insert("networkHosts".into(),json!(hosts));out.insert("serviceBridge".into(),true.into());Ok(Value::Object(out))
}
pub fn request(id:&str,dir:&Path,d:&Declaration,r:&Value,input:&Value,usb:&UsbSerialManager,notify:&dyn Fn(Value)->Result<()>)->Result<Value>{
    let plan=parse(d,r)?;let signature=digest(&(d,r))?;
    let scope=if let Some(generation)=input["_deviceEpoch"].as_u64(){
        if r["surface"]["services"]==surface::DEVICE_ENGINE&&input["contract"]!=r["host_contract"]{return Err("设备仍是另一版界面，请在组件中心保存并同步；此次操作未执行".into());}
        let target=view(id)?;if !crate::component_app::bind_bridge_view(id,generation,&target){return Err("设备视图已退出，服务请求未执行".into());}format!("device.{generation}")
    }else{let token=input["viewId"].as_str().unwrap_or("default");if !name(token,36){return Err(fail());}format!("desktop.{token}")};
    let session_key=format!("{id}:{scope}");
    if input["operation"]=="detach"{if let Some(m)=SESSIONS.get(){if let Ok(mut m)=m.lock(){if let Some(old)=m.remove(&session_key){old.revoke();}}}return Ok(json!({"detached":true}));}
    if input["operation"]=="asset"{let reference=ImageRef::parse(&input["reference"])?;return assets::preview(id,&reference);}
    if input["operation"]=="import"{
        if input.get("_deviceEpoch").is_some(){return Err("本地导入仅允许 PC 显式文件选择".into());}
        let alias=input["service"].as_str().filter(|s|plan.services.get(*s).is_some_and(|v|matches!(source(v),Ok(Source::LocalMedia{})))).ok_or_else(fail)?;
        let paths:Vec<String>=serde_json::from_value(input["paths"].clone()).map_err(|_|fail())?;
        let adapter=Adapter{id,dir,usb,notify,views:RefCell::new(BTreeMap::new()),revoked:None};adapter.player(alias,&plan.services[alias])?.import(paths)?;
    }
    let (session,retire)=session_for(id,&scope,&session_key,&signature)?;
    if retire{retire_resources(id);}
    let mut s=session.session.lock().map_err(|_|fail())?;
    let adapter=Adapter{id,dir,usb,notify,views:RefCell::new(BTreeMap::new()),revoked:Some(&session.revoked)};
    adapter.current()?;
    let mut value=match input["operation"].as_str(){Some("status"|"enable"|"import")=>s.snapshot(&plan,r,&adapter)?,Some("action")=>s.command(&plan,r,&adapter,input)?,_=>return Err(fail())};
    adapter.current()?; // Never publish the result of a retired in-flight call.
    value["permissions"]=permissions(d)?;
    value["commands"]=json!(plan.commands);
    value["serviceDescriptors"]=json!(plan.services.iter().map(|(alias,d)|Ok((alias,json!({"permissions":Catalog.permissions(d)?})))).collect::<Result<BTreeMap<_,_>>>()?);
    Ok(value)
}
fn retire_resources(id:&str){
    // Provider registries use the component ID. Retiring only the bridge
    // receipts leaves old notes recognition/news jobs alive after an update.
    crate::component_notes::cancel(Some(id));
    crate::component_news::disable(id);
    crate::component_audio::stop(Some(id));
    assets::clear(id);if let Ok(mut all)=players().lock(){let prefix=format!("{id}:");all.retain(|key,s|{if key.starts_with(&prefix){s.player.interrupt();false}else{true}});}
}
pub fn disable(id:&str){
    if let Some(m)=SESSIONS.get(){if let Ok(mut m)=m.lock(){let prefix=format!("{id}:");m.retain(|key,s|{if key.starts_with(&prefix){s.revoke();false}else{true}});}}
    retire_resources(id);if let Some(m)=VIEWS.get(){if let Ok(mut m)=m.lock(){if let Some(old)=m.remove(id){if let Ok(mut v)=old.lock(){v.clear();}}}}
}
pub fn detach_device(id:&str){
    if let Some(m)=VIEWS.get(){if let Ok(m)=m.lock(){if let Some(v)=m.get(id){if let Ok(mut v)=v.lock(){v.clear();}}}}
    // No session mutex or provider call on the ordered reader thread.
    // Revocation is atomic even while that session is inside a provider.
    if let Some(m)=SESSIONS.get(){if let Ok(mut m)=m.try_lock(){let prefix=format!("{id}:device.");m.retain(|key,s|{if key.starts_with(&prefix){s.revoke();false}else{true}});}}
}
/// No USB waits, allocations, retries or playback on the serial-reader thread.
pub fn interrupt_media(){if let Ok(all)=players().lock(){for p in all.values(){p.player.interrupt();}}}
/// An explicit desktop file-picker result. No declaration, device event, timer
/// or workflow may reach this operation or submit filesystem paths.
pub fn import_local(id:&str,alias:&str,paths:Vec<String>)->Result<Value>{
    let key=format!("{id}:{alias}");let player=players().lock().map_err(|_|fail())?.get(&key).map(|p|p.player.clone()).ok_or_else(fail)?;player.import(paths)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn actual_registered_catalog_accepts_shared_contract_and_keeps_legacy_player_closed(){
        let suite:Value=serde_json::from_str(include_str!("../../../skills/petui/contracts/service-bridge-v1.json")).unwrap();
        let d:Declaration=serde_json::from_value(suite["declaration"].clone()).unwrap();
        parse(&d,&suite["runtime"]).unwrap();
        assert_eq!(permissions(&d).unwrap()["localAudioImport"],true);
        assert!(!crate::music_player::AVAILABLE);
        let recursive=json!({"kind":"service","declaration":suite["declaration"],"runtime":suite["runtime"]});
        assert!(Catalog.validate(&recursive).is_err());
    }
    #[test]
    fn real_local_import_has_private_paths_no_autoplay_and_preserves_corrupt_libraries(){
        let root=tempfile::tempdir().unwrap();let audio=root.path().join("record.wav");
        std::fs::write(&audio,b"RIFF-test-file-no-decoder-is-started").unwrap();
        let library=root.path().join("library");let view=std::sync::Arc::new(Mutex::new(String::new()));
        let player=LocalPlayer::open("bridge-import-test",library.clone(),UsbSerialManager::new(),view.clone()).unwrap();
        let imported=player.import(vec![audio.to_string_lossy().into()]).unwrap();
        assert_eq!(imported["state"],"idle");assert_eq!(imported["queue"].as_array().unwrap().len(),1);
        assert!(!imported.to_string().contains(root.path().to_str().unwrap()));
        let key=imported["queue"][0]["key"].as_str().unwrap();
        assert!(player.command_observed("play",&json!({"key":key}),"").is_err());
        player.command_observed("volume",&json!({"volume":42}),"").unwrap();
        let next=LocalPlayer::open("bridge-import-test",library.clone(),UsbSerialManager::new(),view).unwrap();assert_eq!(next.snapshot()["volume"],42);
        let file=library.join("music-library.json");std::fs::write(&file,b"corrupt-library-must-survive").unwrap();
        assert!(LocalPlayer::open("bridge-import-test",library,UsbSerialManager::new(),std::sync::Arc::new(Mutex::new(String::new()))).is_err());
        assert_eq!(std::fs::read(file).unwrap(),b"corrupt-library-must-survive");
    }
    #[test]
    fn cancelled_storage_scope_does_not_publish_an_import_or_revive_old_view_commands(){
        use crate::component_runtime_lifecycle::{ResourceOwner,enter_owner};
        let root=tempfile::tempdir().unwrap();let audio=root.path().join("record.wav");std::fs::write(&audio,b"test").unwrap();
        let view=std::sync::Arc::new(Mutex::new("view-one".into()));
        let player=LocalPlayer::open("bridge-cancel-test",root.path().join("library"),UsbSerialManager::new(),view.clone()).unwrap();
        let owner=ResourceOwner::fresh();let _scope=enter_owner(&owner);owner.cancel();
        assert!(player.import(vec![audio.to_string_lossy().into()]).is_err());assert!(player.snapshot()["queue"].as_array().unwrap().is_empty());
        assert!(!root.path().join("library/music-library.json").exists());
        *view.lock().unwrap()="view-two".into();assert!(player.command_observed("mode",&json!({"mode":"single"}),"view-one").is_err());
    }
    #[test]
    fn production_registry_is_not_held_by_a_slow_session_and_retirement_never_waits_for_it(){
        use std::sync::mpsc;
        use std::time::Duration;
        let id="bridge-concurrency-regression";
        let (old,_)=session_for(id,"device.1",&format!("{id}:device.1"),"one").unwrap();
        let (entered_tx,entered_rx)=mpsc::channel();let (release_tx,release_rx)=mpsc::channel();
        let held=old.clone();
        let worker=std::thread::spawn(move||{let _guard=held.session.lock().unwrap();entered_tx.send(()).unwrap();release_rx.recv().unwrap();});
        entered_rx.recv_timeout(Duration::from_secs(2)).unwrap();
        let (done_tx,done_rx)=mpsc::channel();
        let observer=std::thread::spawn(move||{
            // Same production registry helper must progress while an old
            // session holds its mutex across a simulated provider operation.
            let (next,_)=session_for(id,"device.2",&format!("{id}:device.2"),"one").unwrap();
            done_tx.send(next.revoked.load(Ordering::Acquire)).unwrap();
        });
        let progressed=done_rx.recv_timeout(Duration::from_secs(2));
        release_tx.send(()).unwrap();worker.join().unwrap();observer.join().unwrap();
        assert_eq!(progressed.unwrap(),false);
        assert!(old.revoked.load(Ordering::Acquire));
        let mut registry=SESSIONS.get().unwrap().lock().unwrap();registry.retain(|key,_|!key.starts_with(id));
    }
    #[test]
    fn local_provider_mode_labels_describe_the_existing_playback_policy(){
        let root=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();
        let adapter=Adapter{id:"bridge-mode-label-test",dir:root.path(),usb:&usb,notify:&|_|Ok(()),views:RefCell::new(BTreeMap::new()),revoked:None};
        let declaration=json!({"kind":"local-media"});let player=adapter.player("music",&declaration).unwrap();
        for (mode,label) in [("sequence","顺序播放"),("single","单曲循环"),("loop","列表循环"),("shuffle","随机播放")]{
            player.command_observed("mode",&json!({"mode":mode}),"").unwrap();
            assert_eq!(adapter.snapshot("music",&declaration).unwrap().value["modeLabel"],label);
        }
        // No playback was started. Remove only this test's registry entries;
        // do not advance the global image-retirement epoch of parallel tests.
        players().lock().unwrap().remove("bridge-mode-label-test:music");
        VIEWS.get().unwrap().lock().unwrap().remove("bridge-mode-label-test");
    }
    #[test]
    fn local_provider_methods_are_native_registered_and_do_not_accept_spurious_arguments(){
        let methods=Catalog.methods(&json!({"kind":"local-media"})).unwrap();
        for name in ["volume_up","volume_down","cycle_mode"]{assert!(methods[name].arguments.is_empty());assert!(!methods[name].confirmation);}
        assert!(methods["remove"].confirmation);
    }

    #[test]
    fn authored_player_plan_uses_registered_methods_and_exact_device_contract(){
        let declaration:Declaration=serde_json::from_str(include_str!("../../../candidate-clawpkgs/local-music-player/pc/application.json")).unwrap();
        let runtime:Value=serde_json::from_str(include_str!("../../../candidate-clawpkgs/local-music-player/runtime/widget.json")).unwrap();
        let plan=parse(&declaration,&runtime).unwrap();
        assert_eq!(plan.services.len(),1);assert_eq!(plan.collections.len(),1);
        assert_eq!(runtime["surface"]["services"],surface::DEVICE_ENGINE);
        assert!(!runtime.to_string().contains("http://"));assert!(!runtime.to_string().contains("https://"));
        assert_eq!(runtime["host_contract"],contract::package(&declaration,&runtime).unwrap());
    }
    #[test]
    fn authored_player_reaches_the_real_complete_package_validator(){
        let path=Path::new(env!("CARGO_MANIFEST_DIR")).join("../candidate-clawpkgs/local-music-player.clawpkg");
        let result=crate::clawpkg::validate_clawpkg_at_path(&path).unwrap();
        assert!(result.ok,"{:?}",result.errors);
    }

    #[test]
    fn authored_player_worst_case_visible_projection_fits_the_real_device_envelope(){
        struct QueueProvider;
        impl Provider for QueueProvider {
            fn validate(&self,v:&Value)->Result<()>{Catalog.validate(v)}
            fn permissions(&self,v:&Value)->Result<Value>{Catalog.permissions(v)}
            fn methods(&self,v:&Value)->Result<BTreeMap<String,Method>>{Catalog.methods(v)}
            fn snapshot(&self,_:&str,v:&Value)->Result<Snapshot>{
                let text="汉".repeat(21);
                Ok(Snapshot{value:json!({"title":text,"artist":text,"lyric":text,"modeLabel":"顺序播放","stateLabel":"正在播放","volume":100,"progressPercent":50,"positionMs":60000,"durationMs":120000,"running":true,"cover":null,
                    "queue":(0..20).map(|i|json!({"key":format!("local:{i:036}"),"title":text,"artist":text})).collect::<Vec<_>>() }),
                    observation:"local-queue-observation".into(),phase:"running".into(),actions:Catalog.methods(v)?.keys().cloned().collect()})
            }
            fn command(&self,_:&str,_:&Value,_:&str,_:&Value,_:&str,_:&str,_:bool)->Result<()>{Err(fail())}
        }
        let declaration:Declaration=serde_json::from_str(include_str!("../../../candidate-clawpkgs/local-music-player/pc/application.json")).unwrap();
        let runtime:Value=serde_json::from_str(include_str!("../../../candidate-clawpkgs/local-music-player/runtime/widget.json")).unwrap();
        let plan=parse(&declaration,&runtime).unwrap();
        let value=Session::enter(1).snapshot(&plan,&runtime,&QueueProvider).unwrap();
        let envelope=json!({"componentId":"c".repeat(47),"instance":"0".repeat(31),"sequence":u32::MAX,"contract":runtime["host_contract"],"values":value["values"],"ui":value["ui"],"variables":value["variables"],"presentation":value["presentation"],"collectionView":value["collectionView"]});
        crate::app_host_collection::bounded_envelope(envelope).unwrap();
    }

}
