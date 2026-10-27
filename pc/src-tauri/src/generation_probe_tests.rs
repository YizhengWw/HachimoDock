//! Manual, paid, one-shot diagnosis. Not part of default tests or release binary.
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use serde_json::{json, Value};
use std::{fs, io::{Cursor, Write}, path::Path};

fn private_json(path: &Path, value: &Value) {
    let mut options = fs::OpenOptions::new(); options.write(true).create_new(true);
    #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.mode(0o600); }
    let mut file = options.open(path).expect("create private probe artifact");
    file.write_all(serde_json::to_string_pretty(value).unwrap().as_bytes()).unwrap();
}
fn string<'a>(v: &'a Value, field: &str, default: &'a str) -> &'a str {
    v.get(field).and_then(Value::as_str).filter(|s| !s.is_empty()).unwrap_or(default)
}

fn private_bytes(path: &Path, bytes: &[u8]) {
    let mut options=fs::OpenOptions::new(); options.write(true).create_new(true);
    #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.mode(0o600); }
    options.open(path).unwrap().write_all(bytes).unwrap();
}

#[test]
#[ignore = "Offline conversion of two already-generated trial videos; no model or device access"]
fn trial_videos_prepare_device_assets() {
    let root=std::path::PathBuf::from(std::env::var("PET_GENERATION_PROBE_DIR").unwrap());
    let trial=root.join("video-corrected");
    let target=root.join("device-assets-check");
    fs::create_dir(&target).expect("fresh isolated artifact directory");
    let mut families=Vec::new();
    for family in ["waiting_user","done"] {
        let filename=format!("{family}.mp4");
        fs::copy(trial.join(&filename),target.join(&filename)).unwrap();
        families.push(json!({"family":family,"ok":true,"videoPath":filename}));
    }
    private_json(&target.join("manifest.json"),&json!({"schema_version":1,"families":families}));
    let result=crate::usb_serial::prepare_p4_appearance(&target,&target).expect("real P4 conversion and validation");
    println!("Prepared trial device resources: {}",serde_json::to_string(&result).unwrap());
}

#[test]
#[ignore = "Authorized workflow trial: one prompt request on reviewed clean reference; no image/video generation"]
fn authorized_clean_reference_prompts() {
    assert_eq!(std::env::var("PET_GENERATION_PROBE_APPROVED").as_deref(), Ok("two-video-workflow"));
    let output=std::path::PathBuf::from(std::env::var("PET_GENERATION_PROBE_DIR").unwrap());
    let plan:Value=serde_json::from_slice(&fs::read(output.join("workflow-plan.json")).unwrap()).unwrap();
    private_json(&output.join("prompt-started.json"), &json!({"limit":1}));
    let dir=std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").unwrap());
    let providers=crate::secure_config::load_providers(&dir).unwrap();
    crate::llm_network::configure_storage_dir(dir).unwrap();
    crate::generation_diagnostics::configure(output.clone());
    let key=string(&providers["volcengine"],"apiKey",""); assert!(!key.is_empty());
    let source=image::load_from_memory(&fs::read(output.join("processed.png")).unwrap()).unwrap();
    let mut small_data=Vec::new();
    for width in [400_u32,768] {
        let height=width*3/4;
        let resized=source.resize(width,height,image::imageops::FilterType::Triangle).to_rgba8();
        let mut canvas=image::RgbaImage::from_pixel(width,height,image::Rgba([0,0,0,255]));
        image::imageops::overlay(&mut canvas,&resized,((width-resized.width())/2) as i64,((height-resized.height())/2) as i64);
        let mut bytes=Cursor::new(Vec::new()); canvas.write_to(&mut bytes,image::ImageFormat::Png).unwrap();
        if width==400 { small_data=bytes.into_inner(); } else { private_bytes(&output.join("video-reference.png"),&bytes.into_inner()); }
    }
    let mut body=plan["thinkingBody"].clone();
    body["input"][0]["content"][0]["image_url"]=json!(format!("data:image/png;base64,{}",B64.encode(small_data)));
    tauri::async_runtime::block_on(async {
        let r=crate::http_request_text("https://ark.cn-beijing.volces.com/api/v3/responses".into(),Some("POST".into()),Some(json!({"Authorization":format!("Bearer {key}"),"Content-Type":"application/json"}).to_string()),Some(body.to_string()),Some(120000),Some("thinking".into())).await.expect("prompt request stopped without retry");
        let payload:Value=serde_json::from_str(&r.body).unwrap();
        private_json(&output.join("thinking-response.private.json"),&payload);
        println!("Prompt result: {}",json!({"ok":r.ok,"diagnostic":r.diagnostic,"usage":payload["usage"]}));
        assert!(r.ok,"prompt failed; no videos submitted");
    });
}

