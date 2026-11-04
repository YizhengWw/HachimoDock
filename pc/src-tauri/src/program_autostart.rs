//! PC-online, explicit opt-in policy. No OS login item, device boot flag, RAM
//! resume, variable owner, resource grant, timer, or retry loop is created here.
//! See pc/docs/program-autostart.md for the mandatory transport adapter contract.
use crate::app_program_background as background;
use serde::{Deserialize, Serialize};
use serde_json::Value;
use sha2::{Digest, Sha256};
use std::collections::BTreeSet;
use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};
pub(crate) mod native;

pub const MAX_ATTEMPTS: usize = 256;
pub const UNAVAILABLE_ERROR: &str = "program_autostart_unavailable";
const MAX_JOURNAL_BYTES: u64 = 128 * 1024;
const STORAGE_ERROR: &str = "连接后启动记录不可用，未自动启动程序";
fn invalid() -> String { "连接后启动授权或设备内容未通过核对".into() }
fn board_valid(s: &str) -> bool { !s.is_empty() && s.len() <= 47 && s.bytes().all(|b| b.is_ascii_alphanumeric() || b"._-".contains(&b)) }
fn hex(s: &str, len: usize) -> bool { s.len() == len && s.bytes().all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)) }

/// Reuse strict v1/v2 negotiation; opt in only to the explicit public 1 lease,
/// 4096 fuel / 64 quantum contract. Missing/expanded budgets never default in.
pub fn supported(caps: &Value) -> bool {
    background::supported(caps) && caps["widgetProgramBackgroundMaxActive"].as_u64() == Some(1)
        && caps["widgetProgramAggregateFuel"].as_u64() == Some(4096)
        && caps["widgetProgramMaxFuel"].as_u64() == Some(4096)
        && caps["widgetProgramQuantum"].as_u64() == Some(64)
        && caps["widgetProgramAggregateQuantum"].as_u64() == Some(64)
}

