//! Bounded package-owned location/time-series mapping. No executable templates.
use serde::{Deserialize,Serialize};
use serde_json::Value;
use std::collections::BTreeMap;
use crate::app_weather::Location;

#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Profile {
    pub schema_version:u8,pub cities:Vec<Location>,pub geo:Geo,pub forecast:Forecast,
    pub text:BTreeMap<String,String>,pub links:Vec<Link>,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Request {pub endpoint:String,pub params:BTreeMap<String,String>}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Geo {pub request:Request,pub fields:BTreeMap<String,String>,pub require:BTreeMap<String,Value>,pub aliases:BTreeMap<String,String>,pub cooldown_sec:i64}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields,rename_all="camelCase")]
pub struct Forecast {
    pub request:Request,pub rows:String,pub time:String,pub value:String,
    pub conditions:Vec<String>,pub labels:Vec<Label>,pub fallback_label:String,
    pub refresh_sec:i64,pub max_age_sec:i64,
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Label {pub contains:String,pub label:String}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Link {pub label:String,pub url:String}
const TEXT_KEYS:&[&str]=&["readyEyebrow","readyHeadline","readyValue","sourceNote","detailsNote","cachedNote","cityEyebrow","loadingHeadline","failedHeadline","locatingHeadline","chooseHeadline","emptyValue","autoMode","manualMode","locatingNote","locateFailedNote","loadingNote","failedNote","deprecatedNote","todoTitle","todoEmpty","autoButton","selectLabel","selectPlaceholder","customLabel","cityLabel","latLabel","lonLabel","saveButton","autoPending","unspecified","syncHint","dataNotice","geoNotice","forecastNotice"];
fn short(s:&str,max:usize)->bool{!s.trim().is_empty()&&s.len()<=max&&!s.chars().any(char::is_control)}
fn pointer(s:&str)->bool{
    short(s,128)&&s.starts_with('/')&&s.split('~').skip(1).all(|v|v.starts_with('0')||v.starts_with('1'))
}
pub fn url(s:&str)->Result<reqwest::Url,String>{
    let error=||"组件接口必须为无凭据的公网 HTTPS 地址".to_owned();
    if !short(s,512){return Err(error());}
    let u=reqwest::Url::parse(s).map_err(|_|error())?;
    let h=u.host_str().ok_or_else(error)?;
    if u.scheme()!="https"||!u.username().is_empty()||u.password().is_some()||u.port_or_known_default()!=Some(443)||u.fragment().is_some()||u.query().is_some()
        ||h.parse::<std::net::IpAddr>().is_ok()||!crate::media_dns::public_name(h){return Err(error());}
    Ok(u)
}
impl Request{
    fn validate(&self,coordinates:bool)->Result<(),String>{
        url(&self.endpoint)?;
        if self.params.len()>12||self.params.iter().any(|(k,v)|!short(k,40)||!k.bytes().all(|c|c.is_ascii_alphanumeric()||b"_-".contains(&c))||!short(v,256)||(v.contains('$')&&!(coordinates&&matches!(v.as_str(),"$lat"|"$lon")))){
            return Err("组件接口参数超出声明边界".into());
        }
        if coordinates&&(!self.params.values().any(|s|s=="$lat")||!self.params.values().any(|s|s=="$lon")){return Err("预报接口缺少坐标绑定".into());}
        Ok(())
    }
    pub fn build(&self,location:Option<&Location>)->Result<reqwest::Url,String>{
        let mut u=url(&self.endpoint)?;
        for(k,v)in &self.params {
            let value=match v.as_str(){"$lat"=>format!("{:.3}",location.ok_or("缺少位置")?.lat),"$lon"=>format!("{:.3}",location.ok_or("缺少位置")?.lon),_=>v.clone()};
            u.query_pairs_mut().append_pair(k,&value);
        }Ok(u)
    }
}
impl Profile {
    pub fn validate(&self)->Result<(),String>{
        let error=||"位置预报资源配置无效或超过安全边界".to_owned();
        self.geo.request.validate(false)?;self.forecast.request.validate(true)?;
        if self.schema_version!=1||self.cities.is_empty()||self.cities.len()>64||self.cities.iter().any(|c|!c.valid())
            ||self.cities.iter().map(|c|&c.city).collect::<std::collections::HashSet<_>>().len()!=self.cities.len()
            ||self.geo.fields.len()!=3||["city","lat","lon"].iter().any(|k|!self.geo.fields.get(*k).is_some_and(|s|pointer(s)))
            ||self.geo.require.len()>4||self.geo.require.iter().any(|(k,v)|!pointer(k)||!(v.is_boolean()||v.as_str().is_some_and(|s|short(s,64))))
            ||self.geo.aliases.len()>64||self.geo.aliases.iter().any(|(k,v)|!short(k,60)||!short(v,60))
            ||!(86400..=604800).contains(&self.geo.cooldown_sec)
            ||![&self.forecast.rows,&self.forecast.time,&self.forecast.value].iter().all(|s|pointer(s))
            ||self.forecast.conditions.is_empty()||self.forecast.conditions.len()>4||self.forecast.conditions.iter().any(|s|!pointer(s))
            ||self.forecast.labels.len()>32||self.forecast.labels.iter().any(|l|!short(&l.contains,64)||!short(&l.label,60))
            ||!short(&self.forecast.fallback_label,60)||!(1800..=86400).contains(&self.forecast.refresh_sec)
            ||!(3600..=86400).contains(&self.forecast.max_age_sec)
            ||TEXT_KEYS.iter().any(|k|!self.text.get(*k).is_some_and(|s|short(s,600)))
            ||self.text.iter().any(|(k,v)|!short(v,600)||!TEXT_KEYS.contains(&k.as_str())&&!["cityButton","emptyCityButton","pickerNote","pickerConfirm","pickerCancel","refreshButton","todoButton"].contains(&k.as_str()))
            ||self.links.len()>4||self.links.iter().any(|l|!short(&l.label,90)||url(&l.url).is_err()){return Err(error());}
        for s in self.text.values(){
            let mut rest=s.as_str();
            while let Some((before,tail))=rest.split_once('{'){
                if before.contains('}'){return Err(error());}
                let (name,next)=tail.split_once('}').ok_or_else(error)?;
                if !["city","time","value","condition","mode"].contains(&name){return Err(error());}
                rest=next;
            }
            if rest.contains('}'){return Err(error());}
        }Ok(())
    }
    /// Hash all adapter/display changes so old-provider responses cannot leak into a new package.
    pub fn key(&self)->String{
        use sha2::{Digest,Sha256};
        format!("{:x}",Sha256::digest(serde_json::to_vec(self).expect("profile serialization")))
    }
    pub fn text(&self,key:&str)->String{self.text.get(key).cloned().unwrap_or_default()}
    /// One pass; braces inside untrusted remote strings are never evaluated.
    pub fn render(&self,key:&str,vars:&[(&str,String)])->String{
        let template=self.text(key);let mut rest=template.as_str();let mut out=String::new();
        while let Some((before,tail))=rest.split_once('{'){
            out.push_str(before);
            let Some((name,next))=tail.split_once('}')else{break;};
            if let Some((_,v))=vars.iter().find(|(k,_)|*k==name){out.push_str(v);}
            rest=next;
        }out.push_str(rest);out
    }
    pub fn hosts(&self)->Vec<String>{
        [&self.geo.request,&self.forecast.request].iter().filter_map(|r|url(&r.endpoint).ok()?.host_str().map(str::to_owned)).collect()
    }
}
/// Compatibility for v1 packages only. v2 packages must supply their own profile.
pub fn legacy()->&'static Profile{
    static LEGACY:std::sync::OnceLock<Profile>=std::sync::OnceLock::new();
    LEGACY.get_or_init(||serde_json::from_str(include_str!("../../component-profiles/weather.json")).expect("bundled compatibility profile"))
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn package_profile_is_valid_and_requests_cannot_target_local_network(){
        legacy().validate().unwrap();
        for bad in ["http://example.com/","https://127.0.0.1/","https://a.local/","https://u:p@example.com/","https://example.com/?key=x","https://example.com:444/","https://example.com/#x"]{assert!(url(bad).is_err(),"{bad}");}
        let mut p=legacy().clone();p.geo.request.params.insert("steal".into(),"$secret".into());assert!(p.validate().is_err());
        let mut p=legacy().clone();p.text.insert("readyHeadline".into(),"{unknown}".into());assert!(p.validate().is_err());
    }
    #[test]fn package_copy_and_mapping_change_without_client_constants(){
        let mut p=legacy().clone();let old=p.key();
        p.cities=vec![Location{city:"青岛".into(),lat:36.067,lon:120.383}];
        p.text.insert("readyHeadline".into(),"今日 · {condition}".into());
        p.forecast.request.endpoint="https://forecast.example.com/v2".into();
        p.validate().unwrap();assert_ne!(old,p.key());
        assert_eq!(p.render("readyHeadline",&[("condition","{city}".into()),("city","不能递归插入".into())]),"今日 · {city}");
    }
}
