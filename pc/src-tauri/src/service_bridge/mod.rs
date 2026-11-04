//! Bounded, provider-independent service composition. Providers own IO and policy;
//! this layer owns projection, command typing and observation identity.
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet, VecDeque};

pub mod native;
pub mod assets;
pub mod surface;
pub mod contract;

pub const ENGINE: &str = "pc-service-bridge-v1";
pub const PHASES:&[&str]=&["idle","preparing","ready","running","paused","completed","interrupted","unavailable","failed","cancelled"];
pub const MAX_PACKET: usize = 2048;
pub type Result<T> = std::result::Result<T, String>;
fn fail() -> String { "服务桥声明、观察或数据无效，操作未执行".into() }
fn name(s:&str,n:usize)->bool { !s.is_empty()&&s.len()<=n&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b)) }
fn text(s:&str,n:usize)->bool { s.len()<=n&&!s.chars().any(char::is_control) }
fn pointer(s:&str)->bool { s.len()<=128&&(s.is_empty()||s.starts_with('/'))&&!s.chars().any(char::is_control)&&!s.as_bytes().windows(2).any(|p|p[0]==b'~'&&!matches!(p[1],b'0'|b'1'))&&!s.ends_with('~') }
fn digest(v:&impl Serialize)->Result<String>{ Ok(format!("{:x}",Sha256::digest(crate::json_canonical::canonical_vec(v).map_err(|_|fail())?))) }

#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Plan {
    pub services:BTreeMap<String,Value>,
    pub bindings:BTreeMap<String,Binding>,
    pub commands:BTreeMap<String,Command>,
    #[serde(default)] pub timelines:BTreeMap<String,TimelineBinding>,
    #[serde(default)] pub images:BTreeMap<String,Binding>,
    #[serde(default)] pub collections:BTreeMap<String,CollectionBinding>,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Binding { pub service:String,pub pointer:String,pub fallback:Value }
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Command {
    pub service:String,
    pub method:String,
    #[serde(default)] pub arguments:BTreeMap<String,Argument>,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Argument {
    #[serde(rename="type")] pub kind:String,
    pub value:Value,
    #[serde(default)] pub min:Option<i32>,
    #[serde(default)] pub max:Option<i32>,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct TimelineBinding {
    pub service:String,pub position:String,pub duration:String,pub running:String,
    /// Maximum visual extrapolation, never an execution clock or seek receipt.
    pub horizon_ms:u32,
    #[serde(default="percent")] pub projection:String,
}
fn percent()->String{"percent".into()}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct CollectionBinding {
    pub service:String,pub pointer:String,pub key:String,pub label:String,
    pub detail:String,pub visible:u8,
}

/// Native providers expose only host-approved methods. No declaration may
/// increase a provider's permissions, request arbitrary IPC, or select a path.
pub trait Provider {
    fn validate(&self, declaration:&Value)->Result<()>;
    fn permissions(&self, declaration:&Value)->Result<Value>;
    fn methods(&self, declaration:&Value)->Result<BTreeMap<String,Method>>;
    fn snapshot(&self, alias:&str,declaration:&Value)->Result<Snapshot>;
    fn image(&self,_alias:&str,_slot:&str,value:&Value)->Result<ImageRef>{ImageRef::parse(value)}
    fn command(&self,alias:&str,declaration:&Value,method:&str,args:&Value,observation:&str,event_id:&str,confirmed:bool)->Result<()>;
}
#[derive(Clone,Debug)]
pub struct Method {
    pub arguments:BTreeMap<String,String>,
    pub confirmation:bool,
}
#[derive(Clone,Debug)]
pub struct Snapshot {
    pub value:Value,
    /// A stable provider observation. Providers must change this on away/back,
    /// selection/content replacement and asynchronous command completion.
    pub observation:String,
    pub actions:BTreeSet<String>,
    pub phase:String,
}

