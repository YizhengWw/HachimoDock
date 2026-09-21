/*
 * [Input] P4 voice_ptt input events when the board has no microphone codec.
 * [Output] Windows default-input PCM normalized to 16 kHz mono S16LE for UDP
 *          relay and bounded local zh-CN speech recognition with retryable
 *          capability-probe caching.
 * [Pos] Tauri-side capture/STT fallback for P4 boards without ES7210 hardware.
 */

use serde_json::{json, Value};

pub const TARGET_SAMPLE_RATE: u32 = 16_000;
pub const TARGET_CHANNELS: u16 = 1;
pub const TARGET_BITS_PER_SAMPLE: u16 = 16;
pub const FRAME_SAMPLES: usize = 320;
pub const FRAME_BYTES: usize = FRAME_SAMPLES * 2;
const MAX_CAPTURE_DURATION_MS: u128 = 30_000;
const MAX_CAPTURE_BYTES: usize = TARGET_SAMPLE_RATE as usize * 2 * 30;

pub struct PcAudioCaptureResult {
    pub event: Value,
    pub pcm: Vec<u8>,
    pub capture_id: u64,
}

#[derive(Debug, Default)]
struct PcmPacketizer {
    source_rate: u32,
    phase: u64,
    packet: Vec<u8>,
}

impl PcmPacketizer {
    fn new(source_rate: u32) -> Self {
        Self {
            source_rate: source_rate.max(1),
            phase: 0,
            packet: Vec::with_capacity(FRAME_BYTES),
        }
    }

    fn push_mono(&mut self, sample: f32, mut emit: impl FnMut(&[u8])) {
        self.phase += u64::from(TARGET_SAMPLE_RATE);
        while self.phase >= u64::from(self.source_rate) {
            self.phase -= u64::from(self.source_rate);
            let pcm = (sample.clamp(-1.0, 1.0) * f32::from(i16::MAX)) as i16;
            self.packet.extend_from_slice(&pcm.to_le_bytes());
            if self.packet.len() == FRAME_BYTES {
                emit(&self.packet);
                self.packet.clear();
            }
        }
    }
}

#[cfg(windows)]
mod platform {
    use super::*;
    use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
    use cpal::{SampleFormat, Stream, StreamConfig};
    use std::net::UdpSocket;
    use std::sync::atomic::{AtomicU64, Ordering};
    use std::sync::{mpsc, Arc, Mutex};
    use std::thread;
    use std::time::{Duration, Instant};

    #[derive(Default)]
    struct CaptureStats {
        bytes: AtomicU64,
        packets: AtomicU64,
        pcm: Mutex<Vec<u8>>,
        stream_error: Mutex<Option<String>>,
        relay_error: Mutex<Option<String>>,
    }

    struct ActiveCapture {
        stream: Stream,
        stats: Arc<CaptureStats>,
        started_at: Instant,
        device_name: String,
        source_rate: u32,
        source_channels: u16,
        capture_id: u64,
    }

    enum CaptureCommand {
        Configure {
            enabled: bool,
            target_port: u16,
            response: mpsc::Sender<Value>,
        },
        Start {
            response: mpsc::Sender<Value>,
        },
        Stop {
            response: mpsc::Sender<Option<PcAudioCaptureResult>>,
        },
        TakeCompleted {
            response: mpsc::Sender<Option<PcAudioCaptureResult>>,
        },
    }

    pub struct PcAudioCapture {
        commands: mpsc::Sender<CaptureCommand>,
    }

    struct CaptureRuntime {
        enabled: bool,
        target_port: u16,
        active: Option<ActiveCapture>,
        completed: Option<PcAudioCaptureResult>,
        next_capture_id: u64,
    }

