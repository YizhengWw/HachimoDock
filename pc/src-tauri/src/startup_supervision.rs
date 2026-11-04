//! One native worker, one bounded policy and at most one native dialog.
//! No USB, provider actions, automatic reload, or mutation recovery/replay.
use std::sync::{Arc, Condvar, Mutex, atomic::{AtomicBool, Ordering}};
use std::time::{Duration, Instant};
use tauri::{AppHandle, Manager};
use tauri_plugin_dialog::{DialogExt, MessageDialogButtons, MessageDialogKind, MessageDialogResult};
use crate::startup_policy::{Policy, Choice, Effect};
struct Inner { policy: Policy, token: String }
struct Shared { state: Mutex<Inner>, changed: Condvar, clock: Instant, started: AtomicBool }
#[derive(Clone)]
pub struct Supervisor { shared: Arc<Shared> }
impl Default for Supervisor {
    fn default() -> Self {
        Self { shared: Arc::new(Shared { state:Mutex::new(Inner { policy:Policy::new(0),token:uuid::Uuid::new_v4().to_string() }), changed:Condvar::new(),clock:Instant::now(),started:AtomicBool::new(false) }) }
    }
}
impl Supervisor {
    fn now(&self) -> u64 { self.shared.clock.elapsed().as_millis().min(u128::from(u64::MAX)) as u64 }
    pub fn navigate(&self) {
        if let Ok(mut s)=self.shared.state.lock() {
            if s.policy.navigate(self.now()) {s.token=uuid::Uuid::new_v4().to_string();}
        }
        self.shared.changed.notify_one();
    }
    pub fn challenge(&self) -> Result<serde_json::Value,String> {
        let s=self.shared.state.lock().map_err(|_|"startup supervision unavailable")?;
        if s.policy.closed() {return Err("startup supervision closed".into());}
        Ok(serde_json::json!({"generation":s.policy.generation(),"token":s.token}))
    }
    pub fn ready(&self,generation:u64,token:&str) -> bool {
        let accepted=self.shared.state.lock().map(|mut s|s.token==token&&s.policy.ready(generation)).unwrap_or(false);
        self.shared.changed.notify_one();
        eprintln!("[startup-native] category={}",if accepted {"ready"} else {"stale_ready"});
        accepted
    }
    pub fn close(&self) {
        if let Ok(mut s)=self.shared.state.lock(){s.policy.close();}
        self.shared.changed.notify_one();
    }
    pub fn start(&self,app:AppHandle) -> std::io::Result<()> {
        if self.shared.started.swap(true,Ordering::AcqRel) { return Ok(()); }
        let this=self.clone();
        std::thread::Builder::new().name("startup-supervision".into()).spawn(move || loop {
            let notice={
                let Ok(mut s)=this.shared.state.lock() else {break;};
                if s.policy.closed(){break;}
                let notice=s.policy.poll(this.now());
                if notice.is_none(){
                    let Ok((s,_))=this.shared.changed.wait_timeout(s,Duration::from_millis(500)) else {break;};
                    if s.policy.closed(){break;}
                }
                notice
            };
            if let Some(generation)=notice {
                let supervisor=this.clone();let handle=app.clone();
                if app.run_on_main_thread(move || supervisor.present(handle,generation)).is_err(){
                    this.close();break;
                }
            }
        })?;
        Ok(())
    }
    fn present(&self,app:AppHandle,generation:u64) {
        let current=self.shared.state.lock().map(|s|s.policy.can_present(generation)).unwrap_or(false);
        if !current {
            if let Ok(mut s)=self.shared.state.lock(){s.policy.choose(generation,Choice::Dismiss,self.now());}
            self.shared.changed.notify_one();return;
        }
        eprintln!("[startup-native] category=ready_timeout");
        let supervisor=self.clone();let handle=app.clone();
        app.dialog().message("主窗口尚未确认界面就绪，可能是页面未加载或加载异常；原因尚未确定。\n\n仅重载界面不会重发同步、删除或其他操作。若刚有设备写入，请先等待并在恢复后核对设备清单；退出可能停止正在运行的任务。")
            .title("界面未就绪：请选择恢复方式")
            .kind(MessageDialogKind::Warning)
            .buttons(MessageDialogButtons::YesNoCancelCustom("仅重载界面".into(),"退出客户端".into(),"暂不处理".into()))
            .show_with_result(move |result| {
                let choice=match result {
                    MessageDialogResult::Yes => Choice::Reload,
                    MessageDialogResult::No => Choice::Exit,
                    MessageDialogResult::Custom(label) if label=="仅重载界面" => Choice::Reload,
                    MessageDialogResult::Custom(label) if label=="退出客户端" => Choice::Exit,
                    _ => Choice::Dismiss,
                };
                let supervisor=supervisor.clone();let action_handle=handle.clone();
                // Serialize validation and the chosen effect with native navigation callbacks.
                let _=handle.run_on_main_thread(move || {
                    let effect=if let Ok(mut s)=supervisor.shared.state.lock(){
                        let effect=s.policy.choose(generation,choice,supervisor.now());
                        if effect==Effect::Reload{s.token=uuid::Uuid::new_v4().to_string();}
                        effect
                    }else{Effect::Ignore};
                    supervisor.shared.changed.notify_one();
                    match effect {
                        Effect::Reload => {
                            eprintln!("[startup-native] category=user_reload");
                            if let Some(window)=action_handle.get_webview_window("main") {
                                if window.reload().is_err(){eprintln!("[startup-native] category=reload_failed");}
                            }
                        },
                        Effect::Exit => {
                            eprintln!("[startup-native] category=user_exit");
                            // Preserve the existing exit protection and runtime shutdown path.
                            action_handle.exit(0);
                        },
                        Effect::Ignore => {},
                    }
                });
            });
    }
}
