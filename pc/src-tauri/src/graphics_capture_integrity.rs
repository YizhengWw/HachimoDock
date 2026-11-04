//! Shared fail-closed collector for local screenshot tests and fault injection.
use base64::{engine::general_purpose::STANDARD, Engine};
use serde_json::{json, Value};
use std::path::Path;

fn safe_id(value:&str,max:usize)->bool{
    !value.is_empty()&&value.len()<=max&&value.bytes().all(|b|b.is_ascii_alphanumeric()||b"_-.".contains(&b))
}
fn safe_checksum(value:&Value)->Option<&str>{
    value.as_str().filter(|s|s.len()==8&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b)))
}
pub(super) fn device_error(p:&Value,phase:&str)->Value{
    let code=match p["error"].as_str().unwrap_or(""){
        "audio busy"=>"audio_busy","no memory"=>"no_memory","renderer busy"=>"renderer_busy",
        "no framebuffer"=>"no_framebuffer","transport or encode failed"=>"transport_or_encode_failed",
        "snapshot expired or mismatched"=>"snapshot_expired_or_mismatched","invalid snapshot read"=>"invalid_snapshot_read",
        "snapshot idle expired"=>"snapshot_idle_expired","snapshot lifetime expired"=>"snapshot_lifetime_expired",
        "snapshot lease revoked"=>"snapshot_lease_revoked","snapshot transport failed"=>"snapshot_transport_failed",
        "snapshot transport busy"=>"snapshot_transport_busy",
        "snapshot already closed"=>"snapshot_already_closed","snapshot route mismatched"=>"snapshot_route_mismatched",
        "snapshot id mismatched"=>"snapshot_id_mismatched",
        "invalid snapshot close"=>"invalid_snapshot_close",
        "snapshot close acknowledgement failed"=>"snapshot_close_ack_failed",
        "invalid snapshot request"=>"invalid_snapshot_request","unsupported snapshot mode"=>"unsupported_snapshot_mode",
        "snapshot busy"=>"snapshot_busy","snapshot resource busy"=>"snapshot_resource_busy",_=>"unrecognized_device_error"
    };
    let mut out=json!({"kind":"device_error","phase":phase,"code":code,"failedIndex":p["failedIndex"].as_u64(),"sentBytes":p["sentBytes"].as_u64()});
    if matches!(code,"transport_or_encode_failed"|"snapshot_transport_failed"|"snapshot_transport_busy"|"snapshot_close_ack_failed") {
        if let Some(writer)=screenshot_writer_metadata(p){out["writer"]=writer;}
    }
    if let Some(correlation)=screenshot_request_metadata(p){out["requestCorrelation"]=correlation;}
    out
}
fn screenshot_request_metadata(p:&Value)->Option<Value>{
    let bytes=p["requestBytes"].as_u64()?;let hash=p["requestHash"].as_u64()?;
    let received=p["receivedIdBytes"].as_u64()?;let received_hash=p["receivedIdHash"].as_u64()?;
    let expected=p["expectedIdBytes"].as_u64()?;let expected_hash=p["expectedIdHash"].as_u64()?;
    if bytes>4096||received>32||expected>31||[hash,received_hash,expected_hash].iter().any(|n|*n>u32::MAX as u64){return None;}
    Some(json!({"bytes":bytes,"hash":hash,"receivedIdBytes":received,"receivedIdHash":received_hash,
        "expectedIdBytes":expected,"expectedIdHash":expected_hash,"authentication":false}))
}
fn screenshot_writer_metadata(p:&Value)->Option<Value>{
    // All-or-nothing fixed numeric schema. No arbitrary device text, driver
    // error strings, image data or extension fields enter acceptance reports.
    let reason=p["writerReason"].as_u64()?;
    let name=match reason {0=>"unclassified",1=>"audio",2=>"send_budget",3=>"lease_revoked",
        4=>"driver_no_progress",5=>"driver_invalid_count",6=>"driver_drain",7=>"transport_mutex",
        8=>"encode",9=>"route",_=>return None};
    let part=p["writerPart"].as_u64()?;let requested=p["writerRequested"].as_u64()?;
    let accepted=p["writerAccepted"].as_u64()?;let calls=p["writerCalls"].as_u64()?;
    let returned=p["writerReturn"].as_i64()?;let drain=p["writerDrain"].as_i64()?;
    let ticks=p["writerElapsedTicks"].as_u64()?;
    if part>6||requested>16384||accepted>requested||calls>16384||ticks>u32::MAX as u64
        ||i32::try_from(returned).is_err()||i32::try_from(drain).is_err(){return None;}
    Some(json!({"reason":name,"code":reason,"part":part,"requestedBytes":requested,
        "acceptedBytes":accepted,"calls":calls,"driverReturn":returned,"drainReturn":drain,"elapsedTicks":ticks}))
}
/// Reports are metadata, not another copy of arbitrary wire text or image data.
fn frame_metadata(p:&Option<Value>)->Value{
    let Some(p)=p else{return Value::Null;};
    let mut out=serde_json::Map::new();
    for (key,max) in [("id",31),("requestId",64)]{if let Some(s)=p[key].as_str().filter(|s|safe_id(s,max)){out.insert(key.into(),json!(s));}}
    for key in ["width","height","totalBytes","chunkBytes","chunks"]{if let Some(n)=p[key].as_u64(){out.insert(key.into(),json!(n));}}
    for (key,value) in [("format","rgb565le"),("integrity","chunk-fnv1a32-v1"),("mode","pull-v1")]{if p[key]==value{out.insert(key.into(),json!(value));}}
    if let Some(checksum)=safe_checksum(&p["checksum"]){out.insert("checksum".into(),json!(checksum));}
    if let Some(boot)=p["bootId"].as_str().filter(|s|s.len()==16&&s.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))){out.insert("bootId".into(),json!(boot));}
    Value::Object(out)
}

