//! Correlate read-only identity with a captured frame. No device enumeration,
//! deployment, automatic retries or authority inferred from these metadata.
use super::pull::Transport;
use super::integrity::Capture;
use serde::{Deserialize,Serialize};
use serde_json::{json,Value};
use std::time::Duration;
const BUDGET:Duration=Duration::from_millis(500);
fn token(s:&str,max:usize,build:bool)->bool{!s.is_empty()&&s.len()<=max&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"_.-".contains(&b)||(build&&b==b'+'))}
fn hex(s:&str,n:usize)->bool{s.len()==n&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))}
fn failure(code:&str)->Value{json!({"kind":"identity_evidence","code":code})}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub(super) struct Identity{
    engine:String,request_id:String,board_device_id:String,boot_id:String,build_id:String,
    git_sha:String,app_elf_sha256:String,build_dirty:bool,protocol_schema:u32,uptime_us:u64,
}
impl Identity{
    fn checked(value:Value,request:&str,board:&str)->Result<Self,Value>{
        let i:Self=serde_json::from_value(value).map_err(|_|failure("invalid_fields"))?;
        if i.engine!="p4-evidence-identity-v1"||!token(&i.request_id,64,false)||i.request_id!=request
            ||!token(&i.board_device_id,47,false)||i.board_device_id!=board||!hex(&i.boot_id,16)
            ||!token(&i.build_id,95,true)||!token(&i.git_sha,40,false)||!hex(&i.app_elf_sha256,64)
            ||i.protocol_schema==0||i.uptime_us>9_007_199_254_740_991{return Err(failure("identity_mismatch"));}
        Ok(i)
    }
}
pub(super) fn query(io:&mut impl Transport,request:&str,board:&str,mut capture:Option<&mut Capture>)->Result<Identity,Value>{
    if !token(request,64,false)||!token(board,47,false){return Err(failure("invalid_expectation"));}
    let start=io.elapsed();io.send("debug/identity",&json!({"requestId":request})).map_err(|_|failure("send_failed"))?;
    let mut events=0;let mut polls=0;
    loop{
        let elapsed=io.elapsed().checked_sub(start).ok_or_else(||failure("clock_regressed"))?;
        let remaining=BUDGET.checked_sub(elapsed).filter(|n|!n.is_zero()).ok_or_else(||failure("timeout"))?;
        if events>=64{return Err(failure("event_budget"));}
        if polls>=64{return Err(failure("poll_budget"));}polls+=1;
        if let Some((topic,payload))=io.receive(remaining.min(Duration::from_millis(20))).map_err(|_|failure("transport_closed"))?{
            events+=1;
            // Refuse an over-budget response even if it was already buffered.
            if io.elapsed().checked_sub(start).is_none_or(|n|n>=BUDGET){return Err(failure("timeout"));}
            if topic=="debug/identity"{return Identity::checked(payload,request,board);}
            // The post-capture identity query must not swallow a late error
            // from the final screenshot drain or callback shutdown.
            if let Some(capture)=capture.as_deref_mut(){capture.observe(&topic,payload);}
        }
    }
}
/// Separate fresh read-only observation after a failure, never a request retry.
/// Only boolean continuity goes into the failure checkpoint, no raw board ID.
pub(super) fn failure_continuity(before:&Identity,after:&Identity)->Value{
    json!({"freshReadOnlyObservation":true,"authentication":false,
        "sameBoard":before.board_device_id==after.board_device_id,
        "sameBoot":before.boot_id==after.boot_id,
        "sameBuild":before.build_id==after.build_id&&before.app_elf_sha256==after.app_elf_sha256,
        "uptimeNondecreasing":after.uptime_us>=before.uptime_us})
}
/// Expected ELF hash comes from the caller's trusted local build artifact, not
/// the device response. Missing provenance stays false, never guessed from a
/// version string or dirty Git SHA. The raw board ID is not copied to the report.
pub(super) fn bind(before:&Identity,after:&Identity,frame:&Value,expected_elf:Option<&str>)->Result<Value,Value>{
    if before.request_id==after.request_id||before.board_device_id!=after.board_device_id||before.boot_id!=after.boot_id
        ||before.build_id!=after.build_id||before.git_sha!=after.git_sha||before.app_elf_sha256!=after.app_elf_sha256
        ||before.build_dirty!=after.build_dirty||before.protocol_schema!=after.protocol_schema||after.uptime_us<=before.uptime_us
        ||frame["passed"]!=true||frame["begin"]["bootId"]!=before.boot_id{return Err(failure("capture_lifetime_mismatch"));}
    if expected_elf.is_some_and(|hash|!hex(hash,64)||hash!=before.app_elf_sha256){return Err(failure("build_artifact_mismatch"));}
    Ok(json!({"engine":"p4-evidence-binding-v1","boardMatched":true,"bootBound":true,"sourceBound":expected_elf.is_some(),
        "bootId":before.boot_id,"buildId":before.build_id,"gitSha":before.git_sha,"appElfSha256":before.app_elf_sha256,
        "buildDirty":before.build_dirty,"protocolSchema":before.protocol_schema,
        "fromUptimeUs":before.uptime_us,"toUptimeUs":after.uptime_us,"authentication":false}))
}

