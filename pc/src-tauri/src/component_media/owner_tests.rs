use super::*;
use std::sync::{
    atomic::{AtomicBool, AtomicUsize, Ordering},
    Arc,
};
#[derive(Default)]
struct Memory {
    records: Mutex<Vec<Record>>,
    fail_read: AtomicBool,
    fail_write: AtomicBool,
    writes: AtomicUsize,
}
struct Store(Arc<Memory>);
impl Storage for Store {
    fn read(&self) -> Result<Vec<Record>, String> {
        if self.0.fail_read.load(Ordering::SeqCst) {
            Err("synthetic unreadable".into())
        } else {
            Ok(self.0.records.lock().unwrap().clone())
        }
    }
    fn write(&self, r: &[Record]) -> Result<(), String> {
        self.0.writes.fetch_add(1, Ordering::SeqCst);
        if self.0.fail_write.load(Ordering::SeqCst) {
            Err("synthetic storage failure".into())
        } else {
            *self.0.records.lock().unwrap() = r.to_vec();
            Ok(())
        }
    }
}
fn setup() -> (Arc<Guard>, Arc<Memory>) {
    let m = Arc::new(Memory::default());
    (Arc::new(Guard::new(Box::new(Store(m.clone())))), m)
}
fn scope() -> Scope {
    Scope::new("synthetic-board", "0000000000000001").unwrap()
}
fn tx(
    g: &Guard,
    who: &str,
    session: &str,
    op: Op,
    calls: &AtomicUsize,
    ok: bool,
) -> Result<Outcome<()>, String> {
    g.request(&scope(), who, session, op, || {
        calls.fetch_add(1, Ordering::SeqCst);
        if ok {
            Ok(())
        } else {
            Err("synthetic lost acknowledgement".into())
        }
    })
}
#[test]
fn unknown_begin_one_writer_then_zero_stop_or_new_begin_across_services() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    assert!(tx(&g, "A", "one", Op::Begin, &c, false).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
    for who in ["A", "B"] {
        for op in [Op::Begin, Op::Stop, Op::Chunk, Op::Query, Op::Library] {
            assert!(tx(&g, who, "fresh", op, &c, true).is_err());
        }
    }
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn unknown_chunk_never_cleanup_or_source_restart() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    assert!(tx(&g, "A", "one", Op::Begin, &c, true).is_ok());
    assert!(tx(&g, "A", "one", Op::Chunk, &c, false).is_err());
    assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
    assert!(tx(&g, "A", "two", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn source_retry_requires_once_confirmed_cleanup() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    for session in ["first", "second"] {
        assert!(tx(&g, "A", session, Op::Begin, &c, true).is_ok());
        assert!(tx(&g, "A", session, Op::Stop, &c, true).is_ok());
    }
    assert_eq!(c.load(Ordering::SeqCst), 4);
}
#[test]
fn unknown_stop_is_one_stop_not_a_retry_and_blocks_b() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    assert!(tx(&g, "A", "one", Op::Stop, &c, false).is_err());
    for _ in 0..3 {
        assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
        assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
    }
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn b_begins_only_after_a_once_positive_stop() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
    tx(&g, "A", "one", Op::Stop, &c, true).unwrap();
    tx(&g, "B", "two", Op::Begin, &c, true).unwrap();
    assert_eq!(c.load(Ordering::SeqCst), 3);
    assert!(matches!(
        tx(&g, "A", "one", Op::Stop, &c, true).unwrap(),
        Outcome::Retired
    ));
    assert_eq!(c.load(Ordering::SeqCst), 3);
}
#[test]
fn confirmed_idle_stop_is_typed_no_wire_not_a_fabricated_ack() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    tx(&g, "A", "one", Op::Stop, &c, true).unwrap();
    assert!(matches!(
        tx(&g, "A", "one", Op::Stop, &c, true).unwrap(),
        Outcome::KnownIdle
    ));
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn drop_disable_remove_rebuild_upgrade_cannot_drop_host_barrier() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    let old_service = g.clone();
    tx(&old_service, "A", "one", Op::Begin, &c, false).unwrap_err();
    drop(old_service);
    for _ in 0..4 {
        let new_service = g.clone();
        assert!(tx(&new_service, "A", "fresh", Op::Begin, &c, true).is_err());
        drop(new_service);
    }
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn same_board_boot_reconnect_does_not_change_scope() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, false).unwrap_err();
    let new_connection_scope = Scope::new("synthetic-board", "0000000000000001").unwrap();
    assert_eq!(new_connection_scope, scope());
    assert!(g
        .request(&new_connection_scope, "B", "two", Op::Begin, || {
            c.fetch_add(1, Ordering::SeqCst);
            Ok(())
        })
        .is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn real_different_boot_can_start_without_erasing_old_unknown() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, false).unwrap_err();
    let new_boot = Scope::new("synthetic-board", "0000000000000002").unwrap();
    assert!(g
        .request(&new_boot, "B", "two", Op::Begin, || {
            c.fetch_add(1, Ordering::SeqCst);
            Ok(())
        })
        .is_ok());
    assert!(g.ensure_known(&scope()).is_err());
    assert_eq!(m.records.lock().unwrap().len(), 2);
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn restart_after_acknowledged_begin_still_blocks_same_boot() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    drop(g);
    let restarted = Guard::new(Box::new(Store(m)));
    assert!(tx(&restarted, "A", "new", Op::Begin, &c, true).is_err());
    assert!(tx(&restarted, "A", "one", Op::Stop, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn restart_after_unknown_begin_blocks_all_service_recreation() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, false).unwrap_err();
    drop(g);
    let restarted = Guard::new(Box::new(Store(m)));
    assert!(tx(&restarted, "B", "two", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn restart_after_positive_stop_may_admit_fresh_intent() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    tx(&g, "A", "one", Op::Stop, &c, true).unwrap();
    drop(g);
    let restarted = Guard::new(Box::new(Store(m)));
    tx(&restarted, "B", "two", Op::Begin, &c, true).unwrap();
    assert_eq!(c.load(Ordering::SeqCst), 3);
}
#[test]
fn unresolved_record_is_durable_before_first_writer() {
    let (g, m) = setup();
    g.request(&scope(), "A", "one", Op::Begin, || {
        let rows = m.records.lock().unwrap();
        assert_eq!(rows.len(), 1);
        assert!(rows[0].unresolved);
        assert_eq!(rows[0].session, "one");
        Ok(())
    })
    .unwrap();
    assert_eq!(m.writes.load(Ordering::SeqCst), 1);
}
#[test]
fn pcm_chunks_do_not_fsync_each_packet_but_keep_durable_unknown_on_restart() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    for _ in 0..64 {
        tx(&g, "A", "one", Op::Chunk, &c, true).unwrap();
    }
    assert_eq!(m.writes.load(Ordering::SeqCst), 1);
    assert!(m.records.lock().unwrap()[0].unresolved);
    drop(g);
    let restarted = Guard::new(Box::new(Store(m)));
    assert!(tx(&restarted, "A", "one", Op::Chunk, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 65);
}
#[test]
fn persistence_failure_before_mutation_emits_zero_wire() {
    let (g, m) = setup();
    m.fail_write.store(true, Ordering::SeqCst);
    let c = AtomicUsize::new(0);
    assert!(tx(&g, "A", "one", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 0);
    m.fail_write.store(false, Ordering::SeqCst);
    assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 0);
}
#[test]
fn stop_persistence_failure_does_not_authorize_next_begin() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    m.fail_write.store(true, Ordering::SeqCst);
    assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
    m.fail_write.store(false, Ordering::SeqCst);
    assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn unreadable_or_corrupt_journal_never_resets_to_idle() {
    let m = Arc::new(Memory::default());
    m.fail_read.store(true, Ordering::SeqCst);
    let g = Guard::new(Box::new(Store(m)));
    let c = AtomicUsize::new(0);
    assert!(tx(&g, "A", "one", Op::Begin, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 0);
}
#[test]
fn malformed_duplicate_or_over_budget_records_fail_closed() {
    let good = Record {
        scope: scope(),
        unresolved: true,
        owner: "A".into(),
        session: "one".into(),
    };
    for rows in [
        vec![good.clone(), good.clone()],
        vec![Record {
            owner: String::new(),
            ..good.clone()
        }],
        vec![good; 129],
    ] {
        let m = Arc::new(Memory::default());
        *m.records.lock().unwrap() = rows;
        let g = Guard::new(Box::new(Store(m)));
        let c = AtomicUsize::new(0);
        assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
        assert_eq!(c.load(Ordering::SeqCst), 0);
    }
}
#[test]
fn no_boot_identity_no_admission() {
    for boot in ["", "unknown", "000000000000000G", "1"] {
        assert!(Scope::new("synthetic-board", boot).is_err());
    }
    for board in ["", "../board", "bad\nboard"] {
        assert!(Scope::new(board, "0000000000000001").is_err());
    }
}
#[test]
fn cross_service_library_does_not_change_active_owner_queue() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    assert!(tx(&g, "B", "", Op::Library, &c, true).is_err());
    tx(&g, "A", "", Op::Library, &c, true).unwrap();
    tx(&g, "A", "one", Op::Chunk, &c, true).unwrap();
    assert_eq!(c.load(Ordering::SeqCst), 3);
}
#[test]
fn idle_library_is_durable_and_unknown_cannot_be_cleared_by_query() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "", Op::Library, &c, false).unwrap_err();
    assert_eq!(m.writes.load(Ordering::SeqCst), 1);
    assert!(tx(&g, "A", "one", Op::Query, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
}
#[test]
fn successful_idle_library_does_not_forge_a_play_session() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "", Op::Library, &c, true).unwrap();
    assert_eq!(m.writes.load(Ordering::SeqCst), 2);
    assert!(!m.records.lock().unwrap()[0].unresolved);
    assert!(tx(&g, "A", "one", Op::Chunk, &c, true).is_err());
    tx(&g, "B", "two", Op::Begin, &c, true).unwrap();
    assert_eq!(c.load(Ordering::SeqCst), 2);
}
#[test]
fn disconnected_stop_poison_survives_service_drop() {
    let (g, m) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
    g.poison_owner("A");
    assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
    assert_eq!(c.load(Ordering::SeqCst), 1);
    assert!(m.records.lock().unwrap()[0].unresolved);
}
#[test]
fn owner_mutex_is_held_through_actual_writer_outcome() {
    let (g, _) = setup();
    let a = g.clone();
    let b = g.clone();
    let calls = Arc::new(AtomicUsize::new(0));
    let c = calls.clone();
    let (entered, seen) = std::sync::mpsc::channel();
    let (release, wait) = std::sync::mpsc::channel();
    let first = std::thread::spawn(move || {
        a.request(&scope(), "A", "one", Op::Begin, || {
            c.fetch_add(1, Ordering::SeqCst);
            entered.send(()).unwrap();
            wait.recv().unwrap();
            Err::<(), _>("unknown".into())
        })
    });
    seen.recv().unwrap();
    let c = calls.clone();
    let second = std::thread::spawn(move || {
        b.request(&scope(), "B", "two", Op::Begin, || {
            c.fetch_add(1, Ordering::SeqCst);
            Ok(())
        })
    });
    release.send(()).unwrap();
    assert!(first.join().unwrap().is_err());
    assert!(second.join().unwrap().is_err());
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[test]
fn missing_nonboolean_or_malformed_status_emits_one_writer_then_blocks_all_cleanup() {
    for (ok, buffer, state, session) in [
        (None, Some(0), Some("buffering"), Some("one")),
        (Some(false), Some(0), Some("buffering"), Some("one")),
        (Some(true), None, Some("buffering"), Some("one")),
        (Some(true), Some(0), Some("invented"), Some("one")),
        (Some(true), Some(0), Some("buffering"), None),
        (Some(true), Some(0), Some("buffering"), Some("other")),
    ] {
        let (g, _) = setup();
        let c = AtomicUsize::new(0);
        let result = g.request(&scope(), "A", "one", Op::Begin, || {
            c.fetch_add(1, Ordering::SeqCst);
            validate_status(Op::Begin, "one", ok, buffer, state, session)
        });
        assert!(result.is_err());
        assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
        assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
        assert_eq!(c.load(Ordering::SeqCst), 1);
    }
}
#[test]
fn contradictory_stop_ok_cannot_unlock_b_or_replay_stop() {
    for (state, session, buffer) in [
        ("playing", "one", 0),
        ("paused", "one", 0),
        ("buffering", "one", 0),
        ("idle", "one", 0),
        ("idle", "", 20),
    ] {
        let (g, _) = setup();
        let c = AtomicUsize::new(0);
        tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
        assert!(g
            .request(&scope(), "A", "one", Op::Stop, || {
                c.fetch_add(1, Ordering::SeqCst);
                validate_status(
                    Op::Stop,
                    "one",
                    Some(true),
                    Some(buffer),
                    Some(state),
                    Some(session),
                )
            })
            .is_err());
        assert!(tx(&g, "B", "two", Op::Begin, &c, true).is_err());
        assert!(tx(&g, "A", "one", Op::Stop, &c, true).is_err());
        assert_eq!(c.load(Ordering::SeqCst), 2);
    }
}
#[test]
fn positive_terminal_empty_stop_unlocks_b_exactly_once() {
    for state in ["idle", "error", "ended", "interrupted"] {
        let (g, _) = setup();
        let c = AtomicUsize::new(0);
        tx(&g, "A", "one", Op::Begin, &c, true).unwrap();
        g.request(&scope(), "A", "one", Op::Stop, || {
            c.fetch_add(1, Ordering::SeqCst);
            validate_status(Op::Stop, "one", Some(true), Some(0), Some(state), Some(""))
        })
        .unwrap();
        tx(&g, "B", "two", Op::Begin, &c, true).unwrap();
        assert_eq!(c.load(Ordering::SeqCst), 3);
    }
}

#[test]
fn actual_journal_lease_refuses_second_owner_and_releases_on_drop() {
    let root = std::path::PathBuf::from(std::env::var_os("RADIO_OWNER_HOST_ROOT").unwrap_or_else(
        || {
            std::env::temp_dir()
                .join(format!(
                    "hachimidock-radio-owner-test-{}",
                    std::process::id()
                ))
                .into_os_string()
        },
    ));
    std::fs::create_dir_all(&root).unwrap();
    let path = root.join(format!("journal-lock-{}.lock", std::process::id()));
    let first = journal_lock(&path).unwrap();
    assert!(journal_lock(&path).is_err());
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        assert_eq!(
            std::fs::metadata(&path).unwrap().permissions().mode() & 0o777,
            0o600
        );
    }
    drop(first);
    let next = journal_lock(&path).unwrap();
    drop(next);
    std::fs::remove_file(path).unwrap();
}
#[cfg(unix)]
#[test]
fn actual_journal_lease_refuses_symlink_without_touching_target() {
    let root = std::path::PathBuf::from(std::env::var_os("RADIO_OWNER_HOST_ROOT").unwrap_or_else(
        || {
            std::env::temp_dir()
                .join(format!(
                    "hachimidock-radio-owner-test-{}",
                    std::process::id()
                ))
                .into_os_string()
        },
    ));
    std::fs::create_dir_all(&root).unwrap();
    let target = root.join(format!("synthetic-lock-target-{}", std::process::id()));
    let link = root.join(format!("synthetic-lock-link-{}", std::process::id()));
    std::fs::write(&target, b"synthetic unchanged").unwrap();
    std::os::unix::fs::symlink(&target, &link).unwrap();
    assert!(journal_lock(&link).is_err());
    assert_eq!(std::fs::read(&target).unwrap(), b"synthetic unchanged");
    std::fs::remove_file(link).unwrap();
    std::fs::remove_file(target).unwrap();
}

