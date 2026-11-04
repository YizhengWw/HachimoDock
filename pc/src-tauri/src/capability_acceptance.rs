//! Test-only, opt-in JSON-plan driver of the production USB reader. No retries,
//! shell commands, arbitrary topics, network provider, or implicit cleanup.
use super::{events, identity, pull, Disconnect, ProductionPull};
use crate::usb_serial::UsbSerialManager;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use sha2::{Digest, Sha256};
use std::{
    collections::{BTreeMap, BTreeSet},
    fs::{self, OpenOptions},
    io::{Read, Write},
    path::{Component, Path, PathBuf},
    sync::{
        atomic::{AtomicU32, Ordering},
        mpsc, Arc,
    },
    time::{Duration, Instant},
};
const MAX_PLAN: u64 = 256 * 1024;
const QUEUE: usize = 128;
const MAX_STEPS: usize = 128;
type Result<T> = std::result::Result<T, String>;
fn need(ok: bool, why: &str) -> Result<()> {
    if ok {
        Ok(())
    } else {
        Err(why.into())
    }
}
fn hex(s: &str) -> bool {
    s.len() == 64
        && s.bytes()
            .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
}
fn token(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 64
        && s.bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"_.-".contains(&b))
}
fn hash(path: &Path) -> Result<String> {
    let mut f = fs::File::open(path).map_err(|e| e.to_string())?;
    need(
        f.metadata().map_err(|e| e.to_string())?.len() <= 1024 * 1024 * 1024,
        "hash input too large",
    )?;
    let mut h = Sha256::new();
    let mut b = [0u8; 8192];
    loop {
        let n = f.read(&mut b).map_err(|e| e.to_string())?;
        if n == 0 {
            break;
        }
        h.update(&b[..n]);
    }
    Ok(format!("{:x}", h.finalize()))
}
fn write_new(path: &Path, value: &Value) -> Result<()> {
    let mut f = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(path)
        .map_err(|e| e.to_string())?;
    f.write_all(&serde_json::to_vec_pretty(value).map_err(|e| e.to_string())?)
        .map_err(|e| e.to_string())?;
    f.sync_all().map_err(|e| e.to_string())
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Expected {
    board_id: String,
    boot_id: String,
    firmware: String,
    build_id: String,
    elf_sha256: String,
    elf_path: PathBuf,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Fixture {
    id: String,
    path: PathBuf,
    files: BTreeMap<String, String>,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Plan {
    schema_version: u32,
    plan_id: String,
    source_digest: String,
    reader_sha256: String,
    expected: Expected,
    fixtures: Vec<Fixture>,
    steps: Vec<Step>,
}
#[derive(Debug, Deserialize, Serialize)]
#[serde(tag = "op", rename_all = "camelCase", deny_unknown_fields)]
enum Step {
    Diagnostics,
    Inventory,
    Programs,
    State,
    Install { fixture: String },
    Activate { fixture: String, show: bool },
    Action { fixture: String, action: String },
    Hide,
    RestoreSelection,
    Start { fixture: String },
    Stop { fixture: String },
    Remove { fixture: String },
    Screenshot { label: String },
    Delay { ms: u64 },
}
impl Step {
    fn mutation(&self) -> bool {
        matches!(
            self,
            Self::Install { .. }
                | Self::Activate { .. }
                | Self::Action { .. }
                | Self::Hide
                | Self::RestoreSelection
                | Self::Start { .. }
                | Self::Stop { .. }
                | Self::Remove { .. }
        )
    }
    fn fixture(&self) -> Option<&str> {
        match self {
            Self::Install { fixture }
            | Self::Activate { fixture, .. }
            | Self::Action { fixture, .. }
            | Self::Start { fixture }
            | Self::Stop { fixture }
            | Self::Remove { fixture } => Some(fixture),
            _ => None,
        }
    }
}
fn parse(bytes: &[u8]) -> Result<Plan> {
    need(bytes.len() <= MAX_PLAN as usize, "plan byte limit")?;
    // Serde's internally tagged unit variants can otherwise ignore payload
    // fields. Check their allowed keys explicitly, then deserialize original
    // bytes so duplicate struct fields are still rejected by Serde.
    let raw: Value = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    if let Some(steps) = raw["steps"].as_array() {
        for step in steps {
            let object = step.as_object().ok_or("step object required")?;
            let keys: &[&str] = match step["op"].as_str() {
                Some(
                    "diagnostics" | "inventory" | "programs" | "state" | "hide"
                    | "restoreSelection",
                ) => &["op"],
                Some("install" | "start" | "stop" | "remove") => &["op", "fixture"],
                Some("activate") => &["op", "fixture", "show"],
                Some("action") => &["op", "fixture", "action"],
                Some("screenshot") => &["op", "label"],
                Some("delay") => &["op", "ms"],
                _ => return Err("unknown operation".into()),
            };
            need(
                object.keys().all(|key| keys.contains(&key.as_str())),
                "unknown step fields",
            )?;
        }
    }
    let p: Plan = serde_json::from_slice(bytes).map_err(|e| e.to_string())?;
    validate(&p)?;
    Ok(p)
}
/// Pure validation, including declared ownership sequencing. Runtime ownership
/// is stricter: only confirmed installation/start receipts create authority.
fn validate(p: &Plan) -> Result<()> {
    need(
        p.schema_version == 1 && token(&p.plan_id),
        "schemaVersion/planId",
    )?;
    need(
        hex(&p.source_digest) && hex(&p.reader_sha256) && hex(&p.expected.elf_sha256),
        "hash format",
    )?;
    need(
        token(&p.expected.board_id) && p.expected.board_id.len() <= 47,
        "boardId",
    )?;
    need(
        p.expected.boot_id.len() == 16
            && p.expected
                .boot_id
                .bytes()
                .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b)),
        "bootId",
    )?;
    need(
        !p.expected.firmware.is_empty()
            && token(&p.expected.firmware)
            && p.expected
                .build_id
                .starts_with(&(p.expected.firmware.clone() + "+"))
            && p.expected.build_id.len() <= 95
            && p.expected
                .build_id
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b"_.-+".contains(&b)),
        "firmware/buildId",
    )?;
    need(
        p.expected.elf_path.is_absolute(),
        "absolute ELF path required",
    )?;
    need(
        p.fixtures.len() <= 8 && !p.steps.is_empty() && p.steps.len() <= MAX_STEPS,
        "plan count limit",
    )?;
    let mut ids = BTreeSet::new();
    for f in &p.fixtures {
        need(
            crate::app_program_background::valid_widget_id(&f.id) && ids.insert(f.id.clone()),
            "fixture ID duplicate/invalid",
        )?;
        need(
            f.path.is_absolute() && f.path.extension().is_none_or(|s| s != "clawpkg"),
            "fixture must be an absolute directory",
        )?;
        need(
            !f.files.is_empty() && f.files.len() <= 128,
            "fixture file count",
        )?;
        for (name, digest) in &f.files {
            need(
                !name.is_empty()
                    && name.len() <= 200
                    && !name.contains('\\')
                    && Path::new(name)
                        .components()
                        .all(|c| matches!(c, Component::Normal(_)))
                    && hex(digest),
                "fixture file/hash",
            )?;
            need(
                name.split('/')
                    .all(|v| !v.is_empty() && v != "." && v != ".."),
                "noncanonical relative path",
            )?;
        }
        for name in [
            "component.json",
            "runtime/widget.json",
            "buttons.json",
            "negative-screen.json",
            "share.json",
        ] {
            need(f.files.contains_key(name), "missing fixture declaration")?;
        }
    }
    let mut owned = BTreeSet::new();
    let mut ever = BTreeSet::new();
    let mut started = BTreeSet::new();
    let mut labels = BTreeSet::new();
    let mut delay = 0u64;
    let mut screens = 0;
    let mut selection_dirty = false;
    for s in &p.steps {
        if s.mutation() {
            selection_dirty = !matches!(s, Step::RestoreSelection);
        }
        if let Some(id) = s.fixture() {
            need(ids.contains(id), "undeclared fixture")?;
        }
        match s {
            Step::Install { fixture } => {
                need(ever.insert(fixture.clone()), "no reinstall/replay")?;
                owned.insert(fixture.clone());
            }
            Step::Remove { fixture } => {
                need(
                    owned.remove(fixture) && !started.contains(fixture),
                    "remove requires owned stopped fixture",
                )?;
            }
            Step::Start { fixture } => {
                need(
                    owned.contains(fixture) && started.insert(fixture.clone()),
                    "start requires owned nonrunning fixture",
                )?;
            }
            Step::Stop { fixture } => {
                need(
                    owned.contains(fixture) && started.remove(fixture),
                    "stop requires this plan start",
                )?;
            }
            Step::Activate { fixture, .. } | Step::Action { fixture, .. } => need(
                owned.contains(fixture),
                "mutation requires this plan install",
            )?,
            Step::Hide => need(!owned.is_empty(), "hide requires this plan own fixture")?,
            Step::Screenshot { label } => {
                screens += 1;
                need(
                    token(label) && labels.insert(label.clone()) && screens <= 8,
                    "screenshot label/count",
                )?;
            }
            Step::Delay { ms } => {
                delay = delay.checked_add(*ms).ok_or("delay overflow")?;
                need(*ms <= 30_000 && delay <= 120_000, "delay budget")?;
            }
            _ => {}
        }
        if let Step::Action { action, .. } = s {
            need(token(action), "action token")?;
        }
    }
    need(
        !selection_dirty,
        "mutating plans require final explicit restoreSelection",
    )?;
    Ok(())
}
fn checked_fixture(f: &Fixture) -> Result<PathBuf> {
    // Inspect the whole directory, not just paths named by the plan. No links,
    // device files, hidden unpinned extras or unbounded recursion are accepted.
    fn walk(
        root: &Path,
        cur: &Path,
        depth: usize,
        files: &mut BTreeMap<String, String>,
        total: &mut u64,
        nodes: &mut usize,
    ) -> Result<()> {
        need(depth <= 12, "fixture nesting")?;
        for e in fs::read_dir(cur).map_err(|e| e.to_string())? {
            let e = e.map_err(|e| e.to_string())?;
            *nodes += 1;
            need(*nodes <= 256, "fixture directory entry budget")?;
            let meta = fs::symlink_metadata(e.path()).map_err(|e| e.to_string())?;
            need(!meta.file_type().is_symlink(), "fixture symlink")?;
            if meta.is_dir() {
                walk(root, &e.path(), depth + 1, files, total, nodes)?;
            } else {
                need(
                    meta.is_file() && meta.len() <= 4 * 1024 * 1024 && files.len() < 128,
                    "fixture file budget/type",
                )?;
                *total += meta.len();
                need(*total <= 16 * 1024 * 1024, "fixture total budget")?;
                let path = e.path();
                let rel = path
                    .strip_prefix(root)
                    .map_err(|e| e.to_string())?
                    .to_str()
                    .ok_or("non UTF-8 fixture path")?
                    .to_owned();
                files.insert(rel, hash(&path)?);
            }
        }
        Ok(())
    }
    need(
        fs::symlink_metadata(&f.path)
            .map_err(|e| e.to_string())?
            .is_dir(),
        "fixture directory required",
    )?;
    let root = f.path.canonicalize().map_err(|e| e.to_string())?;
    need(
        root == f.path,
        "fixture path must be canonical (no ancestor symlink)",
    )?;
    let mut files = BTreeMap::new();
    let mut total = 0;
    walk(&root, &root, 0, &mut files, &mut total, &mut 0)?;
    need(
        files == f.files,
        "fixture contents differ from reviewed hashes",
    )?;
    let validation = crate::clawpkg::validate_clawpkg_at_path(&root)?;
    need(
        validation.ok && validation.manifest.as_ref().is_some_and(|m| m.id == f.id),
        "production package validation/manifest ID",
    )?;
    let component: Value =
        serde_json::from_slice(&fs::read(root.join("component.json")).map_err(|e| e.to_string())?)
            .map_err(|e| e.to_string())?;
    need(component["id"] == f.id, "component declaration ID")?;
    Ok(root)
}
fn inventory(v: &Value, board: &str) -> Result<BTreeSet<String>> {
    need(
        v["boardDeviceId"] == board
            && v["ok"] == true
            && v["complete"] == true
            && v["recoveryBlocked"] == false,
        "unconfirmed inventory",
    )?;
    let rows = v["items"]
        .as_array()
        .filter(|a| a.len() <= 32)
        .ok_or("inventory count")?;
    let mut ids = BTreeSet::new();
    for r in rows {
        let id = r["id"].as_str().ok_or("inventory id")?;
        need(
            crate::app_program_background::valid_widget_id(id) && ids.insert(id.into()),
            "invalid/duplicate inventory ID",
        )?;
    }
    Ok(ids)
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize)]
#[serde(rename_all = "camelCase")]
struct Selection {
    selected_widget_id: Option<String>,
    selected_index: Option<u64>,
}
fn selection(v: &Value, board: &str) -> Result<Selection> {
    inventory(v, board)?;
    let active = v
        .get("activeWidgetId")
        .ok_or("active inventory field absent")?;
    need(
        active.is_null()
            || active
                .as_str()
                .is_some_and(crate::app_program_background::valid_widget_id),
        "invalid active inventory ID",
    )?;
    let id = v
        .get("selectedWidgetId")
        .ok_or("legacy inventory has no selection ID")?;
    let index = v
        .get("selectedIndex")
        .ok_or("legacy inventory has no selection index")?;
    if id.is_null() && index.is_null() {
        return Ok(Selection {
            selected_widget_id: None,
            selected_index: None,
        });
    }
    let id = id
        .as_str()
        .filter(|s| crate::app_program_background::valid_widget_id(s))
        .ok_or("invalid selected ID")?;
    let index = index.as_u64().ok_or("invalid selected index")?;
    need(
        v["items"]
            .as_array()
            .and_then(|rows| rows.get(index as usize))
            .is_some_and(|row| row["id"] == id),
        "selected ID/index inconsistent",
    )?;
    Ok(Selection {
        selected_widget_id: Some(id.into()),
        selected_index: Some(index),
    })
}
fn selection_target(baseline: &Selection, before: &Value, board: &str) -> Result<Option<String>> {
    let current = selection(before, board)?;
    if let Some(id) = &baseline.selected_widget_id {
        let at = baseline.selected_index.ok_or("baseline index absent")?;
        need(
            before["items"]
                .as_array()
                .and_then(|rows| rows.get(at as usize))
                .is_some_and(|row| row["id"] == *id),
            "baseline selected ID absent or original catalog order changed",
        )?;
        Ok(Some(id.clone()))
    } else {
        need(
            current == *baseline,
            "baseline had no selection; no API to clear a selection",
        )?;
        Ok(None)
    }
}
fn selection_readback(
    baseline: &Selection,
    active_before: &Value,
    after: &Value,
    board: &str,
) -> Result<()> {
    need(
        selection(after, board)? == *baseline,
        "baseline selection was not restored",
    )?;
    need(
        after.get("activeWidgetId") == Some(active_before),
        "selection changed active widget",
    )
}
fn identity_check(
    usb: &UsbSerialManager,
    rx: &mpsc::Receiver<(String, Value)>,
    e: &Expected,
) -> Result<identity::Identity> {
    need(
        usb.status().board_device_id == e.board_id && usb.status().firmware == e.firmware,
        "connected board/version changed",
    )?;
    let mut io = ProductionPull {
        usb,
        board: &e.board_id,
        rx,
        started: Instant::now(),
    };
    let id = identity::query(
        &mut io,
        &uuid::Uuid::new_v4().simple().to_string(),
        &e.board_id,
        None,
    )
    .map_err(|v| v.to_string())?;
    let value = serde_json::to_value(&id).map_err(|e| e.to_string())?;
    need(
        value["bootId"] == e.boot_id
            && value["buildId"] == e.build_id
            && value["appElfSha256"] == e.elf_sha256,
        "exact boot/build/ELF mismatch",
    )?;
    Ok(id)
}
fn ack_matches(name: &str, v: &Value, board: &str, topic: &str, rid: &str) -> Result<bool> {
    if name != "protocol/ack" || v["requestId"] != rid {
        return Ok(false);
    }
    need(
        v["requestTopic"] == topic && v["ok"].is_boolean(),
        "malformed matched ACK",
    )?;
    need(
        v.get("boardDeviceId").is_none_or(|b| b == board),
        "ACK board mismatch",
    )?;
    Ok(true)
}
fn checked_observation(v: &Value, e: &Expected, rid: &str) -> Result<()> {
    let short_hex = |value: &Value| {
        value.as_str().is_some_and(|s| {
            s.len() == 8
                && s.bytes()
                    .all(|b| b.is_ascii_digit() || (b'a'..=b'f').contains(&b))
        })
    };
    need(
        v["requestId"] == rid
            && v["bootId"] == e.boot_id
            && v["observationAvailable"] == true
            && v["observationEngine"] == "p4-miniapp-observation-fnv1a32-v1"
            && short_hex(&v["instanceGeneration"])
            && short_hex(&v["persistProjectionDigest"])
            && v["durability"] == "not_observed",
        "missing/mismatched correlated observation",
    )?;
    need(
        v["active"].is_boolean()
            && (!v["active"].as_bool().unwrap_or(false)
                || v["widgetId"]
                    .as_str()
                    .is_some_and(crate::app_program_background::valid_widget_id)),
        "observation widget identity",
    )
}
fn state_query(io: &mut impl pull::Transport, e: &Expected, rid: &str) -> Result<Value> {
    let start = io.elapsed();
    io.send("miniapp/query", &json!({"requestId":rid}))
        .map_err(|_| "state query send failed")?;
    let mut state = None;
    let mut ack = None;
    for _ in 0..512 {
        let remaining = Duration::from_secs(5)
            .checked_sub(
                io.elapsed()
                    .checked_sub(start)
                    .ok_or("state clock regressed")?,
            )
            .filter(|v| !v.is_zero())
            .ok_or("state timeout")?;
        let Some((topic, v)) = io
            .receive(remaining.min(Duration::from_millis(20)))
            .map_err(|_| "state reader closed")?
        else {
            continue;
        };
        need(
            io.elapsed()
                .checked_sub(start)
                .is_some_and(|d| d < Duration::from_secs(5)),
            "late state observation",
        )?;
        if topic == "miniapp/state" && v["requestId"] == rid {
            checked_observation(&v, e, rid)?;
            state = Some(v);
        } else if ack_matches(&topic, &v, &e.board_id, "miniapp/query", rid)? {
            need(v["ok"] == true, "state ACK rejected")?;
            ack = Some(v);
        }
        if state.is_some() && ack.is_some() {
            return Ok(
                json!({"ack":ack,"observation":state,"stateRequestCorrelated":true,"durabilityObserved":false,"sent":1,"matched":2,"retries":0}),
            );
        }
    }
    Err("state event/poll budget".into())
}
fn request_ack(
    usb: &UsbSerialManager,
    rx: &mpsc::Receiver<(String, Value)>,
    board: &str,
    topic: &str,
    mut payload: Value,
) -> Result<Value> {
    let rid = format!("cap-{}", uuid::Uuid::new_v4().simple());
    payload["requestId"] = json!(rid);
    usb.send_to_board(board, topic, &payload)?; // A successful write is NOT an acknowledgement.
    let end = Instant::now() + Duration::from_secs(5);
    let mut count = 0;

    loop {
        need(count < 512, "ACK event budget")?;
        count += 1;
        let remain = end
            .checked_duration_since(Instant::now())
            .ok_or("ACK timeout: unknown outcome")?;
        let (name, v) = rx
            .recv_timeout(remain)
            .map_err(|_| "ACK timeout/disconnect: unknown outcome")?;
        need(Instant::now() < end, "late ACK: unknown outcome")?;
        if ack_matches(&name, &v, board, topic, &rid)? {
            need(v["ok"] == true, "negative ACK; action not confirmed")?;
            return Ok(v);
        }
    }
}
/// The shared pull engine supports read retries elsewhere. This entry point
/// blocks duplicate submissions before they reach USB, including busy admission.
struct NoReplay<T> {
    inner: T,
    sent: BTreeSet<String>,
    blocked: u32,
}
impl<T: pull::Transport> pull::Transport for NoReplay<T> {
    fn elapsed(&self) -> Duration {
        self.inner.elapsed()
    }
    fn send(&mut self, topic: &str, payload: &Value) -> std::result::Result<(), ()> {
        let key = format!("{topic}:{}", payload);
        if self.sent.len() >= 256 || !self.sent.insert(key) {
            self.blocked += 1;
            return Err(());
        }
        self.inner.send(topic, payload)
    }
    fn receive(&mut self, t: Duration) -> std::result::Result<Option<(String, Value)>, ()> {
        self.inner.receive(t)
    }
    fn delay(&mut self, t: Duration) {
        self.inner.delay(t)
    }
    fn close_ack_supported(&self) -> bool {
        self.inner.close_ack_supported()
    }
}
fn screenshot(
    usb: &UsbSerialManager,
    rx: &mpsc::Receiver<(String, Value)>,
    p: &Plan,
    out: &Path,
    label: &str,
) -> Result<Value> {
    need(
        usb.status().capabilities["screenshotTransfer"] == "pull-fnv1a32-v1",
        "pull screenshot unsupported",
    )?;
    let before = identity_check(usb, rx, &p.expected)?;
    let mut io = NoReplay {
        inner: ProductionPull {
            usb,
            board: &p.expected.board_id,
            rx,
            started: Instant::now(),
        },
        sent: BTreeSet::new(),
        blocked: 0,
    };
    let result = pull::capture(
        &mut io,
        640,
        480,
        &uuid::Uuid::new_v4().simple().to_string(),
    );
    let mut capture = result.capture;
    let binding = identity::query(
        &mut io,
        &uuid::Uuid::new_v4().simple().to_string(),
        &p.expected.board_id,
        Some(&mut capture),
    )
    .and_then(|after| {
        identity::bind(
            &before,
            &after,
            &capture.report(),
            Some(&p.expected.elf_sha256),
        )
    });
    let binding_report = match &binding {
        Ok(value) => value.clone(),
        Err(error) => json!({"status":"failed","error":error}),
    };
    write_new(
        &out.join(format!("{label}.capture.json")),
        &json!({"frame":capture.report(),"identity":binding_report,"pull":result.diagnostics,"blockedDuplicateSubmissions":io.blocked}),
    )?;
    let binding = binding.map_err(|v| v.to_string())?;
    need(
        capture.report()["passed"] == true && binding["sourceBound"] == true && io.blocked == 0,
        "capture integrity/source binding",
    )?;
    let mut rgb = Vec::with_capacity(640 * 480 * 3);
    for word in capture.raw.chunks_exact(2) {
        let v = u16::from_le_bytes([word[0], word[1]]);
        let (r, g, b) = ((v >> 11) & 31, (v >> 5) & 63, v & 31);
        rgb.extend_from_slice(&[
            ((r << 3) | (r >> 2)) as u8,
            ((g << 2) | (g >> 4)) as u8,
            ((b << 3) | (b >> 2)) as u8,
        ]);
    }
    image::save_buffer(
        out.join(format!("{label}.png")),
        &rgb,
        640,
        480,
        image::ColorType::Rgb8,
    )
    .map_err(|e| e.to_string())?;
    Ok(
        json!({"label":label,"integrityPassed":true,"identity":binding,"featureAssertions":"not inferred from pixels"}),
    )
}
#[derive(Default)]
struct Authority {
    owned: BTreeSet<String>,
    running: BTreeMap<String, (String, String)>,
    locked: bool,
}
impl Authority {
    fn admit(&self, s: &Step) -> Result<()> {
        need(
            !s.mutation() || !self.locked,
            "mutation lock: unknown prior outcome; read-only only",
        )?;
        if s.mutation() && !matches!(s, Step::Install { .. }) {
            if let Some(id) = s.fixture() {
                need(self.owned.contains(id), "not a confirmed own fixture")?;
            }
        }
        if matches!(s, Step::Hide) {
            need(!self.owned.is_empty(), "no own visible fixture")?;
        }
        Ok(())
    }
}
fn run_plan(p: &Plan, out: &Path) -> Result<Value> {
    need(
        std::env::var("PET_CAPABILITY_SOURCE_DIGEST").as_deref() == Ok(p.source_digest.as_str()),
        "explicit source digest binding missing",
    )?;
    let reader = std::env::current_exe().map_err(|e| e.to_string())?;
    need(
        hash(&reader)? == p.reader_sha256,
        "reader executable SHA256 mismatch",
    )?;
    need(
        hash(&p.expected.elf_path)? == p.expected.elf_sha256,
        "trusted local ELF SHA256 mismatch",
    )?;
    for f in &p.fixtures {
        checked_fixture(f)?;
    }
    let usb = UsbSerialManager::new();
    usb.set_desktop_device_id("pet-maintenance-hil");
    let (tx, rx) = mpsc::sync_channel(QUEUE);
    let drops = Arc::new(AtomicU32::new(0));
    let dropped = drops.clone();
    usb.connect(
        &std::env::var("P4_SERIAL_PORT").map_err(|_| "explicit P4_SERIAL_PORT required")?,
        move |topic, v| {
            if topic == "protocol/ack" || topic == "miniapp/state" {
                if tx.try_send((topic, v)).is_err() {
                    let _ = dropped.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| {
                        Some(n.saturating_add(1))
                    });
                }
            } else {
                events::forward(&tx, &dropped, topic, v);
            }
        },
    )?;
    let _disconnect = Disconnect(usb.clone());
    let mut authority = Authority::default();
    let mut failed = false;
    let mut completed = 0;
    let mut selection_preserved = false;
    let execution = (|| -> Result<()> {
        let initial = identity_check(&usb, &rx, &p.expected)?;
        write_new(
            &out.join("identity.before.json"),
            &serde_json::to_value(initial).map_err(|e| e.to_string())?,
        )?;
        let baseline = usb.query_widget_inventory(&p.expected.board_id)?;
        let ids = inventory(&baseline, &p.expected.board_id)?;
        let baseline_selection = selection(&baseline, &p.expected.board_id)?;
        need(
            !p.steps.iter().any(Step::mutation)
                || baseline_selection.selected_widget_id.is_some()
                || ids.is_empty(),
            "cannot restore invalid nonempty baseline selection using existing-id API",
        )?;
        need(
            ids.len() + p.fixtures.len() <= 32,
            "insufficient catalog slots; never remove user data",
        )?;
        need(
            p.fixtures.iter().all(|f| !ids.contains(&f.id)),
            "fixture ID already present; never overwrite/adopt/delete it",
        )?;
        write_new(&out.join("inventory.baseline.json"), &baseline)?;
        for (n, s) in p.steps.iter().enumerate() {
            if s.mutation() {
                selection_preserved = false;
            }
            write_new(
                &out.join(format!("{n:03}.intent.json")),
                &json!({"step":s,"mutation":s.mutation(),"unknownOutcome":s.mutation(),"retries":0}),
            )?;
            let response = (|| -> Result<Value> {
                authority.admit(s)?;
                if s.mutation() {
                    need(
                        drops.load(Ordering::Relaxed) == 0,
                        "event queue loss; no further mutations",
                    )?;
                    identity_check(&usb, &rx, &p.expected)?;
                }
                let board = &p.expected.board_id;
                match s {
                    Step::Diagnostics => {
                        let v = usb.query_diagnostics(board)?;
                        need(
                            v["boardDeviceId"] == *board
                                && v["runtime"]["firmware"] == p.expected.firmware
                                && v["requestId"].as_str().is_some_and(|s| !s.is_empty()),
                            "diagnostics identity/request mismatch",
                        )?;
                        Ok(v)
                    }
                    Step::Inventory => {
                        let v = usb.query_widget_inventory(board)?;
                        inventory(&v, board)?;
                        Ok(v)
                    }
                    Step::Programs => serde_json::to_value(usb.query_background_programs(board)?)
                        .map_err(|e| e.to_string()),
                    Step::State => {
                        let mut io = ProductionPull {
                            usb: &usb,
                            board,
                            rx: &rx,
                            started: Instant::now(),
                        };
                        state_query(
                            &mut io,
                            &p.expected,
                            &uuid::Uuid::new_v4().simple().to_string(),
                        )
                    }
                    Step::Install { fixture } => {
                        let f = p
                            .fixtures
                            .iter()
                            .find(|f| &f.id == fixture)
                            .ok_or("fixture missing")?;
                        let root = checked_fixture(f)?;
                        let before = usb.query_widget_inventory(board)?;
                        need(
                            !inventory(&before, board)?.contains(fixture),
                            "ID appeared before install",
                        )?;
                        let (installed, attempts) =
                            crate::usb_serial::installer_acceptance::single_attempt(|| {
                                usb.install_widget_clawpkg_on_board(
                                    board,
                                    fixture,
                                    &root,
                                    &std::collections::HashMap::new(),
                                    |_, _, _| {},
                                )
                            });
                        let single = attempts.single_attempts_only();
                        write_new(
                            &out.join(format!("{n:03}.installer-attempts.json")),
                            &serde_json::to_value(&attempts).map_err(|e| e.to_string())?,
                        )?;
                        need(
                            single,
                            "installer replay detected; mutation outcome unknown",
                        )?;
                        installed?;
                        let after = usb.query_widget_inventory(board)?;
                        need(
                            inventory(&after, board)?.contains(fixture),
                            "install ACK lacks inventory confirmation",
                        )?;
                        authority.owned.insert(fixture.clone());
                        Ok(json!({"installed":fixture,"commitAckMatched":true,"inventory":after}))
                    }
                    Step::Activate { fixture, show } => {
                        let v = usb.activate_installed_widget(board, fixture, *show)?;
                        need(
                            v["ok"] == true
                                && v["requestTopic"] == "widget/activate"
                                && v["requestId"].as_str().is_some_and(|s| !s.is_empty()),
                            "activation lacks matched positive ACK",
                        )?;
                        Ok(v)
                    }
                    Step::Action { fixture, action } => {
                        let v = usb.query_widget_inventory(board)?;
                        inventory(&v, board)?;
                        need(
                            v["activeWidgetId"] == *fixture,
                            "action target is not active fixture",
                        )?;
                        request_ack(&usb, &rx, board, "miniapp/event", json!({"action":action}))
                    }
                    Step::Hide => {
                        let v = usb.query_widget_inventory(board)?;
                        inventory(&v, board)?;
                        need(
                            v["activeWidgetId"]
                                .as_str()
                                .is_some_and(|id| authority.owned.contains(id)),
                            "never hide an unrelated user widget",
                        )?;
                        request_ack(
                            &usb,
                            &rx,
                            board,
                            "control/screen-page",
                            json!({"page":"main"}),
                        )
                    }
                    Step::RestoreSelection => {
                        let before = usb.query_widget_inventory(board)?;
                        let active_before = before
                            .get("activeWidgetId")
                            .ok_or("active baseline field absent")?
                            .clone();
                        let target = selection_target(&baseline_selection, &before, board)?;
                        let ack = if let Some(id) = target {
                            Some(request_ack(
                                &usb,
                                &rx,
                                board,
                                "widget/select_by_id",
                                json!({"widgetId":id}),
                            )?)
                        } else {
                            None
                        };
                        let after = usb.query_widget_inventory(board)?;
                        selection_readback(&baseline_selection, &active_before, &after, board)?;
                        selection_preserved = true;
                        Ok(
                            json!({"baselineSelection":baseline_selection,"ack":ack,"inventory":after,"selectionMutationSent":ack.is_some(),"activeUnchangedBySelection":true}),
                        )
                    }
                    Step::Start { fixture } => {
                        let initial = usb.query_background_programs(board)?;
                        need(
                            initial.items.iter().all(|e| !e.live),
                            "existing live PROGRAM owner; do not take over",
                        )?;
                        let v = usb.start_background_program(board, fixture)?;
                        need(v.confirmed, "PROGRAM result unknown; no BUSY inference")?;
                        let state = v.snapshot.as_ref().ok_or("missing start snapshot")?;
                        let owner = state
                            .items
                            .iter()
                            .find(|e| e.widget_id == *fixture && e.live)
                            .ok_or("missing live owner")?;
                        need(state.boot_id == p.expected.boot_id, "start boot mismatch")?;
                        authority.running.insert(
                            fixture.clone(),
                            (state.boot_id.clone(), owner.registration.clone()),
                        );
                        serde_json::to_value(v).map_err(|e| e.to_string())
                    }
                    Step::Stop { fixture } => {
                        let (boot, registration) = authority
                            .running
                            .get(fixture)
                            .ok_or("not this plan registration")?
                            .clone();
                        let v =
                            usb.stop_background_program(board, fixture, &boot, &registration)?;
                        need(v.confirmed, "stop result unknown")?;
                        authority.running.remove(fixture);
                        serde_json::to_value(v).map_err(|e| e.to_string())
                    }
                    Step::Remove { fixture } => {
                        need(
                            !authority.running.contains_key(fixture),
                            "owned PROGRAM still live",
                        )?;
                        usb.remove_widget(board, fixture)?;
                        let v = usb.query_widget_inventory(board)?;
                        need(
                            !inventory(&v, board)?.contains(fixture),
                            "delete ACK lacks absence confirmation",
                        )?;
                        authority.owned.remove(fixture);
                        Ok(json!({"removed":fixture,"deleteAckMatched":true,"inventory":v}))
                    }
                    Step::Screenshot { label } => screenshot(&usb, &rx, p, out, label),
                    Step::Delay { ms } => {
                        std::thread::sleep(Duration::from_millis(*ms));
                        Ok(json!({"waitedMs":ms}))
                    }
                }
            })();
            let receipt = match response {
                Ok(v) => {
                    json!({"step":s,"status":"confirmed","result":v,"unknownOutcome":false,"retries":0})
                }
                Err(e) => {
                    failed = true;
                    if s.mutation() {
                        authority.locked = true;
                    }
                    json!({"step":s,"status":"failed_or_blocked","error":e,"unknownOutcome":s.mutation(),"mutationsLocked":authority.locked,"retries":0})
                }
            };
            write_new(&out.join(format!("{n:03}.receipt.json")), &receipt)?;
            completed += 1;
        }
        let final_inventory = usb.query_widget_inventory(&p.expected.board_id)?;
        write_new(&out.join("inventory.final.json"), &final_inventory)?;
        need(
            selection(&final_inventory, &p.expected.board_id)? == baseline_selection,
            "final inventory selection differs from baseline",
        )?;
        if !p.steps.iter().any(Step::mutation) {
            selection_preserved = true;
        }
        need(selection_preserved,"explicit baseline restore was not confirmed; read-only inventory cannot clear unknown mutation")?;
        identity_check(&usb, &rx, &p.expected)?;
        Ok(())
    })();
    if execution.is_err() {
        authority.locked = true;
    }
    // No Drop cleanup mutations. A timeout never causes a delete, stop or replay.
    usb.disconnect();
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut closed = false;
    let mut tail = 0;
    let mut late_error = false;
    while Instant::now() < deadline && tail < 512 {
        match rx.recv_timeout(Duration::from_millis(20)) {
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                closed = true;
                break;
            }
            Ok((topic, _)) => {
                tail += 1;
                if topic == "debug/screenshot_error" {
                    late_error = true;
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
        }
    }
    Ok(
        json!({"schemaVersion":1,"planId":p.plan_id,"sourceDigest":p.source_digest,"readerSha256":p.reader_sha256,
        "boardId":p.expected.board_id,"bootId":p.expected.boot_id,"elfSha256":p.expected.elf_sha256,"firmware":p.expected.firmware,"buildId":p.expected.build_id,
        "passed":execution.is_ok()&&!failed&&!late_error&&closed&&drops.load(Ordering::Relaxed)==0,"fatalError":execution.err(),"completedSteps":completed,
        "mutationsLocked":authority.locked,"unknownOutcome":authority.locked,"remainingOwnedFixtures":authority.owned,"remainingOwnedPrograms":authority.running,
        "readerClosed":closed,"lateScreenshotError":late_error,"callbackDrops":drops.load(Ordering::Relaxed),"retries":0,"retryScope":"no plan operation or installer chunk replay; partial writes are transport continuation","installerChunkMaxAttempts":1,"reader":"production-rust-bufreader","deviceExecution":true,
        "selectionPreserved":selection_preserved,"selectionRestoration":"explicit restoreSelection ACK and inventory readback; no automatic cleanup","featureAcceptance":"requires explicit state/pixel assertions; ACK is admission evidence only","audioNotRequested":true,"cloudCalls":0}),
    )
}
#[test]
#[ignore = "Explicit opt-in USB fixture mutations; requires reviewed bounded JSON plan and fresh evidence directory"]
fn local_capability_production_plan() {
    assert_eq!(std::env::var("PET_CAPABILITY_ACCEPT").as_deref(), Ok("1"));
    assert_ne!(std::env::var("P4_SERIAL_LOG_RAW").as_deref(), Ok("1"));
    let path = PathBuf::from(std::env::var("PET_CAPABILITY_PLAN").expect("JSON plan path"));
    assert!(fs::metadata(&path).unwrap().len() <= MAX_PLAN);
    let mut bytes = Vec::new();
    fs::File::open(&path)
        .unwrap()
        .take(MAX_PLAN + 1)
        .read_to_end(&mut bytes)
        .unwrap();
    let plan = parse(&bytes).expect("invalid plan; USB not opened");
    let out = PathBuf::from(
        std::env::var("PET_CAPABILITY_OUTPUT").expect("new private evidence directory"),
    );
    assert!(out.is_absolute());
    fs::create_dir(&out).expect("fresh evidence directory required");
    write_new(
        &out.join("plan.json"),
        &serde_json::to_value(&plan).unwrap(),
    )
    .unwrap();
    let result = run_plan(&plan, &out)
        .unwrap_or_else(|e| json!({"passed":false,"error":e,"planId":plan.plan_id,"retries":0}));
    write_new(&out.join("receipt.json"), &result).unwrap();
    assert_eq!(
        result["passed"], true,
        "production capability plan failed; no mutations replayed; inspect receipt.json"
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    fn sample() -> Value {
        json!({"schemaVersion":1,"planId":"offline-unit","sourceDigest":"a".repeat(64),"readerSha256":"b".repeat(64),
        "expected":{"boardId":"board-test","bootId":"0123456789abcdef","firmware":"0.7.103-p4-r150","buildId":"0.7.103-p4-r150+123456789abc","elfSha256":"c".repeat(64),"elfPath":"/private/tmp/fixture.elf"},
        "fixtures":[{"id":"hil-fixture","path":"/private/tmp/hil-fixture","files":{"component.json":"d".repeat(64),"runtime/widget.json":"d".repeat(64),"buttons.json":"d".repeat(64),"negative-screen.json":"d".repeat(64),"share.json":"d".repeat(64)}}],
        "steps":[{"op":"install","fixture":"hil-fixture"},{"op":"activate","fixture":"hil-fixture","show":true},{"op":"action","fixture":"hil-fixture","action":"confirm"},{"op":"screenshot","label":"after"},{"op":"remove","fixture":"hil-fixture"},{"op":"restoreSelection"}]})
    }
    fn valid(v: Value) -> bool {
        parse(&serde_json::to_vec(&v).unwrap()).is_ok()
    }
    #[test]
    fn valid_plan_and_query_only_plan_do_not_touch_usb_or_files() {
        assert!(valid(sample()));
        let mut v = sample();
        v["fixtures"] = json!([]);
        v["steps"] =
            json!([{"op":"diagnostics"},{"op":"inventory"},{"op":"programs"},{"op":"state"}]);
        assert!(valid(v));
    }
    #[test]
    fn unknown_fields_topics_and_unowned_operations_are_rejected() {
        for pointer in [
            "/extra",
            "/expected/extra",
            "/fixtures/0/extra",
            "/steps/0/extra",
        ] {
            let mut v = sample();
            let (parent, key) = pointer.rsplit_once('/').unwrap();
            v.pointer_mut(parent)
                .unwrap()
                .as_object_mut()
                .unwrap()
                .insert(key.into(), json!(true));
            assert!(!valid(v));
        }
        for op in ["diagnostics", "inventory", "programs", "state", "hide"] {
            let mut v = sample();
            v["steps"] = json!([{"op":op,"extra":true}]);
            assert!(!valid(v));
        }
        for op in ["send", "reboot", "reset", "audio", "shell"] {
            let mut v = sample();
            v["steps"] = json!([{"op":op}]);
            assert!(!valid(v));
        }
        for op in ["activate", "remove", "start", "stop", "action"] {
            let mut v = sample();
            v["steps"].as_array_mut().unwrap().remove(0);
            v["steps"][0] = json!({"op":op,"fixture":"hil-fixture"});
            assert!(!valid(v));
        }
    }
    #[test]
    fn unsafe_paths_hashes_duplicates_and_budgets_fail_closed() {
        for key in [
            "../escape",
            "runtime/../escape",
            "/absolute",
            "runtime//file",
            "runtime/./file",
            "runtime\\file",
        ] {
            let mut v = sample();
            v["fixtures"][0]["files"][key] = json!("d".repeat(64));
            assert!(!valid(v));
        }
        for pointer in [
            "/sourceDigest",
            "/readerSha256",
            "/expected/elfSha256",
            "/fixtures/0/files/component.json",
        ] {
            let mut v = sample();
            *v.pointer_mut(pointer).unwrap() = json!("z".repeat(64));
            assert!(!valid(v));
        }
        let mut v = sample();
        v["steps"] = json!(vec![json!({"op":"diagnostics"}); 129]);
        assert!(!valid(v));
        let mut v = sample();
        v["steps"] = json!([{"op":"delay","ms":30001}]);
        assert!(!valid(v));
        let mut v = sample();
        v["steps"] = json!(vec![json!({"op":"delay","ms":30000}); 5]);
        assert!(!valid(v));
        let mut v = sample();
        v["steps"] = json!([{"op":"screenshot","label":"same"},{"op":"screenshot","label":"same"}]);
        assert!(!valid(v));
        let mut v = sample();
        let duplicate = v["fixtures"][0].clone();
        v["fixtures"].as_array_mut().unwrap().push(duplicate);
        assert!(!valid(v));
    }
    #[test]
    fn ownership_never_survives_unknown_mutation_for_further_writes() {
        let mut a = Authority::default();
        a.owned.insert("hil-fixture".into());
        a.locked = true;
        for s in [
            Step::Remove {
                fixture: "hil-fixture".into(),
            },
            Step::Install {
                fixture: "other".into(),
            },
            Step::Hide,
            Step::RestoreSelection,
            Step::Action {
                fixture: "hil-fixture".into(),
                action: "confirm".into(),
            },
        ] {
            assert!(a.admit(&s).is_err());
        }
        for s in [
            Step::Diagnostics,
            Step::Inventory,
            Step::Programs,
            Step::State,
            Step::Screenshot {
                label: "read-only".into(),
            },
        ] {
            assert!(a.admit(&s).is_ok());
        }
    }
    #[test]
    fn stop_delete_and_reinstall_cannot_bypass_declared_ownership() {
        for steps in [
            json!([{"op":"install","fixture":"hil-fixture"},{"op":"start","fixture":"hil-fixture"},{"op":"remove","fixture":"hil-fixture"}]),
            json!([{"op":"install","fixture":"hil-fixture"},{"op":"remove","fixture":"hil-fixture"},{"op":"install","fixture":"hil-fixture"}]),
            json!([{"op":"stop","fixture":"hil-fixture"}]),
        ] {
            let mut v = sample();
            v["steps"] = steps;
            assert!(!valid(v));
        }
    }
    #[test]
    fn ack_requires_exact_request_topic_id_and_positive_boolean() {
        let v = json!({"requestId":"one","requestTopic":"miniapp/event","ok":true});
        assert!(ack_matches("protocol/ack", &v, "board", "miniapp/event", "one").unwrap());
        assert!(!ack_matches("protocol/ack", &v, "board", "miniapp/event", "other").unwrap());
        assert!(!ack_matches("miniapp/state", &v, "board", "miniapp/event", "one").unwrap());
        assert!(ack_matches("protocol/ack", &v, "board", "control/screen-page", "one").is_err());
        let mut bad = v.clone();
        bad["ok"] = json!(1);
        assert!(ack_matches("protocol/ack", &bad, "board", "miniapp/event", "one").is_err());
        let mut bad = v.clone();
        bad["boardDeviceId"] = json!("other");
        assert!(ack_matches("protocol/ack", &bad, "board", "miniapp/event", "one").is_err());
        // A matched NACK is retained as a rejection, never positive execution proof.
        let mut nack = v;
        nack["ok"] = json!(false);
        assert!(ack_matches("protocol/ack", &nack, "board", "miniapp/event", "one").unwrap());
        assert_ne!(nack["ok"], true);
    }
    #[test]
    fn screenshot_wrapper_never_resubmits_identical_requests_even_after_send_failure() {
        use super::pull::Transport;
        struct Fake {
            calls: u32,
        }
        impl Transport for Fake {
            fn elapsed(&self) -> Duration {
                Duration::ZERO
            }
            fn send(&mut self, _: &str, _: &Value) -> std::result::Result<(), ()> {
                self.calls += 1;
                Err(())
            }
            fn receive(&mut self, _: Duration) -> std::result::Result<Option<(String, Value)>, ()> {
                Ok(None)
            }
            fn delay(&mut self, _: Duration) {}
        }
        let mut io = NoReplay {
            inner: Fake { calls: 0 },
            sent: BTreeSet::new(),
            blocked: 0,
        };
        let p = json!({"requestId":"one"});
        assert!(io.send("debug/screenshot_open", &p).is_err());
        assert!(io.send("debug/screenshot_open", &p).is_err());
        assert_eq!(io.inner.calls, 1);
        assert_eq!(io.blocked, 1);
    }
    #[test]
    fn production_installer_single_attempt_guard_is_scoped_and_records_unknown_without_replay() {
        use crate::usb_serial::installer_acceptance as hook;
        assert_eq!(hook::limit(3), 3);
        let (failed, receipt) = hook::single_attempt(|| {
            assert_eq!(hook::limit(3), 1);
            let mut sends = 0;
            for _ in 1..=hook::limit(3) {
                sends += 1;
                hook::attempt("transfer", "runtime/widget.json", 0);
                hook::outcome(
                    "transfer",
                    "runtime/widget.json",
                    0,
                    "ack_unknown_no_replay",
                );
            }
            assert_eq!(sends, 1);
            assert_eq!(std::thread::spawn(|| hook::limit(3)).join().unwrap(), 3);
            Err::<(), _>("unknown ACK")
        });
        assert!(failed.is_err());
        assert!(receipt.single_attempts_only());
        assert_eq!(
            receipt.chunks.values().next().unwrap().outcome,
            "ack_unknown_no_replay"
        );
        assert_eq!(hook::limit(3), 3);
        let panicked = std::panic::catch_unwind(|| hook::single_attempt(|| panic!("probe unwind")));
        assert!(panicked.is_err());
        assert_eq!(hook::limit(3), 3);
    }
    #[test]
    fn installer_receipt_rejects_duplicate_chunk_submissions() {
        use crate::usb_serial::installer_acceptance as hook;
        let (_, receipt) = hook::single_attempt(|| {
            hook::attempt("transfer", "file", 0);
            hook::attempt("transfer", "file", 0);
        });
        assert!(!receipt.single_attempts_only());
    }
    #[test]
    fn real_installer_unknown_chunk_ack_sends_once_and_never_aborts_or_commits() {
        let (result, receipt, topics) =
            crate::usb_serial::installer_acceptance::missing_chunk_ack_probe();
        let error = result.unwrap_err();
        assert!(error.contains("未确认") && error.contains("不会自动重发"), "{error}");
        assert_eq!(topics, vec!["widget/begin", "widget/chunk"]);
        assert!(receipt.single_attempts_only());
        assert_eq!(receipt.chunks.len(), 1);
        assert_eq!(
            receipt.chunks.values().next().unwrap().outcome,
            "ack_unknown_no_replay"
        );
    }
    #[test]
    fn correlated_state_requires_boot_generation_and_matches_ack_in_either_order() {
        use std::collections::VecDeque;
        struct Wire {
            now: Duration,
            events: VecDeque<(String, Value)>,
            sends: u32,
        }
        impl pull::Transport for Wire {
            fn elapsed(&self) -> Duration {
                self.now
            }
            fn send(&mut self, topic: &str, p: &Value) -> std::result::Result<(), ()> {
                assert_eq!(topic, "miniapp/query");
                assert_eq!(p["requestId"], "state-one");
                self.sends += 1;
                Ok(())
            }
            fn receive(&mut self, t: Duration) -> std::result::Result<Option<(String, Value)>, ()> {
                self.now += t;
                Ok(self.events.pop_front())
            }
            fn delay(&mut self, _: Duration) {
                panic!("no state retry")
            }
        }
        let plan = parse(&serde_json::to_vec(&sample()).unwrap()).unwrap();
        let e = &plan.expected;
        let state = json!({"requestId":"state-one","bootId":e.boot_id,"observationAvailable":true,"instanceGeneration":"1234abcd","observationEngine":"p4-miniapp-observation-fnv1a32-v1","persistProjectionDigest":"8765abcd","durability":"not_observed","active":true,"widgetId":"hil-fixture"});
        let ack = json!({"requestId":"state-one","requestTopic":"miniapp/query","ok":true});
        for events in [
            vec![
                ("miniapp/state".into(), state.clone()),
                ("protocol/ack".into(), ack.clone()),
            ],
            vec![
                ("protocol/ack".into(), ack.clone()),
                ("miniapp/state".into(), state.clone()),
            ],
        ] {
            let mut io = Wire {
                now: Duration::ZERO,
                events: events.into(),
                sends: 0,
            };
            let result = state_query(&mut io, e, "state-one").unwrap();
            assert_eq!(result["stateRequestCorrelated"], true);
            assert_eq!(result["durabilityObserved"], false);
            assert_eq!(io.sends, 1);
        }
        for (key, value) in [
            ("requestId", json!("other")),
            ("bootId", json!("ffffffffffffffff")),
            ("instanceGeneration", json!("bad")),
            ("observationAvailable", json!(false)),
            ("durability", json!("durable")),
        ] {
            let mut bad = state.clone();
            bad[key] = value;
            assert!(checked_observation(&bad, e, "state-one").is_err());
        }
        assert!(checked_observation(
            &json!({"active":true,"widgetId":"hil-fixture"}),
            e,
            "state-one"
        )
        .is_err());
        let mut io = Wire {
            now: Duration::ZERO,
            events: VecDeque::from([("protocol/ack".into(), ack)]),
            sends: 0,
        };
        assert!(state_query(&mut io, e, "state-one").is_err());
        assert_eq!(io.sends, 1);
    }
    #[test]
    fn selection_restore_uses_independent_baseline_and_requires_exact_readback_without_activation()
    {
        let baseline = json!({"boardDeviceId":"board","ok":true,"complete":true,"recoveryBlocked":false,"activeWidgetId":"active-user","selectedWidgetId":"selected-user","selectedIndex":1,"items":[{"id":"active-user"},{"id":"selected-user"}]});
        let saved = selection(&baseline, "board").unwrap();
        assert_eq!(saved.selected_widget_id.as_deref(), Some("selected-user"));
        let mut before = baseline.clone();
        before["activeWidgetId"] = json!("test-fixture");
        before["selectedWidgetId"] = json!("active-user");
        before["selectedIndex"] = json!(0);
        assert_eq!(
            selection_target(&saved, &before, "board").unwrap(),
            Some("selected-user".into())
        );
        let mut after = before.clone();
        after["selectedWidgetId"] = json!("selected-user");
        after["selectedIndex"] = json!(1);
        assert!(selection_readback(&saved, &before["activeWidgetId"], &after, "board").is_ok());
        assert!(selection_readback(&saved, &before["activeWidgetId"], &before, "board").is_err());
        after["activeWidgetId"] = json!("selected-user");
        assert!(selection_readback(&saved, &before["activeWidgetId"], &after, "board").is_err());
        let mut legacy = baseline.clone();
        legacy.as_object_mut().unwrap().remove("selectedWidgetId");
        assert!(selection(&legacy, "board").is_err());
        let mut malformed = baseline;
        malformed["selectedIndex"] = json!(0);
        assert!(selection(&malformed, "board").is_err());
        let empty = json!({"boardDeviceId":"board","ok":true,"complete":true,"recoveryBlocked":false,"activeWidgetId":null,"selectedWidgetId":null,"selectedIndex":null,"items":[]});
        let saved_empty = selection(&empty, "board").unwrap();
        assert_eq!(
            selection_target(&saved_empty, &empty, "board").unwrap(),
            None
        );
        assert!(selection_readback(&saved_empty, &Value::Null, &empty, "board").is_ok());
    }
    #[test]
    fn selection_restore_is_explicit_last_mutation_and_cannot_name_arbitrary_user_target() {
        let mut v = sample();
        v["steps"].as_array_mut().unwrap().pop();
        assert!(!valid(v));
        let mut v = sample();
        v["steps"]
            .as_array_mut()
            .unwrap()
            .push(json!({"op":"restoreSelection","widgetId":"unrelated-user"}));
        assert!(!valid(v));
        assert!(valid(sample()));
    }
}
