//! Track C capability expansion for bounded components: p4-sequence-v1 (on-device note
//! sequencer), p4-samples-v1 (PCM one-shots), p4-clip-v1 (one H.264 clip node),
//! p4-big-image-v1 (tiled pannable image) and p4-events-v1 (event bus + alarms).
//! Declaration checks mirror pc/skills/petui/scripts/capability_ext.py and the firmware
//! parser through the shared contracts/*.json vectors; `strip` hands the unchanged
//! validators the base declaration; install compiles the media into the device
//! containers (P4A1 samples, P4V1 clips, P4T1 tile sets) through the bundled ffmpeg.
use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::path::Path;

pub(crate) const SEQUENCE_ENGINE: &str = "p4-sequence-v1";
pub(crate) const SAMPLES_ENGINE: &str = "p4-samples-v1";
pub(crate) const CLIP_ENGINE: &str = "p4-clip-v1";
pub(crate) const BIG_IMAGE_ENGINE: &str = "p4-big-image-v1";
pub(crate) const EVENTS_ENGINE: &str = "p4-events-v1";
const WAVES: [&str; 3] = ["square", "triangle", "noise"];
const AUDIO_OWNERS: [&str; 4] = ["instrument", "media", "local", "program"];
pub(crate) const SAMPLE_RATE: u32 = 16000;
pub(crate) const SAMPLE_BYTES: usize = 64 * 1024;
pub(crate) const SAMPLE_BUDGET: usize = 256 * 1024;
pub(crate) const CLIP_FPS: u32 = 15;
pub(crate) const CLIP_MAX_FRAMES: u32 = 75;
pub(crate) const CLIP_BYTES: usize = 512 * 1024;
pub(crate) const CLIP_HEADER: usize = 16;
pub(crate) const BIG_TILE: u32 = 240;
pub(crate) const BIG_TILES: u32 = 16;
pub(crate) const BIG_BUDGET: usize = 1024 * 1024;
pub(crate) const BIG_TILE_BYTES: usize = 64 * 1024;
pub(crate) const BIG_HEADER: usize = 20;
const EVENT_NAMES: usize = 8;
const ALARMS: usize = 4;
const ALARM_MIN_MS: i64 = 60000;
const ALARM_MAX_MS: i64 = 7 * 86_400_000;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) enum MediaKind { Wav, Mp4, Png }

fn name_ok(s: &str) -> bool { !s.is_empty() && s.len() <= 15 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) }
fn int(v: &Value, lo: i64, hi: i64) -> bool { v.as_i64().is_some_and(|n| (lo..=hi).contains(&n)) }
fn asset_id(asset: &Value, ext: &str) -> Option<String> {
    let a = asset.as_str()?;
    let name = a.strip_prefix("assets/")?.strip_suffix(ext)?;
    (!name.contains('/') && name_ok(name)).then(|| name.to_string())
}
pub(crate) fn big_tiles(width: u64, height: u64) -> u64 { width.div_ceil(u64::from(BIG_TILE)) * height.div_ceil(u64::from(BIG_TILE)) }
pub(crate) fn event_action(on: &str) -> bool { on.strip_prefix("event:").is_some_and(name_ok) }
fn widget(files: &HashMap<String, Vec<u8>>) -> Value { files.get("runtime/widget.json").and_then(|b| serde_json::from_slice(b).ok()).unwrap_or_default() }
fn keys_within(o: &Value, allowed: &[&str]) -> bool { o.as_object().is_some_and(|m| m.keys().all(|k| allowed.contains(&k.as_str()))) }

fn ops(location: &str, obj: &Value, errors: &mut Vec<String>, sequence: bool, samples: &[String], emits: &[String]) {
    if !obj.is_object() { return; }
    if obj.get("sequence").is_some() && (!sequence || !matches!(obj["sequence"].as_str(), Some("play" | "stop"))) { errors.push(format!("{location}.sequence 需要声明 sequence={SEQUENCE_ENGINE} 且只接受 play/stop")); }
    if obj.get("sample").is_some() && !obj["sample"].as_str().is_some_and(|s| samples.iter().any(|k| k == s)) { errors.push(format!("{location}.sample 需要声明 samples={SAMPLES_ENGINE} 并引用已声明 id")); }
    if obj.get("emit").is_some() && !obj["emit"].as_str().is_some_and(|s| emits.iter().any(|k| k == s)) { errors.push(format!("{location}.emit 需要声明 events={EVENTS_ENGINE} 并引用已声明名称")); }
}

