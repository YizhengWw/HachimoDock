//! Data-only catalog evidence validation. Wire fields are owned jointly with
//! the firmware protocol owner; no component-ID policy or device actions here.
use serde::{Deserialize,Serialize};
use serde_json::Value;
use std::collections::BTreeSet;

pub const MAX_VISIBLE:usize=32;
pub const MAX_PHYSICAL:usize=33; // existing private acceptance profile; no slot allocation here
pub const ENGINE:&str="p4-catalog-metadata-v1";
fn invalid()->String{"catalog_evidence_unverified".into()}
pub(crate) fn valid_board(s:&str)->bool{!s.is_empty()&&s.len()<=47&&s.bytes().all(|b|b.is_ascii_alphanumeric()||b"._-".contains(&b))}
pub(crate) fn valid_id(s:&str)->bool{crate::app_program_background::valid_widget_id(s)}
pub(crate) fn hex(s:&str,n:usize)->bool{s.len()==n&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))}
fn text<'a>(value:&'a Value,key:&str)->Result<&'a str,String>{value[key].as_str().ok_or_else(invalid)}
fn ids(values:&[String])->Result<BTreeSet<&str>,String>{
    if values.len()>MAX_VISIBLE{return Err(invalid());}
    let mut unique=BTreeSet::new();
    for value in values{if !valid_id(value)||!unique.insert(value.as_str()){return Err(invalid());}}
    Ok(unique)
}
#[derive(Clone,Debug,PartialEq,Eq)]
struct EvidenceEntry{id:String,resource_state:String,manifest_state:String,image_count:u32}
#[derive(Clone,Debug)]
struct InventoryEvidence{board:String,items:Vec<EvidenceEntry>,active:Option<String>,selected:Option<String>}
fn optional_id(value:&Value,key:&str)->Result<Option<String>,String>{
    match value.get(key){Some(Value::Null)=>Ok(None),Some(Value::String(id)) if valid_id(id)=>Ok(Some(id.clone())),_=>Err(invalid())}
}
fn inventory_evidence(value:&Value)->Result<InventoryEvidence,String>{
    if serde_json::to_vec(value).map_err(|_|invalid())?.len()>32768{return Err(invalid());}
    let board=text(value,"boardDeviceId")?;
    if !valid_board(board)||value["schemaVersion"]!=1||value["ok"]!=true||value["complete"]!=true||value["recoveryBlocked"]!=false{return Err(invalid());}
    if !value["warnings"].as_array().is_some_and(|a|a.is_empty()){return Err(invalid());}
    let rows=value["items"].as_array().filter(|a|a.len()<=MAX_VISIBLE).ok_or_else(invalid)?;
    let mut unique=BTreeSet::new();let mut items=Vec::with_capacity(rows.len());let mut active_rows=Vec::new();
    for row in rows{
        let id=text(row,"id")?;
        if !valid_id(id)||!unique.insert(id){return Err(invalid());}
        let resource=text(row,"resourceState")?;let manifest=text(row,"manifestState")?;
        if !matches!((resource,manifest),("ready","valid")|("bootstrap","resources-incomplete")){return Err(invalid());}
        let image_count=row["imageCount"].as_u64().and_then(|n|u32::try_from(n).ok()).filter(|n|*n<=255).ok_or_else(invalid)?;
        match row["active"].as_bool(){Some(true)=>active_rows.push(id.to_owned()),Some(false)=>{},None=>return Err(invalid())}
        items.push(EvidenceEntry{id:id.into(),resource_state:resource.into(),manifest_state:manifest.into(),image_count});
    }
    let active=optional_id(value,"activeWidgetId")?;let selected=optional_id(value,"selectedWidgetId")?;
    if active.as_ref().is_some_and(|id|!unique.contains(id.as_str()))||selected.as_ref().is_some_and(|id|!unique.contains(id.as_str()))
        ||active_rows.len()>1||active_rows.first()!=active.as_ref(){return Err(invalid());}
    match (selected.as_ref(),value["selectedIndex"].as_u64()){
        (Some(id),Some(index)) if items.get(index as usize).is_some_and(|row|&row.id==id)=>{},
        (None,None) if value["selectedIndex"].is_null()&&items.is_empty()=>{},
        _=>return Err(invalid()),
    }
    Ok(InventoryEvidence{board:board.into(),items,active,selected})
}
#[derive(Clone,Debug,PartialEq,Eq,Serialize)]
#[serde(rename_all="camelCase")]
pub struct RestoreDelta{pub board_id:String,pub remove:Vec<String>,pub visible_order:Vec<String>}

