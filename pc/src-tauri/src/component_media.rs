//! Shared live-media service, instantiated per authorized component package.
//! Logs fixed-stage invalid ACK evidence, never session IDs or station content.
//! Confirmed device-side termination ends the old lease without poisoning re-entry.
use crate::media_network as network;
mod owner;
use crate::usb_serial::{UsbSerialManager, UsbConnectionStatus};
use base64::{engine::general_purpose::STANDARD as B64, Engine};
use network::Station;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::{
    collections::{HashMap, VecDeque},
    io::Read,
    path::PathBuf,
    process::{Command, Stdio},
    sync::{
        atomic::{AtomicBool, AtomicU64, Ordering},
        mpsc, Arc, Mutex, OnceLock,
    },
    time::{Duration, Instant},
};

#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
struct Snapshot {
    state: String,
    message: String,
    current: Option<Station>,
    stations: Vec<Station>,
    #[serde(default)]
    catalog: Vec<Station>,
    history: Vec<Station>,
    volume: u8,
    connected: bool,
    #[serde(default)]
    sleep_minutes: u32,
    #[serde(default)]
    automatic: bool,
    #[serde(default)]
    location: String,
    #[serde(default)]
    catalog_at: u64,
}
impl Default for Snapshot {
    fn default() -> Self {
        Self {
            state: "idle".into(),
            message: "连接设备后在组件内收听".into(),
            current: None,
            stations: vec![],
            catalog: vec![],
            history: vec![],
            volume: 35,
            connected: false,
            sleep_minutes: 0,
            automatic: false,
            location: String::new(),
            catalog_at: 0,
        }
    }
}
impl Snapshot {
    /// Favorites first; discovery never silently turns entries into favorites.
    fn queue(&self) -> Vec<Station> {
        let mut queue = Vec::new();
        for station in self.stations.iter().filter(|_|!self.automatic).chain(&self.catalog) {
            if !queue.iter().any(|s: &Station| s.id == station.id) {
                queue.push(station.clone());
            }
            if queue.len() == if self.automatic {crate::media_catalog::QUEUE_MAX}else{20} {
                break;
            }
        }
        queue
    }
}
// Bound Mac USB/WCH audio bursts to 40 ms; other platforms keep 80 ms.
// Keep the reservoir and initial buffering duration identical in PCM bytes.
const RADIO_PCM_FRAME_BYTES: usize = if cfg!(target_os = "macos") { 3840 } else { 7680 };
const RADIO_QUEUE_FRAMES: usize = (32 * 7680) / RADIO_PCM_FRAME_BYTES;
const RADIO_PREBUFFER_FRAMES: usize = (16 * 7680) / RADIO_PCM_FRAME_BYTES;

// A source interruption can reconnect. A device request with an indeterminate
// outcome cannot authorize cleanup, retransmission, or a replacement begin.
#[derive(Debug)]
enum RadioPlaybackError {
    Source(String),
    Stopped(String),
    DeviceUnknown(String),
}
impl RadioPlaybackError {
    fn message(&self) -> &str {
        match self { Self::Source(s) | Self::Stopped(s) | Self::DeviceUnknown(s) => s }
    }
}
impl std::fmt::Display for RadioPlaybackError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result { f.write_str(self.message()) }
}
impl From<String> for RadioPlaybackError { fn from(s: String) -> Self { Self::Stopped(s) } }
impl From<&str> for RadioPlaybackError { fn from(s: &str) -> Self { Self::Stopped(s.into()) } }
#[derive(Default)]
struct RadioDeviceState { unknown: AtomicBool }
impl RadioDeviceState {
    fn ensure_known(&self) -> Result<(), RadioPlaybackError> {
        if self.unknown.load(Ordering::Acquire) {
            Err(RadioPlaybackError::DeviceUnknown("设备播放结果尚未确认，未自动停止或重新播放".into()))
        } else { Ok(()) }
    }
    fn unproven(&self, reason: String) -> RadioPlaybackError {
        self.unknown.store(true, Ordering::Release);
        RadioPlaybackError::DeviceUnknown(reason)
    }
    fn request<T>(&self, send: impl FnOnce() -> Result<T, String>) -> Result<T, RadioPlaybackError> {
        self.ensure_known()?;
        send().map_err(|error| self.unproven(error))
    }
    fn status<'a>(&self, ack: &'a Value) -> Result<(u64, &'a str), RadioPlaybackError> {
        let buffered=ack["bufferedMs"].as_u64();let state=ack["state"].as_str();
        if ack["ok"]!=true || buffered.is_none() || !matches!(state,
            Some("playing"|"buffering"|"paused"|"ended"|"idle"|"interrupted"|"error")) {
            return Err(self.unproven("设备播放状态回执无效，未自动停止或重新播放".into()));
        }
        Ok((buffered.unwrap(),state.unwrap()))
    }
    fn finish<T>(&self, result: Result<T, RadioPlaybackError>, stop: impl FnOnce() -> Result<(), String>) -> Result<T, RadioPlaybackError> {
        // Retain the original reason, but never send stop once ownership is unknown.
        if let Err(RadioPlaybackError::DeviceUnknown(_)) = &result {
            self.unknown.store(true, Ordering::Release);return result;
        }
        self.ensure_known()?;
        self.request(stop)?;
        result
    }
}
fn run_radio_attempts(mut attempt: impl FnMut(usize) -> Result<(), RadioPlaybackError>) -> Result<(), RadioPlaybackError> {
    for number in 0..=3 {
        match attempt(number) {
            Err(RadioPlaybackError::Source(_)) if number < 3 => {},
            result => return result,
        }
    }
    unreachable!("bounded radio attempt loop always returns")
}

#[derive(Clone, Debug, PartialEq, Eq)]
struct RadioConnectionBinding {
    scope: owner::Scope,
    connection: String,
}
fn radio_connection_binding(status: &UsbConnectionStatus) -> Result<RadioConnectionBinding, String> {
    if !status.connected || status.connection_id.parse::<u64>().ok().filter(|id| *id > 0).is_none() {
        return Err("电台连接尚未确认".into());
    }
    Ok(RadioConnectionBinding {
        scope: owner::Scope::new(&status.board_device_id, &status.boot_id)?,
        connection: status.connection_id.clone(),
    })
}
fn radio_device_state_for_binding(owner: &owner::Guard, binding: Option<&RadioConnectionBinding>) -> RadioDeviceState {
    let state = RadioDeviceState::default();
    if binding.is_some_and(|binding| owner.ensure_known(&binding.scope).is_err()) {
        let _ = state.unproven("设备播放结果尚未确认，未自动停止或重新播放".into());
    }
    state
}

