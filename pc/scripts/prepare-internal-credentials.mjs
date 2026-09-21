/** Internal-only credential input; never log values or copy the source file into resources. */
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export function parseInternalCredentials(raw) {
  if (Buffer.byteLength(raw) > 64 * 1024) throw new Error('Internal credentials input is too large');
  let value;
  try { value = JSON.parse(raw); } catch { throw new Error('Invalid internal credentials JSON'); }
  if (!value || typeof value !== 'object' || Array.isArray(value) ||
      Object.keys(value).sort().join(',') !== 'arkApiKey,speechApiKey') {
    throw new Error('Only arkApiKey and speechApiKey are allowed');
  }
  for (const key of ['arkApiKey', 'speechApiKey']) {
    if (typeof value[key] !== 'string' || !/^[A-Za-z0-9_.-]{16,256}$/.test(value[key])) {
      throw new Error('Internal API key is missing or invalid');
    }
  }
  return { arkApiKey: value.arkApiKey, speechApiKey: value.speechApiKey };
}

export function readInternalCredentials() {
  try {
    const path = process.env.PET_MANAGER_INTERNAL_CREDENTIALS_FILE;
    if (!path) throw new Error();
    return parseInternalCredentials(readFileSync(path, 'utf8'));
  } catch { throw new Error('Cannot load validated internal credentials; contents and path are not logged'); }
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    if (!process.argv[2]) throw new Error();
    writeFileSync(process.argv[2], JSON.stringify(readInternalCredentials()), { mode: 0o600 });
    console.log('Internal credential inputs validated (values hidden).');
  } catch {
    console.error('Internal credential validation failed (values hidden).');
    process.exitCode = 1;
  }
}
