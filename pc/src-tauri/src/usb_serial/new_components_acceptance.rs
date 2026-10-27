// Opt-in acceptance of only the three newly authored packages. Never runs in CI.
#[test]
#[ignore = "Explicit notice required: verified public news on device, current-pet TTS, stop/switch/exit; no recording"]
fn p4_news_reading_audio_hardware(){
    use serde_json::{json,Value};
    assert_eq!(std::env::var("PET_NEWS_AUDIO_TEST").as_deref(),Ok("1"));
    let layout_only=std::env::var("PET_NEWS_LAYOUT_ONLY").as_deref()==Ok("1");
    let package=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap()).join("daily-news");
    let output=PathBuf::from(std::env::var("P4_NEW_REPORT_DIR").unwrap());
    let config=PathBuf::from(std::env::var("P4_SERVICE_CONFIG").unwrap());
    crate::voice_chat_settings::configure_storage_dir(config.clone()).unwrap();
    crate::volcengine_asr::configure_storage_dir(config.clone()).unwrap(); // settings only, no capture
    crate::llm_network::configure_storage_dir(config.clone()).unwrap();
    let d:Value=serde_json::from_slice(&std::fs::read(package.join("pc/application.json")).unwrap()).unwrap();
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();let(stx,srx)=mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,p|{
        crate::component_audio::observe(&topic,&p);
        if topic=="app/event"{if let Some(id)=p["componentId"].as_str(){crate::component_audio::heartbeat(id,p["visible"]==true);}let _=tx.send(p);}
        else if topic.starts_with("debug/"){let _=stx.send((topic,p));}
    }).unwrap();assert_eq!(usb.status().board_device_id,board);
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){crate::component_news::leave("daily-news");crate::component_audio::stop(Some("daily-news"));let b=self.0.status().board_device_id;let _=self.0.send_to_board(&b,"control/screen-page",&serde_json::json!({"page":"main"}));thread::sleep(Duration::from_millis(1200));self.0.disconnect();let _=crate::component_voice::component_speech_set_pet_voice(None);}}
    let _close=Close(usb.clone());
    // Silent layout verification must not require a saved pet voice or TTS key.
    // Audio acceptance still enforces the exact current-pet identity below.
    let voice=if layout_only{Value::Null}else{
    let slots=usb.query_p4_appearance_slots("news-current-pet").unwrap();let active=slots.slots.iter().find(|s|Some(s.slot)==slots.active_slot).unwrap();
    eprintln!("Current appearance resource identity: {active:?}");
    let mut selected=None;
    for id in ["builtin-terrier","codex-apex-nessie"]{
        let manifest=if id=="builtin-terrier"{let factory:Value=serde_json::from_str(include_str!("../../../../firmware/factory-config.json")).unwrap();json!({"packId":factory["appearance"]["packId"]})}else{let p=config.join("custom-appearances").join(id).join("p4-ready/v9-640x480-15fps-225f-h264-crf27/p4/manifest.json");match std::fs::read(p){Ok(raw)=>serde_json::from_slice(&raw).unwrap(),Err(_)=>continue}};
        if manifest["packId"]==active.pack_id{selected=Some(id);break;}
    }
    // A replacement board may retain an older version of the same pet. An
    // explicit human confirmation is accepted only for that exact observed
    // pack, and only in this opt-in test; production identity is not changed.
    let confirmed=std::env::var("P4_CONFIRMED_APPEARANCE_ID").ok();
    if let Some(id)=confirmed.as_deref(){
        assert!(matches!(id,"builtin-terrier"|"codex-apex-nessie"));
        assert_eq!(std::env::var("P4_CONFIRMED_APPEARANCE_PACK").as_deref(),Ok(active.pack_id.as_str()),"confirmed board appearance changed");
        selected=Some(id);
    }
    let appearance=selected.expect("current pet must match a known or explicitly confirmed pack; never guess voice");
    let persona=crate::component_voice::acceptance_persona(&config,appearance);let v=&persona["voice"];
    json!({"boardDeviceId":board,"appearanceId":appearance,"speaker":v["speaker"],"cloneSpeakerId":v["clone_speaker_id"].as_str().unwrap_or(""),"speed":v["speed"].as_f64().unwrap_or(1.0)})};
    let appearance=voice["appearanceId"].as_str().unwrap_or("silent-layout-only");
    let volume=if layout_only{0}else{(crate::voice_chat_settings::realtime_volume().unwrap().clamp(0.0,0.35)*100.0)as u32};assert!(layout_only||volume>0);
    let before=usb.query_diagnostics(&board).unwrap();let buttons=usb.query_button_config(&board).unwrap();
    assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    let existing_only=std::env::var("PET_NEWS_EXISTING_ONLY").as_deref()==Ok("1");
    if existing_only {
        assert!(layout_only||std::env::var("PET_NEWS_CACHE_DIR").is_ok(),"audio existing-only mode requires prepared cache");
        let inventory=usb.query_widget_inventory(&board).unwrap();
        assert_eq!(inventory["complete"],true);assert_eq!(inventory["activeWidgetId"],"daily-news");
        let item=inventory["items"].as_array().unwrap().iter().find(|i|i["id"]=="daily-news").unwrap();
        assert_eq!(item["resourceState"],"ready");assert!(item["imageCount"].as_u64().unwrap()>0);
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
    } else {
        usb.install_widget_clawpkg_on_board(&board,"daily-news",&package,&HashMap::new(),|_,_,_|{}).unwrap();
    }
    crate::component_news::seed_verified_acceptance_report("daily-news",&PathBuf::from(std::env::var("PET_NEWS_REPORT").unwrap()),d["content"].as_array().unwrap());
    let declaration:crate::app_services::Declaration=serde_json::from_value(d.clone()).unwrap();
    let prepared=crate::component_news::request_with_surface("daily-news",&declaration,"status",None,false,&Value::Null).unwrap();
    let expected:String=prepared["knowledge"]["playlist"].as_array().unwrap().iter().map(|item|crate::knowledge_narration::speech_parts(item["title"].as_str().unwrap(),item["text"].as_str().unwrap()).concat()).collect();
    if let Ok(path)=std::env::var("PET_NEWS_CACHE_DIR"){crate::component_audio::configure_speech_cache(path.into());}
    if existing_only {
        let declaration:crate::app_services::Declaration=serde_json::from_value(d.clone()).unwrap();
        crate::component_news::prefetch("daily-news",&declaration).unwrap();
        let cached=crate::component_news::request_with_surface("daily-news",&declaration,"status",None,false,&Value::Null).unwrap();
        assert_eq!(cached["news"]["generating"],false);assert_eq!(cached["permissions"]["autoRead"],false);
        assert_eq!(cached["news"]["items"].as_array().unwrap().len(),9);
        assert!(!crate::component_audio::playing("daily-news"));
    }
    let storage=tempfile::tempdir().unwrap();let mut narrator=crate::knowledge_narration::Narration::default();let mut seen=HashSet::new();let mut visible=false;let mut epoch=0;let mut starts=0;let mut last=Value::Null;let mut spoken=String::new();let mut shown=HashSet::new();let mut completed=false;
    let mut pump=|ms:u64|{
        let until=Instant::now()+Duration::from_millis(ms);
        while Instant::now()<until{
            let Ok(p)=rx.recv_timeout(Duration::from_millis(30))else{continue;};if p["componentId"]!="daily-news"{continue;}
            let showing=p["visible"]==true;let entering=showing&&!visible;if showing!=visible{epoch+=1;visible=showing;}
            if !visible{crate::component_news::leave("daily-news");narrator.stop();continue;}
            if !layout_only{crate::component_voice::component_speech_set_pet_voice(Some(serde_json::from_value(voice.clone()).unwrap())).unwrap();}
            let action=p["action"].as_str().unwrap_or("status");if action!="status"&&!seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
            let input=if action=="status"{json!({"operation":"status","_deviceEpoch":epoch,"_entered":entering})}else{json!({"operation":"action","action":action,"_deviceEpoch":epoch})};
            let result=crate::component_app::test_request(&package,storage.path(),input,&usb).unwrap();
            let audio=crate::component_audio::status("daily-news");assert_ne!(audio["phase"],"error","{}",audio["error"]);
            if let Some(source)=if layout_only{None}else{narrator.next_source(epoch,&result,audio["phase"].as_str().unwrap_or("idle"))}{
                if let crate::component_audio::Source::SpeechQueue{items,..}=&source{for item in items{spoken.push_str(&item.text);}}
                crate::component_audio::start("daily-news",&usb,source,volume).unwrap();starts+=1;eprintln!("News continuous queue {starts}");
            }
            if narrator.completed(){completed=true;crate::component_news::finished("daily-news");}
            // Match production: PCM has priority; a congested UI snapshot is
            // replaced by the next heartbeat, not treated as an audio failure.
            if let Err(error)=usb.send_app_snapshot(&board,&json!({"componentId":"daily-news","instance":p["instance"],"sequence":p["sequence"],"values":result["values"],"ui":result["ui"]})){
                assert_eq!(error,"设备传输繁忙，等待下一次状态更新");
            }else if audio["phase"]=="playing"{
                let selected=result["news"]["selected"].as_u64().unwrap();
                if shown.insert(selected){eprintln!("AUDIBLE + SCREEN item {}: {}",selected+1,result["values"]["headline"]);}
            }
            last=result;
        }(starts,last.clone(),crate::component_audio::status("daily-news")["phase"].clone(),completed,spoken.clone(),shown.clone())
    };
    let entered_at=Instant::now();let mut entry_latency_ms=0;
    usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
    if !layout_only{
    let until=Instant::now()+Duration::from_secs(90);loop{let s=pump(150);if s.2=="playing"{entry_latency_ms=entered_at.elapsed().as_millis();break;}assert!(Instant::now()<until,"no news narration");}
    let until=Instant::now()+Duration::from_secs(600);let full=loop{let s=pump(250);if s.3{break s;}assert!(Instant::now()<until,"nine-item narration did not finish");};
    assert_eq!(full.1["news"]["items"].as_array().unwrap().len(),9);
    assert_eq!(full.4,expected);assert_eq!(full.0,1,"must use one continuous queue");
    assert_eq!(full.5,(0..9).collect(),"screen must follow all nine audible items");
    assert_eq!(full.1["news"]["selected"],8);
    let gap=crate::component_audio::status("daily-news")["maxSpeechGapMs"].as_u64().unwrap();
    eprintln!("Maximum TTS preparation gap: {gap} ms");assert!(gap<=1000,"continuous reading starved for {gap} ms");
    assert_eq!(pump(1500).0,full.0,"completed briefing restarted");
    }else{
        let state=pump(1500);assert_eq!(state.0,0);assert_eq!(state.1["news"]["items"].as_array().unwrap().len(),9);
        assert!(!state.1["values"]["note"].as_str().unwrap().is_empty());
        for index in 1..9{
            usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.next"})).unwrap();
            let state=pump(700);assert_eq!(state.0,0);assert_eq!(state.1["news"]["selected"],index);
            assert_eq!(state.1["values"]["headline"],state.1["news"]["items"][index]["title"]);
            assert!(!state.1["values"]["note"].as_str().unwrap().is_empty());
        }
    }
    // A full-resolution debug frame shares the serial output queue. Capture
    // only after narration drains, never compete with real-time PCM ACKs.
    use base64::Engine;let mut captured=false;
    // Diagnostic reads alone may be repeated once; never repeat an installation
    // or billable generation. Reject partial frames rather than rendering them.
    for attempt in 0..2{
        while srx.try_recv().is_ok(){}usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
        let until=Instant::now()+Duration::from_secs(30);let mut identity=Value::Null;let mut bytes=Vec::new();let mut index=0;let mut valid=true;
        while Instant::now()<until{
            pump(20);let Ok((t,p))=srx.recv_timeout(Duration::from_millis(20))else{continue;};
            if t=="debug/screenshot_error"{break;}
            if t=="debug/screenshot_begin"{identity=p["id"].clone();valid=p["width"]==640&&p["height"]==480&&p["format"]=="rgb565le";}
            else if t=="debug/screenshot_chunk"&&p["id"]==identity{
                valid&=p["index"]==index;index+=1;
                match base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap_or("")){Ok(chunk)=>bytes.extend(chunk),Err(_)=>valid=false}
                if bytes.len()>640*480*2{break;}
            }else if t=="debug/screenshot_end"&&p["id"]==identity{
                let hash=bytes.chunks_exact(2).fold(2166136261u32,|hash,b|(hash^u16::from_le_bytes([b[0],b[1]])as u32).wrapping_mul(16777619));
                if valid&&bytes.len()==640*480*2&&p["chunks"]==index&&p["checksum"]==format!("{hash:08x}"){
                    let mut im=image::RgbImage::new(640,480);
                    for(n,b)in bytes.chunks_exact(2).enumerate(){let v=u16::from_le_bytes([b[0],b[1]]);let(r,g,b)=((v>>11)as u8,((v>>5)&63)as u8,(v&31)as u8);im.put_pixel(n as u32%640,n as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));}
                    im.save(output.join("news-device.png")).unwrap();captured=true;
                }else{eprintln!("Rejected screenshot attempt {}: bytes={}, chunks={index}, integrity=false",attempt+1,bytes.len());}
                break;
            }
        }
        if captured{break;}
    }
    assert!(captured,"no complete checksummed news screenshot in two read attempts");
    if layout_only{
        assert!(!crate::component_audio::playing("daily-news"));
        let after=usb.query_diagnostics(&board).unwrap();
        assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
        std::fs::write(output.join("news-layout.json"),serde_json::to_vec_pretty(&json!({"verifiedPublicBatch":true,"nineItemsDisplayed":true,"screenshot":true,"audioStarted":false,"microphoneStarted":false,"noReset":true,"buttonsPreserved":true})).unwrap()).unwrap();
        eprintln!("PASS nine news items and layout screenshot; no audio started");return;
    }
    // Narration counters remain available through the shared pump state.
    let full=pump(10);let gap=crate::component_audio::status("daily-news")["maxSpeechGapMs"].as_u64().unwrap();
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.previous"})).unwrap();assert_eq!(pump(800).1["news"]["selected"],7);
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.toggle"})).unwrap();let until=Instant::now()+Duration::from_secs(45);loop{let s=pump(150);if s.2=="playing"{break;}assert!(Instant::now()<until);}
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.toggle"})).unwrap();let stopped=pump(1500);assert_eq!(stopped.1["permissions"]["autoRead"],false);let count=stopped.0;assert_eq!(pump(2000).0,count,"poll restarted stopped narration");
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.next"})).unwrap();assert_eq!(pump(800).1["news"]["selected"],8);
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.toggle"})).unwrap();let until=Instant::now()+Duration::from_secs(45);loop{let s=pump(150);if s.2=="playing"{break;}assert!(Instant::now()<until);}
    usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump(2500);assert!(!crate::component_audio::playing("daily-news"));
    assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
    std::fs::write(output.join("news-audio-continuous.json"),serde_json::to_vec_pretty(&json!({"verifiedPublicBatch":true,"currentPet":appearance,"entryAudio":true,"entryLatencyMs":entry_latency_ms,"existingPackage":existing_only,"allNineNarrated":true,"continuousQueues":full.0,"screenItems":full.5,"maxPreparationGapMs":gap,"spokenCharacters":full.4.chars().count(),"completionDoesNotRestart":true,"stopDoesNotRestart":true,"nextItem":true,"exitStops":true,"microphone":false,"voiceQuality":"requires user listening confirmation"})).unwrap()).unwrap();
}