#[test]
fn retired_old_worker_cleanup_never_stops_or_poisons_new_session() {
    let (g, _) = setup();
    let c = AtomicUsize::new(0);
    tx(&g, "A", "old", Op::Begin, &c, true).unwrap();
    tx(&g, "A", "old", Op::Stop, &c, true).unwrap();
    tx(&g, "A", "new", Op::Begin, &c, true).unwrap();
    assert!(matches!(
        tx(&g, "A", "old", Op::Stop, &c, true).unwrap(),
        Outcome::Retired
    ));
    assert_eq!(c.load(Ordering::SeqCst), 3);
    tx(&g, "A", "new", Op::Chunk, &c, true).unwrap();
    assert_eq!(c.load(Ordering::SeqCst), 4);
}


fn distinct_boot(number: usize) -> Scope {
    Scope::new("synthetic-board", &format!("{number:016x}")).unwrap()
}

#[test]
fn more_than_128_known_boots_stop_and_restart_reclaim_only_resolved_idle() {
    let (mut g, memory) = setup();
    let writes = AtomicUsize::new(0);
    let rounds = MAX_SCOPES * 3 + 1;
    for number in 1..=rounds {
        if number == MAX_SCOPES + 1 || number == MAX_SCOPES * 2 + 1 {
            g = Arc::new(Guard::new(Box::new(Store(memory.clone()))));
        }
        let scope = distinct_boot(number);
        for operation in [Op::Begin, Op::Stop] {
            g.request(&scope, "A", "one", operation, || {
                writes.fetch_add(1, Ordering::SeqCst);
                Ok(())
            }).unwrap();
        }
        let records = memory.records.lock().unwrap();
        assert!(records.len() <= MAX_SCOPES);
        assert!(records.iter().all(|r| !r.unresolved));
    }
    assert_eq!(writes.load(Ordering::SeqCst), rounds * 2);
}

