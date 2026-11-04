//! Regressions through production ZIP collection and directory import, not just
//! extension helpers. All content is synthetic public contract data.
use super::*;
use serde_json::{json, Value};
use std::{collections::HashMap, io::Write};
fn suite() -> Value { serde_json::from_str(include_str!("../../skills/petui/contracts/phase2-pc-contracts-v1.json")).unwrap() }
fn runtime(s: &Value, c: &Value) -> Value {
    let mut r=s["bases"][c["base"].as_str().unwrap()].clone();
    for p in c["patches"].as_array().unwrap() {
        let path=p["path"].as_array().unwrap();let mut at=&mut r;
        for k in &path[..path.len()-1] { at=if let Some(k)=k.as_str(){&mut at[k]}else{&mut at[k.as_u64().unwrap() as usize]}; }
        let key=path.last().unwrap();
        if let Some(key)=key.as_str(){at[key]=p["value"].clone();}else{at[key.as_u64().unwrap() as usize]=p["value"].clone();}
    }
    r
}
fn files(s: &Value, base: &str, r: &Value) -> HashMap<String,Vec<u8>> {
    let mut files:HashMap<String,Vec<u8>>=s["packages"][base].as_object().unwrap().iter().map(|(n,v)|(n.clone(),serde_json::to_vec(v).unwrap())).collect();
    files.insert("runtime/widget.json".into(),serde_json::to_vec(r).unwrap());
    files.insert("assets/.keep".into(),Vec::new());files
}
fn zip_bytes(files: &HashMap<String,Vec<u8>>) -> Vec<u8> {
    let mut zip=zip::ZipWriter::new(std::io::Cursor::new(Vec::new()));
    for (name,bytes) in files { zip.start_file(name,zip::write::FileOptions::default()).unwrap();zip.write_all(bytes).unwrap(); }
    zip.finish().unwrap().into_inner()
}
fn write_dir(path: &std::path::Path, files: &HashMap<String,Vec<u8>>) {
    for (name,bytes) in files {let p=path.join(name);std::fs::create_dir_all(p.parent().unwrap()).unwrap();std::fs::write(p,bytes).unwrap();}
}
#[test]
fn shared_catalog_controls_match_complete_zip_and_directory_admission() {
    let suite: Value = serde_json::from_str(include_str!("../../skills/petui/contracts/pc-catalog-controls-v1.json")).unwrap();
    let root=std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/city-radio");
    let mut files:HashMap<String,Vec<u8>>=suite["packageFiles"].as_array().unwrap().iter().map(|name| {
        let name=name.as_str().unwrap();(name.to_owned(),std::fs::read(root.join(name)).unwrap())
    }).collect();
    let dir=tempfile::tempdir().unwrap();write_dir(dir.path(),&files);
    for case in suite["cases"].as_array().unwrap() {
        let name=case["name"].as_str().unwrap();let expected=case["ok"]==true;
        let mut row=suite["row"].clone();row["name"]=case["value"].clone();
        if let Some(extra)=case["extraFields"].as_object() {
            for (key,value) in extra {row[key.as_str()]=value.clone();}
        }
        let bytes=serde_json::to_vec(&json!([row])).unwrap();
        let catalog=crate::component_extension::Extension::validate_catalog_bytes(&bytes);
        assert_eq!(catalog.is_ok(),expected,"catalog {name}: {catalog:?}");
        files.insert("pc/catalog.json".into(),bytes.clone());
        std::fs::write(dir.path().join("pc/catalog.json"),bytes).unwrap();
        let zipped=validate_clawpkg_bytes(&zip_bytes(&files)).unwrap();
        let folder=validate_clawpkg_at_path(dir.path()).unwrap();
        for (path,report) in [("ZIP",zipped),("directory",folder)] {
            assert_eq!(report.ok,expected,"{path} {name}: {:?}",report.errors);
            if !expected {assert!(report.errors.iter().any(|e|e.contains("包内电台目录的文本字段无效")),"{path} {name}: {:?}",report.errors);}
        }
    }
}
#[test]
fn complete_zip_and_directory_contracts() {
    let s=suite();
    for c in s["cases"].as_array().unwrap() {
        let r=runtime(&s,c);let f=files(&s,c["base"].as_str().unwrap(),&r);let name=c["name"].as_str().unwrap();let expected=c["ok"]==true;
        if name.starts_with("app-form-")&&!expected&&!name.contains("-derived-") {
            assert!(crate::component_extension::app_form::validate_runtime(&r).is_err(),"protected form writer {name}");
        }
        if name.contains("-derived-")||name.starts_with("derived-") {
            assert!(crate::app_longtext::validate_derived_writers(&r).is_err(),"derived writer {name}");
        }
        let bytes=zip_bytes(&f);let zipped=validate_clawpkg_bytes(&bytes).unwrap();assert_eq!(zipped.ok,expected,"ZIP {name}: {:?}",zipped.errors);
        let dir=tempfile::tempdir().unwrap();write_dir(dir.path(),&f);
        let folder=validate_clawpkg_at_path(dir.path()).unwrap();assert_eq!(folder.ok,expected,"directory {name}: {:?}",folder.errors);
        let mut ze=zipped.errors;let mut de=folder.errors;ze.sort();de.sort();assert_eq!(ze,de,"collector error parity {name}");
    }
}
#[test]
fn accepted_complete_import_still_requires_exact_native_capabilities() {
    let s=suite();
    for (base,key,engine) in [("data-rows-v1","widgetDataRows","p4-data-rows-v1"),("texts-v1","widgetTexts","p4-texts-v1"),("longtext","widgetLongText","p4-longtext-v1")] {
        let r=&s["bases"][base];let f=files(&s,base,r);let bytes=zip_bytes(&f);let tmp=tempfile::tempdir().unwrap();let archive=tmp.path().join("contract.clawpkg");std::fs::write(&archive,&bytes).unwrap();
        let imported=validate_clawpkg_at_path(&archive).unwrap();assert!(imported.ok,"{base}: {:?}",imported.errors);
        let check=|caps:&Value|if base=="data-rows-v1"{crate::app_data_rows::require_capability(r,caps)}else{crate::app_longtext::require_capability(r,caps)};
        let mut caps=json!({});caps[key]=json!(engine);check(&caps).unwrap();
        assert!(check(&json!({})).is_err());
        for value in [json!("unknown"),json!([engine]),json!(true),Value::Null] {let mut caps=json!({});caps[key]=value;assert!(check(&caps).is_err(),"{base}: {caps}");}
    }
}
#[test]
fn derived_conflicts_reject_the_intended_writer_in_complete_packages() {
    let s=suite();
    for name in ["derived-program-output","derived-host","derived-explicit-host","derived-duplicate"] {
        let c=s["cases"].as_array().unwrap().iter().find(|c|c["name"]==name).unwrap();let r=runtime(&s,c);
        assert!(crate::app_longtext::validate_derived_writers(&r).is_err(),"{name}");
        let report=validate_clawpkg_bytes(&zip_bytes(&files(&s,c["base"].as_str().unwrap(),&r))).unwrap();
        assert!(!report.ok&&report.errors.iter().any(|e|e.contains("derived writer")),"{name}: {:?}",report.errors);
    }
}

