/*
 * [Input] Native USB frame bytes and nonce-bound ESP32-P4 identity responses.
 * [Output] Bounded frame codec plus exact boardDeviceId candidate selection.
 * [Pos] Pure native USB protocol boundary beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use super::validate_expected_board_device_id;

const P4_NATIVE_USB_FRAME_MAGIC: &[u8; 4] = b"P4BU";
const P4_NATIVE_USB_FRAME_VERSION: u8 = 1;
const P4_NATIVE_USB_HEADER_LEN: usize = 16;
pub(super) const P4_NATIVE_USB_MAX_PAYLOAD: usize = 64 * 1024;

pub(super) const P4_NATIVE_KIND_JSON: u8 = 1;
pub(super) const P4_NATIVE_KIND_FILE_BEGIN: u8 = 2;
pub(super) const P4_NATIVE_KIND_FILE_DATA: u8 = 3;
pub(super) const P4_NATIVE_KIND_FILE_END: u8 = 4;
pub(super) const P4_NATIVE_KIND_COMMIT: u8 = 5;
pub(super) const P4_NATIVE_KIND_PING: u8 = 6;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct NativeUsbFrame {
    pub(super) kind: u8,
    pub(super) seq: u32,
    pub(super) payload: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct NativeUsbIdentity {
    pub(super) board_device_id: String,
    pub(super) protocol: String,
    pub(super) nonce: String,
    pub(super) protocol_schema: u32,
    pub(super) build_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(super) struct NativeUsbCandidateIdentity {
    pub(super) bus: u8,
    pub(super) address: u8,
    pub(super) identity: NativeUsbIdentity,
}

pub(super) fn encode_native_usb_frame(kind: u8, seq: u32, payload: &[u8]) -> Vec<u8> {
    let mut frame = Vec::with_capacity(P4_NATIVE_USB_HEADER_LEN + payload.len());
    frame.extend_from_slice(P4_NATIVE_USB_FRAME_MAGIC);
    frame.push(P4_NATIVE_USB_FRAME_VERSION);
    frame.push(kind);
    frame.extend_from_slice(&0u16.to_le_bytes());
    frame.extend_from_slice(&seq.to_le_bytes());
    frame.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    frame.extend_from_slice(payload);
    frame
}

pub(super) fn try_pop_native_usb_frame(
    buffer: &mut Vec<u8>,
) -> Result<Option<NativeUsbFrame>, String> {
    if buffer.len() < P4_NATIVE_USB_HEADER_LEN {
        return Ok(None);
    }
    if &buffer[0..4] != P4_NATIVE_USB_FRAME_MAGIC {
        let Some(pos) = buffer
            .windows(P4_NATIVE_USB_FRAME_MAGIC.len())
            .position(|window| window == P4_NATIVE_USB_FRAME_MAGIC)
        else {
            buffer.clear();
            return Ok(None);
        };
        buffer.drain(0..pos);
        if buffer.len() < P4_NATIVE_USB_HEADER_LEN {
            return Ok(None);
        }
    }
    if buffer[4] != P4_NATIVE_USB_FRAME_VERSION {
        return Err(format!(
            "unsupported native USB frame version {}",
            buffer[4]
        ));
    }
    let kind = buffer[5];
    let seq = u32::from_le_bytes([buffer[8], buffer[9], buffer[10], buffer[11]]);
    let payload_len = u32::from_le_bytes([buffer[12], buffer[13], buffer[14], buffer[15]]) as usize;
    if payload_len > P4_NATIVE_USB_MAX_PAYLOAD {
        return Err(format!("native USB frame too large: {payload_len}"));
    }
    let frame_len = P4_NATIVE_USB_HEADER_LEN + payload_len;
    if buffer.len() < frame_len {
        return Ok(None);
    }
    let payload = buffer[P4_NATIVE_USB_HEADER_LEN..frame_len].to_vec();
    buffer.drain(0..frame_len);
    Ok(Some(NativeUsbFrame { kind, seq, payload }))
}

pub(super) fn next_native_usb_nonce() -> String {
    uuid::Uuid::new_v4().simple().to_string()
}

pub(super) fn parse_native_usb_pong(
    message_bytes: &[u8],
    expected_nonce: &str,
) -> Result<NativeUsbIdentity, String> {
    let message: serde_json::Value = serde_json::from_slice(message_bytes)
        .map_err(|error| format!("Native USB identity JSON parse failed: {error}"))?;
    if message.get("topic").and_then(|value| value.as_str()) != Some("native/pong") {
        return Err("Native USB identity response used an unexpected topic".to_string());
    }
    let payload = message
        .get("payload")
        .and_then(|value| value.as_object())
        .ok_or("Native USB identity response did not include an object payload")?;
    let protocol = payload
        .get("protocol")
        .and_then(|value| value.as_str())
        .unwrap_or_default()
        .trim();
    if protocol != "pet-usb-native-v1" {
        return Err(format!(
            "Native USB identity response used unsupported protocol {}",
            if protocol.is_empty() {
                "<missing>"
            } else {
                protocol
            }
        ));
    }
    let board_device_id = payload
        .get("boardDeviceId")
        .and_then(|value| value.as_str())
        .unwrap_or_default()
        .trim();
    if board_device_id.is_empty() {
        return Err("Native USB identity response did not include boardDeviceId".to_string());
    }
    let nonce = payload
        .get("nonce")
        .and_then(|value| value.as_str())
        .unwrap_or_default();
    if nonce != expected_nonce {
        return Err("Native USB identity nonce mismatch".to_string());
    }
    let protocol_schema = payload
        .get("protocolSchema")
        .and_then(|value| value.as_u64())
        .and_then(|value| u32::try_from(value).ok())
        .unwrap_or_default();
    let build_id = payload
        .get("buildId")
        .and_then(|value| value.as_str())
        .unwrap_or_default()
        .to_string();
    Ok(NativeUsbIdentity {
        board_device_id: board_device_id.to_string(),
        protocol: protocol.to_string(),
        nonce: nonce.to_string(),
        protocol_schema,
        build_id,
    })
}

pub(super) fn select_native_usb_candidate(
    expected_board_device_id: &str,
    candidates: &[NativeUsbCandidateIdentity],
) -> Result<usize, String> {
    let expected_board_device_id = validate_expected_board_device_id(expected_board_device_id)?;
    let matches = candidates
        .iter()
        .enumerate()
        .filter_map(|(index, candidate)| {
            (candidate.identity.board_device_id == expected_board_device_id).then_some(index)
        })
        .collect::<Vec<_>>();
    match matches.as_slice() {
        [index] => Ok(*index),
        [] => {
            let discovered = if candidates.is_empty() {
                "没有候选设备完成身份握手".to_string()
            } else {
                candidates
                    .iter()
                    .map(|candidate| {
                        format!(
                            "{} (bus {} address {})",
                            candidate.identity.board_device_id, candidate.bus, candidate.address
                        )
                    })
                    .collect::<Vec<_>>()
                    .join(", ")
            };
            Err(format!(
                "未找到 boardDeviceId={expected_board_device_id} 的 ESP32-P4 原生 USB 设备；{discovered}。已拒绝写入以避免写错设备"
            ))
        }
        _ => {
            let locations = matches
                .iter()
                .map(|index| {
                    let candidate = &candidates[*index];
                    format!("bus {} address {}", candidate.bus, candidate.address)
                })
                .collect::<Vec<_>>()
                .join(", ");
            Err(format!(
                "检测到多个设备都声明 boardDeviceId={expected_board_device_id}（{locations}）；已拒绝写入，请先排除重复设备身份"
            ))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn candidate(board_device_id: &str, bus: u8, address: u8) -> NativeUsbCandidateIdentity {
        NativeUsbCandidateIdentity {
            bus,
            address,
            identity: NativeUsbIdentity {
                board_device_id: board_device_id.to_string(),
                protocol: "pet-usb-native-v1".to_string(),
                nonce: board_device_id.to_string(),
                protocol_schema: 4,
                build_id: format!("build-{board_device_id}"),
            },
        }
    }

    #[test]
    fn frame_header_is_little_endian() {
        let frame = encode_native_usb_frame(3, 0x1122_3344, b"abc");

        assert_eq!(&frame[0..4], b"P4BU");
        assert_eq!(frame[4], 1);
        assert_eq!(frame[5], 3);
        assert_eq!(&frame[8..12], &[0x44, 0x33, 0x22, 0x11]);
        assert_eq!(&frame[12..16], &[3, 0, 0, 0]);
        assert_eq!(&frame[16..], b"abc");
    }

    #[test]
    fn frame_parser_waits_for_complete_payload() {
        let frame = encode_native_usb_frame(1, 7, br#"{"topic":"native/pong","payload":{}}"#);
        let mut buffer = frame[..20].to_vec();

        assert!(try_pop_native_usb_frame(&mut buffer).unwrap().is_none());
        buffer.extend_from_slice(&frame[20..]);
        let parsed = try_pop_native_usb_frame(&mut buffer).unwrap().unwrap();

        assert_eq!(parsed.kind, 1);
        assert_eq!(parsed.seq, 7);
        assert_eq!(parsed.payload, br#"{"topic":"native/pong","payload":{}}"#);
        assert!(buffer.is_empty());
    }

    #[test]
    fn identity_pong_requires_matching_nonce_and_board_id() {
        let message = br#"{"topic":"native/pong","payload":{"protocol":"pet-usb-native-v1","boardDeviceId":"p4-board-a","nonce":"challenge-1","protocolSchema":4,"buildId":"0.7.21-p4+abc123"}}"#;
        let identity = parse_native_usb_pong(message, "challenge-1").unwrap();

        assert_eq!(identity.board_device_id, "p4-board-a");
        assert_eq!(identity.protocol_schema, 4);
        assert_eq!(identity.build_id, "0.7.21-p4+abc123");
        assert!(parse_native_usb_pong(message, "stale-challenge")
            .unwrap_err()
            .contains("nonce mismatch"));

        let missing_id = br#"{"topic":"native/pong","payload":{"protocol":"pet-usb-native-v1","nonce":"challenge-1"}}"#;
        assert!(parse_native_usb_pong(missing_id, "challenge-1")
            .unwrap_err()
            .contains("boardDeviceId"));
    }

    #[test]
    fn selection_uses_exact_board_identity_across_multiple_devices() {
        let candidates = [candidate("p4-board-a", 1, 7), candidate("p4-board-b", 2, 9)];

        assert_eq!(
            select_native_usb_candidate("p4-board-a", &candidates).unwrap(),
            0
        );
        assert_eq!(
            select_native_usb_candidate("p4-board-b", &candidates).unwrap(),
            1
        );
    }

    #[test]
    fn selection_fails_closed_for_missing_or_duplicate_identity() {
        let candidate = candidate("p4-board-a", 1, 7);
        let no_match = select_native_usb_candidate("p4-board-b", std::slice::from_ref(&candidate))
            .unwrap_err();
        assert!(no_match.contains("未找到 boardDeviceId=p4-board-b"));
        assert!(no_match.contains("避免写错设备"));

        let duplicate = NativeUsbCandidateIdentity {
            bus: 2,
            address: 9,
            ..candidate.clone()
        };
        let duplicate_error =
            select_native_usb_candidate("p4-board-a", &[candidate, duplicate]).unwrap_err();
        assert!(duplicate_error.contains("多个设备都声明 boardDeviceId=p4-board-a"));
        assert!(duplicate_error.contains("bus 1 address 7"));
        assert!(duplicate_error.contains("bus 2 address 9"));

        assert!(select_native_usb_candidate("p4-board-a", &[])
            .unwrap_err()
            .contains("没有候选设备完成身份握手"));
    }
}
