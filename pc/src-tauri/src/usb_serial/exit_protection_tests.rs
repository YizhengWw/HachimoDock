// Exit checks exercise real manager operations and deferred protocol receipts.
mod exit_protection_tests {
    use super::*;

    struct DeferredWire {
        pending: Vec<u8>,
        requests: mpsc::Sender<serde_json::Value>,
    }
    impl Write for DeferredWire {
        fn write(&mut self, bytes: &[u8]) -> std::io::Result<usize> {
            self.pending.extend_from_slice(bytes);
            while let Some(end) = self.pending.iter().position(|b| *b == b'\n') {
                let line: Vec<_> = self.pending.drain(..=end).collect();
                self.requests.send(serde_json::from_slice(&line).unwrap()).unwrap();
            }
            Ok(bytes.len())
        }
        fn flush(&mut self) -> std::io::Result<()> { Ok(()) }
    }
    fn manager() -> (UsbSerialManager, mpsc::Receiver<serde_json::Value>) {
        let (manager, _) = firmware_once_test_manager(Vec::new());
        let (tx, rx) = mpsc::channel();
        manager.connection.lock().unwrap().as_mut().unwrap().writer =
            Box::new(DeferredWire { pending: Vec::new(), requests: tx });
        (manager, rx)
    }

    #[test]
    fn exit_protection_read_only_query_does_not_claim_firmware_write() {
        let (manager, requests) = manager();
        let worker_manager = manager.clone();
        let worker = thread::spawn(move || worker_manager.query_button_config("p4-board-a"));
        let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
        assert_eq!(request["topic"], "input/config-query");
        let transport_busy = manager.asset_transfer_guard.try_lock().is_err();
        let exit_blocked = manager.transfer_in_progress();
        resolve_device_response(&manager.device_response_waiters, "input/config-state",
            &serde_json::json!({"requestId":request["payload"]["requestId"],
                "boardDeviceId":"p4-board-a","ok":true,"bindings":[]}));
        worker.join().unwrap().unwrap();
        assert!(transport_busy, "queries must still serialize with bulk writes");
        assert!(!exit_blocked, "a pending read-only reply must not claim a flash write");
    }

    #[test]
    fn exit_protection_inventory_and_media_queries_remain_read_only() {
        for inventory in [true, false] {
            let (manager, requests) = manager();
            manager.connection.lock().unwrap().as_mut().unwrap().capabilities["widgetInventory"] = true.into();
            let copy = manager.clone();
            let worker = thread::spawn(move || if inventory {
                copy.query_widget_inventory("p4-board-a")
            } else {
                copy.media_request("p4-board-a", "media/query", serde_json::json!({}))
            });
            let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
            assert_eq!(request["topic"], if inventory {"widget/list"} else {"media/query"});
            let busy = manager.asset_transfer_guard.try_lock().is_err();
            let blocked = manager.transfer_in_progress();
            resolve_device_response(&manager.device_response_waiters,
                if inventory {"widget/inventory"} else {"media/status"},
                &serde_json::json!({"requestId":request["payload"]["requestId"],
                    "boardDeviceId":"p4-board-a","ok":true,"widgets":[],"state":"idle"}));
            worker.join().unwrap().unwrap();
            assert!(busy);
            assert!(!blocked, "{request}");
        }
    }

    #[test]
    fn exit_protection_firmware_covers_begin_chunks_and_commit_waits() {
        let (manager, requests) = manager();
        let directory = tempfile::tempdir().unwrap();
        let path = directory.path().join("firmware.bin");
        let mut image = app_image_fixture(P4_FIRMWARE_PROJECT_NAME, "0.7.103-p4-test");
        image[12..14].copy_from_slice(&18u16.to_le_bytes());
        image[15..17].copy_from_slice(&103u16.to_le_bytes());
        image[17..19].copy_from_slice(&103u16.to_le_bytes());
        std::fs::write(&path, image).unwrap();
        let copy = manager.clone();
        let worker = thread::spawn(move || copy.update_firmware(&path, "p4-board-a",
            |_, _, _| assert!(copy.transfer_in_progress(), "packet gaps retain protection"),
            || panic!("rejected commit cannot reconnect")));
        let mut received = 0u64;
        let mut phases = Vec::new();
        loop {
            let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
            let topic = request["topic"].as_str().unwrap();
            let p = &request["payload"];
            if topic == "firmware/query" {
                assert!(!manager.transfer_in_progress(), "preflight is read-only");
                resolve_firmware_status(&manager.firmware_status_waiters, "firmware/status",
                    &serde_json::json!({"requestId":p["requestId"],"supported":true,"active":false,
                        "runningPartition":{"label":"ota_0"},"nextPartition":{"label":"ota_1","size":2621440}}));
            } else if topic == "diagnostics/query" {
                assert!(!manager.transfer_in_progress());
                resolve_device_response(&manager.device_response_waiters, "diagnostics/status",
                    &serde_json::json!({"requestId":p["requestId"],"boardDeviceId":"p4-board-a",
                        "bootCount":1,"runtime":{"firmware":"0.7.103-p4-old","runningPartition":"ota_0"}}));
            } else {
                assert!(manager.transfer_in_progress(), "{topic} must protect the ACK wait");
                let phase = topic.strip_prefix("firmware/").unwrap();
                phases.push(phase.to_owned());
                if phase == "chunk" { received += p["decodedSize"].as_u64().unwrap(); }
                resolve_firmware_ack(&manager.firmware_ack_waiters, "firmware/ack",
                    &serde_json::json!({"transferId":p["transferId"],"phase":phase,"ok":phase!="commit",
                        "targetPartition":"ota_1","seq":p["seq"],"nextSequence":if phase=="begin" {0} else {1},
                        "receivedBytes":received,"chunkBytes":p["decodedSize"],"duplicate":false,
                        "error":"test-only rejected commit"}));
                if phase == "commit" { break; }
            }
        }
        assert!(worker.join().unwrap().unwrap_err().contains("rejected commit"));
        assert_eq!(phases, ["begin", "chunk", "commit"]);
        assert!(!manager.transfer_in_progress(), "failed transactions release their exit lease");
    }

