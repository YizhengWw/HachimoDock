//! Bounded stable-key collection view model, not component-specific UI or IO.
//! Data remains in the document source. Only navigation/selection is saved.
use serde::{Deserialize,Serialize};
use serde_json::{json,Value};
use std::collections::BTreeSet;

#[derive(Clone,Deserialize,Serialize)]
#[serde(deny_unknown_fields)]
pub struct Config {
    pub key:String,
    pub label:String,
    #[serde(default)]
    pub detail:Option<String>,
    pub page_size:u8,
}
#[derive(Clone,Default,Deserialize,Serialize,PartialEq,Debug)]
#[serde(deny_unknown_fields)]
pub struct State {
    pub offset:u16,
    pub selected:Option<String>,
}
pub const OPERATIONS:&[&str]=&["page_next","page_previous","up","down","clear_selection",
    "row_0","row_1","row_2","row_3","row_4","row_5","row_6","row_7"];
fn pointer(s:&str)->bool{
    if s.len()>128||!s.starts_with('/')||s.chars().any(char::is_control){return false;}
    let mut chars=s.chars();
    while let Some(c)=chars.next(){if c=='~'&&!matches!(chars.next(),Some('0'|'1')){return false;}}
    true
}
pub fn validate(c:&Config)->Result<(),String>{
    if !(1..=8).contains(&c.page_size)||!pointer(&c.key)||!pointer(&c.label)
        ||c.detail.as_ref().is_some_and(|s|!pointer(s)){
        return Err("集合视图需要有效字段路径和 1–8 个可见项".into());
    }
    Ok(())
}
fn short(s:&str)->String{
    let mut end=s.len().min(63);while !s.is_char_boundary(end){end-=1;}
    s[..end].to_string()
}
fn text(v:Option<&Value>)->Result<&str,String>{
    v.and_then(Value::as_str).filter(|s|s.len()<=512&&!s.chars().any(char::is_control))
        .ok_or_else(||"集合显示字段必须为有界文本，未修改原数据".into())
}
pub fn project(c:&Config,items:&Value,state:&mut State,operation:Option<&str>)->Result<Value,String>{
    validate(c)?;
    if !crate::component_expr::bounded(items)||state.offset>127
        ||state.selected.as_ref().is_some_and(|s|s.is_empty()||s.len()>63||s.chars().any(char::is_control)){
        return Err("集合数据或既有视图存档超出预算，原数据保留".into());
    }
    let rows=items.as_array().filter(|a|a.len()<=128).ok_or("集合源必须为至多 128 项的数组")?;
    let mut keys=BTreeSet::new();let mut parsed=Vec::with_capacity(rows.len());
    for row in rows{
        let key=text(row.pointer(&c.key))?;
        if key.is_empty()||key.len()>63||!keys.insert(key){return Err("集合条目需要唯一稳定 ID，不能使用行号代替".into());}
        let label=text(row.pointer(&c.label))?;
        let detail=match &c.detail{Some(p)=>text(row.pointer(p))?,None=>""};
        parsed.push((key,label,detail));
    }
    if operation.is_some_and(|op|!OPERATIONS.contains(&op)){return Err("未知集合视图操作".into());}
    let size=usize::from(c.page_size);
    let last=rows.len().saturating_sub(1)/size*size;
    let mut next=state.clone();let mut offset=usize::from(next.offset).min(last)/size*size;
    let selected=next.selected.as_deref().and_then(|key|parsed.iter().position(|r|r.0==key));
    if let Some(op)=operation{
        match op{
            "page_next"=>offset=(offset+size).min(last),
            "page_previous"=>offset=offset.saturating_sub(size),
            "clear_selection"=>next.selected=None,
            "up"|"down"=>{
                if rows.is_empty(){return Err("集合为空，尚未选择任何条目".into());}
                let index=match (selected,op){(Some(i),"up")=>i.saturating_sub(1),(Some(i),_)=>i.saturating_add(1).min(rows.len()-1),(None,_)=>0};
                next.selected=Some(parsed[index].0.into());offset=index/size*size;
            }
            _=>{
                let row=op.strip_prefix("row_").and_then(|s|s.parse::<usize>().ok()).ok_or("无效集合行")?;
                if row>=size||offset+row>=rows.len(){return Err("目标行不存在，未选择其他条目".into());}
                next.selected=Some(parsed[offset+row].0.into());
            }
        }
        next.offset=offset as u16;
    }
    let visible:Vec<_>=(0..size).map(|slot|{
        let i=offset+slot;
        if let Some((key,label,detail))=parsed.get(i){
            json!({"present":true,"key":key,"label":short(label),"detail":short(detail),"index":i,
                "selected":next.selected.as_deref()==Some(*key),"truncated":label.len()>63||detail.len()>63})
        }else{
            json!({"present":false,"key":"","label":"","detail":"","index":-1,"selected":false,"truncated":false})
        }
    }).collect();
    let selected=next.selected.as_deref().and_then(|key|parsed.iter().position(|r|r.0==key));
    let value=json!({"rows":visible,"count":rows.len(),"offset":offset,
        "page":offset/size,"pages":if rows.is_empty(){0}else{last/size+1},
        "selectionKey":next.selected,
        "selectedLabel":selected.map(|i|short(parsed[i].1)).unwrap_or_default(),
        "selected":selected.map(|i|json!({"key":parsed[i].0,"label":parsed[i].1,"detail":parsed[i].2,"index":i,"value":rows[i]})),
        "canPrevious":offset>0,"canNext":offset<last,
        "canUp":!rows.is_empty()&&selected.is_none_or(|i|i>0),
        "canDown":!rows.is_empty()&&selected.is_none_or(|i|i+1<rows.len())});
    if !crate::component_expr::bounded(&value){return Err("集合可见快照超出预算，原状态保留".into());}
    // Status can clamp a rendered page after filtering, but does not silently
    // rewrite saved selection/page. Only an explicit action changes the model.
    if operation.is_some(){*state=next;}
    Ok(value)
}
pub fn enabled(value:&Value,operation:&str)->bool{
    match operation{
        "page_next"=>value["canNext"]==true,
        "page_previous"=>value["canPrevious"]==true,
        "up"=>value["canUp"]==true,"down"=>value["canDown"]==true,
        "clear_selection"=>value["selectionKey"].is_string(),
        _=>operation.strip_prefix("row_").and_then(|s|s.parse::<usize>().ok())
            .is_some_and(|i|OPERATIONS.contains(&operation)&&value["rows"].get(i).is_some_and(|row|row["present"]==true)),
    }
}

