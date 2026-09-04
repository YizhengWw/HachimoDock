/**
 * [Input] Build environment and local-only credential filenames.
 * [Output] Fail-closed public build gate; no private build override is supported.
 * [Pos] Public source build boundary. See ../.folder.md.
 */
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
const forbiddenVariables = [
  'PET_MANAGER_INTERNAL_ASR_API_KEY', 'VITE_INTERNAL_CONTENT_API_KEY',
  'PET_MANAGER_INTERNAL_BUILD_AUTHORIZED', 'PET_MANAGER_BUNDLED_MQTT_URL',
  'PET_MANAGER_BUNDLED_MQTT_USERNAME', 'PET_MANAGER_BUNDLED_MQTT_PASSWORD',
];
for (const name of forbiddenVariables) {
  if (process.env[name]?.trim()) throw new Error(`Public build refuses credential injection through ${name}`);
}
for (const relative of ['../.internal-build-secrets.json', '../src-tauri/bundle-secrets.env']) {
  if (existsSync(fileURLToPath(new URL(relative, import.meta.url)))) {
    throw new Error('Remove local credential build files before building public sources');
  }
}
console.log('Public build verified: no embedded credential inputs.');
