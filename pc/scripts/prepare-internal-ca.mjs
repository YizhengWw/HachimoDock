/** Select explicitly approved public CA certificates for an internal-only build.
 * Source bundle, fingerprints and generated PEM must stay outside the source tree.
 */
import { X509Certificate } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

export function selectApprovedCa(raw, approved, now = new Date()) {
  if (Buffer.byteLength(raw) > 2 * 1024 * 1024 || /PRIVATE KEY/.test(raw)) {
    throw new Error('CA input must be a bounded public certificate bundle, without private keys');
  }
  const fingerprints = approved.split(',').map(s => s.trim().replaceAll(':', '').toUpperCase());
  if (!fingerprints.length || fingerprints.length > 8 || fingerprints.some(s => !/^[A-F0-9]{64}$/.test(s)) || new Set(fingerprints).size !== fingerprints.length) {
    throw new Error('Provide 1–8 distinct approved SHA-256 certificate fingerprints');
  }
  const all = [...raw.matchAll(/-----BEGIN CERTIFICATE-----[\s\S]*?-----END CERTIFICATE-----/g)]
    .map(m => new X509Certificate(m[0]));
  const selected = fingerprints.map(f => {
    const matches = all.filter(c => c.fingerprint256.replaceAll(':', '') === f);
    if (matches.length !== 1) throw new Error('Approved certificate missing or duplicated');
    const cert = matches[0];
    if (!cert.ca || new Date(cert.validFrom) > now || new Date(cert.validTo) <= now) {
      throw new Error('Approved certificate must be a currently valid CA');
    }
    return cert;
  });
  // Every selected certificate must lead to a selected self-signed root.
  function reachesRoot(cert, visited = new Set()) {
    if (visited.has(cert.fingerprint256)) return false;
    const next = new Set(visited).add(cert.fingerprint256);
    if (cert.checkIssued(cert) && cert.verify(cert.publicKey)) return true;
    return selected.some(parent => parent !== cert && cert.checkIssued(parent) && cert.verify(parent.publicKey) && reachesRoot(parent, next));
  }
  if (!selected.every(c => reachesRoot(c))) throw new Error('Approved CA chain is incomplete or invalid');
  const pem = selected.map(c => c.toString()).join('\n');
  if (Buffer.byteLength(pem) > 64 * 1024) throw new Error('Selected CA chain is too large');
  return pem;
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
  try {
    const input = process.env.PET_MANAGER_INTERNAL_CA_FILE;
    const approved = process.env.PET_MANAGER_INTERNAL_CA_SHA256;
    if (!input || !approved || !process.argv[2]) throw new Error('Internal CA file, approved fingerprints and output are required');
    const pem = selectApprovedCa(readFileSync(input, 'utf8'), approved);
    writeFileSync(process.argv[2], pem, { mode: 0o600 });
    console.log('Internal CA chain validated; no credentials or proxy included.');
  } catch (error) {
    // Avoid leaking source paths or certificate contents into build logs.
    console.error('Internal CA validation failed. Check the public CA bundle, approved fingerprints, validity and chain.');
    process.exitCode = 1;
  }
}
