import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import {createHash} from 'node:crypto';
const root=new URL('../../',import.meta.url);
const notices={
  'MiSans-Font-License-Agreement.pdf':'4a93a27cd2bd81b3b5ecfd0a853144a876fa26938a93a68443c67d74172fcb86',
  'MiSans-Embedded-Fonts-NOTICE.txt':'5511b1367b17f089d554b138c3a08e98e36ce5e5675dd049419492d65a282b11',
};
test('both exact public MiSans notice payloads accompany the app resources',()=>{
  const config=JSON.parse(readFileSync(new URL('pc/src-tauri/tauri.conf.json',root)));
  for(const [name,hash] of Object.entries(notices)){
    const bytes=readFileSync(new URL('licenses/'+name,root));
    assert.equal(createHash('sha256').update(bytes).digest('hex'),hash);
    assert.equal(config.bundle.resources['../../licenses/'+name],'licenses/'+name);
  }
  const text=readFileSync(new URL('licenses/MiSans-Embedded-Fonts-NOTICE.txt',root),'utf8');
  assert(text.includes('16 px, 1 bpp')&&text.includes('32 px, 2 bpp')&&text.includes('199 glyphs'));
  assert(!text.includes('/Users/')&&!text.includes('/home/'));
});
