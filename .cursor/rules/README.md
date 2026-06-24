# Rules Reference

## Rules Index

- `docs/rules/no-hardcoding.md`
- `docs/rules/component-reuse.md`
- `.cursor/rules/vibe-engineering.mdc`
- `.cursor/rules/vibe-loading.mdc`
- `.cursor/rules/vibe-doc-sync.mdc`
- `.cursor/rules/vibe-component-reuse.mdc`

## Golden Rules

1. No hardcoding in business paths, IDs, thresholds, or environment-specific settings.
2. Before introducing any constant or variable, search higher-level/global config first and reuse existing definitions.
3. If no global definition exists, add one in centralized config and document source-of-truth.
