//! Owner-only local encryption-key storage. No OS credential access on normal
//! reads/writes. Before creating a new key, a caller checks that existing
//! ciphertext will not be stranded. No recovery backend or OS password UI.
use ring::rand::{SecureRandom, SystemRandom};
use std::{
    fs,
    io::{Read, Write},
    path::Path,
};
const KEY_FILE: &str = "config-key-v1";

fn private_dir(root: &Path) -> Result<std::path::PathBuf, String> {
    fs::create_dir_all(root).map_err(|_| "无法创建配置目录")?;
    let meta = fs::symlink_metadata(root).map_err(|_| "无法检查配置目录")?;
    if !meta.is_dir() || meta.file_type().is_symlink() {
        return Err("配置目录无效；原配置保留".into());
    }
    let dir = root.join("private");
    match fs::create_dir(&dir) {
        Ok(()) => (),
        Err(e) if e.kind() == std::io::ErrorKind::AlreadyExists => (),
        Err(_) => return Err("无法创建私有配置目录".into()),
    }
    let meta = fs::symlink_metadata(&dir).map_err(|_| "无法检查私有配置目录")?;
    if !meta.is_dir() || meta.file_type().is_symlink() {
        return Err("私有配置目录无效；原配置保留".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if meta.uid() != unsafe { libc::geteuid() } {
            return Err("私有配置目录不属于当前用户".into());
        }
        fs::set_permissions(&dir, fs::Permissions::from_mode(0o700))
            .map_err(|_| "无法保护私有配置目录")?;
    }
    Ok(dir)
}

fn read_key(path: &Path) -> Result<Option<[u8; 32]>, String> {
    let mut options = fs::OpenOptions::new();
    options.read(true);
    #[cfg(unix)]
    {
        use std::os::unix::fs::OpenOptionsExt;
        options.custom_flags(libc::O_NOFOLLOW | libc::O_NONBLOCK);
    }
    let file = match options.open(path) {
        Ok(file) => file,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
        Err(_) => return Err("本地配置密钥不可读取；原配置保留".into()),
    };
    let meta = file.metadata().map_err(|_| "无法检查本地配置密钥")?;
    if !meta.is_file() || meta.len() != 32 {
        return Err("本地配置密钥损坏；不会覆盖或重建".into());
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::{MetadataExt, PermissionsExt};
        if meta.uid() != unsafe { libc::geteuid() } || meta.nlink() != 1 {
            return Err("本地配置密钥归属或链接无效".into());
        }
        file.set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| "无法保护本地配置密钥")?;
    }
    let mut bytes = Vec::new();
    file.take(33)
        .read_to_end(&mut bytes)
        .map_err(|_| "无法读取本地配置密钥")?;
    bytes
        .try_into()
        .map(Some)
        .map_err(|_| "本地配置密钥大小无效；原配置保留".into())
}

pub(super) fn load_key(
    root: &Path,
    create: bool,
    preflight: impl FnOnce() -> Result<(), String>,
) -> Result<[u8; 32], String> {
    let dir = private_dir(root)?;
    let path = dir.join(KEY_FILE);
    if let Some(key) = read_key(&path)? {
        return Ok(key);
    }
    // Existing local keys are used directly. A lost key never causes automatic
    // replacement or an OS credential-store lookup.
    preflight()?;
    let key = if create {
        let mut key = [0u8; 32];
        SystemRandom::new()
            .fill(&mut key)
            .map_err(|_| "无法生成本地配置密钥")?;
        key
    } else {
        return Err("原配置密钥缺失；不会生成新密钥覆盖已有配置".into());
    };
    let mut temp = tempfile::NamedTempFile::new_in(&dir).map_err(|_| "无法保存本地配置密钥")?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        temp.as_file()
            .set_permissions(fs::Permissions::from_mode(0o600))
            .map_err(|_| "无法保护本地配置密钥")?;
    }
    temp.write_all(&key)
        .and_then(|_| temp.as_file().sync_all())
        .map_err(|_| "无法写入本地配置密钥")?;
    match temp.persist_noclobber(&path) {
        Ok(_) => Ok(key),
        Err(e) if e.error.kind() == std::io::ErrorKind::AlreadyExists => {
            let winner = read_key(&path)?.ok_or("本地配置密钥发生变化；原配置保留")?;
            Ok(winner)
        }
        Err(_) => Err("无法提交本地配置密钥；原配置保留".into()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn new_keys_are_stable_and_never_call_legacy_storage() {
        let dir = tempfile::tempdir().unwrap();
        let key = load_key(dir.path(), true, || Ok(())).unwrap();
        assert_eq!(
            load_key(dir.path(), false, || panic!(
                "local storage must not access Keychain"
            ))
            .unwrap(),
            key
        );
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(
                fs::metadata(dir.path().join("private"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o700
            );
            assert_eq!(
                fs::metadata(dir.path().join("private/config-key-v1"))
                    .unwrap()
                    .permissions()
                    .mode()
                    & 0o777,
                0o600
            );
        }
    }
    #[test]
    fn missing_or_corrupt_old_keys_never_generate_replacements() {
        let dir = tempfile::tempdir().unwrap();
        assert!(load_key(dir.path(), true, || Err("legacy unavailable".into())).is_err());
        assert!(!dir.path().join("private/config-key-v1").exists());
        assert!(load_key(dir.path(), true, || Err("bad ciphertext".into())).is_err());
        assert!(!dir.path().join("private/config-key-v1").exists());
        fs::write(dir.path().join("private/config-key-v1"), [1; 31]).unwrap();
        assert!(load_key(dir.path(), true, || panic!()).is_err());
        assert_eq!(
            fs::read(dir.path().join("private/config-key-v1")).unwrap(),
            [1; 31]
        );
    }
    #[test]
    fn concurrent_first_saves_use_one_key_without_overwrite() {
        let dir = tempfile::tempdir().unwrap();
        let root = dir.path().to_owned();
        let jobs = (0..8)
            .map(|_| {
                let root = root.clone();
                std::thread::spawn(move || load_key(&root, true, || Ok(())).unwrap())
            })
            .collect::<Vec<_>>();
        let keys = jobs
            .into_iter()
            .map(|j| j.join().unwrap())
            .collect::<Vec<_>>();
        assert!(keys.iter().all(|k| *k == keys[0]));
    }
    #[cfg(unix)]
    #[test]
    fn symlinked_keys_and_directories_are_rejected() {
        use std::os::unix::fs::symlink;
        let dir = tempfile::tempdir().unwrap();
        let outside = tempfile::tempdir().unwrap();
        symlink(outside.path(), dir.path().join("private")).unwrap();
        assert!(load_key(dir.path(), true, || panic!()).is_err());
        assert!(!outside.path().join(KEY_FILE).exists());
        let fresh = tempfile::tempdir().unwrap();
        fs::create_dir(fresh.path().join("private")).unwrap();
        let target = outside.path().join("key");
        fs::write(&target, [42; 32]).unwrap();
        symlink(&target, fresh.path().join("private/config-key-v1")).unwrap();
        assert!(load_key(fresh.path(), true, || panic!()).is_err());
        assert_eq!(fs::read(target).unwrap(), [42; 32]);
    }
}
