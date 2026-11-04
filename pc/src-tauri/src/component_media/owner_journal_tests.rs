//! Real serde/tempfile/File-lock adapter tests. Synthetic, TempDir-only inputs;
//! no HOST, USB manager, decoder, device, network or provider is initialized.
use super::{owner, OwnerJournal};
use owner::Storage;
use serde_json::{json, Value};
use std::{
    fs,
    path::{Path, PathBuf},
    sync::atomic::{AtomicUsize, Ordering},
};

fn fixture() -> (tempfile::TempDir, PathBuf) {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("owner.json");
    (dir, path)
}
fn scope(boot: &str) -> owner::Scope {
    owner::Scope::new("synthetic-board", boot).unwrap()
}
fn first_scope() -> owner::Scope {
    scope("0000000000000001")
}
fn guard(path: &Path) -> owner::Guard {
    owner::Guard::new(Box::new(OwnerJournal::open(path.into())))
}
fn read_json(path: &Path) -> Value {
    serde_json::from_slice(&fs::read(path).unwrap()).unwrap()
}
fn ack(op: owner::Op, session: &str) -> Value {
    let stop = op == owner::Op::Stop;
    json!({"ok":true,"bufferedMs":0,"state":if stop {"idle"} else {"buffering"},"sessionId":if stop {""} else {session}})
}
fn checked(value: Value, op: owner::Op, session: &str) -> Result<Value, String> {
    owner::validate_status(
        op,
        session,
        value["ok"].as_bool(),
        value["bufferedMs"].as_u64(),
        value["state"].as_str(),
        value["sessionId"].as_str(),
    )?;
    Ok(value)
}
fn request(
    g: &owner::Guard,
    owner: &str,
    session: &str,
    op: owner::Op,
    writes: &AtomicUsize,
) -> Result<owner::Outcome<Value>, String> {
    g.request(&first_scope(), owner, session, op, || {
        writes.fetch_add(1, Ordering::SeqCst);
        checked(ack(op, session), op, session)
    })
}
fn assert_zero_begin(g: &owner::Guard) {
    let count = AtomicUsize::new(0);
    assert!(request(g, "new-service", "fresh-attempt", owner::Op::Begin, &count).is_err());
    assert_eq!(count.load(Ordering::SeqCst), 0);
}

#[test]
fn real_adapter_syncs_pending_json_before_first_writer() {
    let (_dir, path) = fixture();
    let g = guard(&path);
    let count = AtomicUsize::new(0);
    let result = g.request(&first_scope(), "A", "one", owner::Op::Begin, || {
        let data = read_json(&path);
        assert_eq!(data["schema"], 1);
        assert_eq!(data["records"].as_array().unwrap().len(), 1);
        assert_eq!(data["records"][0]["unresolved"], true);
        assert_eq!(data["records"][0]["owner"], "A");
        assert_eq!(data["records"][0]["session"], "one");
        count.fetch_add(1, Ordering::SeqCst);
        Err::<Value, _>("synthetic lost reply".into())
    });
    assert!(result.is_err());
    assert_eq!(count.load(Ordering::SeqCst), 1);
    assert_zero_begin(&g);
}

#[test]
fn real_adapter_restart_after_begin_retains_unknown_and_zero_replay() {
    let (_dir, path) = fixture();
    let count = AtomicUsize::new(0);
    let g = guard(&path);
    request(&g, "A", "one", owner::Op::Begin, &count).unwrap();
    let before = fs::read(&path).unwrap();
    drop(g);
    for _ in 0..3 {
        let restarted = guard(&path);
        assert_zero_begin(&restarted);
        let stop_writes = AtomicUsize::new(0);
        assert!(request(&restarted, "A", "one", owner::Op::Stop, &stop_writes).is_err());
        assert_eq!(stop_writes.load(Ordering::SeqCst), 0);
        assert_eq!(fs::read(&path).unwrap(), before);
    }
    assert_eq!(count.load(Ordering::SeqCst), 1);
}

#[test]
fn real_adapter_restart_after_positive_stop_admits_fresh_intent() {
    let (_dir, path) = fixture();
    let count = AtomicUsize::new(0);
    let g = guard(&path);
    request(&g, "A", "one", owner::Op::Begin, &count).unwrap();
    request(&g, "A", "one", owner::Op::Stop, &count).unwrap();
    let data = read_json(&path);
    assert_eq!(data["records"][0]["unresolved"], false);
    assert_eq!(data["records"][0]["session"], "");
    drop(g);
    let restarted = guard(&path);
    request(&restarted, "B", "two", owner::Op::Begin, &count).unwrap();
    assert_eq!(count.load(Ordering::SeqCst), 3);
}

