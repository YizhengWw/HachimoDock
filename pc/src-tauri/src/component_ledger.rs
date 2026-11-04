//! Generic bounded timestamp ledger for declarative app composition.
//! No component identity branches, fabricated history, network or device audio.
use chrono::{DateTime,Local,NaiveDate,TimeZone};
use serde::{Deserialize,Serialize};
use serde_json::{json,Value};
#[derive(Clone,Deserialize,Serialize)]
#[serde(deny_unknown_fields)]
pub struct Ledger {pub entries:Vec<i64>,pub selected:usize}
pub struct Config<'a>{pub unit:&'a str,pub target:u16,pub capacity:u16,pub label:&'a str}
pub fn valid(c:&Config)->bool{
    matches!(c.unit,"day"|"event")&&(1..=128).contains(&c.capacity)&&c.target>0&&c.target<=c.capacity
        &&!c.label.trim().is_empty()&&c.label.len()<=24&&!c.label.chars().any(char::is_control)
}
fn date(ms:i64)->Result<DateTime<Local>,String>{Local.timestamp_millis_opt(ms).single().filter(|t|t.timestamp_millis()>=0).ok_or("记录时间无效，已有记录未修改".into())}
pub fn empty()->Ledger{Ledger{entries:vec![],selected:0}}
fn checked(c:&Config,s:&Ledger)->Result<(),String>{
    if !valid(c)||s.entries.len()>128||s.entries.len()>c.capacity as usize||(!s.entries.is_empty()&&s.selected>=s.entries.len())||s.entries.is_empty()&&s.selected!=0{return Err("记录配置与既有数据不兼容，原记录保留".into());}
    let mut previous=None;let mut day=None;
    for &ms in &s.entries{let d=date(ms)?.date_naive();if previous.is_some_and(|v|v>ms)||c.unit=="day"&&day==Some(d){return Err("记录数据无效，原记录保留".into());}previous=Some(ms);day=Some(d);}
    Ok(())
}
pub fn allowed(c:&Config,s:&Ledger,now:i64,op:&str)->bool{
    if checked(c,s).is_err(){return false;}
    match op{
        "append"=>s.entries.len()<(c.capacity as usize)&&s.entries.last().is_none_or(|&v|v<=now)
            &&(c.unit!="day"||!s.entries.iter().any(|&v|date(v).ok().map(|d|d.date_naive())==date(now).ok().map(|d|d.date_naive()))),
        "remove_latest"=>!s.entries.is_empty(),
        "previous"=>s.selected>0,
        "next"=>s.selected+1<s.entries.len(),
        _=>false,
    }
}
pub fn apply(c:&Config,s:&mut Ledger,now:i64,op:Option<&str>)->Result<Value,String>{
    checked(c,s)?;let today=date(now)?.date_naive();
    match op{
        Some("append")=>{
            // Day records are idempotent even if a transport delivers a duplicate.
            let already=c.unit=="day"&&s.entries.iter().any(|&v|date(v).is_ok_and(|d|d.date_naive()==today));
            if !already{
                if !allowed(c,s,now,"append"){return Err("记录已达容量或系统时间早于既有记录，原数据未修改".into());}
                s.entries.push(now);s.selected=s.entries.len()-1;
            }
        }
        Some("remove_latest")=>{s.entries.pop();s.selected=s.selected.min(s.entries.len().saturating_sub(1));}
        Some("previous")=>{s.selected=s.selected.saturating_sub(1);},
        Some("next")=>{if s.selected+1<s.entries.len(){s.selected+=1;}},
        None=>{},Some(_)=>return Err("未声明的记录操作".into()),
    }
    let first=s.entries.first().map(|&v|date(v)).transpose()?;
    let days=first.as_ref().map_or(0,|v|(today-v.date_naive()).num_days().max(0)+1);
    let mut unique:Vec<NaiveDate>=s.entries.iter().map(|&v|date(v).map(|t|t.date_naive())).collect::<Result<_,_>>()?;unique.dedup();
    let count=s.entries.len();let selected=s.entries.get(s.selected).map(|&v|date(v)).transpose()?;
    let history=selected.map_or_else(||"尚无记录".to_string(),|v|format!("{} · {} +1",v.format("%m/%d %H:%M"),c.label));
    Ok(json!({"count":count,"target":c.target,"progress":(count*100/c.target as usize).min(100),"progress_label":format!("{count}/{}",c.target),
        "span_days":days,"span_label":format!("{days}天"),"distinct_days":unique.len(),"today":i32::from(unique.contains(&today)),
        "date_range":first.map_or_else(||"开始后记录日期".to_string(),|v|format!("{}–至今",v.format("%Y.%m.%d"))),
        "entry_label":history,"position":if count==0{"0/0".to_string()}else{format!("{}/{}",s.selected+1,count)}}))
}

#[cfg(test)]mod tests{
    use super::*;
    fn ms(d:&str)->i64{chrono::DateTime::parse_from_rfc3339(d).unwrap().timestamp_millis()}
    #[test]fn actual_dates_deduplicate_days_and_keep_history(){
        let c=Config{unit:"day",target:7,capacity:128,label:"Reading"};let mut s=empty();
        let first=ms("2026-10-20T12:00:00+08:00");let last=ms("2026-10-22T12:00:00+08:00");
        for _ in 0..4{apply(&c,&mut s,first,Some("append")).unwrap();}assert_eq!(s.entries.len(),1);
        let v=apply(&c,&mut s,last,Some("append")).unwrap();assert_eq!(v["count"],2);assert_eq!(v["span_days"],3);assert_eq!(v["progress"],28);
        let before=s.entries.clone();apply(&c,&mut s,last,Some("previous")).unwrap();assert_eq!(s.selected,0);assert_eq!(s.entries,before);
        apply(&c,&mut s,last,Some("remove_latest")).unwrap();assert_eq!(s.entries,vec![first]);
    }
    #[test]fn event_mode_capacity_clock_rollback_and_invalid_saved_data(){
        let c=Config{unit:"event",target:2,capacity:2,label:"Session"};let mut s=empty();
        for _ in 0..2{apply(&c,&mut s,100000,Some("append")).unwrap();}assert_eq!(s.entries.len(),2);
        assert!(apply(&c,&mut s,100000,Some("append")).is_err());assert_eq!(s.entries.len(),2);
        assert!(apply(&c,&mut s,99999,Some("append")).is_err());s.entries.push(0);assert!(apply(&c,&mut s,100000,None).is_err());
    }
}