#[test]
fn idle_compaction_retains_every_mixed_unknown_record_and_barrier() {
    let (g, memory) = setup();
    let writes = AtomicUsize::new(0);
    let unknown_count = 8;
    for number in 1..=MAX_SCOPES {
        let scope = distinct_boot(number);
        let begin = g.request(&scope, "A", "one", Op::Begin, || {
            writes.fetch_add(1, Ordering::SeqCst);
            if number <= unknown_count { Err("synthetic unknown".into()) } else { Ok(()) }
        });
        if number <= unknown_count {
            assert!(begin.is_err());
        } else {
            begin.unwrap();
            g.request(&scope, "A", "one", Op::Stop, || {
                writes.fetch_add(1, Ordering::SeqCst); Ok(())
            }).unwrap();
        }
    }
    let unknown_before: Vec<_> = memory.records.lock().unwrap().iter()
        .filter(|r| r.unresolved).cloned().collect();
    assert_eq!(unknown_before.len(), unknown_count);
    g.request(&distinct_boot(MAX_SCOPES + 1), "B", "fresh", Op::Begin, || {
        writes.fetch_add(1, Ordering::SeqCst); Ok(())
    }).unwrap();
    let records = memory.records.lock().unwrap().clone();
    assert_eq!(records.len(), unknown_count + 1);
    assert!(unknown_before.iter().all(|old| records.contains(old)));
    let before = writes.load(Ordering::SeqCst);
    for old in unknown_before {
        assert!(g.ensure_known(&old.scope).is_err());
        assert!(g.request(&old.scope, "A", "one", Op::Stop, || {
            writes.fetch_add(1, Ordering::SeqCst); Ok(())
        }).is_err());
    }
    assert_eq!(writes.load(Ordering::SeqCst), before);
}

