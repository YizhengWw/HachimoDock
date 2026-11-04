//! Bound decoded provider responses independently of HTTP framing/headers.
use std::io::Read;

pub(super) const MAX_RESPONSE_BYTES: usize = 65_536;

pub(super) fn read_body(reader: impl Read) -> Result<Vec<u8>, String> {
    let mut bytes = Vec::new();
    reader.take((MAX_RESPONSE_BYTES + 1) as u64)
        .read_to_end(&mut bytes).map_err(|_| "响应读取失败".to_string())?;
    if bytes.len() > MAX_RESPONSE_BYTES {
        return Err("响应过大".into());
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{self, Cursor};

    #[test]
    fn accepts_exact_limit_without_a_length_header() {
        let bytes = vec![b' '; MAX_RESPONSE_BYTES];
        assert_eq!(read_body(Cursor::new(&bytes)).unwrap(), bytes);
    }

    #[test]
    fn rejects_one_byte_over_limit() {
        let bytes = vec![b' '; MAX_RESPONSE_BYTES + 1];
        assert_eq!(read_body(Cursor::new(bytes)).unwrap_err(), "响应过大");
    }

    #[test]
    fn stops_reading_at_limit_plus_one_even_without_eof() {
        struct Endless { consumed: usize }
        impl Read for Endless {
            fn read(&mut self, buffer: &mut [u8]) -> io::Result<usize> {
                assert!(self.consumed + buffer.len() <= MAX_RESPONSE_BYTES + 1);
                buffer.fill(b'x');
                self.consumed += buffer.len();
                Ok(buffer.len())
            }
        }
        let mut reader = Endless { consumed: 0 };
        assert_eq!(read_body(&mut reader).unwrap_err(), "响应过大");
        assert_eq!(reader.consumed, MAX_RESPONSE_BYTES + 1);
    }

    #[test]
    fn preserves_small_body_and_fails_closed_on_read_error() {
        assert_eq!(read_body(Cursor::new(b"{\"ok\":true}")).unwrap(), b"{\"ok\":true}");
        struct Broken;
        impl Read for Broken {
            fn read(&mut self, _: &mut [u8]) -> io::Result<usize> {
                Err(io::Error::new(io::ErrorKind::Other, "sensitive upstream detail"))
            }
        }
        assert_eq!(read_body(Broken).unwrap_err(), "响应读取失败");
    }
}
