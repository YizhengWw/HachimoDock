//! Metadata-only description of an already rejected AEC ingress packet.
//! This module NEVER admits audio, changes VAD/DSP, retries capture, or logs IDs.
use serde_json::{json, Map, Value};

const FAULTS: &[&str] = &["init_failed", "stream_reset_failed", "reference_overflow",
    "processed_queue_overflow", "dsp_too_slow"];
const BOOL_FIELDS: &[&str] = &["missing", "null", "boolean_true", "boolean_false",
    "string", "number", "array", "object"];
const BINDINGS: &[&str] = &["missing", "null", "current", "stale", "empty", "wrong_type"];
const COUNTERS: &[&str] = &["dspFrames", "dspLastUs", "dspMaxUs", "dspAecUs", "dspNsUs",
    "dspVadUs", "aecReferenceSamples", "internalFreeBytes", "internalLargestBytes",
    "captureTxLastUs", "captureTxMaxUs", "captureStackMinBytes"];

pub(crate) struct Context<'a> {
    pub board_id: &'a str,
    pub timeline_id: &'a str,
    pub duplex: bool,
    pub armed: bool,
    pub ready: bool,
}

fn field(value: Option<&Value>) -> &'static str {
    match value {
        None => "missing", Some(Value::Null) => "null",
        Some(Value::Bool(true)) => "boolean_true", Some(Value::Bool(false)) => "boolean_false",
        Some(Value::String(_)) => "string", Some(Value::Number(_)) => "number",
        Some(Value::Array(_)) => "array", Some(Value::Object(_)) => "object",
    }
}

fn binding(value: Option<&Value>, expected: &str) -> &'static str {
    match value {
        None => "missing", Some(Value::Null) => "null",
        Some(Value::String(v)) if v.is_empty() => "empty",
        Some(Value::String(v)) if v == expected => "current",
        Some(Value::String(_)) => "stale", _ => "wrong_type",
    }
}

/// Return only a known device reason, never arbitrary device-supplied text.
pub(crate) fn reported_reason(payload: &Value) -> &'static str {
    match payload.get("aecReason").and_then(Value::as_str) {
        Some("none") => "none", Some("init_failed") => "init_failed",
        Some("stream_reset_failed") => "stream_reset_failed",
        Some("reference_overflow") => "reference_overflow",
        Some("processed_queue_overflow") => "processed_queue_overflow",
        Some("dsp_too_slow") => "dsp_too_slow", _ => "unprocessed_audio",
    }
}

pub(crate) struct Failure {
    pub detail: &'static str,
    pub metadata: Value,
}

/// An exact reason is a DEVICE REPORT, not proof of a physical DSP root cause.
/// Context flags are sampled at receipt time, not an atomic firmware snapshot.
pub(crate) fn failure(topic: &str, payload: &Value, context: Context<'_>) -> Failure {
    let reported = reported_reason(payload);
    let reason = if reported == "none" { "unprocessed_audio" } else { reported };
    let reported_fault = FAULTS.contains(&reason);
    let (source, trigger, invalid, unprocessed) = match topic {
        "audio/status" => ("audio/status", "status_aec_false", payload["audioAec"] != false, false),
        "audio/chunk" => match payload.get("aec") {
            None => ("audio/chunk", "chunk_aec_missing", true, false),
            Some(Value::Bool(false)) => ("audio/chunk", "chunk_aec_false", false, true),
            Some(Value::Bool(true)) => match payload.get("vadSpeech") {
                None => ("audio/chunk", "chunk_vad_missing", true, false),
                Some(Value::Bool(_)) => ("audio/chunk", "unexpected_aec_rejection", true, false),
                _ => ("audio/chunk", "chunk_vad_not_boolean", true, false),
            },
            _ => ("audio/chunk", "chunk_aec_not_boolean", true, false),
        },
        _ => ("unsupported", "unexpected_aec_rejection", true, false),
    };
    let (class, kind) = if invalid {
        ("invalid_frame_metadata", "aec_metadata_invalid")
    } else if reported_fault {
        ("device_reported_dsp_failure", "aec_device_reported_failure")
    } else if unprocessed {
        ("unprocessed_frame", "aec_unprocessed_frame")
    } else {
        ("processing_unavailable", "aec_processing_unavailable")
    };
    let detail = if invalid { "设备返回的语音数据不完整或格式异常" }
    else { match reason {
        "init_failed" | "stream_reset_failed" => "设备报告音频处理器初始化失败",
        "reference_overflow" => "设备报告回声参考缓冲溢出",
        "processed_queue_overflow" => "设备报告收音处理缓冲溢出",
        "dsp_too_slow" => "设备报告音频处理持续超时",
        _ if unprocessed => "收到的语音暂时无法处理，具体原因未确定",
        _ => "设备报告回声消除尚不可用，具体原因未确定",
    }};
    let mut metadata = json!({"aecMetadataVersion":1,"ok":false,"reason":reason,"errorKind":kind,
        "aecTopic":source,"aecFailureClass":class,"aecTrigger":trigger,
        "aecField":field(payload.get("aec")),"vadSpeechField":field(payload.get("vadSpeech")),
        "audioAecField":field(payload.get("audioAec")),"audioAecPreparingField":field(payload.get("audioAecPreparing")),
        "conversationField":field(payload.get("conversation")),
        "boardBinding":binding(payload.get("boardDeviceId"),context.board_id),
        "timelineBinding":binding(payload.get("conversationSessionId"),context.timeline_id),
        "captureArmed":context.armed,"captureReady":context.ready,"duplex":context.duplex,
        "aecReasonField":match payload.get("aecReason") {
            None=>"missing",Some(Value::Null)=>"null",Some(Value::String(v)) if v=="none"=>"none",
            Some(Value::String(_)) if reported_fault=>"known_failure",Some(Value::String(_))=>"unknown_string",_=>"wrong_type",
        }});
    for (key, source, bound) in [("frameSeq", "seq", u32::MAX as u64), ("declaredBytes", "bytes", 65536)] {
        if let Some(value) = payload.get(source).and_then(Value::as_u64).filter(|v|*v<=bound) {
            metadata[key] = json!(value);
        }
    }
    for &key in COUNTERS {
        if let Some(value) = payload.get(key).and_then(Value::as_u64).filter(|v|*v<=u32::MAX as u64) {
            metadata[key] = json!(value);
        }
    }
    Failure { detail, metadata }
}