impl Plan {
    pub fn parse(v:&Value)->Result<Self>{serde_json::from_value(v.clone()).map_err(|_|fail())}
    pub fn validate(&self,runtime:&Value,p:&dyn Provider)->Result<()> {
        let vars=runtime["vars"].as_object().ok_or_else(fail)?;
        if runtime["host"]!=true||runtime["host_context"]!="p4-host-context-v1"||runtime["input"]["engine"]!="p4-input-v1"
            ||runtime["surface"]["bindings"]!="p4-variable-bindings-v1"||runtime.get("persist").is_some()
            ||!runtime["tick"].as_array().is_some_and(Vec::is_empty)||runtime.get("ownership").is_some()
            ||vars.values().any(|v|!matches!(v["type"].as_str(),Some("int"|"string")))
            ||self.services.is_empty()||self.services.len()>4||vars.len()>8
            ||self.bindings.keys().collect::<BTreeSet<_>>()!=vars.keys().collect::<BTreeSet<_>>()
            ||self.commands.len()>16||self.timelines.len()>2||self.images.len()>2||self.collections.len()>1 {return Err(fail());}
        let rules=runtime["transitions"].as_array().ok_or_else(fail)?;
        if rules.len()!=self.commands.len()||rules.iter().any(|r|r.as_object().is_none_or(|m|m.len()!=2)||r["from"]!="*"||!r["on"].as_str().is_some_and(|s|self.commands.contains_key(s))){return Err(fail());}
        let has=|s:&str|self.services.contains_key(s);
        if (!self.timelines.is_empty()||!self.images.is_empty()||self.commands.values().any(|c|c.arguments.values().any(|a|a.value.get("event").is_some())))&&runtime["surface"]["services"]!=surface::DEVICE_ENGINE{return Err("动态素材、时间轴和设备参数需要显式服务桥能力".into());}
        for n in runtime["surface"]["elements"].as_array().ok_or_else(fail)?{
            if let Some(alias)=n.get("image_bind").and_then(Value::as_str){if !self.images.contains_key(alias){return Err(fail());}}
            if let Some(param)=n.get("value_param").and_then(Value::as_str){
                let c=self.commands.get(n["action"].as_str().unwrap_or("")).ok_or_else(fail)?;
                if !c.arguments.values().any(|a|a.kind=="int"&&a.value["event"]==param){return Err(fail());}
            }
        }
        for (a,d) in &self.services {if !name(a,16){return Err(fail());}p.validate(d)?;}
        for (v,b) in &self.bindings {
            if !has(&b.service)||!pointer(&b.pointer)||(!scalar(&b.fallback,vars[v]["type"].as_str().unwrap_or(""))||!within_limits(runtime,v,&b.fallback)){return Err(fail());}
        }
        for (a,c) in &self.commands {
            if !a.starts_with("host.")||!name(a,47)||(!has(&c.service)&&!self.collections.contains_key(&c.service))||!name(&c.method,47)||c.arguments.len()>4{return Err(fail());}
            if let Some(collection)=self.collections.get(&c.service){
                if !c.arguments.is_empty()||!crate::component_collection::OPERATIONS.contains(&c.method.as_str())||c.method.strip_prefix("row_").and_then(|s|s.parse::<u8>().ok()).is_some_and(|i|i>=collection.visible){return Err(fail());}continue;
            }
            let methods=p.methods(&self.services[&c.service])?;
            let m=methods.get(&c.method).ok_or_else(fail)?;
            if m.arguments.keys().collect::<BTreeSet<_>>()!=c.arguments.keys().collect::<BTreeSet<_>>() {return Err(fail());}
            if m.confirmation&&!runtime["input"]["confirm"][a].as_str().is_some_and(|s|!s.trim().is_empty()){return Err(fail());}
            for (k,arg) in &c.arguments {
                if !name(k,31)||m.arguments[k]!=arg.kind||!matches!(arg.kind.as_str(),"int"|"string"|"bool")||arg.min.zip(arg.max).is_some_and(|(lo,hi)|lo>hi)
                    ||arg.kind!="int"&&(arg.min.is_some()||arg.max.is_some()) {return Err(fail());}
                if let Some(o)=arg.value.as_object(){
                    if o.len()!=1{return Err(fail());}
                    if let Some(key)=o.get("event").and_then(Value::as_str){if !name(key,31){return Err(fail());}}
                    else if let Some(key)=o.get("selection").and_then(Value::as_str){if arg.kind!="string"||!self.collections.contains_key(key){return Err(fail());}}
                    else{return Err(fail());}
                }else{argument_value(arg,&arg.value)?;}
            }
        }
        for (a,t) in &self.timelines {if !name(a,31)||vars.get(a).is_none_or(|v|v["type"]!="int")||!has(&t.service)||![&t.position,&t.duration,&t.running].iter().all(|s|pointer(s))||!(100..=10000).contains(&t.horizon_ms)||!matches!(t.projection.as_str(),"percent"|"position"|"remaining"){return Err(fail());}}
        for (a,b) in &self.images {if !name(a,15)||!has(&b.service)||!pointer(&b.pointer)||!b.fallback.is_null(){return Err(fail());}}
        if let Some(view)=runtime["surface"].get("collection_view"){
            let config=crate::app_host_collection::Config::parse(view)?;
            let collection=self.collections.get(&config.source).ok_or_else(fail)?;
            if config.visible!=collection.visible||config.actions.iter().enumerate().any(|(i,a)|self.commands.get(a).is_none_or(|c|c.service!=config.source||c.method!=format!("row_{i}"))){return Err(fail());}
        }
        for (a,c) in &self.collections {if !name(a,16)||has(a)||!has(&c.service)||![&c.pointer,&c.key,&c.label,&c.detail].iter().all(|s|pointer(s))||c.key.is_empty()||!(1..=8).contains(&c.visible){return Err(fail());}}
        Ok(())
    }
}
fn dashboard(r:&Value)->Value{let mut values=serde_json::Map::new();for k in ["eyebrow","headline","metricValue","metricLabel","metricUnit","badge","note","footer"]{values.insert(k.into(),r["dashboard"][k].as_str().unwrap_or("").into());}Value::Object(values)}