#[cfg(test)]
mod tests{
    use super::*;
    fn config()->Config{Config{key:"/id".into(),label:"/label".into(),detail:None,page_size:2}}
    fn rows()->Value{json!([{"id":"a","label":"A"},{"id":"b","label":"B"},{"id":"c","label":"C"}])}
    #[test]fn shared_native_collection_projection_contract_preserves_legacy_service_semantics(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/collection-model-v1.json")).unwrap();
        let c=Config{key:"/key".into(),label:"/label".into(),detail:Some("/detail".into()),page_size:suite["pageSize"].as_u64().unwrap() as u8};
        let mut state:State=serde_json::from_value(suite["initialState"].clone()).unwrap();
        let mut data=suite["rows"].clone();let mut previous=project(&c,&data,&mut state,None).unwrap();
        for step in suite["steps"].as_array().unwrap(){
            let candidate=step.get("rows").unwrap_or(&data);
            let op=step["on"].as_str().unwrap();let saved=state.clone();
            let result=project(&c,candidate,&mut state,if op=="status"{None}else{Some(op)});
            if step.get("error").is_some(){
                assert!(result.is_err());assert_eq!(state,saved);
                assert_eq!(project(&c,&data,&mut state,None).unwrap(),previous);continue;
            }
            let v=result.unwrap();
            assert_eq!(json!({"state":state,"offset":v["offset"],"selectedIndex":v["selected"]["index"].as_i64().unwrap_or(-1),
                "labels":v["rows"].as_array().unwrap().iter().map(|r|r["label"].clone()).collect::<Vec<_>>()}),step["expected"]);
            previous=v;if let Some(rows)=step.get("rows"){data=rows.clone();}
        }
    }
    #[test]fn stable_selection_survives_insert_reorder_and_filter(){
        let mut state=State::default();project(&config(),&rows(),&mut state,Some("row_1")).unwrap();
        assert_eq!(state.selected.as_deref(),Some("b"));
        let changed=json!([{"id":"c","label":"C"},{"id":"a","label":"A"},{"id":"b","label":"B"}]);
        let value=project(&config(),&changed,&mut state,None).unwrap();assert_eq!(value["selected"]["index"],2);
        let saved=state.clone();let filtered=json!([{"id":"a","label":"A"}]);
        assert!(project(&config(),&filtered,&mut state,None).unwrap()["selected"].is_null());assert_eq!(state,saved);
    }
    #[test]fn page_bounds_empty_rows_and_failures_do_not_mutate(){
        let mut state=State::default();assert!(project(&config(),&json!([]),&mut state,Some("row_0")).is_err());
        project(&config(),&rows(),&mut state,Some("page_next")).unwrap();assert_eq!(state.offset,2);
        let before=state.clone();assert!(project(&config(),&rows(),&mut state,Some("row_1")).is_err());assert_eq!(state,before);
        let duplicates=json!([{"id":"a","label":"one"},{"id":"a","label":"two"}]);
        assert!(project(&config(),&duplicates,&mut state,Some("down")).is_err());assert_eq!(state,before);
        assert!(project(&config(),&rows(),&mut state,Some("remove")).is_err());assert_eq!(state,before);
    }
    #[test]fn utf8_display_is_explicitly_bounded_without_losing_document_text(){
        let data=json!([{"id":"key","label":"字".repeat(40)}]);let mut state=State::default();
        let v=project(&config(),&data,&mut state,Some("row_0")).unwrap();
        assert_eq!(v["rows"][0]["label"].as_str().unwrap().len(),63);assert_eq!(v["rows"][0]["truncated"],true);
        assert_eq!(v["selected"]["label"],data[0]["label"]);
    }
}
