/*
 * [Input] Desktop window, system tray, and explicit quit requests.
 * [Output] Close-to-background, restore/quit menu, bounded window sizing and
 *          existing transfer/unsaved-task protection on actual application exit.
 * [Pos] Desktop window and tray lifecycle shared by the Tauri event loop.
 * [Sync] If this file changes, update `pc/.folder.md` and background usage docs.
 */
use std::sync::{Mutex,atomic::{AtomicBool,Ordering}};
use tauri::{Manager, AppHandle};
use tauri::menu::{Menu, MenuItem, PredefinedMenuItem};
use tauri::tray::TrayIconBuilder;
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind};
static REASONS:Mutex<Vec<String>>=Mutex::new(Vec::new());
static PROMPT:AtomicBool=AtomicBool::new(false);
static APPROVED:AtomicBool=AtomicBool::new(false);
const TRAY_ID:&str="hachimidock-background";

#[cfg(target_os="macos")]
pub fn route_macos_quit(app:&AppHandle)->tauri::Result<()>{
    // Keep Tauri's default menu and all native Edit/Window/Services items.
    // Its predefined Quit sends terminate: directly, bypassing ExitRequested.
    let menu=app.menu().ok_or_else(||std::io::Error::new(std::io::ErrorKind::NotFound,"default application menu unavailable"))?;
    let quit_text=PredefinedMenuItem::quit(app,None)?.text()?;
    for item in menu.items()?{
        let Some(submenu)=item.as_submenu() else {continue;};
        for (index,old) in submenu.items()?.into_iter().enumerate(){
            if old.id().as_ref()=="guarded-app-quit"{return Ok(());}
            if let Some(predefined)=old.as_predefined_menuitem(){
                if predefined.text()?!=quit_text{continue;}
                let quit=MenuItem::with_id(app,"guarded-app-quit",quit_text.clone(),true,Some("CmdOrCtrl+Q"))?;
                submenu.insert(&quit,index)?;
                if let Err(error)=submenu.remove(&old){
                    let _=submenu.remove(&quit);
                    return Err(error);
                }
                app.on_menu_event(|app,event|{
                    if event.id.as_ref()=="guarded-app-quit"{app.exit(0);}
                });
                return Ok(());
            }
        }
    }
    Err(std::io::Error::new(std::io::ErrorKind::NotFound,"default Quit menu item unavailable").into())
}

pub fn init_tray(app:&AppHandle)->tauri::Result<()>{
    let open=MenuItem::with_id(app,"show-main","打开 HachimiDock",true,None::<&str>)?;
    let separator=PredefinedMenuItem::separator(app)?;
    let quit=MenuItem::with_id(app,"quit-app","退出 HachimiDock",true,None::<&str>)?;
    let menu=Menu::with_items(app,&[&open,&separator,&quit])?;
    let tray=TrayIconBuilder::with_id(TRAY_ID)
        .tooltip("HachimiDock · 后台运行")
        .menu(&menu)
        .show_menu_on_left_click(true)
        .on_menu_event(|app,event|match event.id.as_ref(){
            "show-main"=>show_main(app),
            "quit-app"=>app.exit(0),
            _=>{}
        });
    let icon=app.default_window_icon().ok_or_else(||std::io::Error::new(std::io::ErrorKind::NotFound,"default tray icon unavailable"))?;
    let tray=tray.icon(icon.clone()).build(app)?;
    // Keep an owned handle for the entire application lifetime.
    app.manage(tray);
    Ok(())
}
pub fn show_main(app:&AppHandle){
    if let Some(window)=app.get_webview_window("main"){
        #[cfg(target_os="macos")]
        let _=app.set_activation_policy(tauri::ActivationPolicy::Regular);
        fit(app,false);
        let _=window.unminimize();
        let _=window.show();
        let _=window.set_focus();
    }
}
pub fn hide_main(app:&AppHandle)->bool{
    // AppIndicator can build successfully without a visible tray host. Until
    // Linux has a reliable restore/quit entry, closing retains the exit guards.
    if cfg!(target_os="linux"){return false;}
    if app.tray_by_id(TRAY_ID).is_none(){return false;}
    if !app.get_webview_window("main").is_some_and(|window|window.hide().is_ok()){return false;}
    #[cfg(target_os="macos")]
    let _=app.set_activation_policy(tauri::ActivationPolicy::Accessory);
    true
}

