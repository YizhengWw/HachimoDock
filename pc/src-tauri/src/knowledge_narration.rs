//! Ephemeral, device-entry-only narration. Polling never restarts a finished,
//! cancelled or failed article. Speech follows punctuation, not screen pages.
use serde_json::Value;

pub(crate) fn speech_parts(title:&str,text:&str)->Vec<String>{
    let punctuation=if title.is_empty()||title.ends_with(['。','！','？','!','?']){""}else{"。"};
    let content=format!("{title}{punctuation}{text}");
    let chars:Vec<char>=content.chars().collect();let mut parts=Vec::new();let mut at=0;
    while at<chars.len(){
        // Bounded for the existing PCM/TTS limit, including slow pet voices.
        let limit=(at+90).min(chars.len());
        let end=if limit==chars.len(){limit}else{
            let boundary=|marks:&[char]|(at..limit).rev().find(|&i|marks.contains(&chars[i])).map(|i|i+1);
            boundary(&['。','！','？','!','?','；',';']).filter(|i|*i>=at+24)
                .or_else(||boundary(&['，',',','：',':'])).unwrap_or(limit)
        };
        parts.push(chars[at..end].iter().collect());at=end;
    }
    parts
}

#[derive(Default)]
pub struct Narration {
    epoch:u64,
    article:String,
    parts:Vec<String>,
    next:usize,
    waiting:bool,
    finished:bool,
    completed:bool,
}
impl Narration {
    // Only a rejected, not-yet-started request can be deferred. Never revive an
    // explicitly stopped or already interrupted reading.
    pub fn defer_start(&mut self){if !self.finished{self.waiting=false;}}
    pub fn stop(&mut self){self.finished=true;self.waiting=false;self.completed=false;}
    pub fn completed(&self)->bool{self.completed}
    /// Start the full bounded reading queue once. The audio service performs
    /// cancellable prefetch; no new TTS/session is started by each status poll.
    pub fn next_source(&mut self,epoch:u64,result:&Value,phase:&str)->Option<crate::component_audio::Source>{
        if result["enabled"]!=true||result["permissions"]["autoRead"]!=true{self.stop();return None;}
        let info=&result["knowledge"];let card=&info["current"];let id=card["id"].as_str()?;
        if epoch!=self.epoch||id!=self.article{
            self.epoch=epoch;self.article=id.into();self.waiting=false;self.finished=false;self.completed=false;
        }
        if info["choosing"]==true{self.stop();}
        if self.finished{return None;}
        if self.waiting{
            match phase{"completed"=>{self.stop();self.completed=true;},"idle"|"error"=>self.stop(),_=>{}}
            return None;
        }
        let cards=info["playlist"].as_array().cloned().unwrap_or_else(||vec![card.clone()]);
        let mut items=Vec::new();
        for c in cards{
            items.push(crate::component_audio::SpeechItem{id:c["id"].as_str()?.into(),text:speech_parts(c["title"].as_str().unwrap_or(""),c["text"].as_str()?).concat()});
        }
        self.waiting=true;
        Some(crate::component_audio::Source::SpeechQueue{key:id.into(),items})
    }
    pub fn next(&mut self,epoch:u64,result:&Value,phase:&str)->Option<String>{
        if result["enabled"]!=true||result["permissions"]["autoRead"]!=true{
            self.stop();return None;
        }
        let info=&result["knowledge"];let card=&info["current"];
        let id=card["id"].as_str()?;
        if epoch!=self.epoch||id!=self.article{
            self.epoch=epoch;self.article=id.into();self.next=0;
            self.waiting=false;self.finished=false;self.completed=false;
            self.parts=speech_parts(card["title"].as_str().unwrap_or(""),card["text"].as_str()?);
        }
        // Entering the picker stops this narration; cancelling it doesn't
        // silently restart. Confirming another article or re-entry starts anew.
        if info["choosing"]==true{self.stop();}
        if self.finished{return None;}
        if self.waiting{
            match phase{
                "completed"=>self.waiting=false,
                "idle"|"error"=>{self.stop();return None;},
                _=>return None,
            }
        }
        if let Some(text)=self.parts.get(self.next).cloned(){
            self.next+=1;self.waiting=true;Some(text)
        }else{self.stop();self.completed=true;None}
    }
}

