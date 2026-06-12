# No Hardcoding Rule

## Mandatory

- Do not hardcode business identifiers, host paths, model params, timeouts, retries, thresholds, or feature flags in feature code.
- Before writing any new value, search whether a higher-level/global definition already exists.
- If a global definition exists, reuse it directly.
- If not, define it once in centralized config and reference it.

## Global-First Search Order

1. Project-level config/settings modules
2. Environment variables and typed settings
3. Shared constants/tokens files
4. Local module constants (only if previous layers do not apply)
