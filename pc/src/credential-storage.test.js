import test from "node:test";
import assert from "node:assert/strict";
import fs from "node:fs";
const read = file => fs.readFileSync(new URL(file, import.meta.url), "utf8");
test("there is no unlock UI, interactive IPC or automatic Keychain write on Mac", () => {
  for (const file of ["./App.jsx", "./ApiSettings.jsx", "../src-tauri/src/lib.rs", "../src-tauri/permissions/local-app-commands.json"]) {
    assert.doesNotMatch(read(file), /CredentialUnlockNotice|unlock_secure_config|解锁已保存的配置/);
  }
  assert.equal(fs.existsSync(new URL('../src-tauri/src/secure_config_macos.rs',import.meta.url)),false);
  const cargo=read('../src-tauri/Cargo.toml');
  const mac=cargo.split(`[target.'cfg(target_os = "macos")'.dependencies]`)[1].split('[dev-dependencies]')[0];
  assert.doesNotMatch(mac,/keyring|apple-native/);
  const local=read("../src-tauri/src/secure_config_local.rs");
  assert.doesNotMatch(local,/keyring::|SecKeychain/);
  assert.match(local,/0o700/);assert.match(local,/0o600/);
  assert.match(local,/persist_noclobber/);assert.match(local,/O_NOFOLLOW/);
  const storage=read("../src-tauri/src/secure_config.rs");
  assert.doesNotMatch(storage,/read_legacy_key|config_key_with_legacy|mod macos/);
  assert.match(storage,/AES_256_GCM/);assert.match(storage,/pet-manager-aes256gcm-v1/);
});
