//! Typed, package-owned declarations for host services; no scripts or arbitrary IPC.
use serde::{Deserialize,Serialize};
use serde_json::Value;
use std::collections::{BTreeMap,HashSet};

#[derive(Clone,Debug,Deserialize,Serialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Declaration{
    pub schema_version:u8,
    pub engine:String,
    pub service:String,
    pub actions:BTreeMap<String,String>,
    #[serde(default)]pub content:Vec<Value>,
    #[serde(default)]pub auto_read:bool,
    #[serde(default,skip_serializing_if="Option::is_none")]pub location:Option<crate::location_profile::Profile>,
}
impl Declaration {
    pub fn is_library(&self)->bool{(self.engine=="pc-app-services-v3"&&self.service=="knowledge")||(self.engine=="pc-app-books-v1"&&self.service=="books")}
}
pub fn operations(service:&str)->&'static [&'static str]{match service{
    "desktop-tasks"=>&["voice","details","previous","next"],
    "journey"=>&["depart","collection","previous","next"],
    "weather"=>&["refresh","todo","details","choose_city","previous","next"],
    "notes"=>&["record","save","todo","delete","previous","next"],
    "todos"=>&["open","toggle","filter","previous","next","up","down","row_a","row_b","row_c","row_d","row_e"],
    "knowledge"=>&["speak","favorite","previous","next","random","category"],
    "books"=>&["previous","next","random","category"],
    "news"=>&["refresh","toggle","previous","next","up","down"],
    "calendar"=>&["today","details","previous","next"],
    "soundscape"=>&["toggle","timer","previous","next","up","down","rain","sea","fire","insects"],_=>&[]}}