fn within_limits(r:&Value,key:&str,value:&Value)->bool{let Some(limits)=r["limits"].get(key)else{return true};value.as_i64().is_some_and(|v|limits[0].as_i64().is_some_and(|lo|v>=lo)&&limits[1].as_i64().is_some_and(|hi|v<=hi))}

fn scalar(v:&Value,kind:&str)->bool {
    match kind {"int"=>v.as_i64().is_some_and(|n|(-1_000_000_000..=1_000_000_000).contains(&n)),"string"=>v.as_str().is_some_and(|s|text(s,63)),"bool"=>v.is_boolean(),_=>false}
}
fn argument_value(a:&Argument,v:&Value)->Result<Value>{
    if !scalar(v,&a.kind)||v.as_i64().is_some_and(|n|a.min.is_some_and(|m|n<i64::from(m))||a.max.is_some_and(|m|n>i64::from(m))){return Err(fail());}Ok(v.clone())
}

#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Timeline {pub position_ms:u32,pub duration_ms:u32,pub running:bool,pub horizon_ms:u32,pub projection:String}
impl Timeline {
    pub fn position(&self,age_ms:u64)->u32 {
        let n=u64::from(self.position_ms)+if self.running{age_ms.min(u64::from(self.horizon_ms))}else{0};
        n.min(u64::from(self.duration_ms)) as u32
    }
    pub fn value(&self,age_ms:u64)->u32{let p=self.position(age_ms);match self.projection.as_str(){"position"=>p,"remaining"=>self.duration_ms-p,_=>((u64::from(p)*100)/u64::from(self.duration_ms)) as u32}}
}
fn timeline(t:&TimelineBinding,v:&Value)->Result<Timeline>{
    let position=v.pointer(&t.position).and_then(Value::as_u64).filter(|n|*n<=86_400_000).ok_or_else(fail)?;
    let duration=v.pointer(&t.duration).and_then(Value::as_u64).filter(|n|*n>0&&*n<=86_400_000).ok_or_else(fail)?;
    let running=v.pointer(&t.running).and_then(Value::as_bool).ok_or_else(fail)?;
    if position>duration{return Err(fail());}
    Ok(Timeline{position_ms:position as u32,duration_ms:duration as u32,running,horizon_ms:t.horizon_ms,projection:t.projection.clone()})
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct ImageRef {pub key:String,pub revision:String,pub width:u16,pub height:u16}
impl ImageRef {
    pub fn parse(v:&Value)->Result<Self>{
        let x:Self=serde_json::from_value(v.clone()).map_err(|_|fail())?;
        if !name(&x.key,47)||!name(&x.revision,64)||!(8..=192).contains(&x.width)||!(8..=192).contains(&x.height){return Err(fail());}Ok(x)
    }
}

/// One runtime object per installed component version and view lifetime. It is
/// never restored from package/user JSON and is discarded on owner revocation.
#[derive(Default)]
pub struct Session {
    pub generation:u64,
    nonce:String,
    active:bool,
    observations:BTreeMap<String,String>,
    receipts:VecDeque<(String,String,bool)>,
    pending:BTreeSet<String>,
    views:BTreeMap<String,crate::component_collection::State>,
}
impl Session {
    pub fn enter(generation:u64)->Self{Self{generation,nonce:uuid::Uuid::new_v4().to_string(),active:true,..Default::default()}}
    pub fn revoke(&mut self){self.active=false;self.observations.clear();self.pending.clear();}
    fn capture(&mut self,plan:&Plan,p:&dyn Provider)->Result<BTreeMap<String,Snapshot>>{
        if !self.active{return Err(fail());}
        let mut out=BTreeMap::new();
        for (a,d) in &plan.services {
            let mut s=p.snapshot(a,d).unwrap_or(Snapshot{value:Value::Null,observation:digest(&(a,"unavailable"))?,actions:BTreeSet::new(),phase:"unavailable".into()});
            if !name(&s.observation,64)||!PHASES.contains(&s.phase.as_str())||s.actions.iter().any(|s|!name(s,47)){return Err(fail());}
            if let Some(value)=s.value.as_object_mut(){value.insert("$service".into(),json!({"phase":s.phase,"revision":s.observation}));}
            if self.observations.get(a)!=Some(&s.observation){self.observations.insert(a.clone(),s.observation.clone());}
            out.insert(a.clone(),s);
        }
        Ok(out)
    }
    fn context(&self,plan:&Plan,runtime:&Value)->Result<String>{digest(&(&self.nonce,self.generation,plan,runtime,&self.observations,&self.views))}
    pub fn snapshot(&mut self,plan:&Plan,runtime:&Value,p:&dyn Provider)->Result<Value>{
        let sources=self.capture(plan,p)?;
        let mut variables=serde_json::Map::new();let mut errors=vec![];
        for (a,b) in &plan.bindings {
            let v=sources[&b.service].value.pointer(&b.pointer).filter(|v|scalar(v,runtime["vars"][a]["type"].as_str().unwrap_or(""))&&within_limits(runtime,a,v));
            if v.is_none(){errors.push(a);}
            variables.insert(a.clone(),v.unwrap_or(&b.fallback).clone());
        }
        let mut collection_models=BTreeMap::new();
        for (alias,c) in &plan.collections {
            let config=crate::component_collection::Config{key:c.key.clone(),label:c.label.clone(),detail:Some(c.detail.clone()),page_size:c.visible};
            let empty=json!([]);
            let items=if sources[&c.service].phase=="unavailable"{&empty}else{sources[&c.service].value.pointer(&c.pointer).ok_or_else(fail)?};
            let state=self.views.entry(alias.clone()).or_default();
            let model=crate::component_collection::project(&config,items,state,None).or_else(|_|{errors.push(alias);crate::component_collection::project(&config,&empty,state,None)})?;
            collection_models.insert(alias.clone(),model);
        }
        let actions:Vec<_>=plan.commands.iter().filter(|(_,c)|if let Some(model)=collection_models.get(&c.service){crate::component_collection::enabled(model,&c.method)}else{sources[&c.service].actions.contains(&c.method)&&!self.pending.contains(&c.service)}).map(|(a,_)|a).collect();
        let mut timelines=BTreeMap::new();
        for (a,t) in &plan.timelines {if let Ok(v)=timeline(t,&sources[&t.service].value){
            let ceiling=if v.projection=="percent"{100}else{v.duration_ms};
            if within_limits(runtime,a,&json!(0))&&within_limits(runtime,a,&json!(ceiling)){variables.insert(a.clone(),v.value(0).into());timelines.insert(a,v);}else{errors.push(a);}
        }}
        let mut images=BTreeMap::new();
        for (a,b) in &plan.images {if let Some(raw)=sources[&b.service].value.pointer(&b.pointer){if !raw.is_null()&&raw.as_str()!=Some(""){match p.image(&b.service,a,raw){Ok(image)=>{images.insert(a,image);},Err(_)=>errors.push(a)}}}}
        let phases:BTreeMap<_,_>=sources.iter().map(|(a,s)|(a,&s.phase)).collect();
        let mut value=json!({"enabled":true,"service":"bridge","variables":variables,"values":dashboard(runtime),"ui":{"mode":0,"actions":actions,"context":self.context(plan,runtime)?},"presentation":{"timelines":timelines,"images":images},"sources":phases,"bindingErrors":errors});
        if runtime["surface"]["services"]!=surface::DEVICE_ENGINE{value.as_object_mut().unwrap().remove("presentation");}
        // The existing stable-key collection transport and renderer remain the
        // one device list implementation; no application-specific list renderer.
        if let Some((alias,model))=collection_models.iter().next(){
            value["collectionView"]=crate::app_host_collection::project(alias,plan.collections[alias].visible,model)?;
        }
        // Never silently truncate an ID, drop a channel or split an observation.
        if serde_json::to_vec(&value).map_err(|_|fail())?.len()>MAX_PACKET{return Err("服务桥快照超过 2048 字节，原数据保留".into());}
        Ok(value)
    }
    pub fn command(&mut self,plan:&Plan,runtime:&Value,p:&dyn Provider,input:&Value)->Result<Value>{
        if !self.active{return Err(fail());}
        let action=input["action"].as_str().ok_or_else(fail)?;
        let command=plan.commands.get(action).ok_or_else(fail)?;
        let event=input.get("data").cloned().unwrap_or(json!({}));
        let data=event.as_object().filter(|o|o.len()<=4).ok_or_else(fail)?;
        let event_id=input.get("_eventId").or_else(||input.get("eventId")).and_then(Value::as_str).filter(|s|!s.is_empty()&&s.len()<=128&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-:".contains(&b))).ok_or_else(fail)?;
        let fingerprint=digest(&(plan,runtime,action,&event))?;
        if let Some((_,old,completed))=self.receipts.iter().find(|(id,_,_)|id==event_id){
            if old!=&fingerprint{return Err(fail());}
            if !completed{return Err("此请求的结果未确认；仅可读取状态，不自动重发".into());}
            let mut s=self.snapshot(plan,runtime,p)?;s["duplicate"]=true.into();return Ok(s);
        }
        let snapshots=self.capture(plan,p)?;
        if input["context"].as_str()!=Some(self.context(plan,runtime)?.as_str())||self.pending.contains(&command.service){return Err(fail());}
        if runtime["input"]["confirm"].get(action).is_some()&&input["confirm"]!=true{return Err(fail());}
        if let Some(c)=plan.collections.get(&command.service){
            if !data.is_empty(){return Err(fail());}
            let config=crate::component_collection::Config{key:c.key.clone(),label:c.label.clone(),detail:Some(c.detail.clone()),page_size:c.visible};
            let items=snapshots[&c.service].value.pointer(&c.pointer).ok_or_else(fail)?;
            let state=self.views.entry(command.service.clone()).or_default();
            let model=crate::component_collection::project(&config,items,state,None)?;
            if !crate::component_collection::enabled(&model,&command.method){return Err(fail());}
            crate::component_collection::project(&config,items,state,Some(&command.method))?;
            self.receipts.push_back((event_id.into(),fingerprint,true));if self.receipts.len()>128{self.receipts.pop_front();}
            self.generation=self.generation.checked_add(1).ok_or_else(fail)?;
            return self.snapshot(plan,runtime,p);
        }
        let snapshot=&snapshots[&command.service];
        if !snapshot.actions.contains(&command.method){return Err(fail());}
        let methods=p.methods(&plan.services[&command.service])?;
        let method=methods.get(&command.method).ok_or_else(fail)?;
        if (method.confirmation||runtime["input"]["confirm"].get(action).is_some())&&input["confirm"]!=true{return Err(fail());}
        let mut arguments=serde_json::Map::new();let mut used=BTreeSet::new();
        for (a,spec) in &command.arguments {
            let selected;
            let value=if let Some(key)=spec.value.get("event").and_then(Value::as_str){used.insert(key);data.get(key).ok_or_else(fail)?}
                else if let Some(alias)=spec.value.get("selection").and_then(Value::as_str){
                    let c=&plan.collections[alias];let config=crate::component_collection::Config{key:c.key.clone(),label:c.label.clone(),detail:Some(c.detail.clone()),page_size:c.visible};
                    let items=snapshots[&c.service].value.pointer(&c.pointer).ok_or_else(fail)?;
                    let model=crate::component_collection::project(&config,items,self.views.entry(alias.into()).or_default(),None)?;
                    selected=model["selected"]["key"].clone();if !selected.is_string(){return Err(fail());}&selected
                }else{&spec.value};
            arguments.insert(a.clone(),argument_value(spec,value)?);
        }
        if data.keys().any(|k|!used.contains(k.as_str())){return Err(fail());}
        // Claim BEFORE calling a native service. An error is an unknown outcome,
        // never permission to retry this event. Read-only status remains usable.
        self.receipts.push_back((event_id.into(),fingerprint,false));if self.receipts.len()>128{self.receipts.pop_front();}
        self.pending.insert(command.service.clone());
        // Consuming an observation also invalidates it when the outcome is
        // unknown. Receipt eviction must never make an old event replayable.
        self.generation=self.generation.checked_add(1).ok_or_else(fail)?;
        let result=p.command(&command.service,&plan.services[&command.service],&command.method,&Value::Object(arguments),&snapshot.observation,event_id,input["confirm"]==true);
        self.pending.remove(&command.service);
        result?;
        if let Some(receipt)=self.receipts.iter_mut().find(|(id,_,_)|id==event_id){receipt.2=true;}
        self.snapshot(plan,runtime,p)
    }
}


#[cfg(test)] mod tests;
