//! Package-owned offline calendar. The host selects dates; it does not invent
//! auspicious days, fetch location, or treat folk customs as statutory holidays.
use chrono::{Datelike, Duration, NaiveDate, TimeZone, Utc};
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};

#[derive(Clone, Default, PartialEq, Serialize, Deserialize)]
#[serde(default)]
pub struct Reading {
    pub date: Option<String>,
    pub details: bool,
    pub page: usize,
}

pub fn validate(records: &[Value]) -> Result<(), String> {
    let invalid = || "黄历数据必须为连续有效日期与有界民俗资料".to_owned();
    if records.is_empty() || records.len() > 8000 { return Err(invalid()); }
    let mut previous: Option<NaiveDate> = None;
    for record in records {
        const KEYS: [&str; 7] = ["date", "lunar", "ganzhi", "term", "festivals", "yi", "ji"];
        if record.as_object().is_none_or(|o| o.len() != KEYS.len() || o.keys().any(|k| !KEYS.contains(&k.as_str()))) { return Err(invalid()); }
        let Some(raw) = record["date"].as_str() else { return Err(invalid()); };
        let date = NaiveDate::parse_from_str(raw, "%Y-%m-%d").map_err(|_| invalid())?;
        if date.format("%Y-%m-%d").to_string() != raw || !(1901..=2100).contains(&date.year()) || previous.is_some_and(|p| p.succ_opt() != Some(date)) { return Err(invalid()); }
        previous = Some(date);
        for (key, limit, optional) in [("lunar", 60, false), ("ganzhi", 60, false), ("term", 30, true)] {
            if record[key].as_str().is_none_or(|s| (!optional && s.trim().is_empty()) || s.len() > limit || s.chars().any(char::is_control)) { return Err(invalid()); }
        }
        for key in ["festivals", "yi", "ji"] {
            if record[key].as_array().is_none_or(|a| a.len() > 32 || a.iter().any(|v| v.as_str().is_none_or(|s| s.trim().is_empty() || s.len() > 90 || s.chars().any(char::is_control)))) { return Err(invalid()); }
        }
    }
    Ok(())
}