    impl Default for PcAudioCapture {
        fn default() -> Self {
            let (commands, receiver) = mpsc::channel();
            thread::Builder::new()
                .name("pet-pc-microphone".to_string())
                .spawn(move || {
                    let mut runtime = CaptureRuntime::default();
                    loop {
                        match receiver.recv_timeout(Duration::from_millis(250)) {
                            Ok(command) => match command {
                                CaptureCommand::Configure {
                                    enabled,
                                    target_port,
                                    response,
                                } => {
                                    let _ = response.send(runtime.configure(enabled, target_port));
                                }
                                CaptureCommand::Start { response } => {
                                    let _ = response.send(runtime.start());
                                }
                                CaptureCommand::Stop { response } => {
                                    let _ = response.send(runtime.stop_result());
                                }
                                CaptureCommand::TakeCompleted { response } => {
                                    let _ = response.send(runtime.completed.take());
                                }
                            },
                            Err(mpsc::RecvTimeoutError::Timeout) => {
                                if runtime.active.as_ref().is_some_and(|active| {
                                    active.started_at.elapsed().as_millis()
                                        >= MAX_CAPTURE_DURATION_MS
                                }) {
                                    runtime.completed = runtime.finish_active();
                                }
                            }
                            Err(mpsc::RecvTimeoutError::Disconnected) => break,
                        }
                    }
                })
                .expect("failed to start PC microphone control thread");
            Self { commands }
        }
    }

    impl Default for CaptureRuntime {
        fn default() -> Self {
            Self {
                enabled: false,
                target_port: crate::usb_audio::DEFAULT_PCM_RELAY_PORT,
                active: None,
                completed: None,
                next_capture_id: 1,
            }
        }
    }

    impl PcAudioCapture {
        pub fn configure(&mut self, enabled: bool, target_port: u16) -> Value {
            let (response, receiver) = mpsc::channel();
            if self
                .commands
                .send(CaptureCommand::Configure {
                    enabled,
                    target_port,
                    response,
                })
                .is_err()
            {
                return capture_error("configured", "PC microphone control thread stopped");
            }
            receiver.recv().unwrap_or_else(|_| {
                capture_error("configured", "PC microphone control response was lost")
            })
        }

        pub fn start(&mut self) -> Value {
            let (response, receiver) = mpsc::channel();
            if self
                .commands
                .send(CaptureCommand::Start { response })
                .is_err()
            {
                return capture_error("begin", "PC microphone control thread stopped");
            }
            receiver
                .recv()
                .unwrap_or_else(|_| capture_error("begin", "PC microphone start response was lost"))
        }

        pub fn stop_with_pcm(&mut self) -> Option<PcAudioCaptureResult> {
            let (response, receiver) = mpsc::channel();
            self.commands.send(CaptureCommand::Stop { response }).ok()?;
            receiver.recv().ok().flatten()
        }

        pub fn take_completed(&mut self) -> Option<PcAudioCaptureResult> {
            let (response, receiver) = mpsc::channel();
            self.commands
                .send(CaptureCommand::TakeCompleted { response })
                .ok()?;
            receiver.recv().ok().flatten()
        }
    }

    impl CaptureRuntime {
        fn configure(&mut self, enabled: bool, target_port: u16) -> Value {
            let stopped = self.stop_result().map(|result| result.event);
            self.enabled = enabled;
            self.target_port = target_port;
            json!({
                "phase": "configured",
                "ok": true,
                "enabled": enabled,
                "source": "pc-microphone",
                "target": format!("127.0.0.1:{target_port}"),
                "stopped": stopped,
            })
        }

        fn start(&mut self) -> Value {
            if !self.enabled {
                return capture_error("begin", "PC microphone fallback is not enabled");
            }
            if let Some(active) = self.active.as_ref() {
                return begin_event(active, true);
            }
            self.completed = None;

            let host = cpal::default_host();
            let Some(device) = host.default_input_device() else {
                return capture_error("begin", "Windows has no default microphone input");
            };
            let device_name = device
                .name()
                .unwrap_or_else(|_| "Default microphone".to_string());
            let supported = match device.default_input_config() {
                Ok(config) => config,
                Err(error) => {
                    return capture_error(
                        "begin",
                        &format!("failed to read default microphone format: {error}"),
                    )
                }
            };
            let sample_format = supported.sample_format();
            let config: StreamConfig = supported.into();
            let source_rate = config.sample_rate.0;
            let source_channels = config.channels;
            let stats = Arc::new(CaptureStats::default());
            let capture_id = self.next_capture_id;
            self.next_capture_id = self.next_capture_id.wrapping_add(1).max(1);
            let stream = match build_stream(
                &device,
                &config,
                sample_format,
                self.target_port,
                Arc::clone(&stats),
            ) {
                Ok(stream) => stream,
                Err(error) => return capture_error("begin", &error),
            };
            if let Err(error) = stream.play() {
                return capture_error("begin", &format!("failed to start microphone: {error}"));
            }
            self.active = Some(ActiveCapture {
                stream,
                stats,
                started_at: Instant::now(),
                device_name,
                source_rate,
                source_channels,
                capture_id,
            });
            begin_event(self.active.as_ref().expect("capture inserted"), false)
        }

