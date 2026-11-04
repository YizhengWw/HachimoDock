/*
 * [Input] Widget package paths/JSON bytes, bounded PNG sprite sheets, device capabilities, and chunk metadata.
 * [Output] P4-safe widget payloads with legacy package-navigation bindings removed,
 *          frame-contiguous RGB565-alpha sprite compilation, capability gates, path checks,
 *          and OTA timing policy.
 * [Pos] Pure component/widget transaction contract beneath usb_serial.rs.
 * [Sync] If this file changes, update `pc/.folder.md`.
 */

use super::UsbConnectionStatus;
use crate::clawpkg::validate_p4_vars_object_capped;
use serde_json::Value;
use std::path::Path;
use std::time::Duration;

pub(super) const WIDGET_BEGIN_ACK_TIMEOUT: Duration = Duration::from_secs(2);
pub(super) const WIDGET_CHUNK_ACK_TIMEOUT: Duration = Duration::from_secs(2);
// P4 may spend several seconds syncing its flash-backed widget catalog and
// refreshing the active mini-app before the final acknowledgement reaches the host.
// A/B catalog + image generations can trigger SPIFFS GC during durable commit.
// Real full-resolution app packages approach/exceed 15s after repeated updates.
// Wait for the original transaction only; never retransmit an uncertain commit.
pub(super) const WIDGET_COMMIT_ACK_TIMEOUT: Duration = Duration::from_secs(45);
pub(super) const WIDGET_DELETE_ACK_TIMEOUT: Duration = Duration::from_secs(15);
pub(super) const WIDGET_CHUNK_MAX_ATTEMPTS: usize = 3;
pub(super) const P4_WIDGET_JSON_MAX_BYTES: usize = 4095;
const P4_BUTTONS_JSON_MAX_BYTES: usize = 2047;
const P4_SCENE_MAX_SPRITES: usize = 4;
const P4_SCENE_MAX_SPRITE_PIXELS: usize = 4096;
const P4_SCENE_MAX_SPRITE_SOURCE_BYTES: usize = 128 * 1024;
const P4_SCENE_SPRITE_MAGIC: &[u8; 4] = b"P4S1";
const WIDGET_DELETE_CAPABILITY: &str = "widgetDelete";
const WIDGET_INVENTORY_CAPABILITY: &str = "widgetInventory";

pub(super) fn build_widget_begin_payload(transfer_id:&str,widget_id:&str,repair:bool,caps:&Value)->Value{
    let mut payload=serde_json::json!({"transferId":transfer_id,"widgetId":widget_id,"repairResources":repair});
    if !repair&&caps["widgetPreserveUnchanged"]=="p4-preserve-unchanged-v1"{
        payload["preserveUnchanged"]=serde_json::json!(true);
    }
    payload
}

#[test]
fn unchanged_install_begin_requires_exact_negotiation_and_never_changes_repairs(){
    let legacy=serde_json::json!({"transferId":"transfer","widgetId":"owned","repairResources":false});
    for caps in [Value::Null,serde_json::json!({}),serde_json::json!({"widgetPreserveUnchanged":true}),serde_json::json!({"widgetPreserveUnchanged":"other-engine"})]{
        assert_eq!(build_widget_begin_payload("transfer","owned",false,&caps),legacy);
    }
    let caps=serde_json::json!({"widgetPreserveUnchanged":"p4-preserve-unchanged-v1"});
    let mut opted=legacy.clone();opted["preserveUnchanged"]=serde_json::json!(true);
    assert_eq!(build_widget_begin_payload("transfer","owned",false,&caps),opted);
    let mut repair=legacy;repair["repairResources"]=serde_json::json!(true);
    assert_eq!(build_widget_begin_payload("transfer","owned",true,&caps),repair);
}

pub(super) fn build_widget_chunk_payload(
    transfer_id: &str,
    relative_path: &str,
    data_base64: &str,
    index: u32,
    decoded_size: usize,
    checksum: &str,
) -> Value {
    serde_json::json!({
        "transferId": transfer_id,
        "path": relative_path,
        "data": data_base64,
        "index": index.to_string(),
        "decodedSize": decoded_size,
        "checksum": checksum,
    })
}

pub(super) fn format_widget_ack_timeout(transfer_id: &str, phase: &str) -> String {
    format!(
        "未收到板端组件 OTA 确认: transferId={} phase={}",
        transfer_id, phase
    )
}

pub(super) fn ensure_widget_delete_supported(status: &UsbConnectionStatus) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_DELETE_CAPABILITY, "组件删除")
}

pub(super) fn ensure_widget_inventory_supported(
    status: &UsbConnectionStatus,
) -> Result<(), String> {
    ensure_p4_widget_capability(status, WIDGET_INVENTORY_CAPABILITY, "组件清单")
}