#[cfg(test)]
mod tests{
    use super::*;use std::collections::VecDeque;
    #[test]
    fn failure_context_distinguishes_reboot_from_same_boot_without_identity_output(){
        let before=Identity::checked(payload("first",100),"first","synthetic-board").unwrap();
        let mut after=Identity::checked(payload("second",150),"second","synthetic-board").unwrap();
        let same=failure_continuity(&before,&after);assert_eq!(same["sameBoot"],true);assert_eq!(same["sameBuild"],true);
        after.boot_id="0000000000000001".into();after.uptime_us=5;
        let reset=failure_continuity(&before,&after);assert_eq!(reset["sameBoot"],false);assert_eq!(reset["uptimeNondecreasing"],false);
        assert!(!reset.to_string().contains("synthetic-board")&&!reset.to_string().contains("0000000000000001"));
        after.app_elf_sha256="b".repeat(64);assert_eq!(failure_continuity(&before,&after)["sameBuild"],false);
    }
    fn payload(request:&str,uptime:u64)->Value{json!({"engine":"p4-evidence-identity-v1","requestId":request,"boardDeviceId":"synthetic-board",
        "bootId":"01020304aabbccdd","buildId":"0.7.103-p4-r27+abc-dirty","gitSha":"abc","appElfSha256":"a".repeat(64),"buildDirty":true,"protocolSchema":7,"uptimeUs":uptime})}
    struct Wire{now:Duration,messages:VecDeque<(String,Value)>,sent:usize}
    impl Transport for Wire{
        fn elapsed(&self)->Duration{self.now}
        fn send(&mut self,topic:&str,p:&Value)->Result<(),()>{assert_eq!(topic,"debug/identity");assert_eq!(p.as_object().unwrap().len(),1);self.sent+=1;Ok(())}
        fn receive(&mut self,timeout:Duration)->Result<Option<(String,Value)>,()>{self.now+=if self.messages.is_empty(){timeout}else{Duration::from_millis(1)};Ok(self.messages.pop_front())}
        fn delay(&mut self,_:Duration){panic!("identity must not sleep/retry submissions");}
    }
    #[test]fn identity_query_is_one_bounded_read_only_request(){
        let mut io=Wire{now:Duration::ZERO,messages:VecDeque::from([("audio/status".into(),json!({})),("debug/identity".into(),payload("before",1))]),sent:0};
        let i=query(&mut io,"before","synthetic-board",None).unwrap();assert_eq!(i.boot_id,"01020304aabbccdd");assert_eq!(io.sent,1);
        io.messages.clear();assert!(query(&mut io,"after","synthetic-board",None).is_err());assert_eq!(io.sent,2);assert!(io.now<=BUDGET+Duration::from_millis(2));
    }
    #[test]fn identity_fields_requests_and_unknown_extensions_fail_closed_without_echo(){
        for (name,value) in [("bootId",json!("SECRET/path")),("appElfSha256",json!("SECRET/token")),("requestId",json!("different")),("boardDeviceId",json!("other")),("uptimeUs",json!(1.5)),("extra",json!("SECRET/extra"))]{
            let mut p=payload("before",1);p[name]=value;let error=Identity::checked(p,"before","synthetic-board").unwrap_err();assert!(!error.to_string().contains("SECRET"));
        }
        let mut io=Wire{now:Duration::ZERO,messages:(0..65).map(|_|("noise".into(),json!({}))).collect(),sent:0};
        assert_eq!(query(&mut io,"before","synthetic-board",None).unwrap_err()["code"],"event_budget");assert_eq!(io.sent,1);
    }
    #[test]fn frame_binding_requires_same_boot_build_strict_time_and_explicit_source(){
        let before=Identity::checked(payload("before",10),"before","synthetic-board").unwrap();
        let after=Identity::checked(payload("after",20),"after","synthetic-board").unwrap();let frame=json!({"passed":true,"begin":{"bootId":before.boot_id}});
        let report=bind(&before,&after,&frame,None).unwrap();assert_eq!(report["sourceBound"],false);assert!(!report.to_string().contains("synthetic-board"));
        assert_eq!(bind(&before,&after,&frame,Some(&"a".repeat(64))).unwrap()["sourceBound"],true);
        assert!(bind(&before,&after,&frame,Some(&"b".repeat(64))).is_err());assert!(bind(&before,&before,&frame,None).is_err());
        for p in [payload("after",9),{let mut p=payload("after",20);p["bootId"]=json!("02020304aabbccdd");p},{let mut p=payload("after",20);p["appElfSha256"]=json!("b".repeat(64));p}]{
            let other=Identity::checked(p,"after","synthetic-board").unwrap();assert!(bind(&before,&other,&frame,None).is_err());
        }
        assert!(bind(&before,&after,&json!({"passed":true,"begin":{}}),None).is_err());assert!(bind(&before,&after,&json!({"passed":false,"begin":{"bootId":before.boot_id}}),None).is_err());
    }
    #[test]fn post_capture_identity_query_preserves_late_frame_errors(){
        let mut io=Wire{now:Duration::ZERO,messages:VecDeque::from([("debug/screenshot_error".into(),json!({"error":"transport or encode failed"})),("debug/identity".into(),payload("after",20))]),sent:0};
        let mut capture=Capture::new(2,1);query(&mut io,"after","synthetic-board",Some(&mut capture)).unwrap();
        assert_eq!(capture.report()["errors"][0]["kind"],"device_error");assert_eq!(capture.report()["errors"][0]["code"],"transport_or_encode_failed");
    }
    #[test]fn broken_nonblocking_transport_cannot_create_an_unbounded_loop(){
        struct Frozen{reads:usize}
        impl Transport for Frozen{
            fn elapsed(&self)->Duration{Duration::ZERO}
            fn send(&mut self,_:&str,_:&Value)->Result<(),()>{Ok(())}
            fn receive(&mut self,_:Duration)->Result<Option<(String,Value)>,()>{self.reads+=1;Ok(None)}
            fn delay(&mut self,_:Duration){}
        }
        let mut io=Frozen{reads:0};assert_eq!(query(&mut io,"before","synthetic-board",None).unwrap_err()["code"],"poll_budget");assert_eq!(io.reads,64);
    }
}
