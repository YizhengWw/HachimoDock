//! Bounded, package-declared automatic public media discovery.
use crate::{component_extension::Extension, media_network::{self as net, Station}};
use serde_json::Value;
use std::collections::HashSet;
pub const QUEUE_MAX: usize = 128;
pub struct Catalog { pub stations: Vec<Station>, pub location: String }
fn normalized(s: &str) -> String {
    s.trim().trim_end_matches('市').chars().filter(|c|!c.is_whitespace()).flat_map(char::to_lowercase).collect()
}
pub fn select(e: &Extension, rows: &[Value], location: Option<&Value>) -> Result<Catalog,String> {
    let d=e.discovery.as_ref().ok_or("未声明自动目录")?;
    let city=location.filter(|v|v.pointer(&d.country_field).and_then(Value::as_str)==Some(d.country.as_str()))
        .and_then(|v|v.pointer(&d.city_field)).and_then(Value::as_str).filter(|s|!s.is_empty()&&s.len()<=80)
        .map(|s|d.aliases.get(s).map(String::as_str).unwrap_or(s)).unwrap_or("");
    let city_key=normalized(city);
    let mut ranked=Vec::new();
    let mut urls=HashSet::new();let mut names=HashSet::new();
    let mut ordered:Vec<_>=rows.iter().collect();
    ordered.sort_by_key(|r|std::cmp::Reverse((r.pointer(&d.popularity).and_then(Value::as_u64).unwrap_or(0),r.pointer(&d.votes).and_then(Value::as_u64).unwrap_or(0))));
    for row in ordered {
        // Reuse the same constrained field mapper, including availability/URL validation.
        let mut mapped=Value::Null;
        if e.search.items_path.is_empty(){mapped=serde_json::json!([row]);}
        let Some(mut station)=e.map_results(&mapped, "全国").into_iter().next() else {continue};
        if !urls.insert(station.url.clone()) || !names.insert(normalized(&station.name)){continue;}
        let local=!city_key.is_empty()&&(normalized(&station.name).contains(&city_key)
            || normalized(row.pointer(&d.station_region).and_then(Value::as_str).unwrap_or(""))==city_key);
        if local {station.city=city.into();}
        ranked.push((station,local));
    }
    let has_local=ranked.iter().any(|(_,local)|*local);
    let mut extra=0;
    let stations:Vec<_>=ranked.into_iter().filter_map(|(s,local)|{
        if has_local && local {Some(s)} else if extra<if has_local {d.local_supplement}else{d.national_count} {extra+=1;Some(s)}else{None}
    }).collect();
    if stations.is_empty(){return Err("目录暂未返回可用电台".into());}
    // Never claim all city stations while silently dropping the tail.
    if stations.len()>QUEUE_MAX {return Err("本地目录超过设备队列上限，请更新支持更大队列的版本".into());}
    Ok(Catalog{stations,location:if has_local {city.into()}else{"全国".into()}})
}
pub fn discover(e: &Extension) -> Result<Catalog,String> {
    e.validate()?;
    let d=e.discovery.as_ref().ok_or("未声明自动目录")?;
    // Location failure never prevents country-wide discovery. No IP is persisted.
    let location=net::scoped_json(&d.location_endpoint,&e.permissions.network_hosts,16384).ok();
    let mut last="目录服务暂不可用".to_string();
    let start=std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs() as usize % d.endpoints.len();
    for index in 0..d.endpoints.len() {
        let endpoint=&d.endpoints[(start+index)%d.endpoints.len()];
        let result=(|| {
            // Do not download the entire national directory to select its top 30.
            let mut url=net::valid_url(endpoint)?;
            url.query_pairs_mut().extend_pairs(&e.search.params).append_pair("limit","250");
            let national=net::scoped_json(url.as_str(),&e.permissions.network_hosts,2*1024*1024)?;
            let mut rows=national.as_array().ok_or("目录格式无效")?.clone();
            let city=location.as_ref().filter(|v|v.pointer(&d.country_field).and_then(Value::as_str)==Some(d.country.as_str()))
                .and_then(|v|v.pointer(&d.city_field)).and_then(Value::as_str).unwrap_or("");
            let city=d.aliases.get(city).map(String::as_str).unwrap_or(city).trim_end_matches('市');
            if !city.is_empty() && city.len()<=80 {
                for field in [e.search.query_param.as_str(),"state"] {
                    for page in 0..4 {
                        let mut url=net::valid_url(endpoint)?;
                        url.query_pairs_mut().extend_pairs(&e.search.params).append_pair(field,city).append_pair("limit","250").append_pair("offset",&(page*250).to_string());
                        let value=net::scoped_json(url.as_str(),&e.permissions.network_hosts,2*1024*1024)?;
                        let batch=value.as_array().ok_or("目录格式无效")?;
                        rows.extend(batch.iter().cloned());
                        if batch.len()<250 {break;}
                        if page==3 {return Err("本地目录超过分页安全上限，保留已有列表".into());}
                    }
                }
            }
            select(e,&rows,location.as_ref())
        })();
        match result {Ok(c)=>return Ok(c),Err(e)=>last=e}
    }
    Err(last)
}
#[cfg(test)] mod tests {
    use super::*;
    fn ext()->Extension {crate::component_extension::parse(include_bytes!("../../tests/fixtures/media-sample/pc/extension.json")).unwrap()}
    fn rows()->Vec<Value>{(0..70).map(|i|serde_json::json!({"stationuuid":format!("s{i}"),"name":format!("{}广播{i}",if i<12{"北京"}else{"其他"}),"url_resolved":format!("https://example.com/{i}"),"lastcheckok":1,"clickcount":70-i,"votes":1,"state":""})).collect()}
    #[test] fn city_all_plus_ten_and_national_thirty(){let e=ext();let r=rows();let c=select(&e,&r,Some(&serde_json::json!({"city":"Beijing","country_code":"CN"}))).unwrap();assert_eq!(c.stations.len(),22);assert_eq!(c.location,"北京");for v in [None,Some(serde_json::json!({"city":"Beijing","country_code":"US"})),Some(serde_json::json!({"city":"未收录","country_code":"CN"}))]{let c=select(&e,&r,v.as_ref()).unwrap();assert_eq!(c.stations.len(),30);assert_eq!(c.location,"全国");}}
    #[test] fn deduplicates_and_filters_invalid(){let e=ext();let mut r=rows();r.push(r[0].clone());r[1]["lastcheckok"]=0.into();r[2]["url_resolved"]="file:///etc/passwd".into();let c=select(&e,&r,None).unwrap();assert_eq!(c.stations.len(),30);assert!(!c.stations.iter().any(|s|s.id=="s1"||s.id=="s2"));}
}
