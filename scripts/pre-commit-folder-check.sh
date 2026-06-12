#!/usr/bin/env bash
# [Input] Consume upstream contracts defined by `scripts/.folder.md`[Pos].
# [Output] Provide pre commit folder check capability to downstream modules.
# [Pos] script node in scripts
# [Sync] If this file changes, update this header and `scripts/.folder.md`.

set -euo pipefail

if ! git rev-parse --git-dir >/dev/null 2>&1; then
  echo "Not a git repository. Skip folder map check."
  exit 0
fi

declare -A changed_dirs=()
declare -A staged_files=()

while IFS= read -r -d '' file_path; do
  staged_files["$file_path"]=1
  dir_path="$(dirname "$file_path")"
  changed_dirs["$dir_path"]=1
done < <(git diff --cached --name-only -z)

missing=0
for dir in "${!changed_dirs[@]}"; do
  folder_map="$dir/.folder.md"
  if [[ -f "$folder_map" ]]; then
    if [[ -z "${staged_files[$folder_map]+x}" ]]; then
      echo "WARNING: Files changed in '$dir' but '$folder_map' is not staged."
      missing=1
    fi
  fi
done

if [[ "$missing" -eq 1 ]]; then
  echo "Stage matching .folder.md updates or bypass with --no-verify if intentional."
  exit 1
fi

exit 0
