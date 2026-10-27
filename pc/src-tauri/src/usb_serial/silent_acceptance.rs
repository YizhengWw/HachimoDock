// Explicitly gated silent hardware acceptance; never opens the user's todo store.
#[test]
#[ignore = "Explicit approval: sync input-review packages and test isolated todos; no audio/microphone"]
fn p4_input_upgrade_silent_hardware() {
    use base64::Engine;
    use serde_json::{json, Value};
    assert_eq!(std::env::var("PET_SILENT_INPUT_ACCEPTANCE").as_deref(), Ok("1"));
    let root = PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
    let output = PathBuf::from(std::env::var("P4_INPUT_REPORT_DIR").unwrap());
    let board = std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let ids = ["snake-turn", "flappy-bird", "two-key-pong", "tomato-clock", "upcoming-todos", "block-combo"];
    for id in ids {
        let entry = crate::component_library::inspect(&root.join(id)).unwrap();
        assert!(entry.valid, "{id}: {:?}", entry.validation_errors);
    }
    std::fs::create_dir_all(&output).unwrap();
    let usb = UsbSerialManager::new();
    let (tx, rx) = mpsc::channel();
    let (app_tx, app_rx) = mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(), move |topic, payload| {
        if topic == "app/event" {
            let _ = app_tx.send((topic, payload));
        } else if topic == "miniapp/state" || topic.starts_with("debug/") {
            let _ = tx.send((topic, payload));
        }
    }).unwrap();
    struct Close(UsbSerialManager);
    impl Drop for Close { fn drop(&mut self) { self.0.disconnect(); } }
    let _close = Close(usb.clone());
    assert_eq!(usb.status().board_device_id, board);
    let before = usb.query_diagnostics(&board).unwrap();
    assert_eq!(before["runtime"]["firmware"], "0.7.92-p4");
    assert_eq!(before["runtime"]["imageState"], "valid");
    let media = usb.media_request(&board, "media/query", json!({})).unwrap();
    assert!(!matches!(media["state"].as_str(), Some("playing" | "buffering" | "paused")));
    let inventory = usb.query_widget_inventory(&board).unwrap();
    let buttons = usb.query_button_config(&board).unwrap();
    assert_eq!(inventory["complete"], true);
    let inventory_ids = |v: &Value| v["items"].as_array().unwrap().iter()
        .map(|i| i["id"].as_str().unwrap().to_owned()).collect::<HashSet<_>>();
    let mut expected_ids = inventory_ids(&inventory);
    for id in ids { expected_ids.insert(id.into()); }
    assert!(expected_ids.len() <= inventory["maxInstalled"].as_u64().unwrap() as usize);
    let shown = |id: &str| {
        while rx.try_recv().is_ok() {}
        usb.send_to_board(&board, "miniapp/query", &json!({})).unwrap();
        let end = Instant::now() + Duration::from_secs(5);
        while Instant::now() < end {
            if let Ok((topic, p)) = rx.recv_timeout(Duration::from_millis(50)) {
                if topic == "miniapp/state" && p["widgetId"] == id { return p; }
            }
        }
        panic!("no current view: {id}");
    };
    let capture = |name: &str| {
        while rx.try_recv().is_ok() {}
        usb.send_to_board(&board, "debug/screenshot", &json!({"fullResolution":true})).unwrap();
        let end = Instant::now() + Duration::from_secs(30);
        let mut identity = Value::Null;
        let mut data = Vec::new();
        let mut index = 0;
        while Instant::now() < end {
            let Ok((topic, p)) = rx.recv_timeout(Duration::from_millis(50)) else { continue; };
            assert_ne!(topic, "debug/screenshot_error", "{p}");
            if topic == "debug/screenshot_begin" {
                assert_eq!(p["width"], 640); assert_eq!(p["height"], 480);
                assert_eq!(p["format"], "rgb565le"); identity = p["id"].clone();
            } else if topic == "debug/screenshot_chunk" && p["id"] == identity {
                assert_eq!(p["index"], index); index += 1;
                data.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());
            } else if topic == "debug/screenshot_end" && p["id"] == identity {
                assert_eq!(data.len(), 640 * 480 * 2); assert_eq!(p["chunks"], index);
                let mut hash = 2166136261u32;
                let mut image = image::RgbImage::new(640, 480);
                for (at, bytes) in data.chunks_exact(2).enumerate() {
                    let px = u16::from_le_bytes([bytes[0], bytes[1]]);
                    hash = (hash ^ px as u32).wrapping_mul(16777619);
                    let (r,g,b) = ((px>>11) as u8, ((px>>5)&63) as u8, (px&31) as u8);
                    image.put_pixel(at as u32 % 640, at as u32 / 640,
                        image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));
                }
                assert_eq!(p["checksum"], format!("{hash:08x}"));
                image.save(output.join(format!("{name}.png"))).unwrap(); return;
            }
        }
        panic!("screenshot timed out: {name}");
    };
    let storage = tempfile::tempdir().unwrap();
    let todo_path = storage.path().join("todos.json");
    let mut store = crate::todos::isolated_store(todo_path.clone());
    for n in 1..=7 {
        store(crate::todos::Input { operation: "add".into(), id: None, revision: None,
            title: Some(format!("验收测试{n}：确认设备上的事项详情可以完整阅读，并在确认后完成；这些只是临时测试数据，不会修改你的真实待办。")),
            due_at: None, filter: None, query: None }).unwrap();
    }
    let mut reading = crate::component_todos::Reading::default();
    let mut seen = HashSet::new();
    let mut delivered = Vec::new();
    let mut pump = |milliseconds: u64| {
        let end = Instant::now() + Duration::from_millis(milliseconds);
        while Instant::now() < end {
            let Ok((topic, p)) = app_rx.recv_timeout(Duration::from_millis(40)) else { continue; };
            if topic != "app/event" || p["componentId"] != "upcoming-todos" || p["visible"] != true { continue; }
            assert_eq!(p["boardDeviceId"], board);
            let identity = format!("{}:{}", p["instance"], p["sequence"]);
            if !seen.insert(identity) { continue; }
            let action = p["action"].as_str().unwrap();
            let op = action.strip_prefix("host.").unwrap_or(action);
            let values = crate::component_todos::test_execute(&mut reading, op,
                &json!({"confirm":p["confirmed"] == true, "context":p["context"]}), &todo_path).unwrap();
            let actions: Vec<_> = reading.ui["operations"].as_array().unwrap().iter()
                .map(|v| format!("host.{}", v.as_str().unwrap())).collect();
            usb.send_app_snapshot(&board, &json!({"componentId":"upcoming-todos",
                "instance":p["instance"], "sequence":p["sequence"], "values":values,
                "ui":{"mode":reading.ui["mode"], "actions":actions, "context":reading.ui["context"]}})).unwrap();
            if action != "status" { delivered.push(action.to_string()); }
        }
        delivered.clone()
    };
    let event = |action: &str| usb.send_to_board(&board, "miniapp/event", &json!({"action":action})).unwrap();
    for id in ids {
        usb.install_widget_clawpkg(id, &root.join(id), &HashMap::new(), |f,n,_| eprintln!("SYNC {id} {f}/{n}")).unwrap();
        usb.send_to_board(&board, "control/screen-page", &json!({"page":"app"})).unwrap();
        if id == "upcoming-todos" {
            pump(1800); assert_eq!(shown(id)["metricValue"], "待完成 · 7"); capture("todos-list"); pump(500);
            event("host.next"); pump(500); assert_eq!(shown(id)["badge"], "2/2");
            event("host.row_a"); pump(500); assert_eq!(shown(id)["metricUnit"], "标记完成"); capture("todos-detail"); pump(500);
            event("host.toggle"); let opened = pump(300); assert!(!opened.contains(&"host.toggle".into())); capture("todos-confirm"); pump(500);
            event("input.cancel"); let cancelled = pump(300); assert!(!cancelled.contains(&"host.toggle".into()));
            event("host.toggle"); pump(200); event("input.confirm");
            assert_eq!(pump(500).iter().filter(|a| *a == "host.toggle").count(), 1);
            assert_eq!(shown(id)["metricUnit"], "撤销完成"); capture("todos-completed"); pump(500);
            event("host.toggle"); pump(200); event("input.confirm");
            assert_eq!(pump(500).iter().filter(|a| *a == "host.toggle").count(), 2);
            assert_eq!(shown(id)["metricUnit"], "标记完成");
        } else {
            thread::sleep(Duration::from_millis(600));
            assert_eq!(shown(id)["widgetId"], id); capture(id);
            // Do not fabricate game scores, timer completions or saved progress.
            // Physical controls are tested by the user after this process closes.
        }
        usb.send_to_board(&board, "control/screen-page", &json!({"page":"main"})).unwrap();
        let d = usb.query_diagnostics(&board).unwrap();
        assert_eq!(d["bootCount"], before["bootCount"]);
        assert_eq!(d["faultResetCount"], before["faultResetCount"]);
        assert_eq!(d["runtime"]["screenPage"], "main");
        eprintln!("PASS {id}: installed, native rendered, exit without reset");
    }
    let after = usb.query_diagnostics(&board).unwrap();
    assert_eq!(inventory_ids(&usb.query_widget_inventory(&board).unwrap()), expected_ids);
    assert_eq!(usb.query_button_config(&board).unwrap()["config"], buttons["config"]);
    assert_eq!(before["storage"]["activeAppearanceSlot"], after["storage"]["activeAppearanceSlot"]);
    assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    std::fs::write(output.join("report.json"), serde_json::to_vec_pretty(&json!({
        "firmware":"0.7.92-p4", "imageState":"valid", "components":ids,
        "nativeScreenshots":true, "temporaryTodoCompletionAndUndo":true,
        "realUserTodosAccessed":false, "audioOrMicrophoneUsed":false,
        "globalButtonsUnchanged":true, "catalogIdsPreserved":true,
        "physicalTouchAndJoystick":"requires_user_acceptance"})).unwrap()).unwrap();
    usb.send_to_board(&board, "control/screen-page", &json!({"page":"app"})).unwrap();
    eprintln!("PASS silent sync and isolated todo roundtrip; blocks ready for manual input acceptance");
}