fn ensure_p4_widget_capability(
    status: &UsbConnectionStatus,
    capability: &str,
    capability_label: &str,
) -> Result<(), String> {
    if !status.runtime.eq_ignore_ascii_case("esp-p4") {
        return Ok(());
    }
    let advertised = status
        .capabilities
        .get(capability)
        .and_then(Value::as_bool)
        .or_else(|| {
            status
                .capabilities
                .get("features")
                .and_then(|features| features.get(capability))
                .and_then(Value::as_bool)
        });
    if advertised == Some(true) {
        return Ok(());
    }
    Err(format!(
        "当前 ESP32-P4 板端固件 {} 未声明{}能力；请先在设备操作中升级板端固件并重新连接，再重试。",
        if status.firmware.trim().is_empty() {
            "<未知版本>"
        } else {
            status.firmware.trim()
        },
        capability_label,
    ))
}

pub(super) fn widget_ota_relative_path(root: &Path, path: &Path) -> Result<String, String> {
    let rel = path.strip_prefix(root).map_err(|error| error.to_string())?;
    let parts = rel
        .components()
        .map(|component| match component {
            std::path::Component::Normal(value) => value
                .to_str()
                .map(str::to_string)
                .ok_or_else(|| format!("widget path is not UTF-8: {}", path.display())),
            _ => Err(format!("unsafe widget path: {}", path.display())),
        })
        .collect::<Result<Vec<_>, _>>()?;
    Ok(parts.join("/"))
}

pub(super) fn widget_ota_should_skip_path(path: &str) -> bool {
    path.starts_with("pc/") || path.starts_with("licenses/")
        || matches!(path, "LICENSE.txt" | "THIRD_PARTY_NOTICES.txt")
        || path.split('/').next_back() == Some(".keep")
}

