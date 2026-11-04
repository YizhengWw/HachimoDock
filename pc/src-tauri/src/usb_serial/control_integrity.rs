//! Bounded serial CONTROL framing, not authentication or payload recovery.
//! Check original bytes before UTF-8/JSON. Legacy remains legacy; never search
//! for a JSON suffix or repair a damaged frame. Audio/media/snapshots retain
//! their independent existing transport. No IO, allocation or business retry.
pub(super) const ENGINE: &str = "p4-control-fnv1a32-v1";
const PREFIX: &[u8] = b"!P4C1:";
pub(super) const MAX_BODY: usize = 32768;
const HEADER: usize = 21;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Failure { Header, Length, Hash, Missing, Negotiation, Utf8 }
impl Failure {
    pub(super) fn label(self) -> &'static str {
        match self { Self::Header=>"header", Self::Length=>"length", Self::Hash=>"hash", Self::Missing=>"required_prefix",
            Self::Negotiation=>"negotiation",Self::Utf8=>"utf8" }
    }
    pub(super) fn index(self) -> usize { self as usize }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Observation {
    pub(super) framed: bool,
    pub(super) expected: Option<usize>,
    pub(super) actual: usize,
    pub(super) failure: Option<Failure>,
}
pub(super) fn fingerprint(bytes: &[u8]) -> u32 {
    bytes.iter().fold(2166136261u32, |v,b| (v ^ u32::from(*b)).wrapping_mul(16777619))
}
pub(super) fn decode(raw: &[u8]) -> (&[u8], Observation) {
    let mut observation=Observation { framed:false, expected:None, actual:raw.len(), failure:None };
    if !raw.starts_with(PREFIX) { return (raw,observation); }
    observation.framed=true;
    let invalid=|mut value: Observation,reason| { value.failure=Some(reason); (raw,value) };
    if raw.len()<HEADER || raw[11]!=b':' || raw[20]!=b':' {
        return invalid(observation,Failure::Header);
    }
    let mut length=0usize;
    for b in &raw[6..11] {
        if !b.is_ascii_digit() { return invalid(observation,Failure::Header); }
        length=length*10+usize::from(*b-b'0');
    }
    let mut expected_hash=0u32;
    for b in &raw[12..20] {
        let value=match b { b'0'..=b'9'=>u32::from(*b-b'0'), b'a'..=b'f'=>u32::from(*b-b'a'+10),
            _=>return invalid(observation,Failure::Header) };
        expected_hash=(expected_hash<<4)|value;
    }
    observation.expected=Some(length);observation.actual=raw.len()-HEADER;
    if !(1..=MAX_BODY).contains(&length) || length!=observation.actual {
        return invalid(observation,Failure::Length);
    }
    let body=&raw[HEADER..];
    if fingerprint(body)!=expected_hash { return invalid(observation,Failure::Hash); }
    (body,observation)
}
pub(super) fn unframed_exempt(topic: &str) -> bool {
    topic.starts_with("audio/") || topic.starts_with("media/")
        || matches!(topic,"debug/screenshot_begin"|"debug/screenshot_chunk"|"debug/screenshot_end"|"debug/screenshot_error")
}
pub(super) fn required_from_hello(payload: &serde_json::Value) -> bool {
    payload.get("capabilities").and_then(|v|v.get("serialControlIntegrity")).and_then(|v|v.as_str())==Some(ENGINE)
}
pub(super) fn valid_nonce(nonce: &str) -> bool {
    nonce.len()==32 && nonce.bytes().all(|b|b.is_ascii_digit()||(b'a'..=b'f').contains(&b))
}
pub(super) fn confirmed_nonce(payload: &serde_json::Value) -> Option<&str> {
    let caps=payload.get("capabilities")?;
    if caps.get("serialControlOffer")?.as_str()?!=ENGINE || !required_from_hello(payload) { return None; }
    caps.get("serialControlNonce")?.as_str().filter(|v|valid_nonce(v))
}
// A previous connection's valid framed HELLO must not acknowledge our probe.
// Pre-negotiation firmware (no offer field) retains its exact legacy behavior.
pub(super) fn hello_matches_probe(payload: &serde_json::Value,framed: bool,nonce: &str) -> bool {
    let offer=payload.get("capabilities").and_then(|c|c.get("serialControlOffer"));
    match offer {
        None=>required_from_hello(payload)==framed,
        Some(v)=>v.as_str()==Some(ENGINE)&&framed&&valid_nonce(nonce)&&confirmed_nonce(payload)==Some(nonce),
    }
}
pub(super) fn ack_payload(desktop_id: &str,nonce: Option<&str>) -> Option<serde_json::Value> {
    if nonce.is_some_and(|v|!valid_nonce(v)) { return None; } // Never turn a malformed proposal into a legacy reset.
    let mut payload=serde_json::json!({"desktopDeviceId":desktop_id});
    if let Some(nonce)=nonce {
        payload["serialControl"]=serde_json::json!({"engine":ENGINE,"nonce":nonce});
    }
    Some(payload)
}
pub(super) fn permitted(observation: Observation,required: bool,topic: &str) -> bool {
    observation.failure.is_none() && (!required || observation.framed || unframed_exempt(topic))
}

