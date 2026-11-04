//! Offline regressions for actual decoded-byte budgets and explicit local sources.
use super::*;
use std::io::{Cursor, Read};

fn archive(count: usize, per_file: u64) -> Vec<u8> {
    let mut writer = zip::ZipWriter::new(Cursor::new(Vec::new()));
    for i in 0..count {
        writer.start_file(format!("assets/file-{i}.bin"), zip::write::FileOptions::default()
            .compression_method(zip::CompressionMethod::Deflated).compression_level(Some(1))).unwrap();
        std::io::copy(&mut std::io::repeat(0).take(per_file), &mut writer).unwrap();
    }
    writer.finish().unwrap().into_inner()
}

fn forge_uncompressed_sizes(bytes: &mut [u8], count: usize) {
    let end = bytes.len() - 22;
    assert_eq!(&bytes[end..end + 4], b"PK\x05\x06");
    let mut at = u32::from_le_bytes(bytes[end + 16..end + 20].try_into().unwrap()) as usize;
    for _ in 0..count {
        assert_eq!(&bytes[at..at + 4], b"PK\x01\x02");
        let name = u16::from_le_bytes(bytes[at + 28..at + 30].try_into().unwrap()) as usize;
        let extra = u16::from_le_bytes(bytes[at + 30..at + 32].try_into().unwrap()) as usize;
        let comment = u16::from_le_bytes(bytes[at + 32..at + 34].try_into().unwrap()) as usize;
        let local = u32::from_le_bytes(bytes[at + 42..at + 46].try_into().unwrap()) as usize;
        assert_eq!(&bytes[local..local + 4], b"PK\x03\x04");
        bytes[at + 24..at + 28].copy_from_slice(&0u32.to_le_bytes());
        bytes[local + 22..local + 26].copy_from_slice(&0u32.to_le_bytes());
        at += 46 + name + extra + comment;
    }
}

#[test]
fn forged_zip_lengths_cannot_bypass_actual_expanded_budget() {
    let mut bytes = archive(5, CLAWPKG_MAX_ENTRY_BYTES);
    assert!(bytes.len() < CLAWPKG_MAX_ARCHIVE_BYTES);
    forge_uncompressed_sizes(&mut bytes, 5);
    // Establish that this is the risky format: locked zip sees declared zero,
    // yet its actual Deflate reader returns the full, CRC-valid first payload.
    let mut zip = zip::ZipArchive::new(Cursor::new(&bytes)).unwrap();
    let mut first = zip.by_index(0).unwrap();
    assert_eq!(first.size(), 0);
    assert_eq!(std::io::copy(&mut first, &mut std::io::sink()).unwrap(), CLAWPKG_MAX_ENTRY_BYTES);
    drop(first); drop(zip);
    let error = validate_clawpkg_bytes(&bytes).unwrap_err();
    assert!(error.contains("实际解压长度") || error.contains("总大小超过上限"), "{error}");
}

#[test]
fn honest_zip_total_boundary_and_single_entry_limits_remain_enforced() {
    // Missing component manifest is a structured validation result, not an IO
    // budget failure: four exact-limit entries may be collected for validation.
    assert!(validate_clawpkg_bytes(&archive(4, CLAWPKG_MAX_ENTRY_BYTES)).is_ok());
    assert!(validate_clawpkg_bytes(&archive(5, CLAWPKG_MAX_ENTRY_BYTES)).is_err());
    assert!(validate_clawpkg_bytes(&archive(1, CLAWPKG_MAX_ENTRY_BYTES + 1)).is_err());
}

#[test]
fn directory_collection_respects_actual_total_boundary() {
    let root = tempfile::tempdir().unwrap();
    for i in 0..4 {
        std::fs::File::create(root.path().join(format!("file-{i}"))).unwrap()
            .set_len(CLAWPKG_MAX_ENTRY_BYTES).unwrap();
    }
    let files = collect_clawpkg_directory_files(root.path()).unwrap();
    assert_eq!(files.values().map(|b| b.len() as u64).sum::<u64>(), CLAWPKG_MAX_EXPANDED_BYTES);
    drop(files);
    std::fs::write(root.path().join("file-4"), b"x").unwrap();
    assert!(collect_clawpkg_directory_files(root.path()).is_err());
}

#[test]
fn explicit_source_requires_a_regular_file_and_actual_byte_limit() {
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("input.clawpkg");
    std::fs::write(&path, b"abcd").unwrap();
    assert_eq!(read_clawpkg_source_file(&path, 4).unwrap(), b"abcd");
    assert!(read_clawpkg_source_file(&path, 3).is_err());
    assert!(read_clawpkg_source_file(root.path(), 4).is_err());
    #[cfg(unix)] {
        let linked = root.path().join("linked.clawpkg");
        std::os::unix::fs::symlink(&path, &linked).unwrap();
        assert!(read_clawpkg_source_file(&linked, 4).is_err());
        assert!(crate::component_library::inspect(&linked).is_err());
    }
    std::fs::File::create(&path).unwrap().set_len(CLAWPKG_MAX_ARCHIVE_BYTES as u64 + 1).unwrap();
    assert!(validate_clawpkg_at_path(&path).is_err());
    assert!(crate::component_library::inspect(&path).is_err());
}

#[cfg(unix)]
#[test]
fn archive_validation_and_inspection_reject_fifo_without_a_writer() {
    use std::os::unix::ffi::OsStrExt;
    let root = tempfile::tempdir().unwrap();
    let path = root.path().join("no-writer.clawpkg");
    let name = std::ffi::CString::new(path.as_os_str().as_bytes()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(name.as_ptr(), 0o600) }, 0);
    for inspect in [false, true] {
        let path = path.clone();
        let (tx, rx) = std::sync::mpsc::channel();
        // A reintroduced blocking open fails this test within two seconds; no
        // writer is ever started and no device/serial endpoint is touched.
        std::thread::spawn(move || {
            let rejected = if inspect { crate::component_library::inspect(&path).is_err() }
                else { validate_clawpkg_at_path(&path).is_err() };
            let _ = tx.send(rejected);
        });
        assert_eq!(rx.recv_timeout(std::time::Duration::from_secs(2)).unwrap(), true);
    }
}
