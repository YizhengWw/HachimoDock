// Read-only release verification. Never increments download counts or changes works.
import { createHash } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { resolve } from 'node:path';
const out = resolve(process.argv[2] || '../builds/local-delights-20261022');
const origin = process.argv[3];
if (!origin || new URL(origin).protocol !== 'https:') throw new Error('Provide an HTTPS community origin');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const manifests = JSON.parse(readFileSync(resolve(out, 'publication-manifest.json')));
const report = [];
async function get(path) {
  const r = await fetch(new URL(path, origin), { signal: AbortSignal.timeout(20000) });
  if (!r.ok) throw new Error(`${path}: HTTP ${r.status}`);
  return r;
}
for (const local of manifests) {
  const work = await (await get(`/api/v1/works/${local.id}`)).json();
  const bytes = Buffer.from(await (await get(`/api/v1/packages/${work.sha256}`)).arrayBuffer());
  const localBytes = readFileSync(resolve(out, `${local.id}.clawpkg`));
  if (!work.available || work.version !== local.version || hash(bytes) !== local.sha256
      || !bytes.equals(localBytes) || work.packageBytes !== bytes.length
      || work.requirements.localTools !== 'p4-local-tools-v1') throw new Error(`${local.id}: release mismatch`);
  for (const cover of work.covers) {
    const pixels = Buffer.from(await (await get(`/api/v1/images/${cover}`)).arrayBuffer());
    if (hash(pixels) !== cover) throw new Error(`${local.id}: cover checksum mismatch`);
  }
  report.push({ id: local.id, name: work.name, version: work.version, available: true,
    sha256: work.sha256, bytes: bytes.length, covers: work.covers,
    downloadMatchesLocal: true, deviceVerified: false });
}
writeFileSync(resolve(out, 'community-verification.json'), JSON.stringify({ verifiedAt: new Date().toISOString(), works: report }, null, 2));
console.log(JSON.stringify(report, null, 2));
