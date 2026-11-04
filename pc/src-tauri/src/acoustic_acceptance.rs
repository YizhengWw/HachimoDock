//! Opt-in physical acoustic acceptance using production USB RX/TX. No cloud,
//! credentials, ASR, application data or recorded PCM files are used.
use crate::usb_serial::UsbSerialManager;
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde_json::{json, Value};
use std::{
    path::{Path, PathBuf},
    process::{Child, Command},
    sync::mpsc,
    time::{Duration, Instant},
};
type Event = (String, Value, Instant);
static REJECTED: std::sync::Mutex<Vec<Value>> = std::sync::Mutex::new(Vec::new());
pub(crate) fn observe_rejected_line(line: &str) {
    if std::env::var("PET_ACOUSTIC_RX_DIAGNOSTICS").as_deref() != Ok("1")
        || !line.contains("\"audio/chunk\"")
    {
        return;
    }
    let trimmed: Option<Value> = serde_json::from_str(line.trim_matches('\0').trim()).ok();
    let embedded = line
        .find('{')
        .zip(line.rfind('}'))
        .filter(|(a, b)| a <= b)
        .and_then(|(a, b)| serde_json::from_str::<Value>(&line[a..=b]).ok());
    let detail = json!({"bytes":line.len(),"nulCount":line.bytes().filter(|&b|b==0).count(),"startsJson":line.starts_with('{'),"firstJsonOffset":line.find('{'),"lastJsonOffset":line.rfind('}'),"trimNulRecoverable":trimmed.as_ref().is_some_and(|v|v["topic"]=="audio/chunk"),"embeddedRecoverable":embedded.as_ref().is_some_and(|v|v["topic"]=="audio/chunk"),"recoverableSequence":trimmed.or(embedded).and_then(|v|v["payload"]["seq"].as_u64())});
    if let Ok(mut list) = REJECTED.lock() {
        if list.len() < 48 {
            list.push(detail);
        }
    }
}
fn fnv(mut hash: u64, bytes: &[u8]) -> u64 {
    for &b in bytes {
        hash = (hash ^ u64::from(b)).wrapping_mul(0x100000001b3);
    }
    hash
}
fn wav(path: &Path, peak: f64) -> Vec<u8> {
    let data = std::fs::read(path).unwrap();
    assert_eq!(&data[..4], b"RIFF");
    assert_eq!(&data[8..12], b"WAVE");
    let (mut pos, mut pcm, mut format) = (12, None, false);
    while pos + 8 <= data.len() {
        let size = u32::from_le_bytes(data[pos + 4..pos + 8].try_into().unwrap()) as usize;
        assert!(pos + 8 + size <= data.len());
        let v = &data[pos + 8..pos + 8 + size];
        if &data[pos..pos + 4] == b"fmt " {
            assert!(v.len() >= 16);
            assert_eq!(u16::from_le_bytes(v[..2].try_into().unwrap()), 1);
            assert_eq!(u16::from_le_bytes(v[2..4].try_into().unwrap()), 1);
            assert_eq!(u32::from_le_bytes(v[4..8].try_into().unwrap()), 16000);
            assert_eq!(u16::from_le_bytes(v[14..16].try_into().unwrap()), 16);
            format = true;
        }
        if &data[pos..pos + 4] == b"data" {
            pcm = Some(v.to_vec());
        }
        pos += 8 + size + (size % 2);
    }
    assert!(format);
    let pcm = pcm.unwrap();
    let mut samples: Vec<f64> = pcm
        .chunks_exact(2)
        .map(|s| i16::from_le_bytes([s[0], s[1]]) as f64)
        .take(64000)
        .collect();
    samples.resize(64000, 0.);
    let max = samples.iter().map(|x| x.abs()).fold(1., f64::max);
    let mut result = Vec::with_capacity(128000);
    for (i, s) in samples.iter().enumerate() {
        let ramp = (i as f64 / 320.).min((63999 - i) as f64 / 320.).min(1.);
        result.extend_from_slice(&((*s * peak / max * ramp) as i16).to_le_bytes());
    }
    result
}
#[derive(Default)]
struct Col {
    begin: Option<Value>,
    end: Option<Value>,
    frames: Vec<(Instant, f64, u32, u32, bool, bool)>,
    hash: u64,
    bad: u32,
    bad_details: Vec<Value>,
    next: u64,
    audio: Value,
    events: Vec<Value>,
    play_start: Option<Instant>,
    play_end: Option<Instant>,
}
impl Col {
    fn new() -> Self {
        Self {
            hash: 0xcbf29ce484222325,
            ..Self::default()
        }
    }
    fn observe(&mut self, (topic, p, t): Event, play: &str) {
        match topic.as_str() {
            "audio/begin" => self.begin = Some(p),
            "audio/end" => self.end = Some(p),
            "audio/chunk" => {
                let bytes = B64.decode(p["data"].as_str().unwrap()).unwrap();
                let seq = p["seq"].as_u64().unwrap();
                let sum_ok = p["checksum"] == format!("{:016x}", fnv(0xcbf29ce484222325, &bytes));
                if seq != self.next || bytes.len() != 640 || !sum_ok {
                    self.bad += 1;
                    self.bad_details.push(json!({"expectedSeq":self.next,"receivedSeq":seq,"bytes":bytes.len(),"checksumOk":sum_ok}));
                }
                self.next = seq + 1;
                self.hash = fnv(self.hash, &bytes);
                let mut squares = 0.;
                let (mut peak, mut clipped) = (0, 0);
                for x in bytes.chunks_exact(2) {
                    let v = i16::from_le_bytes([x[0], x[1]]) as i32;
                    squares += (v as f64).powi(2);
                    peak = peak.max(v.unsigned_abs());
                    if v.unsigned_abs() >= 32760 {
                        clipped += 1;
                    }
                }
                self.frames.push((
                    t,
                    squares / 320.,
                    peak,
                    clipped,
                    p["aec"] == true,
                    p["vadSpeech"] == true,
                ));
            }
            "audio/status" => self.audio = p,
            "audio/diagnostic" => {
                if p["sessionId"] == play {
                    if p["event"] == "playback_started" {
                        self.play_start = Some(t);
                    }
                    if p["event"] == "playback_completed" {
                        self.play_end = Some(t);
                    }
                }
                self.events.push(p);
            }
            "audio/error" => {
                self.bad += 1;
            }
            _ => {}
        }
    }
    fn stats(&self, from: Instant, to: Instant) -> Value {
        let a: Vec<_> = self
            .frames
            .iter()
            .filter(|x| x.0 >= from && x.0 <= to)
            .collect();
        let rms = (a.iter().map(|x| x.1).sum::<f64>() / a.len().max(1) as f64).sqrt();
        json!({"frames":a.len(),"rms":rms,"peak":a.iter().map(|x|x.2).max().unwrap_or(0),"clipped":a.iter().map(|x|x.3).sum::<u32>(),"aecFrames":a.iter().filter(|x|x.4).count(),"vadSpeechFrames":a.iter().filter(|x|x.5).count()})
    }
}
struct Cleanup(UsbSerialManager, String);
impl Drop for Cleanup {
    fn drop(&mut self) {
        let _ = self
            .0
            .send_to_board(&self.1, "audio/conversation", &json!({"enabled":false}));
        let _ = self
            .0
            .send_to_board(&self.1, "audio/control", &json!({"action":"stop"}));
        let _ = self
            .0
            .send_to_board(&self.1, "audio/play_flush", &json!({}));
        std::thread::sleep(Duration::from_millis(150));
        self.0.disconnect();
    }
}
fn pump(rx: &mpsc::Receiver<Event>, c: &mut Col, play: &str, duration: Duration) {
    let end = Instant::now() + duration;
    while Instant::now() < end {
        if let Ok(e) = rx.recv_timeout(Duration::from_millis(5)) {
            c.observe(e, play);
        }
    }
}
fn round(
    usb: &UsbSerialManager,
    board: &str,
    rx: &mpsc::Receiver<Event>,
    name: &str,
    full: bool,
    far: Option<&[u8]>,
    near: Option<&Path>,
    out: &Path,
) -> Value {
    while rx.try_recv().is_ok() {}
    let id = format!("local-acoustic-{name}-{}", std::process::id());
    let play = format!("{id}-play");
    let began = Instant::now();
    let mut c = Col::new();
    eprintln!("ACOUSTIC ROUND {name}");
    if full {
        usb.send_to_board(
            board,
            "audio/conversation",
            &json!({"enabled":true,"halfDuplex":false,"sessionId":id}),
        )
        .unwrap();
    } else {
        usb.send_to_board(
            board,
            "audio/control",
            &json!({"action":"start","captureOwner":id}),
        )
        .unwrap();
    }
    let deadline = Instant::now() + Duration::from_secs(3);
    while c.begin.is_none()
        || (full && (c.audio["audioAec"] != true || c.audio["audioAecPreparing"] == true))
    {
        assert!(
            Instant::now() < deadline,
            "capture/DSP preparation deadline"
        );
        pump(rx, &mut c, &play, Duration::from_millis(30));
        usb.send_to_board(board, "audio/query", &json!({})).unwrap();
    }
    let near_volume = std::env::var("PET_ACOUSTIC_NEAR_VOLUME").unwrap_or_else(|_| "0.25".into());
    let volume: f64 = near_volume.parse().unwrap();
    assert!(volume > 0. && volume <= 1.);
    pump(rx, &mut c, &play, Duration::from_millis(800));
    let played = Instant::now();
    let mut mac: Option<Child> = near.map(|path| {
        Command::new("/usr/bin/afplay")
            .args(["-v", &near_volume])
            .arg(path)
            .spawn()
            .unwrap()
    });
    if far.is_some() {
        usb.send_to_board(board, "audio/play_begin", &json!({"sessionId":play}))
            .unwrap();
    }
    let (mut sent, mut ended) = (0, false);
    let mut writes = Vec::new();
    while played.elapsed() < Duration::from_millis(4700) || (far.is_some() && c.play_end.is_none())
    {
        assert!(
            played.elapsed() < Duration::from_secs(8),
            "playback completion deadline"
        );
        if let Some(pcm) = far {
            if sent < pcm.len()
                && (sent as u64 * 1000 / 32000) <= played.elapsed().as_millis() as u64 + 1200
            {
                let end = (sent + 3200).min(pcm.len());
                let at = Instant::now();
                usb.send_to_board(board,"audio/play_chunk",&json!({"sessionId":play,"seq":sent/3200,"bytes":end-sent,"data":B64.encode(&pcm[sent..end])})).unwrap();
                writes.push(at.elapsed().as_micros() as u64);
                sent = end;
            }
            if sent == pcm.len() && !ended {
                usb.send_to_board(board, "audio/play_end", &json!({"sessionId":play}))
                    .unwrap();
                ended = true;
            }
        }
        pump(rx, &mut c, &play, Duration::from_millis(5));
    }
    if let Some(p) = mac.as_mut() {
        assert!(p.wait().unwrap().success());
    }
    usb.send_to_board(board, "audio/query", &json!({})).unwrap();
    pump(rx, &mut c, &play, Duration::from_millis(80));
    if full {
        usb.send_to_board(
            board,
            "audio/conversation",
            &json!({"enabled":false,"sessionId":id}),
        )
        .unwrap();
    } else {
        usb.send_to_board(
            board,
            "audio/control",
            &json!({"action":"stop","captureOwner":id}),
        )
        .unwrap();
    }
    let deadline = Instant::now() + Duration::from_secs(2);
    while c.end.is_none() {
        assert!(Instant::now() < deadline);
        pump(rx, &mut c, &play, Duration::from_millis(20));
    }
    let all = c.stats(began, Instant::now());
    let window = if near.is_some() {
        c.stats(
            played + Duration::from_millis(300),
            played + Duration::from_millis(3800),
        )
    } else if let (Some(start), Some(end)) = (c.play_start, c.play_end) {
        c.stats(
            start + Duration::from_millis(800),
            end - Duration::from_millis(200),
        )
    } else {
        all.clone()
    };
    let failures: Vec<_> = c
        .events
        .iter()
        .filter(|p| p["ok"] == false)
        .cloned()
        .collect();
    let integrity = c.bad == 0
        && c.end.as_ref().unwrap()["chunks"] == c.frames.len()
        && c.end.as_ref().unwrap()["checksum"] == format!("{:016x}", c.hash);
    let valid = integrity
        && failures.is_empty()
        && (!full || (all["aecFrames"] == c.frames.len() && c.audio["aecReason"] == "none"));
    let result = json!({"name":name,"accepted":valid,"integrityOk":integrity,"all":all,"window":window,"badFrames":c.bad,"badDetails":c.bad_details,"end":c.end,"audio":c.audio,"events":c.events,"failedDiagnostics":failures,"writeUs":writes,"recordingsSaved":false,"cloudCalls":0});
    std::fs::write(
        out.join(format!("native-{name}.json")),
        serde_json::to_vec_pretty(&result).unwrap(),
    )
    .unwrap();
    eprintln!("ACOUSTIC RESULT {name} {}", result["window"]);
    assert!(c.frames.len() > 200);
    assert_eq!(all["clipped"], 0);
    result
}
#[test]
#[ignore = "Explicit local physical playback/capture; synthetic WAV only, no credentials/cloud or recorded PCM files"]
fn local_acoustic_production_transport() {
    assert_ne!(
        std::env::var("P4_SERIAL_LOG_RAW").as_deref(),
        Ok("1"),
        "raw PCM logging forbidden"
    );
    let out = PathBuf::from(std::env::var("PET_ACOUSTIC_DIR").unwrap());
    let near = out.join("nearend-synthetic.wav");
    let far = wav(&out.join("farend-synthetic.wav"), 1800.);
    let usb = UsbSerialManager::new();
    let (tx, rx) = mpsc::channel();
    usb.connect(
        &std::env::var("P4_SERIAL_PORT").unwrap(),
        move |topic, p| {
            let _ = tx.send((topic, p, Instant::now()));
        },
    )
    .unwrap();
    let board = usb.status().board_device_id;
    assert_eq!(board, std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
    usb.send_to_board(&board, "audio/query", &json!({}))
        .unwrap();
    let mut pre = Col::new();
    pump(&rx, &mut pre, "", Duration::from_millis(200));
    assert_eq!(pre.audio["active"], false);
    assert_eq!(pre.audio["playbackActive"], false);
    assert_eq!(pre.audio["conversation"], false);
    let _cleanup = Cleanup(usb.clone(), board.clone());
    let raw = round(&usb, &board, &rx, "raw-far", false, Some(&far), None, &out);
    let aec = round(&usb, &board, &rx, "aec-far", true, Some(&far), None, &out);
    let reduction = 20.
        * (raw["window"]["rms"].as_f64().unwrap()
            / aec["window"]["rms"].as_f64().unwrap().max(1e-12))
        .log10()
        - 6.;
    let raw_near = round(
        &usb,
        &board,
        &rx,
        "raw-near",
        false,
        None,
        Some(&near),
        &out,
    );
    let near_only = round(&usb, &board, &rx, "aec-near", true, None, Some(&near), &out);
    let double = round(
        &usb,
        &board,
        &rx,
        "double-talk",
        true,
        Some(&far),
        Some(&near),
        &out,
    );
    let accepted = [&raw, &aec, &raw_near, &near_only, &double]
        .iter()
        .all(|v| v["accepted"] == true);
    let near_vad_ok = near_only["window"]["vadSpeechFrames"].as_u64().unwrap() > 10;
    let double_vad_ok = double["window"]["vadSpeechFrames"].as_u64().unwrap() > 10;
    let double_signal_ok =
        double["window"]["rms"].as_f64().unwrap() > aec["window"]["rms"].as_f64().unwrap() * 3.;
    let summary = json!({"overallPassed":accepted&&reduction>10.&&near_vad_ok&&double_vad_ok&&double_signal_ok,"transportAndDspPassed":accepted,"nearVadPassed":near_vad_ok,"doubleTalkVadPassed":double_vad_ok,"doubleTalkSignalPassed":double_signal_ok,"gainAdjustedPipelineReductionDb":reduction,"notPureAecErle":true,"includesNoiseSuppressionAndGain":true,"rawNear":raw_near["window"],"aecNear":near_only["window"],"doubleTalk":double["window"],"rejectedLineMetadata":*REJECTED.lock().unwrap(),"cloudCalls":0,"recordingsSaved":false});
    std::fs::write(
        out.join("native-summary.json"),
        serde_json::to_vec_pretty(&summary).unwrap(),
    )
    .unwrap();
    assert!(
        accepted,
        "frame integrity or device diagnostics failed; see metadata-only summary"
    );
    assert!(
        reduction > 10.,
        "gain-adjusted pipeline echo reduction insufficient: {reduction}"
    );
    assert!(near_only["window"]["vadSpeechFrames"].as_u64().unwrap() > 10);
    assert!(double["window"]["vadSpeechFrames"].as_u64().unwrap() > 10);
    assert!(
        double["window"]["rms"].as_f64().unwrap() > aec["window"]["rms"].as_f64().unwrap() * 3.
    );
    eprintln!("ACOUSTIC SUMMARY {summary}");
}

#[test]
#[ignore = "Requires a person speaking near the device; explicit local capture/playback, no recorded PCM files"]
fn local_acoustic_human_double_talk() {
    assert_eq!(
        std::env::var("PET_ACOUSTIC_HUMAN_READY").as_deref(),
        Ok("1")
    );
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(), Ok("1"));
    let out = PathBuf::from(std::env::var("PET_ACOUSTIC_DIR").unwrap());
    let far = wav(&out.join("farend-synthetic.wav"), 1800.);
    let usb = UsbSerialManager::new();
    let (tx, rx) = mpsc::channel();
    usb.connect(
        &std::env::var("P4_SERIAL_PORT").unwrap(),
        move |topic, p| {
            let _ = tx.send((topic, p, Instant::now()));
        },
    )
    .unwrap();
    let board = usb.status().board_device_id;
    assert_eq!(board, std::env::var("P4_EXPECTED_BOARD_ID").unwrap());
    usb.send_to_board(&board, "audio/query", &json!({}))
        .unwrap();
    let mut pre = Col::new();
    pump(&rx, &mut pre, "", Duration::from_millis(200));
    assert_eq!(pre.audio["active"], false);
    assert_eq!(pre.audio["playbackActive"], false);
    assert_eq!(pre.audio["conversation"], false);
    let _cleanup = Cleanup(usb.clone(), board.clone());
    let raw = round(&usb, &board, &rx, "human-raw", false, None, None, &out);
    let near = round(&usb, &board, &rx, "human-near", true, None, None, &out);
    let double = round(
        &usb,
        &board,
        &rx,
        "human-double-talk",
        true,
        Some(&far),
        None,
        &out,
    );
    let transport = [&raw, &near, &double].iter().all(|v| v["accepted"] == true);
    let near_vad = near["window"]["vadSpeechFrames"].as_u64().unwrap() > 10;
    let double_vad = double["window"]["vadSpeechFrames"].as_u64().unwrap() > 10;
    let result = json!({"overallPassed":transport&&near_vad&&double_vad,"transportAndDspPassed":transport,"nearVadPassed":near_vad,"doubleTalkVadPassed":double_vad,"humanSpeech":true,"raw":raw["window"],"near":near["window"],"doubleTalk":double["window"],"rejectedLineMetadata":*REJECTED.lock().unwrap(),"cloudCalls":0,"recordingsSaved":false});
    std::fs::write(
        out.join("native-human-summary.json"),
        serde_json::to_vec_pretty(&result).unwrap(),
    )
    .unwrap();
    assert!(transport, "capture integrity or device diagnostic failed");
    assert!(near_vad, "near-end human speech not detected");
    assert!(
        double_vad,
        "human speech not detected during far-end playback"
    );
    eprintln!("ACOUSTIC HUMAN SUMMARY {result}");
}
