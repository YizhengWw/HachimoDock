// Synthetic modal regression: no real settings, credentials, speech, or USB access.
import { chromium } from 'playwright';
import assert from 'node:assert/strict';
const base = process.env.PET_TEST_URL || 'http://127.0.0.1:4189';
assert.equal(new URL(base).hostname, '127.0.0.1');
const browser = await chromium.launch({ headless: true, ...(process.env.PET_TEST_BROWSER ? {executablePath:process.env.PET_TEST_BROWSER} : {}) });
try {
  const page = await browser.newPage({ viewport: { width: 1100, height: 850 } });
  const errors = [];
  page.on('console', message => { if(message.type()==='error') console.error(message.text()); });
  page.on('pageerror', e => errors.push(e.message));
  await page.route('**/__test-global-volume', r => r.fulfill({ contentType:'text/html', body:'<html lang="zh"><body><div id="root"></div></body></html>' }));
  await page.goto(`${base}/__test-global-volume`);
  await page.evaluate(async () => {
    const Refresh = (await import('/@react-refresh')).default;
    Refresh.injectIntoGlobalHook(window); window.$RefreshReg$=()=>{}; window.$RefreshSig$=()=>type=>type;
    window.__vite_plugin_react_preamble_installed__=true;
    window.testVolume=0.3; window.testWrites=[]; window.testSaved=false;
    window.__TAURI_INTERNALS__={ invoke:async (command,args)=>{
      if(command==='load_realtime_voice_volume')return window.testVolume;
      if(command==='save_realtime_voice_volume'){window.testVolume=args.volume;return args.volume;}
      if(command==='realtime_chat_update_persona')return false;
      if(command==='plugin:fs|exists')return true;
      if(command==='plugin:fs|mkdir')return;
      if(command==='plugin:fs|write_text_file'){window.testWrites.push(args);return;}
      if(command==='plugin:path|resolve_directory')return '/synthetic-app-data';
      if(command==='load_board_button_config')return null;
      throw new Error(`Unexpected mock command: ${command}`);
    }};
    const React=(await import('/node_modules/.vite/deps/react.js')).default;
    const dom=await import('/node_modules/.vite/deps/react-dom_client.js');
    const {default:Modal}=await import('/src/PersonaVoiceModal.jsx');
    const {normalizePersonaVoice}=await import('/src/lib/persona-voice.js');
    const root=(dom.default||dom).createRoot(document.getElementById('root'));
    let counter=0;
    window.showVolumePet=(name,oldVolume)=>root.render(React.createElement(Modal,{
      key:++counter,appearance:{id:name,name,type:'custom',personaVoice:normalizePersonaVoice({
        persona:{display_name:name,intro:'测试角色',style:'简短回答'},voice:{volume:oldVolume},
      },{name})},previewAvailable:true,onPreview:async form=>{window.testPreviewVolume=form.voice.volume;},
      onSaved:()=>{window.testSaved=true;},onClose:()=>{},
    }));
    const css=document.createElement('link');css.rel='stylesheet';css.href='/src/styles.css';document.head.append(css);
    window.showVolumePet('pet-a',1.5);
  });
  const volume=page.locator('label').filter({hasText:'全局音量'}).locator('input');
  await page.waitForFunction(()=>document.querySelectorAll('input[type="range"]')[1]?.value==='0.3');
  assert.equal(await volume.inputValue(),'0.3');
  await page.getByRole('button',{name:'试听',exact:true}).click();
  await page.waitForFunction(()=>window.testPreviewVolume===0.3);
  await volume.fill('0.65');
  await page.getByRole('button',{name:'恢复默认',exact:true}).click();
  assert.equal(await volume.inputValue(),'0.65');
  await page.getByRole('button',{name:'保存',exact:true}).click();
  await page.waitForFunction(()=>window.testSaved===true);
  assert.equal(await page.evaluate(()=>window.testVolume),0.65);
  await page.evaluate(()=>window.showVolumePet('pet-b',0.2));
  await page.waitForFunction(()=>document.querySelectorAll('input[type="range"]')[1]?.value==='0.65');
  assert.equal(await volume.inputValue(),'0.65');
  assert.deepEqual(errors,[]);
  console.log('PASS: global gain overrides both pets; preview, save, factory reset and switching verified.');
} finally { await browser.close(); }