#[test]
#[ignore = "Explicit approval: sync current 26 builtins and delete only two retired components; silent"]
fn p4_builtin_26_sync_hardware(){
    use serde_json::{json,Value};
    use base64::Engine;
    assert_eq!(std::env::var("P4_BUILTIN_26_TEST").as_deref(),Ok("1"));
    let packages=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
    let output=PathBuf::from(std::env::var("P4_NEW_REPORT_DIR").unwrap());
    std::fs::create_dir_all(&output).unwrap();
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let config:Value=serde_json::from_str(include_str!("../../../../firmware/factory-config.json")).unwrap();
    let ids:Vec<&str>=config["components"]["ids"].as_array().unwrap().iter().map(|v|v.as_str().unwrap()).collect();assert_eq!(ids.len(),26);
    for id in &ids{let e=crate::component_library::inspect(&packages.join(id)).unwrap();assert!(e.valid,"{id}: {:?}",e.validation_errors);}
    // Recovery packages are outside the live library. Preserve device saves
    // explicitly; private host records are not touched.
    let backup=PathBuf::from(std::env::var("P4_RETIRED_BACKUP").unwrap());
    for id in ["hachimi-agent","hachimi-desk"]{assert!(backup.join(id).is_dir(),"missing recovery package");}
    let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|t,p|{if t.starts_with("debug/"){let _=tx.send((t,p));}}).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());assert_eq!(usb.status().board_device_id,board);
    let before=usb.query_diagnostics(&board).unwrap();assert_eq!(before["runtime"]["firmware"],"0.7.99-p4");assert_eq!(before["runtime"]["imageState"],"valid");
    let buttons=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();
    assert_eq!(inventory["complete"],true);
    assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    if !output.join("before.json").exists(){std::fs::write(output.join("before.json"),serde_json::to_vec_pretty(&json!({"diagnostics":before,"inventory":inventory,"buttons":buttons})).unwrap()).unwrap();}
    std::fs::write(output.join("capabilities.json"),serde_json::to_vec_pretty(&usb.status().capabilities).unwrap()).unwrap();
    for id in ["hachimi-agent","hachimi-desk"]{usb.remove_widget_with_save_policy(&board,id,true).unwrap();eprintln!("REMOVED {id}; recovery package and device save preserved");}
    let resume=std::env::var("P4_RESUME_COMPONENT").ok();
    let start=resume.as_ref().map(|r|ids.iter().position(|i|i==r).expect("unknown resume component")).unwrap_or(0);
    for id in &ids[start..]{
        usb.install_widget_clawpkg_on_board(&board,id,&packages.join(id),&HashMap::new(),|_,_,_|{}).unwrap();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        let inv=usb.query_widget_inventory(&board).unwrap();let manifest:Value=serde_json::from_slice(&std::fs::read(packages.join(id).join("component.json")).unwrap()).unwrap();
        let entry=inv["items"].as_array().unwrap().iter().find(|i|i["id"]==*id).unwrap();assert_eq!(entry["name"],manifest["name"],"title not updated for {id}");
        eprintln!("SYNC VERIFIED {id}: {}",entry["name"]);
    }
    let after=usb.query_diagnostics(&board).unwrap();let inv=usb.query_widget_inventory(&board).unwrap();
    let actual:Vec<_>=inv["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap()).collect();assert_eq!(actual,ids);
    for item in inv["items"].as_array().unwrap(){let manifest:Value=serde_json::from_slice(&std::fs::read(packages.join(item["id"].as_str().unwrap()).join("component.json")).unwrap()).unwrap();assert_eq!(item["name"],manifest["name"]);}
    assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
    assert_eq!(before["storage"]["activeAppearanceSlot"],after["storage"]["activeAppearanceSlot"]);assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
    usb.send_to_board(&board,"control/screen-page",&json!({"page":"components"})).unwrap();thread::sleep(Duration::from_millis(500));
    while rx.try_recv().is_ok(){}usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
    let until=Instant::now()+Duration::from_secs(30);let mut identity=Value::Null;let mut bytes=Vec::new();let mut index=0;
    loop{assert!(Instant::now()<until,"screenshot timeout");let Ok((t,p))=rx.recv_timeout(Duration::from_millis(50))else{continue;};assert_ne!(t,"debug/screenshot_error","{p}");
        if t=="debug/screenshot_begin"{assert_eq!(p["width"],640);assert_eq!(p["height"],480);assert_eq!(p["format"],"rgb565le");identity=p["id"].clone();}
        else if t=="debug/screenshot_chunk"&&p["id"]==identity{assert_eq!(p["index"],index);index+=1;bytes.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());}
        else if t=="debug/screenshot_end"&&p["id"]==identity{assert_eq!(bytes.len(),640*480*2);assert_eq!(p["chunks"],index);let mut hash=2166136261u32;let mut im=image::RgbImage::new(640,480);
            for(n,b)in bytes.chunks_exact(2).enumerate(){let v=u16::from_le_bytes([b[0],b[1]]);hash=(hash^v as u32).wrapping_mul(16777619);let(r,g,b)=((v>>11)as u8,((v>>5)&63)as u8,(v&31)as u8);im.put_pixel(n as u32%640,n as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));}
            assert_eq!(p["checksum"],format!("{hash:08x}"));im.save(output.join("component-center-device.png")).unwrap();break;}}
    std::fs::write(output.join("after.json"),serde_json::to_vec_pretty(&json!({"diagnostics":after,"inventory":inv,"exactOrder":true,"allNamesVerified":true,"audioOrMicrophoneUsed":false})).unwrap()).unwrap();
}