#[cfg(test)]
mod tests {
    use super::*;
    fn frame(body: &[u8]) -> Vec<u8> {
        let mut bytes=format!("!P4C1:{:05}:{:08x}:",body.len(),fingerprint(body)).into_bytes();
        assert_eq!(bytes.len(),HEADER);bytes.extend_from_slice(body);bytes
    }
    #[test]
    fn exact_bytes_not_json_syntax_or_lossy_text_are_the_integrity_boundary() {
        let body=br#"{"topic":"reply","payload":{"count":10}}"#;
        let raw=frame(body);let (decoded,o)=decode(&raw);assert_eq!(decoded,body);assert!(o.framed&&o.failure.is_none());
        let mut corrupted=raw.clone();*corrupted.last_mut().unwrap()=b']';
        assert_eq!(decode(&corrupted).1.failure,Some(Failure::Hash));
        let mut valid_json=raw.clone();let at=valid_json.iter().position(|b|*b==b'1').unwrap();
        // Change only the payload digit, retain perfectly valid JSON.
        let at=if at<HEADER {HEADER+body.iter().rposition(|b|*b==b'1').unwrap()}else{at};
        valid_json[at]=b'2';assert!(serde_json::from_slice::<serde_json::Value>(&valid_json[HEADER..]).is_ok());
        assert_eq!(decode(&valid_json).1.failure,Some(Failure::Hash));
        let mut shortened=raw.clone();shortened.remove(HEADER+2);
        let o=decode(&shortened).1;assert_eq!(o.failure,Some(Failure::Length));assert_eq!(o.expected,Some(body.len()));assert_eq!(o.actual,body.len()-1);
        assert_eq!(decode(&frame(&[0xff])).1.failure,None); // UTF-8 is checked by the actual serial reader, never repaired here.
    }
    #[test]
    fn malformed_overbound_header_and_embedded_json_cannot_be_recovered() {
        for raw in [b"!P4C1:".as_slice(),b"!P4C1:00001:zzzzzzzz:x",b"!P4C1:00001:00000000!x",b"!P4C1:00x01:00000000:x"] {
            assert_eq!(decode(raw).1.failure,Some(Failure::Header));
        }
        assert_eq!(decode(&frame(&[])).1.failure,Some(Failure::Length));
        assert_eq!(decode(&frame(&vec![0;MAX_BODY+1])).1.failure,Some(Failure::Length));
        let body=vec![b'x';MAX_BODY];assert_eq!(decode(&frame(&body)).0,body);
        let embedded=b"noise{\"topic\":\"hello\"}";let (bytes,o)=decode(embedded);assert_eq!(bytes,embedded);assert!(!o.framed);
    }
    #[test]
    fn negotiated_control_never_downgrades_or_exempts_receipts() {
        let (_,legacy)=decode(b"{\"topic\":\"hello\"}");
        for topic in ["hello","protocol/ack","widget-install-ack","widget/program-status","diagnostics/status","debug/screenshot_fake"] {
            assert!(!permitted(legacy,true,topic));assert!(permitted(legacy,false,topic));
        }
        for topic in ["audio/frame","media/ack","debug/screenshot_begin","debug/screenshot_chunk","debug/screenshot_end","debug/screenshot_error"] {
            assert!(permitted(legacy,true,topic));
        }
        let mut bad=legacy;bad.failure=Some(Failure::Hash);assert!(!permitted(bad,true,"audio/frame"));
        assert!(required_from_hello(&serde_json::json!({"capabilities":{"serialControlIntegrity":ENGINE}})));
        assert!(!required_from_hello(&serde_json::json!({"capabilities":{"serialControlIntegrity":true}})));
    }
    #[test]
    fn shared_c_and_rust_utf8_vectors_are_exact_original_bytes() {
        let rows:Vec<serde_json::Value>=serde_json::from_str(include_str!("../../../../firmware/tests/vectors/p4_control_integrity.json")).unwrap();
        for row in rows {
            let bytes=row["body"].as_str().unwrap().as_bytes();
            assert_eq!(bytes.len() as u64,row["bytes"].as_u64().unwrap());
            assert_eq!(format!("{:08x}",fingerprint(bytes)),row["hash"].as_str().unwrap());
            let framed=frame(bytes);let (body,observation)=decode(&framed);
            assert_eq!(body,bytes);assert!(observation.failure.is_none());
        }
    }
    #[test]
    fn offered_integrity_requires_this_probes_nonce_and_a_framed_confirmation() {
        let nonce="0123456789abcdef0123456789abcdef";
        let other="1123456789abcdef0123456789abcdef";
        let legacy=serde_json::json!({"capabilities":{}});
        assert!(hello_matches_probe(&legacy,false,nonce));
        assert!(!hello_matches_probe(&legacy,true,nonce));
        let old_guarded=serde_json::json!({"capabilities":{"serialControlIntegrity":ENGINE}});
        assert!(hello_matches_probe(&old_guarded,true,nonce));
        assert!(!hello_matches_probe(&old_guarded,false,nonce));
        let mut offered=serde_json::json!({"capabilities":{"serialControlOffer":ENGINE}});
        assert!(!hello_matches_probe(&offered,false,nonce));
        assert!(!hello_matches_probe(&offered,true,nonce));
        offered["capabilities"]["serialControlIntegrity"]=ENGINE.into();
        offered["capabilities"]["serialControlNonce"]=nonce.into();
        assert!(hello_matches_probe(&offered,true,nonce));
        assert!(!hello_matches_probe(&offered,false,nonce));
        assert!(!hello_matches_probe(&offered,true,other));
        assert_eq!(confirmed_nonce(&offered),Some(nonce));
        for bad in [serde_json::Value::Null,true.into(),"unknown".into()] {
            offered["capabilities"]["serialControlOffer"]=bad;
            assert!(!hello_matches_probe(&offered,true,nonce));
        }
    }
    #[test]
    fn ack_preserves_confirmed_session_without_including_payload_or_identity_logs() {
        let nonce="0123456789abcdef0123456789abcdef";
        assert!(valid_nonce(nonce));
        assert_eq!(ack_payload("desktop",Some(nonce)).unwrap()["serialControl"]["nonce"],nonce);
        assert_eq!(ack_payload("desktop",Some(nonce)).unwrap()["serialControl"]["engine"],ENGINE);
        assert!(ack_payload("desktop",None).unwrap().get("serialControl").is_none());
        for bad in ["","0123456789abcdef0123456789abcde","0123456789abcdef0123456789abcdef0",
            "0123456789abcdef0123456789abcdeF","0123456789abcdef0123456789abcdeg"] {
            assert!(!valid_nonce(bad));assert!(ack_payload("desktop",Some(bad)).is_none());
        }
    }
}