        fn stop_result(&mut self) -> Option<PcAudioCaptureResult> {
            self.completed.take().or_else(|| self.finish_active())
        }

        fn finish_active(&mut self) -> Option<PcAudioCaptureResult> {
            let active = self.active.take()?;
            let elapsed_ms = active.started_at.elapsed().as_millis() as u64;
            let ActiveCapture {
                stream,
                stats,
                device_name,
                capture_id,
                ..
            } = active;
            drop(stream);
            let bytes = stats.bytes.load(Ordering::Relaxed);
            let packets = stats.packets.load(Ordering::Relaxed);
            let pcm = stats
                .pcm
                .lock()
                .map(|value| value.clone())
                .unwrap_or_default();
            let stream_error = stats
                .stream_error
                .lock()
                .ok()
                .and_then(|value| value.clone());
            let relay_error = stats
                .relay_error
                .lock()
                .ok()
                .and_then(|value| value.clone());
            let event = json!({
                "phase": "end",
                "ok": stream_error.is_none(),
                "source": "pc-microphone",
                "device": device_name,
                "captureId": capture_id,
                "bytes": bytes,
                "chunks": packets,
                "durationMs": elapsed_ms,
                "forwardedDurationMs": bytes * 1000 / (u64::from(TARGET_SAMPLE_RATE) * 2),
                "error": stream_error,
                "relayError": relay_error,
            });
            Some(PcAudioCaptureResult {
                event,
                pcm,
                capture_id,
            })
        }
    }

    fn begin_event(active: &ActiveCapture, already_active: bool) -> Value {
        json!({
            "phase": "begin",
            "ok": true,
            "source": "pc-microphone",
            "device": active.device_name,
            "sampleRate": TARGET_SAMPLE_RATE,
            "channels": TARGET_CHANNELS,
            "bitsPerSample": TARGET_BITS_PER_SAMPLE,
            "sourceSampleRate": active.source_rate,
            "sourceChannels": active.source_channels,
            "captureId": active.capture_id,
            "alreadyActive": already_active,
        })
    }

    fn capture_error(phase: &str, error: &str) -> Value {
        json!({
            "phase": phase,
            "ok": false,
            "source": "pc-microphone",
            "error": error,
        })
    }

    fn build_stream(
        device: &cpal::Device,
        config: &StreamConfig,
        format: SampleFormat,
        target_port: u16,
        stats: Arc<CaptureStats>,
    ) -> Result<Stream, String> {
        match format {
            SampleFormat::F32 => {
                build_typed_stream(device, config, target_port, stats, |sample: f32| sample)
            }
            SampleFormat::I16 => {
                build_typed_stream(device, config, target_port, stats, |sample: i16| {
                    f32::from(sample) / f32::from(i16::MAX)
                })
            }
            SampleFormat::U16 => {
                build_typed_stream(device, config, target_port, stats, |sample: u16| {
                    (f32::from(sample) / f32::from(u16::MAX)) * 2.0 - 1.0
                })
            }
            other => Err(format!("unsupported microphone sample format: {other}")),
        }
    }