#[test]
#[ignore = "Explicitly authorized two-video trial, guarded against rerun; no retries or model fallback"]
fn authorized_two_video_workflow() {
    assert_eq!(std::env::var("PET_GENERATION_PROBE_APPROVED").as_deref(),Ok("two-video-workflow"));
    let output=std::path::PathBuf::from(std::env::var("PET_GENERATION_PROBE_DIR").unwrap());
    let plan:Value=serde_json::from_slice(&fs::read(output.join("video-plan.json")).unwrap()).unwrap();
    let entries=plan.as_array().unwrap(); assert_eq!(entries.len(),2);
    private_json(&output.join("videos-started.json"),&json!({"limit":2}));
    let dir=std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").unwrap());
    let providers=crate::secure_config::load_providers(&dir).unwrap();
    crate::llm_network::configure_storage_dir(dir).unwrap();
    crate::generation_diagnostics::configure(output.clone());
    let key=string(&providers["volcengine"],"apiKey",""); assert!(!key.is_empty());
    let headers=json!({"Authorization":format!("Bearer {key}"),"Content-Type":"application/json"}).to_string();
    let reference=format!("data:image/png;base64,{}",B64.encode(fs::read(output.join("video-reference.png")).unwrap()));
    tauri::async_runtime::block_on(async {
        let mut tasks=Vec::new();
        for entry in entries {
            let family=entry["family"].as_str().unwrap();
            assert!(family.chars().all(|c|c.is_ascii_lowercase()||c=='.'||c=='_'));
            let mut body=entry["body"].clone();
            assert_eq!(body["model"],providers["volcengine"]["model"],"model must match user's selected model");
            for content in body["content"].as_array_mut().unwrap() {
                if content["type"]=="image_url" {content["image_url"]["url"]=json!(reference);}
            }
            let r=crate::http_request_text("https://ark.cn-beijing.volces.com/api/v3/contents/generations/tasks".into(),Some("POST".into()),Some(headers.clone()),Some(body.to_string()),Some(120000),Some("video_submit".into())).await.expect("submission outcome unknown; never replay");
            let payload:Value=serde_json::from_str(&r.body).unwrap();
            private_json(&output.join(format!("{family}-submit.private.json")),&payload);
            println!("Submit {family}: {}",json!({"ok":r.ok,"diagnostic":r.diagnostic}));
            assert!(r.ok,"submission rejected; remaining calls stopped");
            let id=payload["id"].as_str().expect("task id").to_owned();
            tasks.push((family.to_owned(),id));
        }
        let started=std::time::Instant::now();
        let mut completed=std::collections::HashSet::new();
        while completed.len()<tasks.len() {
            assert!(started.elapsed().as_secs()<1200,"poll budget exceeded; task IDs saved, do not resubmit");
            for (family,id) in &tasks {
                if completed.contains(family) {continue;}
                let r=crate::http_request_text(format!("https://ark.cn-beijing.volces.com/api/v3/contents/generations/tasks/{id}"),Some("GET".into()),Some(headers.clone()),None,Some(30000),Some("video_poll".into())).await.expect("poll stopped; retain task id");
                assert!(r.ok,"poll HTTP error; do not resubmit");
                let payload:Value=serde_json::from_str(&r.body).unwrap();
                let status=payload["status"].as_str().unwrap_or("unknown");
                if status=="succeeded" {
                    let url=payload.pointer("/content/video_url").and_then(Value::as_str).unwrap();
                    let bytes=crate::download_bytes(url.into()).await.unwrap();
                    private_bytes(&output.join(format!("{family}.mp4")),&bytes);
                    private_json(&output.join(format!("{family}-result.private.json")),&payload);
                    println!("Completed {family}: {}",json!({"seconds":started.elapsed().as_secs(),"bytes":bytes.len(),"usage":payload["usage"]}));
                    completed.insert(family.clone());
                } else if ["failed","cancelled","expired"].contains(&status) {
                    private_json(&output.join(format!("{family}-failed.private.json")),&payload);
                    panic!("video terminal failure {family}: {status}; no resubmission");
                }
            }
            if completed.len()<tasks.len() {tokio::time::sleep(std::time::Duration::from_secs(10)).await;}
        }
        println!("Trial stopped after exactly two video submissions.");
    });
}

