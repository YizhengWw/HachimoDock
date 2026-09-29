//! Independently framed IMA ADPCM: LE predictor i16, index u8, reserved zero,
//! sample count u16, low nibble first. Each block resets index, so seek/drop has
//! no hidden cross-block decoder state. Maximum 3840 mono samples (80 ms @ 48k).
const STEP: [i32; 89] = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
    494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
    2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
    10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
];
const INDEX: [i32; 8] = [-1, -1, -1, -1, 2, 4, 6, 8];
pub fn encode(pcm: &[u8]) -> Result<Vec<u8>, String> {
    if pcm.len() < 2 || pcm.len() > 7680 || pcm.len() % 2 != 0 {
        return Err("音乐音频块长度无效".into());
    }
    let first = i16::from_le_bytes([pcm[0], pcm[1]]);
    let mut predictor = i32::from(first);
    // A moderate starting step avoids long attack ramp for independent blocks.
    let mut index = 40i32;
    let count = pcm.len() / 2;
    let mut out = vec![
        pcm[0],
        pcm[1],
        index as u8,
        0,
        count as u8,
        (count >> 8) as u8,
    ];
    for (i, pair) in pcm[2..].chunks_exact(2).enumerate() {
        let sample = i32::from(i16::from_le_bytes([pair[0], pair[1]]));
        let step = STEP[index as usize];
        let delta = sample - predictor;
        let mut code = if delta < 0 { 8u8 } else { 0 };
        let mut difference = delta.abs();
        let mut change = step >> 3;
        for (mask, threshold) in [(4, step), (2, step >> 1), (1, step >> 2)] {
            if difference >= threshold {
                code |= mask;
                difference -= threshold;
                change += threshold;
            }
        }
        predictor = (predictor + if code & 8 != 0 { -change } else { change }).clamp(-32768, 32767);
        index = (index + INDEX[(code & 7) as usize]).clamp(0, 88);
        if i % 2 == 0 {
            out.push(code);
        } else {
            let last = out.last_mut().unwrap();
            *last |= code << 4;
        }
    }
    Ok(out)
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn blocks_are_bounded_and_self_contained() {
        assert!(encode(&[]).is_err());
        assert!(encode(&[0]).is_err());
        assert!(encode(&vec![0; 7682]).is_err());
        let frame = encode(&vec![0; 7680]).unwrap();
        assert_eq!(frame.len(), 1926);
        assert_eq!(&frame[4..6], &3840u16.to_le_bytes());
        assert_eq!(encode(&[12, 0]).unwrap().len(), 6);
    }
}