/// Declaration-level validation of every Track C section (capabilities are checked at install).
pub(crate) fn validate_runtime(w: &Value, errors: &mut Vec<String>) {
    if !w.is_object() { return; }
    let v4 = w["engine"] == "p4-bounded-runtime-v4";
    let exclusive = AUDIO_OWNERS.iter().any(|k| w.get(*k).is_some());
    let mut sequence = false;
    let mut sample_ids: Vec<String> = Vec::new();
    let mut emit_names: Vec<String> = Vec::new();
    if let Some(s) = w.get("sequence") {
        let mut ok = keys_within(s, &["engine", "bpm", "volume", "tracks", "loop"]) && s["engine"] == SEQUENCE_ENGINE && v4 && !exclusive
            && int(&s["bpm"], 40, 240) && int(&s["volume"], 0, 100) && s.get("loop").is_none_or(Value::is_boolean);
        let tracks = s["tracks"].as_array().filter(|a| (1..=2).contains(&a.len()));
        ok = ok && tracks.is_some();
        if ok {
            for t in tracks.unwrap() {
                let notes = t["notes"].as_array();
                if !t.as_object().is_some_and(|o| o.len() == 3 && o.contains_key("wave") && o.contains_key("volume") && o.contains_key("notes"))
                    || !t["wave"].as_str().is_some_and(|wv| WAVES.contains(&wv)) || !int(&t["volume"], 0, 100)
                    || !notes.is_some_and(|n| (1..=64).contains(&n.len()) && n.iter().all(|n| n.as_array().is_some_and(|p| p.len() == 2 && (p[0] == 0 || int(&p[0], 36, 96)) && int(&p[1], 1, 16)))) { ok = false; }
            }
        }
        if ok { sequence = true; } else { errors.push(format!("runtime/widget.json.sequence 需要 engine={SEQUENCE_ENGINE}、bpm 40-240、volume 0-100、1-2 条轨道（wave square/triangle/noise，1-64 个 [midi 36-96|0, beats 1-16]），且不与 instrument/media/local/program 混用")); }
    }
    if let Some(s) = w.get("samples") {
        let mut ok = keys_within(s, &["engine", "volume", "items"]) && s["engine"] == SAMPLES_ENGINE && v4 && !exclusive && int(&s["volume"], 0, 100);
        let items = s["items"].as_array().filter(|a| (1..=4).contains(&a.len()));
        ok = ok && items.is_some();
        if ok {
            for it in items.unwrap() {
                let id = it["id"].as_str().filter(|id| name_ok(id));
                if !it.as_object().is_some_and(|o| o.len() == 2) || id.is_none() || asset_id(&it["asset"], ".wav").as_deref() != id || sample_ids.iter().any(|k| Some(k.as_str()) == id) { ok = false; break; }
                sample_ids.push(id.unwrap().to_string());
            }
        }
        if !ok { sample_ids.clear(); errors.push(format!("runtime/widget.json.samples 需要 engine={SAMPLES_ENGINE}、volume 0-100 与 1-4 个唯一 {{id, asset: assets/<id>.wav}}，且不与 instrument/media/local/program 混用")); }
    }
    let mut events = false;
    if let Some(e) = w.get("events") {
        let emit: Vec<&str> = e["emit"].as_array().map(|a| a.iter().filter_map(Value::as_str).collect()).unwrap_or_default();
        let ok = keys_within(e, &["engine", "emit"]) && e["engine"] == EVENTS_ENGINE && v4 && e.get("emit").is_none_or(|a| a.as_array().is_some_and(|a| a.len() <= EVENT_NAMES && a.len() == emit.len()))
            && emit.iter().all(|n| name_ok(n)) && emit.iter().collect::<std::collections::HashSet<_>>().len() == emit.len();
        if ok { events = true; emit_names = emit.iter().map(|s| s.to_string()).collect(); }
        else { errors.push(format!("runtime/widget.json.events 需要 engine={EVENTS_ENGINE} 与最多 8 个唯一安全事件名（1-15 位 A-Za-z0-9_.-）")); }
    }
    let transitions: Vec<&Value> = w["transitions"].as_array().map(|a| a.iter().collect()).unwrap_or_default();
    let triggers: Vec<&str> = transitions.iter().filter_map(|t| t["on"].as_str()).collect();
    let subs: Vec<&str> = triggers.iter().filter_map(|on| on.strip_prefix("event:")).collect();
    if !subs.is_empty() && (!events || subs.iter().collect::<std::collections::HashSet<_>>().len() > EVENT_NAMES || subs.iter().any(|n| !name_ok(n))) {
        errors.push(format!("transitions.on 使用 event:<name> 需要声明 events={EVENTS_ENGINE}，最多 8 个不同名称"));
    }
    for (i, t) in transitions.iter().enumerate() {
        ops(&format!("transitions[{i}]"), t, errors, sequence, &sample_ids, &emit_names);
        if t["else"].as_object().is_some_and(|o| ["sequence", "sample", "emit"].iter().any(|k| o.contains_key(*k))) { errors.push(format!("transitions[{i}].else 不接受 sequence/sample/emit")); }
    }
    for (i, t) in w["tick"].as_array().into_iter().flatten().enumerate() {
        ops(&format!("tick[{i}].then"), &t["then"], errors, sequence, &sample_ids, &emit_names);
        if t.as_object().is_some_and(|o| ["sequence", "sample", "emit"].iter().any(|k| o.contains_key(*k))) { errors.push(format!("tick[{i}] 的 sequence/sample/emit 只能放在 then 中")); }
    }
    if let Some(alarms) = w.get("alarms") {
        let mut ok = events && w["persist"].is_object() && alarms.as_array().is_some_and(|a| (1..=ALARMS).contains(&a.len()));
        let mut ids: Vec<&str> = Vec::new();
        if ok {
            for a in alarms.as_array().unwrap() {
                let id = a["id"].as_str().filter(|id| name_ok(id));
                if !keys_within(a, &["id", "every_ms", "at_ms_var", "action", "background", "text"]) || id.is_none() || ids.contains(&id.unwrap()) { ok = false; break; }
                ids.push(id.unwrap());
                let periodic = a.get("every_ms").is_some();
                if periodic == a.get("at_ms_var").is_some() || !a["action"].as_str().is_some_and(|act| (act.len() < 48 && !act.is_empty() && (act.bytes().all(|b|b.is_ascii_alphanumeric() || b"._-".contains(&b)) || event_action(act))) && triggers.contains(&act)) || !a.get("background").is_none_or(Value::is_boolean) { ok = false; break; }
                if periodic && !int(&a["every_ms"], ALARM_MIN_MS, ALARM_MAX_MS) { ok = false; break; }
                if !periodic && !a["at_ms_var"].as_str().is_some_and(|v| w["vars"][v]["type"] == "int") { ok = false; break; }
                if !a.get("text").is_none_or(|t| t.as_str().is_some_and(|t| t.len() <= 40)) { ok = false; break; }
            }
        }
        if !ok { errors.push(format!("runtime/widget.json.alarms 需要 events={EVENTS_ENGINE} 与 persist，1-4 个 {{id, every_ms>=60000 | at_ms_var:<int var>, action:<已声明 on>, background?, text<=40 字节}}")); }
    }
    let surface = &w["surface"];
    if surface.is_object() {
        let app = surface["engine"] == "p4-app-surface-v1" && surface["graphics"] == "p4-app-graphics-v1";
        let bindings = surface["bindings"] == "p4-variable-bindings-v1";
        let elements: Vec<&Value> = surface["elements"].as_array().map(|a| a.iter().collect()).unwrap_or_default();
        let clips: Vec<&&Value> = elements.iter().filter(|n| n["type"] == "clip").collect();
        if !clips.is_empty() || elements.iter().any(|n| n.get("asset").is_some()) {
            let mut ok = app && bindings && clips.len() == 1 && elements.iter().all(|n| n.get("asset").is_none() || n["type"] == "clip");
            for n in &clips {
                ok = ok && keys_within(n, &["type", "x", "y", "w", "h", "color", "asset", "page", "state_mask", "host_mask", "show"])
                    && matches!((n["w"].as_u64(), n["h"].as_u64()), (Some(160), Some(120)) | (Some(320), Some(240))) && asset_id(&n["asset"], ".mp4").is_some();
            }
            if !ok { errors.push("surface clip 节点需要 p4-app-graphics-v1 + p4-variable-bindings-v1、恰好 1 个 {type:clip, w/h 160x120|320x240, asset: assets/<id>.mp4}，不接受 action/motion/tile".into()); }
        }
        let bigs = surface.get("big_images");
        let big_nodes: Vec<&&Value> = elements.iter().filter(|n| n.get("big").is_some() || n.get("offset_x_var").is_some() || n.get("offset_y_var").is_some()).collect();
        if bigs.is_some() || !big_nodes.is_empty() {
            let mut declared: HashMap<String, (u64, u64)> = HashMap::new();
            let mut ok = app && bindings && bigs.is_some_and(|b| b.as_array().is_some_and(|a| (1..=2).contains(&a.len())));
            if ok {
                for b in bigs.unwrap().as_array().unwrap() {
                    let id = b["id"].as_str().filter(|id| name_ok(id)).map(str::to_string);
                    if !b.as_object().is_some_and(|o| o.len() == 4) || id.is_none() || asset_id(&b["asset"], ".png") != id || declared.contains_key(id.as_ref().unwrap())
                        || !int(&b["width"], i64::from(BIG_TILE) + 1, 1280) || !int(&b["height"], 1, 960) || big_tiles(b["width"].as_u64().unwrap(), b["height"].as_u64().unwrap()) > u64::from(BIG_TILES) { ok = false; break; }
                    declared.insert(id.unwrap(), (b["width"].as_u64().unwrap(), b["height"].as_u64().unwrap()));
                }
            }
            if ok {
                for n in &big_nodes {
                    let dims = n["big"].as_str().and_then(|id| declared.get(id));
                    if n["type"] != "image" || n.get("image").is_some() || dims.is_none() || n.get("offset_x_var").is_none() || n.get("offset_y_var").is_none()
                        || ["tile", "rotate_ms", "motion", "action", "in_view", "view"].iter().any(|k| n.get(*k).is_some())
                        || !["offset_x_var", "offset_y_var"].iter().all(|k| n[*k].as_str().is_some_and(|v| w["vars"][v]["type"] == "int"))
                        || !n["w"].as_u64().is_some_and(|v| v >= 1 && v <= dims.unwrap().0) || !n["h"].as_u64().is_some_and(|v| v >= 1 && v <= dims.unwrap().1) { ok = false; break; }
                }
            }
            if !ok { errors.push("surface.big_images 需要 p4-app-graphics-v1 + p4-variable-bindings-v1、1-2 个 {id, asset: assets/<id>.png, width 241-1280, height 1-960} 且 ceil(w/240)*ceil(h/240)<=16；image 节点用 big + 两个 int offset 变量引用，窗口不大于图片".into()); }
        }
    }
}
pub(crate) fn internal_actions(w: &Value) -> HashSet<String> {
    let mut errors = Vec::new(); validate_runtime(w, &mut errors);
    if !errors.is_empty() || w["engine"] != "p4-bounded-runtime-v4" || w["events"]["engine"] != EVENTS_ENGINE { return HashSet::new(); }
    let names: HashSet<&str> = w["transitions"].as_array().into_iter().flatten().filter_map(|t|t["on"].as_str()).collect();
    let mut out: HashSet<String> = names.iter().filter(|n|event_action(n)).map(|n|(*n).to_string()).collect();
    for alarm in w["alarms"].as_array().into_iter().flatten() {
        if let Some(action) = alarm["action"].as_str().filter(|a|names.contains(a)) { out.insert(action.to_string()); }
    }
    out
}
fn validate_internal_binding_coverage(w: &Value, buttons: &Value, errors: &mut Vec<String>) {
    // Preserve legacy non-Track-C package validation. In an event/alarm runtime,
    // ordinary actions still require their physical or explicitly allowed touch path.
    if w["engine"] != "p4-bounded-runtime-v4" || (w.get("events").is_none() && w.get("alarms").is_none()) { return; }
    let internal = internal_actions(w);
    let mut bound: HashSet<&str> = buttons.as_array().into_iter().flatten().filter_map(|b|b["action"].as_str()).collect();
    if w["input"]["engine"] == "p4-input-v1" {
        bound.extend(w["surface"]["elements"].as_array().into_iter().flatten().filter_map(|n|n["action"].as_str()));
        bound.extend(w["input"]["swipe"].as_object().into_iter().flat_map(|o|o.values()).filter_map(Value::as_str));
    }
    let declared: HashSet<&str> = w["transitions"].as_array().into_iter().flatten().filter_map(|t|t["on"].as_str()).collect();
    for action in &declared {
        if !internal.contains(*action) && !bound.contains(action) { errors.push(format!("transitions 存在未绑定 action: {action}")); }
    }
    for action in buttons.as_array().into_iter().flatten().filter_map(|b|b["action"].as_str()) {
        if !declared.contains(action) { errors.push(format!("按钮 action 未出现在 transitions: {action}")); }
    }
}
pub(crate) fn validate(files: &HashMap<String, Vec<u8>>, errors: &mut Vec<String>) {
    let w = widget(files); validate_runtime(&w, errors);
    let buttons = files.get("buttons.json").and_then(|b|serde_json::from_slice::<Value>(b).ok()).unwrap_or(Value::Null);
    validate_internal_binding_coverage(&w, &buttons, errors);
}