    fn build_typed_stream<T, F>(
        device: &cpal::Device,
        config: &StreamConfig,
        target_port: u16,
        stats: Arc<CaptureStats>,
        convert: F,
    ) -> Result<Stream, String>
    where
        T: cpal::SizedSample,
        F: Fn(T) -> f32 + Send + 'static,
    {
        let socket = UdpSocket::bind("127.0.0.1:0")
            .map_err(|error| format!("failed to open PC microphone relay socket: {error}"))?;
        let target = ("127.0.0.1", target_port);
        let channels = usize::from(config.channels.max(1));
        let mut packetizer = PcmPacketizer::new(config.sample_rate.0);
        let callback_stats = Arc::clone(&stats);
        let error_stats = Arc::clone(&stats);
        device
            .build_input_stream(
                config,
                move |data: &[T], _| {
                    for frame in data.chunks(channels) {
                        if frame.is_empty() {
                            continue;
                        }
                        let mono =
                            frame.iter().copied().map(&convert).sum::<f32>() / frame.len() as f32;
                        packetizer.push_mono(mono, |packet| {
                            if let Ok(mut pcm) = callback_stats.pcm.lock() {
                                if pcm.len() + packet.len() <= MAX_CAPTURE_BYTES {
                                    pcm.extend_from_slice(packet);
                                }
                            }
                            callback_stats
                                .bytes
                                .fetch_add(packet.len() as u64, Ordering::Relaxed);
                            callback_stats.packets.fetch_add(1, Ordering::Relaxed);
                            match socket.send_to(packet, target) {
                                Ok(sent) if sent == packet.len() => {}
                                Ok(sent) => set_relay_error(
                                    &callback_stats.relay_error,
                                    format!(
                                        "short PC microphone UDP write: {sent}/{}",
                                        packet.len()
                                    ),
                                ),
                                Err(error) => set_relay_error(
                                    &callback_stats.relay_error,
                                    format!("PC microphone UDP relay failed: {error}"),
                                ),
                            }
                        });
                    }
                },
                move |error| {
                    set_relay_error(
                        &error_stats.stream_error,
                        format!("PC microphone stream failed: {error}"),
                    );
                },
                None,
            )
            .map_err(|error| format!("failed to open default microphone: {error}"))
    }

    fn set_relay_error(target: &Mutex<Option<String>>, error: String) {
        if let Ok(mut current) = target.lock() {
            if current.is_none() {
                *current = Some(error);
            }
        }
    }
}

#[cfg(not(windows))]
mod platform {
    use super::*;

    #[derive(Default)]
    pub struct PcAudioCapture;

    impl PcAudioCapture {
        pub fn configure(&mut self, enabled: bool, target_port: u16) -> Value {
            json!({
                "phase": "configured",
                "ok": !enabled,
                "enabled": false,
                "source": "pc-microphone",
                "target": format!("127.0.0.1:{target_port}"),
                "error": enabled.then_some("PC microphone fallback is currently available on Windows only"),
            })
        }

        pub fn start(&mut self) -> Value {
            json!({
                "phase": "begin",
                "ok": false,
                "source": "pc-microphone",
                "error": "PC microphone fallback is currently available on Windows only",
            })
        }

        pub fn stop_with_pcm(&mut self) -> Option<PcAudioCaptureResult> {
            None
        }

        pub fn take_completed(&mut self) -> Option<PcAudioCaptureResult> {
            None
        }
    }
}

pub use platform::PcAudioCapture;

fn encode_pcm_wave(pcm: &[u8]) -> Result<Vec<u8>, String> {
    let data_len =
        u32::try_from(pcm.len()).map_err(|_| "PCM recording is too large".to_string())?;
    let riff_len = data_len
        .checked_add(36)
        .ok_or_else(|| "PCM recording is too large".to_string())?;
    let byte_rate =
        TARGET_SAMPLE_RATE * u32::from(TARGET_CHANNELS) * u32::from(TARGET_BITS_PER_SAMPLE) / 8;
    let block_align = TARGET_CHANNELS * TARGET_BITS_PER_SAMPLE / 8;
    let mut wave = Vec::with_capacity(44 + pcm.len());
    wave.extend_from_slice(b"RIFF");
    wave.extend_from_slice(&riff_len.to_le_bytes());
    wave.extend_from_slice(b"WAVEfmt ");
    wave.extend_from_slice(&16u32.to_le_bytes());
    wave.extend_from_slice(&1u16.to_le_bytes());
    wave.extend_from_slice(&TARGET_CHANNELS.to_le_bytes());
    wave.extend_from_slice(&TARGET_SAMPLE_RATE.to_le_bytes());
    wave.extend_from_slice(&byte_rate.to_le_bytes());
    wave.extend_from_slice(&block_align.to_le_bytes());
    wave.extend_from_slice(&TARGET_BITS_PER_SAMPLE.to_le_bytes());
    wave.extend_from_slice(b"data");
    wave.extend_from_slice(&data_len.to_le_bytes());
    wave.extend_from_slice(pcm);
    Ok(wave)
}