#[test]
fn full_unknown_table_survives_restart_and_new_boot_admission_emits_zero_wire() {
    let (g, memory) = setup();
    for number in 1..=MAX_SCOPES {
        assert!(g.request(&distinct_boot(number), "A", "one", Op::Begin, || {
            Err::<(), _>("synthetic unknown".into())
        }).is_err());
    }
    let pending_before = memory.records.lock().unwrap().clone();
    let saves_before = memory.writes.load(Ordering::SeqCst);
    assert_eq!(pending_before.len(), MAX_SCOPES);
    drop(g);
    let restarted = Guard::new(Box::new(Store(memory.clone())));
    let writes = AtomicUsize::new(0);
    assert!(restarted.request(&distinct_boot(MAX_SCOPES + 1), "B", "fresh", Op::Begin, || {
        writes.fetch_add(1, Ordering::SeqCst); Ok(())
    }).is_err());
    assert_eq!(writes.load(Ordering::SeqCst), 0);
    assert_eq!(*memory.records.lock().unwrap(), pending_before);
    assert_eq!(memory.writes.load(Ordering::SeqCst), saves_before);
}

#[test]
fn idle_compaction_does_not_evict_active_leases() {
    let (g, memory) = setup();
    for number in 1..=MAX_SCOPES {
        let scope = distinct_boot(number);
        g.request(&scope, "A", "one", Op::Begin, || Ok(())).unwrap();
        if number % 2 == 0 {
            g.request(&scope, "A", "one", Op::Stop, || Ok(())).unwrap();
        }
    }
    let active_before: Vec<_> = memory.records.lock().unwrap().iter()
        .filter(|r| r.unresolved).cloned().collect();
    g.request(&distinct_boot(MAX_SCOPES + 1), "B", "fresh", Op::Begin, || Ok(())).unwrap();
    let records = memory.records.lock().unwrap().clone();
    assert_eq!(records.len(), MAX_SCOPES / 2 + 1);
    assert!(active_before.iter().all(|old| records.contains(old)));
    for old in active_before {
        assert!(g.request(&old.scope, "B", "wrong", Op::Begin, || {
            panic!("active owner must not be replaced");
            #[allow(unreachable_code)] Ok(())
        }).is_err());
    }
}

