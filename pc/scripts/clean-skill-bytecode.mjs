/** Remove generated Python bytecode only; source files and non-cache data are preserved. */
import { existsSync, readdirSync, rmdirSync, unlinkSync } from "node:fs";
import { join } from "node:path";

export function cleanSkillBytecode(root) {
  if (!existsSync(root)) return 0;
  let removed = 0;
  for (const entry of readdirSync(root, { withFileTypes: true })) {
    const file = join(root, entry.name);
    if (entry.isDirectory()) {
      removed += cleanSkillBytecode(file);
      if (entry.name === "__pycache__" && readdirSync(file).length === 0) rmdirSync(file);
    } else if (entry.isFile() && /\.py[co]$/.test(entry.name)) {
      unlinkSync(file); removed++;
    }
  }
  return removed;
}