fn today(timestamp: i64) -> Result<NaiveDate, String> {
    Utc.timestamp_opt(timestamp, 0).single().map(|v| v.with_timezone(&chrono_tz::Asia::Shanghai).date_naive()).ok_or("电脑时间无效，请校准时钟".into())
}
fn selected(s: &Reading, timestamp: i64) -> Result<NaiveDate, String> {
    match s.date.as_deref() { Some(date) => NaiveDate::parse_from_str(date, "%Y-%m-%d").map_err(|_| "日历选择记录无效，请返回今天".into()), None => today(timestamp) }
}
fn record<'a>(date: NaiveDate, records: &'a [Value]) -> Option<&'a Value> {
    let key = date.format("%Y-%m-%d").to_string();
    records.binary_search_by(|r| r["date"].as_str().unwrap_or("").cmp(&key)).ok().map(|i| &records[i])
}
fn detail_text(r: &Value) -> String {
    let items = |key: &str| r[key].as_array().unwrap().iter().filter_map(Value::as_str).collect::<Vec<_>>().join("、");
    format!("传统宜：{}。传统忌：{}。这些内容来自传统历书，属于民俗文化参考，不预测吉凶，不代替医疗、投资、法律等现实决策。节日标记不等于法定休假。", items("yi"), items("ji"))
}
pub fn apply(s: &mut Reading, records: &[Value], op: &str, timestamp: i64) -> Result<(), String> {
    if op == "today" { *s = Reading::default(); }
    let date = selected(s, timestamp)?;
    match op {
        "details" => { s.details = !s.details; s.page = 0; },
        "previous" | "next" if s.details => {
            s.page = if op == "previous" { s.page.saturating_sub(1) } else { s.page.saturating_add(1) };
        },
        "previous" | "next" => {
            if let Some(next) = date.checked_add_signed(Duration::days(if op == "previous" { -1 } else { 1 })) {
                if record(next, records).is_some() { s.date = Some(next.format("%Y-%m-%d").to_string()); s.page = 0; }
            }
        },
        _ => {}
    }
    if let Some(r) = record(selected(s, timestamp)?, records) {
        s.page = s.page.min(crate::knowledge_library::segments(&detail_text(r)).len().div_ceil(2).saturating_sub(1));
    } else { s.details = false; s.page = 0; }
    Ok(())
}
pub fn view(s: &Reading, records: &[Value], timestamp: i64) -> Result<Value, String> {
    let date = selected(s, timestamp)?;
    let weekday = ["一", "二", "三", "四", "五", "六", "日"][date.weekday().num_days_from_monday() as usize];
    let Some(r) = record(date, records) else {
        return Ok(json!({"eyebrow":date.format("%Y-%m-%d").to_string(),"headline":"日期超出资源范围","metricValue":"—","metricLabel":"回到今天","metricUnit":"宜忌详情","badge":"无数据","note":"请更新黄历资源包，或核对电脑日期。","footer":"不会套用其他年份的数据。"}));
    };
    let festivals = r["festivals"].as_array().unwrap().iter().filter_map(Value::as_str).collect::<Vec<_>>().join(" · ");
    let mut v = json!({"eyebrow":format!("{}年{}月 · 星期{weekday}",date.year(),date.month()),"headline":r["lunar"],"metricValue":format!("{:02}",date.day()),"metricLabel":"回到今天","metricUnit":"宜忌详情","badge":if date==today(timestamp)?{"今天"}else{"浏览"},"note":format!("{}{}{}",r["term"].as_str().unwrap(),if r["term"]!=""&&!festivals.is_empty(){" · "}else{""},festivals),"footer":r["ganzhi"]});
    if s.details {
        let chunks = crate::knowledge_library::segments(&detail_text(r)); let count = chunks.len().div_ceil(2); let page = s.page.min(count-1);
        v["eyebrow"] = date.format("%Y-%m-%d · 民俗参考").to_string().into();
        v["headline"] = "传统宜忌".into(); v["metricValue"] = "".into(); v["metricUnit"] = "返回日历".into();
        v["badge"] = format!("{}/{}",page+1,count).into(); v["note"] = chunks[page*2].clone().into(); v["footer"] = chunks.get(page*2+1).cloned().unwrap_or_default().into();
    }
    Ok(v)
}
pub fn operations(s: &Reading, records: &[Value], timestamp: i64) -> Vec<&'static str> {
    let mut out = vec!["today"];
    let Ok(date) = selected(s, timestamp) else { return out; };
    if let Some(r) = record(date, records) {
        out.push("details");
        if s.details {
            if s.page > 0 { out.push("previous"); }
            if s.page+1 < crate::knowledge_library::segments(&detail_text(r)).len().div_ceil(2) { out.push("next"); }
        } else {
            if date.pred_opt().is_some_and(|d|record(d,records).is_some()) { out.push("previous"); }
            if date.succ_opt().is_some_and(|d|record(d,records).is_some()) { out.push("next"); }
        }
    }
    out
}