#[test]
fn correlated_terminal_chunk_allows_later_entry_without_replaying_old_chunk() {
    let (g,memory)=setup(); let calls=AtomicUsize::new(0);
    tx(&g,"city-radio","first",Op::Begin,&calls,true).unwrap();
    let result=g.request_observed(&scope(),"city-radio","first",Op::Chunk,||{
        calls.fetch_add(1,Ordering::SeqCst);Ok(Reply::DeviceStopped(()))
    }).unwrap();
    assert!(matches!(result,Outcome::DeviceStopped(())));
    assert!(g.ensure_known(&scope()).is_ok());
    assert!(!memory.records.lock().unwrap()[0].unresolved);
    let before=calls.load(Ordering::SeqCst);
    assert!(matches!(tx(&g,"city-radio","first",Op::Stop,&calls,true).unwrap(),Outcome::KnownIdle));
    assert_eq!(calls.load(Ordering::SeqCst),before);
    tx(&g,"city-radio","second",Op::Begin,&calls,true).unwrap();
    assert_eq!(calls.load(Ordering::SeqCst),3);
}
#[test]
fn terminal_proof_rejects_missing_fields_live_state_and_nonempty_session() {
    assert!(stopped_rejection(Op::Chunk,Some(false),Some(0),Some("idle"),Some("")));
    assert!(stopped_rejection(Op::Query,Some(false),Some(0),Some("ended"),Some("")));
    for (op,ok,buffer,state,session) in [
        (Op::Begin,Some(false),Some(0),Some("idle"),Some("")),
        (Op::Chunk,None,Some(0),Some("idle"),Some("")),
        (Op::Chunk,Some(true),Some(0),Some("idle"),Some("")),
        (Op::Chunk,Some(false),None,Some("idle"),Some("")),
        (Op::Chunk,Some(false),Some(1),Some("idle"),Some("")),
        (Op::Chunk,Some(false),Some(0),Some("paused"),Some("")),
        (Op::Chunk,Some(false),Some(0),Some("playing"),Some("")),
        (Op::Chunk,Some(false),Some(0),Some("idle"),Some("other")),
        (Op::Chunk,Some(false),Some(0),Some("idle"),None),
    ] {assert!(!stopped_rejection(op,ok,buffer,state,session));}
}
#[test]
fn terminal_reply_cannot_resolve_unknown_or_mismatched_owner() {
    let (g,_)=setup();let calls=AtomicUsize::new(0);
    tx(&g,"A","first",Op::Begin,&calls,true).unwrap();
    assert!(g.request_observed::<()>(&scope(),"B","first",Op::Chunk,||panic!("foreign owner must not query")).is_err());
    assert!(tx(&g,"A","first",Op::Chunk,&calls,false).is_err());
    assert!(g.request_observed::<()>(&scope(),"A","first",Op::Chunk,||panic!("unknown owner must not query")).is_err());
}
#[test]
fn terminal_reply_storage_failure_still_blocks_new_begin() {
    let (g,memory)=setup();let calls=AtomicUsize::new(0);
    tx(&g,"A","first",Op::Begin,&calls,true).unwrap();
    memory.fail_write.store(true,Ordering::SeqCst);
    assert!(g.request_observed(&scope(),"A","first",Op::Chunk,||Ok(Reply::DeviceStopped(()))).is_err());
    assert!(g.ensure_known(&scope()).is_err());
    assert!(memory.records.lock().unwrap()[0].unresolved);
}