struct Service {
    id: String,
    source: String,
    version: String,
    extension: crate::component_extension::Extension,
    usb: UsbSerialManager,
    dir: PathBuf,
    data: Mutex<Snapshot>,
    results: Mutex<HashMap<String, Station>>,
    generation: AtomicU64,
    operation: Mutex<()>,
    transport: Mutex<()>,
    session: Mutex<String>,
    device_state: RadioDeviceState,
    binding: Mutex<Option<RadioConnectionBinding>>,
    owner: Arc<owner::Guard>,
    sleep_until: Mutex<Option<Instant>>,
    search_lock: Mutex<Instant>,
    discovering: AtomicBool,
    discovery_at: Mutex<Instant>,
    visibility: Mutex<(u64, u64, bool, Instant)>,
    pending_auto: AtomicBool,
}
struct Host {
    owner: Arc<owner::Guard>,
    root: PathBuf,
    usb: UsbSerialManager,
    services: Mutex<HashMap<String, Arc<Service>>>,
    operation: Mutex<()>,
    requests: Mutex<()>,
}
static HOST: OnceLock<Host> = OnceLock::new();
pub(crate) fn is_playing()->bool{
    HOST.get().is_some_and(|host| host.services.try_lock().map(|services|
        services.values().any(|service| service.data.try_lock().map(|data|
            matches!(data.state.as_str(),"playing"|"buffering"|"starting")).unwrap_or(true))).unwrap_or(true))
}
// Favorites + catalog + history can contain 50 entries with bounded URLs.
const MAX_LIBRARY_BYTES: u64 = 1024 * 1024;
fn unix_now() -> u64 {std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs()}
fn service(id: &str) -> Result<Arc<Service>, String> {
    HOST.get()
        .ok_or("组件媒体服务尚未初始化")?
        .services
        .lock()
        .unwrap()
        .get(id)
        .cloned()
        .ok_or_else(|| "请先在组件详情启用 PC 扩展".into())
}
// App-private durable owner evidence; never read a user/component-supplied path.
struct OwnerJournal { path: PathBuf, lease: Option<std::fs::File> }
impl OwnerJournal {
    fn open(path:PathBuf)->Self {
        let lease=owner::journal_lock(&path.with_extension("lock")).ok();
        Self {path,lease}
    }
}
impl owner::Storage for OwnerJournal {
    fn read(&self) -> Result<Vec<owner::Record>, String> {
        if self.lease.is_none(){return Err("电台状态记录尚未独占".into());}
        let metadata=match std::fs::symlink_metadata(&self.path) {
            Ok(value)=>value,Err(e) if e.kind()==std::io::ErrorKind::NotFound=>return Ok(Vec::new()),
            Err(_)=>return Err("电台状态记录不可读".into()),
        };
        if !metadata.is_file()||metadata.len()>65536 {return Err("电台状态记录无效".into());}
        let mut options=std::fs::OpenOptions::new();options.read(true);
        #[cfg(unix)] {use std::os::unix::fs::OpenOptionsExt;options.custom_flags(libc::O_NOFOLLOW|libc::O_NONBLOCK);}
        let file=options.open(&self.path).map_err(|_|"电台状态记录不可读")?;
        if !file.metadata().map_err(|_|"电台状态记录不可读")?.is_file() {return Err("电台状态记录无效".into());}
        let mut bytes=Vec::new();file.take(65537).read_to_end(&mut bytes).map_err(|_|"电台状态记录不可读")?;
        if bytes.len()>65536 {return Err("电台状态记录超限".into());}
        let data:Value=serde_json::from_slice(&bytes).map_err(|_|"电台状态记录无效")?;
        if data["schema"]!=1||!data.as_object().is_some_and(|v|v.len()==2) {return Err("电台状态记录版本无效".into());}
        let rows=data["records"].as_array().filter(|v|v.len()<=128).ok_or("电台状态记录无效")?;
        rows.iter().map(|r| {
            if !r.as_object().is_some_and(|v|v.len()==5) {return Err("电台状态记录无效".into());}
            Ok(owner::Record {
                scope:owner::Scope::new(r["board"].as_str().ok_or("电台状态记录无效")?,r["boot"].as_str().ok_or("电台状态记录无效")?)?,
                unresolved:r["unresolved"].as_bool().ok_or("电台状态记录无效")?,
                owner:r["owner"].as_str().ok_or("电台状态记录无效")?.into(),
                session:r["session"].as_str().ok_or("电台状态记录无效")?.into(),
            })
        }).collect()
    }
    fn write(&self, records:&[owner::Record])->Result<(),String> {
        if self.lease.is_none(){return Err("电台状态记录尚未独占".into());}
        let parent=self.path.parent().ok_or("电台状态目录无效")?;
        std::fs::create_dir_all(parent).map_err(|_|"电台状态记录无法保存")?;
        let value=json!({"schema":1,"records":records.iter().map(|r|json!({"board":r.scope.board,"boot":r.scope.boot,"unresolved":r.unresolved,"owner":r.owner,"session":r.session})).collect::<Vec<_>>()});
        let bytes=serde_json::to_vec(&value).map_err(|_|"电台状态记录无法保存")?;
        if bytes.len()>65536 {return Err("电台状态记录超限".into());}
        let mut temporary=tempfile::NamedTempFile::new_in(parent).map_err(|_|"电台状态记录无法保存")?;
        std::io::Write::write_all(&mut temporary,&bytes).and_then(|_|temporary.as_file().sync_all()).map_err(|_|"电台状态记录无法保存")?;
        let file=temporary.persist(&self.path).map_err(|_|"电台状态记录无法保存")?;
        file.sync_all().map_err(|_|"电台状态记录无法保存")?;
        #[cfg(unix)] std::fs::File::open(parent).and_then(|f|f.sync_all()).map_err(|_|"电台状态记录无法保存")?;
        Ok(())
    }
}
impl Service {
    fn owner_scope(&self) -> Result<(owner::Scope,String),String> {
        let current = radio_connection_binding(&self.usb.status())?;
        let mut binding = self.binding.lock().map_err(|_| "电台连接状态不可用")?;
        if binding.as_ref().is_some_and(|prior| prior != &current) {
            return Err("电台连接已变化，未发送旧播放操作".into());
        }
        if binding.is_none() { *binding = Some(current.clone()); }
        Ok((current.scope, current.connection))
    }
    fn binding_changed(&self) -> bool {
        let Ok(current) = radio_connection_binding(&self.usb.status()) else { return false; };
        self.binding.lock().ok().and_then(|binding| binding.clone())
            .is_some_and(|prior| prior != current)
    }
    fn poison_bound_owner(&self) {
        if let Some(binding) = self.binding.lock().ok().and_then(|binding| binding.clone()) {
            self.owner.poison_scoped_owner(&self.id, &binding.scope);
        }
    }
    fn retire_for_connection_change(&self) {
        // Local retirement only: a reconnect is not an idle/stop receipt.
        // Retain unresolved old board+boot ownership; never stop via a new handle.
        self.pending_auto.store(false, Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst);
        self.poison_bound_owner();
    }
    fn ensure_owner(&self)->Result<(),String> {let (scope,_)=self.owner_scope()?;self.owner.ensure_known(&scope)}
    fn media_wire(&self, board:&str, topic:&str, payload:Value)->Result<owner::Outcome<Value>,String> {
        let (scope,connection)=self.owner_scope()?;
        if scope.board!=board {return Err("电台连接主体已变化，未发送操作".into());}
        let operation=match topic {
            "media/library"=>owner::Op::Library,"media/begin"=>owner::Op::Begin,
            "media/chunk"=>owner::Op::Chunk,"media/query"=>owner::Op::Query,
            "media/stop"=>owner::Op::Stop,_=>return Err("电台操作未准入".into()),
        };
        let session=payload["sessionId"].as_str().unwrap_or("");
        self.owner.request_observed(&scope,&self.id,session,operation,|| {
            if self.owner_scope()? != (scope.clone(),connection.clone()) {return Err("电台连接已变化".into());}
            let generation=connection.parse::<u64>().ok().filter(|v|*v>0).ok_or("电台连接代次无效")?;
            let ack=self.usb.media_status_for_connection(board,generation,topic,payload.clone())?;
            if self.owner_scope()? != (scope.clone(),connection.clone()) {return Err("电台连接已变化".into());}
            if owner::stopped_rejection(operation,ack["ok"].as_bool(),ack["bufferedMs"].as_u64(),ack["state"].as_str(),ack["sessionId"].as_str()) {
                crate::realtime_chat_log::record("", "radio_device_stopped", json!({"stage":"correlated_terminal_rejection"}));
                return Ok(owner::Reply::DeviceStopped(ack));
            }
            if owner::rejected_active(operation,session,ack["ok"].as_bool(),ack["bufferedMs"].as_u64(),
                ack["state"].as_str(),ack["sessionId"].as_str(),ack["message"].as_str()) {
                crate::realtime_chat_log::record("", "radio_chunk_rejected", json!({"stage":"known_active_rejection"}));
                return Ok(owner::Reply::RejectedActive(ack));
            }
            // Validate before shared admission releases; a later Service cannot
            // slip past a malformed status accepted only by the wire layer.
            if let Err(error)=owner::validate_status(operation,session,ack["ok"].as_bool(),ack["bufferedMs"].as_u64(),ack["state"].as_str(),ack["sessionId"].as_str()) {
                crate::realtime_chat_log::record("", "radio_ack_invalid", json!({
                    "stage":match operation {owner::Op::Begin=>"begin",owner::Op::Chunk=>"chunk",owner::Op::Query=>"query",owner::Op::Stop=>"stop",owner::Op::Library=>"library"},
                    "ok":ack["ok"].as_bool(),"count":(ack["bufferedMs"].as_u64().is_some() as u8)
                        | ((ack["sessionId"].as_str().is_some() as u8)<<1) | (((ack["sessionId"].as_str()==Some(session)) as u8)<<2)
                }));
                return Err(error);
            }
            Ok(owner::Reply::Accepted(ack))
        })
    }
    fn media_ack(&self,board:&str,topic:&str,payload:Value)->Result<Value,RadioPlaybackError> {
        self.device_state.ensure_known()?;
        match self.media_wire(board,topic,payload).map_err(|e|self.device_state.unproven(e))? {
            owner::Outcome::Sent(ack)=>Ok(ack),
            // Never call a rejected chunk successful or replay it. The current
            // owned session must receive its own stop ACK before a fresh begin.
            owner::Outcome::RejectedActive(_)=>Err(RadioPlaybackError::Source("设备拒绝当前音频块，正在恢复播放".into())),
            owner::Outcome::DeviceStopped(_)=>Err(RadioPlaybackError::Stopped("设备已结束电台播放，可重新进入收听".into())),
            owner::Outcome::KnownIdle|owner::Outcome::Retired=>Err(RadioPlaybackError::Stopped("没有新的设备回执".into())),
        }
    }
    fn media_stop(&self,board:&str,payload:Value)->Result<(),String> {
        // KnownIdle/Retired are local no-wire outcomes, never synthesized into an ACK.
        self.media_wire(board,"media/stop",payload).map(|_|())
    }
    // Only a new explicit user play may reconcile a prior uncertain session.
    // This sends one read-only query, with ownership held across the observation.
    fn reconcile_for_explicit_play(&self, board: &str) -> Result<(), String> {
        self.pending_auto.store(false,Ordering::SeqCst);
        self.generation.fetch_add(1,Ordering::SeqCst);
        let _wire=self.transport.lock().map_err(|_|"电台传输状态不可用")?;
        let (scope,connection)=self.owner_scope()?;
        if scope.board!=board {return Err("电台连接主体已变化，未核对旧播放".into());}
        let active=self.owner.reconcile_unknown(&scope,&self.id,|old_session| {
            if self.owner_scope()? != (scope.clone(),connection.clone()) {return Err("电台连接已变化".into());}
            let generation=connection.parse::<u64>().ok().filter(|v|*v>0).ok_or("电台连接代次无效")?;
            let ack=self.usb.media_status_for_connection(board,generation,"media/query",json!({"sessionId":old_session}))?;
            if self.owner_scope()? != (scope.clone(),connection.clone()) {return Err("电台连接已变化".into());}
            Ok(owner::RecoveryProof {
                ok:ack["ok"].as_bool(),buffered_ms:ack["bufferedMs"].as_u64(),
                state:ack["state"].as_str().map(str::to_owned),session_id:ack["sessionId"].as_str().map(str::to_owned),
                message:ack["message"].as_str().map(str::to_owned),
            })
        })?;
        if self.owner_scope()? != (scope.clone(),connection.clone()) {
            self.owner.poison_scoped_owner(&self.id,&scope);
            return Err(self.device_state.unproven("电台连接已变化，未恢复旧播放".into()).to_string());
        }
        *self.session.lock().map_err(|_|"电台会话状态不可用")?=active.unwrap_or_default();
        self.device_state.unknown.store(false,Ordering::Release);
        crate::realtime_chat_log::record("", "radio_manual_reconciled",json!({"stage":"read_only_current_scope"}));
        Ok(())
    }
    fn confirm_idle_for_retry(&self, board: &str) -> Result<(), RadioPlaybackError> {
        self.device_state.ensure_known()?;
        let (scope, connection) = self.owner_scope()?;
        if scope.board != board { return Err("电台连接主体已变化，未自动换台".into()); }
        self.owner.observe_idle(&scope, || {
            let generation = connection.parse::<u64>().map_err(|_| "电台连接代次无效")?;
            let ack = self.usb.media_request_for_connection(board, generation, "media/query", json!({}))?;
            if self.owner_scope()? != (scope.clone(), connection.clone()) {
                return Err("电台连接已变化，未自动换台".into());
            }
            owner::validate_status(owner::Op::Stop, "", ack["ok"].as_bool(),
                ack["bufferedMs"].as_u64(), ack["state"].as_str(), ack["sessionId"].as_str())
        }).map_err(RadioPlaybackError::Stopped)
    }
    fn alive(&self, g: u64) -> bool {
        self.generation.load(Ordering::SeqCst) == g
    }
    fn snapshot(&self) -> Value {
        let mut s = self.data.lock().unwrap().clone();
        let board = self.usb.status();
        s.connected = board.connected;
        let mut value = json!(s);
        value["queue"] = json!(s.queue());
        value["discovering"] = self.discovering.load(Ordering::SeqCst).into();
        value["enabled"] = true.into();
        value["supported"] =
            (board.capabilities["widgetMediaSurface"] == "p4-media-surface-v1" && (!s.automatic || (board.capabilities["widgetMediaQueueMax"].as_u64().unwrap_or(20)>=128 && board.capabilities["widgetMediaLifecycle"]=="p4-media-lifecycle-v1"))).into();
        value
    }
    fn save(&self) -> Result<(), String> {
        let s = self.data.lock().unwrap().clone();
        std::fs::create_dir_all(&self.dir).map_err(|_| "无法保存电台设置")?;
        let mut file =
            tempfile::NamedTempFile::new_in(&self.dir).map_err(|_| "无法保存电台设置")?;
        serde_json::to_writer(&mut file, &s).map_err(|_| "无法保存电台设置")?;
        file.persist(self.dir.join("library.json"))
            .map_err(|_| "无法保存电台设置")?;
        Ok(())
    }
    fn library(&self) -> Result<(), String> {
        self.device_state.ensure_known().map_err(|e|e.to_string())?;
        let board = self.usb.status();
        if !board.connected || board.capabilities["widgetMediaSurface"] != "p4-media-surface-v1" {
            return Ok(());
        }
        self.ensure_owner()?;
        // Editing an inactive component must not replace another owner's queue or volume.
        if HOST.get().is_some_and(|h| {
            h.services.lock().unwrap().values().any(|s| {
                s.id != self.id
                    && matches!(
                        s.data.lock().unwrap().state.as_str(),
                        "playing" | "buffering"
                    )
            })
        }) {
            return Ok(());
        }
        let s = self.data.lock().unwrap().clone();
        let queue = s.queue();
        let maximum=board.capabilities["widgetMediaQueueMax"].as_u64().unwrap_or(20) as usize;
        if queue.len()>maximum {return Err("请升级固件后使用完整自动电台列表".into());}
        let _wire=self.transport.lock().unwrap();
        self.media_ack(&board.board_device_id,"media/library",json!({"source":self.source,"mode":"live","queue":queue.iter().map(|t|crate::widget_data::bounded_text(&t.name,90)).collect::<Vec<_>>(),"keys":queue.iter().map(|t|&t.id).collect::<Vec<_>>(),"volume":s.volume})).map_err(|e|e.to_string())?;
        Ok(())
    }
    fn ensure_catalog(self: &Arc<Self>) {
        let Some(query) = self.extension.search.browse_query.clone() else {
            return;
        };
        {let data=self.data.lock().unwrap();
        if !data.catalog.is_empty() && self.extension.discovery.as_ref().is_none_or(|d|data.catalog_at>0&&unix_now().saturating_sub(data.catalog_at)<d.refresh_sec) {return;}}
        let mut last = self.discovery_at.lock().unwrap();
        if last.elapsed() < Duration::from_secs(15) || self.discovering.swap(true, Ordering::SeqCst)
        {
            return;
        }
        *last = Instant::now();
        drop(last);
        let s = self.clone();
        std::thread::spawn(move || {
            // Network work never holds the host/operation locks: stop and revoke stay responsive.
            let result = if s.extension.discovery.is_some(){crate::media_catalog::discover(&s.extension)}else{network::search(&s.extension,&query).map(|stations|crate::media_catalog::Catalog{stations,location:String::new()})};
            let Some(host) = HOST.get() else {
                s.discovering.store(false, Ordering::SeqCst);
                return;
            };
            let _owner = host.operation.lock().unwrap();
            let _op = s.operation.lock().unwrap();
            if host
                .services
                .lock()
                .unwrap()
                .get(&s.id)
                .is_none_or(|current| !Arc::ptr_eq(current, &s))
            {
                s.discovering.store(false, Ordering::SeqCst);
                return;
            }
            let notice = match result {
                Ok(catalog) => {
                    let empty = catalog.stations.is_empty();
                    {let mut data=s.data.lock().unwrap();data.catalog=catalog.stations;data.location=catalog.location;data.catalog_at=unix_now();}
                    let result = s.save().and_then(|_| s.library());
                    result.err().unwrap_or_else(|| {
                        if empty {
                            "暂未找到电台，请稍后重新打开列表".into()
                        } else {
                            if s.extension.discovery.is_some(){"电台已自动备好，进入设备组件收听".into()}else{"电台列表已就绪，选择后收听".into()}
                        }
                    })
                }
                Err(error) => format!("电台列表加载失败：{error}；稍后重新打开列表重试"),
            };
            s.discovering.store(false, Ordering::SeqCst);
            s.maybe_auto();
            let mut data = s.data.lock().unwrap();
            if data.state == "idle" {
                data.state = "idle".into();
                data.message = notice.clone();
                drop(data);
                device_notice(&s.id, "idle", &notice);
            }
        });
    }
    fn stop(&self, state: &str, message: &str) {
        self.pending_auto.store(false,Ordering::SeqCst);
        self.generation.fetch_add(1, Ordering::SeqCst);
        let _wire = self.transport.lock().unwrap();
        let id = self.session.lock().unwrap().clone();
        if !id.is_empty() {
            if self.usb.status().connected {
                let _ = self.device_state.request(|| self.media_stop(
                    &self.usb.status().board_device_id, json!({"sessionId":id}),
                ));
            } else {
                // Disconnect proves no stop receipt, not that this session ended.
                self.poison_bound_owner();
                let _ = self.device_state.unproven("连接已断开，设备播放结果尚未确认；未自动停止或重新播放".into());
            }
        }
        if self.device_state.ensure_known().is_ok() { self.session.lock().unwrap().clear(); }
        let mut s = self.data.lock().unwrap();
        if let Err(error)=self.device_state.ensure_known() {
            s.state="error".into();s.message=error.to_string();
        } else { s.state = state.into();s.message = message.into(); }
    }
    /// Only a current device visibility epoch can authorize delayed automatic playback.
    fn maybe_auto(self:&Arc<Self>) {
        if self.device_state.ensure_known().is_err(){self.pending_auto.store(false,Ordering::SeqCst);return;}
        if !self.pending_auto.load(Ordering::SeqCst){return;}
        let visible=self.visibility.lock().unwrap();
        if !visible.2 || visible.3.elapsed()>Duration::from_secs(6) || !self.usb.status().connected {return;}
        drop(visible);
        let data=self.data.lock().unwrap().clone();
        if matches!(data.state.as_str(),"playing"|"buffering") {self.pending_auto.store(false,Ordering::SeqCst);return;}
        let queue=data.queue();
        let selected=data.current.and_then(|old|queue.iter().find(|s|s.id==old.id).cloned()).or_else(||queue.first().cloned());
        if let Some(station)=selected {
            crate::realtime_chat_log::record("","radio_auto_play",json!({"stage":"start","count":queue.len()}));
            self.pending_auto.store(false,Ordering::SeqCst);
            if let Err(e)=self.library().and_then(|_|self.start_with_policy(station,true)){
                crate::realtime_chat_log::record("","radio_auto_play",json!({"stage":"start","ok":false,"errorKind":crate::realtime_chat_log::error_kind(&e)}));
                self.data.lock().unwrap().message=e;
            }
        }
    }
    fn start(self: &Arc<Self>, station: Station) -> Result<(), String> {
        self.start_with_policy(station,false)
    }
    fn start_with_policy(self: &Arc<Self>, station: Station, auto_fallback:bool) -> Result<(), String> {
        if !self.extension.allows_station(&station) {
            return Err("该电台已不在当前组件目录中，请从当前列表重新选择；原收藏和历史仍保留".into());
        }
        let board = self.usb.status();
        if !board.connected {
            return Err("请先连接设备".into());
        }
        if board.capabilities["widgetMediaSurface"] != "p4-media-surface-v1" {
            return Err("请先更新支持此媒体组件的固件".into());
        }
        if crate::realtime_chat::status().active {
            return Err("请先结束实时对话，再播放电台".into());
        }
        if !auto_fallback && (self.device_state.ensure_known().is_err() || self.ensure_owner().is_err()) {
            self.reconcile_for_explicit_play(&board.board_device_id)?;
        }
        self.device_state.ensure_known().map_err(|e|e.to_string())?;
        self.ensure_owner()?;
        self.stop("buffering", "正在连接电台");
        self.device_state.ensure_known().map_err(|e|e.to_string())?;
        device_notice(&self.id, "buffering", "正在连接电台，请稍候");
        let g = self.generation.load(Ordering::SeqCst);
        {
            let mut s = self.data.lock().unwrap();
            s.current = Some(station.clone());
            s.history.retain(|t| t.id != station.id);
            s.history.insert(0, station.clone());
            s.history.truncate(10);
        }
        if let Err(e) = self.save() {
            self.stop("error", &e);
            return Err(e);
        }
        let s = self.clone();
        std::thread::spawn(move || {
            // Same dedicated-thread scheduling protection as realtime speech.
            // Without it macOS coalesces every 250 us UART pacing sleep, making
            // an 80 ms audio packet take >100 ms even with decoded PCM ready.
            #[cfg(target_os = "macos")]
            let _activity = crate::realtime_audio_macos::AudioActivity::begin_on_player_thread();
            let mut station=station;
            let candidates=s.data.lock().unwrap().queue();
            let mut tried=std::collections::HashSet::from([station.id.clone()]);
            let mut ever_played=false;
            let result=run_radio_attempts(|attempt| {
                if !s.alive(g) || crate::realtime_chat::status().active || !s.usb.status().connected {
                    return Err(RadioPlaybackError::Stopped("电台播放已取消或连接已变化".into()));
                }
                if attempt > 0 {
                    // Only a typed current-scope idle response can authorize a
                    // replacement source. Unknown/active/read failure never retries.
                    s.confirm_idle_for_retry(&board.board_device_id)?;
                    if auto_fallback && !ever_played {
                        if let Some(next)=candidates.iter().find(|t|!tried.contains(&t.id)) {
                            station=next.clone();tried.insert(station.id.clone());
                            s.data.lock().unwrap().current=Some(station.clone());
                            let _=s.save();
                        }
                    }
                    {
                        let mut d=s.data.lock().unwrap();
                        if !s.alive(g) { return Err(RadioPlaybackError::Stopped("播放已取消".into())); }
                        d.state="buffering".into();d.message=format!("直播中断，正在重连（{}/3）",attempt);
                    }
                    for _ in 0..10*attempt {
                        if !s.alive(g) { return Err(RadioPlaybackError::Stopped("播放已取消".into())); }
                        std::thread::sleep(Duration::from_millis(100));
                    }
                }
                let result=s.play(&station,g,&board.board_device_id);
                ever_played|=s.data.lock().unwrap().state=="playing";
                if let Err(error)=&result {
                    crate::realtime_chat_log::record("", "component_media_retry", json!({
                        "retries":attempt,"errorKind":crate::realtime_chat_log::error_kind(error.message()),
                        "stage":"playback","ok":false
                    }));
                }
                result
            });
            if s.alive(g) {
                let mut state = s.data.lock().unwrap();
                if !s.alive(g) {
                    return;
                }
                state.state = "error".into();
                state.message = result
                    .err().map(|e|e.to_string())
                    .unwrap_or_else(|| "直播已结束，请重新播放".into());
                let message = state.message.clone();
                drop(state);
                if s.device_state.ensure_known().is_ok() { device_notice(&s.id, "error", &message); }
            }
        });
        Ok(())
    }
    fn play(self: &Arc<Self>, station: &Station, g: u64, board: &str) -> Result<(), RadioPlaybackError> {
        self.device_state.ensure_known()?;
        if !self.extension.allows_station(station) {
            return Err("该电台已不在当前组件目录中".into());
        }
        let mut cmd = Command::new(crate::codex_import::resolve_ffmpeg()?);
        cmd.args([
            "-hide_banner",
            "-loglevel",
            "error",
            "-nostdin",
            "-protocol_whitelist",
            "pipe",
            "-i",
            "pipe:0",
            "-vn",
            "-ac",
            "1",
            "-ar",
            "48000",
            "-f",
            "s16le",
            "pipe:1",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            cmd.creation_flags(0x08000000);
        }
        let mut child = cmd.spawn().map_err(|_| "无法启动电台解码器")?;
        let input = child.stdin.take().ok_or("解码输入不可用")?;
        let mut output = child.stdout.take().ok_or("解码输出不可用")?;
        // Bounded 2.56 s PCM reservoir on both platforms; Mac uses shorter wire frames.
        let (tx, rx) = mpsc::sync_channel::<Result<Vec<u8>, String>>(RADIO_QUEUE_FRAMES);
        let net = self.clone();
        let station = station.clone();
        let errors = tx.clone();
        std::thread::spawn(move || {
            let r = network::feed(&station, input, || net.alive(g));
            if let Err(e) = r {
                let _ = errors.send(Err(e));
            }
        });
        let decode = self.clone();
        std::thread::spawn(move || {
            while decode.alive(g) {
                let mut frame = vec![0; RADIO_PCM_FRAME_BYTES];
                if output.read_exact(&mut frame).is_err() {
                    break;
                }
                if tx.send(Ok(frame)).is_err() {
                    break;
                }
            }
        });
        let id = format!("music-extension-{}", uuid::Uuid::new_v4());
        let result = (|| {
            // Do not seize the codec until actual decoded audio is available.
            let mut pending = VecDeque::new();
            let first = Instant::now();
            // Gather 1.28 s before acquiring the device codec. Network/TLS
            // scheduling must not starve the sub-second USB playback reservoir.
            while self.alive(g) && pending.len() < RADIO_PREBUFFER_FRAMES {
                match rx.recv_timeout(Duration::from_millis(100)) {
                    Ok(r) => pending.push_back(r.map_err(RadioPlaybackError::Source)?),
                    Err(mpsc::RecvTimeoutError::Timeout)
                        if first.elapsed() < Duration::from_secs(35) => {}
                    _ => return Err(RadioPlaybackError::Source("电台没有返回可播放的音频".into())),
                }
            }
            {
                let host = HOST.get().ok_or("组件服务尚未初始化")?;
                let _owner = host.operation.lock().unwrap();
                if !self.alive(g) {
                    return Ok(());
                }
                for other in host.services.lock().unwrap().values() {
                    if other.id != self.id
                        && matches!(
                            other.data.lock().unwrap().state.as_str(),
                            "playing" | "buffering"
                        )
                    {
                        other.stop("idle", "其他组件正在使用音频");
                        other.device_state.ensure_known().map_err(|e|RadioPlaybackError::DeviceUnknown(e.to_string()))?;
                    }
                }
                let _wire = self.transport.lock().unwrap();
                if !self.alive(g) {
                    return Ok(());
                }
                let s = self.data.lock().unwrap().clone();
                *self.session.lock().unwrap() = id.clone();
                let queue = s.queue();
                let begin_ack=self.media_ack(board,"media/begin",json!({"source":self.source,"mode":"live","sessionId":id,"sampleRate":48000,"format":"ima-block-v1","durationMs":0,"offsetMs":0,"volume":s.volume,"title":crate::widget_data::bounded_text(&s.current.as_ref().map(|t|t.name.as_str()).unwrap_or(""),120),"artist":crate::widget_data::bounded_text(&s.current.as_ref().map(|t|t.city.as_str()).unwrap_or(""),90),"queue":queue.iter().map(|t|crate::widget_data::bounded_text(&t.name,90)).collect::<Vec<_>>(),"keys":queue.iter().map(|t|&t.id).collect::<Vec<_>>()}))?;
                self.device_state.status(&begin_ack)?;
            }
            let mut seq = 0;
            let mut buffered = 0;
            let mut last = Instant::now();
            #[cfg(test)]
            let stream_start = Instant::now();
            #[cfg(test)]
            let mut starving = 0;
            while self.alive(g) {
                if crate::realtime_chat::status().active {
                    return Err("语音优先，电台已停止".into());
                }
                let mut frame = None;
                if buffered < 650 {
                    frame = if !pending.is_empty() {
                        pending.pop_front()
                    } else {
                        match rx.recv_timeout(Duration::from_millis(80)) {
                            Ok(r) => Some(r.map_err(RadioPlaybackError::Source)?),
                            Err(mpsc::RecvTimeoutError::Timeout) => {
                                #[cfg(test)]
                                {
                                    starving += 1;
                                }
                                None
                            }
                            Err(_) => return Err(RadioPlaybackError::Source("直播连接中断，请重新播放".into())),
                        }
                    };
                } else {
                    std::thread::sleep(Duration::from_millis(40));
                }
                let _wire = self.transport.lock().unwrap();
                if !self.alive(g) {
                    break;
                }
                let ack = if let Some(pcm) = frame {
                    let data = crate::media_codec::encode(&pcm)?;
                    let ack = self.media_ack(
                        board,
                        "media/chunk",
                        json!({"sessionId":id,"seq":seq,"data":B64.encode(data)}),
                    )?;
                    seq += 1;
                    last = Instant::now();
                    ack
                } else {
                    self.media_ack(board, "media/query", json!({"sessionId":id}))?
                };
                if !self.alive(g) {
                    break;
                }
                let status=self.device_state.status(&ack)?;
                buffered=status.0;
                let state=status.1;
                if !matches!(state, "playing" | "buffering") {
                    return Err(RadioPlaybackError::Stopped("设备已停止电台播放".into()));
                }
                {
                    let mut s = self.data.lock().unwrap();
                    if !self.alive(g) {
                        break;
                    }
                    s.state = state.into();
                    s.message = if state == "playing" {
                        ""
                    } else {
                        "正在缓冲直播"
                    }
                    .into();
                }
                if last.elapsed() > Duration::from_secs(15) {
                    return Err(RadioPlaybackError::Source("直播缓冲超时，请换台或重试".into()));
                }
            }
            #[cfg(test)]
            eprintln!("radio stream: frames={seq}, elapsedMs={}, decodeWaits={starving}, bufferedMs={buffered}",stream_start.elapsed().as_millis());
            Ok(())
        })();
        let _ = child.kill();
        let _ = child.wait();
        drop(rx);
        let _wire = self.transport.lock().unwrap();
        if !self.alive(g) {
            // Current explicit start/stop owns cleanup. A retired decoder must
            // neither stop a freshly reconciled session nor poison its state.
            return result;
        }
        let state=if result.is_err(){"error"}else{"idle"};
        let message=result.as_ref().err().map(RadioPlaybackError::message).unwrap_or("").to_owned();
        self.device_state.finish(result,|| self.media_stop(board,
            json!({"sessionId":id,"state":state,"message":message})))
    }
}
fn create_service_with_owner(
    id: String,
    version: String,
    extension: crate::component_extension::Extension,
    dir: PathBuf,
    usb: UsbSerialManager,
    owner: Arc<owner::Guard>,
) -> Arc<Service> {
    let mut snapshot = Snapshot::default();
    let path = dir.join("library.json");
    if path.metadata().is_ok_and(|m| m.len() < MAX_LIBRARY_BYTES) {
        if let Ok(bytes) = std::fs::read(path) {
            if let Ok(s) = serde_json::from_slice::<Snapshot>(&bytes) {
                snapshot = s;
            }
        }
    }
    for list in [
        &mut snapshot.stations,
        &mut snapshot.history,
        &mut snapshot.catalog,
    ] {
        list.retain(|s| !s.id.is_empty() && s.id.len() <= 80 && network::valid_url(&s.url).is_ok());
        list.truncate(crate::media_catalog::QUEUE_MAX);
    }
    snapshot.history.truncate(10);
    snapshot.stations.truncate(20);
    // Cached automatic lists from an earlier package must not survive a change
    // to the authoritative bundled directory. User favorites/history stay intact.
    if extension.search.catalog.is_some() {
        snapshot.catalog.retain(|s|extension.allows_station(s));
        snapshot.catalog_at=0;
    }
    if extension.discovery.is_some() && !snapshot.automatic {snapshot.catalog.clear();snapshot.catalog_at=0;}
    snapshot.automatic=extension.discovery.is_some();
    snapshot.volume = snapshot.volume.min(100);
    snapshot.state = "idle".into();
    snapshot.message = if snapshot.automatic {"正在自动准备电台，无需选城"}else{"选择电台后开始收听"}.into();
    snapshot.sleep_minutes = 0;
    let binding = radio_connection_binding(&usb.status()).ok();
    let device_state = radio_device_state_for_binding(&owner, binding.as_ref());
    let s = Arc::new(Service {
        source: format!("pc.{id}"),
        id,
        version,
        extension,
        usb,
        dir,
        data: Mutex::new(snapshot),
        results: Mutex::new(HashMap::new()),
        generation: AtomicU64::new(0),
        operation: Mutex::new(()),
        transport: Mutex::new(()),
        session: Mutex::new(String::new()),
        device_state,
        binding: Mutex::new(binding),
        owner,
        sleep_until: Mutex::new(None),
        search_lock: Mutex::new(Instant::now() - Duration::from_secs(10)),
        discovering: AtomicBool::new(false),
        discovery_at: Mutex::new(Instant::now() - Duration::from_secs(60)),
        visibility: Mutex::new((0,0,false,Instant::now())),
        pending_auto: AtomicBool::new(false),
    });
    s
}
#[cfg(test)]
fn create_service(id:String,version:String,extension:crate::component_extension::Extension,dir:PathBuf,usb:UsbSerialManager)->Arc<Service> {
    let owner=Arc::new(owner::Guard::new(Box::new(OwnerJournal::open(dir.join(".radio-owner-test.json")))));
    create_service_with_owner(id,version,extension,dir,usb,owner)
}
pub fn configure(dir: PathBuf, usb: UsbSerialManager) {
    if HOST
        .set(Host {
            owner:Arc::new(owner::Guard::new(Box::new(OwnerJournal::open(dir.join("component-data/.radio-owner-v1.json"))))),
            root: dir.join("component-data"),
            usb,
            services: Mutex::new(HashMap::new()),
            operation: Mutex::new(()),
            requests: Mutex::new(()),
        })
        .is_err()
    {
        return;
    }
    std::thread::spawn(|| {
        let mut tick = 0u64;
        loop {
            std::thread::sleep(Duration::from_secs(1));
            let Some(host) = HOST.get() else { return };
            let services: Vec<_> = host.services.lock().unwrap().values().cloned().collect();
            for s in services {
                if tick % 5 == 0 && resolve(&s.id).map_or(true, |(version, _)| version != s.version)
                {
                    let _owner = host.operation.lock().unwrap();
                    s.stop("idle", "组件已删除或升级，请重新启用");
                    let mut services = host.services.lock().unwrap();
                    if services
                        .get(&s.id)
                        .is_some_and(|current| Arc::ptr_eq(current, &s))
                    {
                        services.remove(&s.id);
                    }
                    continue;
                }
                let _op = s.operation.lock().unwrap();
                if s.sleep_until
                    .lock()
                    .unwrap()
                    .is_some_and(|t| Instant::now() >= t)
                {
                    s.stop("idle", "定时已到，已停止");
                    *s.sleep_until.lock().unwrap() = None;
                    s.data.lock().unwrap().sleep_minutes = 0;
                }
                if !s.usb.status().connected
                    && matches!(
                        s.data.lock().unwrap().state.as_str(),
                        "playing" | "buffering"
                    )
                {
                    s.stop("idle", "设备已断开，请连接后重新播放");
                }
            }
            tick = tick.wrapping_add(1);
        }
    });
}
fn capture_became_active(active:&AtomicBool,topic:&str,p:&Value)->bool{
    match topic{
        "audio/begin"=>{active.store(true,Ordering::SeqCst);true},
        "audio/status"=>match p["active"].as_bool(){Some(value)=>{let prior=active.swap(value,Ordering::SeqCst);value&&!prior},None=>false},
        "audio/end"=>{active.store(false,Ordering::SeqCst);false},
        _=>false
    }
}
pub fn observe_capture(topic: &str, p: &Value) {
    static CAPTURE_ACTIVE:AtomicBool=AtomicBool::new(false);
    if !capture_became_active(&CAPTURE_ACTIVE,topic,p){return;}
    if let Some(host) = HOST.get() {
        for s in host.services.lock().unwrap().values() {
            if s.pending_auto.swap(false,Ordering::SeqCst){crate::realtime_chat_log::record("","radio_auto_play",json!({"stage":"cancel","reason":"capture_started"}));}
            if matches!(
                s.data.lock().unwrap().state.as_str(),
                "playing" | "buffering"
            ) {
                s.generation.fetch_add(1, Ordering::SeqCst);
                let mut state = s.data.lock().unwrap();
                state.state = "interrupted".into();
                state.message = "语音优先，电台已停止；可手动恢复".into();
            }
        }
    }
}
pub fn stop_all() {
    std::thread::spawn(|| {
        if let Some(host) = HOST.get() {
            let _owner = host.operation.lock().unwrap();
            for s in host.services.lock().unwrap().values() {
                s.stop("idle", "已停止播放");
            }
        }
    });
}
pub fn device_event(p: &Value) {
    let Some(id) = p["source"].as_str().and_then(|s| s.strip_prefix("pc.")) else {
        return;
    };
    let id = id.to_owned();
    if !crate::component_extension::valid_id(&id) {
        return;
    }
    // Physical controls use validated, default-enabled services, never arbitrary searches.
    if !matches!(
        p["operation"].as_str(),
        Some(
            "toggle"
                | "visibility"
                | "sync"
                | "select"
                | "next"
                | "previous"
                | "stop"
                | "volume_up"
                | "volume_down"
                | "volume"
        )
    ) {
        return;
    }
    // Preserve wire order: an old enter must not overtake a newer physical stop.
    let Some(event_binding) = HOST.get().and_then(|host| radio_connection_binding(&host.usb.status()).ok()) else { return; };
    static EVENTS: OnceLock<mpsc::SyncSender<(String,Value,RadioConnectionBinding)>>=OnceLock::new();
    let sender=EVENTS.get_or_init(|| {
        let (tx,rx)=mpsc::sync_channel::<(String,Value,RadioConnectionBinding)>(128);
        std::thread::spawn(move || {for (id,p,event_binding) in rx {
        if HOST.get().and_then(|host| radio_connection_binding(&host.usb.status()).ok()).as_ref() != Some(&event_binding) { continue; }
        let current_service = service(&id).ok().filter(|s| !s.binding_changed());
        let result=if p["operation"]=="visibility" && current_service.is_some(){execute_for_binding(&id,p,Some(&event_binding))}else{request_for_binding(id.clone(),p,Some(&event_binding))};
        if HOST.get().and_then(|host| radio_connection_binding(&host.usb.status()).ok()).as_ref() != Some(&event_binding) { continue; }
        match result {
            Ok(value) if value["queue"].as_array().is_some_and(|a| a.is_empty()) => device_notice(
                &id,
                "idle",
                value["message"].as_str().unwrap_or("打开列表加载电台"),
            ),
            Err(error) => {
                if let Ok(s) = service(&id) {
                    let mut data = s.data.lock().unwrap();
                    // A failed selection/volume command must not mark an existing
                    // live stream stopped or change audio ownership.
                    if !matches!(data.state.as_str(), "playing" | "buffering") {
                        data.state = "error".into();
                    }
                    data.message = error.clone();
                }
                device_notice(&id, "error", &error);
            }
            _ => {}
        }}});
        tx
    });
    if sender.try_send((id.clone(),p.clone(),event_binding)).is_err() && p["operation"]!="visibility" {
        // Never synchronously wait for USB ACK from its reader callback.
        crate::realtime_chat_log::record("","component_media_event_overflow",json!({"ok":false}));
    }
}
fn device_notice(id: &str, state: &str, message: &str) {
    let Some(host) = HOST.get() else { return };
    let board = host.usb.status();
    if !board.connected || board.capabilities["widgetMediaNotice"] != true {
        return;
    }
    // Firmware only applies this notice to the matching view, never to another
    // component's active playback; it does not flush or start any audio.
    let _ = host.usb.media_request(&board.board_device_id,"media/notice",json!({"source":format!("pc.{id}"),"state":state,"message":crate::widget_data::bounded_text(message,120)}));
}
pub fn execute(id: &str, input: Value) -> Result<Value, String> {
    execute_for_binding(id, input, None)
}
fn execute_for_binding(id: &str, input: Value, expected: Option<&RadioConnectionBinding>) -> Result<Value, String> {
    let s = service(id)?;
    if let Some(expected) = expected {
        let (scope, connection) = s.owner_scope()?;
        if expected.scope != scope || expected.connection != connection {
            return Err("电台连接已变化，未处理旧设备事件".into());
        }
    }
    let op = input["operation"].as_str().ok_or("电台操作无效")?;
    if matches!(op, "sync" | "toggle" | "next" | "previous" | "select" | "visibility") {
        s.ensure_catalog();
    }
    if op == "status" {
        return Ok(s.snapshot());
    }
    if op == "search" {
        let mut at = s
            .search_lock
            .try_lock()
            .map_err(|_| "搜索尚未结束，请稍候")?;
        if at.elapsed() < Duration::from_secs(2) {
            return Err("请稍候再搜索".into());
        }
        *at = Instant::now();
        let stations = network::search(&s.extension, input["query"].as_str().unwrap_or(""))?;
        let mut results = s.results.lock().unwrap();
        results.clear();
        for station in &stations {
            results.insert(station.id.clone(), station.clone());
        }
        return Ok(json!({"stations":stations}));
    }
    let host = HOST.get().ok_or("组件服务尚未初始化")?;
    let _host_op = host.operation.lock().unwrap();
    let _op = s.operation.lock().unwrap();
    if host
        .services
        .lock()
        .unwrap()
        .get(id)
        .is_none_or(|current| !Arc::ptr_eq(current, &s))
    {
        return Err("组件已停用或升级，请重试".into());
    }
    if op=="visibility" {
        let boot=input["boot"].as_u64().ok_or("无效媒体生命周期")?;
        let epoch=input["epoch"].as_u64().ok_or("无效媒体生命周期")?;
        let visible=input["visible"].as_bool().ok_or("无效媒体生命周期")?;
        let mut prior=s.visibility.lock().unwrap();
        if boot==prior.0 && epoch<prior.1 {return Ok(s.snapshot());}
        let entered=visible && (boot!=prior.0 || epoch!=prior.1 || !prior.2);
        *prior=(boot,epoch,visible,Instant::now());drop(prior);
        if !visible {s.pending_auto.store(false,Ordering::SeqCst);}
        else if entered && s.extension.discovery.as_ref().is_some_and(|d|d.auto_play_on_enter) {
            s.pending_auto.store(true,Ordering::SeqCst);
            crate::realtime_chat_log::record("","radio_auto_play",json!({"stage":"enter","count":s.data.lock().unwrap().queue().len()}));
        }
        s.maybe_auto();
        return Ok(s.snapshot());
    }
    if matches!(op,"stop"|"toggle"|"next"|"previous"|"select"|"play") {s.pending_auto.store(false,Ordering::SeqCst);}
    // Empty selection is a setup/loading state, not an audio playback failure.
    let empty = {
        let data = s.data.lock().unwrap();
        data.queue().is_empty() && data.current.is_none()
    };
    if matches!(op, "toggle" | "next" | "previous" | "select" | "sync") && empty {
        let message = if s.discovering.load(Ordering::SeqCst) {
            "正在加载电台，稍候打开列表选台"
        } else {
            "暂无电台，重新打开列表重试或在 PC 搜索"
        };
        let mut data = s.data.lock().unwrap();
        data.state = "idle".into();
        data.message = message.into();
        drop(data);
        s.library()?;
        device_notice(id, "idle", message);
        return Ok(s.snapshot());
    }
    match op {
        "sync" => {
            s.library()?;
        }
        "add" | "play" | "select" => {
            let id = input["id"]
                .as_str()
                .or_else(|| input["key"].as_str())
                .ok_or("请选择电台")?;
            let found = s
                .results
                .lock()
                .unwrap()
                .get(id)
                .cloned()
                .or_else(|| {
                    let d = s.data.lock().unwrap();
                    d.stations
                        .iter()
                        .chain(&d.catalog)
                        .chain(&d.history)
                        .find(|t| t.id == id)
                        .cloned()
                })
                .ok_or("电台已过期，请重新搜索")?;
            {
                let mut d = s.data.lock().unwrap();
                if op == "add" && !d.stations.iter().any(|t| t.id == id) {
                    if d.stations.len() >= 20 {
                        return Err("最多保存 20 个电台".into());
                    }
                    d.stations.push(found.clone());
                }
            }
            s.save()?;
            if op == "add" {
                s.library()?;
            } else {
                // Explicit play reconciles before any queue mutation; begin
                // carries this same queue once owned-stop is confirmed.
                s.start(found)?;
            }
        }
        "remove" => {
            let id = input["id"].as_str().ok_or("请选择电台")?;
            s.data.lock().unwrap().stations.retain(|t| t.id != id);
            s.save()?;
            s.library()?;
        }
        "toggle" => {
            let d = s.data.lock().unwrap().clone();
            if matches!(d.state.as_str(), "playing" | "buffering") {
                s.stop("idle", "已停止收听");
            } else {
                s.start(
                    d.current
                        .clone()
                        .or_else(|| d.queue().first().cloned())
                        .ok_or("请先在 PC 添加电台")?,
                )?;
            }
        }
        "stop" => s.stop("idle", "已停止收听"),
        "next" | "previous" => {
            let d = s.data.lock().unwrap().clone();
            let queue = d.queue();
            if queue.is_empty() {
                return Err("请先添加电台".into());
            }
            let at = d
                .current
                .as_ref()
                .and_then(|t| queue.iter().position(|e| e.id == t.id));
            let index = at
                .map(|i| {
                    (i as isize + if op == "next" { 1 } else { -1 })
                        .rem_euclid(queue.len() as isize) as usize
                })
                .unwrap_or(0);
            s.start(queue[index].clone())?;
        }
        "volume" | "volume_up" | "volume_down" => {
            let old = s.data.lock().unwrap().volume as i64;
            let value = match op {
                "volume_up" => (old + 5).min(100),
                "volume_down" => (old - 5).max(0),
                _ => input["volume"]
                    .as_i64()
                    .filter(|v| (0..=100).contains(v))
                    .ok_or("音量应为 0–100")?,
            };
            s.data.lock().unwrap().volume = value as u8;
            s.library()?;
            s.save()?;
        }
        "sleep" => {
            let n = input["minutes"]
                .as_u64()
                .filter(|n| [0, 15, 30, 60, 90].contains(n))
                .ok_or("定时时长无效")?;
            *s.sleep_until.lock().unwrap() = if n == 0 {
                None
            } else {
                Some(Instant::now() + Duration::from_secs(n * 60))
            };
            s.data.lock().unwrap().sleep_minutes = n as u32;
        }
        _ => return Err("不支持的电台操作".into()),
    }
    Ok(s.snapshot())
}
/// Resolve identity from the validated formal library, never from a caller-supplied path.
fn resolve(id: &str) -> Result<(String, crate::component_extension::Extension), String> {
    if !crate::component_extension::valid_id(id) {
        return Err("组件 ID 无效".into());
    }
    let home = crate::get_home_dir()?;
    let entry = crate::component_library::list(&home)?
        .components
        .into_iter()
        .find(|e| e.id == id && e.valid)
        .ok_or("组件不存在或校验失败")?;
    let ext = entry.pc_extension.ok_or("组件没有 PC 扩展")?;
    let actual = crate::component_library::package_content_hash(std::path::Path::new(&entry.path))?;
    if actual != entry.version_hash {
        return Err("组件内容已变化，请重新导入".into());
    }
    let mut extension: crate::component_extension::Extension =
        serde_json::from_value(ext).map_err(|_| "组件扩展无效")?;
    // A bundled catalog is read from the hash-verified formal copy only.
    extension.load_catalog(std::path::Path::new(&entry.path))?;
    Ok((actual, extension))
}
fn request(id: String, input: Value) -> Result<Value, String> {
    request_for_binding(id, input, None)
}
fn request_for_binding(id: String, input: Value, expected: Option<&RadioConnectionBinding>) -> Result<Value, String> {
    let (version, extension) = resolve(&id)?;
    let host = HOST.get().ok_or("组件服务尚未初始化")?;
    if let Some(expected) = expected {
        if radio_connection_binding(&host.usb.status())? != *expected {
            return Err("电台连接已变化，未处理旧设备事件".into());
        }
    }
    let dir = host.root.join(&id);
    let _request = host.requests.lock().unwrap();
    let op = input["operation"].as_str().ok_or("组件操作无效")?;
    if matches!(op, "enable" | "disable") {
        // Stop immediately even if persisting the user's choice subsequently fails.
        if op == "disable" {
            let _op = host.operation.lock().unwrap();
            if let Some(s) = host.services.lock().unwrap().remove(&id) {
                s.stop("idle", "PC 扩展已停用");
            }
        }
        crate::component_service_settings::set_enabled(&dir, op == "enable")?;
        if op == "disable" { return Ok(json!({"enabled":false})); }
    }
    let enabled = crate::component_service_settings::enabled(&dir)?;
    if op == "permissions" {
        return Ok(json!({"enabled":enabled,"permissions":extension.permissions}));
    }
    if op == "status" && !enabled {
        let board = host.usb.status();
        let mut value = json!(Snapshot::default());
        value["enabled"] = false.into();
        value["connected"] = board.connected.into();
        value["supported"] =
            (board.capabilities["widgetMediaSurface"] == "p4-media-surface-v1").into();
        value["message"] = "组件服务已手动停用，可在 PC 组件详情重新开启".into();
        return Ok(value);
    }
    if !enabled {
        return Err("组件服务已手动停用，可在 PC 组件详情重新开启".into());
    }
    let mut created = None;
    {
        let _op = host.operation.lock().unwrap();
        let mut services = host.services.lock().unwrap();
        let binding_changed = services.get(&id).is_some_and(|s| s.binding_changed());
        if binding_changed || services.get(&id).is_none_or(|s| s.version != version) {
            if let Some(s) = services.remove(&id) {
                if binding_changed { s.retire_for_connection_change(); }
                else { s.stop("idle", "组件已升级"); }
            }
            if services.len() >= 16 {
                return Err("已达到 PC 扩展运行数量上限，请先停用其他组件".into());
            }
            let s = create_service_with_owner(id.clone(), version, extension, dir, host.usb.clone(),host.owner.clone());
            // Pin physical event admission even if connection changes during resolve/create.
            if let Some(expected) = expected { *s.binding.lock().unwrap() = Some(expected.clone()); }
            services.insert(id.clone(), s.clone());
            created = Some(s);
        }
    }
    drop(_request); // A slow search must never block stop, disable or voice input.
    if let Some(s) = created {
        s.ensure_catalog();
    }
    execute_for_binding(
        &id,
        if op == "enable" {
            json!({"operation":"status"})
        } else {
            input
        },
        expected,
    )
}
#[tauri::command]
pub async fn component_extension_request(
    component_id: String,
    input: Value,
) -> Result<Value, String> {
    tauri::async_runtime::spawn_blocking(move || request(component_id, input))
        .await
        .map_err(|_| "组件任务异常".to_string())?
}

#[cfg(test)]
mod tests {
    #[test]fn capture_status_heartbeats_do_not_cancel_a_later_explicit_radio_entry(){
        use super::*;
        let active=AtomicBool::new(false);
        assert!(capture_became_active(&active,"audio/status",&json!({"active":true})));
        assert!(!capture_became_active(&active,"audio/status",&json!({"active":true})));
        assert!(!capture_became_active(&active,"audio/status",&json!({"active":false})));
        assert!(capture_became_active(&active,"audio/status",&json!({"active":true})));
        assert!(capture_became_active(&active,"audio/begin",&json!({})));
        assert!(!capture_became_active(&active,"audio/end",&json!({})));
        assert!(!active.load(Ordering::SeqCst));
    }
    use super::*;
    #[test]
    fn automatic_cache_migration_and_stop_cancel_pending() {
        let root=tempfile::tempdir().unwrap();
        let ext=crate::component_extension::parse(include_bytes!("../../builtin-clawpkgs/city-radio/pc/extension.json")).unwrap();
        let mut old=Snapshot::default();
        old.catalog=vec![Station{id:"old".into(),name:"old".into(),city:"".into(),homepage:"".into(),codec:"".into(),hls:false,url:"https://example.com/live".into()}];
        std::fs::write(root.path().join("library.json"),serde_json::to_vec(&old).unwrap()).unwrap();
        let s=create_service("test".into(),"v".into(),ext,root.path().into(),UsbSerialManager::new());
        assert!(s.data.lock().unwrap().catalog.is_empty());
        s.pending_auto.store(true,Ordering::SeqCst);s.stop("idle","test");assert!(!s.pending_auto.load(Ordering::SeqCst));
        assert_eq!(s.snapshot()["state"],"idle");
    }
    #[test]
    #[ignore="Explicit permission: synchronize radio, automatic play/switch/stop on a real device; no recording"]
    fn automatic_radio_hardware() {
        assert_eq!(std::env::var("RADIO_AUTOMATIC_TEST").as_deref(),Ok("1"));
        let board=std::env::var("P4_EXPECTED_BOARD_ID").unwrap();
        let usb=UsbSerialManager::new();
        usb.connect(&std::env::var("P4_SERIAL_PORT").unwrap(),|topic,p|{if topic=="media/event"{device_event(&p);}}).unwrap();
        assert_eq!(usb.status().board_device_id,board);
        assert_eq!(usb.status().capabilities["widgetMediaQueueMax"],128);
        let before=usb.query_diagnostics(&board).unwrap();
        let prior=usb.media_request(&board,"media/query",json!({})).unwrap();
        assert!(!matches!(prior["state"].as_str(),Some("playing"|"buffering"|"paused")));
        let inventory=usb.query_widget_inventory(&board).unwrap();
        let package=PathBuf::from(std::env::var("P4_WIDGET_DIR").unwrap());
        usb.install_widget_clawpkg_on_board(&board,"city-radio",&package,&HashMap::new(),|_,_,_|{}).unwrap();
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        let root=tempfile::tempdir().unwrap();configure(root.path().into(),usb.clone());
        struct Close(UsbSerialManager,String,u64);impl Drop for Close{fn drop(&mut self){if let Ok(s)=service("city-radio"){s.stop("idle","验收结束");}let _=self.0.media_request(&self.1,"media/control",json!({"operation":"volume","volume":self.2}));self.0.disconnect();}}
        let _close=Close(usb.clone(),board.clone(),prior["volume"].as_u64().unwrap());
        request("city-radio".into(),json!({"operation":"status"})).unwrap();
        let s=service("city-radio").unwrap();
        let until=Instant::now()+Duration::from_secs(120);
        while s.snapshot()["queue"].as_array().unwrap().is_empty(){assert!(Instant::now()<until,"{}",s.snapshot()["message"]);std::thread::sleep(Duration::from_millis(200));}
        assert_eq!(s.snapshot()["state"],"idle","PC status must not autoplay");
        let count=s.snapshot()["queue"].as_array().unwrap().len();assert!(count>=10);
        if s.snapshot()["location"]=="全国"{assert_eq!(count,30);}
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"app"})).unwrap();
        let wait_play=|| {let until=Instant::now()+Duration::from_secs(150);loop{let v=s.snapshot();if v["state"]=="playing"{return v;}assert_ne!(v["state"],"error","{}",v["message"]);assert!(Instant::now()<until,"automatic start timeout: {v}");std::thread::sleep(Duration::from_millis(250));}};
        let first=wait_play();eprintln!("AUTO PLAY: city={} count={} station={}",first["location"],count,first["current"]["name"]);
        let session=s.session.lock().unwrap().clone();
        for _ in 0..20{std::thread::sleep(Duration::from_millis(500));assert_eq!(s.snapshot()["state"],"playing");}
        execute("city-radio",json!({"operation":"sync"})).unwrap();assert_eq!(*s.session.lock().unwrap(),session,"catalog sync restarted playback");
        usb.send_to_board(&board,"miniapp/event",&json!({"action":"media.next"})).unwrap();
        let until=Instant::now()+Duration::from_secs(8);while s.snapshot()["current"]["id"]==first["current"]["id"]{assert!(Instant::now()<until);std::thread::sleep(Duration::from_millis(100));}
        let second=wait_play();eprintln!("NEXT PLAY: {}",second["current"]["name"]);
        std::thread::sleep(Duration::from_secs(8));
        execute("city-radio",json!({"operation":"stop"})).unwrap();
        std::thread::sleep(Duration::from_secs(5));assert_eq!(s.snapshot()["state"],"idle","visibility heartbeat restarted stopped audio");
        usb.send_to_board(&board,"control/screen-page",&json!({"page":"main"})).unwrap();
        let after=usb.query_diagnostics(&board).unwrap();assert_eq!(before["bootCount"],after["bootCount"]);assert_eq!(before["faultResetCount"],after["faultResetCount"]);
        let after_inventory=usb.query_widget_inventory(&board).unwrap();assert_eq!(inventory["items"].as_array().unwrap().len(),after_inventory["items"].as_array().unwrap().len());
        eprintln!("PASS: production lifecycle, automatic catalogue, auto playback, next, refresh continuity, stopped heartbeat, unchanged component count");
    }
    fn station(id: &str) -> Station {
        Station {
            id: id.into(),
            name: id.into(),
            city: String::new(),
            homepage: String::new(),
            codec: "mp3".into(),
            hls: false,
            url: "https://example.com/live".into(),
        }
    }
    #[test]
    #[ignore = "Read-only public station-directory request; no USB or audio"]
    fn live_device_catalog() {
        let ext = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        let stations = network::search(&ext, ext.search.browse_query.as_deref().unwrap()).unwrap();
        assert!(!stations.is_empty() && stations.len() <= 20);
        eprintln!(
            "device catalog: {} real stations, no playback",
            stations.len()
        );
    }
    #[test]
    fn device_queue_is_bounded_deduplicated_and_preserves_favorites() {
        let mut s = Snapshot::default();
        s.stations = vec![station("favorite")];
        s.catalog = vec![station("favorite")];
        s.catalog.extend((0..25).map(|i| station(&i.to_string())));
        let q = s.queue();
        assert_eq!(q.len(), 20);
        assert_eq!(q[0].id, "favorite");
        assert_eq!(q[1].id, "0");
        assert_eq!(s.stations.len(), 1);
    }
    #[test]
    fn catalog_survives_restart_without_autoplay_and_old_libraries_migrate() {
        let root = tempfile::tempdir().unwrap();
        let mut s = Snapshot::default();
        s.catalog = vec![station("a"), station("b")];
        s.state = "playing".into();
        std::fs::write(
            root.path().join("library.json"),
            serde_json::to_vec(&s).unwrap(),
        )
        .unwrap();
        let mut ext = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        // This tests the legacy/manual cache. Automatic discovery deliberately
        // invalidates a manual catalog on first upgrade (covered separately).
        ext.discovery = None;
        ext.search.catalog = None;
        serde_json::from_slice::<Snapshot>(&std::fs::read(root.path().join("library.json")).unwrap()).unwrap();
        let service = create_service(
            "city-radio".into(),
            "test".into(),
            ext,
            root.path().into(),
            UsbSerialManager::new(),
        );
        assert_eq!(service.snapshot()["state"], "idle");
        assert_eq!(service.snapshot()["queue"].as_array().unwrap().len(), 2);
        let mut old = json!(s);
        old.as_object_mut().unwrap().remove("catalog");
        assert!(serde_json::from_value::<Snapshot>(old)
            .unwrap()
            .catalog
            .is_empty());
    }
    #[test]
    fn full_catalog_and_favorites_fit_the_bounded_cache() {
        let root = tempfile::tempdir().unwrap();
        let mut data = Snapshot::default();
        // This is an automatic catalog cache, not the obsolete manual catalog
        // that the current radio package intentionally invalidates on upgrade.
        data.automatic = true;
        data.catalog_at = unix_now();
        data.catalog = (0..20).map(|i| { let mut s = station(&i.to_string()); s.homepage = "播".repeat(2000); s }).collect();
        data.stations = data.catalog.clone();
        data.history = data.catalog[..10].to_vec();
        let bytes = serde_json::to_vec(&data).unwrap();
        serde_json::from_slice::<Snapshot>(&bytes).unwrap();
        assert!(bytes.len() > 128 * 1024 && bytes.len() < MAX_LIBRARY_BYTES as usize);
        std::fs::write(root.path().join("library.json"), bytes).unwrap();
        let mut ext = crate::component_extension::parse(include_bytes!("../../builtin-clawpkgs/city-radio/pc/extension.json")).unwrap();
        ext.catalog_rows=data.catalog.iter().map(|s|json!({"id":s.id,"name":s.name,"url":s.url,"available":true})).collect();
        let service = create_service("city-radio".into(), "test".into(), ext, root.path().into(), UsbSerialManager::new());
        assert_eq!(service.data.lock().unwrap().stations.len(), 20);
        assert_eq!(service.data.lock().unwrap().catalog.len(), 20);
    }
    #[test]
    fn bundled_directory_upgrade_excludes_stale_cache_without_deleting_user_saves() {
        let root=tempfile::tempdir().unwrap();
        let mut data=Snapshot::default();data.automatic=true;
        data.catalog=vec![station("old")];data.stations=data.catalog.clone();data.history=data.catalog.clone();
        let original=serde_json::to_vec(&data).unwrap();
        std::fs::write(root.path().join("library.json"),&original).unwrap();
        let mut ext=crate::component_extension::parse(include_bytes!("../../builtin-clawpkgs/city-radio/pc/extension.json")).unwrap();
        ext.load_catalog(&std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../builtin-clawpkgs/city-radio")).unwrap();
        let service=create_service("city-radio".into(),"test".into(),ext,root.path().into(),UsbSerialManager::new());
        let snapshot=service.data.lock().unwrap();
        assert!(snapshot.catalog.is_empty());assert_eq!(snapshot.catalog_at,0);
        assert_eq!(snapshot.stations.len(),1);assert_eq!(snapshot.history.len(),1);
        assert!(!service.extension.allows_station(&snapshot.stations[0]));
        assert_eq!(std::fs::read(root.path().join("library.json")).unwrap(),original);
    }
    #[test]
    fn empty_device_controls_remain_idle_instead_of_playback_error() {
        let root = tempfile::tempdir().unwrap();
        let mut ext = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        ext.search.browse_query = None;
        let usb = UsbSerialManager::new();
        let s = create_service(
            "city-radio".into(),
            "test".into(),
            ext,
            root.path().into(),
            usb.clone(),
        );
        assert!(HOST
            .set(Host {
            owner:s.owner.clone(),
                root: root.path().into(),
                usb,
                services: Mutex::new(HashMap::from([("city-radio".into(), s.clone())])),
                operation: Mutex::new(()),
                requests: Mutex::new(())
            })
            .is_ok());
        for op in ["toggle", "next", "previous", "select", "sync"] {
            let state = execute("city-radio", json!({"operation":op})).unwrap();
            assert_eq!(state["state"], "idle");
            assert!(state["message"].as_str().unwrap().contains("暂无电台"));
        }
    }
    #[test]
    #[ignore = "Requires explicit permission, exclusive USB and RADIO_TEST_PORT; plays real radio for 25–120 seconds"]
    fn live_usb_radio() {
        let port = std::env::var("RADIO_TEST_PORT").expect("explicit test port");
        let test_seconds = std::env::var("RADIO_TEST_SECONDS")
            .map(|v| v.parse::<u64>().expect("integer test duration"))
            .unwrap_or(25);
        assert!((25..=120).contains(&test_seconds), "test duration must be 25–120 seconds");
        let play_duration = Duration::from_secs(test_seconds);
        let usb = UsbSerialManager::new();
        usb.set_desktop_device_id("radio-playback-hil");
        let input_seen = Arc::new(AtomicBool::new(false));
        let input_flag = input_seen.clone();
        usb.connect(&port, move |topic, _| {
            if matches!(topic.as_str(), "input/event" | "touch/event" | "media/event") {
                eprintln!("radio test interrupted by device input: {topic}");
                input_flag.store(true, Ordering::SeqCst);
                // A physical stop must never be treated as a failed stream and
                // automatically restarted by this unattended test harness.
                stop_all();
            }
        })
            .expect("exclusive USB connection");
        let board = usb.status().board_device_id;
        if let Ok(expected) = std::env::var("P4_EXPECTED_BOARD_ID") {
            assert_eq!(board, expected, "refusing playback on a different board");
        }
        let diagnostics_before = usb.query_diagnostics(&board).unwrap();
        if let Ok(expected) = std::env::var("P4_EXPECTED_VERSION") {
            assert_eq!(diagnostics_before["runtime"]["firmware"], expected, "unexpected firmware");
        }
        let inventory = usb.query_widget_inventory(&board).unwrap();
        assert_eq!(inventory["activeWidgetId"], "city-radio", "playback test must exercise the authorized component");
        let prior = usb
            .media_request(&usb.status().board_device_id, "media/query", json!({}))
            .unwrap();
        assert!(
            !matches!(
                prior["state"].as_str(),
                Some("playing" | "buffering" | "paused")
            ),
            "do not interrupt existing playback"
        );
        let prior_volume = prior["volume"].as_u64().expect("device volume");
        usb.send_to_board(&board, "control/screen-page", &json!({"page":"app"}))
            .expect("show the authorized radio component for observation");
        let root = tempfile::tempdir().unwrap();
        let extension = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        let station = network::search(&extension, "北京")
            .expect("live directory")
            .into_iter()
            .find(|s| s.hls && s.url.contains("rbc.cn"))
            .expect("public broadcaster");
        let s = create_service(
            "city-radio".into(),
            "explicit-live-test".into(),
            extension,
            root.path().join("city-radio"),
            usb.clone(),
        );
        assert!(HOST
            .set(Host {
            owner:s.owner.clone(),
                root: root.path().into(),
                usb: usb.clone(),
                services: Mutex::new(HashMap::from([("city-radio".into(), s.clone())])),
                operation: Mutex::new(()),
                requests: Mutex::new(())
            })
            .is_ok());
        struct Cleanup(Arc<Service>, u64);
        impl Drop for Cleanup {
            fn drop(&mut self) {
                self.0.stop("idle", "测试结束");
                let _ = self.0.usb.media_request(
                    &self.0.usb.status().board_device_id,
                    "media/control",
                    json!({"operation":"volume", "volume":self.1}),
                );
                self.0.usb.disconnect();
            }
        }
        let _cleanup = Cleanup(s.clone(), prior_volume);
        s.start(station.clone()).expect("start playback");
        let test_started = Instant::now();
        let deadline = test_started + play_duration + Duration::from_secs(25);
        let mut first = None;
        let mut samples = 0;
        let mut last_state = String::new();
        let mut rebuffer_samples = 0;
        while Instant::now() < deadline {
            std::thread::sleep(Duration::from_millis(500));
            assert!(!input_seen.load(Ordering::SeqCst), "manual input interrupted the unattended playback test; not a continuous-playback result");
            let snapshot = s.snapshot();
            if snapshot["state"].as_str().unwrap_or("") != last_state {
                last_state = snapshot["state"].as_str().unwrap_or("").into();
                eprintln!(
                    "radio state at {} ms: {last_state}",
                    test_started.elapsed().as_millis()
                );
            }
            assert_ne!(snapshot["state"], "error", "{}", snapshot["message"]);
            if first.is_some() && snapshot["state"] != "playing" {
                rebuffer_samples += 1;
            }
            if snapshot["state"] == "playing" {
                samples += 1;
                let started = first.get_or_insert_with(Instant::now);
                if started.elapsed() >= play_duration {
                    break;
                }
            }
        }
        assert!(
            first.is_some_and(|t| t.elapsed() >= play_duration) && samples >= test_seconds * 8 / 5,
            "insufficient sustained playback samples: {samples}"
        );
        s.stop("idle", "测试结束");
        let ack = usb
            .media_request(&usb.status().board_device_id, "media/query", json!({}))
            .unwrap();
        assert_eq!(ack["state"], "idle");
        let diagnostics_after = usb.query_diagnostics(&board).unwrap();
        assert_eq!(diagnostics_after["bootCount"], diagnostics_before["bootCount"]);
        assert_eq!(diagnostics_after["faultResetCount"], diagnostics_before["faultResetCount"]);
        assert_eq!(diagnostics_after["runtime"]["screenPage"], "app");
        assert_eq!(rebuffer_samples, 0, "playback stalled after startup");
        eprintln!(
            "LIVE USB PASS: station={}, playingSeconds={}, playingSamples={}, rebufferSamples=0, stopped=true, display=app, noReboot=true",
            station.name, test_seconds, samples
        );
    }
    #[test]
    fn component_storage_survives_update_without_cross_component_data() {
        let root = tempfile::tempdir().unwrap();
        let extension = crate::component_extension::parse(include_bytes!(
            "../../builtin-clawpkgs/city-radio/pc/extension.json"
        ))
        .unwrap();
        let a = create_service(
            "one".into(),
            "v1".into(),
            extension.clone(),
            root.path().join("one"),
            UsbSerialManager::new(),
        );
        a.data.lock().unwrap().volume = 15;
        a.save().unwrap();
        let b = create_service(
            "two".into(),
            "v1".into(),
            extension.clone(),
            root.path().join("two"),
            UsbSerialManager::new(),
        );
        assert_eq!(b.data.lock().unwrap().volume, 35);
        let upgrade = create_service(
            "one".into(),
            "v2".into(),
            extension,
            root.path().join("one"),
            UsbSerialManager::new(),
        );
        assert_eq!(upgrade.data.lock().unwrap().volume, 15);
        assert_eq!(upgrade.data.lock().unwrap().state, "idle");
        let g = a.generation.load(Ordering::SeqCst);
        a.stop("idle", "stopped");
        assert!(!a.alive(g));
    }
}

#[cfg(test)]
#[path = "component_media_lifecycle_tests.rs"]
mod lifecycle_tests;

#[cfg(test)]
#[path = "component_media/owner_journal_tests.rs"]
mod owner_journal_tests;