#[test]
#[ignore = "Explicit approval only: one Flash image request; no thinking, video or retries"]
fn authorized_flash_image_only_probe() {
    assert_eq!(std::env::var("PET_GENERATION_PROBE_APPROVED").as_deref(), Ok("one-flash-image"));
    let output = std::path::PathBuf::from(std::env::var("PET_GENERATION_PROBE_DIR").unwrap());
    let plan: Value = serde_json::from_slice(&fs::read(output.join("plan.json")).unwrap()).unwrap();
    let mut body = plan["imageBody"].clone();
    assert_eq!(body["model"], "doubao-seedream-5-0-flash-260915");
    private_json(&output.join("started.json"), &json!({"limit":"one-flash-image"}));
    let dir = std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").unwrap());
    let providers = crate::secure_config::load_providers(&dir).expect("read existing config");
    crate::llm_network::configure_storage_dir(dir).unwrap();
    crate::generation_diagnostics::configure(output.clone());
    let config = &providers["volcengine"];
    let key = string(config,"apiKey",""); assert!(!key.is_empty());
    assert_eq!(string(config,"baseUrl","https://ark.cn-beijing.volces.com").trim_end_matches('/'),"https://ark.cn-beijing.volces.com");
    let original = fs::read(plan["imagePath"].as_str().unwrap()).unwrap();
    body["image"] = json!([format!("data:image/png;base64,{}", B64.encode(original))]);
    tauri::async_runtime::block_on(async {
        println!("Starting Flash image processing: exactly one request; no fallback or retry");
        let result = crate::http_request_text("https://ark.cn-beijing.volces.com/api/v3/images/generations".into(),Some("POST".into()),Some(json!({"Content-Type":"application/json","Authorization":format!("Bearer {key}")}).to_string()),Some(body.to_string()),Some(120_000),Some("image_edit".into())).await;
        let report = match result {
            Ok(response) => {
                let payload: Value = serde_json::from_str(&response.body).expect("JSON response");
                let mut report = json!({"diagnostic":response.diagnostic,"model":body["model"],"httpOk":response.ok});
                if response.ok {
                    let encoded = payload.pointer("/data/0/b64_json").and_then(Value::as_str).expect("requested base64 image missing; no additional request");
                    let bytes = B64.decode(encoded).unwrap();
                    let decoded = image::load_from_memory(&bytes).expect("valid generated image");
                    let mut options = fs::OpenOptions::new(); options.write(true).create_new(true);
                    #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.mode(0o600); }
                    options.open(output.join("processed.png")).unwrap().write_all(&bytes).unwrap();
                    report["image"] = json!({"width":decoded.width(),"height":decoded.height(),"bytes":bytes.len()});
                    report["usage"] = payload["usage"].clone();
                } else {
                    private_json(&output.join("error-response.private.json"), &payload);
                    report["errorCode"] = json!(payload.pointer("/error/code").and_then(Value::as_str).and_then(crate::generation_diagnostics::safe_request_id));
                }
                report
            }
            Err(error) => json!({"stage":"image_edit","error":error}),
        };
        private_json(&output.join("summary.json"), &report);
        println!("{report}");
        println!("Stopped. Thinking requests: 0. Video submissions: 0.");
    });
}

