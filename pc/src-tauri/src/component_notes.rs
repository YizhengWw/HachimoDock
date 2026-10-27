//! Component-scoped voice drafts. No desktop injection; saving needs an explicit action.
use crate::{usb_serial::UsbSerialManager,volcengine_asr::{StreamingSpeechRecognizer,StreamingSpeechEvent}};
use serde::{Deserialize,Serialize};use serde_json::{json,Value};
use std::{path::Path,sync::{Arc,Mutex,OnceLock},time::{Duration,Instant},io::Write};
#[derive(Default)]struct CaptureData{phase:String,text:String,error:String,session:String,diagnostics:CaptureDiagnostics}
/// Counters only: never persist microphone samples, transcripts, credentials, or server payloads.
#[derive(Default,Serialize)]struct CaptureDiagnostics{ready:bool,chunks:u64,bytes:u64,peak:u32,energetic_samples:u64,ended:bool,partials:u64,final_received:bool,error_stage:String}
pub(crate) fn diagnostics(id:&str)->Value{current(id).map(|c|{let d=c.data.lock().unwrap();json!({"phase":d.phase,"audio":d.diagnostics})}).unwrap_or(Value::Null)}
fn error_stage(message:&str)->&'static str{
    if message.contains("超时"){"timeout"} else if message.contains("连接") {"connection"} else if message.contains("返回错误")||message.contains("拒绝请求"){"provider"} else if message.contains("发送"){"send"} else {"recognition"}
}
fn accepting_audio(phase:&str)->bool{matches!(phase,"starting"|"recording"|"recognizing")}
fn apply_speech_event(data:&mut CaptureData,event:StreamingSpeechEvent){
    // Late provider messages must never revive a failed recording or erase a final draft.
    if !accepting_audio(&data.phase){return;}
    match event{
        StreamingSpeechEvent::Ready=>data.diagnostics.ready=true,
        StreamingSpeechEvent::Partial{text,..}=>{data.diagnostics.partials+=1;data.text=crate::widget_data::bounded_text(&text,1800);},
        StreamingSpeechEvent::Final{text,..}=>{data.diagnostics.final_received=true;data.text=crate::widget_data::bounded_text(&text,1800);data.phase=if data.text.trim().is_empty(){"empty"}else{"draft"}.into();},
        StreamingSpeechEvent::Error(error)=>{data.diagnostics.error_stage=error_stage(&error).into();data.phase="error".into();data.error=match error_stage(&error){"timeout"=>"云端识别超时，请稍后重试；未保存笔记","connection"=>"无法完成云端识别连接，请检查网络；未保存笔记","provider"=>"语音服务拒绝请求，请检查 ASR 配置与额度；未保存笔记",_=>"云端识别未完成，请稍后重试；未保存笔记"}.into();}
    }
}
fn owns_audio(data:&CaptureData,token:&str,topic:&str,payload:&Value)->bool{
    if topic=="audio/begin"{return payload["captureOwner"]==token&&(data.session.is_empty()||payload["sessionId"]==data.session);}
    !data.session.is_empty()&&payload["sessionId"]==data.session
}
struct Capture{id:String,board:String,token:String,data:Mutex<CaptureData>,asr:Mutex<Option<StreamingSpeechRecognizer>>,relay:Mutex<crate::usb_audio::UsbAudioRelay>,usb:UsbSerialManager,started:Instant}
static ACTIVE:OnceLock<Mutex<Option<Arc<Capture>>>>=OnceLock::new();
fn active()->&'static Mutex<Option<Arc<Capture>>>{ACTIVE.get_or_init(Default::default)}
fn current(id:&str)->Option<Arc<Capture>>{active().lock().ok()?.as_ref().filter(|c|c.id==id).cloned()}
fn control(c:&Capture,action:&str)->Result<(),String>{c.usb.send_to_board(&c.board,"audio/control",&json!({"action":action,"captureOwner":c.token,"requestId":format!("notes-{}-{action}",c.token)}))}
fn capture_error(c:&Capture,message:&str){{let mut d=c.data.lock().unwrap();if !accepting_audio(&d.phase){return;}d.phase="error".into();d.error=message.into();}if let Some(a)=c.asr.lock().unwrap().as_ref(){a.cancel();}let _=control(c,"stop");}
pub fn cancel(id:Option<&str>){
    let c={let mut a=active().lock().unwrap();if a.as_ref().is_some_and(|c|id.is_none_or(|id|id==c.id)){a.take()}else{None}};
    if let Some(c)=c{if let Some(a)=c.asr.lock().unwrap().as_ref(){a.cancel();}if matches!(c.data.lock().unwrap().phase.as_str(),"recording"|"starting"|"recognizing"){let _=control(&c,"stop");}}
}
fn start(id:&str,usb:&UsbSerialManager)->Result<(),String>{
    let board=usb.status();if !board.connected{return Err("请先连接设备".into());}
    if crate::realtime_chat::status().active{return Err("请先结束实时对话，再录制笔记".into());}
    if crate::active_device_voice_context().lock().map_err(|_|"语音正在使用")?.is_some(){return Err("按键语音输入尚未结束，请先完成或取消".into());}
    if active().lock().unwrap().as_ref().is_some_and(|c|matches!(c.data.lock().unwrap().phase.as_str(),"starting"|"recording"|"recognizing")){return Err("已有录音正在处理".into());}
    if current(id).is_some_and(|c|c.data.lock().unwrap().phase=="draft"){return Err("请先保存或转为待办，避免覆盖未保存的转写".into());}
    if !crate::volcengine_asr::settings_status()?.configured{return Err("请先配置语音识别服务；未启动录音".into());}
    cancel(None);crate::component_audio::stop(None);crate::component_media::stop_all();let _=crate::music_player::execute(json!({"operation":"stop"}));
    let mut relay=crate::usb_audio::UsbAudioRelay::default();relay.configure(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false);
    let c=Arc::new(Capture{id:id.into(),board:board.board_device_id,token:uuid::Uuid::new_v4().to_string(),data:Mutex::new(CaptureData{phase:"starting".into(),..Default::default()}),asr:Mutex::new(None),relay:Mutex::new(relay),usb:usb.clone(),started:Instant::now()});
    *active().lock().unwrap()=Some(c.clone());let callback=c.clone();
    let asr=StreamingSpeechRecognizer::start(move|event|{
        if !current(&callback.id).is_some_and(|c|c.token==callback.token){return;}
        let mut data=callback.data.lock().unwrap();
        apply_speech_event(&mut data,event);
    }).map_err(|_|{cancel(Some(id));"云端语音识别未配置或不可用"})?;
    *c.asr.lock().unwrap()=Some(asr);
    if c.data.lock().unwrap().phase=="error"{cancel(Some(id));return Err("语音识别服务未就绪".into());}
    if control(&c,"start").is_err(){cancel(Some(id));return Err("设备录音未启动".into());}
    let timer=c.clone();std::thread::spawn(move||{
        for _ in 0..180{
            std::thread::sleep(Duration::from_millis(250));
            if !current(&timer.id).is_some_and(|c|c.token==timer.token){return;}
            let phase=timer.data.lock().unwrap().phase.clone();
            if matches!(phase.as_str(),"draft"|"error"|"empty"){let _=control(&timer,"stop");return;}
            if !timer.usb.status().connected{capture_error(&timer,"设备已断开，未保存录音");return;}
            if phase=="starting"&&timer.started.elapsed()>Duration::from_secs(5){capture_error(&timer,"设备麦克风未开始录音，请检查设备状态后重试");return;}
            if timer.started.elapsed()>Duration::from_secs(28)&&matches!(phase.as_str(),"recording"|"starting"){
                timer.data.lock().unwrap().phase="recognizing".into();let _=control(&timer,"stop");
            }
        }
        capture_error(&timer,"识别超时，未保存笔记");
    });Ok(())
}
/// Own the complete audio session before the generic PTT route sees it.
pub fn route(topic:&str,payload:&Value)->bool{
    let Some(c)=active().lock().ok().and_then(|a|a.clone())else{return false;};
    if topic=="protocol/ack"&&payload["requestTopic"]=="audio/control"{
        if payload["requestId"]==format!("notes-{}-start",c.token){if payload["ok"]==false{capture_error(&c,"设备未接受录音，请结束其他语音后重试");}return true;}
        if payload["requestId"]==format!("notes-{}-stop",c.token){return true;}
        return false;
    }
    if payload["boardDeviceId"]!=c.board||!topic.starts_with("audio/"){return false;}
    if !matches!(topic,"audio/begin"|"audio/chunk"|"audio/end"|"audio/error"){return false;}
    {let mut d=c.data.lock().unwrap();if !owns_audio(&d,&c.token,topic,payload){return false;}
    if topic=="audio/begin"&&d.session.is_empty(){d.session=payload["sessionId"].as_str().unwrap_or("").into();}
    // Drain our late packets instead of leaking them into generic voice-input routing.
    if !accepting_audio(&d.phase){return true;}}
    let mut relay=c.relay.lock().unwrap();let event=relay.handle(topic,payload);
    if event.as_ref().is_some_and(|v|v["ok"]==false){drop(relay);capture_error(&c,"设备录音传输中断，未保存笔记");return true;}
    if topic=="audio/begin"{let mut d=c.data.lock().unwrap();d.session=payload["sessionId"].as_str().unwrap_or("").into();if d.phase=="starting"{d.phase="recording".into();}}
    if let Some(chunk)=relay.take_validated_chunk(){
        {let mut d=c.data.lock().unwrap();d.diagnostics.chunks+=1;d.diagnostics.bytes+=chunk.pcm.len() as u64;for sample in chunk.pcm.chunks_exact(2){let level=i16::from_le_bytes([sample[0],sample[1]]).unsigned_abs() as u32;d.diagnostics.peak=d.diagnostics.peak.max(level);if level>512{d.diagnostics.energetic_samples+=1;}}}
        let failed=c.asr.lock().unwrap().as_ref().is_none_or(|asr|asr.push_pcm(&chunk.pcm).is_err());if failed{drop(relay);capture_error(&c,"语音识别连接已结束");return true;}}
    if relay.take_completed().is_some(){{let mut d=c.data.lock().unwrap();d.diagnostics.ended=true;if !accepting_audio(&d.phase){return true;}d.phase="recognizing".into();}let failed=c.asr.lock().unwrap().as_ref().is_none_or(|asr|asr.finish().is_err());if failed{drop(relay);capture_error(&c,"语音识别连接已结束，未保存笔记");return true;}}
    true
}
#[derive(Clone,Serialize,Deserialize)]struct Note{id:String,text:String,created:String,todo:Option<String>}
#[derive(Default,Serialize,Deserialize)]struct Notes{items:Vec<Note>,selected:usize}
fn read(dir:&Path)->Result<Notes,String>{match std::fs::read(dir.join("notes.json")){Ok(b)if b.len()<=1048576=>{let mut n:Notes=serde_json::from_slice(&b).map_err(|_|"笔记文件损坏，未覆盖")?;let mut ids=std::collections::HashSet::new();if n.items.len()>300||n.items.iter().any(|n|n.id.is_empty()||n.id.len()>64||!ids.insert(&n.id)||n.text.len()>1800||n.text.trim().is_empty()||chrono::DateTime::parse_from_rfc3339(&n.created).is_err()){return Err("笔记文件超出限制或存在无效记录，未覆盖".into());}n.selected%=n.items.len().max(1);Ok(n)},Ok(_)=>Err("笔记文件超出限制".into()),Err(e)if e.kind()==std::io::ErrorKind::NotFound=>Ok(Notes::default()),Err(_)=>Err("无法读取笔记".into())}}
fn save(dir:&Path,notes:&Notes)->Result<(),String>{std::fs::create_dir_all(dir).map_err(|_|"无法创建笔记目录")?;let mut f=tempfile::NamedTempFile::new_in(dir).map_err(|_|"无法准备笔记存储")?;f.write_all(&serde_json::to_vec(notes).map_err(|_|"笔记编码失败")?).and_then(|_|f.as_file().sync_all()).map_err(|_|"笔记写入失败")?;f.persist(dir.join("notes.json")).map_err(|_|"笔记保存失败")?;Ok(())}
fn todo_title(text:&str)->String{
    let normalized=text.split_whitespace().collect::<Vec<_>>().join(" ");
    if normalized.chars().count()<=120{normalized}else{format!("{}…",normalized.chars().take(119).collect::<String>())}
}
/// The token binds confirmation to the exact displayed draft/history record.
/// It is not a credential and never contains a transcript or a filesystem path.
pub(crate) fn controls(id:&str,dir:&Path)->Result<(u8,Vec<&'static str>,String),String>{
    use sha2::{Digest,Sha256};
    let n=read(dir)?;
    if let Some(c)=current(id){
        let d=c.data.lock().unwrap();
        return Ok(match d.phase.as_str(){
            "starting"|"recognizing"=>(6,vec![],String::new()),
            "recording"=>(1,vec!["record"],String::new()),
            "draft"=>(4,vec!["save","todo"],format!("{:x}",Sha256::digest(format!("{}:{}",c.token,d.text).as_bytes()))),
            _=>(7,vec!["record"],String::new()),
        });
    }
    if let Some(note)=n.items.get(n.selected){
        let mut actions=vec!["record","delete"];
        if note.todo.is_none(){actions.push("todo");}
        if n.items.len()>1{actions.extend(["previous","next"]);}
        Ok((5,actions,format!("{:x}",Sha256::digest(serde_json::to_vec(note).map_err(|_|"无法验证笔记版本")?))))
    }else{Ok((0,vec!["record"],String::new()))}
}
pub fn execute(id:&str,op:&str,input:&Value,dir:&Path,usb:&UsbSerialManager)->Result<Value,String>{
    let mut n=read(dir)?;
    let contextual=matches!(op,"delete"|"todo")&&(input.get("context").is_some()||input.get("_deviceEpoch").is_some());
    if contextual{
        let (_,allowed,token)=controls(id,dir)?;
        if input["confirm"]!=true||token.is_empty()||input["context"]!=token||!allowed.contains(&op){
            return Err("笔记内容或选择已变化，请重新查看并确认；未执行操作".into());
        }
    }
    if op=="record"{if let Some(c)=current(id).filter(|c|matches!(c.data.lock().unwrap().phase.as_str(),"starting"|"recording")){c.data.lock().unwrap().phase="recognizing".into();control(&c,"stop")?;}else{start(id,usb)?;}}
    let draft=current(id).and_then(|c|{let d=c.data.lock().ok()?;(d.phase=="draft").then(||Note{id:c.token.clone(),text:d.text.clone(),created:chrono::Utc::now().to_rfc3339(),todo:None})});
    match op{
        "save"|"todo"=>{
            if op=="save"&&draft.is_none(){return Err("没有待确认的最终转写，请先录制".into());}
            if let Some(d)=draft.as_ref(){if !n.items.iter().any(|v|v.id==d.id){if n.items.len()>=300{return Err("最多保存 300 条笔记，请先清理".into());}n.items.insert(0,d.clone());n.selected=0;}}
            if n.items.is_empty(){return Err("没有可保存的最终转写，请先录制".into());}
            let index=n.selected%n.items.len();
            if op=="todo"&&n.items[index].todo.is_none(){
                // Keep the complete note; only the todo's title is an excerpt.
                let title=todo_title(&n.items[index].text);
                n.items[index].todo=Some(crate::todos::create_component_note_once(&format!("{id}:{}",n.items[index].id),&title)?);
            }save(dir,&n)?;if draft.is_some(){cancel(Some(id));}
        },
        "previous"|"next"=>{if draft.is_some(){return Err("请先保存当前转写，再浏览笔记".into());}if !n.items.is_empty(){n.selected=(n.selected%n.items.len()+n.items.len()+if op=="previous"{n.items.len()-1}else{1})%n.items.len();save(dir,&n)?;}},
        "edit"=>{
            let target=input["noteId"].as_str().ok_or("请选择笔记")?;
            let text=input["text"].as_str().ok_or("请输入笔记内容")?.trim();
            if text.is_empty()||text.len()>1800||text.chars().any(|c|c.is_control()&&!matches!(c,'\n'|'\t')){return Err("笔记需为 1–1800 字节有效文字".into());}
            let note=n.items.iter_mut().find(|v|v.id==target).ok_or("笔记不存在，请刷新列表")?;
            note.text=text.into();save(dir,&n)?;
        },
        "delete"=>{
            if input["confirm"]!=true{return Ok(json!({"headline":"删除需要确认","metricValue":"","note":"请在 PC 笔记列表确认删除；不会删除关联待办"}));}
            let target=if contextual{n.items.get(n.selected).map(|v|v.id.clone()).ok_or("请选择笔记")?}else{input["noteId"].as_str().ok_or("请选择笔记")?.to_string()};n.items.retain(|v|v.id!=target);n.selected=0;save(dir,&n)?;
        },_=>{}
    }
    let mut result=json!({"eyebrow":"笔记仅保存在这台电脑","headline":"接住一个想法","metricValue":"","metricLabel":"开始录制","metricUnit":"保存笔记","note":"录音转写后，由你确认保存或转为待办"});
    if let Some(c)=current(id){let data=c.data.lock().unwrap();result["note"]=if data.error.is_empty(){data.text.clone()}else{data.error.clone()}.into();result["headline"]=match data.phase.as_str(){"starting"=>"正在准备麦克风","recording"=>"正在录音","recognizing"=>"正在识别","draft"=>if n.items.iter().any(|v|v.id==c.token){"笔记已保存"}else{"转写完成 · 待确认"},"empty"=>"没有识别到内容",_=>"录音未完成"}.into();if matches!(data.phase.as_str(),"starting"|"recording"){result["metricLabel"]="结束录音".into();}}
    else if let Some(note)=n.items.get(n.selected%n.items.len().max(1)){result["headline"]="我的灵感胶囊".into();result["note"]=note.text.clone().into();result["eyebrow"]=format!("{}/{} · {}",n.selected+1,n.items.len(),if note.todo.is_some(){"已转待办"}else{"已保存"}).into();}
    Ok(result)
}
pub fn list(dir:&Path)->Result<Value,String>{Ok(json!(read(dir)?.items))}
pub fn stop_recording(id:&str){if let Some(c)=current(id){let mut d=c.data.lock().unwrap();if matches!(d.phase.as_str(),"starting"|"recording"){d.phase="recognizing".into();drop(d);let _=control(&c,"stop");}}}
pub fn stop_recording_all(){let c=active().lock().ok().and_then(|v|v.clone());if let Some(c)=c{stop_recording(&c.id);if let Some(asr)=c.asr.lock().unwrap().as_ref(){let _=asr.finish();}}}

