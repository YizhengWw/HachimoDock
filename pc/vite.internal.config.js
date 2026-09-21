/** Explicit internal-only entry point. Public Vite config always embeds an empty key. */
import { mergeConfig } from 'vite';
import publicConfig from './vite.config.js';
import { readInternalCredentials } from './scripts/prepare-internal-credentials.mjs';

export default mergeConfig(publicConfig, {
  define: { __PET_MANAGER_INTERNAL_CONTENT_API_KEY__: JSON.stringify(readInternalCredentials().arkApiKey) },
});
