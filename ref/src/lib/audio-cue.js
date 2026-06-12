/**
 * [Input] User-uploaded WAV cue files and decoded AudioBuffer-like sources.
 * [Output] Board-safe 16 kHz mono PCM WAV bytes for compact appearance OTA transfer and aplay playback.
 * [Pos] lib node in ref/src/lib
 * [Sync] If this file changes, update `ref/src/.folder.md` and appearance cue tests.
 */

export const BOARD_AUDIO_CUE_SAMPLE_RATE = 16000;
export const BOARD_AUDIO_CUE_CHANNELS = 1;
export const BOARD_AUDIO_CUE_BITS_PER_SAMPLE = 16;

function writeAscii(view, offset, text) {
  for (let i = 0; i < text.length; i += 1) {
    view.setUint8(offset + i, text.charCodeAt(i));
  }
}

function clampSample(value) {
  if (!Number.isFinite(value)) return 0;
  if (value > 1) return 1;
  if (value < -1) return -1;
  return value;
}

export function encodeMonoPcm16Wav(samples, sampleRate = BOARD_AUDIO_CUE_SAMPLE_RATE) {
  const dataBytes = samples.length * 2;
  const bytes = new Uint8Array(44 + dataBytes);
  const view = new DataView(bytes.buffer);
  const byteRate = sampleRate * BOARD_AUDIO_CUE_CHANNELS * (BOARD_AUDIO_CUE_BITS_PER_SAMPLE / 8);
  const blockAlign = BOARD_AUDIO_CUE_CHANNELS * (BOARD_AUDIO_CUE_BITS_PER_SAMPLE / 8);

  writeAscii(view, 0, "RIFF");
  view.setUint32(4, 36 + dataBytes, true);
  writeAscii(view, 8, "WAVE");
  writeAscii(view, 12, "fmt ");
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, BOARD_AUDIO_CUE_CHANNELS, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, BOARD_AUDIO_CUE_BITS_PER_SAMPLE, true);
  writeAscii(view, 36, "data");
  view.setUint32(40, dataBytes, true);

  let offset = 44;
  for (const rawSample of samples) {
    const sample = clampSample(rawSample);
    const intSample = sample < 0 ? sample * 0x8000 : sample * 0x7fff;
    view.setInt16(offset, intSample, true);
    offset += 2;
  }
  return bytes;
}

export function downmixAudioBufferToMonoSamples(audioBuffer, targetSampleRate = BOARD_AUDIO_CUE_SAMPLE_RATE) {
  const sourceRate = Number(audioBuffer.sampleRate) || targetSampleRate;
  const sourceLength = Math.max(0, Number(audioBuffer.length) || 0);
  const channelCount = Math.max(1, Number(audioBuffer.numberOfChannels) || 1);
  const targetLength = Math.max(1, Math.round((sourceLength / sourceRate) * targetSampleRate));
  const ratio = sourceRate / targetSampleRate;
  const channels = Array.from({ length: channelCount }, (_, channel) => audioBuffer.getChannelData(channel));
  const mono = new Float32Array(targetLength);

  for (let i = 0; i < targetLength; i += 1) {
    const sourcePosition = i * ratio;
    const sourceIndex = Math.min(sourceLength - 1, Math.floor(sourcePosition));
    const nextIndex = Math.min(sourceLength - 1, sourceIndex + 1);
    const mix = sourcePosition - sourceIndex;
    let sum = 0;
    for (const channel of channels) {
      const current = channel[sourceIndex] ?? 0;
      const next = channel[nextIndex] ?? current;
      sum += current + (next - current) * mix;
    }
    mono[i] = sum / channelCount;
  }
  return mono;
}

function audioContextCtor() {
  const scope = typeof window !== "undefined" ? window : globalThis;
  return scope.AudioContext || scope.webkitAudioContext;
}

export async function compressWavFileForBoard(file, targetSampleRate = BOARD_AUDIO_CUE_SAMPLE_RATE) {
  const AudioContextCtor = audioContextCtor();
  if (!AudioContextCtor) {
    throw new Error("当前环境不支持音频压缩，请在桌面客户端中上传 WAV");
  }

  const bytes = await file.arrayBuffer();
  const audioContext = new AudioContextCtor();
  try {
    const decoded = await audioContext.decodeAudioData(bytes.slice(0));
    const mono = downmixAudioBufferToMonoSamples(decoded, targetSampleRate);
    return encodeMonoPcm16Wav(mono, targetSampleRate);
  } finally {
    await audioContext.close?.();
  }
}