#[test]
fn guest_count_writers_reject_targets_but_allow_reads_in_complete_packages() {
    let s:Value=serde_json::from_str(include_str!("../../skills/petui/contracts/count-var-writers-v1.json")).unwrap();
    for c in s["cases"].as_array().unwrap() {
        let r=runtime(&s,c);let expected=c["ok"]==true;let name=c["name"].as_str().unwrap();
        let gate=crate::app_longtext::validate_derived_writers(&r);
        assert_eq!(gate.is_ok(),expected,"derived gate {name}: {gate:?}");
        if !expected {assert!(gate.unwrap_err().contains("guest set/inc"),"wrong rejection {name}");}
        let f=files(&s,c["base"].as_str().unwrap(),&r);let bytes=zip_bytes(&f);
        let zipped=validate_clawpkg_bytes(&bytes).unwrap();assert_eq!(zipped.ok,expected,"ZIP {name}: {:?}",zipped.errors);
        if !expected {assert!(zipped.errors.iter().any(|e|e.contains("guest set/inc")),"ZIP cause {name}: {:?}",zipped.errors);}
        let dir=tempfile::tempdir().unwrap();write_dir(dir.path(),&f);
        let folder=validate_clawpkg_at_path(dir.path()).unwrap();assert_eq!(folder.ok,expected,"directory {name}: {:?}",folder.errors);
        let archive=tempfile::tempdir().unwrap();let path=archive.path().join("count-contract.clawpkg");std::fs::write(&path,&bytes).unwrap();
        let imported=validate_clawpkg_at_path(&path).unwrap();assert_eq!(imported.ok,expected,"ZIP path {name}: {:?}",imported.errors);
        let mut ze=zipped.errors;let mut de=folder.errors;let mut ie=imported.errors;ze.sort();de.sort();ie.sort();assert_eq!(ze,de,"directory parity {name}");assert_eq!(ze,ie,"ZIP path parity {name}");
    }
}