pub fn parse(bytes:&[u8],runtime:&Value)->Result<Declaration,String>{
    let error=||"PC 应用声明或动作映射无效".to_string();
    if bytes.len()>4*1024*1024{return Err(error());}
    let d:Declaration=serde_json::from_slice(bytes).map_err(|_|error())?;
    let library=d.is_library();
    let books=d.engine=="pc-app-books-v1"&&d.service=="books";
    let calendar=d.engine=="pc-app-calendar-v1"&&d.service=="calendar";
    let news=d.engine=="pc-app-news-v1"&&d.service=="news";
    if d.auto_read&&!library&&!news{return Err(error());}
    if !library&&!calendar&&bytes.len()>16384{return Err(error());}
    if d.schema_version!=1||!matches!(d.engine.as_str(),"pc-app-services-v1"|"pc-app-services-v2"|"pc-app-services-v3"|"pc-app-calendar-v1"|"pc-app-books-v1"|"pc-app-news-v1")||d.engine=="pc-app-services-v3"&&!library||(d.engine=="pc-app-news-v1")!=news||(d.service=="news")!=news||(d.engine=="pc-app-calendar-v1")!=calendar||(d.service=="calendar")!=calendar||(d.engine=="pc-app-books-v1")!=books||(d.service=="books")!=books||operations(&d.service).is_empty()||runtime["host"]!=true||runtime["surface"]["engine"]!="p4-app-surface-v1"
        ||runtime["vars"].as_object().is_none_or(|v|!v.is_empty())||runtime["tick"].as_array().is_some_and(|v|!v.is_empty())||runtime.get("persist").is_some(){return Err(error());}
    if (d.engine=="pc-app-services-v2")!=d.location.is_some()||d.location.is_some()&&d.service!="weather"{return Err(error());}
    if let Some(p)=&d.location{p.validate()?;}
    if d.service=="weather"&&d.actions.values().any(|v|matches!(v.as_str(),"choose_city"|"previous"|"next")){
        let p=d.location.as_ref().ok_or_else(error)?;
        if ["cityButton","emptyCityButton","pickerNote","pickerConfirm","pickerCancel","refreshButton","todoButton"].iter().any(|k|!p.text.contains_key(*k))
            ||["choose_city","previous","next","refresh","todo"].iter().any(|op|!d.actions.values().any(|v|v==op)){return Err(error());}
    }
    let mut ops=HashSet::new();
    let input_v1=runtime["input"]["engine"]=="p4-input-v1";
    if d.service=="todos"&&(!input_v1||d.actions.iter().any(|(a,op)|op=="toggle"&&runtime["input"]["confirm"][a].as_str().is_none_or(|s|s.trim().is_empty()))){return Err(error());}
    if d.actions.is_empty()||d.actions.len()>if input_v1{16}else{8}||d.actions.iter().any(|(a,o)|a.strip_prefix("host.").is_none_or(|s|s.is_empty()||s.len()>32||!s.bytes().all(|c|c.is_ascii_lowercase()||c==b'_'))||!operations(&d.service).contains(&o.as_str())||!ops.insert(o)){return Err(error());}
    if !input_v1&&d.service=="soundscape"&&d.actions.values().any(|o|matches!(o.as_str(),"rain"|"sea"|"fire"|"insects")){return Err(error());}
    let rules=runtime["transitions"].as_array().ok_or_else(error)?;
    if rules.len()!=d.actions.len()||rules.iter().any(|r|r.as_object().is_none_or(|o|o.len()!=2)||r["from"]!="*"||!r["on"].as_str().is_some_and(|a|d.actions.contains_key(a))){return Err(error());}
    if news {
        if !input_v1||operations("news").iter().any(|op|!d.actions.values().any(|v|v==op)){return Err(error());}
        crate::component_news::topics(&d.content)?;return Ok(d);
    }
    if calendar {
        if !input_v1||["today","details","previous","next"].iter().any(|op|!d.actions.values().any(|v|v==op)){return Err(error());}
        crate::component_calendar::validate(&d.content)?;
        return Ok(d);
    }
    if d.content.len()>if books{100}else if library{1000}else{20}||d.content.is_empty()==matches!(d.service.as_str(),"journey"|"knowledge"|"books"){return Err(error());}
    let short=|v:&Value,n:usize|v.as_str().is_some_and(|s|!s.trim().is_empty()&&s.len()<=n&&!s.chars().any(char::is_control));
    let mut ids=HashSet::new();
    for c in &d.content{
        let keys:&[&str]=if library{&["id","title","text","source","url","category","license","revision"]}else if d.service=="knowledge"{&["id","title","text","source","url"]}else{&["id","title","text","minutes"]};
        if c.as_object().is_none_or(|o|o.len()!=keys.len()||o.keys().any(|k|!keys.contains(&k.as_str())))||!short(&c["title"],120)||!short(&c["text"],if library{6000}else{600}){return Err(error());}
        if library&&(!short(&c["category"],40)||!short(&c["revision"],32)||if books{c["license"]!="Original commentary"||c["category"]=="全部书籍"}else{!matches!(c["license"].as_str(),Some("CC BY-SA 4.0"|"CC BY 4.0"|"CC0 1.0"))}
            ||c["text"].as_str().unwrap().chars().filter(|ch|('\u{3400}'..='\u{9fff}').contains(ch)).count()<if books{800}else{300}){return Err(error());}
        let id=c["id"].as_str().filter(|s|!s.is_empty()&&s.len()<=32&&s.bytes().all(|c|c.is_ascii_lowercase()||c.is_ascii_digit()||b"_-".contains(&c))).ok_or_else(error)?;
        if !ids.insert(id){return Err(error());}
        if matches!(d.service.as_str(),"knowledge"|"books"){
            if !short(&c["source"],80)||!short(&c["url"],512){return Err(error());}
            let u=reqwest::Url::parse(c["url"].as_str().unwrap()).map_err(|_|error())?;
            if u.scheme()!="https"||!u.username().is_empty()||u.password().is_some()||u.port_or_known_default()!=Some(443)||!u.host_str().is_some_and(|h|h.contains('.')){return Err(error());}
        }else if !c["minutes"].as_u64().is_some_and(|m|(1..=1440).contains(&m)){return Err(error());}
    }
    if library{
        let categories:HashSet<_>=d.content.iter().map(|c|c["category"].as_str().unwrap()).collect();
        if categories.len()>16||["random","category","previous","next"].iter().any(|op|!d.actions.values().any(|v|v==op)){return Err(error());}
    }
    Ok(d)
}
pub fn permissions(d:&Declaration)->Value{let service=d.service.as_str();serde_json::json!({
    "storage":true,"networkHosts":if service=="weather"{d.location.as_ref().unwrap_or(crate::location_profile::legacy()).hosts()}else{vec![]},
    "microphone":service=="notes","speechProvider":matches!(service,"notes"|"knowledge"|"books"|"news"),"autoRead":d.auto_read,"newsGeneration":service=="news",
    "deviceAudio":matches!(service,"knowledge"|"books"|"soundscape"|"news"),"todos":matches!(service,"notes"|"weather"|"todos"),"todoWrites":service=="todos",
    "desktopReceipts":service=="desktop-tasks","startVoiceConversation":service=="desktop-tasks"
})}