#[cfg(windows)]
fn hidden_powershell() -> std::process::Command {
    use std::os::windows::process::CommandExt;
    let mut command = std::process::Command::new("powershell.exe");
    command.creation_flags(0x08000000);
    command.args([
        "-NoProfile",
        "-NonInteractive",
        "-ExecutionPolicy",
        "Bypass",
    ]);
    command
}

#[cfg(windows)]
fn probe_built_in_stt_status() -> Result<String, String> {
    let script = r#"
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Speech
$recognizer = [System.Speech.Recognition.SpeechRecognitionEngine]::InstalledRecognizers() |
  Where-Object { $_.Culture.Name -eq 'zh-CN' } | Select-Object -First 1
if ($null -eq $recognizer) { throw 'Windows zh-CN speech recognizer is not installed' }
[Console]::Write($recognizer.Description)
"#;
    let output = hidden_powershell()
        .args(["-Command", script])
        .output()
        .map_err(|error| format!("failed to probe Windows speech recognizer: {error}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_string());
    }
    let description = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if description.is_empty() {
        Err("Windows zh-CN speech recognizer returned no description".to_string())
    } else {
        Ok(description)
    }
}

#[cfg(windows)]
pub fn built_in_stt_status() -> Result<String, String> {
    const SUCCESS_TTL: std::time::Duration = std::time::Duration::from_secs(30 * 60);
    const FAILURE_TTL: std::time::Duration = std::time::Duration::from_secs(15);
    type CachedStatus = Option<(std::time::Instant, Result<String, String>)>;
    static STATUS: std::sync::OnceLock<std::sync::Mutex<CachedStatus>> = std::sync::OnceLock::new();

    let cache = STATUS.get_or_init(|| std::sync::Mutex::new(None));
    let Ok(mut cached) = cache.lock() else {
        return probe_built_in_stt_status();
    };
    if let Some((checked_at, result)) = cached.as_ref() {
        let ttl = if result.is_ok() {
            SUCCESS_TTL
        } else {
            FAILURE_TTL
        };
        if checked_at.elapsed() < ttl {
            return result.clone();
        }
    }

    let result = probe_built_in_stt_status();
    *cached = Some((std::time::Instant::now(), result.clone()));
    result
}

#[cfg(not(windows))]
pub fn built_in_stt_status() -> Result<String, String> {
    Err("built-in speech recognition is currently available on Windows only".to_string())
}

#[cfg(windows)]
pub fn transcribe_pcm_s16le(pcm: &[u8]) -> Result<String, String> {
    if pcm.len() < FRAME_BYTES {
        return Err("recording is too short for speech recognition".to_string());
    }
    let wav_path = write_pcm_wave_temp(pcm)?;

    let script = r#"
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)
Add-Type -AssemblyName System.Speech
$culture = [Globalization.CultureInfo]::GetCultureInfo('zh-CN')
$recognizer = [System.Speech.Recognition.SpeechRecognitionEngine]::new($culture)
try {
  $recognizer.LoadGrammar([System.Speech.Recognition.DictationGrammar]::new())
  $recognizer.SetInputToWaveFile($env:PET_MANAGER_STT_WAV)
  $phrases = [System.Collections.Generic.List[string]]::new()
  while ($true) {
    try {
      $result = $recognizer.Recognize([TimeSpan]::FromSeconds(35))
    } catch [System.InvalidOperationException] {
      if ($phrases.Count -gt 0) { break }
      throw
    }
    if ($null -eq $result) { break }
    if (-not [string]::IsNullOrWhiteSpace($result.Text)) { $phrases.Add($result.Text.Trim()) }
  }
  [Console]::Write(($phrases -join ''))
} finally {
  $recognizer.Dispose()
}
"#;
    let output = hidden_powershell()
        .env("PET_MANAGER_STT_WAV", wav_path.as_os_str())
        .args(["-Command", script])
        .output()
        .map_err(|error| format!("failed to start Windows speech recognition: {error}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_string());
    }
    let text = String::from_utf8_lossy(&output.stdout).trim().to_string();
    if text.is_empty() {
        Err("speech recognizer did not detect an utterance".to_string())
    } else {
        Ok(text)
    }
}

fn write_pcm_wave_temp(pcm: &[u8]) -> Result<tempfile::TempPath, String> {
    use std::io::Write;

    let wave = encode_pcm_wave(pcm)?;
    let mut wav_file = tempfile::Builder::new()
        .prefix("pet-manager-ptt-")
        .suffix(".wav")
        .tempfile()
        .map_err(|error| format!("failed to create temporary PTT recording: {error}"))?;
    wav_file
        .write_all(&wave)
        .map_err(|error| format!("failed to write temporary PTT recording: {error}"))?;
    wav_file
        .flush()
        .map_err(|error| format!("failed to flush temporary PTT recording: {error}"))?;
    Ok(wav_file.into_temp_path())
}

#[cfg(not(windows))]
pub fn transcribe_pcm_s16le(_pcm: &[u8]) -> Result<String, String> {
    Err("built-in speech recognition is currently available on Windows only".to_string())
}

pub use crate::volcengine_asr::{StreamingSpeechEvent, StreamingSpeechRecognizer};

pub fn fallback_gesture(topic: &str, payload: &Value) -> Option<&'static str> {
    if topic != "input/event"
        || payload.get("action").and_then(Value::as_str) != Some("voice_ptt")
        || payload.get("handledLocally").and_then(Value::as_bool) == Some(true)
    {
        return None;
    }
    match payload.get("gesture").and_then(Value::as_str) {
        Some("hold_start") => Some("start"),
        Some("hold_end") => Some("stop"),
        _ => None,
    }
}