    #[test]
    fn exit_protection_widget_covers_success_and_error_until_commit_receipt() {
        for commit_ok in [true, false] {
            let (manager, requests) = manager();
            let directory = super::super::component_sync_safety_tests::minimal_widget();
            let id = super::super::component_sync_safety_tests::WIDGET;
            let copy = manager.clone();
            let worker = thread::spawn(move || copy.install_widget_clawpkg_on_board(
                "p4-board-a", id, directory.path(), &HashMap::new(),
                |_, _, _| assert!(copy.transfer_in_progress())));
            let mut chunks = 0;
            loop {
                let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
                assert!(manager.transfer_in_progress(), "{request}");
                let phase = request["topic"].as_str().unwrap().strip_prefix("widget/").unwrap();
                let p = &request["payload"];
                if phase == "chunk" { chunks += 1; }
                resolve_widget_ack(&manager.widget_ack_waiters, "widget-install-ack",
                    &serde_json::json!({"transferId":p["transferId"],"phase":phase,
                        "path":p["path"],"index":p["index"],"ok":phase!="commit" || commit_ok,
                        "msg":"test-only rejected commit"}));
                if phase == "commit" { break; }
            }
            assert_eq!(worker.join().unwrap().is_ok(), commit_ok);
            assert!(chunks > 0, "real resource bytes must pass through the installer");
            assert!(!manager.transfer_in_progress());
        }
    }

    #[test]
    fn exit_protection_appearance_and_nested_writes_release_on_error_or_unwind() {
        let (manager, _) = manager();
        let error = manager.with_appearance_attempt("p4-board-a", || {
            assert!(!manager.transfer_in_progress(), "appearance preflight/cache reads are not writes");
            let attempt = manager.active_appearance_attempt()?.unwrap();
            attempt.before_mutation("asset/begin", "exit-test-appearance")?;
            assert!(manager.transfer_in_progress());
            let nested = manager.protect_device_write();
            drop(nested);
            assert!(manager.transfer_in_progress(), "ending one scope cannot clear another");
            Err::<(), _>("test-only failure".into())
        });
        assert!(error.is_err());
        assert!(!manager.transfer_in_progress());
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = manager.protect_device_write();
            panic!("test-only unwinding");
        }));
        assert!(!manager.transfer_in_progress());
    }

    #[test]
    fn exit_protection_poisoned_transport_is_not_an_active_write() {
        let (manager, _) = manager();
        let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            let _guard = manager.asset_transfer_guard.lock().unwrap();
            panic!("test-only poisoned transport");
        }));
        assert!(manager.asset_transfer_guard.try_lock().is_err());
        assert!(!manager.transfer_in_progress());
    }

    #[test]
    fn exit_protection_widget_delete_retains_guard_until_receipt() {
        let (manager, requests) = manager();
        {
            let mut slot = manager.connection.lock().unwrap();
            let caps = &mut slot.as_mut().unwrap().capabilities;
            caps["widgetDelete"] = true.into();
            caps["widgetDeletePreserveSave"] = true.into();
        }
        let copy = manager.clone();
        let worker = thread::spawn(move || copy.remove_widget("p4-board-a", "exit-test-widget"));
        let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
        assert_eq!(request["topic"], "widget/delete");
        assert!(manager.transfer_in_progress());
        resolve_widget_ack(&manager.widget_ack_waiters, "widget-install-ack",
            &serde_json::json!({"transferId":request["payload"]["transferId"],"phase":"delete","ok":true}));
        worker.join().unwrap().unwrap();
        assert!(!manager.transfer_in_progress());
    }

    #[test]
    fn exit_protection_volume_distinguishes_query_from_persistent_set() {
        for volume in [None, Some(42)] {
            let (manager, requests) = manager();
            manager.connection.lock().unwrap().as_mut().unwrap().capabilities["audioMasterVolume"] =
                "p4-master-volume-v1".into();
            let copy = manager.clone();
            let worker = thread::spawn(move || copy.device_volume("p4-board-a", volume));
            let request = requests.recv_timeout(Duration::from_secs(2)).unwrap();
            assert_eq!(request["topic"], "audio/volume");
            assert!(manager.asset_transfer_guard.try_lock().is_err());
            assert_eq!(manager.transfer_in_progress(), volume.is_some());
            resolve_device_response(&manager.device_response_waiters, "audio/volume-state",
                &serde_json::json!({"requestId":request["payload"]["requestId"],"boardDeviceId":"p4-board-a",
                    "engine":"p4-master-volume-v1","ok":true,"volume":42,"persisted":true}));
            worker.join().unwrap().unwrap();
            assert!(!manager.transfer_in_progress());
        }
    }
}