pub(super) fn chunk_checksum(bytes: &[u8]) -> u32 {
    bytes.iter().fold(2166136261u32, |sum, b| {
        (sum ^ u32::from(*b)).wrapping_mul(16777619)
    })
}

#[cfg(test)]
#[test]
fn screenshot_request_correlation_is_numeric_atomic_bounded_and_not_authentication(){
    let mut p=json!({"error":"snapshot id mismatched","requestBytes":88,"requestHash":123,
        "receivedIdBytes":8,"receivedIdHash":456,"expectedIdBytes":8,"expectedIdHash":789,
        "requestBody":"SECRET/ignored","id":"SECRET/ignored"});
    let out=device_error(&p,"close");assert_eq!(out["requestCorrelation"]["authentication"],false);
    assert_eq!(out["requestCorrelation"]["hash"],123);assert!(!out.to_string().contains("SECRET"));
    for key in ["requestBytes","requestHash","receivedIdBytes","receivedIdHash","expectedIdBytes","expectedIdHash"] {
        let mut missing=p.clone();missing.as_object_mut().unwrap().remove(key);
        assert!(device_error(&missing,"close").get("requestCorrelation").is_none());
    }
    for (key,bad) in [("requestBytes",4097u64),("requestHash",u32::MAX as u64+1),
        ("receivedIdBytes",33),("expectedIdBytes",32),("expectedIdHash",u32::MAX as u64+1)] {
        let mut invalid=p.clone();invalid[key]=json!(bad);
        assert!(device_error(&invalid,"close").get("requestCorrelation").is_none());
    }
    p["error"]=json!("invalid snapshot close");assert_eq!(device_error(&p,"close")["code"],"invalid_snapshot_close");
}
pub(super) fn frame_checksum(bytes: &[u8]) -> u32 {
    bytes.chunks_exact(2).fold(2166136261u32, |sum, p| {
        (sum ^ u32::from(u16::from_le_bytes([p[0], p[1]]))).wrapping_mul(16777619)
    })
}
/// Validate before admitting a pull response to the forensic collector. Errors
/// contain bounded numeric metadata only, never screenshot bytes or device text.
pub(super) fn checked_chunk(
    p: &Value,
    id: &str,
    index: u64,
    total: usize,
    chunk_bytes: usize,
) -> Result<Vec<u8>, Value> {
    if p["id"].as_str() != Some(id) {
        return Err(json!({"kind":"different_frame"}));
    }
    if p["index"].as_u64() != Some(index) {
        return Err(json!({"kind":"sequence","expected":index,"received":p["index"].as_u64()}));
    }
    let expected = index
        .checked_mul(chunk_bytes as u64)
        .and_then(|offset| (total as u64).checked_sub(offset))
        .unwrap_or(0)
        .min(chunk_bytes as u64);
    if expected == 0 || chunk_bytes == 0 || chunk_bytes > 3072 {
        return Err(json!({"kind":"invalid_chunk_bounds","index":index}));
    }
    let Some(data) = p["data"].as_str() else {
        return Err(json!({"kind":"missing_data","index":index}));
    };
    if data.len() > 4096 {
        return Err(json!({"kind":"oversized_base64","index":index,"encodedBytes":data.len()}));
    }
    let bytes = STANDARD
        .decode(data)
        .map_err(|_| json!({"kind":"invalid_base64","index":index}))?;
    if bytes.len() as u64 != expected || p["rawSize"].as_u64() != Some(expected) {
        return Err(
            json!({"kind":"chunk_size","index":index,"expected":expected,
            "declared":p["rawSize"].as_u64(),"received":bytes.len()}),
        );
    }
    if p["checksum"] != format!("{:08x}", chunk_checksum(&bytes)) {
        return Err(json!({"kind":"chunk_checksum","index":index}));
    }
    Ok(bytes)
}
pub(super) struct Capture {
    width: usize,
    height: usize,
    begin: Option<Value>,
    pub(super) end: Option<Value>,
    pub(super) raw: Vec<u8>,
    errors: Vec<Value>,
    chunks: Vec<Value>,
    next: u64,
}
impl Capture {
    pub(super) fn new(width: usize, height: usize) -> Self {
        let capacity = width
            .checked_mul(height)
            .and_then(|n| n.checked_mul(2))
            .filter(|n| width>0&&width<=640&&height>0&&height<=480&&*n <= 640 * 480 * 2)
            .unwrap_or(0);
        Self {
            width,
            height,
            begin: None,
            end: None,
            raw: Vec::with_capacity(capacity),
            errors: Vec::new(),
            chunks: Vec::new(),
            next: 0,
        }
    }
    pub(super) fn error(&mut self, error: Value) {
        if self.errors.len() < 128 {
            self.errors.push(error);
        }
    }
    fn total(&self) -> usize {
        self.width
            .checked_mul(self.height)
            .and_then(|n| n.checked_mul(2))
            .unwrap_or(usize::MAX)
    }
    fn dimensions_valid(&self)->bool{self.width>0&&self.width<=640&&self.height>0&&self.height<=480}
    pub(super) fn observe(&mut self, topic: &str, p: Value) {
        match topic {
            "debug/screenshot_begin" => {
                if self.begin.is_some() {
                    self.error(json!({"kind":"duplicate_begin"}));
                    return;
                }
                if !self.dimensions_valid(){self.error(json!({"kind":"invalid_dimensions"}));}
                if p["format"] != "rgb565le"
                    || p["width"].as_u64() != Some(self.width as u64)
                    || p["height"].as_u64() != Some(self.height as u64)
                    || p["id"]
                        .as_str()
                        .is_none_or(|id| !safe_id(id,31))
                {
                    self.error(json!({"kind":"invalid_begin"}));
                }
                if p["integrity"] != "chunk-fnv1a32-v1"
                    || p["totalBytes"].as_u64() != Some(self.total() as u64)
                    || p["chunkBytes"].as_u64().is_none_or(|n| n == 0 || n > 3072)
                {
                    self.error(json!({"kind":"missing_or_invalid_integrity_metadata"}));
                }
                self.begin = Some(p);
            }
            "debug/screenshot_chunk" => {
                if !self.dimensions_valid(){self.error(json!({"kind":"invalid_dimensions"}));return;}
                let Some(begin) = self.begin.as_ref() else {
                    self.error(json!({"kind":"chunk_before_begin"}));
                    return;
                };
                if p["id"] != begin["id"] {
                    self.error(json!({"kind":"different_frame"}));
                    return;
                }
                let chunk_bytes = begin["chunkBytes"]
                    .as_u64()
                    .filter(|n| *n > 0 && *n <= 3072)
                    .unwrap_or(3072);
                if self.end.is_some() {
                    self.error(json!({"kind":"chunk_after_end"}));
                    return;
                }
                let Some(index) = p["index"].as_u64() else {
                    self.error(json!({"kind":"invalid_index"}));
                    return;
                };
                if index != self.next {
                    self.error(json!({"kind":"sequence","expected":self.next,"received":index}));
                }
                self.next = index.saturating_add(1);
                let Some(data) = p["data"].as_str() else {
                    self.error(json!({"kind":"missing_data","index":index}));
                    return;
                };
                if data.len()>4096{self.error(json!({"kind":"oversized_base64","index":index,"encodedBytes":data.len()}));return;}
                let bytes = match STANDARD.decode(data) {
                    Ok(bytes) => bytes,
                    Err(_) => {
                        self.error(json!({"kind":"invalid_base64","index":index}));
                        return;
                    }
                };
                let expected = index
                    .checked_mul(chunk_bytes)
                    .and_then(|offset| (self.total() as u64).checked_sub(offset))
                    .unwrap_or(0)
                    .min(chunk_bytes);
                let raw_size_matches = p["rawSize"].as_u64() == Some(bytes.len() as u64)
                    && bytes.len() as u64 == expected
                    && expected > 0;
                let checksum = format!("{:08x}", chunk_checksum(&bytes));
                let checksum_matches = p["checksum"] == checksum;
                if self.chunks.len() < 256 {
                    self.chunks.push(json!({"index":index,"expectedBytes":expected,"receivedBytes":bytes.len(),"rawSize":p["rawSize"].as_u64(),"checksum":safe_checksum(&p["checksum"]),"computedChecksum":checksum,"rawSizeMatches":raw_size_matches,"checksumMatches":checksum_matches}));
                }
                if !raw_size_matches {
                    self.error(json!({"kind":"chunk_size","index":index,"expected":expected,"declared":p["rawSize"].as_u64(),"received":bytes.len()}));
                }
                if !checksum_matches {
                    self.error(json!({"kind":"chunk_checksum","index":index}));
                }
                if self.raw.len() + bytes.len() > self.total() {
                    self.error(json!({"kind":"size","index":index}));
                    return;
                }
                self.raw.extend_from_slice(&bytes);
            }
            "debug/screenshot_end" => {
                if self.end.is_some() {
                    self.error(json!({"kind":"duplicate_end"}));
                    return;
                }
                self.end = Some(p);
            }
            "debug/screenshot_error" => self.error(device_error(&p,"observe")),
            _ => {}
        }
    }
    pub(super) fn report(&self) -> Value {
        let checksum = format!("{:08x}", frame_checksum(&self.raw));
        let metadata_matches = self
            .begin
            .as_ref()
            .zip(self.end.as_ref())
            .is_some_and(|(b, e)| {
                b["id"] == e["id"]
                    && e["chunks"].as_u64() == Some(self.next)
                    && e["totalBytes"].as_u64() == Some(self.total() as u64)
            });
        let checksum_matches = self.end.as_ref().is_some_and(|e| e["checksum"] == checksum);
        let valid = self.dimensions_valid()&&self.raw.len() == self.total()
            && self.errors.is_empty()
            && metadata_matches
            && checksum_matches;
        json!({"passed":valid,"receivedBytes":self.raw.len(),"expectedBytes":self.total(),"nextIndex":self.next,"errors":self.errors,"chunks":self.chunks,"begin":frame_metadata(&self.begin),"end":frame_metadata(&self.end),"endReceived":self.end.is_some(),"metadataMatches":metadata_matches,"checksumMatches":checksum_matches,"computedChecksum":checksum,"causeOfEarlier240ByteShortfall":"unknown"})
    }
    pub(super) fn save_report(&self, path: &Path, extra: Value) -> Value {
        let mut report = self.report();
        if let Some(extra) = extra.as_object() {
            let collisions=extra.keys().filter(|key|report.get(*key).is_some()).count();
            if collisions>0{
                report["passed"]=json!(false);
                let errors=report["errors"].as_array_mut().unwrap();
                if errors.len()<128{errors.push(json!({"kind":"extra_metadata_collision","count":collisions}));}
            }
            for (key, value) in extra {
                if report.get(key).is_none(){report[key] = value.clone();}
            }
        }
        std::fs::write(path, serde_json::to_vec_pretty(&report).unwrap()).unwrap();
        report
    }
}