/// The runtime without Track C extensions, for the unchanged surface/runtime validators.
pub(crate) fn strip(w: &Value) -> Value {
    let mut out = w.clone();
    let Some(o) = out.as_object_mut() else { return out; };
    for k in ["sequence", "samples", "events", "alarms"] { o.remove(k); }
    for t in o.get_mut("transitions").and_then(Value::as_array_mut).into_iter().flatten() { if let Some(t) = t.as_object_mut() { for k in ["sequence", "sample", "emit"] { t.remove(k); } } }
    for t in o.get_mut("tick").and_then(Value::as_array_mut).into_iter().flatten() { if let Some(then) = t.get_mut("then").and_then(Value::as_object_mut) { for k in ["sequence", "sample", "emit"] { then.remove(k); } } }
    if let Some(s) = o.get_mut("surface").and_then(Value::as_object_mut) {
        s.remove("big_images");
        for n in s.get_mut("elements").and_then(Value::as_array_mut).into_iter().flatten() {
            if n["type"] == "clip" || n.get("big").is_some() {
                let mut keep = json!({"type": "panel"});
                for k in ["x", "y", "w", "h", "color", "page", "state_mask", "host_mask", "show"] { if let Some(v) = n.get(k) { keep[k] = v.clone(); } }
                *n = keep;
            }
        }
    }
    out
}

/// Every declared media asset (package path, kind).
pub(crate) fn media_assets(w: &Value) -> Vec<(String, MediaKind)> {
    let mut out = Vec::new();
    for it in w["samples"]["items"].as_array().into_iter().flatten() { if let Some(a) = it["asset"].as_str() { out.push((a.to_string(), MediaKind::Wav)); } }
    for n in w["surface"]["elements"].as_array().into_iter().flatten() { if n["type"] == "clip" { if let Some(a) = n["asset"].as_str() { out.push((a.to_string(), MediaKind::Mp4)); } } }
    for b in w["surface"]["big_images"].as_array().into_iter().flatten() { if let Some(a) = b["asset"].as_str() { out.push((a.to_string(), MediaKind::Png)); } }
    out
}

/// Install gate: the connected device must advertise every capability the package uses.
pub(crate) fn require_capability(w: &Value, caps: &Value) -> Result<(), String> {
    let uses_clip = w["surface"]["elements"].as_array().is_some_and(|a| a.iter().any(|n| n["type"] == "clip"));
    let checks = [
        (w.get("sequence").is_some(), "widgetSequence", SEQUENCE_ENGINE, "需要组件音序能力 p4-sequence-v1，请先升级固件；未安装不兼容资源"),
        (w.get("samples").is_some(), "widgetSamples", SAMPLES_ENGINE, "需要组件采样能力 p4-samples-v1，请先升级固件；未安装不兼容资源"),
        (uses_clip, "widgetClip", CLIP_ENGINE, "需要组件短视频能力 p4-clip-v1，请先升级固件；未安装不兼容资源"),
        (w["surface"].get("big_images").is_some(), "widgetBigImage", BIG_IMAGE_ENGINE, "需要大图分块能力 p4-big-image-v1，请先升级固件；未安装不兼容资源"),
        (w.get("events").is_some() || w.get("alarms").is_some(), "widgetEvents", EVENTS_ENGINE, "需要跨组件事件与闹钟能力 p4-events-v1，请先升级固件；未安装不兼容资源"),
    ];
    for (used, key, engine, message) in checks { if used && caps[key] != engine { return Err(message.to_string()); } }
    Ok(())
}