/// A preview, never permission to write. The caller must explicitly supply the
/// exact removal set, then bind it to fresh native metadata/boot/sequence/digest.
/// These old inventories are VISIBLE projections: hidden records are outside
/// the removal set and must be preserved by the native metadata transaction.
pub fn restore_delta(before:&Value,after:&Value,explicit_remove:&[String])->Result<RestoreDelta,String>{
    let before=inventory_evidence(before)?;let after=inventory_evidence(after)?;
    if before.board!=after.board{return Err(invalid());}
    let before_ids=before.items.iter().map(|r|r.id.as_str()).collect::<BTreeSet<_>>();
    for row in &before.items{
        if !after.items.iter().any(|current|current==row){return Err(invalid());}
    }
    let removed=ids(explicit_remove)?;
    let difference=after.items.iter().filter(|r|!before_ids.contains(r.id.as_str())).map(|r|r.id.as_str()).collect::<BTreeSet<_>>();
    if removed!=difference||after.active.as_ref().is_some_and(|id|removed.contains(id.as_str()))
        ||after.selected.as_ref().is_some_and(|id|removed.contains(id.as_str())){return Err(invalid());}
    Ok(RestoreDelta{board_id:before.board,remove:explicit_remove.to_vec(),visible_order:before.items.into_iter().map(|r|r.id).collect()})
}

pub fn supported(caps:&Value)->bool{caps["widgetCatalogMetadata"]==ENGINE}
#[derive(Clone,Debug,PartialEq,Eq,Serialize)]
#[serde(rename_all="camelCase")]
pub struct Entry{pub id:String,pub visible:bool,pub slot:u8,pub package_generation:u8,
    pub widget_checksum:String,pub buttons_checksum:String,pub sprites_checksum:String,pub resource_state:String,pub image_count:u8}
#[derive(Clone,Debug,Serialize)]
#[serde(rename_all="camelCase")]
pub struct Snapshot{pub schema_version:u32,pub engine:&'static str,pub board_device_id:String,pub boot_id:String,
    pub complete:bool,pub recovery_blocked:bool,pub catalog_seq:u32,pub catalog_digest:String,pub physical_count:usize,
    pub active_widget_id:Option<String>,pub selected_widget_id:Option<String>,pub items:Vec<Entry>}
fn uint(value:&Value,key:&str)->Result<u32,String>{value[key].as_u64().and_then(|n|u32::try_from(n).ok()).ok_or_else(invalid)}
pub fn snapshot(value:&Value,board:&str,request_id:&str)->Result<Snapshot,String>{
    if serde_json::to_vec(value).map_err(|_|invalid())?.len()>16384{return Err(invalid());}
    if !valid_board(board)||value["schemaVersion"]!=1||value["engine"]!=ENGINE||value["boardDeviceId"]!=board
        ||value["requestId"]!=request_id||value["ok"]!=true||value["complete"]!=true||value["recoveryBlocked"]!=false{return Err(invalid());}
    let boot=text(value,"bootId")?;let digest=text(value,"catalogDigest")?;
    if !hex(boot,16)||!hex(digest,8){return Err(invalid());}
    let seq=uint(value,"catalogSeq")?;let rows=value["items"].as_array().filter(|a|a.len()<=MAX_PHYSICAL).ok_or_else(invalid)?;
    if value["physicalCount"].as_u64()!=Some(rows.len() as u64){return Err(invalid());}
    let mut names=BTreeSet::new();let mut slots=BTreeSet::new();let mut items=Vec::with_capacity(rows.len());
    for row in rows{
        let id=text(row,"id")?;let slot=uint(row,"slot")?;let generation=uint(row,"packageGeneration")?;
        if !valid_id(id)||!names.insert(id)||slot>=MAX_PHYSICAL as u32||!slots.insert(slot)||generation>1{return Err(invalid());}
        let visible=row["visible"].as_bool().ok_or_else(invalid)?;
        let sums=["widgetChecksum","buttonsChecksum","spritesChecksum"].map(|key|text(row,key));
        let [widget,buttons,sprites]=sums;
        let (widget,buttons,sprites)=(widget?,buttons?,sprites?);
        if ![widget,buttons,sprites].iter().all(|s|hex(s,8)){return Err(invalid());}
        let resource=text(row,"resourceState")?;
        if !matches!(resource,"ready"|"bootstrap"|"damaged"|"unchecked"){return Err(invalid());}
        let images=uint(row,"imageCount")?;if images>255{return Err(invalid());}
        items.push(Entry{id:id.into(),visible,slot:slot as u8,package_generation:generation as u8,widget_checksum:widget.into(),
            buttons_checksum:buttons.into(),sprites_checksum:sprites.into(),resource_state:resource.into(),image_count:images as u8});
    }
    if items.iter().filter(|r|r.visible).count()>MAX_VISIBLE{return Err(invalid());}
    let active=optional_id(value,"activeWidgetId")?;let selected=optional_id(value,"selectedWidgetId")?;
    if active.iter().chain(selected.iter()).any(|id|!names.contains(id.as_str())){return Err(invalid());}
    Ok(Snapshot{schema_version:1,engine:ENGINE,board_device_id:board.into(),boot_id:boot.into(),complete:true,recovery_blocked:false,
        catalog_seq:seq,catalog_digest:digest.into(),physical_count:items.len(),active_widget_id:active,selected_widget_id:selected,items})
}
/// Exact FW contract excluding only the native-generated requestId. No paths,
/// component bytes, cleanup flags, activation or arbitrary extension fields.
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub struct ReconcileRequest{pub expected_board_device_id:String,pub expected_boot_id:String,pub expected_catalog_seq:u32,
    pub expected_catalog_digest:String,pub remove_visible_ids:Vec<String>,pub survivor_visible_order:Vec<String>,pub preserve_save:bool}
