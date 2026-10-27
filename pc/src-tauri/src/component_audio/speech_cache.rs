//! Opt-in news PCM cache. No credentials or PCM in diagnostics. Preparation
//! never owns a USB/media session and yields to foreground voice/playback.
use super::*;
use sha2::{Digest,Sha256};
use std::{path::{Path,PathBuf},io::{Read,Write}};
const MAX_PCM:usize=1_920_000;
const MAX_DISK:u64=128*1024*1024;
static ROOT:OnceLock<PathBuf>=OnceLock::new();
#[derive(Default)]struct Warm{key:String,running:bool,next:Option<Instant>}
static WARM:Mutex<Warm>=Mutex::new(Warm{key:String::new(),running:false,next:None});
pub(super) fn configure(root:PathBuf){let _=ROOT.set(root);}
fn voice_key(cfg:&crate::doubao_tts::DoubaoTtsConfig)->String{
    // Credential rotation must not invalidate identical public voice audio.
    format!("news-pcm16-v1|{}|{}|{}|{}|{}",cfg.ws_url,cfg.resource_id,cfg.model,cfg.speaker,cfg.speed.to_bits())
}
fn key(cfg:&crate::doubao_tts::DoubaoTtsConfig,text:&str)->String{format!("{:x}",Sha256::digest(format!("{}|{text}",voice_key(cfg))))}
fn load_at(root:&Path,key:&str)->Option<Vec<u8>>{
    let path=root.join(format!("{key}.pcm"));let mut raw=Vec::new();
    std::fs::File::open(path).ok()?.take((MAX_PCM+41)as u64).read_to_end(&mut raw).ok()?;
    if raw.len()<42||raw.len()>MAX_PCM+40||&raw[..8]!=b"NEWSPCM1"||(raw.len()-40)%2!=0{return None;}
    if Sha256::digest(&raw[40..]).as_slice()!=&raw[8..40]{return None;}
    Some(raw.split_off(40))
}
fn store_at(root:&Path,key:&str,pcm:&[u8])->Result<(),String>{
    if pcm.is_empty()||pcm.len()>MAX_PCM||pcm.len()%2!=0{return Err("资讯语音长度无效".into());}
    std::fs::create_dir_all(root).map_err(|_|"无法创建资讯语音缓存")?;
    let mut file=tempfile::NamedTempFile::new_in(root).map_err(|_|"无法准备资讯语音缓存")?;
    file.write_all(b"NEWSPCM1").and_then(|_|file.write_all(&Sha256::digest(pcm))).and_then(|_|file.write_all(pcm)).and_then(|_|file.as_file().sync_all()).map_err(|_|"无法写入资讯语音缓存")?;
    file.persist(root.join(format!("{key}.pcm"))).map_err(|_|"无法保存资讯语音缓存")?;
    prune(root);Ok(())
}
fn prune(root:&Path){
    let Ok(entries)=std::fs::read_dir(root)else{return;};
    let mut files:Vec<_>=entries.flatten().filter_map(|e|{
        let name=e.file_name();let name=name.to_str()?;let hash=name.strip_suffix(".pcm")?;
        if hash.len()!=64||!hash.bytes().all(|b|b.is_ascii_hexdigit()){return None;}
        let m=e.metadata().ok()?;if !m.is_file()||e.file_type().ok()?.is_symlink(){return None;}
        Some((e.path(),m.len(),m.modified().ok()?))
    }).collect();
    files.sort_by_key(|f|f.2);let mut total:u64=files.iter().map(|f|f.1).sum();
    for(path,size,at)in files{if total<=MAX_DISK&&at.elapsed().map(|d|d.as_secs()<7*86400).unwrap_or(true){continue;}
        if std::fs::remove_file(path).is_ok(){total=total.saturating_sub(size);}}
}
pub(super) fn load(cfg:&crate::doubao_tts::DoubaoTtsConfig,text:&str)->Option<Vec<u8>>{load_at(ROOT.get()?,&key(cfg,text))}
pub(super) fn save(cfg:&crate::doubao_tts::DoubaoTtsConfig,text:&str,pcm:&[u8]){if let Some(root)=ROOT.get(){let _=store_at(root,&key(cfg,text),pcm);}}
fn busy()->bool{super::voice_busy()||super::active_for_exit()}
pub(super) fn prepare(id:&str,board:&str,cfg:crate::doubao_tts::DoubaoTtsConfig,items:Vec<SpeechItem>){
    if ROOT.get().is_none()||busy(){return;}
    let signature=format!("{:x}",Sha256::digest(format!("{}|{}|{:?}",id,voice_key(&cfg),items)));
    let Ok(mut state)=WARM.lock()else{return;};
    if state.running||state.key==signature&&state.next.is_some_and(|at|Instant::now()<at){return;}
    state.key=signature.clone();state.running=true;drop(state);
    let board=board.to_owned();let id=id.to_owned();
    let spawn=std::thread::Builder::new().name("news-voice-cache".into()).spawn(move||{
        let interrupted=||busy()||!crate::component_news::background_enabled(&id)||crate::component_voice::preparation_config(&board).map(|current|voice_key(&current)!=voice_key(&cfg)).unwrap_or(true);
        let mut client=crate::doubao_tts::DoubaoTtsClient::new(cfg.clone());
        let mut hits=0usize;let mut made=0usize;
        let result=(||->Result<(),String>{
            for item in items{for part in crate::knowledge_narration::speech_parts("",&item.text){
                if interrupted(){return Err("foreground_or_voice_changed".into());}
                if load(&cfg,&part).is_some(){hits+=1;continue;}
                let mut pcm=Vec::new();let mut overflow=false;
                tauri::async_runtime::block_on(async{
                    let future=client.synthesize(&part,|chunk|{if pcm.len()+chunk.len()<=MAX_PCM{pcm.extend_from_slice(chunk);}else{overflow=true;}});
                    tokio::select!{
                        result=tokio::time::timeout(Duration::from_secs(35),future)=>result.map_err(|_|"tts_timeout".to_string())?.map(|_|()).map_err(|_|"tts_failed".to_string()),
                        _=async{while !interrupted(){tokio::time::sleep(Duration::from_millis(100)).await;}}=>Err("foreground_or_voice_changed".into())
                    }
                })?;
                if overflow||pcm.is_empty(){return Err("tts_invalid_length".into());}
                store_at(ROOT.get().unwrap(),&key(&cfg,&part),&pcm)?;made+=1;
            }}Ok(())
        })();
        tauri::async_runtime::block_on(client.reset());
        crate::realtime_chat_log::record("","news_voice_cache",json!({"ok":result.is_ok(),"hits":hits,"prepared":made,"componentId":id,"reason":result.as_ref().err()}));
        if let Ok(mut state)=WARM.lock(){state.running=false;state.next=Some(Instant::now()+Duration::from_secs(if result.is_ok(){3600}else if matches!(&result,Err(e) if e=="foreground_or_voice_changed"){15}else{900}));}
    });
    if spawn.is_err(){if let Ok(mut state)=WARM.lock(){state.running=false;state.next=Some(Instant::now()+Duration::from_secs(60));}}
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn completed_voice_draft_does_not_block_news_warmup(){
        let source=include_str!("speech_cache.rs");let body=source.split("fn busy()->bool{").nth(1).unwrap().split('}').next().unwrap();
        assert!(body.contains("super::voice_busy()"));assert!(!body.contains("active_device_voice_context"));
        assert!(!super::super::capture_busy(true,true,false));
        assert!(super::super::capture_busy(false,false,false));
    }
    #[test]
    #[ignore="Explicit opt-in: public-news TTS preparation, no USB/audio/microphone"]
    fn live_news_voice_cache_prepare(){
        assert_eq!(std::env::var("PET_NEWS_CACHE_TEST").as_deref(),Ok("1"));
        let settings=PathBuf::from(std::env::var("P4_SERVICE_CONFIG").unwrap());
        crate::voice_chat_settings::configure_storage_dir(settings.clone()).unwrap();
        crate::volcengine_asr::configure_storage_dir(settings.clone()).unwrap();
        crate::llm_network::configure_storage_dir(settings.clone()).unwrap();
        let output=PathBuf::from(std::env::var("PET_NEWS_CACHE_DIR").unwrap());
        configure(output.clone());let content_root=output.join("state");std::fs::create_dir_all(content_root.join("daily-news")).unwrap();crate::component_news::configure_cache(content_root);
        let persona=crate::component_voice::acceptance_persona(&settings,"builtin-terrier");
        let v=&persona["voice"];
        let voice=json!({"boardDeviceId":"cache-test-only","appearanceId":"builtin-terrier","speaker":v["speaker"],"cloneSpeakerId":v["clone_speaker_id"].as_str().unwrap_or(""),"speed":v["speed"].as_f64().unwrap_or(1.0)});
        let d:Value=serde_json::from_str(include_str!("../../../builtin-clawpkgs/daily-news/pc/application.json")).unwrap();
        crate::component_news::seed_verified_acceptance_report("daily-news",&PathBuf::from(std::env::var("PET_NEWS_REPORT").unwrap()),d["content"].as_array().unwrap());
        crate::component_voice::component_speech_set_pet_voice(Some(serde_json::from_value(voice.clone()).unwrap())).unwrap();
        let cfg=crate::component_voice::config("cache-test-only").unwrap();
        let declaration=serde_json::from_value(d).unwrap();
        let view=crate::component_news::request_with_surface("daily-news",&declaration,"status",None,false,&Value::Null).unwrap();
        let mut parts=Vec::new();
        for item in view["knowledge"]["playlist"].as_array().unwrap(){let joined=crate::knowledge_narration::speech_parts(item["title"].as_str().unwrap(),item["text"].as_str().unwrap()).concat();parts.extend(crate::knowledge_narration::speech_parts("",&joined));}
        let started=Instant::now();crate::component_news::prepare_audio("cache-test-only");
        loop{
            crate::component_voice::component_speech_set_pet_voice(Some(serde_json::from_value(voice.clone()).unwrap())).unwrap();
            if !WARM.lock().unwrap().running{break;}
            assert!(started.elapsed()<Duration::from_secs(300),"bounded warmup deadline");std::thread::sleep(Duration::from_millis(300));
        }
        let read_started=Instant::now();let hits=parts.iter().filter(|p|load(&cfg,p).is_some()).count();
        assert_eq!(hits,parts.len(),"all nine stories must be completely cached");assert!(hits>=9);
        assert!(!super::super::active_for_exit());
        std::fs::write(output.join("acceptance.json"),serde_json::to_vec_pretty(&json!({"items":9,"parts":hits,"preparedMs":started.elapsed().as_millis(),"cacheReadMs":read_started.elapsed().as_millis(),"usbSession":false,"microphone":false,"playback":false})).unwrap()).unwrap();
    }
    fn cfg()->crate::doubao_tts::DoubaoTtsConfig{crate::doubao_tts::DoubaoTtsConfig::new("test-secret","speaker","",1.0,"")}
    #[test]fn cache_keys_follow_text_voice_rate_model_but_never_credentials(){let a=cfg();let k=key(&a,"公开资讯");let mut b=a.clone();b.api_key="other".into();assert_eq!(k,key(&b,"公开资讯"));b.speed=1.1;assert_ne!(k,key(&b,"公开资讯"));b=a.clone();b.speaker="different".into();assert_ne!(k,key(&b,"公开资讯"));b=a.clone();b.model="new-model".into();assert_ne!(k,key(&b,"公开资讯"));assert_ne!(k,key(&a,"另一条"));}
    #[test]fn pcm_is_atomic_bounded_and_corruption_never_plays(){let temp=tempfile::tempdir().unwrap();let k=key(&cfg(),"资讯");let pcm=vec![0,0,1,0];store_at(temp.path(),&k,&pcm).unwrap();assert_eq!(load_at(temp.path(),&k),Some(pcm.clone()));assert!(store_at(temp.path(),&k,&[1]).is_err());assert_eq!(load_at(temp.path(),&k),Some(pcm));std::fs::write(temp.path().join(format!("{k}.pcm")),b"NEWSPCM1bad").unwrap();assert!(load_at(temp.path(),&k).is_none());}
}
