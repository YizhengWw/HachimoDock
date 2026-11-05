/** Keep public package preflight tied to the actual reviewed lock inventory. */
import test from 'node:test';
import {fileURLToPath} from 'node:url';
import {verifyDesktopLicenseInventory} from '../scripts/dependency-license-resources.mjs';
test('desktop dependency inventory and all notice carriers match current source',()=>{
 verifyDesktopLicenseInventory(fileURLToPath(new URL('../../',import.meta.url)));
});
