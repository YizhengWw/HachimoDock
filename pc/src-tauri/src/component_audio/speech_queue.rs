//! Bounded, cancellable speech prefetch with one USB media session.
//! Progress follows played samples (sent minus device buffer), not TTS requests.
use super::*;
use std::{collections::{HashSet,VecDeque},sync::mpsc};

#[derive(Clone,Debug)]
pub struct SpeechItem{pub id:String,pub text:String}
pub(super) fn validate(key:&str,items:&[SpeechItem])->Result<(),String>{
    let mut ids=HashSet::new();
    if key.is_empty()||key.len()>160||items.is_empty()||items.len()>32
        ||items.iter().map(|i|i.text.chars().count()).sum::<usize>()>16000
        ||items.iter().any(|i|i.id.is_empty()||i.id.len()>160||!ids.insert(&i.id)||i.text.trim().is_empty()){
        return Err("连续朗读队列超出有效范围".into());
    }Ok(())
}
struct Prepared{id:String,offset:u32,characters:u32,pcm:Vec<u8>}
#[derive(Debug)]
struct Mark{id:String,start:u64,end:u64,offset:u32,characters:u32}
fn audible(marks:&mut VecDeque<Mark>,played:u64)->Option<(String,u32)>{
    while marks.len()>1&&played>=marks[1].start{marks.pop_front();}
    let m=marks.front()?;if played<m.start{return None;}
    let fraction=played.saturating_sub(m.start).min(m.end-m.start);
    Some((m.id.clone(),m.offset+(fraction*m.characters as u64/(m.end-m.start).max(1))as u32))
}
pub(super) fn play(j:&Arc<Job>,usb:&UsbSerialManager,cfg:crate::doubao_tts::DoubaoTtsConfig,items:Vec<SpeechItem>)->Result<(),String>{
    // At most two completed segments plus the currently playing segment.
    let (tx,rx)=mpsc::sync_channel::<Result<Prepared,String>>(2);
    let producer_job=j.clone();let producer_usb=usb.clone();
    std::thread::Builder::new().name("component-speech-prefetch".into()).spawn(move||{
        let mut client=crate::doubao_tts::DoubaoTtsClient::new(cfg.clone());
        let cached=producer_job.speech_key.starts_with("news-");
        'items:for item in items{
            let mut offset=0;
            for part in crate::knowledge_narration::speech_parts("",&item.text){
                if stopped(&producer_job,&producer_usb){break 'items;}
                let saved=if cached{super::speech_cache::load(&cfg,&part)}else{None};
                #[cfg(test)]
                if cached&&saved.is_none()&&std::env::var("PET_NEWS_CACHE_ONLY").as_deref()==Ok("1"){
                    let _=tx.send(Err("cache-only acceptance encountered a cache miss".into()));break 'items;
                }
                let hit=saved.is_some();let mut pcm=saved.unwrap_or_default();let mut overflow=false;
                let result=if hit{Ok(())}else{tauri::async_runtime::block_on(async{
                    let future=client.synthesize(&part,|chunk|{if pcm.len()+chunk.len()<=1_920_000{pcm.extend_from_slice(chunk);}else{overflow=true;}});
                    tokio::select!{
                        result=tokio::time::timeout(Duration::from_secs(35),future)=>result.map_err(|_|"朗读生成超时".to_string()).and_then(|v|v.map(|_|()).map_err(|_|"朗读服务失败，请检查语音配置".into())),
                        _=async{while !stopped(&producer_job,&producer_usb){tokio::time::sleep(Duration::from_millis(100)).await;}}=>Err("朗读已取消".into()),
                    }
                })};
                if cached&&result.is_ok()&&!hit&&!overflow{super::speech_cache::save(&cfg,&part,&pcm);}
                let result=result.and_then(|_|if overflow{Err("朗读内容超过音频长度上限".into())}else if pcm.is_empty(){Err("没有生成可播放的语音".into())}else{Ok(Prepared{id:item.id.clone(),offset,characters:part.chars().count()as u32,pcm})});
                let failed=result.is_err();
                if tx.send(result).is_err()||failed{break 'items;}
                offset+=part.chars().count()as u32;
            }
        }
        tauri::async_runtime::block_on(client.reset());
    }).map_err(|_|"无法准备连续朗读")?;
    let mut begun=false;let mut seq=0;let mut sent=0u64;let mut buffered=0u64;let mut marks=VecDeque::new();
    let mut buffer_at=Instant::now();let mut waiting_since=None;
    let progress=|marks:&mut VecDeque<Mark>,sent:u64,buffered:u64|{
        if let Some((id,offset))=audible(marks,sent.saturating_sub(buffered*48)){
            *j.speech_item.lock().unwrap()=id;j.speech_offset.store(offset,Ordering::Release);
        }
    };
    while !stopped(j,usb){
        let part=match rx.recv_timeout(Duration::from_millis(40)){
            Ok(v)=>v?,Err(mpsc::RecvTimeoutError::Disconnected)=>break,
            Err(mpsc::RecvTimeoutError::Timeout)=>{
                if begun{
                    let remaining=buffered.saturating_sub(buffer_at.elapsed().as_millis()as u64);
                    progress(&mut marks,sent,remaining);
                    if remaining==0{waiting_since.get_or_insert_with(Instant::now);*j.phase.lock().unwrap()="buffering".into();}
                }
                continue;
            }
        };
        if let Some(at)=waiting_since.take(){j.speech_gap_ms.fetch_max(at.elapsed().as_millis().min(u32::MAX as u128)as u32,Ordering::AcqRel);}
        if stopped(j,usb){break;}
        if !begun{
            usb.media_request(&j.board,"media/begin",json!({"source":format!("app.{}",j.id),"mode":"live","sessionId":j.session,"format":"ima-block-v1","sampleRate":48000,"durationMs":0,"offsetMs":0,"volume":100,"title":"组件连续朗读","artist":"HachimoDock","queue":[],"keys":[]})).map_err(|_|"设备未接受连续朗读")?;
            begun=true;*j.phase.lock().unwrap()="buffering".into();
            super::report(&j.id,"buffering","");
            if j.speech_key.starts_with("news-"){crate::realtime_chat_log::record("","news_playback",json!({"stage":"device_accepted","ok":true}));}
        }
        let pcm=resample(&part.pcm);let samples=pcm.len()/2;
        marks.push_back(Mark{id:part.id,start:sent,end:sent+samples as u64,offset:part.offset,characters:part.characters});
        let mut offset=0;
        while offset<samples&&!stopped(j,usb){
            if buffered>560{
                std::thread::sleep(Duration::from_millis(40));
                let ack=usb.media_request(&j.board,"media/query",json!({"sessionId":j.session})).map_err(|_|"连续朗读会话已结束")?;
                buffered=ack["bufferedMs"].as_u64().unwrap_or(0);buffer_at=Instant::now();progress(&mut marks,sent,buffered);continue;
            }
            let count=(samples-offset).min(3840);let mut block=pcm[offset*2..(offset+count)*2].to_vec();
            let gain=j.volume.load(Ordering::Acquire)as f32/100.0;
            for b in block.chunks_exact_mut(2){let v=(i16::from_le_bytes([b[0],b[1]])as f32*gain)as i16;b.copy_from_slice(&v.to_le_bytes());}
            let ack=usb.media_request(&j.board,"media/chunk",json!({"sessionId":j.session,"seq":seq,"data":base64::engine::general_purpose::STANDARD.encode(crate::media_codec::encode(&block)?)})).map_err(|_|"设备音频传输中断，已停止连续朗读")?;
            seq+=1;offset+=count;sent+=count as u64;buffered=ack["bufferedMs"].as_u64().unwrap_or(0);buffer_at=Instant::now();
            if ack["state"]=="playing"{*j.phase.lock().unwrap()="playing".into();super::report(&j.id,"playing","");}
            progress(&mut marks,sent,buffered);
        }
    }
    if begun&&!stopped(j,usb){
        usb.media_request(&j.board,"media/end",json!({"sessionId":j.session})).map_err(|_|"连续朗读结束回执丢失")?;
        let deadline=Instant::now()+Duration::from_secs(3);
        let mut drained=false;
        while !stopped(j,usb)&&Instant::now()<deadline{
            let ack=usb.media_request(&j.board,"media/query",json!({"sessionId":j.session})).map_err(|_|"无法确认连续朗读结束")?;
            let remaining=ack["bufferedMs"].as_u64().unwrap_or(0);progress(&mut marks,sent,remaining);
            if remaining==0{drained=true;break;}
            std::thread::sleep(Duration::from_millis(40));
        }
        if !drained&&!stopped(j,usb){return Err("设备朗读尚未排空，未确认播放完成".into());}
    }
    Ok(())
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn progress_follows_audible_samples_not_prefetched_items(){
        let mut marks=VecDeque::from([Mark{id:"0".into(),start:0,end:48000,offset:0,characters:80},Mark{id:"1".into(),start:48000,end:96000,offset:0,characters:100}]);
        assert_eq!(audible(&mut marks,24000),Some(("0".into(),40)));
        assert_eq!(audible(&mut marks,47999).unwrap().0,"0");
        assert_eq!(audible(&mut marks,48000),Some(("1".into(),0)));
        assert_eq!(audible(&mut marks,96000),Some(("1".into(),100)));
    }
    #[test]fn queues_are_bounded_and_have_unique_item_identity(){
        let item=SpeechItem{id:"a".into(),text:"公开知识。".into()};
        assert!(validate("batch",&[item.clone()]).is_ok());
        assert!(validate("batch",&[item.clone(),item]).is_err());
        assert!(validate("batch",&[SpeechItem{id:"a".into(),text:"字".repeat(16001)}]).is_err());
        assert!(validate("",&[]).is_err());
    }
}
