use super::*;
use std::sync::{Mutex,atomic::{AtomicBool,AtomicU64,Ordering}};
struct Fake {value:Mutex<Value>,revision:AtomicU64,calls:AtomicU64,unknown:AtomicBool,unavailable:AtomicBool}
impl Fake {
 fn new()->Self{Self{value:Mutex::new(json!({"title":"Source","volume":20,"positionMs":10000,"durationMs":100000,"running":true,"rows":[{"id":"a","label":"Same label","detail":"A"},{"id":"b","label":"Same label","detail":"B"},{"id":"c","label":"Third","detail":"C"}]})),revision:AtomicU64::new(1),calls:AtomicU64::new(0),unknown:AtomicBool::new(false),unavailable:AtomicBool::new(false)}}
}
impl Provider for Fake {
 fn validate(&self,_:&Value)->Result<()>{Ok(())}
 fn permissions(&self,_:&Value)->Result<Value>{Ok(json!({}))}
 fn methods(&self,_:&Value)->Result<BTreeMap<String,Method>>{Ok([("volume".into(),Method{arguments:[("volume".into(),"int".into())].into(),confirmation:false}),("select".into(),Method{arguments:[("key".into(),"string".into())].into(),confirmation:false})].into())}
 fn snapshot(&self,_:&str,_:&Value)->Result<Snapshot>{
  if self.unavailable.load(Ordering::SeqCst){return Err("private-provider-error-must-not-leak".into());}
  Ok(Snapshot{value:self.value.lock().unwrap().clone(),observation:format!("revision-{}",self.revision.load(Ordering::SeqCst)),actions:["volume".into(),"select".into()].into(),phase:"ready".into()})
 }
 fn command(&self,_:&str,_:&Value,_:&str,args:&Value,observation:&str,_:&str,_:bool)->Result<()>{
  if observation!=format!("revision-{}",self.revision.load(Ordering::SeqCst)){return Err(fail());}
  self.calls.fetch_add(1,Ordering::SeqCst);self.revision.fetch_add(1,Ordering::SeqCst);
  if let Some(n)=args.get("volume"){self.value.lock().unwrap()["volume"]=n.clone();}
  if let Some(key)=args.get("key"){self.value.lock().unwrap()["selected"]=key.clone();}
  if self.unknown.load(Ordering::SeqCst){Err("result unknown".into())}else{Ok(())}
 }
}
fn fixture()->(Plan,Value){
 let suite:Value=serde_json::from_str(include_str!("../../../skills/petui/contracts/service-bridge-v1.json")).unwrap();
 (Plan::parse(&suite["declaration"]["content"][0]).unwrap(),suite["runtime"].clone())
}
fn command(context:&Value,id:&str,n:i32)->Value{json!({"action":"host.level","eventId":id,"context":context["ui"]["context"],"data":{"volume":n}})}
#[test]fn shared_display_timeline_contract_never_becomes_an_execution_clock(){
 let suite:Value=serde_json::from_str(include_str!("../../../skills/petui/contracts/service-bridge-v1.json")).unwrap();
 for v in suite["timelineVectors"].as_array().unwrap(){
  let t=v["timeline"].clone();let age=v["age"].as_u64().unwrap();
  let valid=t["durationMs"].as_u64().unwrap()>0&&age<10000;
  if v["value"].is_null(){assert!(!valid);continue;}
  let t:Timeline=serde_json::from_value(t).unwrap();assert_eq!(u64::from(t.value(age)),v["value"].as_u64().unwrap());
 }
}
#[test]fn typed_command_has_one_receipt_and_stale_context_never_replays(){
 let(p,r)=fixture();let provider=Fake::new();p.validate(&r,&provider).unwrap();let mut session=Session::enter(7);
 let initial=session.snapshot(&p,&r,&provider).unwrap();let input=command(&initial,"one",40);
 let accepted=session.command(&p,&r,&provider,&input).unwrap();assert_eq!(accepted["variables"]["progress"],10); // display position, not fake execution
 assert_eq!(provider.calls.load(Ordering::SeqCst),1);
 assert_eq!(session.command(&p,&r,&provider,&input).unwrap()["duplicate"],true);assert_eq!(provider.calls.load(Ordering::SeqCst),1);
 let mut changed=input.clone();changed["data"]["volume"]=41.into();assert!(session.command(&p,&r,&provider,&changed).is_err());
 let mut stale=input;stale["eventId"]="two".into();assert!(session.command(&p,&r,&provider,&stale).is_err());
 session.revoke();assert!(session.snapshot(&p,&r,&provider).is_err());assert!(session.command(&p,&r,&provider,&command(&accepted,"three",50)).is_err());assert_eq!(provider.calls.load(Ordering::SeqCst),1);
}
#[test]fn unknown_result_is_claimed_before_provider_and_is_never_sent_twice(){
 let(p,r)=fixture();let provider=Fake::new();provider.unknown.store(true,Ordering::SeqCst);let mut session=Session::enter(1);
 let status=session.snapshot(&p,&r,&provider).unwrap();let input=command(&status,"unknown",60);
 assert!(session.command(&p,&r,&provider,&input).is_err());assert!(session.command(&p,&r,&provider,&input).unwrap_err().contains("结果未确认"));assert_eq!(provider.calls.load(Ordering::SeqCst),1);
 assert!(session.snapshot(&p,&r,&provider).is_ok());
}
#[test]fn every_argument_is_typed_bounded_and_explicit_no_reserved_field_injection(){
 let(p,r)=fixture();let provider=Fake::new();let mut session=Session::enter(1);let status=session.snapshot(&p,&r,&provider).unwrap();
 for data in [json!({"volume":true}),json!({"volume":"50"}),json!({"volume":101}),json!({"volume":1.5}),json!({"volume":50,"confirmed":true}),json!({"volume":{"script":"anything"}})]{
  let mut input=command(&status,"invalid",50);input["data"]=data;assert!(session.command(&p,&r,&provider,&input).is_err());
 }assert_eq!(provider.calls.load(Ordering::SeqCst),0);
}
#[test]fn source_failure_projects_fallback_disables_actions_and_keeps_private_error_on_pc(){
 let(p,r)=fixture();let provider=Fake::new();let mut session=Session::enter(1);provider.unavailable.store(true,Ordering::SeqCst);
 let s=session.snapshot(&p,&r,&provider).unwrap();assert_eq!(s["sources"]["source"],"unavailable");assert!(s["ui"]["actions"].as_array().unwrap().is_empty());assert_eq!(s["variables"]["title"],"等待数据");assert!(!s.to_string().contains("private-provider"));
}
#[test]fn new_lifetime_cannot_authorize_an_old_pc_command_even_with_same_provider_revision(){
 let(p,r)=fixture();let provider=Fake::new();let mut old=Session::enter(1);let status=old.snapshot(&p,&r,&provider).unwrap();old.revoke();let mut new=Session::enter(1);new.snapshot(&p,&r,&provider).unwrap();assert!(new.command(&p,&r,&provider,&command(&status,"old",40)).is_err());
}
#[test]fn dynamic_collection_reuses_stable_key_model_and_preserves_selection_after_reorder(){
 let(mut p,mut r)=fixture();p.timelines.clear();p.collections.insert("items".into(),CollectionBinding{service:"source".into(),pointer:"/rows".into(),key:"/id".into(),label:"/label".into(),detail:"/detail".into(),visible:2});
 p.commands.insert("host.row_b".into(),Command{service:"items".into(),method:"row_1".into(),arguments:BTreeMap::new()});
 p.commands.insert("host.select".into(),Command{service:"source".into(),method:"select".into(),arguments:[("key".into(),Argument{kind:"string".into(),value:json!({"selection":"items"}),min:None,max:None})].into()});
 r["transitions"]=json!(p.commands.keys().map(|a|json!({"on":a,"from":"*"})).collect::<Vec<_>>());
 let provider=Fake::new();p.validate(&r,&provider).unwrap();let mut session=Session::enter(1);let status=session.snapshot(&p,&r,&provider).unwrap();
 let selected=session.command(&p,&r,&provider,&json!({"action":"host.row_b","eventId":"pick","context":status["ui"]["context"]})).unwrap();assert_eq!(selected["collectionView"]["selectionKey"],"b");
 provider.value.lock().unwrap()["rows"].as_array_mut().unwrap().swap(0,1);provider.revision.fetch_add(1,Ordering::SeqCst);
 assert!(session.command(&p,&r,&provider,&json!({"action":"host.select","eventId":"stale","context":selected["ui"]["context"]})).is_err());
 let fresh=session.snapshot(&p,&r,&provider).unwrap();let played=session.command(&p,&r,&provider,&json!({"action":"host.select","eventId":"select","context":fresh["ui"]["context"]})).unwrap();assert_eq!(played["collectionView"]["selectionKey"],"b");assert_eq!(provider.value.lock().unwrap()["selected"],"b");
}

#[test]fn package_contract_matches_python_for_float_spelling_unicode_and_key_order(){
 let suite:Value=serde_json::from_str(include_str!("../../../skills/petui/contracts/service-bridge-v1.json")).unwrap();
 for v in suite["contractVectors"].as_array().unwrap(){
  let d:crate::app_services::Declaration=serde_json::from_value(v["declaration"].clone()).unwrap();
  // Keep the authored numeric token: a JSON Value round trip loses -0 in Python.
  let runtime=if let Some(raw)=v["runtimeJson"].as_str(){serde_json::from_str::<Value>(raw).unwrap()}else{v["runtime"].clone()};
  assert_eq!(contract::package(&d,&runtime).unwrap(),v["expected"].as_str().unwrap());
 }
 let d:crate::app_services::Declaration=serde_json::from_value(suite["declaration"].clone()).unwrap();assert_eq!(contract::package(&d,&suite["runtime"]).unwrap(),suite["runtime"]["host_contract"].as_str().unwrap());
}