pub(super) fn prepare_p4_widget_sprite_files(
    widget_root: &Path,
    widget_bytes: &[u8],
) -> Result<Vec<(String, Vec<u8>)>, String> {
    let widget: Value = serde_json::from_slice(widget_bytes)
        .map_err(|error| format!("runtime/widget.json is invalid JSON: {error}"))?;
    if let Some(images)=widget["surface"]["images"].as_array() {
        if widget["surface"]["engine"]=="p4-app-surface-v1" {
            let base=crate::component_capability_ext::strip(&widget);
            crate::component_extension::validate_app_surface(&base["surface"],Some(&base))?;
        } else {
            crate::component_extension::validate_surface(&widget["surface"])?;
        }
        let limits=crate::app_graphics::limits(Some(&widget));
        let mut compiled=Vec::new();let mut total=0;let mut frames_total=0;
        for im in images {
            let id=im["id"].as_str().ok_or("image id")?;
            let bytes=std::fs::read(widget_root.join(im["asset"].as_str().ok_or("image asset")?)).map_err(|e|e.to_string())?;
            let image=crate::component_extension::decode_surface_image(im,&bytes)?;
            let encoded=if im["encoding"]=="jpeg" {crate::surface_image::encode_background(&image)?}else{crate::surface_image::encode(&image)};
            total+=encoded.len();frames_total+=if im["encoding"]=="jpeg"{encoded.len()}else{0};
            if encoded.len()>100000||total>limits.image_bytes{return Err(format!("组件图片超过 {} KiB 设备存储预算",limits.image_bytes/1024));}
            compiled.push((format!("runtime/images/{id}.p4i"),encoded));
        }
        // p4-keyframe-bg-v1: frames 2..N of the JPEG background follow the declared images as "<id>.k".
        if let Some((frames,_))=crate::app_graphics::keyframes(&widget["surface"]){
            let im=&images[0];let id=im["id"].as_str().ok_or("image id")?;let asset=im["asset"].as_str().ok_or("image asset")?;
            for k in 2..=frames{
                let source=crate::app_graphics::keyframe_asset(asset,k);
                let bytes=std::fs::read(widget_root.join(&source)).map_err(|e|format!("{source}: {e}"))?;
                let image=crate::component_extension::decode_surface_image(im,&bytes)?;
                let encoded=crate::surface_image::encode_background(&image)?;
                frames_total+=encoded.len();
                if encoded.len()>crate::app_graphics::KEYFRAME_BYTES||frames_total>crate::app_graphics::KEYFRAME_SET_BYTES{return Err("关键帧背景超过每帧 100 KB / 整组 400 KB 的传输预算".into());}
                compiled.push((format!("runtime/images/{}.p4i",crate::app_graphics::keyframe_id(id,k)),encoded));
            }
        }
        return Ok(compiled);
    }
    let Some(sprites) = widget
        .get("scene")
        .and_then(|scene| scene.get("sprites"))
        .and_then(Value::as_array)
    else {
        return Ok(Vec::new());
    };
    let sprite_limits=crate::app_graphics::limits(Some(&widget));
    if sprites.len() > sprite_limits.scene_sprites {
        return Err(format!(
            "runtime/widget.json scene.sprites exceeds {} entries",
            sprite_limits.scene_sprites
        ));
    }
    let mut total_pixels = 0usize;
    let mut compiled = Vec::with_capacity(sprites.len());
    for sprite in sprites {
        let id = sprite
            .get("id")
            .and_then(Value::as_str)
            .ok_or_else(|| "scene sprite is missing id".to_string())?;
        let asset = sprite
            .get("asset")
            .and_then(Value::as_str)
            .ok_or_else(|| format!("scene sprite {id} is missing asset"))?;
        let frame_width = sprite
            .get("frame_width")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frame_width"))?;
        let frame_height = sprite
            .get("frame_height")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frame_height"))?;
        let frames = sprite
            .get("frames")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid frames"))?;
        let fps = sprite
            .get("fps")
            .and_then(Value::as_u64)
            .and_then(|value| u8::try_from(value).ok())
            .ok_or_else(|| format!("scene sprite {id} has invalid fps"))?;
        let source = widget_root.join(asset);
        let source_bytes = std::fs::read(&source)
            .map_err(|error| format!("read scene sprite {} failed: {error}", source.display()))?;
        if source_bytes.len() > P4_SCENE_MAX_SPRITE_SOURCE_BYTES {
            return Err(format!(
                "scene sprite {id} exceeds the 128 KiB source limit"
            ));
        }
        let image = image::load_from_memory_with_format(&source_bytes, image::ImageFormat::Png)
            .map_err(|error| format!("decode scene sprite {id} failed: {error}"))?
            .to_rgba8();
        let expected_width = u32::from(frame_width) * u32::from(frames);
        if image.width() != expected_width || image.height() != u32::from(frame_height) {
            return Err(format!(
                "scene sprite {id} must be {}x{}, got {}x{}",
                expected_width,
                frame_height,
                image.width(),
                image.height()
            ));
        }
        let pixels = usize::from(frame_width) * usize::from(frame_height) * usize::from(frames);
        total_pixels = total_pixels
            .checked_add(pixels)
            .ok_or_else(|| "scene sprite pixel count overflow".to_string())?;
        if total_pixels > sprite_limits.sprite_pixels as usize {
            return Err(format!(
                "scene sprites exceed {} decoded pixels",
                sprite_limits.sprite_pixels
            ));
        }
        let mut encoded = Vec::with_capacity(8 + pixels * 3);
        encoded.extend_from_slice(P4_SCENE_SPRITE_MAGIC);
        encoded.extend_from_slice(&[frame_width, frame_height, frames, fps]);
        // Source sheets are horizontal, but firmware advances frames as
        // contiguous frame-sized blocks. Repack frame-by-frame instead of
        // preserving whole-sheet scanline order, which would interleave all
        // frame columns on every row and render each animation frame as a
        // flickering mixture of its neighbours.
        for frame in 0..u32::from(frames) {
            for y in 0..u32::from(frame_height) {
                for x in 0..u32::from(frame_width) {
                    let pixel = image.get_pixel(frame * u32::from(frame_width) + x, y);
                    let red = u16::from(pixel[0]);
                    let green = u16::from(pixel[1]);
                    let blue = u16::from(pixel[2]);
                    let rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
                    encoded.extend_from_slice(&rgb565.to_le_bytes());
                    encoded.push(pixel[3]);
                }
            }
        }
        compiled.push((format!("runtime/sprites/{id}.p4s"), encoded));
    }
    Ok(compiled)
}

