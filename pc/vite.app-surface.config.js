// Local-only QA. This config and artifact inputs are not production build entries.
import {defineConfig} from 'vite';
import react from '@vitejs/plugin-react';
import fs from 'node:fs';
import path from 'node:path';
import { componentUiMetadataPlugin } from './scripts/component-ui-metadata.mjs';
export default defineConfig({plugins:[componentUiMetadataPlugin(),react(),{name:'app-fixtures',configureServer(server){
  server.middlewares.use('/__app_fixtures',(_req,res)=>{
    try{
      const root=process.env.PET_APP_PACKAGE_DIR;if(!root)throw Error('未配置测试包');
      const items=fs.readdirSync(root,{withFileTypes:true}).filter(d=>d.isDirectory()&&/^[a-z][a-z0-9_-]{0,43}$/.test(d.name)).map(d=>{
        const dir=path.join(root,d.name),read=n=>JSON.parse(fs.readFileSync(path.join(dir,n),'utf8'));
        const runtime=read('runtime/widget.json');
        const images=(runtime.surface?.images||[]).map(im=>{if(!/^assets\/[\w-]+\.png$/.test(im.asset))throw Error('非法图片路径');return {id:im.id,dataUrl:'data:image/png;base64,'+fs.readFileSync(path.join(dir,im.asset)).toString('base64')};});
        return {manifest:read('component.json'),runtime,buttons:read('buttons.json'),images};
      });res.setHeader('Content-Type','application/json; charset=utf-8');res.setHeader('Cache-Control','no-store');res.end(JSON.stringify(items));
    }catch{res.statusCode=500;res.end('{"error":"测试包无法读取"}');}
  });
}}],server:{host:'127.0.0.1',port:4191,strictPort:true}});
