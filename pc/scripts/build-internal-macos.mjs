/** Internal macOS build with explicit credentials and CA. No fixed proxy or OS trust changes. */
import { existsSync, mkdtempSync, readFileSync, rmSync, unlinkSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { spawnSync } from 'node:child_process';
import { cleanSkillBytecode } from './clean-skill-bytecode.mjs';

const cwd = new URL('..', import.meta.url);
function run(command, args, env = process.env) {
  const result = spawnSync(command, args, { cwd, env, stdio: 'inherit' });
  if (result.error || result.status !== 0) throw new Error(`Internal build step failed: ${command}`);
}
const temporary = mkdtempSync(join(tmpdir(), 'pet-internal-ca-build-'));
try {
  // Validate before compiling. Nothing from this source bundle enters web assets.
  run(process.execPath, ['scripts/prepare-internal-ca.mjs', join(temporary, 'approved.pem')]);
  run(process.execPath, ['scripts/prepare-internal-credentials.mjs', join(temporary, 'credentials.json')]);
  const publicEnv = { ...process.env };
  delete publicEnv.PET_MANAGER_INTERNAL_CA_FILE;
  delete publicEnv.PET_MANAGER_INTERNAL_CA_SHA256;
  delete publicEnv.PET_MANAGER_INTERNAL_CREDENTIALS_FILE;
  run(process.execPath, ['scripts/check-public-build-env.mjs'], publicEnv);
  run(process.execPath, ['scripts/prepare-desktop-resources.mjs', '--target', 'macos', '--internal-development-firmware'], publicEnv);
  run('./node_modules/.bin/vite', ['build', '--config', 'vite.internal.config.js']);
  run(process.execPath, ['scripts/check-release-csp.mjs'], publicEnv);
  // The internal frontend entry point receives only the Ark key; Rust adds speech key + CA.
  run('./node_modules/.bin/tauri', ['build', '--bundles', 'app,dmg', '--ci', '--features', 'internal-network',
    '--config', JSON.stringify({ build: { beforeBuildCommand: '' } })]);
  cleanSkillBytecode(join(fileURLToPath(cwd), 'src-tauri/target/release/bundle/macos/Pet Manager.app/Contents/Resources/skills/petui'));
  run(process.execPath, ['scripts/sign-macos-local-app.mjs'], publicEnv);
  run(process.execPath, ['scripts/bundle-macos-local-dmg.mjs'], { ...publicEnv, PET_MANAGER_DMG_FLAVOR: 'INTERNAL' });
  // Tauri first creates an unsuffixed intermediate DMG containing internal
  // credentials. Once the explicitly labelled, re-signed DMG is verified,
  // remove only that intermediate to prevent accidental public distribution.
  const config = JSON.parse(readFileSync(new URL('src-tauri/tauri.conf.json', cwd), 'utf8'));
  const arch = process.arch === 'arm64' ? 'aarch64' : 'x64';
  const dmgDir = join(fileURLToPath(cwd), 'src-tauri/target/release/bundle/dmg');
  const intermediate = join(dmgDir, `${config.productName}_${config.version}_${arch}.dmg`);
  const internal = join(dmgDir, `${config.productName}_${config.version}_INTERNAL_${arch}.dmg`);
  if (existsSync(internal) && existsSync(intermediate)) unlinkSync(intermediate);
} finally {
  rmSync(temporary, { recursive: true, force: true });
}