#[test]
#[ignore = "Explicit approval: sync renamed book and restart controls only; no audio/host service"]
fn p4_book_name_and_hop_restart_hardware() {
    use serde_json::json;
    assert_eq!(std::env::var("P4_RENAME_RESTART_TEST").as_deref(),Ok("1"));
    let root=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|t,p|{if t=="miniapp/state"{let _=tx.send(p);}}).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());assert_eq!(usb.status().board_device_id,board);
    let before=usb.query_diagnostics(&board).unwrap();assert_eq!(before["runtime"]["firmware"],"0.7.97-p4");assert_eq!(before["runtime"]["imageState"],"valid");
    let media=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
    let buttons=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();
    let ids=|v:&serde_json::Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_owned()).collect::<HashSet<_>>();
    let query=||{
        while rx.try_recv().is_ok(){}usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
        rx.recv_timeout(Duration::from_secs(5)).expect("current miniapp state")
    };
    let previous=query();
    for(id,version,title)in[("book-pocket","1.0.1","每日一本书"),("cloud-hop","1.1.1","云端跳一跳")]{
        let package=root.join(id);let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid,"{:?}",entry.validation_errors);assert_eq!(entry.id,id);assert_eq!(entry.version,version);
        usb.install_widget_clawpkg(id,&package,&HashMap::new(),|_,_,_|{}).unwrap();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();thread::sleep(Duration::from_millis(300));
        let state=query();assert_eq!(state["widgetId"],id);assert_eq!(state["title"],title);eprintln!("PASS installed {id} {version}, actual device title={title}");
    }
    let ready=query();let best=ready["badge"].clone();
    if previous["widgetId"]=="cloud-hop"{assert_eq!(best,previous["badge"],"best score preserved across same-ID update");}
    let event=|a:&str|{usb.send_to_board(&board,"miniapp/event",&json!({"action":a})).unwrap();thread::sleep(Duration::from_millis(100));};
    event("jump.charge");assert_eq!(query()["state"],"playing");
    event("session.restart");let pending=query();assert_eq!(pending["progressPercent"],0,"opening confirmation cancels charge");
    event("input.cancel");assert_eq!(query()["badge"],best);
    event("session.pause");assert_eq!(query()["state"],"paused");
    event("session.restart");assert_eq!(query()["state"],"paused","first press is not a reset");
    event("input.confirm");let restarted=query();assert_eq!(restarted["state"],"playing");assert_eq!(restarted["gameScore"],0);assert_eq!(restarted["progressPercent"],0);assert_eq!(restarted["badge"],best);
    let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
    assert_eq!(ids(&inventory),ids(&usb.query_widget_inventory(&board).unwrap()));assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
    eprintln!("PASS device restart confirmation/cancel/paused restart and best-score retention; silent, no microphone, no host reading storage writes. Physical retest pending.");
}