#[cfg(test)] mod tests {
    use super::*;
    fn records() -> Vec<Value> { ["2026-02-16","2026-02-17","2026-02-18"].iter().map(|d|json!({"date":d,"lunar":"正月初一","ganzhi":"丙午年","term":"","festivals":["春节"],"yi":["祭祀"],"ji":["出行"]})).collect() }
    fn time(s: &str) -> i64 { chrono::DateTime::parse_from_rfc3339(s).unwrap().timestamp() }
    #[test] fn china_midnight_rollover_browse_is_absolute_and_today_resets() {
        let records=records();let mut s=Reading::default();let before=time("2026-02-16T15:59:59Z");let after=before+1;
        assert_eq!(view(&s,&records,before).unwrap()["metricValue"],"16");assert_eq!(view(&s,&records,after).unwrap()["metricValue"],"17");
        apply(&mut s,&records,"previous",after).unwrap();assert_eq!(s.date.as_deref(),Some("2026-02-16"));
        assert_eq!(view(&s,&records,after+86400).unwrap()["metricValue"],"16");
        apply(&mut s,&records,"today",after+86400).unwrap();assert_eq!(view(&s,&records,after+86400).unwrap()["metricValue"],"18");
    }
    #[test] fn details_are_lossless_and_bounds_do_not_wrap_or_fabricate() {
        let r=records();let mut s=Reading::default();let t=time("2026-02-16T20:00:00Z");apply(&mut s,&r,"details",t).unwrap();
        let mut joined=String::new();loop{let v=view(&s,&r,t).unwrap();joined+=v["note"].as_str().unwrap();joined+=v["footer"].as_str().unwrap();if !operations(&s,&r,t).contains(&"next"){break;}apply(&mut s,&r,"next",t).unwrap();}
        assert_eq!(joined,detail_text(&r[1]));
        apply(&mut s,&r,"today",t).unwrap();apply(&mut s,&r,"previous",t).unwrap();apply(&mut s,&r,"previous",t).unwrap();assert_eq!(s.date.as_deref(),Some("2026-02-16"));
        assert!(!operations(&s,&r,t).contains(&"previous"));assert_eq!(view(&Reading::default(),&r,t+86400*30).unwrap()["badge"],"无数据");
    }
    #[test] fn rejects_missing_duplicate_dates_bad_schema_and_control_text() {
        let r=records();assert!(validate(&r).is_ok());let mut bad=r.clone();bad.swap(0,1);assert!(validate(&bad).is_err());
        let mut bad=r.clone();bad.remove(1);assert!(validate(&bad).is_err());
        let mut bad=r.clone();bad[0]["date"]="2026-02-30".into();assert!(validate(&bad).is_err());
        let mut bad=r.clone();bad[0]["yi"]=json!(["\n"]);assert!(validate(&bad).is_err());
    }
    #[test] #[ignore="new resource package integration; isolated store, no device or audio"]
    fn package_host_integration_and_render_snapshots() {
        let package=std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../new-components/daily-almanac");
        let runtime:Value=serde_json::from_slice(&std::fs::read(package.join("runtime/widget.json")).unwrap()).unwrap();
        let bytes=std::fs::read(package.join("pc/application.json")).unwrap();
        let declaration=crate::app_services::parse(&bytes,&runtime).unwrap();
        assert_eq!(declaration.content.len(),5844);
        assert_eq!(crate::app_services::permissions(&declaration)["deviceAudio"],false);
        let mut wrong:Value=serde_json::from_slice(&bytes).unwrap();wrong["autoRead"]=true.into();
        assert!(crate::app_services::parse(&serde_json::to_vec(&wrong).unwrap(),&runtime).is_err());
        let dir=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
        let request=|op|crate::component_app::test_request(&package,dir.path(),json!({"operation":op}),&usb).unwrap();
        let first=request("today");assert_eq!(first["content"],json!([]));assert!(serde_json::to_vec(&first).unwrap().len()<8000);
        let previous=request("previous");assert_ne!(previous["state"]["calendar"]["date"],first["state"]["calendar"]["date"]);
        assert_eq!(request("status")["state"]["calendar"],previous["state"]["calendar"]);
        let details=request("details");assert_eq!(details["ui"]["mode"],1);assert_eq!(request("details")["ui"]["mode"],0);
        let mut s=Reading::default();let t=time("2026-02-17T04:00:00Z");let mut snapshots=serde_json::Map::new();
        let snap=|s:&Reading|json!({"values":view(s,&declaration.content,t).unwrap(),"ui":{"mode":if s.details{1}else{0},"actions":declaration.actions.iter().filter(|(_,op)|operations(s,&declaration.content,t).contains(&op.as_str())).map(|(k,_)|k).collect::<Vec<_>>()}});
        snapshots.insert("today".into(),snap(&s));apply(&mut s,&declaration.content,"details",t).unwrap();snapshots.insert("details".into(),snap(&s));
        apply(&mut s,&declaration.content,"next",t).unwrap();snapshots.insert("detail-page-2".into(),snap(&s));
        s.date=Some("2025-07-25".into());s.details=false;snapshots.insert("leap-month".into(),snap(&s));
        s.date=Some("2026-04-05".into());snapshots.insert("solar-term".into(),snap(&s));
        if let Ok(path)=std::env::var("PET_CALENDAR_SNAPSHOTS"){std::fs::write(path,serde_json::to_vec_pretty(&snapshots).unwrap()).unwrap();}
    }
}