// ---- device containers --------------------------------------------------------------------
/// P4A1: "P4A1" + u32 frames + s16le mono 16 kHz PCM.
pub(crate) fn encode_sample(pcm: &[i16]) -> Result<Vec<u8>, String> {
    if pcm.is_empty() || pcm.len() * 2 > SAMPLE_BYTES { return Err(format!("采样重采样后为 {} 字节，超过 64 KiB（约 2.0 s）", pcm.len() * 2)); }
    let mut out = b"P4A1".to_vec();
    out.extend((pcm.len() as u32).to_le_bytes());
    for s in pcm { out.extend(s.to_le_bytes()); }
    Ok(out)
}
/// Pure PCM WAV reader: 8/16/24/32-bit integer PCM, any channel count (mixed down), linear
/// resampling to 16 kHz. Non-PCM WAVs go through ffmpeg instead.
pub(crate) fn pcm_wav_to_16k_mono(bytes: &[u8]) -> Result<Vec<i16>, String> {
    let bad = || "采样必须是 RIFF/WAVE PCM 文件".to_string();
    if bytes.len() < 12 || &bytes[..4] != b"RIFF" || &bytes[8..12] != b"WAVE" { return Err(bad()); }
    let (mut at, mut fmt, mut data): (usize, Option<(u16, u16, u32, u16)>, Option<&[u8]>) = (12, None, None);
    while at + 8 <= bytes.len() {
        let size = u32::from_le_bytes(bytes[at + 4..at + 8].try_into().unwrap()) as usize;
        if size > bytes.len() - at - 8 { return Err(bad()); }
        let body = &bytes[at + 8..at + 8 + size];
        if &bytes[at..at + 4] == b"fmt " {
            if body.len() < 16 { return Err(bad()); }
            let tag = u16::from_le_bytes([body[0], body[1]]);
            let channels = u16::from_le_bytes([body[2], body[3]]);
            let bits = u16::from_le_bytes([body[14], body[15]]);
            let rate = u32::from_le_bytes(body[4..8].try_into().unwrap());
            let frame = u64::from(channels) * u64::from(bits / 8);
            if u64::from(u16::from_le_bytes([body[12],body[13]])) != frame
                || u64::from(u32::from_le_bytes(body[8..12].try_into().unwrap())) != u64::from(rate) * frame { return Err(bad()); }
            if tag == 0xfffe && (body.len() < 40 || u16::from_le_bytes([body[16],body[17]]) < 22
                || body[24..40] != [1,0,0,0,0,0,16,0,128,0,0,170,0,56,155,113]) { return Err(bad()); }
        }
        match &bytes[at..at + 4] {
            b"fmt " if body.len() >= 16 => fmt = Some((u16::from_le_bytes([body[0], body[1]]), u16::from_le_bytes([body[2], body[3]]), u32::from_le_bytes(body[4..8].try_into().unwrap()), u16::from_le_bytes([body[14], body[15]]))),
            b"data" => data = Some(body),
            _ => {}
        }
        at += 8 + size + (size & 1);
    }
    let (Some((tag, channels, rate, bits)), Some(data)) = (fmt, data) else { return Err(bad()); };
    if !(tag == 1 || tag == 0xFFFE) || channels == 0 || !(8000..=192000).contains(&rate) || ![8, 16, 24, 32].contains(&bits) { return Err(bad()); }
    let frame = usize::from(channels) * usize::from(bits / 8);
    let frames = data.len() / frame;
    if frames == 0 || data.len() % frame != 0 { return Err(bad()); }
    let out_len = ((frames as u64) * u64::from(SAMPLE_RATE) / u64::from(rate)).max(1) as usize;
    if out_len > SAMPLE_BYTES / 2 { return Err(format!("采样重采样后为 {} 字节，超过 64 KiB（约 2.0 s）", out_len * 2)); }
    let mono: Vec<f64> = (0..frames).map(|f| {
        (0..usize::from(channels)).map(|c| {
            let p = &data[f * frame + c * usize::from(bits / 8)..];
            match bits { 8 => (f64::from(p[0]) - 128.0) / 128.0, 16 => f64::from(i16::from_le_bytes([p[0], p[1]])) / 32768.0,
                24 => f64::from(i32::from_le_bytes([0, p[0], p[1], p[2]]) >> 8) / 8_388_608.0, _ => f64::from(i32::from_le_bytes([p[0], p[1], p[2], p[3]])) / 2_147_483_648.0 }
        }).sum::<f64>() / f64::from(channels)
    }).collect();
    Ok((0..out_len).map(|i| {
        let pos = i as f64 * f64::from(rate) / f64::from(SAMPLE_RATE);
        let (a, t) = (pos.floor() as usize, pos.fract());
        let b = (a + 1).min(frames - 1);
        let v = mono[a.min(frames - 1)] * (1.0 - t) + mono[b] * t;
        (v.clamp(-1.0, 1.0) * 32767.0).round() as i16
    }).collect())
}
/// P4V1: "P4V1" w u16 h u16 fps u8 pad u8 frames u16 bytes u32, then Annex-B H.264.
pub(crate) fn encode_clip(width: u16, height: u16, frames: u16, stream: &[u8]) -> Result<Vec<u8>, String> {
    if !matches!((width, height), (160, 120) | (320, 240)) { return Err("短视频只支持 160x120 或 320x240".into()); }
    if frames == 0 || u32::from(frames) > CLIP_MAX_FRAMES { return Err(format!("短视频为 {frames} 帧，须为 1-75 帧（15 fps、不超过 5 s）")); }
    if stream.is_empty() || stream.len() + CLIP_HEADER > CLIP_BYTES { return Err(format!("短视频 H.264 为 {} 字节，超过 512 KiB", stream.len())); }
    let mut out = b"P4V1".to_vec();
    out.extend(width.to_le_bytes()); out.extend(height.to_le_bytes()); out.push(CLIP_FPS as u8); out.push(0);
    out.extend(frames.to_le_bytes()); out.extend((stream.len() as u32).to_le_bytes()); out.extend_from_slice(stream);
    Ok(out)
}
/// Counts coded pictures in an Annex-B stream: slice NALs (1/5) whose first_mb_in_slice is 0.
pub(crate) fn h264_frame_count(stream: &[u8]) -> u32 {
    let mut frames = 0; let mut i = 0;
    while i + 4 < stream.len() {
        let prefix = if stream[i..i + 4] == [0, 0, 0, 1] { 4 } else if stream[i..i + 3] == [0, 0, 1] { 3 } else { i += 1; continue };
        let header = i + prefix;
        if header + 1 < stream.len() && matches!(stream[header] & 0x1f, 1 | 5) && stream[header + 1] & 0x80 != 0 { frames += 1; }
        i = header;
    }
    frames
}
/// P4T1: header (20 bytes) + count x {offset u32, bytes u32} + P4I1 tiles (row-major, <= 240x240 each).
pub(crate) fn encode_big_tiles(image: &image::RgbaImage) -> Result<Vec<u8>, String> {
    let (w, h) = image.dimensions();
    if w <= BIG_TILE || w > 1280 || h == 0 || h > 960 || big_tiles(u64::from(w), u64::from(h)) > u64::from(BIG_TILES) { return Err("大图需要 241-1280 x 1-960 像素且 ceil(w/240)*ceil(h/240) <= 16 块".into()); }
    let (cols, rows) = (w.div_ceil(BIG_TILE), h.div_ceil(BIG_TILE));
    let (tile_w, tile_h) = (w.div_ceil(cols), h.div_ceil(rows));
    let mut tiles = Vec::new();
    for row in 0..rows { for col in 0..cols {
        let (x, y) = (col * tile_w, row * tile_h);
        let (tw, th) = ((w - x).min(tile_w), (h - y).min(tile_h));
        let tile = image::imageops::crop_imm(image, x, y, tw, th).to_image();
        let encoded = crate::surface_image::encode(&tile);
        if encoded.len() > BIG_TILE_BYTES { return Err(format!("大图分块 {row}/{col} 编码后 {} 字节，超过 64 KiB，请简化素材", encoded.len())); }
        tiles.push(encoded);
    } }
    let mut out = b"P4T1".to_vec();
    out.push(cols as u8); out.push(rows as u8);
    out.extend((tile_w as u16).to_le_bytes()); out.extend((tile_h as u16).to_le_bytes()); out.extend((w as u16).to_le_bytes()); out.extend((h as u16).to_le_bytes());
    out.push(tiles.len() as u8); out.push(0); out.extend(0u32.to_le_bytes());
    let mut offset = (BIG_HEADER + tiles.len() * 8) as u32;
    for t in &tiles { out.extend(offset.to_le_bytes()); out.extend((t.len() as u32).to_le_bytes()); offset += t.len() as u32; }
    for t in &tiles { out.extend_from_slice(t); }
    if out.len() > BIG_BUDGET { return Err(format!("大图分块合计 {} 字节，超过 1 MiB 预算", out.len())); }
    Ok(out)
}