#[tauri::command]
pub fn set_exit_reasons(reasons:Vec<String>){
    if let Ok(mut state)=REASONS.lock(){*state=reasons.into_iter().take(30).map(|s|s.chars().take(120).collect()).collect();}
}
pub fn request_exit(app:&AppHandle)->bool{
    if APPROVED.load(Ordering::SeqCst){return false;}
    let mut reasons=REASONS.lock().map(|s|s.clone()).unwrap_or_default();
    if app.state::<crate::usb_serial::UsbSerialManager>().transfer_in_progress(){reasons.push("设备仍在传输数据，请勿中断固件或资源写入".into());}
    if crate::realtime_chat::status().active{reasons.push("实时对话仍在进行".into());}
    if crate::component_media::is_playing() || crate::component_audio::active_for_exit(){reasons.push("组件音频仍在播放，退出将停止播放".into());}
    if reasons.is_empty(){return false;}
    if !PROMPT.swap(true,Ordering::SeqCst){
        show_main(app);
        let handle=app.clone();
        app.dialog().message(format!("{}\n\n退出会中断以上任务或丢弃未保存修改。是否仍要退出？",reasons.join("\n")))
            .title("退出 Pet Manager？").kind(MessageDialogKind::Warning)
            .buttons(MessageDialogButtons::OkCancelCustom("仍然退出".into(),"继续使用".into()))
            .show(move |yes|{PROMPT.store(false,Ordering::SeqCst);if yes{APPROVED.store(true,Ordering::SeqCst);handle.exit(0);}});
    }
    true
}
fn fit_size(width:f64,height:f64,scale:f64,wanted:(f64,f64))->((f64,f64),(f64,f64)){
    let scale=if scale.is_finite()&&scale>0.0{scale}else{1.0};
    let width=(width/scale-32.0).max(320.0);
    let height=(height/scale-64.0).max(240.0);
    let min=(900.0f64.min(width),540.0f64.min(height));
    ((wanted.0.max(min.0).min(width),wanted.1.max(min.1).min(height)),min)
}
pub fn fit(app:&AppHandle,initial:bool){
    if let Some(window)=app.get_webview_window("main"){
        let visible=window.is_visible().unwrap_or(false);
        if let Ok(Some(monitor))=window.current_monitor(){
            let area=monitor.work_area();
            let wanted=if initial{(1200.0,760.0)}else{window.inner_size().map(|s|(s.width as f64/monitor.scale_factor(),s.height as f64/monitor.scale_factor())).unwrap_or((1200.0,760.0))};
            let (size,min)=fit_size(area.size.width as f64,area.size.height as f64,monitor.scale_factor(),wanted);
            let _=window.set_min_size(Some(tauri::LogicalSize::new(min.0,min.1)));
            let _=window.set_size(tauri::LogicalSize::new(size.0,size.1));
            if initial{let _=window.center();}
        }
        if initial||visible{let _=window.show();}
    }
}
#[cfg(test)]mod tests{use super::*;
    #[test]fn high_dpi_windows_fit_work_area(){for scale in [1.0,1.25,1.5,2.0]{let(size,min)=fit_size(1366.0,728.0,scale,(1200.0,760.0));assert!(size.0*scale<1366.0);assert!(size.1*scale<728.0);assert!(min.0<=size.0&&min.1<=size.1);}}
    #[test]fn returning_to_low_dpi_keeps_size_above_minimum(){let(size,min)=fit_size(1920.0,1080.0,1.0,(600.0,350.0));assert_eq!(size,min);}
}
