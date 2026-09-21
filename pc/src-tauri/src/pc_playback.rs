/*
 * [Input] 16 kHz mono signed 16-bit PCM produced by the TTS client.
 * [Output] Blocking playback through the PC's default output device (cpal), with nearest-sample
 *          resampling to the device rate and channel duplication.
 * [Pos] Tauri-side speaker output used by the 人设与声音 preview (the realtime chat itself plays
 *       on the board, not on the PC).
 * [Sync] If this file changes, update `ref/.folder.md`.
 */

use cpal::traits::{DeviceTrait, HostTrait, StreamTrait};
use cpal::{SampleFormat, StreamConfig};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

const SOURCE_RATE: f64 = 16_000.0;

/// Play the PCM to completion (or until `max_duration` elapses). Blocks the calling thread;
/// call it from a worker thread, never from the UI thread.
pub fn play_pcm16_mono_16k(pcm: &[u8], max_duration: Duration) -> Result<(), String> {
    if pcm.len() < 2 {
        return Ok(());
    }
    let samples: Arc<Vec<f32>> = Arc::new(
        pcm.chunks_exact(2)
            .map(|pair| i16::from_le_bytes([pair[0], pair[1]]) as f32 / i16::MAX as f32)
            .collect(),
    );
    let host = cpal::default_host();
    let device = host
        .default_output_device()
        .ok_or_else(|| "这台电脑没有可用的音频输出设备".to_string())?;
    let supported = device
        .default_output_config()
        .map_err(|error| format!("读取音频输出配置失败: {error}"))?;
    let sample_format = supported.sample_format();
    let config: StreamConfig = supported.into();
    let channels = config.channels.max(1) as usize;
    let device_rate = config.sample_rate.0 as f64;
    let ratio = SOURCE_RATE / device_rate;
    let total_frames = ((samples.len() as f64) / ratio).ceil() as usize;
    let position = Arc::new(AtomicUsize::new(0));
    let position_writer = position.clone();
    let source = samples.clone();

    let write = move |out: &mut [f32]| {
        let mut frame_index = position_writer.load(Ordering::Relaxed);
        for frame in out.chunks_mut(channels) {
            let source_index = (frame_index as f64 * ratio) as usize;
            let value = source.get(source_index).copied().unwrap_or(0.0);
            for slot in frame.iter_mut() {
                *slot = value;
            }
            frame_index += 1;
        }
        position_writer.store(frame_index, Ordering::Relaxed);
    };

    let stream = match sample_format {
        SampleFormat::F32 => device.build_output_stream(
            &config,
            move |out: &mut [f32], _| write(out),
            |error| eprintln!("[pc-playback] stream error: {error}"),
            None,
        ),
        SampleFormat::I16 => {
            let mut scratch: Vec<f32> = Vec::new();
            device.build_output_stream(
                &config,
                move |out: &mut [i16], _| {
                    scratch.resize(out.len(), 0.0);
                    write(&mut scratch);
                    for (dst, src) in out.iter_mut().zip(scratch.iter()) {
                        *dst = (src * i16::MAX as f32) as i16;
                    }
                },
                |error| eprintln!("[pc-playback] stream error: {error}"),
                None,
            )
        }
        other => return Err(format!("不支持的音频输出格式: {other:?}")),
    }
    .map_err(|error| format!("打开音频输出失败: {error}"))?;
    stream.play().map_err(|error| format!("开始播放失败: {error}"))?;
    let started = Instant::now();
    while position.load(Ordering::Relaxed) < total_frames && started.elapsed() < max_duration {
        thread::sleep(Duration::from_millis(20));
    }
    // let the device drain its last buffer before tearing the stream down
    thread::sleep(Duration::from_millis(120));
    drop(stream);
    Ok(())
}
