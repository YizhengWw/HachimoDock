//! Bounded component audio: original procedural soundscapes and configured TTS.
//! One owner, exact-session stop, no automatic restart after voice or disconnect.
use crate::usb_serial::UsbSerialManager;
use serde_json::{json,Value};
use std::sync::{Arc,Mutex,OnceLock,atomic::{AtomicBool,AtomicU32,Ordering}};
use std::time::{Duration,Instant};
use base64::Engine;
mod speech_queue;
mod speech_cache;
pub use speech_queue::SpeechItem;
pub(crate) fn configure_speech_cache(root:std::path::PathBuf){speech_cache::configure(root);}
pub(crate) fn prepare_news_voice(id:&str,board:&str,cfg:crate::doubao_tts::DoubaoTtsConfig,items:Vec<SpeechItem>){speech_cache::prepare(id,board,cfg,items);}
struct Job{id:String,board:String,session:String,cancel:AtomicBool,volume:AtomicU32,phase:Mutex<String>,error:Mutex<String>,heartbeat:Mutex<Instant>,sound_total:AtomicU32,sound_offset:AtomicU32,speech_key:String,speech_item:Mutex<String>,speech_offset:AtomicU32,speech_gap_ms:AtomicU32}
static JOB:OnceLock<Mutex<Option<Arc<Job>>>>=OnceLock::new();
pub(crate) fn active_for_exit()->bool {
    JOB.get().is_some_and(|jobs| jobs.try_lock().map(|job| job.as_ref().is_some_and(|j|
        !j.cancel.load(Ordering::Acquire) && j.phase.try_lock().map(|phase|
            matches!(phase.as_str(),"preparing"|"buffering"|"playing")).unwrap_or(true))).unwrap_or(true))
}
static RUN:Mutex<()>=Mutex::new(());
static PLAYBACK:OnceLock<Mutex<std::collections::HashMap<String,Value>>>=OnceLock::new();
pub(crate) fn report(id:&str,phase:&str,message:&str){
    let value=json!({"phase":phase,"message":message});
    let mut map=PLAYBACK.get_or_init(Default::default).lock().unwrap();
    if map.get(id)==Some(&value){return;}
    if map.len()<32||map.contains_key(id){map.insert(id.into(),value);}
    crate::realtime_chat_log::record("","component_playback",json!({"state":phase,"service":match id{"book-pocket"=>"books","hachimi-learn"=>"knowledge","daily-news"=>"news",_=>"other"},"errorKind":if message.is_empty(){"none"}else{crate::realtime_chat_log::error_kind(message)}}));
}
pub(crate) fn playback_status(id:&str)->Value{
    PLAYBACK.get_or_init(Default::default).lock().unwrap().get(id).cloned().unwrap_or(json!({"phase":"idle","message":"等待在设备上打开组件"}))
}
fn jobs()->&'static Mutex<Option<Arc<Job>>>{JOB.get_or_init(Default::default)}
fn job(id:&str)->Option<Arc<Job>>{jobs().lock().ok()?.as_ref().filter(|j|j.id==id).cloned()}
pub fn stop(id:Option<&str>){if let Some(j)=jobs().lock().ok().and_then(|v|v.clone()).filter(|j|id.is_none_or(|id|id==j.id)){j.cancel.store(true,Ordering::Release);}}
pub fn heartbeat(id:&str,visible:bool){if let Some(j)=job(id){if visible{*j.heartbeat.lock().unwrap()=Instant::now();}else{j.cancel.store(true,Ordering::Release);}}}
pub fn observe(topic:&str,p:&Value){
    if topic=="audio/begin" || topic=="media/event"&&p["global"]==true {stop(None);}
}
pub fn status(id:&str)->Value{job(id).map(|j|json!({"phase":j.phase.lock().unwrap().clone(),"error":j.error.lock().unwrap().clone(),"speechId":j.speech_key,"speechItemId":j.speech_item.lock().unwrap().clone(),"speechOffset":j.speech_offset.load(Ordering::Acquire),"maxSpeechGapMs":j.speech_gap_ms.load(Ordering::Acquire),"remainingSeconds":j.sound_total.load(Ordering::Acquire).saturating_sub(j.sound_offset.load(Ordering::Acquire)).div_ceil(48000)})).unwrap_or(json!({"phase":"idle","error":""}))}
/// Change this soundscape's total duration, never replace its audio session.
pub fn sound_timer(id:&str,minutes:u32){if let Some(j)=job(id){if j.sound_total.load(Ordering::Acquire)>0{j.sound_total.store(minutes.clamp(1,60)*60*48000,Ordering::Release);}}}
pub fn volume(id:&str,v:u32){if let Some(j)=job(id){j.volume.store(v.min(100),Ordering::Release);}}
pub fn playing(id:&str)->bool{job(id).is_some_and(|j|!j.cancel.load(Ordering::Acquire)&&matches!(j.phase.lock().unwrap().as_str(),"preparing"|"buffering"|"playing"))}
pub enum Source{Sound{kind:usize,minutes:u32},Speech(String),SpeechQueue{key:String,items:Vec<SpeechItem>}}
fn capture_busy(final_requested:bool,final_handled:bool,cancelled:bool)->bool{
    !cancelled&&!(final_requested&&final_handled)
}
pub(crate) fn voice_busy()->bool{
    crate::realtime_chat::status().active||crate::active_device_voice_context().lock().map(|v|v.as_ref().is_some_and(|c|
        capture_busy(c.final_requested.load(Ordering::SeqCst),c.final_handled.load(Ordering::SeqCst),c.cancelled.load(Ordering::SeqCst)))).unwrap_or(true)
}
pub fn start(id:&str,usb:&UsbSerialManager,source:Source,volume:u32)->Result<(),String>{
    let board=usb.status();if !board.connected{return Err("请先连接设备，并在设备打开这个组件".into());}
    if voice_busy(){return Err("语音正在使用，请结束后再播放".into());}
    let cfg=if matches!(source,Source::Speech(_)|Source::SpeechQueue{..}){Some(crate::component_voice::config(&board.board_device_id)?)}else{None};
    if let Source::SpeechQueue{key,items}=&source{speech_queue::validate(key,items)?;}
    stop(None);crate::component_media::stop_all();
    let total=match &source{Source::Sound{minutes,..}=>(*minutes).clamp(1,60)*60*48000,_=>0};
    let speech_key=if let Source::SpeechQueue{key,..}=&source{key.clone()}else{String::new()};
    let j=Arc::new(Job{id:id.into(),board:board.board_device_id,session:format!("music-app-{}",uuid::Uuid::new_v4().simple()),cancel:AtomicBool::new(false),volume:AtomicU32::new(volume.min(100)),phase:Mutex::new("preparing".into()),error:Mutex::new(String::new()),heartbeat:Mutex::new(Instant::now()),sound_total:AtomicU32::new(total),sound_offset:AtomicU32::new(0),speech_key,speech_item:Mutex::new(String::new()),speech_offset:AtomicU32::new(0),speech_gap_ms:AtomicU32::new(0)});
    *jobs().lock().unwrap()=Some(j.clone());let usb=usb.clone();
    report(id,"preparing","");
    std::thread::Builder::new().name("component-audio".into()).spawn(move||{
        let _gate=RUN.lock().unwrap();if j.cancel.load(Ordering::Acquire){*j.phase.lock().unwrap()="idle".into();return;}
        #[cfg(target_os="macos")]
        let _activity=crate::realtime_audio_macos::AudioActivity::begin_on_player_thread();
        let result=(||{
            if let Source::SpeechQueue{items,..}=&source{return speech_queue::play(&j,&usb,cfg.unwrap(),items.clone());}
            let pcm=if let Source::Speech(text)=&source{
                let mut pcm=Vec::new();let mut overflow=false;let mut client=crate::doubao_tts::DoubaoTtsClient::new(cfg.unwrap());
                let completed=tauri::async_runtime::block_on(async{
                    let result={
                        let future=client.synthesize(text,|chunk|{if pcm.len()+chunk.len()<=1_920_000{pcm.extend_from_slice(chunk);}else{overflow=true;}});
                        tokio::select!{
                            result=tokio::time::timeout(Duration::from_secs(35),future)=>result.map_err(|_|"朗读生成超时".to_string()).and_then(|v|v.map(|_|true).map_err(|_|"朗读服务失败，请检查语音配置".into())),
                            _=async{while !stopped(&j,&usb){tokio::time::sleep(Duration::from_millis(100)).await;}}=>Ok(false),
                        }
                    };
                    client.reset().await;result
                })?;
                if !completed{return Ok(());}
                if overflow{return Err("朗读内容超过音频长度上限，未播放截断语音".into());}
                if pcm.is_empty(){return Err("没有生成可播放的语音".into());}Some(resample(&pcm))
            }else{None};
            if stopped(&j,&usb){return Ok(());}
            // Firmware rejects a source unless this exact host component is open.
            usb.media_request(&j.board,"media/begin",json!({"source":format!("app.{}",j.id),"mode":"live","sessionId":j.session,"format":"ima-block-v1","sampleRate":48000,"durationMs":0,"offsetMs":0,"volume":100,"title":"组件音频","artist":"HachimoDock","queue":[],"keys":[]})).map_err(|_|"设备未接受播放，请在设备打开此组件并检查固件")?;
            *j.phase.lock().unwrap()="buffering".into();
            let (kind,seconds)=match source{Source::Sound{kind,minutes}=>(kind,(minutes.clamp(1,60)*60)as usize),Source::Speech(_)|Source::SpeechQueue{..}=>(0,0)};
            let total=pcm.as_ref().map(|p|p.len()/2).unwrap_or(seconds*48000);let mut synth=Synth::new(kind);
            let mut offset=0;let mut seq=0;let mut buffered=0;let started=Instant::now();
            while !stopped(&j,&usb){
                let total=if pcm.is_some(){total}else{j.sound_total.load(Ordering::Acquire)as usize};
                if offset>=total{break;}
                if started.elapsed()>Duration::from_secs(3605){break;}
                if buffered>560{std::thread::sleep(Duration::from_millis(40));let ack=usb.media_request(&j.board,"media/query",json!({"sessionId":j.session})).map_err(|_|"音频会话已结束")?;buffered=ack["bufferedMs"].as_u64().unwrap_or(0);continue;}
                let samples=(total-offset).min(3840);let mut block=if let Some(p)=&pcm{p[offset*2..(offset+samples)*2].to_vec()}else{synth.block(samples,offset,total)};
                // Volume lives in PCM, so delayed control messages cannot change another owner's volume.
                let gain=j.volume.load(Ordering::Acquire)as f32/100.0;
                for b in block.chunks_exact_mut(2){let v=(i16::from_le_bytes([b[0],b[1]])as f32*gain)as i16;b.copy_from_slice(&v.to_le_bytes());}
                let data=crate::media_codec::encode(&block)?;
                let ack=usb.media_request(&j.board,"media/chunk",json!({"sessionId":j.session,"seq":seq,"data":base64::engine::general_purpose::STANDARD.encode(data)})).map_err(|_|"设备音频传输中断，已停止播放")?;
                buffered=ack["bufferedMs"].as_u64().unwrap_or(0);if ack["state"]=="playing"{*j.phase.lock().unwrap()="playing".into();}offset+=samples;seq+=1;
                if pcm.is_none(){j.sound_offset.store(offset as u32,Ordering::Release);}
            }
            if !stopped(&j,&usb){let _=usb.media_request(&j.board,"media/end",json!({"sessionId":j.session}));for _ in 0..30{if stopped(&j,&usb){break;}std::thread::sleep(Duration::from_millis(40));}}
            Ok::<(),String>(())
        })();
        // A stale worker must never stop the next stream, task, or real-time conversation.
        let _=usb.media_request(&j.board,"media/stop",json!({"sessionId":j.session}));
        if stopped(&j,&usb){*j.phase.lock().unwrap()="idle".into();}
        else{match result{Ok(())=>*j.phase.lock().unwrap()="completed".into(),Err(e)=>{*j.phase.lock().unwrap()="error".into();*j.error.lock().unwrap()=e;}}}
        let phase=j.phase.lock().unwrap().clone();
        report(&j.id,if phase=="idle"{"stopped"}else{&phase},&j.error.lock().unwrap());
        if j.speech_key.starts_with("news-"){let error=j.error.lock().unwrap().clone();let phase=j.phase.lock().unwrap().clone();crate::realtime_chat_log::record("","news_playback",json!({"stage":"audio_end","state":phase,"errorKind":if error.is_empty(){"none"}else{crate::realtime_chat_log::error_kind(&error)}}));crate::component_news::playback_ended(&j.id,&j.speech_key,&phase,&error);}
    }).map_err(|_|"无法启动组件音频")?;Ok(())
}
fn stopped(j:&Job,usb:&UsbSerialManager)->bool{j.cancel.load(Ordering::Acquire)||!usb.status().connected||usb.status().board_device_id!=j.board||crate::realtime_chat::status().active||j.heartbeat.lock().unwrap().elapsed()>Duration::from_secs(8)}
fn resample(bytes:&[u8])->Vec<u8>{let s:Vec<i16>=bytes.chunks_exact(2).map(|b|i16::from_le_bytes([b[0],b[1]])).collect();let mut out=Vec::with_capacity(s.len()*6);for(i,a)in s.iter().enumerate(){let b=s.get(i+1).unwrap_or(a);for part in 0..3{let v=(*a as i32+(i32::from(*b)-i32::from(*a))*part/3)as i16;out.extend_from_slice(&v.to_le_bytes());}}out}
struct Synth{kind:usize,rng:u32,low:f64,click:f64,at:u64}
impl Synth{
    fn new(kind:usize)->Self{Self{kind:kind%4,rng:0x4a77b115,low:0.0,click:0.0,at:0}}
    fn sample(&mut self)->f64{
        self.rng^=self.rng<<13;self.rng^=self.rng>>17;self.rng^=self.rng<<5;let noise=self.rng as f64/u32::MAX as f64*2.0-1.0;
        self.low=self.low*0.985+noise*0.015;let t=self.at as f64/48000.0;self.at+=1;
        let v=match self.kind{
            0=>noise*0.12+self.low*1.2,
            1=>(self.low*2.0+noise*0.025)*(0.45+0.4*(t*0.55).sin()),
            2=>{if self.rng%22000==0{self.click=0.4;}self.click*=0.997;self.low*1.1+self.click*noise},
            _=>self.low*0.5+(t*2.0).sin().max(0.0).powi(12)*(t*std::f64::consts::TAU*3100.0).sin()*0.08,
        };v.clamp(-0.7,0.7)
    }
    fn block(&mut self,count:usize,offset:usize,total:usize)->Vec<u8>{let mut b=Vec::with_capacity(count*2);for i in 0..count{let fade=((offset+i)as f64/96000.0).min(1.0)*((total-offset-i)as f64/144000.0).min(1.0);let v=(self.sample()*fade*32767.0)as i16;b.extend_from_slice(&v.to_le_bytes());}b}
}
#[cfg(test)]mod tests{use super::*;#[test]fn resampling_is_bounded_and_preserves_length(){let p=resample(&[0,0,255,127]);assert_eq!(p.len(),12);assert_eq!(&p[0..2],&[0,0]);assert_eq!(&p[10..12],&[255,127]);}
#[test]fn saved_draft_does_not_own_microphone(){
    assert!(capture_busy(false,false,false));assert!(capture_busy(true,false,false));
    assert!(!capture_busy(true,true,false));assert!(!capture_busy(false,false,true));
}
#[test]fn sounds_have_continuous_state_and_safe_peak(){for kind in 0..4{let mut a=Synth::new(kind);let mut b=Synth::new(kind);let joined=a.block(7680,0,100000);let mut split=b.block(3840,0,100000);split.extend(b.block(3840,3840,100000));assert_eq!(joined,split);assert!(joined.chunks_exact(2).all(|s|i16::from_le_bytes([s[0],s[1]]).unsigned_abs()<=22938));}}}
