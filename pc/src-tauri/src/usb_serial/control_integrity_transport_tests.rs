//! Pure fault injection against the actual production framer and pause guard.
//! No serial port, GUI, app/profile directory, provider, or hardware mutation.
use super::*;
use std::collections::VecDeque;

struct Pieces(VecDeque<Result<Vec<u8>, std::io::ErrorKind>>);
impl Read for Pieces {
    fn read(&mut self, out: &mut [u8]) -> std::io::Result<usize> {
        match self.0.pop_front() {
            None => Ok(0), Some(Err(kind)) => Err(kind.into()),
            Some(Ok(bytes)) => {
                let n=out.len().min(bytes.len()); out[..n].copy_from_slice(&bytes[..n]);
                if n<bytes.len() { self.0.push_front(Ok(bytes[n..].to_vec())); }
                Ok(n)
            }
        }
    }
}
fn guarded(body: &[u8]) -> Vec<u8> {
    let mut raw=format!("!P4C1:{:05}:{:08x}:",body.len(),control_integrity::fingerprint(body)).into_bytes();
    raw.extend(body);raw.extend(b"\r\n");raw
}
fn diagnostic_body() -> Vec<u8> {
    let mut body=br#"{"topic":"diagnostics/status","payload":{"padding":""#.to_vec();
    body.extend(vec![b'x';5889-body.len()-3]);body.extend(b"\"}}");assert_eq!(body.len(),5889);body
}

#[test]
fn missing_128_at_each_uart_boundary_is_rejected_and_never_consumes_next_frame() {
    let original=guarded(&diagnostic_body());let next=guarded(br#"{"topic":"diagnostics/status","payload":{}}"#);
    for removed_at in (128..original.len()-130).step_by(64) {
        let mut damaged=original.clone();damaged.drain(removed_at..removed_at+128);
        let mut stream=damaged.clone();stream.extend(&next);
        for capacity in [1,7,64,128,8192] {
            let mut reader=BufReader::with_capacity(capacity,std::io::Cursor::new(&stream));let mut pending=vec![];
            let failed=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
            assert_eq!(failed.integrity.failure,Some(control_integrity::Failure::Length));
            assert_eq!((failed.integrity.expected,failed.integrity.actual),(Some(5889),5761));
            assert_eq!(failed.text.as_bytes(),&damaged[..damaged.len()-2]);assert!(pending.is_empty());
            let good=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
            assert_eq!(good.integrity.failure,None);assert!(good.integrity.framed&&good.valid_utf8);
            assert!(read_serial_frame(&mut reader,&mut pending).unwrap().is_none());
        }
    }
}

#[test]
fn transient_reads_preserve_every_header_and_utf8_split_without_inventing_loss() {
    let body=r#"{"topic":"diagnostics/status","payload":{"synthetic":"中文😀"}}"#.as_bytes();
    let raw=guarded(body);
    for cut in 1..raw.len() {
        for kind in [std::io::ErrorKind::TimedOut,std::io::ErrorKind::WouldBlock,std::io::ErrorKind::Interrupted] {
            let input=Pieces(VecDeque::from([Ok(raw[..cut].to_vec()),Err(kind),Ok(raw[cut..].to_vec())]));
            let mut reader=BufReader::with_capacity(7,input);let mut pending=vec![];
            assert_eq!(read_serial_frame(&mut reader,&mut pending).err().unwrap().kind(),kind);
            assert_eq!(pending,raw[..cut]);
            let frame=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
            assert_eq!(frame.integrity.failure,None);assert!(frame.valid_utf8);
            assert_eq!(frame.text.as_bytes(),body);assert!(pending.is_empty());
            assert!(frame.diagnostic_failure.is_none());
        }
    }
}

#[test]
fn same_length_corruption_stays_hash_failure_and_partial_eof_is_not_a_frame() {
    let raw=guarded(&diagnostic_body());let mut damaged=raw.clone();damaged[200]=b'y';
    let mut reader=BufReader::new(std::io::Cursor::new(damaged));let mut pending=vec![];
    let frame=read_serial_frame(&mut reader,&mut pending).unwrap().unwrap();
    assert_eq!(frame.integrity.failure,Some(control_integrity::Failure::Hash));
    for cut in [1,6,20,21,64,128,raw.len()-2] {
        let mut reader=BufReader::with_capacity(7,std::io::Cursor::new(&raw[..cut]));
        assert!(read_serial_frame(&mut reader,&mut pending).unwrap().is_none());assert!(pending.is_empty());
    }
}
