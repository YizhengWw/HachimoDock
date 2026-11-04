//! Component-scoped voice drafts. No desktop injection; saving needs an explicit action.
use crate::{usb_serial::UsbSerialManager,volcengine_asr::{StreamingSpeechRecognizer,StreamingSpeechEvent}};
use serde::{Deserialize,Serialize};use serde_json::{json,Value};
use std::{path::Path,sync::{Arc,Mutex,OnceLock},time::{Duration,Instant},io::Write};
mod control;
const MAX_CAPTURE_PCM_BYTES:u64=16_000*2*30;
#[derive(Default)]struct CaptureData{phase:String,text:String,error:String,session:String,diagnostics:CaptureDiagnostics}
/// Counters only: never persist microphone samples, transcripts, credentials, or server payloads.
#[derive(Default,Serialize)]struct CaptureDiagnostics{ready:bool,chunks:u64,bytes:u64,peak:u32,energetic_samples:u64,ended:bool,partials:u64,final_received:bool,stop_failed:bool,error_stage:String,error_cause:String,provider_code:Option<u32>}
pub(crate) fn diagnostics(id:&str)->Value{current(id).map(|c|{let d=c.data.lock().unwrap();json!({"phase":d.phase,"audio":d.diagnostics})}).unwrap_or(Value::Null)}
fn error_stage(message:&str)->&'static str{
    if provider_code(message)==Some(45000081){"provider_audio_timeout"}
    else if message.contains("PCM")&&(message.contains("本地")||message.contains("真实麦克风")){
        if message.contains("超时"){"input_timeout"}else if message.contains("未收到足够"){"input_empty"}else{"input"}
    }else if message.contains("超时"){"timeout"}
    else if message.contains("连接"){"connection"}
    else if provider_code(message).is_some()||message.contains("返回错误")||message.contains("拒绝请求"){"provider"}
    else if message.contains("发送"){"send"}else{"recognition"}
}
fn provider_code(message:&str)->Option<u32>{
    let suffix=message.split_once("code=")?.1;
    let digits:String=suffix.chars().take_while(char::is_ascii_digit).take(11).collect();
    if digits.is_empty()||digits.len()>10{return None;}digits.parse().ok()
}
fn error_cause(message:&str,stage:&str)->&'static str{
    match stage{
        "provider_audio_timeout"=>"provider_audio_timeout",
        "input_timeout" if message.contains("输入中断")=>"pcm_gap_timeout",
        "input_timeout" if message.contains("未建立云端")=>"first_pcm_timeout",
        "input_timeout"=>"pcm_input_timeout","input_empty"=>"no_real_pcm","input"=>"pcm_input_error",
        "timeout" if message.contains("连接")=>"connection_timeout",
        "timeout" if message.contains("写入")=>"write_timeout",
        "timeout" if message.contains("最终")=>"final_response_timeout",
        "timeout"=>"unspecified_timeout","connection"=>"connection_error",
        "provider"=>"provider_error","send"=>"send_error",_=>"recognition_error"
    }
}
fn accepting_audio(phase:&str)->bool{matches!(phase,"starting"|"recording"|"recognizing")}
fn apply_speech_event(data:&mut CaptureData,event:StreamingSpeechEvent){
    // Late provider messages must never revive a failed recording or erase a final draft.
    if !accepting_audio(&data.phase){return;}
    match event{
        StreamingSpeechEvent::Ready=>data.diagnostics.ready=true,
        StreamingSpeechEvent::Partial{text,..}=>{data.diagnostics.partials+=1;data.text=crate::widget_data::bounded_text(&text,1800);},
        StreamingSpeechEvent::Final{text,..}=>{data.diagnostics.final_received=true;data.text=crate::widget_data::bounded_text(&text,1800);data.phase=if data.text.trim().is_empty(){"empty"}else{"draft"}.into();},
        StreamingSpeechEvent::Error(error)=>{
            let stage=error_stage(&error);
            data.diagnostics.error_stage=stage.into();
            data.diagnostics.error_cause=error_cause(&error,stage).into();
            data.diagnostics.provider_code=provider_code(&error);
            data.phase="error".into();
            data.error=match stage{
                "input_timeout"=>"本地麦克风 PCM 等待或输入中断超时，本段已停止；未保存笔记",
                "input_empty"=>"未收到足够的真实麦克风 PCM，未建立云端识别会话；未保存笔记",
                "input"=>"本地麦克风 PCM 输入未完成，本段已停止；未保存笔记",
                "provider_audio_timeout"=>"语音识别服务等待音频超时（45000081），本段已结束；未保存笔记",
                "timeout"=>"语音识别等待超时，本段已结束；未保存笔记",
                "connection"=>"语音识别连接未完成，本段已结束；未保存笔记",
                "provider"=>"语音识别服务返回错误，本段已结束；未保存笔记",
                "send"=>"语音识别发送未完成，本段已停止；未保存笔记",
                _=>"语音识别未完成，本段已结束；未保存笔记"
            }.into();
        }
    }
}
fn owns_audio(data:&CaptureData,token:&str,topic:&str,payload:&Value)->bool{
    if topic=="audio/begin"{return payload["captureOwner"]==token&&(data.session.is_empty()||payload["sessionId"]==data.session);}
    !data.session.is_empty()&&payload["sessionId"]==data.session
}
struct Capture{generation:u64,control:control::Ticket,owner:crate::component_app::MediaAuthority,id:String,board:String,connection:String,boot:String,token:String,data:Mutex<CaptureData>,asr:Mutex<Option<StreamingSpeechRecognizer>>,relay:Mutex<crate::usb_audio::UsbAudioRelay>,usb:UsbSerialManager,started:Instant}
static ACTIVE:OnceLock<Mutex<Option<Arc<Capture>>>>=OnceLock::new();
fn active()->&'static Mutex<Option<Arc<Capture>>>{ACTIVE.get_or_init(Default::default)}
fn current(id:&str)->Option<Arc<Capture>>{active().lock().ok()?.as_ref().filter(|c|c.id==id).cloned()}
static CONTROL:OnceLock<control::Slot<Capture>>=OnceLock::new();
fn control_slot()->&'static control::Slot<Capture>{CONTROL.get_or_init(Default::default)}
fn wake_control(){crate::program_autostart::native::notes_control();}
fn send_capture_control(c:&Capture,action:&str)->Result<(),String>{
    let status=c.usb.status();
    if !status.connected||status.board_device_id!=c.board||status.connection_id!=c.connection||status.boot_id!=c.boot{
        return Err("录音连接身份已变化，未向其他连接发送控制".into());
    }
    c.usb.send_owned_notes_control(&c.board,c.generation,&c.token,action)
}
fn control(c:&Capture,action:&str)->Result<(),String>{
    if action=="stop"{c.control.request();wake_control();return Ok(());}
    send_capture_control(c,action)
}
/// Only the existing serial control worker calls this. Capture stays pinned in
/// its reserved slot even after ACTIVE is removed. No slot/state lock spans TX.
pub(crate) fn drain_control(){
    if let Some(c)=control_slot().snapshot(){
        let result=process_control(&c,&|action|send_capture_control(&c,action));
        if result==Some(true){control_slot().release(&c);}
    }
}
fn process_control(c:&Arc<Capture>,send:&dyn Fn(&str)->Result<(),String>)->Option<bool>{
    match c.control.claim(){
        None=>None,
        Some(false)=>Some(true),
        Some(true)=>match send("stop"){
            Ok(())=>Some(true),
            Err(_)=>{c.control.fail();let mut d=c.data.lock().unwrap();
                d.diagnostics.stop_failed=true;
                if d.error.is_empty(){d.error="录音停止发送失败，未自动重试；新的录音已安全阻止，请检查连接后重启应用".into();}
                if d.diagnostics.error_stage.is_empty(){d.diagnostics.error_stage="control".into();}Some(false)
            }
        }
    }
}
fn capture_error(c:&Capture,message:&str){
    request_stop(c,false);
    {let mut d=c.data.lock().unwrap();if !accepting_audio(&d.phase){return;}d.phase="error".into();d.error=message.into();}
    if let Some(a)=c.asr.lock().unwrap().as_ref(){a.cancel();}
}
/// Returned input errors and delayed worker callbacks share one first-terminal
/// transition. Keep the concrete error only for the existing sanitized classifier.
fn handle_speech_event(c:&Capture,event:StreamingSpeechEvent)->bool{
    let cancel_asr=matches!(&event,StreamingSpeechEvent::Error(_));
    let (terminal,revoked)={
        // Match cancel's ACTIVE -> data order. A removed/replaced capture cannot
        // publish a late terminal, even before cancel has updated its phase.
        let slot=active().lock().unwrap();
        if !slot.as_ref().is_some_and(|v|std::ptr::eq(v.as_ref(),c)){return false;}
        let mut data=c.data.lock().unwrap();
        if !accepting_audio(&data.phase){return false;}
        let revoked=!c.owner.is_current();
        if revoked{data.phase="cancelled".into();}else{apply_speech_event(&mut data,event);}
        (!accepting_audio(&data.phase),revoked)
    };
    // Release state locks before stop/cancel; stop only queues the existing intent.
    if terminal{request_stop(c,false);}
    if revoked||cancel_asr{if let Some(asr)=c.asr.lock().unwrap().as_ref(){asr.cancel();}}
    terminal
}
fn request_stop(c:&Capture,cancelled:bool){
    {let mut d=c.data.lock().unwrap();
        if cancelled{if accepting_audio(&d.phase){d.phase="cancelled".into();}}else if matches!(d.phase.as_str(),"starting"|"recording"){d.phase="recognizing".into();}
    }
    c.control.request();wake_control();
}
pub fn cancel(id:Option<&str>){
    let c={let mut a=active().lock().unwrap();if a.as_ref().is_some_and(|c|id.is_none_or(|id|id==c.id)){a.take()}else{None}};
    if let Some(c)=c{request_stop(&c,true);if let Some(a)=c.asr.lock().unwrap().as_ref(){a.cancel();}}
}
fn capture_admission(phase:Option<&str>)->Result<(),String>{
    match phase{
        Some("draft")=>Err("请先保存或转为待办，避免覆盖未保存的转写".into()),
        Some("starting"|"recording"|"recognizing")=>Err("已有录音正在处理".into()),
        _=>Ok(())
    }
}
fn admit_start(c:&Capture)->bool{
    let d=c.data.lock().unwrap();
    c.owner.is_current()&&d.phase=="starting"&&c.control.admit_start()
}
fn finish_start(c:&Capture)->bool{
    c.control.start_returned();
    c.control.requested()||!c.owner.is_current()
}
fn prepare_and_start(c:&Arc<Capture>,factory:impl FnOnce()->Result<StreamingSpeechRecognizer,String>,send:&dyn Fn(&str)->Result<(),String>)->Result<(),String>{
    {let slot=active().lock().unwrap();let d=c.data.lock().unwrap();
        if !slot.as_ref().is_some_and(|v|v.token==c.token)||!c.owner.is_current()||d.phase!="starting"||c.control.requested(){c.control.prepared();c.control.request();wake_control();return Err("录音请求已撤销，未准备识别会话".into());}
    }
    let asr=match factory(){Ok(asr)=>asr,Err(error)=>{
        {let mut slot=active().lock().unwrap();if slot.as_ref().is_some_and(|v|v.token==c.token){slot.take();}}
        c.control.prepared();request_stop(c,true);return Err(error);
    }};
    c.control.prepared();
    *c.asr.lock().unwrap()=Some(asr);
    let admitted={let slot=active().lock().unwrap();
        slot.as_ref().is_some_and(|v|v.token==c.token)&&admit_start(c)
    };
    if !admitted{request_stop(c,false);{let mut d=c.data.lock().unwrap();if accepting_audio(&d.phase){d.phase="cancelled".into();}}if let Some(asr)=c.asr.lock().unwrap().as_ref(){asr.cancel();}return Err("录音请求已撤销，未启动麦克风".into());}
    // Once admitted, a physical send can finish after revocation. Arrange its
    // exact-owner stop after that send, with no reader/state lock across USB.
    let started=send("start");
    let stop=finish_start(c);
    if stop||started.is_err(){request_stop(c,false);}
    if started.is_err()||stop{{let mut d=c.data.lock().unwrap();if accepting_audio(&d.phase){d.phase="cancelled".into();}}if let Some(asr)=c.asr.lock().unwrap().as_ref(){asr.cancel();}return Err("录音请求已撤销或设备录音未启动".into());}
    Ok(())
}
fn start(id:&str,usb:&UsbSerialManager)->Result<(),String>{
    let owner=crate::component_app::media_authority(id)?;
    let board=usb.status();if !board.connected{return Err("请先连接设备".into());}
    if crate::realtime_chat::status().active{return Err("请先结束实时对话，再录制笔记".into());}
    if crate::active_device_voice_context().lock().map_err(|_|"语音正在使用")?.is_some(){return Err("按键语音输入尚未结束，请先完成或取消".into());}
    {let a=active().lock().unwrap();capture_admission(a.as_ref().map(|c|c.data.lock().unwrap().phase.clone()).as_deref())?;}
    if !owner.is_current(){return Err("组件已退出，未启动录音".into());}
    if !crate::program_autostart::native::notes_worker_ready(){return Err("录音控制工作者不可用，未启动录音".into());}
    let (connection,generation)=usb.autostart_connection().filter(|(status,_)|status.board_device_id==board.board_device_id).ok_or("设备连接已变化，未启动录音")?;
    let token=uuid::Uuid::new_v4().to_string();
    let mut relay=crate::usb_audio::UsbAudioRelay::default();
    let configured=relay.configure_owned_scoped(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false,&connection.board_device_id,&connection.connection_id,&connection.boot_id,&token);
    if connection.runtime!="esp-p4"||configured["ok"]!=true{return Err("设备录音连接身份尚未准备完成；未启动录音".into());}
    let c=Arc::new(Capture{generation,control:Default::default(),owner,id:id.into(),board:connection.board_device_id,connection:connection.connection_id,boot:connection.boot_id,token,data:Mutex::new(CaptureData{phase:"starting".into(),..Default::default()}),asr:Mutex::new(None),relay:Mutex::new(relay),usb:usb.clone(),started:Instant::now()});
    control_slot().reserve(&c).map_err(str::to_owned)?;
    let preparation=(||{
        if !crate::volcengine_asr::settings_status()?.configured{return Err("请先配置语音识别服务；未启动录音".into());}
        if !c.owner.is_current(){return Err("组件已退出，未启动录音".into());}
        crate::component_audio::stop(None);crate::component_media::stop_all();let _=crate::music_player::execute(json!({"operation":"stop"}));
        let mut slot=active().lock().unwrap();
        capture_admission(slot.as_ref().map(|c|c.data.lock().unwrap().phase.clone()).as_deref())?;
        if !c.owner.is_current(){return Err("组件已退出，未启动录音".into());}
        *slot=Some(c.clone());Ok::<(),String>(())
    })();
    if let Err(error)=preparation{control_slot().release(&c);return Err(error);}
    let callback=c.clone();
    prepare_and_start(&c,||StreamingSpeechRecognizer::start(move|event|{
        handle_speech_event(&callback,event);
    }).map_err(|_|"云端语音识别未配置或不可用".to_string()),&|action|control(&c,action))?;
    let timer=c.clone();std::thread::spawn(move||{
        for _ in 0..180{
            std::thread::sleep(Duration::from_millis(250));
            if !current(&timer.id).is_some_and(|c|c.token==timer.token){return;}
            if !timer.owner.is_current(){request_stop(&timer,true);if let Some(asr)=timer.asr.lock().unwrap().as_ref(){asr.cancel();}return;}
            let phase=timer.data.lock().unwrap().phase.clone();
            if matches!(phase.as_str(),"draft"|"error"|"empty"){let _=control(&timer,"stop");return;}
            let status=timer.usb.status();
            if !status.connected||status.board_device_id!=timer.board||status.connection_id!=timer.connection||status.boot_id!=timer.boot{capture_error(&timer,"设备录音连接已断开或变化，未保存录音");return;}
            if phase=="starting"&&timer.started.elapsed()>Duration::from_secs(5){capture_error(&timer,"设备麦克风未开始录音，请检查设备状态后重试");return;}
            if timer.started.elapsed()>Duration::from_secs(28)&&matches!(phase.as_str(),"recording"|"starting"){
                timer.data.lock().unwrap().phase="recognizing".into();let _=control(&timer,"stop");
            }
        }
        capture_error(&timer,"识别超时，未保存笔记");
    });Ok(())
}
fn capture_scope_matches(c:&Capture,payload:&Value)->bool{
    let scope=&payload["_nativeUsbScope"];
    scope["runtime"]=="esp-p4"&&scope["boardDeviceId"]==c.board&&scope["connectionId"]==c.connection&&scope["bootId"]==c.boot
}
/// Own the complete audio session before the generic PTT route sees it.
pub fn route(topic:&str,payload:&Value)->bool{
    let Some(c)=active().lock().ok().and_then(|a|a.clone())else{return false;};
    if topic=="protocol/ack"&&payload["requestTopic"]=="audio/control"{
        if !capture_scope_matches(&c,payload){return false;}
        if payload["requestId"]==format!("notes-{}-start",c.token){if payload["ok"]==false{capture_error(&c,"设备未接受录音，请结束其他语音后重试");}return true;}
        if payload["requestId"]==format!("notes-{}-stop",c.token){return true;}
        return false;
    }
    if payload["boardDeviceId"]!=c.board||!topic.starts_with("audio/"){return false;}
    if !matches!(topic,"audio/begin"|"audio/chunk"|"audio/end"|"audio/error"){return false;}
    if !capture_scope_matches(&c,payload){return false;}
    {let mut d=c.data.lock().unwrap();
    if topic=="audio/begin"&&payload["captureOwner"]==c.token&&!d.session.is_empty()&&payload["sessionId"]!=d.session{return true;}
    if !owns_audio(&d,&c.token,topic,payload){return false;}
    if topic=="audio/begin"&&d.session.is_empty(){d.session=payload["sessionId"].as_str().unwrap_or("").into();}
    // Drain our late packets instead of leaking them into generic voice-input routing.
    if !accepting_audio(&d.phase)||d.diagnostics.ended{return true;}}
    if !c.owner.is_current(){request_stop(&c,true);if let Some(asr)=c.asr.lock().unwrap().as_ref(){asr.cancel();}return true;}
    let mut relay=c.relay.lock().unwrap();let event=relay.handle_scoped(topic,payload,&c.board,&c.connection,&c.boot);
    if event.as_ref().is_some_and(|v|v["ok"]==false){drop(relay);capture_error(&c,"设备录音传输中断，未保存笔记");return true;}
    if topic=="audio/begin"{let mut d=c.data.lock().unwrap();d.session=payload["sessionId"].as_str().unwrap_or("").into();if d.phase=="starting"{d.phase="recording".into();}}
    if let Some(chunk)=relay.take_validated_chunk(){
        let over_limit=c.data.lock().unwrap().diagnostics.bytes.saturating_add(chunk.pcm.len() as u64)>MAX_CAPTURE_PCM_BYTES;
        if over_limit{drop(relay);capture_error(&c,"录音超过 30 秒上限，已停止；未保存笔记");return true;}
        {let mut d=c.data.lock().unwrap();d.diagnostics.chunks+=1;d.diagnostics.bytes+=chunk.pcm.len() as u64;for sample in chunk.pcm.chunks_exact(2){let level=i16::from_le_bytes([sample[0],sample[1]]).unsigned_abs() as u32;d.diagnostics.peak=d.diagnostics.peak.max(level);if level>512{d.diagnostics.energetic_samples+=1;}}}
        let result=c.asr.lock().unwrap().as_ref().ok_or_else(||"语音识别连接已结束".to_string()).and_then(|asr|asr.push_pcm(&chunk.pcm));
        if let Err(error)=result{drop(relay);handle_speech_event(&c,StreamingSpeechEvent::Error(error));return true;}}
    if relay.take_completed().is_some(){
        let empty={let mut d=c.data.lock().unwrap();d.diagnostics.ended=true;if !accepting_audio(&d.phase){return true;}let empty=d.diagnostics.bytes==0;d.phase=if empty{"empty"}else{"recognizing"}.into();empty};
        if empty{if let Some(asr)=c.asr.lock().unwrap().as_ref(){asr.cancel();}}
        else{let result=c.asr.lock().unwrap().as_ref().ok_or_else(||"语音识别连接已结束，未保存笔记".to_string()).and_then(|asr|asr.finish());
            if let Err(error)=result{drop(relay);handle_speech_event(&c,StreamingSpeechEvent::Error(error));return true;}}
    }
    true
}
#[derive(Clone,Serialize,Deserialize)]struct Note{id:String,text:String,created:String,todo:Option<String>}
#[derive(Default,Serialize,Deserialize)]struct Notes{items:Vec<Note>,selected:usize}
fn read(dir:&Path)->Result<Notes,String>{match std::fs::read(dir.join("notes.json")){Ok(b)if b.len()<=1048576=>{let mut n:Notes=serde_json::from_slice(&b).map_err(|_|"笔记文件损坏，未覆盖")?;let mut ids=std::collections::HashSet::new();if n.items.len()>300||n.items.iter().any(|n|n.id.is_empty()||n.id.len()>64||!ids.insert(&n.id)||n.text.len()>1800||n.text.trim().is_empty()||chrono::DateTime::parse_from_rfc3339(&n.created).is_err()){return Err("笔记文件超出限制或存在无效记录，未覆盖".into());}n.selected%=n.items.len().max(1);Ok(n)},Ok(_)=>Err("笔记文件超出限制".into()),Err(e)if e.kind()==std::io::ErrorKind::NotFound=>Ok(Notes::default()),Err(_)=>Err("无法读取笔记".into())}}
fn save(dir:&Path,notes:&Notes)->Result<(),String>{save_before_commit(dir,notes,||{})}
fn save_before_commit(dir:&Path,notes:&Notes,before_commit:impl FnOnce())->Result<(),String>{let permit=crate::component_runtime_lifecycle::storage_commit_permit()?;std::fs::create_dir_all(dir).map_err(|_|"无法创建笔记目录")?;let mut f=tempfile::NamedTempFile::new_in(dir).map_err(|_|"无法准备笔记存储")?;f.write_all(&serde_json::to_vec(notes).map_err(|_|"笔记编码失败")?).and_then(|_|f.as_file().sync_all()).map_err(|_|"笔记写入失败")?;before_commit();permit.ensure_current()?;f.persist(dir.join("notes.json")).map_err(|_|"笔记保存失败")?;Ok(())}
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
    if op=="record"{if let Some(c)=current(id).filter(|c|matches!(c.data.lock().unwrap().phase.as_str(),"starting"|"recording")){request_stop(&c,false);}else{start(id,usb)?;}}
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
pub fn stop_recording(id:&str){if let Some(c)=current(id){request_stop(&c,false);}}
pub fn stop_recording_all(){let c=active().lock().ok().and_then(|v|v.clone());if let Some(c)=c{request_stop(&c,false);}}