#[test]
fn real_adapter_malformed_truncated_extra_or_wrong_typed_json_fails_closed_without_rewrite() {
    let malformed = vec![
        b"{".to_vec(),
        b"{\"schema\":1,\"records\":[]}\0trailing".to_vec(),
        serde_json::to_vec(&json!({"schema":2,"records":[]})).unwrap(),
        serde_json::to_vec(&json!({"schema":1,"records":{},"extra":true})).unwrap(),
        serde_json::to_vec(&json!({"schema":1,"records":[{"board":"synthetic-board","boot":"0000000000000001","unresolved":"true","owner":"A","session":"one"}]})).unwrap(),
        serde_json::to_vec(&json!({"schema":1,"records":[{"board":"synthetic-board","boot":"0000000000000001","unresolved":true,"owner":"","session":"one"}]})).unwrap(),
        serde_json::to_vec(&json!({"schema":1,"records":[{"board":"synthetic-board","boot":"bad","unresolved":true,"owner":"A","session":"one"}]})).unwrap(),
    ];
    for bytes in malformed {
        let (_dir, path) = fixture();
        fs::write(&path, &bytes).unwrap();
        let g = guard(&path);
        assert_zero_begin(&g);
        assert_eq!(fs::read(path).unwrap(), bytes);
    }
}

#[test]
fn real_adapter_rejects_actual_oversize_and_more_than_128_records() {
    let row = json!({"board":"synthetic-board","boot":"0000000000000001","unresolved":true,"owner":"A","session":"one"});
    for bytes in [
        vec![b' '; 65537],
        serde_json::to_vec(&json!({"schema":1,"records":vec![row;129]})).unwrap(),
    ] {
        let (_dir, path) = fixture();
        fs::write(&path, &bytes).unwrap();
        let journal = OwnerJournal::open(path.clone());
        assert!(journal.read().is_err());
        let g = owner::Guard::new(Box::new(journal));
        assert_zero_begin(&g);
        assert_eq!(fs::read(path).unwrap(), bytes);
    }
}

#[test]
fn real_adapter_duplicate_physical_scope_cannot_reset_unresolved_owner() {
    let (_dir, path) = fixture();
    let row = json!({"board":"synthetic-board","boot":"0000000000000001","unresolved":true,"owner":"A","session":"one"});
    fs::write(
        &path,
        serde_json::to_vec(&json!({"schema":1,"records":[row.clone(),row]})).unwrap(),
    )
    .unwrap();
    let g = guard(&path);
    assert_zero_begin(&g);
}

#[test]
fn real_adapter_second_instance_cannot_read_or_overwrite_first_host_journal() {
    let (_dir, path) = fixture();
    let first = guard(&path);
    request(&first, "A", "one", owner::Op::Begin, &AtomicUsize::new(0)).unwrap();
    let before = fs::read(&path).unwrap();
    let second_adapter = OwnerJournal::open(path.clone());
    assert!(second_adapter.read().is_err());
    assert!(second_adapter.write(&[]).is_err());
    let second = owner::Guard::new(Box::new(second_adapter));
    assert_zero_begin(&second);
    assert_eq!(fs::read(&path).unwrap(), before);
    drop(second);
    drop(first);
    let restarted = guard(&path);
    assert_zero_begin(&restarted);
}

#[cfg(unix)]
#[test]
fn real_adapter_journal_and_lease_are_mode_0600() {
    use std::os::unix::fs::PermissionsExt;
    let (_dir, path) = fixture();
    let g = guard(&path);
    request(&g, "A", "one", owner::Op::Begin, &AtomicUsize::new(0)).unwrap();
    for p in [&path, &path.with_extension("lock")] {
        assert_eq!(fs::metadata(p).unwrap().permissions().mode() & 0o777, 0o600);
    }
}

#[cfg(unix)]
#[test]
fn real_adapter_refuses_symlink_and_fifo_without_reading_target_or_waiting() {
    use std::{
        ffi::CString,
        os::unix::{ffi::OsStrExt, fs::symlink},
    };
    let (dir, path) = fixture();
    let target = dir.path().join("synthetic-target");
    fs::write(&target, b"synthetic untouched").unwrap();
    symlink(&target, &path).unwrap();
    let journal = OwnerJournal::open(path.clone());
    assert!(journal.read().is_err());
    assert_eq!(fs::read(&target).unwrap(), b"synthetic untouched");
    drop(journal);
    fs::remove_file(&path).unwrap();
    let cpath = CString::new(path.as_os_str().as_bytes()).unwrap();
    assert_eq!(unsafe { libc::mkfifo(cpath.as_ptr(), 0o600) }, 0);
    let journal = OwnerJournal::open(path.clone());
    assert!(journal.read().is_err());
}