/// AEC receipts have a stricter schema than the general diagnostic stream.
/// No unknown strings, IDs, hashes of user text, payload lengths derived from
/// PCM, raw payload, transcript, provider response or credential can pass.
pub(super) fn sanitize(details: &Value) -> Value {
    let mut out = Map::new();
    let enums: &[(&str, &[&str])] = &[
        ("aecTopic", &["audio/status","audio/chunk"]),
        ("aecFailureClass", &["invalid_frame_metadata","device_reported_dsp_failure","unprocessed_frame","processing_unavailable"]),
        ("aecTrigger", &["status_aec_false","chunk_aec_missing","chunk_aec_not_boolean","chunk_aec_false","chunk_vad_missing","chunk_vad_not_boolean","unexpected_aec_rejection"]),
        ("errorKind", &["aec_metadata_invalid","aec_device_reported_failure","aec_unprocessed_frame","aec_processing_unavailable"]),
        ("reason", &["none","unprocessed_audio","init_failed","stream_reset_failed","reference_overflow","processed_queue_overflow","dsp_too_slow"]),
        ("aecField", BOOL_FIELDS),("vadSpeechField", BOOL_FIELDS),("audioAecField", BOOL_FIELDS),
        ("audioAecPreparingField", BOOL_FIELDS),("conversationField", BOOL_FIELDS),
        ("timelineBinding", BINDINGS),("boardBinding", BINDINGS),
        ("aecReasonField", &["missing","null","none","known_failure","unknown_string","wrong_type"]),
    ];
    if !details.get("aecTopic").and_then(Value::as_str).is_some_and(|s| ["audio/status","audio/chunk"].contains(&s)) {
        return Value::Object(out);
    }
    for (key, allowed) in enums {
        if let Some(value) = details.get(*key).and_then(Value::as_str).filter(|v|allowed.contains(v)) {
            out.insert((*key).into(), json!(value));
        }
    }
    for key in ["ok","duplex","captureArmed","captureReady"] {
        if let Some(value) = details.get(key).and_then(Value::as_bool) { out.insert(key.into(), json!(value)); }
    }
    for (key,bound) in [("aecMetadataVersion",1), ("frameSeq",u32::MAX as u64),("declaredBytes",65536)] {
        if let Some(value) = details.get(key).and_then(Value::as_u64).filter(|v|*v<=bound) {
            if key!="aecMetadataVersion" || value==1 {out.insert(key.into(),json!(value));}
        }
    }
    for &key in COUNTERS {
        if let Some(value) = details.get(key).and_then(Value::as_u64).filter(|v|*v<=u32::MAX as u64) {out.insert(key.into(),json!(value));}
    }
    Value::Object(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn receipt(topic:&str,payload:Value)->Value {
        sanitize(&failure(topic,&payload,Context{board_id:"board-current",timeline_id:"rtc-current",duplex:true,armed:true,ready:true}).metadata)
    }
    #[test] fn chunk_missing_aec_and_missing_vad_are_different_metadata_failures() {
        let a=receipt("audio/chunk",json!({"conversationSessionId":"rtc-current","seq":42}));
        assert_eq!(a["aecTrigger"],"chunk_aec_missing");assert_eq!(a["errorKind"],"aec_metadata_invalid");assert_eq!(a["aecField"],"missing");assert_eq!(a["timelineBinding"],"current");assert_eq!(a["frameSeq"],42);
        let b=receipt("audio/chunk",json!({"aec":true}));assert_eq!(b["aecTrigger"],"chunk_vad_missing");assert_eq!(b["aecField"],"boolean_true");assert_eq!(b["vadSpeechField"],"missing");
    }
    #[test] fn fields_preserve_presence_type_and_boolean_value_without_forwarding_values() {
        for (v,kind) in [(Value::Null,"null"),(json!(true),"boolean_true"),(json!(false),"boolean_false"),(json!("secret"),"string"),(json!(2),"number"),(json!([]),"array"),(json!({}),"object")] {
            let r=receipt("audio/chunk",json!({"aec":v,"vadSpeech":"PRIVATE_WORDS"}));assert_eq!(r["aecField"],kind);assert!(!r.to_string().contains("PRIVATE_WORDS"));
        }
    }
    #[test] fn aec_false_without_an_explicit_device_fault_does_not_claim_dsp_failure() {
        let a=receipt("audio/chunk",json!({"aec":false,"aecReason":"none"}));assert_eq!(a["aecFailureClass"],"unprocessed_frame");assert_eq!(a["errorKind"],"aec_unprocessed_frame");
        let b=receipt("audio/status",json!({"conversation":true,"audioAec":false,"audioAecPreparing":false,"aecReason":"none"}));assert_eq!(b["aecFailureClass"],"processing_unavailable");assert_eq!(b["aecTopic"],"audio/status");assert_eq!(b["audioAecPreparingField"],"boolean_false");
    }
    #[test] fn only_known_device_reasons_are_device_reported_faults() {
        for reason in FAULTS {let r=receipt("audio/status",json!({"audioAec":false,"aecReason":reason}));assert_eq!(r["reason"],*reason);assert_eq!(r["aecFailureClass"],"device_reported_dsp_failure");}
        for reason in [json!("PRIVATE_PROVIDER_BODY"),Value::Null,json!(false),json!({"apiKey":"SECRET"})] {
            let r=receipt("audio/status",json!({"audioAec":false,"aecReason":reason}));assert_eq!(r["aecFailureClass"],"processing_unavailable");assert_eq!(r["reason"],"unprocessed_audio");assert!(!r.to_string().contains("SECRET"));
        }
    }
    #[test] fn contradictory_processed_flag_with_missing_vad_is_not_labelled_proven_dsp_failure() {
        let r=receipt("audio/chunk",json!({"aec":true,"aecReason":"init_failed"}));assert_eq!(r["aecFailureClass"],"invalid_frame_metadata");assert_eq!(r["aecReasonField"],"known_failure");
    }
    #[test] fn binding_labels_never_contain_received_or_expected_ids() {
        for (value,state) in [(Value::Null,"null"),(json!(""),"empty"),(json!("rtc-current"),"current"),(json!("PRIVATE_OLD_TIMELINE"),"stale"),(json!(7),"wrong_type")] {
            let r=receipt("audio/chunk",json!({"aec":false,"conversationSessionId":value,"boardDeviceId":"PRIVATE_BOARD"}));assert_eq!(r["timelineBinding"],state);assert_eq!(r["boardBinding"],"stale");assert!(!r.to_string().contains("PRIVATE"));assert!(!r.to_string().contains("rtc-current"));
        }
    }
    #[test] fn failure_schema_drops_audio_text_credentials_unknown_enums_and_bad_numeric_types() {
        let mut d=failure("audio/chunk",&json!({"aec":false}),Context{board_id:"board",timeline_id:"rtc",duplex:true,armed:true,ready:false}).metadata;
        for key in ["data","pcm","text","transcript","reply","apiKey","headers","sessionId","conversationSessionId","boardDeviceId"] {d[key]=json!("SECRET_PRIVATE_DATA");}
        d["aecField"]=json!("secret");d["frameSeq"]=json!("secret");d["declaredBytes"]=json!(65537);d["dspFrames"]=json!(-1);d["captureReady"]=json!("secret");d["reason"]=json!("privatewords");d["errorKind"]=json!("credentials");
        let safe=sanitize(&d);assert!(!safe.to_string().contains("SECRET"));assert!(!safe.to_string().contains("secret"));assert!(!safe.to_string().contains("privatewords"));assert!(!safe.to_string().contains("credentials"));
        for key in ["frameSeq","declaredBytes","dspFrames","captureReady","aecField","reason","errorKind","data","apiKey"] {assert!(safe.get(key).is_none(),"{key}");}
    }
    #[test] fn bounded_numeric_dsp_metadata_survives_without_new_payload_or_id_fields() {
        let r=receipt("audio/chunk",json!({"aec":false,"seq":u32::MAX,"bytes":640,"dspFrames":5115,"dspMaxUs":10217,"captureTxMaxUs":779,"captureStackMinBytes":1636}));assert_eq!(r["frameSeq"],u32::MAX);assert_eq!(r["declaredBytes"],640);assert_eq!(r["dspFrames"],5115);assert_eq!(r["captureTxMaxUs"],779);
        assert_eq!(receipt("audio/error",json!({"aec":false})),json!({}));
    }
}
