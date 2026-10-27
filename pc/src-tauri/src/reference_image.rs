//! Local-only photo picker/drop bridge. Native events contain normalized pixels, never source paths.
use tauri::WebviewWindow;

#[cfg(target_os = "macos")]
mod mac {
    use std::{ffi::{c_char, c_void, CStr, CString}, sync::OnceLock};
    use tauri::{AppHandle, Emitter, Manager, WebviewWindow};
    static APP: OnceLock<AppHandle> = OnceLock::new();
    extern "C" {
        fn reference_image_mount(view: *mut c_void, token: *const c_char, x: f64, y: f64,
            width: f64, height: f64, viewport: f64, callback: extern "C" fn(*const c_char));
        fn reference_image_action(view: *mut c_void, token: *const c_char, action: i32);
    }
    extern "C" fn event(json: *const c_char) {
        if json.is_null() { return; }
        let bytes = unsafe { CStr::from_ptr(json) }.to_bytes();
        if bytes.len() > 23 * 1024 * 1024 { return; }
        if let (Some(app), Ok(value)) = (APP.get(), serde_json::from_slice::<serde_json::Value>(bytes)) {
            if let Some(window) = app.get_webview_window("main") { let _ = window.emit("reference-image", value); }
        }
    }
    pub fn update(window: WebviewWindow, token: String, rect: Option<[f64; 5]>, action: i32) -> Result<(), String> {
        if window.label() != "main" { return Err("图片导入仅允许在主窗口使用".into()); }
        let token = CString::new(token).map_err(|_| "图片导入标识无效")?;
        let _ = APP.set(window.app_handle().clone());
        window.with_webview(move |webview| unsafe {
            if let Some([x, y, width, height, viewport]) = rect {
                reference_image_mount(webview.inner(), token.as_ptr(), x, y, width, height, viewport, event);
            } else { reference_image_action(webview.inner(), token.as_ptr(), action); }
        }).map_err(|_| "无法打开本机图片导入窗口".into())
    }
}

fn validate(token: &str, rect: Option<[f64; 5]>) -> Result<(), String> {
    if token.is_empty() || token.len() > 80 || !token.bytes().all(|c| c.is_ascii_alphanumeric() || c == b'-') {
        return Err("图片导入标识无效".into());
    }
    if let Some([x, y, w, h, viewport]) = rect {
        if ![x, y, w, h, viewport].iter().all(|v| v.is_finite() && v.abs() <= 100000.) || w < 0. || h < 0. || viewport <= 0. {
            return Err("图片导入区域无效".into());
        }
    }
    Ok(())
}

#[tauri::command]
pub fn reference_image_mount(window: WebviewWindow, token: String, rect: [f64; 5]) -> Result<bool, String> {
    validate(&token, Some(rect))?;
    #[cfg(target_os = "macos")]
    { mac::update(window, token, Some(rect), 0)?; Ok(true) }
    #[cfg(not(target_os = "macos"))]
    { let _ = window; Ok(false) }
}

#[tauri::command]
pub fn reference_image_action(window: WebviewWindow, token: String, action: String) -> Result<(), String> {
    validate(&token, None)?;
    let action = match action.as_str() { "file" => 0, "photos" => 1, "cancel" => 2, "unmount" => 3, _ => return Err("未知图片导入操作".into()) };
    #[cfg(target_os = "macos")]
    { mac::update(window, token, None, action) }
    #[cfg(not(target_os = "macos"))]
    { let _ = (window, action); Err("此系统请使用文件选择器导入图片".into()) }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn import_session_and_rect_are_bounded() {
        assert!(validate("photo-1", Some([0., 0., 320., 200., 1200.])).is_ok());
        for token in ["", "../path", "x\0y"] { assert!(validate(token, None).is_err()); }
        for rect in [[0., 0., -1., 3., 1200.], [0., 0., 1., 3., 0.], [f64::NAN, 0., 1., 3., 1200.]] { assert!(validate("photo-1", Some(rect)).is_err()); }
    }
}