impl ReconcileRequest{
    pub fn validate(&self)->Result<(),String>{
        if !valid_board(&self.expected_board_device_id)||!hex(&self.expected_boot_id,16)||!hex(&self.expected_catalog_digest,8)||!self.preserve_save{return Err(invalid());}
        ids(&self.remove_visible_ids)?;ids(&self.survivor_visible_order)?;
        if self.remove_visible_ids.iter().any(|id|self.survivor_visible_order.contains(id)){return Err(invalid());}
        Ok(())
    }
    pub(crate) fn payload(&self,request_id:&str)->Result<Value,String>{
        self.validate()?;let mut value=serde_json::to_value(self).map_err(|_|invalid())?;value["requestId"]=request_id.into();
        if serde_json::to_vec(&value).map_err(|_|invalid())?.len()>4096{return Err(invalid());}Ok(value)
    }
}
#[derive(Clone,Debug,Deserialize)]
#[serde(rename_all="camelCase",deny_unknown_fields)]
pub struct RecoveryEvidence{pub before_inventory:Value,pub after_inventory:Value}
#[derive(Clone,Debug)]
pub struct Prepared{pub request:ReconcileRequest,pub physical_survivors:Vec<Entry>,pub no_op:bool}
pub fn prepare(current:&Snapshot,request:&ReconcileRequest,evidence:Option<&RecoveryEvidence>)->Result<Prepared,String>{
    request.validate()?;
    if current.board_device_id!=request.expected_board_device_id||current.boot_id!=request.expected_boot_id||current.catalog_seq!=request.expected_catalog_seq
        ||current.catalog_digest!=request.expected_catalog_digest||!current.complete||current.recovery_blocked{return Err(invalid());}
    if !request.remove_visible_ids.is_empty()&&evidence.is_none(){return Err("catalog_removal_evidence_required".into());}
    if let Some(evidence)=evidence{
        let delta=restore_delta(&evidence.before_inventory,&evidence.after_inventory,&request.remove_visible_ids)?;
        if delta.board_id!=current.board_device_id||delta.visible_order!=request.survivor_visible_order{return Err(invalid());}
        let after=inventory_evidence(&evidence.after_inventory)?;
        let live=current.items.iter().filter(|r|r.visible).collect::<Vec<_>>();
        if live.len()!=after.items.len()||after.items.iter().any(|old|!live.iter().any(|row|row.id==old.id&&row.resource_state==old.resource_state&&u32::from(row.image_count)==old.image_count)){return Err(invalid());}
    }
    let removes=ids(&request.remove_visible_ids)?;let order=ids(&request.survivor_visible_order)?;
    let survivors=current.items.iter().filter(|row|row.visible&&!removes.contains(row.id.as_str())).map(|row|row.id.as_str()).collect::<BTreeSet<_>>();
    if survivors!=order||removes.iter().any(|id|!current.items.iter().any(|row|row.visible&&row.id==*id))
        ||current.active_widget_id.iter().chain(current.selected_widget_id.iter()).any(|id|removes.contains(id.as_str())){return Err(invalid());}
    let mut visible=request.survivor_visible_order.iter();let mut result=Vec::new();
    for row in &current.items{
        if removes.contains(row.id.as_str()){continue;}
        if row.visible{let id=visible.next().ok_or_else(invalid)?;result.push(current.items.iter().find(|r|&r.id==id).ok_or_else(invalid)?.clone());}
        else{result.push(row.clone());}
    }
    let no_op=result==current.items;
    if !no_op&&current.catalog_seq==u32::MAX{return Err(invalid());}
    Ok(Prepared{request:request.clone(),physical_survivors:result,no_op})
}
#[derive(Clone,Debug,Serialize,Deserialize)]
#[serde(rename_all="camelCase")]
pub struct Receipt{pub commit_state:String,pub catalog_seq:u32,pub catalog_digest:String}
pub fn receipt(value:&Value,request_id:&str,prepared:&Prepared)->Result<Receipt,String>{
    let r=&prepared.request;
    if value["schemaVersion"]!=1||value["engine"]!=ENGINE||value["requestId"]!=request_id||value["boardDeviceId"]!=r.expected_board_device_id
        ||value["bootId"]!=r.expected_boot_id||value["ok"]!=true{return Err(invalid());}
    let state=text(value,"commitState")?;let seq=uint(value,"catalogSeq")?;let digest=text(value,"catalogDigest")?;
    if !hex(digest,8){return Err(invalid());}
    match (prepared.no_op,state){
        (true,"unchanged") if seq==r.expected_catalog_seq&&digest==r.expected_catalog_digest=>{},
        (false,"committed") if r.expected_catalog_seq.checked_add(1)==Some(seq)=>{},
        _=>return Err(invalid()),
    }
    Ok(Receipt{commit_state:state.into(),catalog_seq:seq,catalog_digest:digest.into()})
}
#[derive(Clone,Debug,Serialize)]
#[serde(rename_all="camelCase")]
pub struct Outcome{pub confirmed:bool,pub read_only:bool,pub code:&'static str,pub receipt:Option<Receipt>}
impl Outcome{
    pub(crate) fn unknown()->Self{Self{confirmed:false,read_only:true,code:"result_unknown_no_replay",receipt:None}}
    pub(crate) fn confirmed(receipt:Receipt)->Self{Self{confirmed:true,read_only:false,code:"catalog_confirmed",receipt:Some(receipt)}}
}

mod journal;
pub(crate) use journal::Journal;

#[tauri::command]
pub async fn usb_catalog_inspect(usb_manager:tauri::State<'_,crate::usb_serial::UsbSerialManager>,expected_board_device_id:String)->Result<Snapshot,String>{
    let manager=usb_manager.inner().clone();tauri::async_runtime::spawn_blocking(move||manager.catalog_inspect(&expected_board_device_id)).await.map_err(|_|"catalog_worker_unavailable")?
}
#[tauri::command]
pub async fn usb_catalog_reconcile(app:tauri::AppHandle,usb_manager:tauri::State<'_,crate::usb_serial::UsbSerialManager>,request:ReconcileRequest,
    evidence:Option<RecoveryEvidence>,user_confirmed:bool)->Result<Outcome,String>{
    use tauri::Manager;
    if !user_confirmed{return Err("catalog_explicit_confirmation_required".into());}request.validate()?;
    let directory=app.path().app_data_dir().map_err(|_|"catalog_journal_unavailable")?.join("catalog-maintenance");
    let manager=usb_manager.inner().clone();tauri::async_runtime::spawn_blocking(move||{
        static JOURNAL:std::sync::OnceLock<Result<std::sync::Arc<Journal>,String>>=std::sync::OnceLock::new();
        let journal=JOURNAL.get_or_init(||Journal::open(&directory).map(std::sync::Arc::new));
        manager.catalog_reconcile(&request,evidence.as_ref(),journal.as_ref().map_err(Clone::clone)?)
    }).await.map_err(|_|"catalog_worker_unavailable")?
}

#[cfg(test)]mod tests{
    use super::*;
    use serde_json::json;
    fn evidence(order:&[&str])->Value{
        let active=order.iter().copied().filter(|id|id.starts_with("keep_")).min().or_else(||order.first().copied());
        json!({"schemaVersion":1,"boardDeviceId":"board-a","ok":true,"complete":true,"recoveryBlocked":false,"warnings":[],
            "activeWidgetId":active,"selectedWidgetId":active,"selectedIndex":order.iter().position(|id|Some(*id)==active),
            "items":order.iter().map(|id|json!({"id":id,"active":Some(*id)==active,"resourceState":"ready","manifestState":"valid","imageCount":2})).collect::<Vec<_>>()})
    }
    fn metadata()->Value{
        let mut items=vec![];
        for (slot,id,visible) in [(0,"extra_x",true),(1,"retained_shadow",false),(2,"keep_a",true),(3,"keep_b",true)]{
            items.push(json!({"id":id,"visible":visible,"slot":slot,"packageGeneration":1,"widgetChecksum":"11111111","buttonsChecksum":"22222222",
                "spritesChecksum":"33333333","resourceState":"ready","imageCount":2}));
        }
        json!({"schemaVersion":1,"engine":ENGINE,"requestId":"request-a","boardDeviceId":"board-a","bootId":"0123456789abcdef","ok":true,
            "complete":true,"recoveryBlocked":false,"catalogSeq":7,"catalogDigest":"12345678","physicalCount":4,
            "activeWidgetId":"keep_a","selectedWidgetId":"keep_a","items":items})
    }
    fn request()->ReconcileRequest{ReconcileRequest{expected_board_device_id:"board-a".into(),expected_boot_id:"0123456789abcdef".into(),
        expected_catalog_seq:7,expected_catalog_digest:"12345678".into(),remove_visible_ids:vec!["extra_x".into()],survivor_visible_order:vec!["keep_b".into(),"keep_a".into()],preserve_save:true}}
    fn recovery()->RecoveryEvidence{RecoveryEvidence{before_inventory:evidence(&["keep_b","keep_a"]),after_inventory:evidence(&["extra_x","keep_a","keep_b"])}}
    #[test]fn full_physical_metadata_and_exact_visible_survivors_preserve_hidden_content(){
        let current=snapshot(&metadata(),"board-a","request-a").unwrap();let prepared=prepare(&current,&request(),Some(&recovery())).unwrap();
        assert_eq!(prepared.physical_survivors.iter().map(|r|r.id.as_str()).collect::<Vec<_>>(),vec!["retained_shadow","keep_b","keep_a"]);
        assert_eq!(prepared.physical_survivors[0],current.items[1]);assert!(!prepared.no_op);
    }
    #[test]fn native_metadata_rejects_partial_wrong_owner_duplicate_slot_and_invalid_checksums(){
        for (path,value) in [("/complete",json!(false)),("/recoveryBlocked",json!(true)),("/physicalCount",json!(3)),("/catalogSeq",json!(4294967296u64)),
            ("/bootId",json!("0123456789abcdeF")),("/catalogDigest",json!("1234567G")),("/items/1/slot",json!(0)),("/items/1/id",json!("keep_a")),
            ("/items/0/widgetChecksum",json!("uppercase")),("/items/0/visible",json!(1)),("/items/0/packageGeneration",json!(2))]{
            let mut bad=metadata();*bad.pointer_mut(path).unwrap()=value;assert!(snapshot(&bad,"board-a","request-a").is_err(),"{path}");
        }
        assert!(snapshot(&metadata(),"other","request-a").is_err());assert!(snapshot(&metadata(),"board-a","wrong-request").is_err());
        assert!(!supported(&json!({"widgetInventory":true})));assert!(supported(&json!({"widgetCatalogMetadata":ENGINE})));
    }
    #[test]fn hidden_active_selected_and_stale_cas_cannot_be_removed_or_reinterpreted(){
        let current=snapshot(&metadata(),"board-a","request-a").unwrap();
        for id in ["retained_shadow","keep_a","unknown"]{let mut r=request();r.remove_visible_ids=vec![id.into()];assert!(prepare(&current,&r,Some(&recovery())).is_err());}
        for field in 0..4{let mut r=request();match field{0=>r.expected_board_device_id="other".into(),1=>r.expected_boot_id="1123456789abcdef".into(),2=>r.expected_catalog_seq+=1,_=>r.expected_catalog_digest="87654321".into()};assert!(prepare(&current,&r,Some(&recovery())).is_err());}
        let mut r=request();r.preserve_save=false;assert!(r.validate().is_err());assert!(prepare(&current,&request(),None).is_err());
        let mut r=request();r.survivor_visible_order=vec!["keep_a".into()];assert!(prepare(&current,&r,Some(&recovery())).is_err());
    }
    #[test]fn only_correlated_known_committed_or_true_noop_receipts_confirm(){
        let prepared=prepare(&snapshot(&metadata(),"board-a","request-a").unwrap(),&request(),Some(&recovery())).unwrap();
        let good=json!({"schemaVersion":1,"engine":ENGINE,"requestId":"submit-a","boardDeviceId":"board-a","bootId":"0123456789abcdef",
            "ok":true,"commitState":"committed","catalogSeq":8,"catalogDigest":"87654321"});
        assert!(receipt(&good,"submit-a",&prepared).is_ok());
        for (key,value) in [("ok",json!(false)),("requestId",json!("foreign")),("boardDeviceId",json!("other")),("bootId",json!("1123456789abcdef")),
            ("commitState",json!("unknown")),("commitState",json!("not_committed")),("commitState",json!("unchanged")),("catalogSeq",json!(9))]{
            let mut bad=good.clone();bad[key]=value;assert!(receipt(&bad,"submit-a",&prepared).is_err());
        }
    }
    #[test]fn seventeen_originals_and_fourteen_added_ids_are_derived_without_component_policy(){
        let before=(0..17).map(|n|format!("keep_{n:02}")).collect::<Vec<_>>();let extras=(0..14).map(|n|format!("extra_{n:02}")).collect::<Vec<_>>();
        let after=extras.iter().chain(before.iter().rev()).map(String::as_str).collect::<Vec<_>>();
        let plan=restore_delta(&evidence(&before.iter().map(String::as_str).collect::<Vec<_>>()),&evidence(&after),&extras).unwrap();
        assert_eq!(plan.visible_order,before);assert_eq!(plan.remove,extras);
    }
    #[test]fn explicit_difference_and_original_order_come_only_from_complete_evidence(){
        let before=evidence(&["keep_a","keep_b","keep_c"]);let after=evidence(&["extra_x","keep_c","extra_y","keep_b","keep_a"]);
        let plan=restore_delta(&before,&after,&["extra_y".into(),"extra_x".into()]).unwrap();
        assert_eq!(plan.visible_order,vec!["keep_a","keep_b","keep_c"]);assert_eq!(plan.remove,vec!["extra_y","extra_x"]);
    }
    #[test]fn ready_additions_are_not_mistaken_for_original_members_or_filtered_out(){
        let before=evidence(&["keep_a"]);let mut after=evidence(&["keep_a","extra_ready","extra_bootstrap"]);
        after["items"][2]["resourceState"]="bootstrap".into();after["items"][2]["manifestState"]="resources-incomplete".into();
        assert!(restore_delta(&before,&after,&["extra_ready".into(),"extra_bootstrap".into()]).is_ok());
        assert!(restore_delta(&before,&after,&["extra_bootstrap".into()]).is_err());
    }
    #[test]fn omissions_duplicates_unknown_ids_or_original_member_removal_are_rejected(){
        let before=evidence(&["keep_a","keep_b"]);let after=evidence(&["extra_x","keep_b","keep_a"]);
        for remove in [vec![],vec!["extra_x".into(),"extra_x".into()],vec!["keep_b".into()],vec!["extra_x".into(),"unknown".into()]]{
            assert!(restore_delta(&before,&after,&remove).is_err());
        }
    }
    #[test]fn changed_original_resource_or_wrong_board_never_becomes_a_restore_plan(){
        let before=evidence(&["keep_a","keep_b"]);
        for (path,value) in [("/boardDeviceId",json!("other")),("/items/1/imageCount",json!(3)),("/items/1/resourceState",json!("bootstrap"))]{
            let mut after=evidence(&["keep_a","keep_b","extra_x"]);*after.pointer_mut(path).unwrap()=value;
            assert!(restore_delta(&before,&after,&["extra_x".into()]).is_err());
        }
    }
    #[test]fn incomplete_or_ambiguous_inventory_is_never_treated_as_absence(){
        let before=evidence(&["keep_a"]);
        for (key,value) in [("complete",json!(false)),("recoveryBlocked",json!(true)),("warnings",json!(["unknown"])),("ok",json!(false))]{
            let mut after=evidence(&["keep_a","extra_x"]);after[key]=value;assert!(restore_delta(&before,&after,&["extra_x".into()]).is_err());
        }
        assert!(restore_delta(&before,&evidence(&["keep_a","keep_a"]),&[]).is_err());
    }
}
