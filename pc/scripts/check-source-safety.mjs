/** [Input] Public tracked and unignored files. [Output] A fail-closed credential/artifact gate.
 * [Pos] Public source CI; complementary to Gitleaks, not a full security audit. */
import { execFileSync } from 'node:child_process';
import { readFileSync, lstatSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { homedir } from 'node:os';
const root = resolve(dirname(fileURLToPath(import.meta.url)), '../..');
const paths = execFileSync('git', ['ls-files', '--cached', '--others', '--exclude-standard', '-z'], {cwd: root})
  .toString().split('\0').filter(Boolean);
const problems = [];
for (const file of new Set(paths)) {
  // These two hand-written fixtures emulate the upstream package layout, not build output.
  const testFixture = /^pc\/src-tauri\/bridge\/packages\/agent-session-bus\/test\/fixtures\/fake-openclaw\/dist\/(?:index|plugin-sdk\/agent-runtime)\.js$/.test(file);
  if (!testFixture && /(?:^|\/)(?:node_modules|dist|target|generated-runtime|\.runtime-cache|\.pio|managed_components|legacy|\.claude|\.codex)(?:\/|$)/.test(file)
      || /(?:\.internal-build-|bundle-secrets\.env|\.p12$|\.pfx$|\.pem$|\.key$|\.log$|\.dmg$|\.exe$|\.msi$|\.jsonl$)/i.test(file)
      || /(?:^|\/)\.env(?:\.|$)/.test(file) && !file.endsWith('.env.example')) problems.push(`${file}: forbidden publication input`);
  const absolute = resolve(root, file);
  if (lstatSync(absolute).isSymbolicLink()) { problems.push(`${file}: symlink requires review`); continue; }
  const bytes = readFileSync(absolute);
  // A public checkout may contain either LFS pointers or resolved binary assets.
  if (bytes.includes(0)) continue;
  const text = bytes.toString();
  if (/-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----/.test(text)
      || /\b(?:ark-[a-f0-9-]{25,}|sk-[A-Za-z0-9_-]{24,}|gh[pousr]_[A-Za-z0-9]{24,}|AKIA[0-9A-Z]{16})/.test(text)) {
    problems.push(`${file}: potential credential; value redacted`);
  }
  if (text.includes(`${homedir()}/`)) problems.push(`${file}: personal build path`);
}
if (problems.length) { console.error(problems.join('\n')); process.exit(1); }
console.log(`Source publication checks passed for ${paths.length} paths.`);