#[test]
fn screenshot_integrity_invalid_dimensions_never_certify_an_empty_frame(){
    for (width,height) in [(0,0),(0,1),(1,0),(641,1),(1,481),(usize::MAX,2)]{
        let mut c=Capture::new(width,height);assert_eq!(c.raw.capacity(),0);
        c.observe("debug/screenshot_begin",json!({"id":"frame","width":width,"height":height,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":c.total(),"chunkBytes":8}));
        c.observe("debug/screenshot_end",json!({"id":"frame","chunks":0,"totalBytes":c.total(),"checksum":format!("{:08x}",frame_checksum(&[]))}));
        assert_eq!(c.report()["passed"],false,"{width}x{height}");
        c.observe("debug/screenshot_chunk",json!({"id":"frame","index":0,"data":"AAAA"}));assert!(c.raw.is_empty());
    }
}

#[test]
fn screenshot_integrity_rejects_encoded_overflow_before_decoding_or_retaining(){
    let mut c=Capture::new(2,1);
    c.observe("debug/screenshot_begin",json!({"id":"frame","width":2,"height":1,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":4,"chunkBytes":4}));
    c.observe("debug/screenshot_chunk",json!({"id":"frame","index":0,"rawSize":4,"checksum":"00000000","data":"A".repeat(4097)}));
    assert!(c.raw.is_empty());assert_eq!(c.report()["errors"][0]["kind"],"oversized_base64");assert_eq!(c.report()["passed"],false);
}

#[test]
fn screenshot_reports_reject_verdict_overrides_and_strip_untrusted_wire_text(){
    let mut c=Capture::new(2,1);let raw=[1u8,2,3,4];
    c.observe("debug/screenshot_begin",json!({"id":"frame","width":2,"height":1,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":4,"chunkBytes":4,"unknown":"SECRET/private/path"}));
    c.observe("debug/screenshot_chunk",json!({"id":"frame","index":0,"rawSize":4,"checksum":format!("{:08x}",chunk_checksum(&raw)),"data":STANDARD.encode(raw)}));
    c.observe("debug/screenshot_end",json!({"id":"frame","chunks":1,"totalBytes":4,"checksum":format!("{:08x}",frame_checksum(&raw)),"unknown":"SECRET/token"}));
    assert_eq!(c.report()["passed"],true);assert!(!c.report().to_string().contains("SECRET"));
    let dir=tempfile::tempdir().unwrap();let report=c.save_report(&dir.path().join("capture.json"),json!({"passed":true,"expectedBytes":0,"errors":[],"hardware":false}));
    assert_eq!(report["passed"],false);assert_eq!(report["expectedBytes"],4);assert_eq!(report["errors"][0]["kind"],"extra_metadata_collision");assert_eq!(report["hardware"],false);
    c.observe("debug/screenshot_error",json!({"error":"SECRET/access-token","failedIndex":{"SECRET":"payload"},"sentBytes":"SECRET/count","unknown":"SECRET/data"}));
    let report=c.report();assert_eq!(report["passed"],false);assert_eq!(report["errors"][0]["code"],"unrecognized_device_error");assert!(!report.to_string().contains("SECRET"));
    c.observe("debug/screenshot_chunk",json!({"id":"frame","index":1,"rawSize":"SECRET/rawSize","checksum":{"SECRET":"hash"},"data":"AA=="}));
    assert!(!c.report().to_string().contains("SECRET"));
}

#[test]
fn screenshot_writer_diagnostics_are_numeric_bounded_and_never_relax_a_failure(){
    let p=json!({"error":"snapshot transport failed","writerReason":2,"writerPart":2,
        "writerRequested":4300,"writerAccepted":4100,"writerCalls":65,"writerReturn":64,
        "writerDrain":0,"writerElapsedTicks":101,"unknown":"SECRET/private-payload"});
    let out=device_error(&p,"read");assert_eq!(out["writer"]["reason"],"send_budget");
    assert_eq!(out["writer"]["elapsedTicks"],101);assert_eq!(out["kind"],"device_error");
    assert!(!out.to_string().contains("SECRET"));
    for (key,value) in [("writerReason",json!(10)),("writerPart",json!(7)),
        ("writerRequested",json!(16385)),("writerAccepted",json!(4301)),("writerCalls",json!(16385)),
        ("writerReturn",json!(2147483648i64)),("writerDrain",json!(-2147483649i64)),
        ("writerElapsedTicks",json!(4294967296u64)),("writerReason",json!("2")),
        ("writerPart",json!(true)),("writerDrain",json!({"SECRET":"text"}))]{
        let mut bad=p.clone();bad[key]=value;let out=device_error(&bad,"read");
        assert!(out.get("writer").is_none());assert!(!out.to_string().contains("SECRET"));
    }
    let mut arbitrary=p.clone();arbitrary["error"]=json!("SECRET/unrecognized-error");
    assert!(device_error(&arbitrary,"read").get("writer").is_none());
    let mut c=Capture::new(2,1);c.observe("debug/screenshot_error",p);
    assert_eq!(c.report()["passed"],false);assert_eq!(c.report()["errors"][0]["writer"]["code"],2);
}

#[test]
fn screenshot_integrity_faults_are_persisted_without_panics() {
    let dir = tempfile::tempdir().unwrap();
    let begin = json!({"id":"frame","width":4,"height":2,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":16,"chunkBytes":8});
    let raw = (0..16u8).collect::<Vec<_>>();
    let chunk = |index: usize| {
        let bytes = &raw[index * 8..index * 8 + 8];
        json!({"id":"frame","index":index,"rawSize":8,"checksum":format!("{:08x}",chunk_checksum(bytes)),"data":STANDARD.encode(bytes)})
    };
    let end = json!({"id":"frame","chunks":2,"totalBytes":16,"checksum":format!("{:08x}",frame_checksum(&raw))});
    for fault in [
        "valid",
        "truncated",
        "corrupted",
        "duplicate",
        "out_of_order",
        "invalid_base64",
        "missing_metadata",
        "device_error",
        "end_then_error",
        "no_end",
    ] {
        let mut c = Capture::new(4, 2);
        let mut b = begin.clone();
        if fault == "missing_metadata" {
            b.as_object_mut().unwrap().remove("integrity");
        }
        c.observe("debug/screenshot_begin", b);
        let mut first = chunk(0);
        if fault == "truncated" {
            first["data"] = STANDARD.encode(&raw[..6]).into();
        }
        if fault == "corrupted" {
            first["data"] = STANDARD.encode([255u8; 8]).into();
        }
        if fault == "invalid_base64" {
            first["data"] = "invalid!!".into();
        }
        if fault == "out_of_order" {
            c.observe("debug/screenshot_chunk", chunk(1));
        }
        c.observe("debug/screenshot_chunk", first.clone());
        if fault == "duplicate" {
            c.observe("debug/screenshot_chunk", first);
        }
        c.observe("debug/screenshot_chunk", chunk(1));
        if fault == "device_error" {
            c.observe(
                "debug/screenshot_error",
                json!({"id":"frame","failedIndex":2,"error":"short write"}),
            );
        }
        if fault != "no_end" {
            c.observe("debug/screenshot_end", end.clone());
        }
        if fault == "end_then_error" {
            c.observe("debug/screenshot_error", json!({"error":"drain failed"}));
        }
        let path = dir.path().join(format!("{fault}.json"));
        let report = c.save_report(&path, json!({"hardware":false}));
        assert_eq!(report["passed"], fault == "valid", "{fault}: {report}");
        assert_eq!(
            serde_json::from_slice::<Value>(&std::fs::read(path).unwrap()).unwrap(),
            report
        );
    }
}

#[test]
fn snapshot_terminal_categories_remain_bounded_and_fail_closed() {
    for (text, code) in [
        ("snapshot idle expired", "snapshot_idle_expired"),
        ("snapshot lifetime expired", "snapshot_lifetime_expired"),
        ("snapshot lease revoked", "snapshot_lease_revoked"),
        ("snapshot transport failed", "snapshot_transport_failed"),
        ("snapshot already closed", "snapshot_already_closed"),
        ("snapshot route mismatched", "snapshot_route_mismatched"),
        ("snapshot id mismatched", "snapshot_id_mismatched"),
    ] {
        let error = device_error(&json!({"error":text,"failedIndex":-1,"sentBytes":0,"extra":"SECRET/path"}),"read");
        assert_eq!(error["code"], code);
        assert!(!error.to_string().contains("SECRET"));
        assert_eq!(error["failedIndex"], Value::Null);
        let mut capture=Capture::new(2,1);capture.error(error);
        assert_eq!(capture.report()["passed"],false);
    }
}

#[test]
fn screenshot_integrity_identifies_240_byte_shortfall_with_all_200_indices() {
    let raw = vec![0u8; 640 * 480 * 2];
    let mut c = Capture::new(640, 480);
    c.observe("debug/screenshot_begin",json!({"id":"frame","width":640,"height":480,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":raw.len(),"chunkBytes":3072}));
    for index in 0..200 {
        let expected = &raw[index * 3072..(index + 1) * 3072];
        let bytes = if index == 87 {
            &expected[..2832]
        } else {
            expected
        };
        c.observe("debug/screenshot_chunk",json!({"id":"frame","index":index,"rawSize":3072,"checksum":format!("{:08x}",chunk_checksum(expected)),"data":STANDARD.encode(bytes)}));
    }
    c.observe("debug/screenshot_end",json!({"id":"frame","chunks":200,"totalBytes":raw.len(),"checksum":format!("{:08x}",frame_checksum(&raw))}));
    let report = c.report();
    assert_eq!(report["passed"], false);
    assert_eq!(report["receivedBytes"], 614160);
    assert_eq!(report["nextIndex"], 200);
    assert_eq!(report["chunks"][87]["receivedBytes"], 2832);
    assert_eq!(report["chunks"][87]["checksumMatches"], false);
    assert_eq!(report["causeOfEarlier240ByteShortfall"], "unknown");
}

#[test]
fn screenshot_integrity_rejects_96_byte_shortfall_from_128_missing_base64_characters() {
    for patterned in [false, true] {
        let raw = (0..640 * 480 * 2)
            .map(|i| {
                if patterned {
                    ((i * 73 + i / 7) % 256) as u8
                } else {
                    0
                }
            })
            .collect::<Vec<_>>();
        let mut c = Capture::new(640, 480);
        c.observe("debug/screenshot_begin", json!({"id":"offline-frame","width":640,"height":480,"format":"rgb565le","integrity":"chunk-fnv1a32-v1","totalBytes":raw.len(),"chunkBytes":3072}));
        for index in 0..200 {
            let expected = &raw[index * 3072..(index + 1) * 3072];
            let mut data = STANDARD.encode(expected);
            assert_eq!(data.len(), 4096);
            if index == 23 {
                // Keep valid JSON/base64 and unchanged declared size/hash, as
                // when an interior ASCII span is absent before collection.
                data.replace_range(1024..1152, "");
                assert_eq!(data.len(), 3968);
                let decoded = STANDARD.decode(&data).unwrap();
                assert_eq!(decoded.len(), 2976);
                assert_eq!(decoded, [&expected[..768], &expected[864..]].concat());
                if !patterned {
                    // Consistent with the recorded pair for an all-zero
                    // fixture, not unique proof of contents or loss location:
                    // FNV is non-cryptographic and can have collisions.
                    assert_eq!(format!("{:08x}", chunk_checksum(expected)), "99fb0dc5");
                    assert_eq!(format!("{:08x}", chunk_checksum(&decoded)), "0f87ba45");
                }
            }
            c.observe("debug/screenshot_chunk", json!({"id":"offline-frame","index":index,"rawSize":3072,"checksum":format!("{:08x}",chunk_checksum(expected)),"data":data}));
        }
        c.observe("debug/screenshot_end", json!({"id":"offline-frame","chunks":200,"totalBytes":raw.len(),"checksum":format!("{:08x}",frame_checksum(&raw))}));
        let mut expected_received = raw.clone();
        expected_received.drain(23 * 3072 + 768..23 * 3072 + 864);
        assert_eq!(c.raw, expected_received);
        let report = c.report();
        assert_eq!(report["passed"], false);
        assert_eq!(report["receivedBytes"], 614304);
        assert_eq!(report["expectedBytes"], 614400);
        assert_eq!(report["nextIndex"], 200);
        assert_eq!(report["endReceived"], true);
        assert_eq!(report["metadataMatches"], true);
        assert_eq!(report["checksumMatches"], false);
        assert_eq!(report["chunks"].as_array().unwrap().len(), 200);
        assert_eq!(report["chunks"][23]["receivedBytes"], 2976);
        assert_eq!(report["chunks"][23]["rawSizeMatches"], false);
        assert_eq!(report["chunks"][23]["checksumMatches"], false);
        assert!(report["chunks"]
            .as_array()
            .unwrap()
            .iter()
            .enumerate()
            .all(|(index, chunk)| index == 23
                || chunk["rawSizeMatches"] == true && chunk["checksumMatches"] == true));
        assert_eq!(
            report["errors"],
            json!([
                {"kind":"chunk_size","index":23,"expected":3072,"declared":3072,"received":2976},
                {"kind":"chunk_checksum","index":23}
            ])
        );
    }
}