pub(super) fn prepare_p4_widget_file(
    widget_id: &str,
    path: &str,
    bytes: &[u8],
) -> Result<Vec<u8>, String> {
    let max_bytes = match path {
        "runtime/widget.json" => P4_WIDGET_JSON_MAX_BYTES,
        "buttons.json" => P4_BUTTONS_JSON_MAX_BYTES,
        _ => return Ok(bytes.to_vec()),
    };
    let mut value: Value = serde_json::from_slice(bytes)
        .map_err(|error| format!("{} is invalid JSON: {}", path, error))?;
    let max_bytes=if path=="runtime/widget.json" {
        match value["quota"].as_str() {
            Some("p4-quota-v3") => 12287,
            Some("p4-quota-v2") => 8191,
            _ if value["surface"]["graphics"]=="p4-app-graphics-v1" => 8191,
            _ => max_bytes,
        }
    } else { max_bytes };

    if path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "runtime/widget.json 必须是 JSON 对象，无法下发到 P4".to_string())?;

        // Missing vars and an empty legacy array both mean no variables.
        let legacy_empty_vars = match object.get("vars") {
            None => true,
            Some(vars) if vars.is_object() => false,
            Some(vars) => vars.as_array().is_some_and(Vec::is_empty),
        };
        if legacy_empty_vars {
            object.insert("vars".to_string(), serde_json::json!({}));
        } else if !object.get("vars").is_some_and(Value::is_object) {
            return Err(
                "runtime/widget.json 的 vars 必须是以变量名为键的 JSON 对象；无变量时请使用 {}"
                    .to_string(),
            );
        }
        let quota_v3 = object.get("quota").is_some_and(|q| q == "p4-quota-v3");
        let quota_v2 = object.get("quota").is_some_and(|q| q == "p4-quota-v2");
        let vars=object.get("vars").and_then(Value::as_object).expect("normalized vars");
        // Reuse the shared structural validator without imposing its historical
        // 63-byte string limit on explicit v3. Only the validation copy changes;
        // the exact user strings remain in the outgoing payload.
        let mut checked=vars.clone();
        if quota_v3 {
            for (name, declaration) in &mut checked {
                if declaration["type"]=="string" {
                    if let Some(init)=declaration.get("init") {
                        let text=init.as_str().filter(|s|s.len()<=127&&!s.contains('\0'))
                            .ok_or_else(||format!("runtime/widget.json.vars.{name}.init 必须是最多 127 个 UTF-8 字节的字符串"))?;
                        let _=text;
                        declaration["init"]=Value::String(String::new());
                    }
                }
            }
        }
        validate_p4_vars_object_capped(
            &checked,
            if quota_v3 { 32 } else if quota_v2 { 16 } else { 8 },
        )?;

        if let Some(pages) = value.get_mut("pages").and_then(Value::as_array_mut) {
            for page in pages {
                if let Some(page) = page.as_object_mut() {
                    page.remove("label");
                }
            }
        }
    }

    if path == "buttons.json" {
        let bindings = value
            .as_array_mut()
            .ok_or_else(|| "buttons.json 必须是 JSON 数组，无法下发到 P4".to_string())?;
        bindings.retain(|binding| {
            !matches!(
                binding.get("action").and_then(Value::as_str),
                Some(
                    "page_toggle"
                        | "page_enter"
                        | "page_back"
                        | "page_main"
                        | "page_app"
                        | "component_center"
                )
            )
        });
    }

    if widget_id == "token-usage" && path == "runtime/widget.json" {
        let object = value
            .as_object_mut()
            .ok_or_else(|| "token-usage runtime/widget.json must be an object".to_string())?;
        object.remove("readers");
        object.remove("fetchers");
    }

    let compact = serde_json::to_vec(&value).map_err(|error| error.to_string())?;
    if compact.len() > max_bytes {
        return Err(format!(
            "{} is {} bytes after JSON compaction; P4 limit is {} bytes",
            path,
            compact.len(),
            max_bytes
        ));
    }
    Ok(compact)
}

#[test]
fn quota_v3_transaction_keeps_32_vars_127_utf8_bytes_and_exact_json_ceiling(){
    let mut vars=serde_json::Map::new();
    for i in 0..32{vars.insert(format!("v{i}"),serde_json::json!({"type":"string","init":"a".repeat(127)}));}
    let mut w=serde_json::json!({"engine":"p4-bounded-runtime-v4","quota":"p4-quota-v3","vars":vars});
    let send=|w:&Value|prepare_p4_widget_file("quota-probe","runtime/widget.json",&serde_json::to_vec(w).unwrap());
    let got:Value=serde_json::from_slice(&send(&w).unwrap()).unwrap();
    assert_eq!(got["vars"],w["vars"]);
    w["vars"]["v0"]["init"]=Value::String("界".repeat(42)+"a");assert!(send(&w).is_ok());
    w["vars"]["v0"]["init"]=Value::String("界".repeat(42)+"ab");assert!(send(&w).is_err());
    w["vars"]["v0"]["init"]=Value::String("a".repeat(127));
    w["vars"]["extra"]=serde_json::json!({"type":"int"});assert!(send(&w).is_err());
    w["vars"].as_object_mut().unwrap().remove("extra");
    w["quota"]="p4-quota-v2".into();assert!(send(&w).is_err());
    w["quota"]="p4-quota-v3".into();w["padding"]="".into();
    let base=serde_json::to_vec(&w).unwrap().len();w["padding"]=Value::String("x".repeat(12287-base));
    assert_eq!(send(&w).unwrap().len(),12287);
    w["padding"].as_str().map(str::to_owned).map(|s|w["padding"]=Value::String(s+"x"));
    assert!(send(&w).is_err());
    assert!(prepare_p4_widget_file("quota-probe","buttons.json",&serde_json::to_vec(&vec!["x".repeat(2048)]).unwrap()).is_err());
}
