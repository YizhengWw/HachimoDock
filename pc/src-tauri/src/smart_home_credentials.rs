//! macOS credentials use owner-only local storage, never an interactive Keychain read.
//! Windows keeps its non-interactive OS credential vault. Tokens never enter UI snapshots.
use serde_json::Value;
use std::path::Path;

#[cfg(not(windows))]
fn directory(root: &Path) -> Result<std::path::PathBuf, String> {
    use std::fs;
    let dir = root.join("private");
    fs::create_dir_all(root).map_err(|_| "无法创建账号凭据目录")?;
    if !dir.exists() {
        match fs::create_dir(&dir) {
            Ok(()) => (),
            Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => (),
            Err(_) => return Err("无法创建账号凭据目录".into()),
        }
    }
    let meta = fs::symlink_metadata(&dir).map_err(|_| "无法读取账号凭据目录")?;
    if !meta.is_dir() || meta.file_type().is_symlink() {
        return Err("账号凭据目录无效".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if meta.uid() != unsafe { libc::geteuid() } {
            return Err("账号凭据目录不属于当前用户".into());
        }
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700))
            .map_err(|_| "无法保护账号凭据目录")?;
    }
    Ok(dir)
}

fn validate(value: &Value) -> Result<(), String> {
    if !value.is_object()
        || ["access_token", "refresh_token"].iter().any(|key| {
            value[key]
                .as_str()
                .is_none_or(|v| v.is_empty() || v.len() > 8192)
        })
    {
        return Err("账号凭据无效，请重新连接米家账号".into());
    }
    Ok(())
}

#[cfg(not(windows))]
pub fn load(root: &Path) -> Result<Option<Value>, String> {
    use std::{fs, io::Read};
    let path = directory(root)?.join("smart-home-credentials.json");
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    let mut file = match options.open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("无法读取本地账号凭据，请重新连接米家账号".into()),
    };
    let meta = file.metadata().map_err(|_| "无法检查账号凭据")?;
    if !meta.is_file() || meta.len() > 32768 {
        return Err("账号凭据文件无效".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if meta.uid() != unsafe { libc::geteuid() } {
            return Err("账号凭据不属于当前用户".into());
        }
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| "无法保护账号凭据")?;
    }
    let mut bytes = Vec::new();
    (&mut file)
        .take(32769)
        .read_to_end(&mut bytes)
        .map_err(|_| "无法读取账号凭据")?;
    if bytes.len() > 32768 {
        return Err("账号凭据过大".into());
    }
    let value = serde_json::from_slice(&bytes).map_err(|_| "账号凭据损坏，请重新连接米家账号")?;
    validate(&value)?;
    Ok(Some(value))
}

#[cfg(not(windows))]
pub fn store(root: &Path, value: Option<&Value>) -> Result<(), String> {
    use std::{fs, io::Write};
    let dir = directory(root)?;
    let path = dir.join("smart-home-credentials.json");
    if let Some(value) = value {
        validate(value)?;
        let bytes = serde_json::to_vec(value).map_err(|_| "无法编码账号凭据")?;
        if bytes.len() > 32768 {
            return Err("账号凭据过大".into());
        }
        let mut file = tempfile::NamedTempFile::new_in(dir).map_err(|_| "无法保存账号凭据")?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            file.as_file()
                .set_permissions(fs::Permissions::from_mode(0o600))
                .map_err(|_| "无法保护账号凭据")?;
        }
        file.write_all(&bytes)
            .and_then(|_| file.as_file().sync_all())
            .map_err(|_| "无法保存账号凭据")?;
        file.persist(path).map_err(|_| "无法替换账号凭据")?;
    } else {
        match fs::remove_file(path) {
            Ok(()) => (),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => (),
            Err(_) => return Err("无法清除本地账号凭据".into()),
        }
    }
    Ok(())
}

#[cfg(windows)]
fn entry() -> Result<keyring::Entry, String> {
    keyring::Entry::new("com.petmanager.desktop.smart-home", "miloco-cn")
        .map_err(|_| "无法打开系统凭据存储".into())
}
#[cfg(windows)]
pub fn load(_: &Path) -> Result<Option<Value>, String> {
    match entry()?.get_password() {
        Ok(text) => {
            let value = serde_json::from_str(&text).map_err(|_| "账号凭据损坏，请重新连接")?;
            validate(&value)?;
            Ok(Some(value))
        }
        Err(keyring::Error::NoEntry) => Ok(None),
        Err(_) => Err("无法读取系统账号凭据".into()),
    }
}
#[cfg(windows)]
pub fn store(_: &Path, value: Option<&Value>) -> Result<(), String> {
    match value {
        Some(value) => {
            validate(value)?;
            entry()?
                .set_password(&value.to_string())
                .map_err(|_| "账号凭据保存失败".into())
        }
        None => match entry()?.delete_credential() {
            Ok(()) | Err(keyring::Error::NoEntry) => Ok(()),
            Err(_) => Err("无法清除账号凭据".into()),
        },
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    use serde_json::json;
    use std::os::unix::fs::{symlink, PermissionsExt};
    #[test]
    fn local_credentials_roundtrip_refresh_and_disconnect_are_private() {
        let dir = tempfile::tempdir().unwrap();
        assert!(load(dir.path()).unwrap().is_none());
        let mut v = json!({"access_token":"fixture-access","refresh_token":"fixture-refresh"});
        store(dir.path(), Some(&v)).unwrap();
        assert_eq!(load(dir.path()).unwrap(), Some(v.clone()));
        let p = dir.path().join("private/smart-home-credentials.json");
        assert_eq!(
            std::fs::metadata(&p).unwrap().permissions().mode() & 0o777,
            0o600
        );
        assert_eq!(
            std::fs::metadata(p.parent().unwrap())
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o700
        );
        v["access_token"] = json!("refreshed");
        store(dir.path(), Some(&v)).unwrap();
        assert_eq!(load(dir.path()).unwrap(), Some(v));
        store(dir.path(), None).unwrap();
        store(dir.path(), None).unwrap();
        assert!(load(dir.path()).unwrap().is_none());
    }
    #[test]
    fn rejects_invalid_large_or_linked_credentials() {
        let dir = tempfile::tempdir().unwrap();
        assert!(store(dir.path(), Some(&json!({}))).is_err());
        let p = directory(dir.path())
            .unwrap()
            .join("smart-home-credentials.json");
        std::fs::write(&p, vec![b'x'; 32769]).unwrap();
        assert!(load(dir.path()).is_err());
        std::fs::remove_file(&p).unwrap();
        let outside = tempfile::NamedTempFile::new().unwrap();
        symlink(outside.path(), &p).unwrap();
        assert!(load(dir.path()).is_err());
        let other = tempfile::tempdir().unwrap();
        symlink(dir.path().join("private"), other.path().join("private")).unwrap();
        assert!(load(other.path()).is_err());
    }
}