/// Memory-reader integration only: no real ASR factory, settings or serial port.
#[cfg(test)]pub(crate) fn install_owned_capture_for_test(usb:UsbSerialManager,board:&str,generation:u64,id:&str)->(impl Drop,String){
    struct Guard{capture:Arc<Capture>,_serial:std::sync::MutexGuard<'static,()>}
    impl Drop for Guard{fn drop(&mut self){
        {let mut slot=active().lock().unwrap();if slot.as_ref().is_some_and(|v|v.token==self.capture.token){slot.take();}}
        control_slot().release(&self.capture);
    }}
    let serial=tests::CAPTURE_TEST.lock().unwrap();
    let c=Arc::new(Capture{generation,control:Default::default(),owner:crate::component_app::MediaAuthority{request:crate::component_runtime_lifecycle::ResourceOwner::fresh(),view:None},id:id.into(),board:board.into(),connection:usb.status().connection_id,boot:usb.status().boot_id,token:uuid::Uuid::new_v4().to_string(),data:Mutex::new(CaptureData{phase:"recording".into(),..Default::default()}),asr:Mutex::new(None),relay:Mutex::new(Default::default()),usb,started:Instant::now()});
    c.control.prepared();assert!(c.control.admit_start());c.control.start_returned();
    control_slot().reserve(&c).unwrap();*active().lock().unwrap()=Some(c.clone());
    c.relay.lock().unwrap().configure_owned_scoped(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false,&c.board,&c.connection,&c.boot,&c.token);
    let token=c.token.clone();(Guard{capture:c,_serial:serial},token)
}

