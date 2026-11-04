//! Owner-only local capability bootstrap shared by approval and voice callers.
//! Credentials are never obtained from HTTP, URLs, redirects, or diagnostics.
use serde_json::Value;
use std::io::Read;
use std::path::PathBuf;
use std::time::Duration;

#[derive(Clone)]
pub(crate) struct Capability {
    pub base_url: String,
    pub epoch: String,
    pub token: String,
}

pub(crate) fn load(role: &str) -> Result<Capability, String> {
    let file = std::env::var_os("HACHIMODOCK_DSH_CAPABILITY_FILE")
        .map(PathBuf::from)
        .or_else(|| {
            std::env::var_os(if cfg!(windows) { "USERPROFILE" } else { "HOME" })
                .map(|h| PathBuf::from(h).join(".claw-pet/dsh-bridge/capability.json"))
        })
        .ok_or("DSH capability unavailable")?;
    let dir = file.parent().ok_or("DSH capability unavailable")?;
    let dm = std::fs::symlink_metadata(dir).map_err(|_| "DSH unpaired")?;
    if !dm.is_dir() || dm.file_type().is_symlink() {
        return Err("unsafe DSH capability directory".into());
    }
    let mut options = std::fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
        if dm.uid() != unsafe { libc::geteuid() } || dm.mode() & 0o077 != 0 {
            return Err("unsafe DSH capability permissions".into());
        }
        options.custom_flags(libc::O_NOFOLLOW);
    }
    #[cfg(windows)]
    {
        use std::os::windows::fs::{MetadataExt, OpenOptionsExt};
        if dm.file_attributes() & 0x400 != 0 {
            return Err("unsafe DSH reparse directory".into());
        }
        crate::dsh_windows_acl::verify_file(&file)?;
        // Open the reparse entry itself rather than following it, and prohibit
        // sharing writes/deletes while the protected descriptor is being read.
        options.custom_flags(0x00200000).share_mode(1);
    }
    #[cfg(not(any(unix, windows)))]
    {
        return Err("DSH capability ACL verification unavailable on this platform".into());
    }
    #[cfg(any(unix, windows))]
    {
        let mut f = options.open(&file).map_err(|_| "DSH unpaired")?;
        let m = f.metadata().map_err(|_| "DSH unpaired")?;
        if !m.is_file() || m.len() > 4096 {
            return Err("unsafe DSH capability file".into());
        }
        #[cfg(unix)]
        {
            use std::os::unix::fs::MetadataExt;
            if m.uid() != unsafe { libc::geteuid() } || m.mode() & 0o077 != 0 {
                return Err("unsafe DSH capability file".into());
            }
        }
        #[cfg(windows)]
        {
            use std::os::windows::fs::MetadataExt;
            if m.file_attributes() & 0x400 != 0 {
                return Err("unsafe DSH reparse file".into());
            }
            crate::dsh_windows_acl::verify_file(&file)?;
        }
        let mut raw = String::new();
        f.by_ref()
            .take(4097)
            .read_to_string(&mut raw)
            .map_err(|_| "DSH unpaired")?;
        let v: Value = serde_json::from_str(&raw).map_err(|_| "invalid DSH capability")?;
        let base = v["baseUrl"].as_str().ok_or("invalid DSH endpoint")?;
        let u = reqwest::Url::parse(base).map_err(|_| "invalid DSH endpoint")?;
        if v["version"] != 2
            || u.scheme() != "http"
            || u.host_str() != Some("127.0.0.1")
            || u.port().is_none()
            || !u.username().is_empty()
            || u.password().is_some()
            || u.path() != "/"
            || u.query().is_some()
            || u.fragment().is_some()
        {
            return Err("invalid DSH endpoint".into());
        }
        let epoch = v["epoch"].as_str().ok_or("invalid DSH epoch")?;
        let token = v["tokens"][role]
            .as_str()
            .ok_or("invalid DSH capability role")?;
        if ![epoch, token]
            .iter()
            .all(|s| s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit()))
        {
            return Err("invalid DSH capability".into());
        }
        Ok(Capability {
            base_url: base.trim_end_matches('/').into(),
            epoch: epoch.into(),
            token: token.into(),
        })
    }
}

pub(crate) fn client(timeout: Duration) -> Result<reqwest::blocking::Client, String> {
    reqwest::blocking::Client::builder()
        .no_proxy()
        .redirect(reqwest::redirect::Policy::none())
        .timeout(timeout)
        .build()
        .map_err(|_| "DSH HTTP client unavailable".into())
}
pub(crate) fn post(cap: &Capability, path: &str, body: &Value) -> Result<Value, String> {
    if ![
        "/voice/status",
        "/pair/ticket",
        "/voice/begin",
        "/voice/draft",
        "/voice/submit",
        "/interrupt",
        "/approval/decision",
        "/relay/close",
    ]
    .contains(&path)
    {
        return Err("invalid DSH operation".into());
    }
    let response = client(Duration::from_secs(5))?
        .post(format!("{}{path}", cap.base_url))
        .bearer_auth(&cap.token)
        .json(body)
        .send()
        .map_err(|_| "DSH delivery unknown; no automatic resend")?;
    let status = response.status();
    let mut raw = String::new();
    response
        .take(65537)
        .read_to_string(&mut raw)
        .map_err(|_| "DSH receipt unavailable")?;
    if raw.len() > 65536 { return Err("DSH receipt too large".into()); }
    let result: Value = serde_json::from_str(&raw).map_err(|_| "invalid DSH receipt")?;
    if !status.is_success() || result["ok"] != true {
        // Return only protocol error codes; never response text/session/token.
        let code = match result["error"].as_str().unwrap_or("") {
            "composer-unavailable" => "composer-unavailable",
            "question-pending" => "question-pending",
            "exact-pair-target-required" => "exact-pair-target-required",
            "forbidden-origin" => "forbidden-origin",
            "delivery-unknown" => "delivery-unknown",
            "stale-lease" => "stale-lease",
            "unpaired" => "unpaired",
            "forbidden" => "forbidden",
            _ => "request-rejected",
        };
        return Err(format!("DSH {code} (HTTP {})", status.as_u16()));
    }
    Ok(result)
}
