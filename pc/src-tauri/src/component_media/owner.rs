//! Radio owner admission shared by all Service instances. No timers or connection
//! generations can retire an unresolved physical-board/boot lease. The storage
//! adapter commits an unresolved record before the first possible mutation.
//! A correlated empty-session, zero-buffer terminal rejection can retire an
//! active lease without treating the rejected chunk as successful or replaying it.
use std::sync::Mutex;

// Held for Host lifetime so another app process cannot overwrite stale journal
// snapshots. Nonblocking admission; no retry loop, PID inference or global cache.
pub(super) fn journal_lock(path: &std::path::Path) -> Result<std::fs::File, String> {
    let error = || "电台状态记录已由其他进程持有或不可用".to_string();
    let parent = path.parent().ok_or_else(error)?;
    std::fs::create_dir_all(parent).map_err(|_| error())?;
    match std::fs::symlink_metadata(path) {
        Ok(m) if !m.is_file() => return Err(error()),
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => return Err(error()),
        _ => {}
    }
    let mut options = std::fs::OpenOptions::new();
    options.read(true).write(true).create(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.mode(0o600);
    }
    let file = options.open(path).map_err(|_| error())?;
    if !file.metadata().map_err(|_| error())?.is_file() {
        return Err(error());
    }
    file.try_lock().map_err(|_| error())?;
    Ok(file)
}
const MAX_SCOPES: usize = 128;
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Scope {
    pub board: String,
    pub boot: String,
}
impl Scope {
    pub fn new(board: &str, boot: &str) -> Result<Self, String> {
        if board.is_empty()
            || board.len() > 128
            || !board
                .bytes()
                .all(|c| c.is_ascii_alphanumeric() || b"._:-".contains(&c))
            || boot.len() != 16
            || !boot
                .bytes()
                .all(|c| c.is_ascii_digit() || (b'a'..=b'f').contains(&c))
        {
            return Err("电台需要已确认的设备和启动身份，请连接支持启动身份的固件".into());
        }
        Ok(Self {
            board: board.into(),
            boot: boot.into(),
        })
    }
}
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct Record {
    pub scope: Scope,
    pub unresolved: bool,
    pub owner: String,
    pub session: String,
}
pub(super) trait Storage: Send + Sync {
    fn read(&self) -> Result<Vec<Record>, String>;
    /// Must atomically persist and sync before returning success.
    fn write(&self, records: &[Record]) -> Result<(), String>;
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum Op {
    Library,
    Begin,
    Chunk,
    Query,
    Stop,
}
/// Parse only typed fields of the correlated media/status response. A stop
/// receipt with another live session is not proof of a completed stop.
pub(super) fn validate_status(
    op: Op,
    expected_session: &str,
    ok: Option<bool>,
    buffered: Option<u64>,
    state: Option<&str>,
    session: Option<&str>,
) -> Result<(), String> {
    if ok != Some(true)
        || buffered.is_none()
        || !matches!(
            state,
            Some("playing" | "buffering" | "paused" | "ended" | "idle" | "interrupted" | "error")
        )
    {
        return Err("电台状态回执无效".into());
    }
    if op == Op::Stop
        && (session != Some("")
            || buffered != Some(0)
            || !matches!(state, Some("idle" | "error" | "ended" | "interrupted")))
    {
        return Err("停止回执未证明本次播放已结束".into());
    }
    if matches!(op, Op::Begin | Op::Chunk | Op::Query) && session != Some(expected_session) {
        return Err("电台回执会话不匹配".into());
    }
    Ok(())
}
#[derive(Debug)]
pub(super) enum Outcome<T> {
    Sent(T),
    DeviceStopped(T),
    // The exact current session rejected one chunk; owned stop is still required.
    RejectedActive(T),
    KnownIdle,
    // A newer acknowledged owner superseded this exact old session. No wire.
    Retired,
}
pub(super) enum Reply<T> { Accepted(T), DeviceStopped(T), RejectedActive(T) }
/// Raw typed fields from one correlated read-only media/query response. This is
/// not an admitted recovery result: Guard validates every field against its own
/// recorded session. The caller must check the same board/boot/connection before
/// and after the query, and must never substitute a stop/begin for this read.
#[derive(Clone, Debug, Default)]
pub(super) struct RecoveryProof {
    pub ok: Option<bool>,
    pub buffered_ms: Option<u64>,
    pub state: Option<String>,
    pub session_id: Option<String>,
    pub message: Option<String>,
}
/// Only an exact active-session negative chunk with a known non-acceptance
/// message qualifies. A query/begin/stop rejection cannot use this exception.
pub(super) fn rejected_active(op:Op,expected_session:&str,ok:Option<bool>,buffered:Option<u64>,state:Option<&str>,session:Option<&str>,message:Option<&str>)->bool {
    op==Op::Chunk && id(expected_session,128) && ok==Some(false) && buffered.is_some()
        && matches!(state,Some("playing"|"buffering"|"paused")) && session==Some(expected_session)
        && matches!(message,Some("音乐数据顺序或格式无效"|"音乐缓冲区已满"))
}
fn recovery_state(expected_session:&str,proof:&RecoveryProof)->Option<State> {
    let session=proof.session_id.as_deref();let state=proof.state.as_deref();
    if proof.ok==Some(true) && proof.buffered_ms.is_some()
        && matches!(state,Some("playing"|"buffering"|"paused")) && session==Some(expected_session) {
        return Some(State::Active);
    }
    if proof.buffered_ms==Some(0) && matches!(state,Some("idle"|"ended"|"interrupted"|"error"))
        && (session==Some("") || session==Some(expected_session))
        && (proof.ok==Some(true) || (proof.ok==Some(false) && session==Some("")
            && proof.message.as_deref()==Some("音乐会话不匹配"))) {
        return Some(State::Idle);
    }
    None
}
/// Only negative Chunk/Query replies with complete stopped-state proof qualify.
/// Correlation and board/boot/connection checks belong to the transport caller.
pub(super) fn stopped_rejection(op:Op,ok:Option<bool>,buffered:Option<u64>,state:Option<&str>,session:Option<&str>)->bool {
    matches!(op,Op::Chunk|Op::Query) && ok==Some(false) && buffered==Some(0)
        && session==Some("") && matches!(state,Some("idle"|"ended"|"interrupted"|"error"))
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Idle,
    Active,
    Unknown,
}
struct Lease {
    record: Record,
    state: State,
}
struct Inner {
    leases: Vec<Lease>,
    invalid: bool,
}
pub(super) struct Guard {
    inner: Mutex<Inner>,
    storage: Box<dyn Storage>,
}
fn id(value: &str, max: usize) -> bool {
    !value.is_empty()
        && value.len() <= max
        && value
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"._:-".contains(&b))
}
fn unknown() -> String {
    "设备播放结果尚未确认；未重放停止、音频或新播放，请先确认设备已真实重新启动".into()
}
impl Guard {
    pub fn new(storage: Box<dyn Storage>) -> Self {
        let loaded = storage.read().and_then(|records| {
            if records.len() > MAX_SCOPES {
                return Err(unknown());
            }
            for (i, r) in records.iter().enumerate() {
                Scope::new(&r.scope.board, &r.scope.boot)?;
                if records[..i].iter().any(|old| old.scope == r.scope)
                    || r.unresolved && (!id(&r.owner, 128) || !id(&r.session, 128))
                    || !r.unresolved && (!r.owner.is_empty() || !r.session.is_empty())
                {
                    return Err(unknown());
                }
            }
            Ok(records)
        });
        let (leases, invalid) = match loaded {
            Ok(records) => (
                records
                    .into_iter()
                    .map(|record| Lease {
                        state: if record.unresolved {
                            State::Unknown
                        } else {
                            State::Idle
                        },
                        record,
                    })
                    .collect(),
                false,
            ),
            Err(_) => (Vec::new(), true),
        };
        Self {
            inner: Mutex::new(Inner { leases, invalid }),
            storage,
        }
    }
    fn save(&self, inner: &Inner) -> Result<(), String> {
        self.storage
            .write(
                &inner
                    .leases
                    .iter()
                    .map(|l| l.record.clone())
                    .collect::<Vec<_>>(),
            )
            .map_err(|_| unknown())
    }
    pub fn ensure_known(&self, scope: &Scope) -> Result<(), String> {
        let inner = self.inner.lock().map_err(|_| unknown())?;
        if inner.invalid
            || inner
                .leases
                .iter()
                .any(|l| l.record.scope == *scope && l.state == State::Unknown)
        {
            Err(unknown())
        } else {
            Ok(())
        }
    }
    /// Explicit user entry only; callers must not invoke this from auto fallback
    /// or retry loops. Reconcile only this owner's unresolved lease, including an
    /// acknowledged Active lease, under the same admission lock as the writer.
    /// Some(old_session) permits a subsequent explicit owned-stop, not a new
    /// begin. None means known Idle. No stop, begin, chunk or replay occurs here.
    pub fn reconcile_unknown(&self,scope:&Scope,owner:&str,
        read:impl FnOnce(&str)->Result<RecoveryProof,String>)->Result<Option<String>,String> {
        Scope::new(&scope.board,&scope.boot)?;
        if !id(owner,128) {return Err("电台播放身份无效".into());}
        let mut inner=self.inner.lock().map_err(|_|unknown())?;
        if inner.invalid {return Err(unknown());}
        let Some(index)=inner.leases.iter().position(|l|l.record.scope==*scope) else {return Ok(None);};
        if inner.leases[index].state==State::Idle {return Ok(None);}
        if inner.leases[index].record.owner!=owner {
            return Err("其他电台仍拥有播放设备，未核对或修改其会话".into());
        }
        if !inner.leases[index].record.unresolved || !id(&inner.leases[index].record.session,128) {
            inner.invalid=true;return Err(unknown());
        }
        let old_session=inner.leases[index].record.session.clone();
        let proof=match read(&old_session) {
            Ok(proof)=>proof,
            Err(_)=>{inner.leases[index].state=State::Unknown;return Err(unknown());}
        };
        match recovery_state(&old_session,&proof) {
            Some(State::Active)=>{
                inner.leases[index].state=State::Active;
                // Durable unresolved ownership is deliberately retained.
                Ok(Some(old_session))
            },
            Some(State::Idle)=>{
                let lease=&mut inner.leases[index];lease.state=State::Idle;
                lease.record.unresolved=false;lease.record.owner.clear();lease.record.session.clear();
                if self.save(&inner).is_err(){inner.invalid=true;return Err(unknown());}
                Ok(None)
            },
            _=>{inner.leases[index].state=State::Unknown;Err(unknown())}
        }
    }
    /// Connection loss cannot prove that the last active session was stopped.
    pub fn poison_owner(&self, owner: &str) {
        if let Ok(mut inner) = self.inner.lock() {
            for lease in &mut inner.leases {
                if lease.record.owner == owner && lease.record.unresolved {
                    lease.state = State::Unknown;
                }
            }
        }
        // Unresolved records were synced before any wire mutation; never erase them here.
    }
    pub fn observe_idle<T>(&self, scope: &Scope, read: impl FnOnce() -> Result<T, String>) -> Result<T, String> {
        Scope::new(&scope.board, &scope.boot)?;
        let inner = self.inner.lock().map_err(|_| unknown())?;
        if inner.invalid || inner.leases.iter().any(|lease|
            lease.record.scope == *scope && lease.state != State::Idle) {
            return Err(unknown());
        }
        // Read-only query, held under the same scoped admission as mutations.
        // No journal erase, state promotion, or manufactured idle acknowledgement.
        read()
    }
    pub fn poison_scoped_owner(&self, owner: &str, scope: &Scope) {
        if let Ok(mut inner) = self.inner.lock() {
            for lease in &mut inner.leases {
                if lease.record.owner == owner && lease.record.scope == *scope && lease.record.unresolved {
                    lease.state = State::Unknown;
                }
            }
        }
        // The unresolved durable record stays intact; no stop/idle proof is invented.
    }
    pub fn request<T>(
        &self,
        scope: &Scope,
        owner: &str,
        session: &str,
        op: Op,
        send: impl FnOnce() -> Result<T, String>,
    ) -> Result<Outcome<T>, String> {
        self.request_observed(scope,owner,session,op,||send().map(Reply::Accepted))
    }
    pub fn request_observed<T>(&self,scope:&Scope,owner:&str,session:&str,op:Op,
        send:impl FnOnce()->Result<Reply<T>,String>)->Result<Outcome<T>,String> {
        Scope::new(&scope.board, &scope.boot)?;
        if !id(owner, 128) || op != Op::Library && !id(session, 128) {
            return Err("电台播放身份无效".into());
        }
        let mut inner = self.inner.lock().map_err(|_| unknown())?;
        if inner.invalid {
            return Err(unknown());
        }
        let index = match inner.leases.iter().position(|l| l.record.scope == *scope) {
            Some(i) => i,
            None => {
                if inner.leases.len() >= MAX_SCOPES {
                    // Only positively resolved Idle leases are reclaimable. The
                    // next mutation persists this compacted table before wire IO.
                    inner.leases.retain(|lease| {
                        !(lease.state == State::Idle && !lease.record.unresolved)
                    });
                }
                if inner.leases.len() >= MAX_SCOPES {
                    return Err("电台设备状态记录已满，未开始新播放".into());
                }
                inner.leases.push(Lease {
                    record: Record {
                        scope: scope.clone(),
                        unresolved: false,
                        owner: String::new(),
                        session: String::new(),
                    },
                    state: State::Idle,
                });
                inner.leases.len() - 1
            }
        };
        let lease = &inner.leases[index];
        if lease.state == State::Unknown {
            return Err(unknown());
        }
        let exact = lease.record.owner == owner && lease.record.session == session;
        match op {
            Op::Begin if lease.state != State::Idle => {
                return Err("旧电台尚未确认停止，未开始新播放".into())
            }
            Op::Stop if lease.state == State::Active && !exact => return Ok(Outcome::Retired),
            Op::Chunk | Op::Query if lease.state == State::Active && !exact => {
                return Err("电台播放主体已变化，未发送旧会话操作".into())
            }
            Op::Chunk | Op::Query if lease.state == State::Idle => {
                return Err("电台没有当前已确认的播放会话".into())
            }
            Op::Library if lease.state == State::Active && lease.record.owner != owner => {
                return Err("其他电台仍拥有播放设备，未修改列表或音量".into())
            }
            Op::Stop if lease.state == State::Idle => return Ok(Outcome::KnownIdle),
            _ => {}
        }
        let idle_library = op == Op::Library && inner.leases[index].state == State::Idle;
        if op == Op::Begin || idle_library {
            let record = &mut inner.leases[index].record;
            record.unresolved = true;
            record.owner = owner.into();
            record.session = if idle_library {
                "library".into()
            } else {
                session.into()
            };
            inner.leases[index].state = State::Unknown;
            if self.save(&inner).is_err() {
                inner.invalid = true;
                return Err(unknown());
            }
        }
        // The mutex owns admission through the actual writer and acknowledgement.
        let reply = match send() {
            Ok(value) => value,
            Err(_) => {
                inner.leases[index].state = State::Unknown;
                return Err(unknown());
            }
        };
        let (reply,device_stopped,rejected_active)=match reply {
            Reply::Accepted(value)=>(value,false,false),
            Reply::DeviceStopped(value) if matches!(op,Op::Chunk|Op::Query)
                && inner.leases[index].state==State::Active && exact => (value,true,false),
            Reply::RejectedActive(value) if op==Op::Chunk
                && inner.leases[index].state==State::Active && exact => (value,false,true),
            _=>{inner.leases[index].state=State::Unknown;return Err(unknown());}
        };
        if op == Op::Stop || idle_library || device_stopped {
            let lease = &mut inner.leases[index];
            lease.state = State::Idle;
            lease.record.unresolved = false;
            lease.record.owner.clear();
            lease.record.session.clear();
            if self.save(&inner).is_err() {
                inner.invalid = true;
                return Err(unknown());
            }
        } else if op == Op::Begin {
            inner.leases[index].state = State::Active;
        }
        if device_stopped {Ok(Outcome::DeviceStopped(reply))}
        else if rejected_active {Ok(Outcome::RejectedActive(reply))}
        else {Ok(Outcome::Sent(reply))}
    }
}

#[cfg(test)]
#[path = "owner_tests.rs"]
mod tests;
