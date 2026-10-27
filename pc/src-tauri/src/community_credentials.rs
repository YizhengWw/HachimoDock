//! Separate, non-exportable-in-UI community identity. Never part of a component package.
use rand::RngCore;
use std::{
    fs,
    io::{Read, Write},
    path::Path,
    sync::Mutex,
};
static LOCK: Mutex<()> = Mutex::new(());

fn new_secret() -> String {
    let mut bytes = [0u8; 32];
    rand::thread_rng().fill_bytes(&mut bytes);
    bytes.iter().map(|x| format!("{x:02x}")).collect()
}
fn valid(value: &str) -> bool {
    value.len() == 64 && value.bytes().all(|b| b.is_ascii_hexdigit())
}

#[cfg(windows)]
pub fn load_or_create(_: &Path) -> Result<String, String> {
    let _lock = LOCK.lock().map_err(|_| "社区凭据正在使用")?;
    let entry = keyring::Entry::new("com.petmanager.desktop.community", "installation-v1")
        .map_err(|_| "无法打开系统凭据库")?;
    match entry.get_password() {
        Ok(value) if valid(&value) => Ok(value),
        Ok(_) => Err("社区凭据格式损坏；请联系开发者，不要覆盖或导出凭据".into()),
        Err(keyring::Error::NoEntry) => {
            let value = new_secret();
            entry.set_password(&value).map_err(|_| "无法保存社区凭据")?;
            Ok(value)
        }
        Err(_) => Err("系统社区凭据不可读取，请稍后重试".into()),
    }
}

#[cfg(not(windows))]
pub fn load_or_create(root: &Path) -> Result<String, String> {
    let _lock = LOCK.lock().map_err(|_| "社区凭据正在使用")?;
    let directory = root.join("private");
    fs::create_dir_all(root).map_err(|_| "无法创建社区存储")?;
    match fs::create_dir(&directory) {
        Ok(()) => (),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => (),
        Err(_) => return Err("无法创建社区凭据目录".into()),
    }
    let meta = fs::symlink_metadata(&directory).map_err(|_| "无法检查社区凭据目录")?;
    if !meta.is_dir() || meta.file_type().is_symlink() {
        return Err("社区凭据目录无效".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if meta.uid() != unsafe { libc::geteuid() } {
            return Err("社区凭据目录不属于当前用户".into());
        }
        fs::set_permissions(&directory, fs::Permissions::from_mode(0o700))
            .map_err(|_| "无法保护社区凭据目录")?;
    }
    let path = directory.join("community-identity-v1");
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    match options.open(&path) {
        Ok(mut file) => {
            let meta = file.metadata().map_err(|_| "社区凭据不可读取")?;
            if !meta.is_file() || meta.len() != 64 {
                return Err("社区凭据损坏，请联系开发者处理".into());
            }
            #[cfg(unix)]
            {
                use std::os::unix::fs::{MetadataExt, PermissionsExt};
                if meta.uid() != unsafe { libc::geteuid() } {
                    return Err("社区凭据不属于当前用户".into());
                }
                file.set_permissions(fs::Permissions::from_mode(0o600))
                    .map_err(|_| "无法保护社区凭据")?;
            }
            let mut value = String::new();
            file.read_to_string(&mut value)
                .map_err(|_| "无法读取社区凭据")?;
            if !valid(&value) {
                return Err("社区凭据损坏，请联系开发者处理".into());
            }
            Ok(value)
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            let value = new_secret();
            let mut file =
                tempfile::NamedTempFile::new_in(&directory).map_err(|_| "无法保存社区凭据")?;
            #[cfg(unix)]
            {
                use std::os::unix::fs::PermissionsExt;
                file.as_file()
                    .set_permissions(fs::Permissions::from_mode(0o600))
                    .map_err(|_| "无法保护社区凭据")?;
            }
            file.write_all(value.as_bytes())
                .and_then(|_| file.as_file().sync_all())
                .map_err(|_| "无法保存社区凭据")?;
            file.persist_noclobber(path)
                .map_err(|_| "社区凭据已变化，请重试")?;
            Ok(value)
        }
        Err(_) => Err("社区凭据不可读取；请勿覆盖，联系开发者处理".into()),
    }
}

#[cfg(all(test, unix))]
mod tests {
    use super::*;
    #[test]
    fn stable_private_identity() {
        use std::os::unix::fs::PermissionsExt;
        let tmp = tempfile::tempdir().unwrap();
        let a = load_or_create(tmp.path()).unwrap();
        assert_eq!(a, load_or_create(tmp.path()).unwrap());
        assert_eq!(
            fs::metadata(tmp.path().join("private/community-identity-v1"))
                .unwrap()
                .permissions()
                .mode()
                & 0o777,
            0o600
        );
    }
    #[test]
    fn refuses_symlink() {
        use std::os::unix::fs::symlink;
        let tmp = tempfile::tempdir().unwrap();
        let other = tempfile::tempdir().unwrap();
        symlink(other.path(), tmp.path().join("private")).unwrap();
        assert!(load_or_create(tmp.path()).is_err());
    }
}