#[test]
#[ignore = "Read-only configured Ark model catalog; no inference calls"]
fn configured_model_catalog() {
    let dir = std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").expect("config directory"));
    let providers = crate::secure_config::load_providers(&dir).expect("existing config");
    crate::llm_network::configure_storage_dir(dir).unwrap();
    let key=string(&providers["volcengine"],"apiKey",""); assert!(!key.is_empty());
    tauri::async_runtime::block_on(async {
        let r=crate::http_request_text("https://ark.cn-beijing.volces.com/api/v3/models".into(),Some("GET".into()),Some(json!({"Authorization":format!("Bearer {key}")}).to_string()),None,Some(15000),None).await.expect("catalog transport");
        assert!(r.ok,"catalog HTTP {}",r.status);
        let body:Value=serde_json::from_str(&r.body).unwrap();
        let ids:Vec<_>=body["data"].as_array().unwrap().iter().filter_map(|r|r["id"].as_str()).filter(|id|id.starts_with("doubao-seedream-")||id.starts_with("doubao-seed-2-1-")).collect();
        println!("Model catalog IDs: {}",json!(ids));
    });
}

#[test]
#[ignore = "Local read-only model selection audit; no network or credential output"]
fn configured_generation_model_selection() {
    let dir = std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").expect("config directory"));
    let providers = crate::secure_config::load_providers(&dir).expect("existing config");
    let config = &providers["volcengine"];
    let mut report = serde_json::Map::new();
    for field in ["model", "thinkingModel", "imageModel"] {
        let value = string(config, field, "");
        if value.starts_with("doubao-") && value.len() < 100 && value.chars().all(|c| c.is_ascii_alphanumeric() || c == '-') {
            report.insert(field.into(), json!(value));
        } else { report.insert(field.into(), json!(if value.is_empty() {"unset"} else {"custom (redacted)"})); }
    }
    report.insert("fastGeneration".into(), json!(config["fastGeneration"].as_bool()));
    println!("Model selection: {}", Value::Object(report));
}