#[test]
#[ignore = "Explicit approval: update only cloud-hop after track-capable firmware; silent"]
fn p4_cloud_track_hardware() {
    use serde_json::json;
    assert_eq!(std::env::var("P4_TRACK_TEST").as_deref(),Ok("1"));
    let package=PathBuf::from(std::env::var("P4_WIDGET_DIR").unwrap());
    let output=PathBuf::from(std::env::var("P4_NEW_REPORT_DIR").unwrap());
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid);assert_eq!(entry.id,"cloud-hop");assert_eq!(entry.version,"1.1.0");
    let usb=UsbSerialManager::new();usb.set_desktop_device_id("petui-track-acceptance");
    let(tx,rx)=mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|t,p|{if t=="miniapp/state"||t.starts_with("debug/"){let _=tx.send((t,p));}}).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());assert_eq!(usb.status().board_device_id,board);
    assert_eq!(usb.status().capabilities["widgetPlatformTrack"],"p4-platform-track-v1");
    let before=usb.query_diagnostics(&board).unwrap();assert_eq!(before["runtime"]["firmware"],"0.7.95-p4");assert_eq!(before["runtime"]["imageState"],"valid");
    let media=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
    let buttons=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();
    let ids=|v:&serde_json::Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_owned()).collect::<HashSet<_>>();
    usb.install_widget_clawpkg("cloud-hop",&package,&HashMap::new(),|f,n,b|eprintln!("track sync {f}/{n} {b}")).unwrap();
    usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
    thread::sleep(Duration::from_millis(500));
    usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
    let until=Instant::now()+Duration::from_secs(5);let mut shown=false;
    while Instant::now()<until{if let Ok((t,p))=rx.recv_timeout(Duration::from_millis(40)){if t=="miniapp/state"&&p["widgetId"]=="cloud-hop"{shown=true;break;}}}assert!(shown);
    use base64::Engine;
    while rx.try_recv().is_ok(){}usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
    let until=Instant::now()+Duration::from_secs(30);let mut identity=serde_json::Value::Null;let mut bytes=Vec::new();let mut index=0;
    loop{
        assert!(Instant::now()<until,"screenshot timeout");let Ok((t,p))=rx.recv_timeout(Duration::from_millis(50))else{continue;};
        assert_ne!(t,"debug/screenshot_error","{p}");
        if t=="debug/screenshot_begin"{assert_eq!(p["width"],640);assert_eq!(p["height"],480);assert_eq!(p["format"],"rgb565le");identity=p["id"].clone();}
        else if t=="debug/screenshot_chunk"&&p["id"]==identity{assert_eq!(p["index"],index);index+=1;bytes.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());}
        else if t=="debug/screenshot_end"&&p["id"]==identity{
            assert_eq!(bytes.len(),640*480*2);assert_eq!(p["chunks"],index);let mut hash=2166136261u32;let mut im=image::RgbImage::new(640,480);
            for(n,b)in bytes.chunks_exact(2).enumerate(){let v=u16::from_le_bytes([b[0],b[1]]);hash=(hash^v as u32).wrapping_mul(16777619);let(r,g,b)=((v>>11)as u8,((v>>5)&63)as u8,(v&31)as u8);im.put_pixel(n as u32%640,n as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));}
            assert_eq!(p["checksum"],format!("{hash:08x}"));im.save(output.join("track-device-ready.png")).unwrap();break;
        }
    }
    let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
    assert_eq!(ids(&inventory),ids(&usb.query_widget_inventory(&board).unwrap()));assert_eq!(buttons["config"],usb.query_button_config(&board).unwrap()["config"]);
    std::fs::write(output.join("track-capabilities.json"),serde_json::to_vec_pretty(&usb.status().capabilities).unwrap()).unwrap();
    eprintln!("PASS 0.7.95 device track declaration/rendering; inventory/global buttons preserved; continuous physical play pending user confirmation");
}