#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Checksums { pub widget_checksum: String, pub buttons_checksum: String, pub sprites_checksum: String }
impl Checksums {
    fn valid(&self) -> bool { [&self.widget_checksum, &self.buttons_checksum, &self.sprites_checksum].iter().all(|s| hex(s, 8)) }
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Target { pub board_id: String, pub component_id: String, pub content_hash: String, pub checksums: Checksums }
impl Target {
    fn valid(&self) -> bool { board_valid(&self.board_id) && background::valid_widget_id(&self.component_id) && hex(&self.content_hash, 64) && self.checksums.valid() }
}

/// Only construct from fresh native inventory/inspect and freshly hashed local
/// bytes. The UI's versionHash, selected path or JSON is never this evidence.
#[derive(Clone, Debug)]
pub struct VerifiedProgram { target: Target, snapshot: background::Snapshot, binding: Option<(u64,u64)> }
impl VerifiedProgram {
    pub fn target(&self) -> &Target { &self.target }
    pub fn boot_id(&self) -> &str { &self.snapshot.boot_id }
    pub fn has_registration(&self) -> bool { !self.snapshot.items.is_empty() }
    pub(crate) fn bind(mut self, connection: u64, admission: u64) -> Self { self.binding=Some((connection,admission));self }
    pub(crate) fn binding(&self) -> Option<(u64,u64)> { self.binding }
    pub fn start_payload(&self) -> Value {
        serde_json::json!({"widgetId":self.target.component_id,"bootId":self.snapshot.boot_id,
          "widgetChecksum":self.target.checksums.widget_checksum,"buttonsChecksum":self.target.checksums.buttons_checksum,
          "spritesChecksum":self.target.checksums.sprites_checksum})
    }
    pub fn from_device(board: &str, component: &str, content_hash: &str, caps: &Value, inventory: &Value, inspected: &Value) -> Result<Self, String> {
        // This policy is only for the public one-PROGRAM lane. It cannot expand
        // firmware's shared lease/fuel/750us or any audio/resource admission.
        if !supported(caps)
          || inventory["boardDeviceId"] != board || inventory["ok"] != true
          || inventory["complete"] != true || inventory["recoveryBlocked"] != false { return Err(invalid()); }
        let rows = inventory["items"].as_array().filter(|rows| rows.len() <= 32).ok_or_else(invalid)?;
        let mut ids = BTreeSet::new();
        for row in rows {
            let id = row["id"].as_str().ok_or_else(invalid)?;
            if !background::valid_widget_id(id) || !ids.insert(id) { return Err(invalid()); }
        }
        if !ids.contains(component) { return Err(invalid()); }
        let installed = background::installed(inspected, board, component)?;
        let payload = background::start_payload(&installed);
        let target = Target { board_id: board.into(), component_id: component.into(), content_hash: content_hash.into(),
            checksums: Checksums { widget_checksum: payload["widgetChecksum"].as_str().ok_or_else(invalid)?.into(),
                buttons_checksum: payload["buttonsChecksum"].as_str().ok_or_else(invalid)?.into(),
                sprites_checksum: payload["spritesChecksum"].as_str().ok_or_else(invalid)?.into() } };
        if !target.valid() { return Err(invalid()); }
        Ok(Self { target, snapshot: background::snapshot(inspected, board, Some(&installed.boot_id))?, binding: None })
    }
}

/// Must distinguish authoritative deletion/update from an unavailable query.
#[derive(Clone, Copy, Debug)]
pub enum CheckFailure { Unavailable, ComponentDeleted, ContentChanged, DeviceChanged }
/// The adapter MUST reverify local bytes + exact installed checksums + boot
/// under existing USB/transfer/resource guards immediately before one send.
/// It must use expected.start_payload(), NOT start_background_program(board,id),
/// which reselects current content. A failed/unconfirmed send is never retried.
pub trait Host: Send + Sync {
    fn inspect(&self, board: &str, component: &str) -> Result<VerifiedProgram, CheckFailure>;
    fn start_once(&self, expected: &VerifiedProgram) -> Result<background::Outcome, CheckFailure>;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum Code {
    Disabled, Enabled, Running, ResultUnknownNoReplay, AlreadyAttempted,
    OwnerPresentNoResume, PreemptedNoResume, RevokedContentChanged,
    RevokedComponentDeleted, RevokedDeviceChanged, VerificationUnavailable,
    BootChanged, JournalFull, StorageUnavailable,
}
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Attempt { board_id: String, boot_id: String, component_id: Option<String>, content_hash: Option<String>, code: Code }
#[derive(Clone, Debug, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Journal { schema_version: u32, policy: Option<Target>, attempts: Vec<Attempt>, code: Code }
impl Default for Journal {
    fn default() -> Self { Self { schema_version: 1, policy: None, attempts: vec![], code: Code::Disabled } }
}
impl Journal {
    fn validate(&self) -> Result<(), String> {
        let mut keys = BTreeSet::new();
        if self.schema_version != 1 || self.policy.as_ref().is_some_and(|p| !p.valid()) || self.attempts.len() > MAX_ATTEMPTS { return Err(STORAGE_ERROR.into()); }
        for a in &self.attempts {
            let identity_valid = match (&a.component_id, &a.content_hash) {
                (Some(id), Some(hash)) => background::valid_widget_id(id) && hex(hash, 64),
                (None, None) => a.code == Code::PreemptedNoResume,
                _ => false,
            };
            if !board_valid(&a.board_id) || !hex(&a.boot_id, 16) || !identity_valid
              || !keys.insert((&a.board_id, &a.boot_id)) { return Err(STORAGE_ERROR.into()); }
        }
        Ok(())
    }
}
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Status { pub schema_version: u32, pub enabled: bool, pub policy: Option<Target>, pub attempts: usize, pub code: Code }
struct Inner { journal: Journal, blocked: bool }
pub struct ProgramAutostart { path: PathBuf, _lock: File, inner: Mutex<Inner>, host: Arc<dyn Host> }

/// Always manage this wrapper, even when the journal/lock/host is unavailable.
/// Optional policy storage must never make Tauri setup fail or close the UI.
#[derive(Clone, Default)]
pub struct ProgramAutostartState { pub service: Option<Arc<ProgramAutostart>> }
impl ProgramAutostartState {
    pub fn unavailable() -> Self { Self::default() }
    pub fn open(directory: &Path, host: Arc<dyn Host>) -> Self {
        Self { service: ProgramAutostart::open(directory, host).ok().map(Arc::new) }
    }
    pub fn service(&self) -> Result<Arc<ProgramAutostart>, String> {
        self.service.clone().ok_or_else(|| UNAVAILABLE_ERROR.into())
    }
}

// Lifetime OS lock; survives process crash without a stale create_new lockfile.
// A second app/process cannot race the read/record/send boundary.
fn lock_file(path: &Path) -> Result<File, String> {
    let mut options = OpenOptions::new();
    options.read(true).write(true).create(true).truncate(false);
    #[cfg(windows)] { use std::os::windows::fs::OpenOptionsExt; options.share_mode(0); }
    let file = options.open(path).map_err(|_| STORAGE_ERROR)?;
    #[cfg(unix)] {
        use std::os::fd::AsRawFd;
        // SAFETY: the owned fd stays open for this service's entire lifetime.
        if unsafe { libc::flock(file.as_raw_fd(), libc::LOCK_EX | libc::LOCK_NB) } != 0 { return Err(STORAGE_ERROR.into()); }
    }
    #[cfg(not(any(unix, windows)))] { return Err(STORAGE_ERROR.into()); }
    Ok(file)
}
fn save(path: &Path, journal: &Journal) -> Result<(), String> {
    journal.validate()?;
    let bytes = serde_json::to_vec(journal).map_err(|_| STORAGE_ERROR)?;
    if bytes.len() as u64 > MAX_JOURNAL_BYTES { return Err(STORAGE_ERROR.into()); }
    let parent = path.parent().ok_or(STORAGE_ERROR)?;
    let mut staged = tempfile::NamedTempFile::new_in(parent).map_err(|_| STORAGE_ERROR)?;
    staged.write_all(&bytes).map_err(|_| STORAGE_ERROR)?;
    staged.as_file().sync_all().map_err(|_| STORAGE_ERROR)?;
    let file = staged.persist(path).map_err(|_| STORAGE_ERROR)?;
    file.sync_all().map_err(|_| STORAGE_ERROR)?;
    #[cfg(unix)] File::open(parent).and_then(|f| f.sync_all()).map_err(|_| STORAGE_ERROR)?;
    Ok(())
}
impl ProgramAutostart {
    /// Use one app-managed service, a trusted app-data directory, and blocking
    /// worker commands. Missing/corrupt journal after initialization fails shut.
    pub fn open(directory: &Path, host: Arc<dyn Host>) -> Result<Self, String> {
        fs::create_dir_all(directory).map_err(|_| STORAGE_ERROR)?;
        let mut lock = lock_file(&directory.join("program-autostart.lock"))?;
        let path = directory.join("program-autostart-v1.json");
        let initialized = lock.metadata().map_err(|_| STORAGE_ERROR)?.len() != 0;
        let journal = match File::open(&path) {
            Ok(file) => {
                let mut bytes = Vec::new();
                file.take(MAX_JOURNAL_BYTES + 1).read_to_end(&mut bytes).map_err(|_| STORAGE_ERROR)?;
                if bytes.len() as u64 > MAX_JOURNAL_BYTES { return Err(STORAGE_ERROR.into()); }
                let journal: Journal = serde_json::from_slice(&bytes).map_err(|_| STORAGE_ERROR)?;
                journal.validate()?;
                journal
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound && !initialized => {
                let journal = Journal::default(); save(&path, &journal)?; journal
            }
            Err(_) => return Err(STORAGE_ERROR.into()),
        };
        lock.write_all(b"1").and_then(|_| lock.sync_all()).map_err(|_| STORAGE_ERROR)?;
        Ok(Self { path, _lock: lock, inner: Mutex::new(Inner { journal, blocked: false }), host })
    }
    fn status_of(inner: &Inner) -> Status {
        Status { schema_version: 1, enabled: !inner.blocked && inner.journal.policy.is_some(),
            policy: if inner.blocked { None } else { inner.journal.policy.clone() }, attempts: inner.journal.attempts.len(),
            code: if inner.blocked { Code::StorageUnavailable } else { inner.journal.code } }
    }
    fn commit(&self, inner: &mut Inner) -> Result<(), String> {
        if inner.blocked { return Err(STORAGE_ERROR.into()); }
        if save(&self.path, &inner.journal).is_err() { inner.blocked = true; return Err(STORAGE_ERROR.into()); }
        Ok(())
    }
    pub fn status(&self) -> Result<Status, String> {
        let inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        Ok(Self::status_of(&inner))
    }
    /// Read-only consent preview. enable() independently reinspects, so this
    /// target cannot authorize an intervening component/device replacement.
    pub fn inspect_target(&self, board: &str, component: &str) -> Result<Target, String> {
        if !board_valid(board) || !background::valid_widget_id(component) { return Err(invalid()); }
        let inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.blocked { return Err(STORAGE_ERROR.into()); }
        let verified = self.host.inspect(board, component).map_err(|_| invalid())?;
        if verified.target.board_id != board || verified.target.component_id != component { return Err(invalid()); }
        Ok(verified.target)
    }
    /// Saves consent only; NEVER starts on enable, including re-enable.
    pub fn enable(&self, target: Target, user_confirmed: bool) -> Result<Status, String> {
        if !user_confirmed || !target.valid() { return Err(invalid()); }
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.blocked { return Err(STORAGE_ERROR.into()); }
        if inner.journal.attempts.len() >= MAX_ATTEMPTS { return Err("连接后启动记录已满，未启用".into()); }
        let verified = self.host.inspect(&target.board_id, &target.component_id).map_err(|_| invalid())?;
        if verified.target != target { return Err(invalid()); }
        inner.journal.policy = Some(target); inner.journal.code = Code::Enabled;
        self.commit(&mut inner)?; Ok(Self::status_of(&inner))
    }
    /// Stops future automatic starts only. Stopping a live program remains the
    /// separate observed boot+registration stop operation; it is not implied.
    pub fn disable(&self) -> Result<Status, String> {
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        inner.journal.policy = None; inner.journal.code = Code::Disabled;
        self.commit(&mut inner)?; Ok(Self::status_of(&inner))
    }
    fn revoke(inner: &mut Inner, code: Code) { inner.journal.policy = None; inner.journal.code = code; }
    fn check_failed(inner: &mut Inner, failure: CheckFailure) -> Code {
        let code = match failure {
            CheckFailure::Unavailable => return Code::VerificationUnavailable,
            CheckFailure::ComponentDeleted => Code::RevokedComponentDeleted,
            CheckFailure::ContentChanged => Code::RevokedContentChanged,
            CheckFailure::DeviceChanged => Code::RevokedDeviceChanged,
        };
        Self::revoke(inner, code); code
    }
    /// Call before update/delete admission (even offline); never restore consent
    /// if that operation later fails. Board switch revokes even if it switches back.
    pub fn component_changed(&self, component: &str, deleted: bool) -> Result<Status, String> {
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.journal.policy.as_ref().is_some_and(|p| p.component_id == component) {
            Self::revoke(&mut inner, if deleted { Code::RevokedComponentDeleted } else { Code::RevokedContentChanged });
            self.commit(&mut inner)?;
        }
        Ok(Self::status_of(&inner))
    }
    pub fn device_changed(&self, board: &str) -> Result<Status, String> {
        if !board_valid(board) { return Err(invalid()); }
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.journal.policy.as_ref().is_some_and(|p| p.board_id != board) {
            Self::revoke(&mut inner, Code::RevokedDeviceChanged); self.commit(&mut inner)?;
        }
        Ok(Self::status_of(&inner))
    }
    /// Audio preemption / explicit stop before any connection attempt consumes
    /// this boot too. Never use an empty/disconnected board or a guessed boot.
    pub fn suppress_boot(&self, board: &str, boot: &str) -> Result<Status, String> {
        if !board_valid(board) || !hex(boot, 16) { return Err(invalid()); }
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.blocked { return Err(STORAGE_ERROR.into()); }
        if let Some(a) = inner.journal.attempts.iter_mut().find(|a| a.board_id == board && a.boot_id == boot) {
            // Preserve unknown: a later audio event cannot claim a lost start
            // was confirmed. Both codes independently prohibit replay.
            if a.code != Code::ResultUnknownNoReplay { a.code = Code::PreemptedNoResume; }
        } else if inner.journal.attempts.len() < MAX_ATTEMPTS {
            let target = inner.journal.policy.as_ref().filter(|p| p.board_id == board);
            let component_id = target.map(|p| p.component_id.clone());
            let content_hash = target.map(|p| p.content_hash.clone());
            inner.journal.attempts.push(Attempt { board_id: board.into(), boot_id: boot.into(),
                component_id, content_hash, code: Code::PreemptedNoResume });
        } else { Self::revoke(&mut inner, Code::JournalFull); self.commit(&mut inner)?; return Ok(Self::status_of(&inner)); }
        inner.journal.code = inner.journal.attempts.iter().find(|a| a.board_id == board && a.boot_id == boot).unwrap().code;
        self.commit(&mut inner)?;
        Ok(Self::status_of(&inner))
    }
    /// Call once from the connection owner after native board+boot validation.
    /// Repeated events/UI remounts/process restarts cannot erase the ledger.
    pub fn observe(&self, board: &str, boot: &str) -> Result<Status, String> {
        if !board_valid(board) || !hex(boot, 16) { return Err(invalid()); }
        let mut inner = self.inner.lock().map_err(|_| STORAGE_ERROR)?;
        if inner.blocked { return Err(STORAGE_ERROR.into()); }
        let Some(target) = inner.journal.policy.clone() else { return Ok(Self::status_of(&inner)); };
        if target.board_id != board {
            Self::revoke(&mut inner, Code::RevokedDeviceChanged); self.commit(&mut inner)?; return Ok(Self::status_of(&inner));
        }
        if let Some(a) = inner.journal.attempts.iter().find(|a| a.board_id == board && a.boot_id == boot) {
            inner.journal.code = a.code; return Ok(Self::status_of(&inner));
        }
        if inner.journal.attempts.len() >= MAX_ATTEMPTS {
            Self::revoke(&mut inner, Code::JournalFull); self.commit(&mut inner)?; return Ok(Self::status_of(&inner));
        }
        // Record BEFORE any host work, including the read-only preflight. A busy
        // adapter/read error consumes the boot; it cannot become a deferred resume.
        inner.journal.attempts.push(Attempt { board_id: board.into(), boot_id: boot.into(), component_id: Some(target.component_id.clone()),
            content_hash: Some(target.content_hash.clone()), code: Code::ResultUnknownNoReplay });
        inner.journal.code = Code::ResultUnknownNoReplay; self.commit(&mut inner)?;
        let code = match self.host.inspect(board, &target.component_id) {
            Ok(verified) if verified.target != target => { Self::revoke(&mut inner, Code::RevokedContentChanged); Code::RevokedContentChanged }
            Ok(verified) if verified.boot_id() != boot => Code::BootChanged,
            // Any registration, including stopped or waiting for exit, prevents
            // resume. Do not steal the one shared foreground/background lease.
            Ok(verified) if !verified.snapshot.items.is_empty() => Code::OwnerPresentNoResume,
            Ok(verified) => {
                match self.host.start_once(&verified) {
                  Ok(out) if out.confirmed && out.snapshot.as_ref().is_some_and(|s| s.boot_id == boot
                    && s.items.len() == 1 && s.items.iter().any(|e| e.widget_id == target.component_id && e.live && !e.stopped))
                    => Code::Running,
                  Ok(_) => Code::ResultUnknownNoReplay,
                  Err(failure) => Self::check_failed(&mut inner, failure),
                }
            }
            Err(failure) => Self::check_failed(&mut inner, failure),
        };
        inner.journal.attempts.last_mut().unwrap().code = code; inner.journal.code = code;
        self.commit(&mut inner)?; Ok(Self::status_of(&inner))
    }
}

/// Hash every package file's real bytes (not versionHash/path/mtime). Paths are
/// sorted UTF-8 relative names, '/' separated; stream is name + NUL + bytes + NUL.
/// Full SHA-256, no truncation. Reject symlinks/special files and bound traversal.
pub fn component_content_hash(root: &Path) -> Result<String, String> {
    fn visit(root: &Path, at: &Path, depth: usize, count: &mut usize, files: &mut Vec<(String, PathBuf)>) -> Result<(), String> {
        if depth > 8 { return Err(invalid()); }
        for entry in fs::read_dir(at).map_err(|_| invalid())? {
            *count += 1; if *count > 256 { return Err(invalid()); }
            let entry = entry.map_err(|_| invalid())?; let path = entry.path();
            let kind = fs::symlink_metadata(&path).map_err(|_| invalid())?.file_type();
            if kind.is_dir() { visit(root, &path, depth + 1, count, files)?; }
            else if kind.is_file() {
                let mut parts = Vec::new();
                for part in path.strip_prefix(root).map_err(|_| invalid())?.components() {
                    let std::path::Component::Normal(part) = part else { return Err(invalid()); };
                    let part = part.to_str().ok_or_else(invalid)?;
                    if part.contains('\0') || part.contains('\\') { return Err(invalid()); }
                    parts.push(part);
                }
                let relative = parts.join("/");
                files.push((relative, path));
            } else { return Err(invalid()); }
        }
        Ok(())
    }
    if !fs::symlink_metadata(root).map_err(|_| invalid())?.file_type().is_dir() { return Err(invalid()); }
    let mut files = Vec::new(); visit(root, root, 0, &mut 0, &mut files)?;
    files.sort_by(|a, b| a.0.cmp(&b.0));
    if files.is_empty() || files.windows(2).any(|w| w[0].0 == w[1].0) { return Err(invalid()); }
    let mut digest = Sha256::new(); let mut remaining = 16 * 1024 * 1024u64;
    for (name, path) in files {
        let mut options = OpenOptions::new(); options.read(true);
        #[cfg(unix)] { use std::os::unix::fs::OpenOptionsExt; options.custom_flags(libc::O_NOFOLLOW); }
        #[cfg(windows)] { use std::os::windows::fs::OpenOptionsExt; options.custom_flags(0x0020_0000); } // OPEN_REPARSE_POINT
        let file = options.open(path).map_err(|_| invalid())?;
        let metadata = file.metadata().map_err(|_| invalid())?;
        if !metadata.is_file() { return Err(invalid()); }
        #[cfg(windows)] {
            use std::os::windows::fs::MetadataExt;
            if metadata.file_attributes() & 0x400 != 0 { return Err(invalid()); } // REPARSE_POINT
        }
        let mut bytes = Vec::new(); file.take(remaining + 1).read_to_end(&mut bytes).map_err(|_| invalid())?;
        remaining = remaining.checked_sub(bytes.len() as u64).ok_or_else(invalid)?;
        digest.update(name.as_bytes()); digest.update([0]); digest.update(&bytes); digest.update([0]);
    }
    Ok(format!("{:x}", digest.finalize()))
}

// Thin command wrappers: registration and manage() stay with the integration
// owner. No frontend-supplied filesystem path, checksum rewrite or direct USB.
#[tauri::command]
pub async fn program_autostart_get(state: tauri::State<'_, ProgramAutostartState>) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.status()).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_inspect(state: tauri::State<'_, ProgramAutostartState>, board_id: String, component_id: String) -> Result<Target, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.inspect_target(&board_id, &component_id)).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_enable(state: tauri::State<'_, ProgramAutostartState>, target: Target, user_confirmed: bool) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.enable(target, user_confirmed)).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_disable(state: tauri::State<'_, ProgramAutostartState>) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.disable()).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_observe(state: tauri::State<'_, ProgramAutostartState>, board_id: String, boot_id: String) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.observe(&board_id, &boot_id)).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_suppress_boot(state: tauri::State<'_, ProgramAutostartState>, board_id: String, boot_id: String) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.suppress_boot(&board_id, &boot_id)).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_component_changed(state: tauri::State<'_, ProgramAutostartState>, component_id: String, deleted: bool) -> Result<Status, String> {
    if !background::valid_widget_id(&component_id) { return Err(invalid()); }
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.component_changed(&component_id, deleted)).await.map_err(|_| STORAGE_ERROR)?
}
#[tauri::command]
pub async fn program_autostart_device_changed(state: tauri::State<'_, ProgramAutostartState>, board_id: String) -> Result<Status, String> {
    let service = state.service()?; tauri::async_runtime::spawn_blocking(move || service.device_changed(&board_id)).await.map_err(|_| STORAGE_ERROR)?
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};
    const BOOT: &str = "0123456789abcdef";
    const NEXT: &str = "1123456789abcdef";
    struct FakeHost {
        proof: Mutex<VerifiedProgram>, failure: Mutex<Option<CheckFailure>>,
        final_failure: Mutex<Option<CheckFailure>>, sends: AtomicUsize,
        unknown: bool, journal: PathBuf,
    }
    impl Host for FakeHost {
        fn inspect(&self, _: &str, _: &str) -> Result<VerifiedProgram, CheckFailure> {
            match *self.failure.lock().unwrap() { Some(f) => Err(f), None => Ok(self.proof.lock().unwrap().clone()) }
        }
        fn start_once(&self, expected: &VerifiedProgram) -> Result<background::Outcome, CheckFailure> {
            if let Some(f) = *self.final_failure.lock().unwrap() { return Err(f); }
            // Read the real journal inside the fake physical-send boundary:
            // the attempt must already be durable, even for a successful start.
            let disk: Journal = serde_json::from_slice(&fs::read(&self.journal).unwrap()).unwrap();
            let a = disk.attempts.iter().find(|a| a.board_id == expected.target.board_id && a.boot_id == expected.boot_id()).unwrap();
            assert_eq!(a.code, Code::ResultUnknownNoReplay);
            self.sends.fetch_add(1, Ordering::SeqCst);
            if self.unknown { return Ok(background::Outcome::unknown()); }
            let p = serde_json::json!({"ok":true,"engine":background::ENGINE,"boardDeviceId":expected.target.board_id,"bootId":expected.boot_id(),
                "items":[{"widgetId":expected.target.component_id,"registration":"1","live":true,"stopped":false,
                    "checkpointPending":false,"revision":1,"phase":1,"lastStepUs":10,"maxStepUs":20}]});
            let installed = background::installed(&inspection(expected.boot_id()), "p4-test", "focus").unwrap();
            Ok(background::Outcome::started(&p, "p4-test", &installed))
        }
    }
    fn inspection(boot: &str) -> Value {
        serde_json::json!({"ok":true,"engine":background::ENGINE,"boardDeviceId":"p4-test","bootId":boot,"items":[],
            "installedContent":{"widgetId":"focus","widgetChecksum":"ffffffff","buttonsChecksum":"00000001","spritesChecksum":"00000000"}})
    }
    fn public_caps() -> Value {
        serde_json::json!({"widgetProgram":"p4-component-program-v2","widgetPrograms":["p4-component-program-v1","p4-component-program-v2"],
            "widgetProgramBackground":background::ENGINE,"widgetProgramInspect":background::INSPECT,"widgetProgramBackgroundMaxActive":1,
            "widgetProgramAggregateFuel":4096,"widgetProgramMaxFuel":4096,"widgetProgramQuantum":64,"widgetProgramAggregateQuantum":64})
    }
    #[test]
    fn public_autostart_requires_exact_4096_fuel_and_64_quantum_capabilities() {
        let caps=public_caps();assert!(supported(&caps));
        for (key,expected) in [("widgetProgramAggregateFuel",4096),("widgetProgramMaxFuel",4096),("widgetProgramQuantum",64),("widgetProgramAggregateQuantum",64)]{
            let mut missing=caps.clone();missing.as_object_mut().unwrap().remove(key);
            assert!(!supported(&missing),"missing {key}");assert!(background::supported(&missing));
            for value in [Value::Null,serde_json::json!(true),serde_json::json!(expected.to_string()),serde_json::json!(0),serde_json::json!(-1),
                serde_json::json!(expected-1),serde_json::json!(expected+1),serde_json::json!(expected*2),serde_json::json!(1.5),serde_json::json!([]),serde_json::json!({})]{
                let mut invalid=caps.clone();invalid[key]=value;assert!(!supported(&invalid),"{key}: {}",invalid[key]);assert!(background::supported(&invalid));
            }
        }
        let legacy=serde_json::json!({"widgetProgram":"p4-component-program-v1","widgetProgramBackground":background::ENGINE,
            "widgetProgramInspect":background::INSPECT,"widgetProgramBackgroundMaxActive":1});
        assert!(background::supported(&legacy));assert!(!supported(&legacy));
    }
    fn proof() -> VerifiedProgram {
        VerifiedProgram::from_device("p4-test", "focus", &"a".repeat(64),
            &public_caps(),
            &serde_json::json!({"boardDeviceId":"p4-test","ok":true,"complete":true,"recoveryBlocked":false,"items":[{"id":"focus"}]}),
            &inspection(BOOT)).unwrap()
    }
    fn setup(unknown: bool) -> (tempfile::TempDir, Arc<FakeHost>, ProgramAutostart, Target) {
        let dir = tempfile::tempdir().unwrap(); let proof = proof(); let target = proof.target.clone();
        let host = Arc::new(FakeHost { proof: Mutex::new(proof), failure: Mutex::new(None), final_failure: Mutex::new(None),
            sends: AtomicUsize::new(0), unknown, journal: dir.path().join("program-autostart-v1.json") });
        let service = ProgramAutostart::open(dir.path(), host.clone()).unwrap();
        (dir, host, service, target)
    }
    #[test]
    fn explicit_enable_has_no_send_and_preview_cannot_authorize_replaced_content() {
        let (_dir, host, service, target) = setup(false);
        assert!(!service.status().unwrap().enabled);
        assert!(service.enable(target.clone(), false).is_err());
        assert_eq!(service.inspect_target("p4-test", "focus").unwrap(), target);
        host.proof.lock().unwrap().target.content_hash = "b".repeat(64);
        assert!(service.enable(target.clone(), true).is_err());
        host.proof.lock().unwrap().target = target.clone();
        assert!(service.enable(target, true).unwrap().enabled);
        assert_eq!(host.sends.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn attempt_is_on_disk_before_send_and_same_boot_never_replays_after_restart_or_reenable() {
        for unknown in [false, true] {
            let (dir, host, service, target) = setup(unknown);
            service.enable(target.clone(), true).unwrap();
            let code = if unknown { Code::ResultUnknownNoReplay } else { Code::Running };
            assert_eq!(service.observe("p4-test", BOOT).unwrap().code, code);
            assert_eq!(service.observe("p4-test", BOOT).unwrap().code, code);
            service.disable().unwrap(); service.enable(target.clone(), true).unwrap();
            assert_eq!(service.observe("p4-test", BOOT).unwrap().code, code);
            drop(service);
            let service = ProgramAutostart::open(dir.path(), host.clone()).unwrap();
            assert_eq!(service.observe("p4-test", BOOT).unwrap().code, code);
            assert_eq!(host.sends.load(Ordering::SeqCst), 1);
            host.proof.lock().unwrap().snapshot.boot_id = NEXT.into();
            service.observe("p4-test", NEXT).unwrap();
            assert_eq!(host.sends.load(Ordering::SeqCst), 2);
            // Older-boot event cannot evict the new one or restart the old one.
            service.observe("p4-test", BOOT).unwrap();
            assert_eq!(host.sends.load(Ordering::SeqCst), 2);
        }
    }
    #[test]
    fn busy_read_and_changed_boot_are_terminal_for_the_claimed_boot() {
        let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
        *host.failure.lock().unwrap() = Some(CheckFailure::Unavailable);
        assert_eq!(service.observe("p4-test", BOOT).unwrap().code, Code::VerificationUnavailable);
        *host.failure.lock().unwrap() = None;
        assert_eq!(service.observe("p4-test", BOOT).unwrap().code, Code::VerificationUnavailable);
        assert_eq!(service.observe("p4-test", NEXT).unwrap().code, Code::BootChanged);
        host.proof.lock().unwrap().snapshot.boot_id = NEXT.into();
        service.observe("p4-test", NEXT).unwrap();
        assert_eq!(host.sends.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn update_delete_device_switch_revoke_and_never_restore_consent_on_return() {
        for code in [Code::RevokedContentChanged, Code::RevokedComponentDeleted, Code::RevokedDeviceChanged] {
            let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
            let result = match code {
                Code::RevokedContentChanged => service.component_changed("focus", false),
                Code::RevokedComponentDeleted => service.component_changed("focus", true),
                _ => service.device_changed("p4-other"),
            }.unwrap();
            assert!(!result.enabled); assert_eq!(result.code, code);
            service.device_changed("p4-test").unwrap(); service.observe("p4-test", BOOT).unwrap();
            assert_eq!(host.sends.load(Ordering::SeqCst), 0);
        }
    }
    #[test]
    fn fresh_checksums_or_local_hash_differences_revoke_instead_of_selecting_current_content() {
        for local in [false, true] {
            let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
            if local { host.proof.lock().unwrap().target.content_hash = "b".repeat(64); }
            else { host.proof.lock().unwrap().target.checksums.buttons_checksum = "ffffffff".into(); }
            let result = service.observe("p4-test", BOOT).unwrap();
            assert!(!result.enabled); assert_eq!(result.code, Code::RevokedContentChanged);
            assert_eq!(host.sends.load(Ordering::SeqCst), 0);
        }
    }
    #[test]
    fn final_guard_revalidation_can_revoke_an_update_between_preflight_and_send() {
        let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
        *host.final_failure.lock().unwrap() = Some(CheckFailure::ContentChanged);
        let result = service.observe("p4-test", BOOT).unwrap();
        assert!(!result.enabled); assert_eq!(result.code, Code::RevokedContentChanged);
        assert_eq!(result.attempts, 1); assert_eq!(host.sends.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn stopped_or_live_registration_is_not_automatically_resumed_or_replaced() {
        for live in [false, true] {
            let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
            host.proof.lock().unwrap().snapshot.items.push(background::Entry { widget_id: "focus".into(), registration: "1".into(),
                live, stopped: !live, checkpoint_pending: false, revision: 1, phase: 1, last_step_us: 0, max_step_us: 0 });
            assert_eq!(service.observe("p4-test", BOOT).unwrap().code, Code::OwnerPresentNoResume);
            host.proof.lock().unwrap().snapshot.items.clear(); service.observe("p4-test", BOOT).unwrap();
            assert_eq!(host.sends.load(Ordering::SeqCst), 0);
        }
    }
    #[test]
    fn audio_preemption_consumes_boot_even_before_first_start_and_unknown_stays_unknown() {
        let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
        assert_eq!(service.suppress_boot("p4-test", BOOT).unwrap().code, Code::PreemptedNoResume);
        service.observe("p4-test", BOOT).unwrap(); assert_eq!(host.sends.load(Ordering::SeqCst), 0);
        let (_dir, _host, service, target) = setup(true); service.enable(target, true).unwrap();
        service.observe("p4-test", BOOT).unwrap();
        assert_eq!(service.suppress_boot("p4-test", BOOT).unwrap().code, Code::ResultUnknownNoReplay);
    }
    #[test]
    fn preemption_before_consent_also_survives_enable_and_process_restart() {
        let (dir, host, service, target) = setup(false);
        service.suppress_boot("p4-test", BOOT).unwrap();
        assert_eq!(service.status().unwrap().attempts, 1);
        service.enable(target, true).unwrap(); drop(service);
        let service = ProgramAutostart::open(dir.path(), host.clone()).unwrap();
        assert_eq!(service.observe("p4-test", BOOT).unwrap().code, Code::PreemptedNoResume);
        assert_eq!(host.sends.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn simultaneous_connection_events_submit_at_most_one_physical_start() {
        let (_dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
        let service = Arc::new(service);
        std::thread::scope(|scope| { for _ in 0..8 { let service = service.clone(); scope.spawn(move || { service.observe("p4-test", BOOT).unwrap(); }); } });
        assert_eq!(host.sends.load(Ordering::SeqCst), 1); assert_eq!(service.status().unwrap().attempts, 1);
    }
    #[test]
    fn ledger_never_evicts_and_full_capacity_disables_automatic_starts() {
        let (dir, host, service, target) = setup(false); service.enable(target.clone(), true).unwrap();
        let mut journal = service.inner.lock().unwrap().journal.clone();
        journal.attempts = (0..MAX_ATTEMPTS).map(|n| Attempt { board_id: "p4-test".into(), boot_id: format!("{n:016x}"),
            component_id: Some("focus".into()), content_hash: Some(target.content_hash.clone()), code: Code::ResultUnknownNoReplay }).collect();
        drop(service); save(&dir.path().join("program-autostart-v1.json"), &journal).unwrap();
        let service = ProgramAutostart::open(dir.path(), host.clone()).unwrap();
        let result = service.observe("p4-test", BOOT).unwrap();
        assert_eq!(result.code, Code::JournalFull); assert!(!result.enabled); assert_eq!(result.attempts, MAX_ATTEMPTS);
        assert!(service.enable(target, true).is_err()); assert_eq!(host.sends.load(Ordering::SeqCst), 0);
    }
    #[test]
    fn storage_failure_before_attempt_commit_prevents_send_and_blocks_the_service() {
        let (dir, host, service, target) = setup(false); service.enable(target, true).unwrap();
        fs::remove_file(&host.journal).unwrap(); fs::create_dir(&host.journal).unwrap();
        assert!(service.observe("p4-test", BOOT).is_err());
        assert_eq!(service.status().unwrap().code, Code::StorageUnavailable);
        assert!(service.observe("p4-test", BOOT).is_err()); assert_eq!(host.sends.load(Ordering::SeqCst), 0);
        // TempDir retains ownership throughout this deliberately broken journal.
        assert!(dir.path().exists());
    }
    #[test]
    fn second_process_instance_corrupt_or_deleted_journal_fail_closed() {
        let (dir, host, service, _) = setup(false);
        assert!(ProgramAutostart::open(dir.path(), host.clone()).is_err()); drop(service);
        fs::write(&host.journal, b"{bad").unwrap();
        assert!(ProgramAutostart::open(dir.path(), host.clone()).is_err());
        fs::remove_file(&host.journal).unwrap(); assert!(ProgramAutostart::open(dir.path(), host).is_err());
    }
    #[test]
    fn availability_wrapper_never_propagates_optional_policy_failure_to_app_setup() {
        assert_eq!(ProgramAutostartState::unavailable().service().err().as_deref(), Some(UNAVAILABLE_ERROR));
        let (dir, host, service, _) = setup(false);
        let locked = ProgramAutostartState::open(dir.path(), host.clone());
        assert!(locked.service.is_none()); assert_eq!(locked.service().err().as_deref(), Some(UNAVAILABLE_ERROR));
        drop(service);
        let available = ProgramAutostartState::open(dir.path(), host.clone());
        assert!(available.service.is_some()); drop(available);
        fs::write(&host.journal, b"{bad").unwrap();
        let corrupt = ProgramAutostartState::open(dir.path(), host);
        assert!(corrupt.service.is_none()); assert_eq!(corrupt.service().err().as_deref(), Some(UNAVAILABLE_ERROR));
    }
    #[test]
    fn full_hash_tracks_bytes_and_names_and_rejects_unbounded_packages() {
        let dir = tempfile::tempdir().unwrap(); fs::write(dir.path().join("component.json"), b"{}").unwrap();
        let before = component_content_hash(dir.path()).unwrap(); assert_eq!(before.len(), 64);
        fs::write(dir.path().join("component.json"), b"{ }").unwrap(); assert_ne!(before, component_content_hash(dir.path()).unwrap());
        fs::write(dir.path().join("component.json"), b"{}").unwrap();
        fs::rename(dir.path().join("component.json"), dir.path().join("other.json")).unwrap();
        assert_ne!(before, component_content_hash(dir.path()).unwrap());
        fs::write(dir.path().join("large"), vec![0; 16 * 1024 * 1024 + 1]).unwrap();
        assert!(component_content_hash(dir.path()).is_err());
    }
    #[cfg(unix)]
    #[test]
    fn hash_rejects_symlinks_instead_of_authorizing_their_targets() {
        let dir = tempfile::tempdir().unwrap(); fs::write(dir.path().join("a"), b"{}").unwrap();
        std::os::unix::fs::symlink(dir.path().join("a"), dir.path().join("link")).unwrap();
        assert!(component_content_hash(dir.path()).is_err());
    }
}
