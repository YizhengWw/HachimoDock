import { API_CONFIGURATION_UPDATED_EVENT } from '../lib/api-configuration.js';

// Subscribe in the persistent dashboard controller, never in its visible-only
// voice panel. API settings deliberately unmounts that panel to save resources.
export function subscribeAsrCredentialUpdates(target, onSaved) {
  const handleSaved = event => {
    if (event?.detail?.providerId !== 'volcengine-asr' || event.detail.configured !== true) return;
    onSaved();
  };
  target.addEventListener(API_CONFIGURATION_UPDATED_EVENT, handleSaved);
  return () => target.removeEventListener(API_CONFIGURATION_UPDATED_EVENT, handleSaved);
}
