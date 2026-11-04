// Render the real React entry, with no browser, USB, providers or network.
import {buildSync} from 'esbuild';
import {mkdtempSync,writeFileSync,rmSync} from 'node:fs';
import {tmpdir} from 'node:os';import {join,resolve} from 'node:path';import {fileURLToPath} from 'node:url';import {spawnSync} from 'node:child_process';
const pc=resolve(fileURLToPath(new URL('..',import.meta.url))),tmp=mkdtempSync(join(tmpdir(),'pet-d-surface-'));
try{
 const entry=join(tmp,'entry.jsx'),output=join(tmp,'test.cjs');
 writeFileSync(entry,`import React from ${JSON.stringify(join(pc,'node_modules/react/index.js'))};
import {renderToStaticMarkup} from ${JSON.stringify(join(pc,'node_modules/react-dom/server.node.js'))};
import {AppSurface} from ${JSON.stringify(join(pc,'src/component-center/AppSurface.jsx'))};
import {initialApp} from ${JSON.stringify(join(pc,'src/component-center/app-runtime.js'))};
import fs from 'node:fs';import assert from 'node:assert/strict';
const suite=n=>JSON.parse(fs.readFileSync(${JSON.stringify(join(pc,'skills/petui/contracts/'))}+n+'.json'));
for(const name of ['app-shell-v1','app-form-v3','data-rows-v1','longtext-v1']){
 const f=suite(name),r=f.runtime,s=initialApp(r);if(f.rows)s.data={rows:f.rows,status:'ok'};
 const html=renderToStaticMarkup(<AppSurface runtime={r} snapshot={s} active={true}/>);
 assert.ok(html.includes('<svg'));assert.ok(!html.includes('NaN'));
 if(r.surface.tabs)for(const t of r.surface.tabs.items)assert.ok(html.includes(t.label),t.label);
 if(name==='longtext-v1'){assert.ok(html.includes('<image'));assert.ok(html.includes(r.surface.elements.find(n=>n.type==='figure').caption));}
 console.log(name+' real AppSurface render OK');
}`);
 buildSync({entryPoints:[entry],bundle:true,platform:'node',format:'cjs',outfile:output,loader:{'.wasm':'dataurl','.css':'empty'},logLevel:'warning'});
 const result=spawnSync(process.execPath,[output],{encoding:'utf8'});process.stdout.write(result.stdout||'');process.stderr.write(result.stderr||'');process.exitCode=result.status??1;
}finally{rmSync(tmp,{recursive:true});}
