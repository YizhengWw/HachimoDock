//! V2 validates text ownership first, then reuses the unchanged strict v1
//! validator for common integer/choice, pages, lifecycle and input contracts.
use super::*;
pub(super) const ENGINE:&str="p4-app-form-v2";
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MixedForm{engine:String,page:String,focus_var:String,active_var:String,fields:Vec<Value>,actions:MixedActions,keyboard:Keyboard}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct MixedActions{previous:String,next:String,decrease:String,increase:String,accept:String,cancel:String,append:String,erase:String}
impl MixedActions{fn all(&self)->[&str;8]{[&self.previous,&self.next,&self.decrease,&self.increase,&self.accept,&self.cancel,&self.append,&self.erase]}}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Keyboard{index_var:String,label_var:String,characters:Vec<String>,#[serde(default="space")]space_label:String}
fn space()->String{"Space".into()}
fn printable(s:&str,max:usize)->bool{s.len()<=max&&!s.chars().any(|c|c<='\u{1f}'||('\u{7f}'..='\u{9f}').contains(&c))}
fn string_init<'a>(r:&'a Value,name:&str)->Result<&'a str,String>{
    if !identifier(name,31)||r["vars"][name]["type"]!="string"||r["limits"].get(name).is_some(){return Err(bad());}
    let s=r["vars"][name].get("init").map_or(Some(""),Value::as_str).ok_or_else(bad)?;
    if printable(s,63){Ok(s)}else{Err(bad())}
}
pub(super) fn normalized(r:&Value)->Result<Value,String>{
    crate::app_variable_owners::mixed(r)?;
    if has_nul(r){return Err(bad());}
    let f:MixedForm=serde_json::from_value(r["form"].clone()).map_err(|_|bad())?;
    if f.engine!=ENGINE||!(1..=3).contains(&f.fields.len()){return Err(bad());}
    let vars=r["vars"].as_object().filter(|v|v.len()<=8).ok_or_else(bad)?;
    for d in vars.values(){if d["type"]=="string"&&!d.get("init").map_or(Some(""),Value::as_str).is_some_and(|s|printable(s,63)){return Err(bad());}}
    let mut normalized=r.clone();let mut roles=HashSet::new();let mut ephemeral=HashSet::new();
    for name in [&f.focus_var,&f.active_var]{if !roles.insert(name.as_str()){return Err(bad());}ephemeral.insert(name.as_str());}
    let mut text_count=0;
    for (i,q) in f.fields.iter().enumerate(){
        let committed=q["var"].as_str().ok_or_else(bad)?;let draft=q["draft_var"].as_str().ok_or_else(bad)?;
        if !roles.insert(committed)||!roles.insert(draft){return Err(bad());}ephemeral.insert(draft);
        if q["kind"]=="text"{
            text_count+=1;let keys=["id","var","draft_var","kind","max_bytes"];
            if !q.as_object().is_some_and(|o|o.len()==keys.len()&&keys.iter().all(|k|o.contains_key(*k))){return Err(bad());}
            let max=q["max_bytes"].as_u64().filter(|n|(1..=63).contains(n)).ok_or_else(bad)? as usize;
            let initial=string_init(r,committed)?;if initial.len()>max||string_init(r,draft)?!=initial{return Err(bad());}
            // Validation-only projection. Never used to run, save or migrate data.
            normalized["form"]["fields"][i]=serde_json::json!({"id":q["id"],"var":committed,"draft_var":draft,"kind":"integer","min":0,"max":1,"step":1});
            for name in [committed,draft]{normalized["vars"][name]=serde_json::json!({"type":"int","init":0});}
        }else if q["kind"]=="choice"&&q["options"].as_array().is_some_and(|options|options.iter().any(|o|!o["label"].as_str().is_some_and(|s|!s.is_empty()&&printable(s,63)))){
            return Err(bad());
        }
    }
    if text_count==0{return Err(bad());}
    let k=&f.keyboard;
    if !(1..=96).contains(&k.characters.len())||k.space_label.is_empty()||!printable(&k.space_label,16){return Err(bad());}
    let mut chars=HashSet::new();
    for ch in &k.characters{if !printable(ch,4)||ch.chars().count()!=1||!chars.insert(ch){return Err(bad());}}
    for name in [&k.index_var,&k.label_var]{if !roles.insert(name.as_str()){return Err(bad());}ephemeral.insert(name.as_str());}
    let selected=if k.characters[0]==" "{&k.space_label}else{&k.characters[0]};
    if int_init(r,&k.index_var)?!=0||string_init(r,&k.label_var)?!=selected{return Err(bad());}
    limit_covers(r,&k.index_var,0,k.characters.len() as i64-1)?;
    let mut actions=HashSet::new();let transitions=r["transitions"].as_array().ok_or_else(bad)?;
    for a in f.actions.all(){
        if !identifier(a,47)||a.starts_with("host.")||a.starts_with("input.")||RESERVED.contains(&a)||!actions.insert(a){return Err(bad());}
        let found:Vec<_>=transitions.iter().filter(|t|t["on"]==a).collect();
        if found.len()!=1||*found[0]!=serde_json::json!({"from":"*","on":a})||r["input"]["hold"]==a
            ||r["input"]["press"].as_array().is_some_and(|v|v.iter().any(|x|x==a))
            ||["confirm","cooldown"].iter().any(|k|r["input"][k].get(a).is_some()){return Err(bad());}
    }
    if r["input"]["repeat"].as_array().is_some_and(|v|v.iter().any(|x|[&f.actions.accept,&f.actions.cancel,&f.actions.append,&f.actions.erase].iter().any(|a|x==*a))){return Err(bad());}
    for t in transitions{cannot_write(t,&roles)?;if let Some(otherwise)=t.get("else"){cannot_write(otherwise,&roles)?;}}
    if let Some(ticks)=r.get("tick"){for tick in ticks.as_array().ok_or_else(bad)?{cannot_write(tick,&roles)?;if let Some(then)=tick.get("then"){cannot_write(then,&roles)?;}}}
    if r["persist"]["vars"].as_array().is_some_and(|v|v.iter().any(|n|n.as_str().is_some_and(|n|ephemeral.contains(n)))){return Err(bad());}
    let form=normalized["form"].as_object_mut().ok_or_else(bad)?;form.remove("keyboard");form.insert("engine".into(),Value::String(super::ENGINE.into()));
    let actions=form["actions"].as_object_mut().ok_or_else(bad)?;actions.remove("append");actions.remove("erase");
    // Read all declared fields; malformed identifiers/pages are rejected by v1.
    let _=(&f.page,&f.focus_var,&f.active_var);
    Ok(normalized)
}
pub(super) fn bindings(r:&Value,b:&Value)->Result<(),String>{
    let f:MixedForm=serde_json::from_value(r["form"].clone()).map_err(|_|bad())?;
    let bindings=b.as_array().filter(|a|a.len()==8).ok_or_else(bad)?;
    for a in f.actions.all(){if !bindings.iter().any(|b|b["action"]==a&&b["event"].as_str().is_some_and(|e|PHYSICAL.contains(&e))){return Err(bad());}}
    Ok(())
}
#[cfg(test)]
mod tests{
    use super::*;
    #[test]fn shared_mixed_form_vectors(){
        let suite:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/app-form-v2.json")).unwrap();let r=&suite["runtime"];
        assert!(super::super::validate_runtime(r).is_ok());assert!(bindings(r,&suite["buttons"]).is_ok());
        for c in suite["invalid"].as_array().unwrap(){let mut bad=r.clone();let path=c["path"].as_array().unwrap();let mut parent=&mut bad;
            for key in &path[..path.len()-1]{parent=if let Some(s)=key.as_str(){&mut parent[s]}else{&mut parent[key.as_u64().unwrap() as usize]};}
            let key=path.last().unwrap();if let Some(s)=key.as_str(){parent[s]=c["value"].clone();}else{parent[key.as_u64().unwrap() as usize]=c["value"].clone();}
            assert!(super::super::validate_runtime(&bad).is_err(),"{c}");
        }
        assert!(super::super::require_capability(r,&serde_json::json!({"widgetAppForm":"p4-app-form-v1"})).is_err());
        assert!(super::super::require_capability(r,&serde_json::json!({"widgetAppFormV2":ENGINE})).is_ok());
        assert!(bindings(r,&serde_json::json!([])).is_err());
    }
}
