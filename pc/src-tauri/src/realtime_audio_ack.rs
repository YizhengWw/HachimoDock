//! Bounded request ownership; terminal capacity is separate from ordinary ACKs.
use std::collections::BTreeMap;
pub(super) const MAX_PENDING: usize = 128;
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(super) enum Terminal { Flush, Disable }
impl Terminal {
    fn index(self)->usize { match self {Self::Flush=>0,Self::Disable=>1} }
    pub(super) fn topic(self)->&'static str {match self {Self::Flush=>"audio/play_flush",Self::Disable=>"audio/conversation"}}
}
#[derive(Clone,Copy,Debug,PartialEq,Eq)]
pub(super) enum Receipt { Unmatched, Retired, Current, Terminal(Terminal), Probe }
#[derive(Clone,Copy,Debug,Default,PartialEq,Eq)]
pub(super) enum Outcome { #[default] Unissued, Waiting, Confirmed, Rejected, Invalid, Unknown }
struct Request {owner:String,topic:String,session:String,playback:Option<(u64,Option<String>)>}
#[derive(Default)]
struct TerminalSlot {id:String,request:Option<Request>,received:bool,outcome:Outcome}
#[derive(Default)]
pub(super) struct Requests {
    next:u64,
    pending:BTreeMap<String,Request>,
    playback_epoch:u64,
    playback_turn:Option<String>,
    closing:bool,
    terminal:[TerminalSlot;2],
    probe_claimed:bool,
    probe_written:bool,
    probe_request:Option<Request>,
    probe_id:String,
    probe_received:bool,
    probe_outcome:Outcome,
    probe_status_seen:bool,
    idle_observed:bool,
}
pub(super) fn managed_topic(topic:&str)->bool {
    matches!(topic,"audio/conversation"|"audio/play_begin"|"audio/play_chunk"|"audio/play_end"|"audio/play_flush"|"audio/play_subtitle")
}
pub(super) fn scope_matches(expected:(&str,&str,&str),incoming:(Option<&str>,Option<&str>,Option<&str>,Option<&str>))->bool {
    incoming==(Some(expected.0),Some(expected.1),Some(expected.2),Some("esp-p4"))
}
impl Requests {
    pub(super) fn closing(&self)->bool { self.closing }
    fn id(&mut self,owner:&str)->Result<String,&'static str>{
        self.next=self.next.checked_add(1).ok_or("实时音频请求编号已耗尽")?;
        Ok(format!("{owner}-a{}",self.next))
    }
    /// Called synchronously at Player Begin/Flush, before a delayed ACK can be
    /// attributed to the replacement. Retired entries remain bounded records;
    /// they are not evicted or counted as successful/unissued requests.
    pub(super) fn playback(&mut self,turn:Option<&str>)->Result<(),&'static str>{
        self.playback_epoch=self.playback_epoch.checked_add(1).ok_or("播放轮次已耗尽")?;
        self.playback_turn=turn.map(str::to_owned);Ok(())
    }
    pub(super) fn issue(&mut self,owner:&str,topic:&str,session:&str)->Result<String,&'static str>{
        if owner.is_empty()||!managed_topic(topic)||self.closing||self.pending.len()>=MAX_PENDING {
            return Err("实时音频回执窗口不可用；未发送或重试指令");
        }
        let id=self.id(owner)?;
        let playback=if topic=="audio/conversation"{None}else{
            Some((self.playback_epoch,if topic=="audio/play_flush"{None}else{Some(session.into())}))
        };
        self.pending.insert(id.clone(),Request{owner:owner.into(),topic:topic.into(),session:session.into(),playback});Ok(id)
    }
    fn fields(request:&Request,owner:&str,topic:&str,session:Option<&str>,conversation:Option<&str>)->bool {
        owner==request.owner&&topic==request.topic
            &&session.is_none_or(|s|s==request.session)&&conversation.is_none_or(|s|s==request.owner)
    }
    pub(super) fn accept(&mut self,owner:&str,topic:&str,id:&str,session:Option<&str>,conversation:Option<&str>)->Receipt {
        for kind in [Terminal::Flush,Terminal::Disable] {
            let slot=&mut self.terminal[kind.index()];
            if slot.id==id&&!slot.received&&slot.request.as_ref().is_some_and(|r|Self::fields(r,owner,topic,session,conversation)) {
                slot.received=true;return Receipt::Terminal(kind);
            }
        }
        if self.probe_id==id&&!self.probe_received&&self.probe_request.as_ref().is_some_and(|r|Self::fields(r,owner,topic,session,conversation)){
            self.probe_received=true;return Receipt::Probe;
        }
        let Some(request)=self.pending.get(id) else{return Receipt::Unmatched;};
        if !Self::fields(request,owner,topic,session,conversation){return Receipt::Unmatched;}
        let retired=request.playback.as_ref().is_some_and(|(epoch,turn)|
            self.closing||*epoch!=self.playback_epoch||turn.as_ref().is_some_and(|t|self.playback_turn.as_ref()!=Some(t)));
        self.pending.remove(id);
        if retired{Receipt::Retired}else{Receipt::Current}
    }
    /// Reserve BOTH exactly-once termination attempts independently of the 128
    /// ordinary entries. No await, write, or user callback while this is locked.
    pub(super) fn begin_terminal(&mut self,owner:&str)->Result<Option<[(Terminal,String);2]>,&'static str>{
        if self.closing{return Ok(None);}self.closing=true;
        let mut ids=[String::new(),String::new()];
        for kind in [Terminal::Flush,Terminal::Disable] {
            let id=match self.id(owner){Ok(id)=>id,Err(error)=>{
                for slot in &mut self.terminal{slot.outcome=Outcome::Unknown;}return Err(error);
            }};
            ids[kind.index()]=id.clone();
            self.terminal[kind.index()]=TerminalSlot{id,request:Some(Request{owner:owner.into(),topic:kind.topic().into(),session:owner.into(),playback:None}),received:false,outcome:Outcome::Waiting};
        }
        Ok(Some([(Terminal::Flush,ids[0].clone()),(Terminal::Disable,ids[1].clone())]))
    }
    pub(super) fn terminal_write_unknown(&mut self,kind:Terminal){
        let slot=&mut self.terminal[kind.index()];if !slot.received{slot.outcome=Outcome::Unknown;}
    }
    pub(super) fn terminal_receipt(&mut self,kind:Terminal,valid:bool,ok:bool){
        let slot=&mut self.terminal[kind.index()];
        if slot.received&&matches!(slot.outcome,Outcome::Waiting|Outcome::Unknown){
            slot.outcome=if !valid{Outcome::Invalid}else if ok{Outcome::Confirmed}else{Outcome::Rejected};
        }
    }
    pub(super) fn terminal_acks(&self)->bool{self.closing&&self.terminal.iter().all(|s|s.outcome==Outcome::Confirmed)}
    pub(super) fn terminal_failed(&self)->bool{self.terminal.iter().any(|s|matches!(s.outcome,Outcome::Rejected|Outcome::Invalid))||matches!(self.probe_outcome,Outcome::Rejected|Outcome::Invalid)}
    /// A single fresh read-only status observation is requested only after both
    /// real ACKs. ACK acceptance alone is not physical microphone/speaker idle.
    pub(super) fn claim_probe(&mut self)->bool{
        if !self.terminal_acks()||self.probe_claimed{return false;}
        let owner=self.terminal[0].request.as_ref().unwrap().owner.clone();
        self.probe_id=format!("{owner}-stop-observe");
        self.probe_request=Some(Request{owner:owner.clone(),topic:"audio/query".into(),session:owner,playback:None});
        self.probe_outcome=Outcome::Waiting;self.probe_claimed=true;true
    }
    pub(super) fn probe_written(&mut self,ok:bool){self.probe_written=ok;}
    pub(super) fn observe_status(&mut self,idle:bool){if self.probe_claimed&&self.terminal_acks(){self.probe_status_seen=true;self.idle_observed=idle;}}
    pub(super) fn probe_receipt(&mut self,valid:bool,ok:bool){
        if self.probe_received&&self.probe_outcome==Outcome::Waiting{self.probe_outcome=if !valid{Outcome::Invalid}else if ok{Outcome::Confirmed}else{Outcome::Rejected};}
    }
    pub(super) fn stopped(&self)->bool{self.terminal_acks()&&self.probe_written&&self.probe_outcome==Outcome::Confirmed&&self.probe_status_seen&&self.idle_observed}
    pub(super) fn stop_state(&self)->&'static str{
        if self.stopped(){"acknowledged_idle_observed"}else if self.terminal_failed(){"rejected_or_invalid"}
        else if self.terminal_acks(){"acknowledged_idle_unconfirmed"}
        else if self.terminal.iter().any(|s|s.outcome==Outcome::Unknown){"write_unconfirmed"}else{"awaiting_ack"}
    }
}