#[cfg(test)]mod tests{
    use super::*;use serde_json::json;
    fn result()->Value{json!({"enabled":true,"permissions":{"autoRead":true},"knowledge":{"choosing":false,"current":{"id":"a","title":"标题","text":"这是中文正文。".repeat(80)}}})}
    #[test]fn deferred_start_does_not_consume_entry_but_explicit_stop_is_final(){
        let mut n=Narration::default();let r=result();
        assert!(n.next_source(1,&r,"idle").is_some());n.defer_start();
        assert!(n.next_source(1,&r,"idle").is_some());
        n.stop();n.defer_start();assert!(n.next_source(1,&r,"idle").is_none());
        assert!(n.next_source(2,&r,"idle").is_some());
    }
    #[test]fn continuous_queue_starts_once_and_cancelled_batches_do_not_restart(){
        let mut n=Narration::default();let mut r=result();
        r["knowledge"]["playlist"]=json!([{"id":"0","title":"第一条","text":"正文一。"},{"id":"1","title":"第二条","text":"正文二。"}]);
        let Some(crate::component_audio::Source::SpeechQueue{items,..})=n.next_source(1,&r,"idle")else{panic!("missing queue")};
        assert_eq!(items.len(),2);assert_eq!(items[1].text,"第二条。正文二。");
        for phase in ["preparing","buffering","playing"]{assert!(n.next_source(1,&r,phase).is_none());}
        assert!(n.next_source(1,&r,"completed").is_none());assert!(n.completed());
        assert!(n.next_source(1,&r,"completed").is_none());
        assert!(n.next_source(2,&r,"idle").is_some());
        r["permissions"]["autoRead"]=false.into();assert!(n.next_source(2,&r,"playing").is_none());
        r["permissions"]["autoRead"]=true.into();assert!(n.next_source(2,&r,"completed").is_none());
        r["knowledge"]["current"]["id"]="b".into();assert!(n.next_source(2,&r,"idle").is_some());
        assert!(n.next_source(2,&r,"error").is_none());assert!(n.next_source(2,&r,"idle").is_none());
    }
    #[test]fn reads_whole_article_once_and_only_advances_on_completion(){
        let r=result();let mut n=Narration::default();let mut text=n.next(1,&r,"idle").unwrap();
        for phase in ["preparing","buffering","playing"]{assert!(n.next(1,&r,phase).is_none());}
        while let Some(part)=n.next(1,&r,"completed"){text+=&part;}
        assert_eq!(text,format!("标题。{}",r["knowledge"]["current"]["text"].as_str().unwrap()));
        for _ in 0..10{assert!(n.next(1,&r,"completed").is_none());}
        assert!(n.next(2,&r,"idle").is_some());
    }
    #[test]fn cancel_error_picker_and_disabled_do_not_restart_from_polling(){
        for phase in ["idle","error"]{
            let mut n=Narration::default();let r=result();assert!(n.next(1,&r,"idle").is_some());
            assert!(n.next(1,&r,phase).is_none());assert!(n.next(1,&r,"completed").is_none());
        }
        let mut n=Narration::default();let mut r=result();n.next(1,&r,"idle");
        r["knowledge"]["choosing"]=true.into();assert!(n.next(1,&r,"playing").is_none());
        r["knowledge"]["choosing"]=false.into();assert!(n.next(1,&r,"completed").is_none());
        r["knowledge"]["current"]["id"]="b".into();assert!(n.next(1,&r,"idle").is_some());
        r["enabled"]=false.into();assert!(n.next(1,&r,"completed").is_none());
        r["enabled"]=true.into();assert!(n.next(1,&r,"completed").is_none());
    }
    #[test]fn narration_prefers_sentence_boundaries_and_does_not_duplicate_question_mark(){
        let text="车停了，身体为什么还想向前？".repeat(40);
        let parts=speech_parts("惯性是什么？",&text);
        assert_eq!(parts.concat(),format!("惯性是什么？{text}"));
        assert!(parts.iter().all(|s|s.chars().count()<=90));
        assert!(parts.iter().all(|s|s.ends_with('？')));
        assert!(speech_parts("长句",&"字".repeat(400)).iter().all(|s|s.chars().count()<=90));
    }
}