#[test]
#[ignore = "Explicit approval: install only cloud-hop/daily-almanac/book-pocket; silent, no firmware"]
fn p4_three_new_components_hardware() {
    use serde_json::{json,Value};
    assert_eq!(std::env::var("PET_NEW_COMPONENT_TEST").as_deref(),Ok("1"));
    let packages=PathBuf::from(std::env::var("P4_APP_PACKAGES").unwrap());
    let output=PathBuf::from(std::env::var("P4_NEW_REPORT_DIR").unwrap());
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
    let ids=["daily-almanac","book-pocket","cloud-hop"];
    for id in ids {let e=crate::component_library::inspect(&packages.join(id)).unwrap();assert!(e.valid,"{id}: {:?}",e.validation_errors);}
    std::fs::create_dir_all(&output).unwrap();
    let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();let(atx,arx)=mpsc::channel();
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,p|{
        if topic=="app/event"{let _=atx.send(p);}else if topic=="miniapp/state"||topic.starts_with("debug/"){let _=tx.send((topic,p));}
    }).unwrap();
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){self.0.disconnect();}}
    let _close=Close(usb.clone());assert_eq!(usb.status().board_device_id,board);
    let caps=usb.status().capabilities;assert_eq!(caps["widgetMotion"],"p4-motion-v1");
    std::fs::write(output.join("capabilities.json"),serde_json::to_vec_pretty(&caps).unwrap()).unwrap();
    let before=usb.query_diagnostics(&board).unwrap();assert_eq!(before["runtime"]["firmware"],"0.7.93-p4");assert_eq!(before["runtime"]["imageState"],"valid");
    let media=usb.media_request(&board,"media/query",json!({})).unwrap();assert!(!matches!(media["state"].as_str(),Some("playing"|"buffering"|"paused")));
    let buttons=usb.query_button_config(&board).unwrap();let inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["complete"],true);
    let inventory_ids=|v:&Value|v["items"].as_array().unwrap().iter().map(|i|i["id"].as_str().unwrap().to_owned()).collect::<HashSet<_>>();
    let mut expected=inventory_ids(&inventory);for id in ids{expected.insert(id.into());}
    assert!(expected.len()<=inventory["maxInstalled"].as_u64().unwrap()as usize);
    let storage=tempfile::tempdir().unwrap();let mut seen=HashSet::new();let mut last=Value::Null;
    let mut pump=|id:&str,ms:u64|{
        let until=Instant::now()+Duration::from_millis(ms);
        while Instant::now()<until{
            let Ok(p)=arx.recv_timeout(Duration::from_millis(30))else{continue;};
            if p["componentId"]!=id||p["visible"]!=true{continue;}
            let action=p["action"].as_str().unwrap_or("status");
            if action!="status"&&!seen.insert(format!("{}:{}:{}",id,p["instance"],p["sequence"])){continue;}
            // Production host interpreter, temporary state. No narration here.
            let result=crate::component_app::test_request(&packages.join(id),storage.path(),if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})},&usb).unwrap();
            usb.send_app_snapshot(&board,&json!({"componentId":id,"instance":p["instance"],"sequence":p["sequence"],"values":result["values"],"ui":result["ui"]})).unwrap();last=result;
        }
        last.clone()
    };
    let shown=|id:&str|{
        while rx.try_recv().is_ok(){}usb.send_to_board(&board,"miniapp/query",&json!({})).unwrap();
        let until=Instant::now()+Duration::from_secs(5);
        loop{assert!(Instant::now()<until,"no view: {id}");if let Ok((t,p))=rx.recv_timeout(Duration::from_millis(40)){if t=="miniapp/state"&&p["widgetId"]==id{return p;}}}
    };
    let capture=|name:&str,id:&str,pump:&mut dyn FnMut(&str,u64)->Value|{
        use base64::Engine;
        while rx.try_recv().is_ok(){}usb.send_to_board(&board,"debug/screenshot",&json!({"fullResolution":true})).unwrap();
        let until=Instant::now()+Duration::from_secs(30);let mut identity=Value::Null;let mut bytes=Vec::new();let mut index=0;
        loop{
            if id!="cloud-hop"{pump(id,30);}
            assert!(Instant::now()<until,"screenshot timeout");let Ok((topic,p))=rx.recv_timeout(Duration::from_millis(40))else{continue;};
            assert_ne!(topic,"debug/screenshot_error","{p}");
            if topic=="debug/screenshot_begin"{assert_eq!(p["width"],640);assert_eq!(p["height"],480);assert_eq!(p["format"],"rgb565le");identity=p["id"].clone();}
            else if topic=="debug/screenshot_chunk"&&p["id"]==identity{assert_eq!(p["index"],index);index+=1;bytes.extend(base64::engine::general_purpose::STANDARD.decode(p["data"].as_str().unwrap()).unwrap());}
            else if topic=="debug/screenshot_end"&&p["id"]==identity{
                assert_eq!(bytes.len(),640*480*2);assert_eq!(p["chunks"],index);let mut hash=2166136261u32;let mut image=image::RgbImage::new(640,480);
                for(n,b)in bytes.chunks_exact(2).enumerate(){let v=u16::from_le_bytes([b[0],b[1]]);hash=(hash^v as u32).wrapping_mul(16777619);let(r,g,b)=((v>>11)as u8,((v>>5)&63)as u8,(v&31)as u8);image.put_pixel(n as u32%640,n as u32/640,image::Rgb([(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)]));}
                assert_eq!(p["checksum"],format!("{hash:08x}"));image.save(output.join(format!("{name}.png"))).unwrap();break;
            }
        }
    };
    let event=|action:&str|usb.send_to_board(&board,"miniapp/event",&json!({"action":action})).unwrap();
    for id in ids{
        usb.install_widget_clawpkg(id,&packages.join(id),&HashMap::new(),|f,n,_|eprintln!("SYNC {id} {f}/{n}")).unwrap();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
        if id=="cloud-hop"{thread::sleep(Duration::from_millis(500));assert_eq!(shown(id)["widgetId"],id);capture(id,id,&mut pump);}
        else{
            let first=pump(id,1500);assert!(first["values"]["headline"].as_str().is_some_and(|s|!s.is_empty()));
            assert_eq!(shown(id)["headline"],first["values"]["headline"]);capture(id,id,&mut pump);pump(id,2300);
            if id=="daily-almanac"{
                event("host.next");let next=pump(id,500);assert_ne!(next["values"]["metricValue"],first["values"]["metricValue"]);
                event("host.today");assert_eq!(pump(id,500)["values"]["metricValue"],first["values"]["metricValue"]);
                event("host.details");assert_eq!(pump(id,500)["state"]["calendar"]["details"],true);capture("calendar-details",id,&mut pump);
            }else{
                event("host.next");assert_eq!(pump(id,600)["knowledge"]["page"],1);
                event("host.category");assert_eq!(pump(id,600)["ui"]["mode"],3);capture("books-category",id,&mut pump);pump(id,2300);
                event("host.next");pump(id,400);event("host.random");let selected=pump(id,600);assert_ne!(selected["ui"]["mode"],3);assert_eq!(selected["knowledge"]["page"],0);
                event("host.random");assert_ne!(pump(id,600)["knowledge"]["current"]["id"],selected["knowledge"]["current"]["id"]);
            }
        }
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        let d=usb.query_diagnostics(&board).unwrap();assert_eq!(d["bootCount"],before["bootCount"]);assert_eq!(d["faultResetCount"],before["faultResetCount"]);
        eprintln!("PASS {id}: real device rendered, exited without reset");
    }
    let slots=usb.query_p4_appearance_slots("new-components-voice-identity").unwrap();
    eprintln!("Active appearance identity only: {slots:?}");
    assert_eq!(inventory_ids(&usb.query_widget_inventory(&board).unwrap()),expected);
    assert_eq!(usb.query_button_config(&board).unwrap()["config"],buttons["config"]);
    assert_eq!(usb.query_diagnostics(&board).unwrap()["storage"]["activeAppearanceSlot"],before["storage"]["activeAppearanceSlot"]);
    std::fs::write(output.join("silent-report.json"),serde_json::to_vec_pretty(&json!({"firmware":"0.7.93-p4","components":ids,"native640x480Screenshots":true,"calendarDateAndDetailRoundtrip":true,"bookCategoryRandomAndPages":true,"userSavesModified":false,"microphoneOrAudioUsed":false,"physicalHoldTouch":"pending_user_test"})).unwrap()).unwrap();
}