#[cfg(test)]mod tests{
    use super::*;
    pub(super) static CAPTURE_TEST:Mutex<()>=Mutex::new(());
    fn fake_capture(id:&str,phase:&str)->Arc<Capture>{
        Arc::new(Capture{generation:1,control:Default::default(),owner:crate::component_app::MediaAuthority{request:crate::component_runtime_lifecycle::ResourceOwner::fresh(),view:None},id:id.into(),board:"test".into(),connection:"test-connection".into(),boot:"test-boot".into(),token:uuid::Uuid::new_v4().to_string(),data:Mutex::new(CaptureData{phase:phase.into(),text:"未保存的想法".into(),..Default::default()}),asr:Mutex::new(None),relay:Mutex::new(Default::default()),usb:UsbSerialManager::new(),started:Instant::now()})
    }
    #[test]fn phase2_exit_during_asr_preparation_never_sends_start_and_cancels_new_asr(){
        let _serial=CAPTURE_TEST.lock().unwrap();
        for mode in 0..3{
            let c=fake_capture("phase2-asr","starting");*active().lock().unwrap()=Some(c.clone());
            let (ready_tx,ready_rx)=std::sync::mpsc::channel();let (resume_tx,resume_rx)=std::sync::mpsc::channel();
            let pending=c.clone();
            let worker=std::thread::spawn(move||{
                let commands=Mutex::new(Vec::new());let (asr,_sink)=StreamingSpeechRecognizer::test_sink();
                let result=prepare_and_start(&pending,||{
                    ready_tx.send(()).unwrap();resume_rx.recv_timeout(Duration::from_secs(5)).unwrap();Ok(asr)
                },&|action|{commands.lock().unwrap().push(action.to_string());Ok(())});
                assert!(result.is_err());assert!(commands.lock().unwrap().is_empty());
                assert!(pending.asr.lock().unwrap().as_ref().unwrap().push_pcm(&[0,0]).is_err());
            });
            ready_rx.recv_timeout(Duration::from_secs(5)).unwrap();
            let replacement=if mode==0{cancel(Some(&c.id));None}
                else if mode==1{c.owner.request.cancel();stop_recording(&c.id);None}
                else{let next=fake_capture("phase2-asr-new","starting");*active().lock().unwrap()=Some(next.clone());Some(next)};
            resume_tx.send(()).unwrap();worker.join().unwrap();
            if let Some(next)=replacement{assert_eq!(current(&next.id).unwrap().token,next.token);assert!(next.asr.lock().unwrap().is_none());cancel(Some(&next.id));}
            else{cancel(Some(&c.id));}
        }
    }
    #[test]fn phase2_cancel_during_physical_start_defers_exact_stop_until_send_returns(){
        let _serial=CAPTURE_TEST.lock().unwrap();let c=fake_capture("phase2-start-order","starting");
        *active().lock().unwrap()=Some(c.clone());
        let commands=Mutex::new(Vec::new());let (asr,_sink)=StreamingSpeechRecognizer::test_sink();
        assert!(prepare_and_start(&c,||Ok(asr),&|action|{
            commands.lock().unwrap().push(action.to_string());
            if action=="start"{c.owner.request.cancel();stop_recording(&c.id);assert!(c.control.requested());}
            Ok(())
        }).is_err());
        assert_eq!(process_control(&c,&|action|{commands.lock().unwrap().push(action.to_string());Ok(())}),Some(true));
        assert_eq!(*commands.lock().unwrap(),vec!["start","stop"]);
        assert!(c.asr.lock().unwrap().as_ref().unwrap().push_pcm(&[0,0]).is_err());
        cancel(Some(&c.id));
    }
    #[test]fn phase2_other_component_cannot_discard_any_unsaved_draft(){
        let _serial=CAPTURE_TEST.lock().unwrap();let c=fake_capture("phase2-draft-a","draft");
        *active().lock().unwrap()=Some(c.clone());
        {let slot=active().lock().unwrap();assert!(capture_admission(slot.as_ref().map(|v|v.data.lock().unwrap().phase.clone()).as_deref()).is_err());}
        assert_eq!(current(&c.id).unwrap().token,c.token);assert_eq!(c.data.lock().unwrap().text,"未保存的想法");
        cancel(Some(&c.id));assert!(capture_admission(None).is_ok());
    }
    #[test]fn phase2_notes_cancel_at_synced_temp_boundary_preserves_original_bytes(){
        use crate::component_runtime_lifecycle::{ResourceOwner,enter_owner};
        let dir=tempfile::tempdir().unwrap();let n=Notes{items:vec![Note{id:"old".into(),text:"old".into(),created:"2026-11-03T00:00:00Z".into(),todo:None}],selected:0};
        save(dir.path(),&n).unwrap();let bytes=std::fs::read(dir.path().join("notes.json")).unwrap();
        let owner=ResourceOwner::fresh();let scope=enter_owner(&owner);
        assert!(save_before_commit(dir.path(),&Notes::default(),||owner.cancel()).is_err());
        assert_eq!(std::fs::read(dir.path().join("notes.json")).unwrap(),bytes);
        assert!(save(dir.path(),&Notes::default()).is_err());
        let usb=UsbSerialManager::new();
        for (op,input) in [("edit",json!({"noteId":"old","text":"revoked edit"})),("delete",json!({"noteId":"old","confirm":true}))]{
            assert!(execute("phase2-storage",op,&input,dir.path(),&usb).is_err());
            assert_eq!(std::fs::read(dir.path().join("notes.json")).unwrap(),bytes);
        }
        drop(scope);
        save(dir.path(),&Notes::default()).unwrap();owner.cancel();assert!(read(dir.path()).unwrap().items.is_empty());
    }
    #[test]fn phase3_error_and_visibility_stop_do_not_send_until_control_worker_drains(){
        let _serial=CAPTURE_TEST.lock().unwrap();
        for error in [false,true]{
            let c=fake_capture("phase3-reader-stop","recording");
            c.control.prepared();assert!(c.control.admit_start());c.control.start_returned();
            *active().lock().unwrap()=Some(c.clone());control_slot().reserve(&c).unwrap();
            if error{assert!(route("protocol/ack",&json!({"requestTopic":"audio/control","requestId":format!("notes-{}-start",c.token),"ok":false,
                "_nativeUsbScope":{"runtime":"esp-p4","boardDeviceId":c.board,"connectionId":c.connection,"bootId":c.boot}})));}
            else{stop_recording(&c.id);}
            assert!(c.control.requested());assert!(!c.control.failed());
            let commands=Mutex::new(Vec::new());
            assert_eq!(process_control(&c,&|action|{commands.lock().unwrap().push((c.token.clone(),action.to_owned()));Ok(())}),Some(true));
            assert_eq!(process_control(&c,&|_|panic!("duplicate physical stop")),None);
            assert_eq!(*commands.lock().unwrap(),vec![(c.token.clone(),"stop".into())]);
            control_slot().release(&c);cancel(Some(&c.id));
        }
    }
    #[test]fn phase3_failed_stop_is_not_retried_and_never_discards_unsaved_draft(){
        let _serial=CAPTURE_TEST.lock().unwrap();let c=fake_capture("phase3-stop-failure","draft");
        c.control.prepared();assert!(c.control.admit_start());c.control.start_returned();
        *active().lock().unwrap()=Some(c.clone());control_slot().reserve(&c).unwrap();
        stop_recording(&c.id);
        assert_eq!(process_control(&c,&|_|Err("injected writer failure".into())),Some(false));
        assert!(c.control.failed());assert_eq!(process_control(&c,&|_|panic!("must not retry")),None);
        assert!(control_slot().reserve(&fake_capture("phase3-next","starting")).is_err());
        assert_eq!(c.data.lock().unwrap().phase,"draft");assert_eq!(c.data.lock().unwrap().text,"未保存的想法");
        control_slot().release(&c);cancel(Some(&c.id));
    }
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
    fn first_cause_capture(id:&str)->(Arc<Capture>,impl Sized){
        let mut c=fake_capture(id,"starting");
        {let c=Arc::get_mut(&mut c).unwrap();c.connection="1".into();c.boot="0123456789abcdef".into();}
        c.data.lock().unwrap().text.clear();
        assert_eq!(c.relay.lock().unwrap().configure_owned_scoped(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false,&c.board,&c.connection,&c.boot,&c.token)["ok"],true);
        let (asr,sink)=StreamingSpeechRecognizer::test_sink();*c.asr.lock().unwrap()=Some(asr);
        c.control.prepared();assert!(c.control.admit_start());c.control.start_returned();
        control_slot().reserve(&c).unwrap();*active().lock().unwrap()=Some(c.clone());
        (c,sink)
    }
    fn first_cause_frame(c:&Capture,mut payload:Value)->Value{
        payload["boardDeviceId"]=json!(c.board);payload["sessionId"]=json!("notes-first-cause-session");
        payload["_nativeUsbScope"]=json!({"runtime":"esp-p4","boardDeviceId":c.board,"connectionId":c.connection,"bootId":c.boot});payload
    }
    fn first_cause_snapshot(c:&Capture)->Value{
        let d=c.data.lock().unwrap();json!({"phase":d.phase,"text":d.text,"error":d.error,"diagnostics":d.diagnostics})
    }
    fn first_cause_stop_once(c:&Arc<Capture>){
        let calls=std::sync::atomic::AtomicUsize::new(0);
        assert_eq!(process_control(c,&|action|{assert_eq!(action,"stop");calls.fetch_add(1,std::sync::atomic::Ordering::SeqCst);Ok(())}),Some(true));
        assert_eq!(process_control(c,&|_|panic!("terminal must not resend stop")),None);
        assert_eq!(calls.load(std::sync::atomic::Ordering::SeqCst),1);
        control_slot().release(c);
    }
    #[test]fn cancelled_owned_begin_is_drained_before_generic_ptt_even_after_stop_or_replacement(){
        let _serial=CAPTURE_TEST.lock().unwrap();
        for replacement in [false,true]{
            let mut old=fake_capture("orphan-old","starting");
            {let c=Arc::get_mut(&mut old).unwrap();c.connection="42".into();c.boot="0123456789abcdef".into();}
            control_slot().reserve(&old).unwrap();old.control.prepared();assert!(old.control.admit_start());
            *active().lock().unwrap()=Some(old.clone());
            cancel(Some(&old.id));assert!(active().lock().unwrap().is_none());
            assert!(old.control.requested());assert_eq!(old.control.claim(),None); // start is still inflight
            let mut next=None;
            if replacement{
                old.control.start_returned();first_cause_stop_once(&old);
                let mut c=fake_capture("orphan-next","starting");
                {let c=Arc::get_mut(&mut c).unwrap();c.connection="42".into();c.boot="0123456789abcdef".into();}
                assert_eq!(c.relay.lock().unwrap().configure_owned_scoped(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false,&c.board,&c.connection,&c.boot,&c.token)["ok"],true);
                control_slot().reserve(&c).unwrap();c.control.prepared();assert!(c.control.admit_start());c.control.start_returned();
                *active().lock().unwrap()=Some(c.clone());next=Some(c);
            }
            let begin=first_cause_frame(&old,json!({"captureOwner":old.token,"format":"pcm_s16le","sampleRate":16000,"channels":1,"bitsPerSample":16}));
            assert!(!route("audio/begin",&begin)); // Exact notes route declines retired owner.
            let mut ptt=crate::usb_audio::UsbAudioRelay::default();
            assert_eq!(ptt.configure_scoped(true,crate::usb_audio::DEFAULT_PCM_RELAY_PORT,false,&old.board,&old.connection,&old.boot)["ok"],true);
            let event=ptt.handle_scoped("audio/begin",&begin,&old.board,&old.connection,&old.boot).unwrap();
            // This is the generic dispatcher's actual admission predicate. No
            // ASR factory/cloud is invoked by this memory-only composition.
            let starts_generic=event["ok"]==true&&event["duplicate"]!=true;
            assert!(!starts_generic);assert_eq!(event["admissionRejected"],true);
            for topic in ["audio/chunk","audio/end"]{
                let event=ptt.handle_scoped(topic,&begin,&old.board,&old.connection,&old.boot).unwrap();
                assert_eq!(event["terminalAlreadyReported"],true);assert_ne!(event["phase"],"end");
                assert!(ptt.take_validated_chunk().is_none());assert!(ptt.take_completed().is_none());
            }
            if let Some(c)=next{
                assert_eq!(current(&c.id).unwrap().token,c.token);
                let mut current_begin=begin.clone();current_begin["captureOwner"]=json!(c.token);current_begin["sessionId"]=json!("notes-current-session");
                assert!(route("audio/begin",&current_begin));assert_eq!(c.data.lock().unwrap().phase,"recording");
                cancel(Some(&c.id));first_cause_stop_once(&c);
            }else{old.control.start_returned();first_cause_stop_once(&old);}
        }
    }
    #[test]fn notes_control_ack_requires_exact_native_scope_and_request_owner(){
        let _serial=CAPTURE_TEST.lock().unwrap();let c=fake_capture("notes-ack-owner","starting");
        *active().lock().unwrap()=Some(c.clone());
        let ack=json!({"requestTopic":"audio/control","requestId":format!("notes-{}-start",c.token),"ok":false,
            "_nativeUsbScope":{"runtime":"esp-p4","boardDeviceId":c.board,"connectionId":c.connection,"bootId":c.boot}});
        for (key,value) in [("boardDeviceId","other"),("connectionId","old-generation"),("bootId","old-boot"),("runtime","other")]{
            let mut stale=ack.clone();stale["_nativeUsbScope"][key]=json!(value);
            assert!(!route("protocol/ack",&stale));assert_eq!(c.data.lock().unwrap().phase,"starting");assert!(!c.control.requested());
        }
        let mut foreign=ack.clone();foreign["requestId"]=json!("notes-foreign-start");
        assert!(!route("protocol/ack",&foreign));assert!(!c.control.requested());
        assert!(route("protocol/ack",&ack));assert_eq!(c.data.lock().unwrap().phase,"error");assert!(c.control.requested());
        cancel(Some(&c.id));
    }
    #[test]fn notes_first_cause_real_failed_push_and_finish_survive_delayed_worker_callback(){
        use base64::{engine::general_purpose::STANDARD,Engine as _};
        let _serial=CAPTURE_TEST.lock().unwrap();
        // Actual relay admission/PCM/End, actual shared recognizer input failure,
        // actual notes route and stop Ticket. Only cloud callback delivery is held.
        for finish in [false,true]{for callback_first in [false,true]{
            let (c,_sink)=first_cause_capture("first-cause-route");
            let begin=first_cause_frame(&c,json!({"captureOwner":c.token,"format":"pcm_s16le","sampleRate":16000,"channels":1,"bitsPerSample":16}));
            assert!(route("audio/begin",&begin));assert_eq!(c.data.lock().unwrap().phase,"recording");
            let pcm=[1u8,2,3,4];let checksum=pcm.iter().fold(0xcbf29ce484222325u64,|hash,b|(hash^u64::from(*b)).wrapping_mul(0x100000001b3));
            let chunk=first_cause_frame(&c,json!({"seq":0,"bytes":pcm.len(),"data":STANDARD.encode(pcm),"checksum":format!("{checksum:016x}")}));
            assert!(route("audio/chunk",&chunk));
            let generation=c.asr.lock().unwrap().as_ref().unwrap().generation();
            let error=c.asr.lock().unwrap().as_ref().unwrap().push_pcm(&[0]).unwrap_err();
            assert!(error.contains("完整的 16-bit"));
            if callback_first{assert!(handle_speech_event(&c,StreamingSpeechEvent::Error(error.clone())));}
            let mut late_chunk=chunk.clone();late_chunk["seq"]=json!(1);
            let end=first_cause_frame(&c,json!({"chunks":1,"bytes":pcm.len(),"checksum":format!("{checksum:016x}")}));
            assert!(route(if finish{"audio/end"}else{"audio/chunk"},if finish{&end}else{&late_chunk}));
            let saved=first_cause_snapshot(&c);assert_eq!(saved["phase"],"error");
            assert_eq!(saved["diagnostics"]["error_stage"],"input");assert_eq!(saved["diagnostics"]["error_cause"],"pcm_input_error");
            assert!(!handle_speech_event(&c,StreamingSpeechEvent::Error(error.clone())));
            for event in [StreamingSpeechEvent::Ready,StreamingSpeechEvent::Partial{revision:5,text:"迟到片段".into(),confidence:None},StreamingSpeechEvent::Final{revision:6,text:"迟到结果".into(),confidence:None}]{assert!(!handle_speech_event(&c,event));}
            assert!(route("audio/chunk",&late_chunk));assert!(route("audio/end",&end));assert!(route("audio/end",&end));
            assert_eq!(first_cause_snapshot(&c),saved);assert!(c.relay.lock().unwrap().take_validated_chunk().is_none());
            {let asr=c.asr.lock().unwrap();let asr=asr.as_ref().unwrap();
                assert_eq!(asr.generation(),generation);assert_eq!(asr.push_pcm(&pcm).unwrap_err(),error);assert_eq!(asr.finish().unwrap_err(),error);}
            first_cause_stop_once(&c);cancel(Some(&c.id));
        }}
    }
    #[test]fn notes_first_cause_provider_return_and_callback_share_one_sanitized_terminal(){
        let _serial=CAPTURE_TEST.lock().unwrap();
        let error="语音识别服务等待音频超时，本段已结束（provider=volcengine, code=45000081, logid=synthetic）: private-provider-detail";
        for phase in ["recording","recognizing"]{
            let (c,_sink)=first_cause_capture("first-cause-provider");c.data.lock().unwrap().phase=phase.into();
            // The return-error branch and the later callback use this same
            // production transition; a second notification cannot become a terminal.
            assert!(handle_speech_event(&c,StreamingSpeechEvent::Error(error.into())));
            let saved=first_cause_snapshot(&c);assert_eq!(saved["diagnostics"]["provider_code"],45000081);
            assert_eq!(saved["diagnostics"]["error_stage"],"provider_audio_timeout");
            assert!(!saved.to_string().contains("private-provider-detail"));assert!(!saved.to_string().contains("logid"));
            assert!(!handle_speech_event(&c,StreamingSpeechEvent::Error(error.into())));
            assert!(!handle_speech_event(&c,StreamingSpeechEvent::Error("连接关闭".into())));
            assert_eq!(first_cause_snapshot(&c),saved);first_cause_stop_once(&c);cancel(Some(&c.id));
        }
    }
    #[test]fn notes_first_cause_cancel_replacement_and_final_cannot_be_overwritten(){
        let _serial=CAPTURE_TEST.lock().unwrap();
        let (old,_old_sink)=first_cause_capture("first-cause-cancel");
        cancel(Some(&old.id));let cancelled=first_cause_snapshot(&old);assert_eq!(cancelled["phase"],"cancelled");
        assert!(!handle_speech_event(&old,StreamingSpeechEvent::Error("具体返回错误 code=45000081".into())));
        assert!(!handle_speech_event(&old,StreamingSpeechEvent::Final{revision:1,text:"不得复活".into(),confidence:None}));
        assert_eq!(first_cause_snapshot(&old),cancelled);first_cause_stop_once(&old);
        let (next,_next_sink)=first_cause_capture("first-cause-next");let fresh=first_cause_snapshot(&next);
        assert!(!handle_speech_event(&old,StreamingSpeechEvent::Error("迟到旧错误".into())));
        assert_eq!(first_cause_snapshot(&next),fresh);assert!(!next.control.requested());
        assert!(handle_speech_event(&next,StreamingSpeechEvent::Final{revision:2,text:"真正最终草稿".into(),confidence:None}));
        let final_draft=first_cause_snapshot(&next);
        assert!(!handle_speech_event(&next,StreamingSpeechEvent::Error("迟到输入错误".into())));
        assert_eq!(first_cause_snapshot(&next),final_draft);assert_eq!(final_draft["phase"],"draft");
        first_cause_stop_once(&next);cancel(Some(&next.id));
        let (revoked,_sink)=first_cause_capture("first-cause-revoked");revoked.owner.request.cancel();
        assert!(handle_speech_event(&revoked,StreamingSpeechEvent::Error("具体错误 code=45000081".into())));
        let d=revoked.data.lock().unwrap();assert_eq!(d.phase,"cancelled");assert!(d.error.is_empty());assert!(d.diagnostics.provider_code.is_none());drop(d);
        first_cause_stop_once(&revoked);cancel(Some(&revoked.id));
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
