// Keep reviewed community packages and factory/desktop built-ins byte-identical.
// Only these three build-owned directories are managed; never user's library.
import {cpSync, existsSync, readdirSync, readFileSync} from 'node:fs';
import {fileURLToPath} from 'node:url';
import {join} from 'node:path';
const root = fileURLToPath(new URL('../', import.meta.url));
const ids = ['decision-capsule', 'answer-book', 'hope-train'];
function files(dir) {
  return readdirSync(dir, {withFileTypes:true}).flatMap(entry => {
    if (entry.isSymbolicLink()) throw Error(`Unexpected symlink: ${entry.name}`);
    return entry.isDirectory() ? files(join(dir,entry.name)).map(p=>`${entry.name}/${p}`) : [entry.name];
  }).sort();
}
for (const id of ids) {
  const src=join(root,'community-components/local-delights',id);
  const dst=join(root,'builtin-clawpkgs',id);
  if (!process.argv.includes('--check')) cpSync(src,dst,{recursive:true});
  if (!existsSync(dst) || JSON.stringify(files(src))!==JSON.stringify(files(dst)) ||
      files(src).some(p=>!readFileSync(join(src,p)).equals(readFileSync(join(dst,p))))) {
    throw Error(`Built-in ${id} differs from reviewed source; regenerate before packaging`);
  }
}
console.log('Three local-delights built-ins match reviewed packages byte-for-byte.');