#[cfg(test)]mod tests{
    use super::*;
    #[test]fn full_window_preserves_independent_once_terminal_capacity_and_never_fakes_idle(){
        let mut r=Requests::default();r.playback(Some("rtc-turn")).unwrap();
        for _ in 0..MAX_PENDING{r.issue("rtc","audio/play_chunk","rtc-turn").unwrap();}
        assert!(r.issue("rtc","audio/play_chunk","rtc-turn").is_err());
        let requests=r.begin_terminal("rtc").unwrap().unwrap();assert_eq!(r.pending.len(),MAX_PENDING);
        assert!(r.begin_terminal("rtc").unwrap().is_none());assert!(!r.stopped());
        for (kind,id) in requests{
            assert_eq!(r.accept("rtc",kind.topic(),&id,None,None),Receipt::Terminal(kind));
            r.terminal_receipt(kind,true,true);assert!(!r.stopped());
            assert_eq!(r.accept("rtc",kind.topic(),&id,None,None),Receipt::Unmatched);
        }
        assert!(r.claim_probe());assert!(!r.claim_probe());assert!(!r.stopped());
        r.observe_status(true);assert!(!r.stopped());r.probe_written(true);assert!(!r.stopped());
        let probe=r.probe_id.clone();assert_eq!(r.accept("rtc","audio/query",&probe,None,None),Receipt::Probe);r.probe_receipt(true,true);assert!(r.stopped());
        assert_eq!(r.pending.len(),MAX_PENDING); // No eviction/forged ordinary ACK.
    }
    #[test]fn unknown_write_can_only_resolve_from_actual_once_ack_not_another_send(){
        let mut r=Requests::default();let requests=r.begin_terminal("rtc").unwrap().unwrap();
        r.terminal_write_unknown(Terminal::Flush);assert_eq!(r.stop_state(),"write_unconfirmed");
        assert!(r.begin_terminal("rtc").unwrap().is_none());
        for (kind,id) in requests{
            assert_eq!(r.accept("rtc",kind.topic(),&id,None,None),Receipt::Terminal(kind));r.terminal_receipt(kind,true,true);
        }
        assert_eq!(r.stop_state(),"acknowledged_idle_unconfirmed");assert!(!r.stopped());
    }
    #[test]fn retired_turn_and_flush_epoch_drain_without_harming_new_turn(){
        let mut r=Requests::default();r.playback(Some("turn-a")).unwrap();
        let a=r.issue("rtc","audio/play_chunk","turn-a").unwrap();let control=r.issue("rtc","audio/conversation","rtc").unwrap();
        r.playback(None).unwrap();let flush=r.issue("rtc","audio/play_flush","turn-a").unwrap();
        r.playback(Some("turn-b")).unwrap();let b=r.issue("rtc","audio/play_chunk","turn-b").unwrap();
        assert_eq!(r.accept("rtc","audio/play_chunk",&a,None,None),Receipt::Retired);
        assert_eq!(r.accept("rtc","audio/play_flush",&flush,None,None),Receipt::Retired);
        assert_eq!(r.accept("rtc","audio/conversation",&control,None,None),Receipt::Current);
        assert_eq!(r.accept("rtc","audio/play_chunk",&b,None,None),Receipt::Current);
    }
    #[test]fn foreign_identity_never_consumes_and_current_malformed_terminal_is_not_success(){
        let mut r=Requests::default();r.playback(Some("turn")).unwrap();let id=r.issue("rtc","audio/play_chunk","turn").unwrap();
        for (owner,topic,key,session) in [("old","audio/play_chunk",id.as_str(),None),("rtc","audio/control",id.as_str(),None),("rtc","audio/play_chunk","unknown",None),("rtc","audio/play_chunk",id.as_str(),Some("other"))]{
            assert_eq!(r.accept(owner,topic,key,session,None),Receipt::Unmatched);
        }
        assert_eq!(r.accept("rtc","audio/play_chunk",&id,None,None),Receipt::Current);
        let requests=r.begin_terminal("rtc").unwrap().unwrap();let (kind,id)=&requests[0];
        assert_eq!(r.accept("rtc",kind.topic(),id,None,None),Receipt::Terminal(*kind));r.terminal_receipt(*kind,false,true);
        assert!(r.terminal_failed());assert!(!r.claim_probe());assert!(!r.stopped());
    }
    #[test]fn native_scope_is_exact(){
        let expected=("board","42","0123456789abcdef");
        assert!(scope_matches(expected,(Some("board"),Some("42"),Some("0123456789abcdef"),Some("esp-p4"))));
        for v in [(Some("other"),Some("42"),Some("0123456789abcdef"),Some("esp-p4")),(Some("board"),Some("41"),Some("0123456789abcdef"),Some("esp-p4")),(Some("board"),Some("42"),Some("old"),Some("esp-p4")),(Some("board"),Some("42"),None,Some("esp-p4"))]{assert!(!scope_matches(expected,v));}
    }
}