#[test]
fn real_adapter_pending_save_failure_prevents_first_writer_and_leaves_no_temp_payload() {
    let (dir, path) = fixture();
    let g = guard(&path);
    fs::create_dir(&path).unwrap(); // deterministic tempfile::persist failure
    let count = AtomicUsize::new(0);
    assert!(request(&g, "A", "one", owner::Op::Begin, &count).is_err());
    assert_eq!(count.load(Ordering::SeqCst), 0);
    assert_zero_begin(&g);
    let names: Vec<_> = fs::read_dir(dir.path())
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect();
    assert_eq!(names.len(), 2); // owner directory + stable lock, no leaked .tmp
}

#[test]
fn real_adapter_stop_replace_failure_preserves_pending_evidence_and_blocks_new_begin() {
    let (dir, path) = fixture();
    let g = guard(&path);
    let count = AtomicUsize::new(0);
    request(&g, "A", "one", owner::Op::Begin, &count).unwrap();
    let before = fs::read(&path).unwrap();
    let retained = dir.path().join("retained-pending.json");
    fs::rename(&path, &retained).unwrap();
    fs::create_dir(&path).unwrap(); // fail only the final atomic replacement
    assert!(request(&g, "A", "one", owner::Op::Stop, &count).is_err());
    assert_eq!(count.load(Ordering::SeqCst), 2); // one begin, one stop; no replay
    assert_zero_begin(&g);
    assert_eq!(fs::read(&retained).unwrap(), before);
    fs::remove_dir(&path).unwrap();
    fs::rename(&retained, &path).unwrap();
    drop(g);
    let restarted = guard(&path);
    assert_zero_begin(&restarted);
    assert_eq!(fs::read(path).unwrap(), before);
}

#[test]
fn real_adapter_replacement_readers_observe_whole_old_or_new_json() {
    let (_dir, path) = fixture();
    let journal = OwnerJournal::open(path.clone());
    let pending = owner::Record {
        scope: first_scope(),
        unresolved: true,
        owner: "A".into(),
        session: "one".into(),
    };
    journal.write(&[pending]).unwrap();
    let old_handle = fs::File::open(&path).unwrap();
    let stopped = owner::Record {
        scope: first_scope(),
        unresolved: false,
        owner: String::new(),
        session: String::new(),
    };
    journal.write(&[stopped.clone()]).unwrap();
    let old: Value = serde_json::from_reader(old_handle).unwrap();
    assert_eq!(old["records"][0]["unresolved"], true);
    assert_eq!(read_json(&path)["records"][0]["unresolved"], false);
    assert_eq!(journal.read().unwrap(), vec![stopped]);
}

#[test]
fn real_adapter_pcm_chunks_do_not_rewrite_durable_marker() {
    let (_dir, path) = fixture();
    let g = guard(&path);
    let count = AtomicUsize::new(0);
    request(&g, "A", "one", owner::Op::Begin, &count).unwrap();
    let pending = fs::read(&path).unwrap();
    for _ in 0..16 {
        request(&g, "A", "one", owner::Op::Chunk, &count).unwrap();
    }
    assert_eq!(fs::read(path).unwrap(), pending);
    assert_eq!(count.load(Ordering::SeqCst), 17);
}

#[test]
fn real_adapter_contradictory_stop_ack_leaves_durable_unknown() {
    let (_dir, path) = fixture();
    let g = guard(&path);
    request(&g, "A", "one", owner::Op::Begin, &AtomicUsize::new(0)).unwrap();
    let before = fs::read(&path).unwrap();
    let writes = AtomicUsize::new(0);
    assert!(g
        .request(&first_scope(), "A", "one", owner::Op::Stop, || {
            writes.fetch_add(1, Ordering::SeqCst);
            checked(
                json!({"ok":true,"state":"playing","sessionId":"one","bufferedMs":0}),
                owner::Op::Stop,
                "one",
            )
        })
        .is_err());
    assert_eq!(writes.load(Ordering::SeqCst), 1);
    assert_zero_begin(&g);
    assert_eq!(fs::read(path).unwrap(), before);
}

#[test]
fn real_adapter_new_boot_admission_retains_old_boot_unknown_record() {
    let (_dir, path) = fixture();
    let g = guard(&path);
    assert!(g
        .request(&first_scope(), "A", "one", owner::Op::Begin, || Err::<
            Value,
            _,
        >(
            "synthetic unknown".into()
        ))
        .is_err());
    let next_boot = scope("0000000000000002");
    g.request(&next_boot, "B", "two", owner::Op::Begin, || {
        checked(ack(owner::Op::Begin, "two"), owner::Op::Begin, "two")
    })
    .unwrap();
    let rows = read_json(&path)["records"].as_array().unwrap().clone();
    assert_eq!(rows.len(), 2);
    assert!(rows.iter().all(|r| r["unresolved"] == true));
    assert!(g.ensure_known(&first_scope()).is_err());
}