#[test]
#[ignore = "Requires explicit user approval: one image edit + one prompt request, no video submissions"]
fn authorized_image_and_prompt_probe() {
    assert_eq!(std::env::var("PET_GENERATION_PROBE_APPROVED").as_deref(), Ok("one-image-one-thinking"));
    let output = std::path::PathBuf::from(std::env::var("PET_GENERATION_PROBE_DIR").expect("probe directory"));
    let plan: Value = serde_json::from_slice(&fs::read(output.join("plan.json")).unwrap()).unwrap();
    // Refuse reruns with the same plan, even after interruption or timeout.
    private_json(&output.join("started.json"), &json!({"started":true,"limit":"one-image-one-thinking"}));
    let config_dir = std::path::PathBuf::from(std::env::var("PET_GENERATION_CONFIG_DIR").expect("config directory"));
    crate::llm_network::configure_storage_dir(config_dir.clone()).unwrap();
    crate::generation_diagnostics::configure(output.clone());
    let providers = crate::secure_config::load_providers(&config_dir).expect("read existing config without changing it");
    let config = &providers["volcengine"];
    let key = string(config,"apiKey",""); assert!(!key.is_empty(),"existing Ark key missing");
    let base = string(config,"baseUrl","https://ark.cn-beijing.volces.com").trim_end_matches('/');
    assert_eq!(base,"https://ark.cn-beijing.volces.com","only explicitly approved Ark destination allowed");
    let headers = json!({"Content-Type":"application/json","Authorization":format!("Bearer {key}")}).to_string();
    let original = fs::read(plan["imagePath"].as_str().unwrap()).expect("read authorized reference");
    let mut source = original.clone();
    let mut image_body = plan["imageBody"].clone();
    image_body["image"] = json!([format!("data:image/png;base64,{}", B64.encode(&original))]);
    image_body["model"] = json!(string(config,"imageModel",image_body["model"].as_str().unwrap()));
    if image_body["model"] == "doubao-seedream-5-0-lite-260128" { image_body["model"] = json!("doubao-seedream-5-0-260128"); }
    let mut reports = Vec::new();
    tauri::async_runtime::block_on(async {
        println!("Starting image_edit: one request, no retries");
        let result = crate::http_request_text(format!("{base}/api/v3/images/generations"),Some("POST".into()),Some(headers.clone()),Some(image_body.to_string()),Some(120_000),Some("image_edit".into())).await;
        match result {
            Ok(response) => {
                let mut report = json!({"diagnostic":response.diagnostic,"model":image_body["model"]});
                if let Ok(body) = serde_json::from_str::<Value>(&response.body) {
                    if response.ok {
                        if let Some(data) = body.pointer("/data/0/b64_json").and_then(Value::as_str) { source = B64.decode(data).expect("decode generated reference"); }
                        else { panic!("Image response did not contain requested b64_json; no additional model call made"); }
                    } else { report["errorCode"] = json!(body.pointer("/error/code").and_then(Value::as_str).and_then(crate::generation_diagnostics::safe_request_id)); }
                }
                println!("{}", report); reports.push(report);
            }
            Err(error) => { println!("image_edit: {error}"); reports.push(json!({"stage":"image_edit","error":error})); }
        }
        if plan["useOriginalForThinking"].as_bool() == Some(true) {
            source = original;
            println!("Thinking reference: original image, held constant for the reasoning-switch comparison");
        }
        // Same 4:3, even-pixel fit and black canvas as processImageForPipeline.
        // Rust raster resampling is the sole substitute for WebView canvas here.
        let image = image::load_from_memory(&source).expect("decode reference");
        let width: u32 = if config["fastGeneration"].as_bool() == Some(false) {512} else {400};
        let even = |n:f64| {let n=n.round().max(2.0) as u32; n + n%2};
        let height = even(width as f64 * 0.75);
        let scale = (width as f64/image.width() as f64).min(height as f64/image.height() as f64);
        let dw=even(image.width() as f64*scale); let dh=even(image.height() as f64*scale);
        let mut resized=image.resize_exact(dw,dh,image::imageops::FilterType::Triangle).to_rgba8();
        for pixel in resized.pixels_mut() { pixel[3]=if pixel[3]>=16 {255} else {0}; }
        let mut canvas=image::RgbaImage::from_pixel(width,height,image::Rgba([0,0,0,255]));
        image::imageops::overlay(&mut canvas,&resized,((width-dw) as f64/2.0).round() as i64,((height-dh) as f64/2.0).round() as i64);
        let mut bytes=Cursor::new(Vec::new()); canvas.write_to(&mut bytes,image::ImageFormat::Png).unwrap();
        let mut thinking=plan["thinkingBody"].clone();
        thinking["model"]=json!(string(config,"thinkingModel",thinking["model"].as_str().unwrap()));
        thinking["input"][0]["content"][0]["image_url"]=json!(format!("data:image/png;base64,{}",B64.encode(bytes.into_inner())));
        println!("Starting thinking: one request, no retries; model={}",thinking["model"]);
        let result=crate::http_request_text(format!("{base}/api/v3/responses"),Some("POST".into()),Some(headers),Some(thinking.to_string()),Some(120_000),Some("thinking".into())).await;
        match result {
            Ok(response) => {
                let mut report=json!({"diagnostic":response.diagnostic,"model":thinking["model"]});
                if let Ok(body)=serde_json::from_str::<Value>(&response.body) {
                    if response.ok { private_json(&output.join("thinking-response.private.json"),&body); report["usage"]=body["usage"].clone(); }
                    else { report["errorCode"]=json!(body.pointer("/error/code").and_then(Value::as_str).and_then(crate::generation_diagnostics::safe_request_id)); }
                }
                println!("{}",report); reports.push(report);
            }
            Err(error) => { println!("thinking: {error}"); reports.push(json!({"stage":"thinking","error":error})); }
        }
    });
    private_json(&output.join("summary.json"),&json!(reports));
    println!("Probe stopped. Video submissions: 0.");
}
