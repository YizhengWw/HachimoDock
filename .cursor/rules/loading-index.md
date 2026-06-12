# Loading Index

## Scenario -> File Mapping

| Scenario | Primary File |
|---|---|
| Session continuity | `today.md` |
| Global constraints | `CLAUDE.md` |
| Rules reference | `docs/rules/README.md` |
| Tech stack baseline | `docs/tech-stack.md` |
| Session start protocol | `docs/session-start.md` |
| Pet-Claw lightweight admin product draft | `docs/pet-claw-lite-admin.md` |
| Engineering guardrails | `.cursor/rules/vibe-engineering.mdc` |
| Loading discipline | `.cursor/rules/vibe-loading.mdc` |
| Folder/File sync contract | `.cursor/rules/vibe-doc-sync.mdc` |
| Component reuse policy | `.cursor/rules/vibe-component-reuse.mdc` |
| Error immunization | `patterns.md` |

## Loading Discipline
- Never pre-load all heavy docs.
- Hold at most 2 heavy docs at once.
- Announce each on-demand load with explicit purpose.