fn recovery_proof(ok:Option<bool>,buffered:Option<u64>,state:Option<&str>,session:Option<&str>,message:Option<&str>)->RecoveryProof {
    RecoveryProof{ok,buffered_ms:buffered,state:state.map(str::to_owned),session_id:session.map(str::to_owned),message:message.map(str::to_owned)}
}
fn known_active_reply(session:&str)->RecoveryProof {
    recovery_proof(Some(true),Some(600),Some("playing"),Some(session),None)
}
fn known_idle_reply(session:&str)->RecoveryProof {
    recovery_proof(Some(true),Some(0),Some("idle"),Some(session),None)
}
#[test]
fn rejected_active_classifier_is_chunk_only_complete_exact_and_known_reason() {
    for state in ["playing","buffering","paused"] {
        for message in ["音乐数据顺序或格式无效","音乐缓冲区已满"] {
            assert!(rejected_active(Op::Chunk,"one",Some(false),Some(600),Some(state),Some("one"),Some(message)));
        }
    }
    for (op,expected,ok,buffer,state,session,message) in [
        (Op::Begin,"one",Some(false),Some(0),Some("playing"),Some("one"),Some("音乐数据顺序或格式无效")),
        (Op::Query,"one",Some(false),Some(0),Some("playing"),Some("one"),Some("音乐数据顺序或格式无效")),
        (Op::Stop,"one",Some(false),Some(0),Some("playing"),Some("one"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",None,Some(0),Some("playing"),Some("one"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(true),Some(0),Some("playing"),Some("one"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(false),None,Some("playing"),Some("one"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(false),Some(0),Some("idle"),Some("one"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(false),Some(0),Some("playing"),Some("foreign"),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(false),Some(0),Some("playing"),None,Some("音乐缓冲区已满")),
        (Op::Chunk,"",Some(false),Some(0),Some("playing"),Some(""),Some("音乐缓冲区已满")),
        (Op::Chunk,"one",Some(false),Some(0),Some("playing"),Some("one"),Some("unrecognized or partial outcome")),
        (Op::Chunk,"one",Some(false),Some(0),Some("playing"),Some("one"),None),
    ] {assert!(!rejected_active(op,expected,ok,buffer,state,session,message));}
}
#[test]
fn rejected_active_chunk_keeps_journal_then_once_owned_stop_allows_new_begin() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","old",Op::Begin,&c,true).unwrap();
    let before=m.records.lock().unwrap().clone();let writes=m.writes.load(Ordering::SeqCst);
    let r=g.request_observed(&scope(),"A","old",Op::Chunk,||{c.fetch_add(1,Ordering::SeqCst);Ok(Reply::RejectedActive(()))}).unwrap();
    assert!(matches!(r,Outcome::RejectedActive(())));assert!(g.ensure_known(&scope()).is_ok());
    assert_eq!(*m.records.lock().unwrap(),before);assert_eq!(m.writes.load(Ordering::SeqCst),writes);
    assert!(tx(&g,"A","new",Op::Begin,&c,true).is_err());assert_eq!(c.load(Ordering::SeqCst),2);
    tx(&g,"A","old",Op::Stop,&c,true).unwrap();tx(&g,"A","new",Op::Begin,&c,true).unwrap();assert_eq!(c.load(Ordering::SeqCst),4);
}
#[test]
fn negative_variant_cannot_admit_foreign_owner_or_nonchunk_operations() {
    let (g,_)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,true).unwrap();
    assert!(g.request_observed::<()>(&scope(),"B","one",Op::Chunk,||panic!("foreign owner must not write")).is_err());
    tx(&g,"A","one",Op::Stop,&c,true).unwrap();assert_eq!(c.load(Ordering::SeqCst),2);
    for op in [Op::Begin,Op::Query,Op::Stop,Op::Library] {
        let (g,m)=setup();let c=AtomicUsize::new(0);if op!=Op::Begin {tx(&g,"A","one",Op::Begin,&c,true).unwrap();}
        assert!(g.request_observed(&scope(),"A","one",op,||Ok(Reply::RejectedActive(()))).is_err());
        assert!(g.ensure_known(&scope()).is_err());assert!(m.records.lock().unwrap()[0].unresolved);
    }
}
#[test]
fn real_prior_validation_path_latches_unknown_but_new_chunk_classification_retains_owner() {
    let (old,_)=setup();let (new,_)=setup();let c=AtomicUsize::new(0);
    for g in [&old,&new] {tx(g,"A","one",Op::Begin,&c,true).unwrap();}
    assert!(old.request(&scope(),"A","one",Op::Chunk,||validate_status(Op::Chunk,"one",Some(false),Some(600),Some("playing"),Some("one"))).is_err());
    assert!(tx(&old,"A","one",Op::Stop,&c,true).is_err());
    assert!(rejected_active(Op::Chunk,"one",Some(false),Some(600),Some("playing"),Some("one"),Some("音乐数据顺序或格式无效")));
    assert!(matches!(new.request_observed(&scope(),"A","one",Op::Chunk,||Ok(Reply::RejectedActive(()))).unwrap(),Outcome::RejectedActive(())));
    tx(&new,"A","one",Op::Stop,&c,true).unwrap();assert_eq!(c.load(Ordering::SeqCst),3);
}
#[test]
fn explicit_read_restores_recorded_active_session_after_restart_without_wire_replay() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","recorded",Op::Begin,&c,true).unwrap();drop(g);
    let g=Guard::new(Box::new(Store(m.clone())));assert!(g.ensure_known(&scope()).is_err());let writes=m.writes.load(Ordering::SeqCst);let reads=AtomicUsize::new(0);
    let recovered=g.reconcile_unknown(&scope(),"A",|session|{assert_eq!(session,"recorded");reads.fetch_add(1,Ordering::SeqCst);Ok(known_active_reply(session))}).unwrap();
    assert_eq!(recovered.as_deref(),Some("recorded"));assert_eq!(reads.load(Ordering::SeqCst),1);assert_eq!(c.load(Ordering::SeqCst),1);assert_eq!(m.writes.load(Ordering::SeqCst),writes);assert!(m.records.lock().unwrap()[0].unresolved);
    assert!(tx(&g,"A","new",Op::Begin,&c,true).is_err());tx(&g,"A",recovered.as_deref().unwrap(),Op::Stop,&c,true).unwrap();tx(&g,"A","new",Op::Begin,&c,true).unwrap();assert_eq!(c.load(Ordering::SeqCst),3);
}
#[test]
fn explicit_read_of_same_owner_already_active_is_allowed_without_new_journal_write() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,true).unwrap();
    for state in ["playing","buffering","paused"] {
        assert_eq!(g.reconcile_unknown(&scope(),"A",|s|Ok(recovery_proof(Some(true),Some(0),Some(state),Some(s),None))).unwrap(),Some("one".into()));
    }
    assert_eq!(m.writes.load(Ordering::SeqCst),1);assert_eq!(c.load(Ordering::SeqCst),1);
}
#[test]
fn terminal_query_clears_only_complete_zero_exact_or_empty_session_and_persists_idle() {
    for state in ["idle","ended","interrupted","error"] {for session in ["","one"] {
        let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();
        assert_eq!(g.reconcile_unknown(&scope(),"A",|_|Ok(recovery_proof(Some(true),Some(0),Some(state),Some(session),None))).unwrap(),None);
        assert_eq!(c.load(Ordering::SeqCst),1);assert!(!m.records.lock().unwrap()[0].unresolved);assert_eq!(m.writes.load(Ordering::SeqCst),2);
        let restarted=Guard::new(Box::new(Store(m)));tx(&restarted,"A","new",Op::Begin,&c,true).unwrap();assert_eq!(c.load(Ordering::SeqCst),2);
    }}
}
#[test]
fn exact_negative_query_mismatch_can_prove_empty_terminal_not_general_nack() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();
    assert_eq!(g.reconcile_unknown(&scope(),"A",|_|Ok(recovery_proof(Some(false),Some(0),Some("idle"),Some(""),Some("音乐会话不匹配")))).unwrap(),None);
    assert!(!m.records.lock().unwrap()[0].unresolved);assert_eq!(c.load(Ordering::SeqCst),1);
}
#[test]
fn malformed_foreign_or_negative_live_query_never_promotes_unknown() {
    for proof in [
        RecoveryProof::default(),
        recovery_proof(None,Some(0),Some("idle"),Some(""),None),
        recovery_proof(Some(true),None,Some("idle"),Some(""),None),
        recovery_proof(Some(true),Some(0),None,Some(""),None),
        recovery_proof(Some(true),Some(0),Some("idle"),None,None),
        recovery_proof(Some(true),Some(0),Some("idle"),Some("foreign"),None),
        recovery_proof(Some(true),Some(1),Some("idle"),Some(""),None),
        recovery_proof(Some(true),Some(0),Some("invented"),Some(""),None),
        recovery_proof(Some(false),Some(600),Some("playing"),Some("one"),Some("音乐缓冲区已满")),
        recovery_proof(Some(true),Some(600),Some("playing"),Some("foreign"),None),
        recovery_proof(Some(true),Some(0),Some("playing"),Some(""),None),
        recovery_proof(Some(false),Some(0),Some("idle"),Some(""),Some("other rejection")),
        recovery_proof(Some(false),Some(0),Some("idle"),Some("one"),Some("音乐会话不匹配")),
    ] {
        let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();let before=m.records.lock().unwrap().clone();
        assert!(g.reconcile_unknown(&scope(),"A",|_|Ok(proof)).is_err());assert!(g.ensure_known(&scope()).is_err());
        assert!(tx(&g,"A","one",Op::Stop,&c,true).is_err());assert!(tx(&g,"A","new",Op::Begin,&c,true).is_err());
        assert_eq!(*m.records.lock().unwrap(),before);assert_eq!(c.load(Ordering::SeqCst),1);
    }
}
#[test]
fn read_error_or_changed_binding_keeps_unknown_and_sends_no_stop_or_begin() {
    for failure in ["query timeout","boot changed","connection generation changed","requestId mismatch"] {
        let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,true).unwrap();let before=m.records.lock().unwrap().clone();
        assert!(g.reconcile_unknown(&scope(),"A",|_|Err(failure.into())).is_err());assert!(g.ensure_known(&scope()).is_err());assert_eq!(*m.records.lock().unwrap(),before);
        assert!(tx(&g,"A","one",Op::Stop,&c,true).is_err());assert_eq!(c.load(Ordering::SeqCst),1);
    }
}
#[test]
fn foreign_owner_or_invalid_scope_is_not_queried_or_released() {
    for lost in [false,true] {
        let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,!lost).ok();let before=m.records.lock().unwrap().clone();
        assert!(g.reconcile_unknown(&scope(),"B",|_|panic!("foreign owner must not query")).is_err());
        assert!(g.reconcile_unknown(&Scope{board:"bad/board".into(),boot:scope().boot},"A",|_|panic!("invalid scope must not query")).is_err());
        assert_eq!(*m.records.lock().unwrap(),before);
    }
}
#[test]
fn absent_or_known_idle_scope_is_local_idle_without_reading_or_erasing_other_boot() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();let before=m.records.lock().unwrap().clone();
    assert_eq!(g.reconcile_unknown(&distinct_boot(2),"A",|_|panic!("no lease to reconcile")).unwrap(),None);
    assert_eq!(*m.records.lock().unwrap(),before);assert!(g.ensure_known(&scope()).is_err());
    let (idle,_)=setup();tx(&idle,"A","one",Op::Begin,&c,true).unwrap();tx(&idle,"A","one",Op::Stop,&c,true).unwrap();
    assert_eq!(idle.reconcile_unknown(&scope(),"A",|_|panic!("known idle is already resolved")).unwrap(),None);
}
#[test]
fn failed_idle_persistence_invalidates_guard_and_keeps_unresolved_disk_record() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();let before=m.records.lock().unwrap().clone();m.fail_write.store(true,Ordering::SeqCst);
    assert!(g.reconcile_unknown(&scope(),"A",|_|Ok(known_idle_reply(""))).is_err());assert!(g.ensure_known(&scope()).is_err());assert_eq!(*m.records.lock().unwrap(),before);
    m.fail_write.store(false,Ordering::SeqCst);assert!(g.reconcile_unknown(&scope(),"A",|_|panic!("invalid guard must not query")).is_err());assert!(tx(&g,"A","new",Op::Begin,&c,true).is_err());assert_eq!(c.load(Ordering::SeqCst),1);
}
#[test]
fn restored_active_is_durable_unknown_again_after_restart_until_new_explicit_read() {
    let (g,m)=setup();let c=AtomicUsize::new(0);tx(&g,"A","one",Op::Begin,&c,false).unwrap_err();
    assert_eq!(g.reconcile_unknown(&scope(),"A",|s|Ok(known_active_reply(s))).unwrap(),Some("one".into()));drop(g);
    let restarted=Guard::new(Box::new(Store(m)));assert!(restarted.ensure_known(&scope()).is_err());assert!(tx(&restarted,"A","one",Op::Chunk,&c,true).is_err());assert_eq!(c.load(Ordering::SeqCst),1);
}