#[test]
#[ignore = "Explicit permission and notice: current-pet book narration, random/exit; no microphone"]
fn p4_book_narration_hardware() {
    use serde_json::{json,Value};
    assert_eq!(std::env::var("PET_BOOK_AUDIO_TEST").as_deref(),Ok("1"));
    let package=PathBuf::from(std::env::var("PET_BOOK_PACKAGE").unwrap());
    let entry=crate::component_library::inspect(&package).unwrap();assert!(entry.valid);assert_eq!(entry.id,"book-pocket");
    let config=PathBuf::from(std::env::var("P4_SERVICE_CONFIG").unwrap());
    crate::voice_chat_settings::configure_storage_dir(config.clone()).unwrap();
    crate::volcengine_asr::configure_storage_dir(config.clone()).unwrap(); // config only; no ASR opened
    let volume=(crate::voice_chat_settings::realtime_volume().unwrap().clamp(0.0,0.35)*100.0)as u32;
    assert!(volume>0,"global volume muted; do not override it for this test");
    let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();let usb=UsbSerialManager::new();let(tx,rx)=mpsc::channel();
    usb.set_desktop_device_id("new-components-approved-audio-test");
    usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),move|topic,p|{
        crate::component_audio::observe(&topic,&p);
        if topic=="app/event"{if let Some(id)=p["componentId"].as_str(){crate::component_audio::heartbeat(id,p["visible"]==true);}let _=tx.send(p);}
    }).unwrap();assert_eq!(usb.status().board_device_id,board);
    struct Close(UsbSerialManager);impl Drop for Close{fn drop(&mut self){crate::component_audio::stop(Some("book-pocket"));let b=self.0.status().board_device_id;let _=self.0.send_to_board(&b,"control/screen-page",&serde_json::json!({"page":"main"}));thread::sleep(Duration::from_millis(1500));self.0.disconnect();let _=crate::component_voice::component_speech_set_pet_voice(None);}}
    let _close=Close(usb.clone());
    let slots=usb.query_p4_appearance_slots("book-audio-current-pet").unwrap();
    let active=slots.slots.iter().find(|s|Some(s.slot)==slots.active_slot).unwrap();
    assert_eq!(active.pack_id,std::env::var("P4_EXPECTED_APPEARANCE_PACK").unwrap(),"never guess another pet's voice");
    let appearance=std::env::var("P4_CURRENT_APPEARANCE_ID").unwrap();assert!(appearance.bytes().all(|b|b.is_ascii_alphanumeric()||b"_-".contains(&b)));
    let persona:Value=serde_json::from_slice(&std::fs::read(config.join("custom-appearances").join(&appearance).join("persona-voice.json")).unwrap()).unwrap();
    let v=&persona["voice"];
    let voice=json!({"boardDeviceId":board,"appearanceId":appearance,"speaker":v["speaker"],"cloneSpeakerId":v["clone_speaker_id"].as_str().unwrap_or(""),"speed":v["speed"].as_f64().unwrap_or(1.0)});
    crate::component_voice::component_speech_set_pet_voice(Some(serde_json::from_value(voice.clone()).unwrap())).unwrap();
    let before=usb.query_diagnostics(&board).unwrap();let controls=usb.query_button_config(&board).unwrap();
    assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    usb.install_widget_clawpkg("book-pocket",&package,&HashMap::new(),|_,_,_|{}).unwrap();
    let storage=tempfile::tempdir().unwrap();let mut narrator=crate::knowledge_narration::Narration::default();let mut seen=HashSet::new();let mut visible=false;let mut epoch=0;let mut starts=0;let mut card=Value::Null;
    let mut pump=|ms:u64|{
        let until=Instant::now()+Duration::from_millis(ms);
        while Instant::now()<until{
            let Ok(p)=rx.recv_timeout(Duration::from_millis(40))else{continue;};
            if p["componentId"]!="book-pocket"{continue;}
            let showing=p["visible"]==true;let entering=showing&&!visible;if showing!=visible{epoch+=1;visible=showing;}if !visible{continue;}
            usb.send_to_board(&board,"system/heartbeat",&json!({})).unwrap();
            crate::component_voice::component_speech_set_pet_voice(Some(serde_json::from_value(voice.clone()).unwrap())).unwrap();
            let action=p["action"].as_str().unwrap_or("status");assert!(matches!(action,"status"|"host.random"));
            if action!="status"&&!seen.insert(format!("{}:{}",p["instance"],p["sequence"])){continue;}
            let input=if entering&&action=="status"{json!({"operation":"random"})}else if action=="status"{json!({"operation":"status"})}else{json!({"operation":"action","action":action})};
            let result=crate::component_app::test_request(&package,storage.path(),input,&usb).unwrap();card=result["knowledge"]["current"]["id"].clone();
            let audio=crate::component_audio::status("book-pocket");assert_ne!(audio["phase"],"error","{}",audio["error"]);
            if let Some(text)=narrator.next(epoch,&result,audio["phase"].as_str().unwrap_or("idle")){
                crate::component_audio::start("book-pocket",&usb,crate::component_audio::Source::Speech(text),volume).unwrap();starts+=1;eprintln!("BOOK auto narration segment {starts}");
            }
            usb.send_app_snapshot(&board,&json!({"componentId":"book-pocket","instance":p["instance"],"sequence":p["sequence"],"values":result["values"],"ui":result["ui"]})).unwrap();
        }
        (starts,card.clone(),crate::component_audio::status("book-pocket")["phase"].clone())
    };
    usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
    let deadline=Instant::now()+Duration::from_secs(110);
    let first=loop{let s=pump(250);assert!(Instant::now()<deadline,"no sequential speech");if s.0>=2&&s.2=="playing"{break s;}};assert_eq!(first.0,2);
    usb.send_to_board(&board,"miniapp/event",&json!({"action":"host.random"})).unwrap();let deadline=Instant::now()+Duration::from_secs(45);
    loop{let s=pump(250);assert!(Instant::now()<deadline,"new book speech timeout");if s.1!=first.1&&s.2=="playing"{assert_eq!(s.0,3);break;}}
    pump(1500);usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();pump(2500);
    assert!(!crate::component_audio::playing("book-pocket"));assert!(!matches!(usb.media_request(&board,"media/query",json!({})).unwrap()["state"].as_str(),Some("playing"|"buffering"|"paused")));
    assert_eq!(pump(3000).0,3,"exit must not restart");
    let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);assert_eq!(controls["config"],usb.query_button_config(&board).unwrap()["config"]);
    eprintln!("PASS book device entry: actual current-pet voice, sequential speech, random switch, exit stops; no microphone, no user-save changes. Host module integration, not installed desktop UI.");
}