#[cfg(test)]mod tests{
    use super::*;
    #[test]fn long_note_title_is_utf8_safe_and_explicitly_abbreviated(){
        let full="想法".repeat(200);let title=todo_title(&full);
        assert_eq!(title.chars().count(),120);assert!(title.ends_with('…'));assert_eq!(full.chars().count(),400);
        assert_eq!(todo_title("明天\n买菜"),"明天 买菜");
    }
    #[test]fn final_draft_and_failed_capture_cannot_be_overwritten_by_late_callbacks(){
        let mut d=CaptureData{phase:"recognizing".into(),..Default::default()};
        apply_speech_event(&mut d,StreamingSpeechEvent::Final{revision:1,text:"测试草稿".into(),confidence:None});
        for event in [StreamingSpeechEvent::Error("连接关闭".into()),StreamingSpeechEvent::Partial{revision:2,text:"旧数据".into(),confidence:None},StreamingSpeechEvent::Final{revision:3,text:String::new(),confidence:None}]{apply_speech_event(&mut d,event);}
        assert_eq!(d.phase,"draft");assert_eq!(d.text,"测试草稿");assert!(d.error.is_empty());
        d.phase="error".into();apply_speech_event(&mut d,StreamingSpeechEvent::Final{revision:4,text:"迟到结果".into(),confidence:None});assert_eq!(d.phase,"error");
    }
    #[test]fn capture_ownership_survives_terminal_state_and_rejects_unbound_packets(){
        let mut d=CaptureData{phase:"starting".into(),..Default::default()};
        assert!(!owns_audio(&d,"owner","audio/chunk",&json!({"sessionId":"other"})));
        assert!(!owns_audio(&d,"owner","audio/begin",&json!({"captureOwner":"other","sessionId":"s"})));
        assert!(owns_audio(&d,"owner","audio/begin",&json!({"captureOwner":"owner","sessionId":"s"})));
        d.session="s".into();d.phase="draft".into();
        assert!(owns_audio(&d,"owner","audio/end",&json!({"sessionId":"s"})));
        assert!(!owns_audio(&d,"owner","audio/end",&json!({"sessionId":"other"})));
        assert!(!accepting_audio(&d.phase));
    }
    #[test]fn diagnostic_error_classification_does_not_expose_provider_payload(){
        let mut d=CaptureData{phase:"recording".into(),..Default::default()};
        apply_speech_event(&mut d,StreamingSpeechEvent::Error("火山引擎 ASR 拒绝请求 secret-value".into()));
        assert_eq!(d.diagnostics.error_stage,"provider");assert!(!d.error.contains("secret-value"));
        assert_eq!(error_stage("连接火山引擎 ASR 超时"),"timeout");
    }
    #[test]fn notes_edit_delete_and_bad_records_preserve_data(){
        let dir=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();
        let note=Note{id:"test-note".into(),text:"原笔记".into(),created:"2026-10-20T10:00:00Z".into(),todo:Some("unmodified-todo".into())};
        save(dir.path(),&Notes{items:vec![note.clone()],selected:usize::MAX}).unwrap();
        assert_eq!(read(dir.path()).unwrap().selected,0);
        execute("test-notes","edit",&json!({"noteId":"test-note","text":"修正后的笔记"}),dir.path(),&usb).unwrap();
        let n=read(dir.path()).unwrap();assert_eq!(n.items[0].text,"修正后的笔记");assert_eq!(n.items[0].todo.as_deref(),Some("unmodified-todo"));
        assert!(execute("test-notes","edit",&json!({"noteId":"test-note","text":""}),dir.path(),&usb).is_err());
        execute("test-notes","delete",&json!({"noteId":"test-note"}),dir.path(),&usb).unwrap();assert_eq!(read(dir.path()).unwrap().items.len(),1);
        execute("test-notes","delete",&json!({"noteId":"test-note","confirm":true}),dir.path(),&usb).unwrap();assert!(read(dir.path()).unwrap().items.is_empty());
        save(dir.path(),&Notes{items:vec![note.clone(),note],selected:0}).unwrap();let before=std::fs::read(dir.path().join("notes.json")).unwrap();assert!(read(dir.path()).is_err());assert_eq!(std::fs::read(dir.path().join("notes.json")).unwrap(),before);
    }
    #[test]fn device_confirmation_is_bound_to_displayed_note_and_cannot_delete_changed_content(){
        let dir=tempfile::tempdir().unwrap();let usb=UsbSerialManager::new();
        let note=Note{id:"device-note".into(),text:"屏幕上的笔记".into(),created:"2026-10-20T10:00:00Z".into(),todo:None};
        save(dir.path(),&Notes{items:vec![note.clone()],selected:0}).unwrap();
        let (mode,actions,token)=controls("device-note-test",dir.path()).unwrap();
        assert_eq!(mode,5);assert!(actions.contains(&"delete"));assert!(!actions.contains(&"save"));
        assert!(execute("device-note-test","delete",&json!({"_deviceEpoch":1,"context":token}),dir.path(),&usb).is_err());
        execute("device-note-test","edit",&json!({"noteId":note.id,"text":"已经修改"}),dir.path(),&usb).unwrap();
        assert!(execute("device-note-test","delete",&json!({"_deviceEpoch":1,"confirm":true,"context":token}),dir.path(),&usb).is_err());
        let (_,_,fresh)=controls("device-note-test",dir.path()).unwrap();
        execute("device-note-test","delete",&json!({"_deviceEpoch":1,"confirm":true,"context":fresh}),dir.path(),&usb).unwrap();
        assert!(read(dir.path()).unwrap().items.is_empty());
        assert!(execute("device-note-test","delete",&json!({"_deviceEpoch":1,"confirm":true,"context":fresh}),dir.path(),&usb).is_err());
    }
}