#[cfg(test)]mod tests{
    use super::*;use serde_json::json;
    #[test]fn checklist_mutations_require_declared_confirmation_and_bounded_operations(){
        let d=json!({"schemaVersion":1,"engine":"pc-app-services-v1","service":"todos","actions":{"host.open":"open","host.toggle":"toggle"}});
        let mut r=json!({"host":true,"surface":{"engine":"p4-app-surface-v1"},"vars":{},"tick":[],"transitions":[{"from":"*","on":"host.open"},{"from":"*","on":"host.toggle"}],"input":{"engine":"p4-input-v1","confirm":{"host.toggle":"确认完成或撤销？"}}});
        let parsed=parse(&serde_json::to_vec(&d).unwrap(),&r).unwrap();
        assert_eq!(permissions(&parsed)["todoWrites"],true);assert_eq!(permissions(&parsed)["microphone"],false);
        r["input"]["confirm"]=json!({});assert!(parse(&serde_json::to_vec(&d).unwrap(),&r).is_err());
        let mut bad=d;bad["actions"]["host.toggle"]="delete".into();assert!(parse(&serde_json::to_vec(&bad).unwrap(),&r).is_err());
    }
    fn fixture()->(Value,Value){
        let actions=json!({"host.primary":"random","host.secondary":"category","host.previous":"previous","host.next":"next"});
        let rules:Vec<_>=actions.as_object().unwrap().keys().map(|a|json!({"from":"*","on":a})).collect();
        let runtime=json!({"host":true,"surface":{"engine":"p4-app-surface-v1"},"vars":{},"tick":[],"transitions":rules});
        let content:Vec<_>=(0..1000).map(|i|json!({"id":format!("entry-{i}"),"title":"测试知识","text":"这是测试使用的长文。".repeat(45),"category":format!("分类{}",i/100),"source":"测试来源","url":"https://example.org/article","license":"CC BY-SA 4.0","revision":"1"})).collect();
        (json!({"schemaVersion":1,"engine":"pc-app-services-v3","service":"knowledge","actions":actions,"content":content}),runtime)
    }
    #[test]fn bounded_library_accepts_thousand_but_rejects_short_duplicate_unlicensed_and_excess(){
        let (v,r)=fixture();let validate=|v:&Value|parse(&serde_json::to_vec(v).unwrap(),&r);
        assert_eq!(validate(&v).unwrap().content.len(),1000);
        let mut auto=v.clone();auto["autoRead"]=true.into();assert!(validate(&auto).unwrap().auto_read);
        auto["autoRead"]="true".into();assert!(validate(&auto).is_err());
        let mut bad=v.clone();bad["content"][0]["text"]="不足字数".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["content"][0]["id"]="entry-1".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["content"][0]["license"]="unknown".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();let c=bad["content"][0].clone();bad["content"].as_array_mut().unwrap().push(c);assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["content"][0]["category"]=" ".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["content"][0]["url"]="https://user:secret@example.org".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["engine"]="pc-app-services-v1".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["service"]="notes".into();assert!(validate(&bad).is_err());
        let mut bad=v.clone();bad["actions"]["host.primary"]="speak".into();assert!(validate(&bad).is_err());
    }
}