pub fn fallback_gesture_for_board(
    topic: &str,
    payload: &Value,
    expected_board_device_id: &str,
) -> Option<&'static str> {
    let expected = expected_board_device_id.trim();
    let actual = payload
        .get("boardDeviceId")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .trim();
    if expected.is_empty() || actual != expected {
        return None;
    }
    fallback_gesture(topic, payload)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn packetizer_emits_20ms_s16le_frames() {
        let mut packetizer = PcmPacketizer::new(48_000);
        let mut packets = Vec::new();
        for _ in 0..960 {
            packetizer.push_mono(0.5, |packet| packets.push(packet.to_vec()));
        }
        assert_eq!(packets.len(), 1);
        assert_eq!(packets[0].len(), FRAME_BYTES);
        assert!(packets[0].iter().any(|byte| *byte != 0));
    }

    #[test]
    fn pcm_wave_header_matches_16khz_mono_s16le() {
        let pcm = vec![0x34, 0x12, 0x78, 0x56];
        let wave = encode_pcm_wave(&pcm).unwrap();
        assert_eq!(&wave[0..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(wave[4..8].try_into().unwrap()), 40);
        assert_eq!(&wave[8..12], b"WAVE");
        assert_eq!(u16::from_le_bytes(wave[22..24].try_into().unwrap()), 1);
        assert_eq!(u32::from_le_bytes(wave[24..28].try_into().unwrap()), 16_000);
        assert_eq!(u16::from_le_bytes(wave[34..36].try_into().unwrap()), 16);
        assert_eq!(u32::from_le_bytes(wave[40..44].try_into().unwrap()), 4);
        assert_eq!(&wave[44..], pcm);
    }

    #[test]
    fn speech_recognition_temp_wave_is_closed_and_readable() {
        let pcm = [0x34, 0x12].repeat(FRAME_BYTES / 2);
        let path = write_pcm_wave_temp(&pcm).unwrap();
        let wave = std::fs::read(path.as_ref() as &std::path::Path).unwrap();
        assert_eq!(&wave[0..4], b"RIFF");
        assert_eq!(&wave[44..], pcm);
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires the Windows zh-CN desktop voice and speech recognizer"]
    fn windows_stt_keeps_recognized_text_when_wave_reaches_eof() {
        let temp_dir = tempfile::tempdir().unwrap();
        let speech_path = temp_dir.path().join("speech.wav");
        let script = r#"
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$synthesizer = [System.Speech.Synthesis.SpeechSynthesizer]::new()
try {
  $culture = [Globalization.CultureInfo]::GetCultureInfo('zh-CN')
  $synthesizer.SelectVoiceByHints(
    [System.Speech.Synthesis.VoiceGender]::NotSet,
    [System.Speech.Synthesis.VoiceAge]::NotSet,
    0,
    $culture
  )
  $format = [System.Speech.AudioFormat.SpeechAudioFormatInfo]::new(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono
  )
  $synthesizer.SetOutputToWaveFile($env:PET_MANAGER_TTS_WAV, $format)
  $synthesizer.Speak('你好，继续当前任务。')
} finally {
  $synthesizer.Dispose()
}
"#;
        let output = hidden_powershell()
            .env("PET_MANAGER_TTS_WAV", &speech_path)
            .args(["-Command", script])
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );

        let wave = std::fs::read(&speech_path).unwrap();
        let mut offset = 12usize;
        let mut pcm = None;
        while offset + 8 <= wave.len() {
            let chunk_len =
                u32::from_le_bytes(wave[offset + 4..offset + 8].try_into().unwrap()) as usize;
            let data_start = offset + 8;
            let data_end = data_start.saturating_add(chunk_len);
            if data_end > wave.len() {
                break;
            }
            if &wave[offset..offset + 4] == b"data" {
                pcm = Some(&wave[data_start..data_end]);
                break;
            }
            offset = data_end + (chunk_len & 1);
        }

        let text = transcribe_pcm_s16le(pcm.expect("synthesized WAV must contain PCM data"))
            .expect("recognized text must survive the recognizer's end-of-input signal");
        assert!(!text.trim().is_empty());
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires the Windows zh-CN desktop voice and speech recognizer"]
    fn windows_streaming_stt_emits_partial_or_final_text_before_exit() {
        use std::sync::mpsc;
        use std::time::{Duration, Instant};

        let temp_dir = tempfile::tempdir().unwrap();
        let speech_path = temp_dir.path().join("streaming-speech.wav");
        let script = r#"
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Speech
$synthesizer = [System.Speech.Synthesis.SpeechSynthesizer]::new()
try {
  $culture = [Globalization.CultureInfo]::GetCultureInfo('zh-CN')
  $synthesizer.SelectVoiceByHints(
    [System.Speech.Synthesis.VoiceGender]::NotSet,
    [System.Speech.Synthesis.VoiceAge]::NotSet,
    0,
    $culture
  )
  $format = [System.Speech.AudioFormat.SpeechAudioFormatInfo]::new(
    16000,
    [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen,
    [System.Speech.AudioFormat.AudioChannel]::Mono
  )
  $synthesizer.SetOutputToWaveFile($env:PET_MANAGER_TTS_WAV, $format)
  $synthesizer.Speak('你好，帮我继续当前任务。')
} finally {
  $synthesizer.Dispose()
}
"#;
        let output = hidden_powershell()
            .env("PET_MANAGER_TTS_WAV", &speech_path)
            .args(["-Command", script])
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stderr)
        );

        let wave = std::fs::read(&speech_path).unwrap();
        let mut offset = 12usize;
        let mut pcm = None;
        while offset + 8 <= wave.len() {
            let chunk_len =
                u32::from_le_bytes(wave[offset + 4..offset + 8].try_into().unwrap()) as usize;
            let data_start = offset + 8;
            let data_end = data_start.saturating_add(chunk_len);
            if data_end > wave.len() {
                break;
            }
            if &wave[offset..offset + 4] == b"data" {
                pcm = Some(&wave[data_start..data_end]);
                break;
            }
            offset = data_end + (chunk_len & 1);
        }
        let pcm = pcm.expect("synthesized WAV must contain PCM data");
        let (sender, receiver) = mpsc::channel();
        let recognizer = StreamingSpeechRecognizer::start(move |event| {
            let _ = sender.send(event);
        })
        .unwrap();
        let mut push_error = None;
        for chunk in pcm.chunks(FRAME_BYTES) {
            if let Err(error) = recognizer.push_pcm(chunk) {
                push_error = Some(error);
                break;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        let _ = recognizer.finish();

        let deadline = Instant::now() + Duration::from_secs(15);
        let mut observed_text = String::new();
        let mut observed_errors = Vec::new();
        let mut observed_events = Vec::new();
        while Instant::now() < deadline {
            match receiver.recv_timeout(Duration::from_millis(500)) {
                Ok(StreamingSpeechEvent::Partial { text, .. }) => {
                    observed_events.push(format!("partial:{text}"));
                    observed_text = text;
                }
                Ok(StreamingSpeechEvent::Final { text, .. }) => {
                    observed_events.push(format!("final:{text}"));
                    if !text.is_empty() {
                        observed_text = text;
                    }
                    break;
                }
                Ok(StreamingSpeechEvent::Error(error)) => {
                    observed_events.push(format!("error:{error}"));
                    observed_errors.push(error);
                }
                Ok(StreamingSpeechEvent::Ready) => observed_events.push("ready".to_string()),
                Err(mpsc::RecvTimeoutError::Timeout) => {}
                Err(mpsc::RecvTimeoutError::Disconnected) => break,
            }
        }
        assert!(
            !observed_text.trim().is_empty(),
            "push_error={push_error:?}, recognizer_errors={observed_errors:?}, events={observed_events:?}"
        );
        assert!(
            observed_events
                .iter()
                .any(|event| event.starts_with("partial:")),
            "streaming recognition emitted no partial text: {observed_events:?}"
        );
    }

    #[test]
    fn fallback_only_handles_unserved_voice_hold_events() {
        let start = json!({
            "boardDeviceId": "p4-board-a",
            "action": "voice_ptt",
            "gesture": "hold_start",
            "handledLocally": false,
        });
        let local = json!({
            "action": "voice_ptt",
            "gesture": "hold_start",
            "handledLocally": true,
        });
        assert_eq!(fallback_gesture("input/event", &start), Some("start"));
        assert_eq!(fallback_gesture("input/event", &local), None);
        assert_eq!(fallback_gesture("hello", &start), None);
        assert_eq!(
            fallback_gesture_for_board("input/event", &start, "p4-board-a"),
            Some("start")
        );
        assert_eq!(
            fallback_gesture_for_board("input/event", &start, "p4-board-b"),
            None
        );
        assert_eq!(fallback_gesture_for_board("input/event", &start, ""), None);
    }

    #[test]
    fn windows_stt_capability_probe_is_cached_for_runtime_polls() {
        let source = include_str!("pc_audio.rs");

        assert!(
            source.contains("static STATUS: std::sync::OnceLock<std::sync::Mutex<CachedStatus>>")
        );
        assert!(source.contains("const FAILURE_TTL: std::time::Duration"));
        assert!(source.contains("let ttl = if result.is_ok()"));
        assert!(source.contains("let result = probe_built_in_stt_status()"));
    }

    #[cfg(windows)]
    #[test]
    #[ignore = "requires a Windows microphone input device"]
    fn pc_microphone_hardware_emits_pcm_datagrams() {
        use std::net::UdpSocket;
        use std::time::Duration;

        let receiver = UdpSocket::bind("127.0.0.1:0").unwrap();
        receiver
            .set_read_timeout(Some(Duration::from_secs(4)))
            .unwrap();
        let port = receiver.local_addr().unwrap().port();
        let mut capture = PcAudioCapture::default();
        assert_eq!(capture.configure(true, port)["ok"], true);
        let begin = capture.start();
        assert_eq!(begin["ok"], true, "{begin}");

        let mut packet = [0u8; FRAME_BYTES];
        let received = receiver.recv(&mut packet).unwrap();
        let result = capture.stop_with_pcm().expect("capture should be active");
        assert_eq!(received, FRAME_BYTES);
        assert!(packet.iter().any(|byte| *byte != 0));
        assert_eq!(result.event["ok"], true, "{}", result.event);
        assert!(result.pcm.len() >= FRAME_BYTES);
    }
}
