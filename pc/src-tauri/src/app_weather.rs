//! Bounded location + dated forecast transport. Adapters live in packages.
use serde::{Deserialize,Serialize};
use serde_json::Value;
use std::{io::Read,time::Duration,sync::{Mutex,OnceLock},collections::HashMap};
use crate::location_profile::{Profile,Request};

#[derive(Clone,Debug,Default,PartialEq,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Location{pub city:String,pub lat:f64,pub lon:f64}
impl Location{pub fn valid(&self)->bool{!self.city.trim().is_empty()&&self.city.len()<=60&&!self.city.chars().any(char::is_control)&&self.lat.is_finite()&&self.lon.is_finite()&&self.lat.abs()<=90.0&&self.lon.abs()<=180.0}}
#[derive(Clone,Debug,Default,PartialEq,Serialize,Deserialize)]
#[serde(default)]
pub struct GeoCache{pub key:String,pub location:Location,pub next:i64,pub error:String}
#[derive(Clone,Debug,Default,PartialEq,Serialize,Deserialize)]
#[serde(default)]
pub struct Cache{pub key:String,pub next:i64,pub fetched:i64,pub modified:String,pub data:Value,pub error:String}
static GEO:OnceLock<Mutex<HashMap<String,(GeoCache,bool)>>>=OnceLock::new();
static PENDING:OnceLock<Mutex<HashMap<String,(Cache,bool)>>>=OnceLock::new();
fn geo_key(p:&Profile)->String{
    use sha2::{Digest,Sha256};
    format!("{:x}",Sha256::digest(serde_json::to_vec(&p.geo).expect("geo serialization")))
}
fn cache_key(p:&Profile,l:&Location)->String{format!("{}:{:.3},{:.3}",p.key(),l.lat,l.lon)}
fn parse_geo(p:&Profile,data:&Value)->Result<Location,String>{
    let bad=||p.text("locateFailedNote");
    if p.geo.require.iter().any(|(path,v)|data.pointer(path)!=Some(v)){return Err(bad());}
    let raw=data.pointer(&p.geo.fields["city"]).and_then(Value::as_str).unwrap_or("").trim();
    let city=p.geo.aliases.iter().find(|(k,_)|k.eq_ignore_ascii_case(raw)).map(|(_,v)|v.as_str()).unwrap_or(raw);
    let l=Location{city:city.into(),lat:data.pointer(&p.geo.fields["lat"]).and_then(Value::as_f64).unwrap_or(f64::NAN),lon:data.pointer(&p.geo.fields["lon"]).and_then(Value::as_f64).unwrap_or(f64::NAN)};
    if !l.valid(){return Err(bad());}
    Ok(Location{lat:(l.lat*100.0).round()/100.0,lon:(l.lon*100.0).round()/100.0,..l})
}
fn get(r:&Request,l:Option<&Location>,modified:&str)->Result<reqwest::blocking::Response,String>{
    let url=r.build(l)?;let host=url.host_str().ok_or("接口域名缺失")?;
    let addresses=crate::media_dns::resolve(host,443)?;
    let client=reqwest::blocking::Client::builder().no_proxy().resolve_to_addrs(host,&addresses)
        .redirect(reqwest::redirect::Policy::none()).connect_timeout(Duration::from_secs(8)).timeout(Duration::from_secs(15))
        .user_agent("HachimoDock/1.0 https://github.com/YizhengWw/HachimoDock").gzip(true).build().map_err(|_|"网络初始化失败")?;
    // Only retry a failed connection once. An HTTP response (including 429/5xx)
    // is never retried here; its cache/backoff policy is handled by the caller.
    for attempt in 0..2 {
        let mut request=client.get(url.clone());
        if !modified.is_empty(){request=request.header("If-Modified-Since",modified);}
        match request.send(){
            Ok(response)=>return Ok(response),
            Err(e) if attempt==0&&e.is_connect()=>std::thread::sleep(Duration::from_millis(750)),
            Err(e)=>{
                #[cfg(test)]eprintln!("forecast transport failure: timeout={}, connect={}, request={}",e.is_timeout(),e.is_connect(),e.is_request());
                return Err(if e.is_timeout(){"网络请求超时".into()}else{"网络连接失败".into()});
            }
        }
    }
    Err("网络连接失败".into())
}
fn body(response:reqwest::blocking::Response,max:usize)->Result<Value,String>{
    let mut bytes=vec![];response.take(max as u64+1).read_to_end(&mut bytes).map_err(|_|"响应中断")?;
    if bytes.len()>max{return Err("响应超限".into());}
    serde_json::from_slice(&bytes).map_err(|_|"响应格式无效".into())
}
fn lookup_geo(p:&Profile)->Result<Location,String>{
    let response=get(&p.geo.request,None,"").map_err(|_|p.text("locateFailedNote"))?;
    if !response.status().is_success(){return Err(p.text("locateFailedNote"));}
    parse_geo(p,&body(response,16384).map_err(|_|p.text("locateFailedNote"))?)
}
pub fn locate_background(p:&Profile,cache:&mut GeoCache,now:i64){
    let key=geo_key(p);
    if cache.key!=key||cache.next>now.saturating_add(604800){*cache=GeoCache{key:key.clone(),..Default::default()};}
    let mut entries=GEO.get_or_init(Default::default).lock().unwrap();
    if entries.len()>=8&&!entries.contains_key(&key){
        if let Some(old)=entries.iter().find(|(_,(_,busy))|!*busy).map(|(k,_)|k.clone()){entries.remove(&old);}else{cache.error=p.text("locateFailedNote");return;}
    }
    let entry=entries.entry(key.clone()).or_insert_with(||(cache.clone(),false));
    if entry.0.next<cache.next{entry.0=cache.clone();}
    *cache=entry.0.clone();
    if entry.1{cache.error=p.text("locatingNote");return;}
    if now<cache.next{return;}
    entry.0.next=now.saturating_add(p.geo.cooldown_sec);entry.0.error=p.text("locateFailedNote");entry.1=true;*cache=entry.0.clone();
    cache.error=p.text("locatingNote");
    if !cfg!(test)||std::env::var("PET_WEATHER_LIVE_TEST").as_deref()==Ok("1")
    {let p=p.clone();std::thread::spawn(move||{
        let result=lookup_geo(&p);let mut entries=GEO.get().unwrap().lock().unwrap();
        if let Some(entry)=entries.get_mut(&key){match result{Ok(location)=>{entry.0.location=location;entry.0.error.clear();},Err(e)=>entry.0.error=e}entry.1=false;}
    });}
    else{entry.0.error=p.text("locateFailedNote");entry.1=false;}
}
fn header_time(response:&reqwest::blocking::Response,key:&str)->Option<i64>{chrono::DateTime::parse_from_rfc2822(response.headers().get(key)?.to_str().ok()?).ok().map(|d|d.timestamp())}
pub fn refresh_background(p:&Profile,location:&Location,cache:&mut Cache,now:i64){
    if !location.valid(){return;}
    let key=cache_key(p,location);
    if cache.key!=key{*cache=Cache{key:key.clone(),..Default::default()};}
    let mut entries=PENDING.get_or_init(Default::default).lock().unwrap();
    if entries.len()>=8&&!entries.contains_key(&key){
        if let Some(old)=entries.iter().find(|(_,(_,busy))|!*busy).map(|(k,_)|k.clone()){entries.remove(&old);}else{cache.error=p.text("failedNote");return;}
    }
    let entry=entries.entry(key.clone()).or_insert_with(||(cache.clone(),false));*cache=entry.0.clone();
    if entry.1{cache.error=p.text("loadingNote");return;}
    if now<cache.next&&now>=cache.fetched.saturating_sub(300)&&cache.next<=now.saturating_add(86400){return;}
    entry.1=true;cache.error=p.text("loadingNote");
    if !cfg!(test)||std::env::var("PET_WEATHER_LIVE_TEST").as_deref()==Ok("1")
    {let p=p.clone();let location=location.clone();let mut snapshot=entry.0.clone();drop(entries);
        std::thread::spawn(move||{let _=refresh(&p,&location,&mut snapshot,now);if let Some(entry)=PENDING.get().unwrap().lock().unwrap().get_mut(&key){*entry=(snapshot,false);}});
    }
    else{entry.0.error=p.text("failedNote");entry.0.next=now+1800;entry.1=false;*cache=entry.0.clone();}
}
pub fn refresh(p:&Profile,location:&Location,cache:&mut Cache,now:i64)->Result<(),String>{
    if !location.valid(){return Err(p.text("chooseHeadline"));}
    let key=cache_key(p,location);
    if cache.key!=key{*cache=Cache{key,..Default::default()};}
    if now<cache.next&&now>=cache.fetched.saturating_sub(300)&&cache.next<=now.saturating_add(86400){return Ok(());}
    cache.next=now.saturating_add(p.forecast.refresh_sec)+(uuid::Uuid::new_v4().as_u128()%120) as i64;
    let result=(||{
        let response=get(&p.forecast.request,Some(location),&cache.modified).map_err(|_|p.text("failedNote"))?;
        if let Some(expires)=header_time(&response,"expires"){cache.next=cache.next.max(expires.min(now.saturating_add(86400)));}
        if let Some(seconds)=response.headers().get("retry-after").and_then(|v|v.to_str().ok()).and_then(|v|v.parse::<i64>().ok()).filter(|v|*v>0){cache.next=cache.next.max(now.saturating_add(seconds.min(86400)));}
        if response.status().as_u16()==304&&!cache.data.is_null(){cache.fetched=now;return Ok(());}
        if !response.status().is_success(){
            #[cfg(test)]eprintln!("forecast HTTP {}",response.status().as_u16());
            return Err(p.text("failedNote"));
        }
        let deprecated=response.status().as_u16()==203;
        let modified=response.headers().get("last-modified").and_then(|v|v.to_str().ok()).filter(|s|s.len()<=128).unwrap_or("").to_owned();
        let data=body(response,524288).map_err(|_|p.text("failedNote"))?;
        let rows=data.pointer(&p.forecast.rows).and_then(Value::as_array).filter(|v|!v.is_empty()&&v.len()<=500).ok_or_else(||p.text("failedNote"))?;
        // Persist only mapped fields, not arbitrary response metadata.
        let normalized:Vec<Value>=rows.iter().filter_map(|r|{
            let time=r.pointer(&p.forecast.time)?.as_str()?;
            chrono::DateTime::parse_from_rfc3339(time).ok()?;
            let value=r.pointer(&p.forecast.value)?.as_f64().filter(|v|v.is_finite()&&(-100.0..=70.0).contains(v))?;
            let code=p.forecast.conditions.iter().find_map(|path|r.pointer(path).and_then(Value::as_str)).unwrap_or("");
            if code.len()>128||code.chars().any(char::is_control){return None;}
            Some(serde_json::json!({"time":time,"value":value,"condition":code}))
        }).collect();
        if normalized.is_empty(){return Err(p.text("failedNote"));}
        cache.data=normalized.into();cache.fetched=now;cache.modified=modified;
        if deprecated{return Err(p.text("deprecatedNote"));}Ok(())
    })();
    cache.error=result.as_ref().err().cloned().unwrap_or_default();result
}
pub fn current(p:&Profile,cache:&Cache,now:i64)->Option<(f64,String,String)>{
    if !cache.key.starts_with(&format!("{}:",p.key()))||now<cache.fetched.saturating_sub(300)||cache.fetched==0||now.saturating_sub(cache.fetched)>p.forecast.max_age_sec{return None;}
    let rows=cache.data.as_array()?;
    let row=rows.iter().filter_map(|row|{
        let time=chrono::DateTime::parse_from_rfc3339(row["time"].as_str()?).ok()?.timestamp();
        (time>=now-3600&&time<=now+10800).then_some(((time-now).unsigned_abs(),row,time))
    }).min_by_key(|(delta,_,_)|*delta)?;
    let temp=row.1["value"].as_f64().filter(|v|v.is_finite()&&(-100.0..=70.0).contains(v))?;
    let code=row.1["condition"].as_str().unwrap_or("");
    let label=p.forecast.labels.iter().find(|l|code.contains(&l.contains)).map(|l|l.label.clone()).unwrap_or_else(||p.forecast.fallback_label.clone());
    Some((temp,label,chrono::DateTime::from_timestamp(row.2,0)?.with_timezone(&chrono::Local).format("%H:%M").to_string()))
}
#[cfg(test)]pub(crate) fn seed_forecast_test(l:&Location){
    let p=crate::location_profile::legacy();let key=cache_key(p,l);
    PENDING.get_or_init(Default::default).lock().unwrap().insert(key.clone(),(Cache{key,next:chrono::Utc::now().timestamp()+3600,error:p.text("failedNote"),..Default::default()},false));
}
#[cfg(test)]mod tests{
    use super::*;use crate::location_profile::legacy;
    #[test]fn refuses_future_expired_and_other_adapter_data(){
        let p=legacy();let now=1792144800;let l=&p.cities[0];
        let mut c=Cache{key:cache_key(p,l),fetched:now,data:serde_json::json!([{"time":chrono::DateTime::from_timestamp(now,0).unwrap().to_rfc3339(),"value":23,"condition":"rain"}]),..Default::default()};
        assert_eq!(current(p,&c,now).unwrap().1,"有雨");c.fetched=i64::MIN;assert!(current(p,&c,now).is_none());
        c.fetched=i64::MAX;assert!(current(p,&c,now).is_none());c.fetched=now;c.key="old-provider".into();assert!(current(p,&c,now).is_none());
    }
    #[test]fn city_lookup_uses_package_mapping_and_discards_identity(){
        let p=legacy();let v=serde_json::json!({"success":true,"ip":"198.51.100.1","city":"Beijing","latitude":39.9042,"longitude":116.4074});
        let l=parse_geo(p,&v).unwrap();assert_eq!(l.city,"北京");assert_eq!(l.lat,39.90);
        assert!(!serde_json::to_string(&l).unwrap().contains("198.51"));
        for bad in [serde_json::json!({"success":false}),serde_json::json!({"success":true,"city":"x","latitude":91,"longitude":0}),serde_json::json!({"success":true,"city":"x","latitude":0})]{assert!(parse_geo(p,&bad).is_err());}
        let mut other=p.clone();other.geo.fields.insert("city".into(),"/data/name".into());
        assert_eq!(parse_geo(&other,&serde_json::json!({"success":true,"data":{"name":"青岛"},"latitude":36.06,"longitude":120.38})).unwrap().city,"青岛");
    }
    #[test]#[ignore="Explicit public weather network check; isolated test city only"]
    fn live_public_forecast(){let p=legacy();let now=chrono::Utc::now().timestamp();let mut c=Cache::default();refresh(p,&p.cities[0],&mut c,now).unwrap();assert!(current(p,&c,now).is_some());eprintln!("PASS public forecast; no user settings changed");}
    #[test]#[ignore="Explicit IP city lookup; sends source IP to declared service, saves no address"]
    fn live_ip_city(){assert!(lookup_geo(legacy()).unwrap().valid());eprintln!("PASS valid IP city; address and city not logged");}
}