// Read at most one byte beyond the limit, including when a file grows after metadata.
fn read_bounded(path: &Path, limit: usize) -> Result<Vec<u8>, String> {
    use std::io::Read;
    let file = std::fs::File::open(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let mut bytes = Vec::new();
    file.take(limit as u64 + 1).read_to_end(&mut bytes).map_err(|e| e.to_string())?;
    if bytes.len() > limit { return Err(format!("{} exceeds {} byte media budget", path.display(), limit)); }
    Ok(bytes)
}

// At most one stdout reader/process at a time. The reader owns the admission bit
// until EOF, even if a broken child leaves an inherited pipe open after timeout.
static MEDIA_PROCESS_BUSY: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
fn run_media_process(command: &mut std::process::Command, limit: usize, timeout: std::time::Duration) -> Result<Vec<u8>, String> {
    use std::io::Read;
    use std::process::Stdio;
    use std::sync::atomic::Ordering;
    if MEDIA_PROCESS_BUSY.compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire).is_err() {
        return Err("media conversion busy".into());
    }
    struct Admission;
    impl Drop for Admission { fn drop(&mut self) { MEDIA_PROCESS_BUSY.store(false, Ordering::Release); } }
    let admission = std::sync::Arc::new(Admission);
    let reader_admission = admission.clone();
    let mut child = command.stdin(Stdio::null()).stdout(Stdio::piped()).stderr(Stdio::null())
        .spawn().map_err(|e| e.to_string())?;
    let stdout = child.stdout.take().ok_or("media stdout unavailable")?;
    let (tx, rx) = std::sync::mpsc::sync_channel(1);
    let reader = std::thread::Builder::new().name("bounded-media-output".into()).spawn(move || {
        let _admission = reader_admission;
        let mut stdout = stdout;
        let mut bytes = vec![0u8; limit + 1];
        let mut used = 0;
        let result = loop {
            match stdout.read(&mut bytes[used..]) {
                Ok(0) => { bytes.truncate(used); break Ok(bytes); }
                Ok(n) => { used += n; if used > limit { break Err("media output exceeds budget".to_string()); } }
                Err(e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
                Err(e) => break Err(e.to_string()),
            }
        };
        drop(stdout);
        drop(_admission);
        let _ = tx.send(result);
    });
    if let Err(e) = reader { let _ = child.kill(); let _ = child.wait(); return Err(e.to_string()); }
    let deadline = std::time::Instant::now() + timeout;
    let mut output = None;
    let result = loop {
        if std::time::Instant::now() >= deadline { break Err("media conversion timeout".into()); }
        match rx.try_recv() {
            Ok(Ok(bytes)) => output = Some(bytes),
            Ok(Err(e)) => break Err(e),
            Err(std::sync::mpsc::TryRecvError::Disconnected) if output.is_none() => break Err("media output reader failed".into()),
            _ => {}
        }
        match child.try_wait() {
            Ok(Some(status)) if !status.success() => break Err(format!("media conversion exited {status}")),
            Ok(Some(_)) if output.is_some() => break Ok(output.take().unwrap()),
            Err(e) => break Err(e.to_string()),
            _ => {}
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    };
    // Every exit path reaps the child. No unbounded stderr capture or output files.
    let _ = child.kill(); let _ = child.wait();
    result
}
fn bounded_ffmpeg_path() -> Option<String> {
    if let Some(path) = std::env::var_os("PET_MANAGER_BUNDLED_FFMPEG") {
        if Path::new(&path).is_file() { return Some(path.to_string_lossy().into_owned()); }
    }
    if let Ok(executable) = std::env::current_exe() {
        #[cfg(target_os = "macos")]
        if let Some(contents) = executable.parent().and_then(Path::parent) {
            let path = contents.join("Resources/tools/ffmpeg");
            if path.is_file() { return Some(path.to_string_lossy().into_owned()); }
        }
        #[cfg(windows)]
        if let Some(dir) = executable.parent() {
            let path = dir.join("tools/ffmpeg.exe");
            if path.is_file() { return Some(path.to_string_lossy().into_owned()); }
        }
    }
    crate::find_executable("ffmpeg", &[])
}
fn ffmpeg_wav_to_pcm(ffmpeg: &str, input: &Path) -> Result<Vec<i16>, String> {
    let mut command = crate::command_for_host(ffmpeg);
    command.args(["-v", "error", "-nostdin", "-max_alloc", "16777216", "-threads", "1", "-i"]).arg(input)
        .args(["-map", "0:a:0", "-vn", "-ac", "1", "-ar", "16000", "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"]);
    let bytes = run_media_process(&mut command, SAMPLE_BYTES, std::time::Duration::from_secs(30))?;
    if bytes.is_empty() || bytes.len() % 2 != 0 { return Err("invalid sample output".into()); }
    Ok(bytes.chunks_exact(2).map(|s| i16::from_le_bytes([s[0], s[1]])).collect())
}
fn ffmpeg_clip(ffmpeg: &str, input: &Path, width: u16, height: u16) -> Result<Vec<u8>, String> {
    if !cfg!(any(target_os = "macos", windows)) { return Err("unsupported system H.264 encoder platform".into()); }
    // Select the same system encoder without the shared unbounded -encoders probe.
    #[cfg(target_os = "macos")]
    let encoder = crate::codex_import::PlatformH264Encoder::VideoToolbox;
    #[cfg(not(target_os = "macos"))]
    let encoder = crate::codex_import::PlatformH264Encoder::MediaFoundation;
    let mut args = vec!["-max_alloc".into(), "16777216".into(), "-threads".into(), "1".into(), "-y".into(), "-v".into(), "error".into(), "-nostdin".into(), "-t".into(), "5".into(), "-i".into(), input.display().to_string(),
        "-vf".into(), format!("fps={CLIP_FPS},scale={width}:{height}:force_original_aspect_ratio=decrease,pad={width}:{height}:(ow-iw)/2:(oh-ih)/2:black"),
        "-frames:v".into(), CLIP_MAX_FRAMES.to_string(), "-an".into(), "-c:v".into(), encoder.codec_name().into(), "-profile:v".into(), encoder.profile_value().into(),
        "-level:v".into(), encoder.level_arg().into(), "-pix_fmt".into(), "nv12".into(), "-g".into(), CLIP_MAX_FRAMES.to_string(), "-bf".into(), "0".into(), "-refs".into(), "1".into(),
        "-b:v".into(), "500k".into(), "-maxrate".into(), "700k".into(), "-bufsize".into(), "1400k".into()];
    encoder.append_quality_args(&mut args);
    if encoder == crate::codex_import::PlatformH264Encoder::VideoToolbox { args.extend(["-bsf:v".into(), "h264_metadata=aud=insert".into()]); }
    args.extend(["-f".into(), "h264".into(), "pipe:1".into()]);
    let mut command = crate::command_for_host(ffmpeg); command.args(&args);
    run_media_process(&mut command, CLIP_BYTES - CLIP_HEADER, std::time::Duration::from_secs(30))
}
/// Compiles the package's declared media into the device files the install transaction carries.
pub(crate) fn prepare_p4_widget_media_files(widget_root: &Path, w: &Value) -> Result<Vec<(String, Vec<u8>)>, String> {
    let mut errors = Vec::new(); validate_runtime(w, &mut errors);
    if !errors.is_empty() { return Err(errors.join("; ")); }
    let assets = media_assets(w);
    if assets.is_empty() { return Ok(Vec::new()); }
    // Do not call the shared resolver: its version/encoder subprocesses are unbounded.
    let ffmpeg = bounded_ffmpeg_path();
    let mut out = Vec::new();
    let mut sample_total = 0usize;
    for (asset, kind) in &assets {
        if asset_id(&Value::String(asset.clone()), match kind { MediaKind::Wav => ".wav", MediaKind::Mp4 => ".mp4", MediaKind::Png => ".png" }).is_none() { return Err("invalid media asset path".into()); }
        let path = widget_root.join(asset).canonicalize().map_err(|e| e.to_string())?;
        if !path.starts_with(widget_root.canonicalize().map_err(|e| e.to_string())?) { return Err("media asset escapes package".into()); }
        let source = read_bounded(&path, 4 * 1024 * 1024)?;
        // Freeze exactly the admitted bytes for native conversion (source may change).
        use std::io::Write;
        let mut admitted = tempfile::NamedTempFile::new().map_err(|e| e.to_string())?;
        admitted.write_all(&source).map_err(|e| e.to_string())?;
        let id = Path::new(asset).file_stem().and_then(|s| s.to_str()).ok_or("media id")?.to_string();
        match kind {
            MediaKind::Wav => {
                let bytes = source;
                let pcm = match &ffmpeg { Some(f) => ffmpeg_wav_to_pcm(f, admitted.path()).or_else(|_| pcm_wav_to_16k_mono(&bytes))?, None => pcm_wav_to_16k_mono(&bytes)? };
                let encoded = encode_sample(&pcm)?;
                sample_total += encoded.len();
                if sample_total > SAMPLE_BUDGET + 4 * 8 { return Err("采样合计超过 256 KiB 预算".into()); }
                out.push((format!("runtime/samples/{id}.pcm"), encoded));
            }
            MediaKind::Mp4 => {
                let node = w["surface"]["elements"].as_array().into_iter().flatten().find(|n| n["type"] == "clip" && n["asset"] == asset.as_str()).ok_or("clip node")?;
                let (width, height) = (node["w"].as_u64().unwrap_or(0) as u16, node["h"].as_u64().unwrap_or(0) as u16);
                let ffmpeg = ffmpeg.as_deref().ok_or("短视频需要内置 FFmpeg 运行时（含系统 H.264 编码器）")?;
                let stream = ffmpeg_clip(ffmpeg, admitted.path(), width, height)?;
                let frames = h264_frame_count(&stream);
                out.push((format!("runtime/clips/{id}.h264"), encode_clip(width, height, u16::try_from(frames).map_err(|_| "clip frames")?, &stream)?));
            }
            MediaKind::Png => {
                let declared = w["surface"]["big_images"].as_array().into_iter().flatten().find(|b| b["asset"] == asset.as_str()).ok_or("big image")?;
                if source.len() < 24 || &source[..8] != b"\x89PNG\r\n\x1a\n" { return Err("invalid big PNG".into()); }
                let width = u32::from_be_bytes(source[16..20].try_into().unwrap());
                let height = u32::from_be_bytes(source[20..24].try_into().unwrap());
                if !(241..=1280).contains(&width) || !(1..=960).contains(&height) || big_tiles(width.into(), height.into()) > BIG_TILES.into() { return Err("big PNG dimensions exceed budget".into()); }
                let mut reader = image::ImageReader::with_format(std::io::Cursor::new(&source), image::ImageFormat::Png);
                let mut limits = image::Limits::default();
                limits.max_image_width = Some(1280); limits.max_image_height = Some(960); limits.max_alloc = Some(16 * 1024 * 1024);
                reader.limits(limits);
                let image = reader.decode().map_err(|e| e.to_string())?.to_rgba8();
                if (u64::from(image.width()), u64::from(image.height())) != (declared["width"].as_u64().unwrap_or(0), declared["height"].as_u64().unwrap_or(0)) { return Err(format!("大图 {asset} 的尺寸与声明不一致")); }
                out.push((format!("runtime/big/{id}.p4t"), encode_big_tiles(&image)?));
            }
        }
    }
    let big_total: usize = out.iter().filter(|(p, _)| p.starts_with("runtime/big/")).map(|(_, b)| b.len()).sum();
    if big_total > BIG_BUDGET { return Err("大图合计超过 1 MiB 预算".into()); }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn suite(name: &str) -> Value { serde_json::from_slice(&std::fs::read(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(format!("../skills/petui/contracts/{name}.json"))).unwrap()).unwrap() }
    #[test]
    fn contract_vectors_match_python_and_firmware() {
        for name in ["sequence-v1", "samples-v1", "clip-v1", "big-image-v1", "events-v1"] {
            let s = suite(name);
            for item in s["vectors"].as_array().unwrap() {
                let mut errors = Vec::new();
                validate_runtime(&item["runtime"], &mut errors);
                let stripped = strip(&item["runtime"]);
                if errors.is_empty() && stripped["surface"]["engine"] == "p4-app-surface-v1" { if let Err(e) = crate::component_extension::validate_app_surface(&stripped["surface"], Some(&stripped)) { errors.push(e); } }
                assert_eq!(errors.is_empty(), item["ok"] == true, "{name}: {}: {:?}", item["name"], errors);
            }
        }
    }
    #[test]
    fn strip_leaves_a_base_declaration_and_capabilities_gate_install() {
        let r = suite("events-v1")["vectors"][1]["runtime"].clone();
        let s = strip(&r);
        assert!(s.get("events").is_none() && s.get("alarms").is_none() && s["transitions"][0].get("emit").is_none());
        let clip = suite("clip-v1")["vectors"][0]["runtime"].clone();
        assert_eq!(strip(&clip)["surface"]["elements"][1]["type"], "panel");
        assert_eq!(media_assets(&clip), vec![("assets/intro.mp4".to_string(), MediaKind::Mp4)]);
        let caps = json!({"widgetSfx": "p4-sfx-v1"});
        assert!(require_capability(&clip, &caps).unwrap_err().contains("p4-clip-v1"));
        assert!(require_capability(&r, &caps).unwrap_err().contains("p4-events-v1"));
        let full = json!({"widgetSequence": SEQUENCE_ENGINE, "widgetSamples": SAMPLES_ENGINE, "widgetClip": CLIP_ENGINE, "widgetBigImage": BIG_IMAGE_ENGINE, "widgetEvents": EVENTS_ENGINE});
        for name in ["sequence-v1", "samples-v1", "clip-v1", "big-image-v1", "events-v1"] { assert!(require_capability(&suite(name)["vectors"][0]["runtime"], &full).is_ok()); }
        assert!(require_capability(&json!({"engine": "p4-bounded-runtime-v4"}), &caps).is_ok(), "packages without the extensions never need them");
        assert!(event_action("event:done") && !event_action("event:a:b") && !event_action("pet"));
    }
    #[test]
    fn bounded_media_reads_reject_one_byte_over_limit() {
        use std::io::Write;
        let mut file = tempfile::NamedTempFile::new().unwrap();
        file.write_all(b"12345").unwrap();
        assert_eq!(read_bounded(file.path(), 5).unwrap(), b"12345");
        assert!(read_bounded(file.path(), 4).is_err());
        let mut truncated = wav(16000, 1, 16, 8);
        truncated.pop();
        assert!(pcm_wav_to_16k_mono(&truncated).is_err());
        let mut bad_alignment = wav(16000,1,16,8); bad_alignment[32] = 0;
        assert!(pcm_wav_to_16k_mono(&bad_alignment).is_err());
        let mut missing_extensible_header = wav(16000,1,16,8);
        missing_extensible_header[20..22].copy_from_slice(&0xfffeu16.to_le_bytes());
        assert!(pcm_wav_to_16k_mono(&missing_extensible_header).is_err());
    }

    // Invoked only by the bounded-process test in a separate host process.
    #[test]
    #[ignore]
    fn media_process_child() {
        use std::io::Write;
        match std::env::var("PET_CEXT_TEST_CHILD").as_deref() {
            Ok("overflow") => { let _ = std::io::stdout().write_all(&vec![b'x'; 65536]); }
            Ok("stderr") => { let _ = std::io::stderr().write_all(&vec![b'x'; 65536]); print!("done"); }
            Ok("timeout") => std::thread::sleep(std::time::Duration::from_secs(10)),
            Ok("failure") => std::process::exit(7),
            _ => return,
        }
    }
    #[test]
    fn native_process_output_timeout_and_failure_are_bounded() {
        fn command(mode: &str) -> std::process::Command {
            let mut c = std::process::Command::new(std::env::current_exe().unwrap());
            c.args(["--exact", "component_capability_ext::tests::media_process_child", "--ignored", "--nocapture", "--quiet"])
                .env("PET_CEXT_TEST_CHILD", mode); c
        }
        let timeout = std::time::Duration::from_secs(3);
        assert!(run_media_process(&mut command("overflow"), 2048, timeout).unwrap_err().contains("budget"));
        assert!(run_media_process(&mut command("failure"), 2048, timeout).is_err());
        let bytes = run_media_process(&mut command("stderr"), 2048, timeout).unwrap();
        assert!(String::from_utf8_lossy(&bytes).contains("done"));
        let start = std::time::Instant::now();
        assert!(run_media_process(&mut command("timeout"), 2048, std::time::Duration::from_millis(100)).unwrap_err().contains("timeout"));
        assert!(start.elapsed() < std::time::Duration::from_secs(3));
    }

    #[test]
    fn internal_action_scope_vectors_validate_complete_packages() {
        let suite = suite("internal-actions-v1");
        for vector in suite["vectors"].as_array().unwrap() {
            let runtime = &vector["runtime"];
            let expected: HashSet<String> = vector["internal"].as_array().unwrap().iter().map(|v|v.as_str().unwrap().to_string()).collect();
            assert_eq!(internal_actions(runtime), expected, "{}", vector["name"]);
            let dir = tempfile::tempdir().unwrap();
            for sub in ["runtime", "assets"] { std::fs::create_dir(dir.path().join(sub)).unwrap(); }
            let declarations = [
                ("component.json",json!({"id":"internal-scope-test","name":"Scope","version":"0.0.1","kind":"tool","description":"Private scoped internal action package regression."})),
                ("negative-screen.json",json!({"dashboard":{"title":runtime["dashboard"]["title"],"visualStyle":"clean","visualPalette":"mono","visualLayout":"tool","visualSprite":"timer"}})),
                ("buttons.json",vector["buttons"].clone()),
                ("runtime/widget.json",runtime.clone()),
                ("share.json",json!({"title":"Scope"})),
            ];
            for (path,value) in declarations { std::fs::write(dir.path().join(path),serde_json::to_vec(&value).unwrap()).unwrap(); }
            std::fs::write(dir.path().join("assets/.keep"),b"").unwrap();
            let result = crate::clawpkg::validate_clawpkg_at_path(dir.path()).unwrap();
            assert_eq!(result.ok, vector["ok"] == true, "{}: {:?}",vector["name"],result.errors);
        }
    }

    #[test]
    fn full_host_packages_admit_referenced_samples_and_clips_only() {
        // Exercise the real filesystem collection + manifest path, including its
        // host whitelist. No transcode, device, FFmpeg invocation or install.
        for (sample, clip) in [(true, false), (false, true), (true, true)] {
            let dir = tempfile::tempdir().unwrap();
            for sub in ["runtime", "assets", "pc"] { std::fs::create_dir(dir.path().join(sub)).unwrap(); }
            let mut transition = json!({"from":"*","on":"host.next"});
            if sample { transition["sample"] = json!("ding"); }
            let mut runtime = json!({"schema_version":1,"engine":"p4-bounded-runtime-v4",
                "host":true,"states":["idle"],"initial_state":"idle","vars":{},
                "transitions":[transition],"tick":[],"dashboard":{"title":"Media","visualStyle":"clean","visualPalette":"mono","visualLayout":"tool","visualSprite":"music"},
                "surface":{"engine":"p4-app-surface-v1","graphics":"p4-app-graphics-v1","bindings":"p4-variable-bindings-v1","background":0,
                    "elements":[{"type":"panel","x":0,"y":0,"w":640,"h":480,"color":0},
                        {"type":"button","x":20,"y":400,"w":160,"h":48,"color":1234,"text":"Next","font_size":17,"action":"host.next"}]}});
            if sample {
                runtime["samples"] = json!({"engine":SAMPLES_ENGINE,"volume":80,"items":[{"id":"ding","asset":"assets/ding.wav"}]});
                std::fs::write(dir.path().join("assets/ding.wav"), wav(16000,1,16,160)).unwrap();
            }
            if clip {
                runtime["surface"]["elements"].as_array_mut().unwrap().push(json!({"type":"clip","x":20,"y":20,"w":160,"h":120,"color":0,"asset":"assets/intro.mp4"}));
                // Genuine one-frame, 160x120 H.264/MP4 fixture (black, 15 fps).
                std::fs::write(dir.path().join("assets/intro.mp4"), include_bytes!("component_capability_ext/fixtures/one-frame.mp4")).unwrap();
            }
            let declarations = [
                ("component.json",json!({"id":"media-host-test","name":"Media","version":"1.0.0","kind":"tool","description":"Private complete package regression for declared samples and clips."})),
                ("negative-screen.json",json!({"dashboard":{"title":"Media","visualStyle":"clean","visualPalette":"mono","visualLayout":"tool","visualSprite":"music"}})),
                ("buttons.json",json!([{"action":"host.next","control":"SW1","event":"button.sw1.short_press","label":"Next"}])),
                ("share.json",json!({"title":"Media"})),
                ("pc/application.json",json!({"schemaVersion":1,"engine":"pc-app-services-v1","service":"notes","actions":{"host.next":"next"}})),
                ("runtime/widget.json",runtime),
            ];
            for (path, value) in declarations { std::fs::write(dir.path().join(path),serde_json::to_vec(&value).unwrap()).unwrap(); }
            let accepted = crate::clawpkg::validate_clawpkg_at_path(dir.path()).unwrap();
            assert!(accepted.ok,"sample={sample} clip={clip}: {:?}",accepted.errors);
            // A valid extra WAV must still fail: retaining the source declaration
            // must not turn the media whitelist into an arbitrary asset exemption.
            std::fs::write(dir.path().join("assets/unreferenced.wav"),wav(16000,1,16,160)).unwrap();
            let rejected = crate::clawpkg::validate_clawpkg_at_path(dir.path()).unwrap();
            assert!(!rejected.ok,"unreferenced audio admitted");
            assert!(rejected.errors.iter().any(|e|e.contains("PC 应用包含额外文件")),"{:?}",rejected.errors);
        }
    }

    fn wav(rate: u32, channels: u16, bits: u16, frames: usize) -> Vec<u8> {
        let frame = usize::from(channels) * usize::from(bits / 8);
        let mut data = Vec::new();
        for i in 0..frames { for _ in 0..channels { let v = if i % 20 < 10 { 0.5 } else { -0.5 }; match bits { 8 => data.push((v * 127.0 + 128.0) as u8), 16 => data.extend(((v * 32767.0) as i16).to_le_bytes()), 24 => data.extend(&((v * 8_388_607.0) as i32).to_le_bytes()[..3]), _ => data.extend(((v * 2_147_483_647.0) as i32).to_le_bytes()) } } }
        let mut out = b"RIFF".to_vec(); out.extend((36 + data.len() as u32).to_le_bytes()); out.extend(b"WAVEfmt "); out.extend(16u32.to_le_bytes());
        out.extend(1u16.to_le_bytes()); out.extend(channels.to_le_bytes()); out.extend(rate.to_le_bytes()); out.extend((rate * frame as u32).to_le_bytes()); out.extend((frame as u16).to_le_bytes()); out.extend(bits.to_le_bytes());
        out.extend(b"data"); out.extend((data.len() as u32).to_le_bytes()); out.extend(data); out
    }
    #[test]
    fn samples_normalize_to_16k_mono_and_fit_the_budget() {
        let pcm = pcm_wav_to_16k_mono(&wav(48000, 2, 16, 48000)).unwrap();
        assert_eq!(pcm.len(), 16000, "one second at 48 kHz stereo becomes 16000 mono frames");
        assert!(pcm.iter().any(|s| *s > 15000) && pcm.iter().any(|s| *s < -15000));
        for bits in [8u16, 24, 32] { assert_eq!(pcm_wav_to_16k_mono(&wav(16000, 1, bits, 100)).unwrap().len(), 100, "{bits} bit"); }
        let encoded = encode_sample(&pcm).unwrap();
        assert_eq!(&encoded[..4], b"P4A1"); assert_eq!(u32::from_le_bytes(encoded[4..8].try_into().unwrap()), 16000); assert_eq!(encoded.len(), 8 + 32000);
        assert!(pcm_wav_to_16k_mono(&wav(16000, 1, 16, 32769)).unwrap_err().contains("64 KiB"), "2.048 s is the ceiling, never trimmed");
        assert!(pcm_wav_to_16k_mono(b"ID3\x03not a wav at all").is_err());
        assert!(encode_sample(&[]).is_err());
    }
    #[test]
    fn clip_container_and_frame_count() {
        // Two IDR + two non-IDR slices with first_mb_in_slice = 0 (leading exp-Golomb bit set), one SPS, one continuation slice.
        let stream = [0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x65, 0x88, 0x80, 0, 0, 1, 0x41, 0x9a, 0, 0, 1, 0x41, 0x1a, 0, 0, 0, 1, 0x65, 0xb8];
        assert_eq!(h264_frame_count(&stream), 3);
        let clip = encode_clip(320, 240, 3, &stream).unwrap();
        assert_eq!(&clip[..4], b"P4V1"); assert_eq!(clip.len(), CLIP_HEADER + stream.len()); assert_eq!(clip[8], 15); assert_eq!(u16::from_le_bytes([clip[10], clip[11]]), 3);
        assert!(encode_clip(640, 480, 3, &stream).is_err() && encode_clip(320, 240, 76, &stream).is_err() && encode_clip(320, 240, 1, &vec![0; CLIP_BYTES]).is_err());
    }
    #[test]
    fn big_image_tiles_follow_the_firmware_header_and_budget() {
        let mut image = image::RgbaImage::from_pixel(960, 720, image::Rgba([40, 90, 200, 255]));
        for x in 0..960 { image.put_pixel(x, 719, image::Rgba([255, 255, 255, 255])); }
        let packed = encode_big_tiles(&image).unwrap();
        assert_eq!(&packed[..4], b"P4T1"); assert_eq!((packed[4], packed[5]), (4, 3)); assert_eq!(packed[14], 12);
        assert_eq!(u16::from_le_bytes([packed[6], packed[7]]), 240); assert_eq!(u16::from_le_bytes([packed[10], packed[11]]), 960);
        let first = u32::from_le_bytes(packed[BIG_HEADER..BIG_HEADER + 4].try_into().unwrap()) as usize;
        assert_eq!(first, BIG_HEADER + 12 * 8); assert_eq!(&packed[first..first + 4], b"P4I1");
        assert!(packed.len() < BIG_BUDGET);
        assert_eq!(big_tiles(1280, 480), 12); assert_eq!(big_tiles(1280, 960), 24);
        assert!(encode_big_tiles(&image::RgbaImage::from_pixel(1280, 960, image::Rgba([0, 0, 0, 255]))).is_err());
        assert!(encode_big_tiles(&image::RgbaImage::from_pixel(240, 240, image::Rgba([0, 0, 0, 255]))).is_err(), "a plain surface image is not a big image");
        let banner = encode_big_tiles(&image::RgbaImage::from_pixel(1280, 480, image::Rgba([0, 0, 0, 255]))).unwrap();
        assert_eq!((banner[4], banner[5]), (6, 2)); assert_eq!(u16::from_le_bytes([banner[6], banner[7]]), 214);
    }
}
