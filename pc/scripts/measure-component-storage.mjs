/** Read-only capacity audit. No device, cloud, user library or credential access.
 * Optional argument: directory of newly compiled native images, grouped by component ID.
 * Uses checked-in verified native caches for builtins and measures PC/source separately.
 */
import {readFileSync,readdirSync,statSync,existsSync} from 'node:fs';
import {resolve,dirname,join,relative} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createHash} from 'node:crypto';
const repo=resolve(dirname(fileURLToPath(import.meta.url)),'../..');
const supplied=process.argv[2]&&resolve(process.argv[2]);
const json=p=>JSON.parse(readFileSync(p,'utf8'));
const digest=b=>createHash('sha256').update(b).digest('hex');
const compact=o=>Buffer.byteLength(JSON.stringify(o));
function stable(o){return Array.isArray(o)?o.map(stable):o&&typeof o==='object'?Object.fromEntries(Object.keys(o).sort().map(k=>[k,stable(o[k])])):o;}
function bytes(dir){if(!existsSync(dir))return 0;return readdirSync(dir,{withFileTypes:true}).filter(e=>!e.name.startsWith('.')).reduce((sum,e)=>sum+(e.isDirectory()?bytes(join(dir,e.name)):e.isFile()?statSync(join(dir,e.name)).size:0),0);}
const items=[];
for(const root of ['pc/builtin-clawpkgs','pc/new-components']){
  for(const entry of readdirSync(join(repo,root),{withFileTypes:true})){
    const path=join(repo,root,entry.name);
    if(!entry.isDirectory()||!existsSync(join(path,'component.json')))continue;
    const manifest=json(join(path,'component.json')),widget=json(join(path,'runtime/widget.json')),buttons=json(join(path,'buttons.json'));
    let imageBytes=0,files=2;const errors=[];
    for(const image of widget.surface?.images||[]){
      const cache=join(repo,'firmware/assets/builtin-surfaces',manifest.id);
      const cacheIndex=join(cache,'index.json');
      let data;
      try{
        if(existsSync(cacheIndex)){
          const record=json(cacheIndex)[image.id];data=readFileSync(join(cache,`${image.id}.p4i`));
          if(record?.source!==digest(readFileSync(join(path,image.asset)))||record?.spec!==digest(JSON.stringify(stable(image)))||record?.encoded!==digest(data))throw Error('native cache is stale');
        }else if(supplied)data=readFileSync(join(supplied,manifest.id,`${image.id}.p4i`));
        else throw Error('no native image cache; compile before estimating');
        if(!['P4I1','P4I2','P4J1'].includes(data.toString('ascii',0,4))||data.readUInt32LE(8)!==data.length)throw Error('invalid native image header');
        imageBytes+=data.length;files++;
      }catch(e){errors.push(`${image.id}: ${e.message}`);}
    }
    // Scene sprites use P4S1: 8-byte header + RGB565/alpha for each pixel.
    if(!widget.surface?.images?.length)for(const sprite of widget.scene?.sprites||[]){imageBytes+=8+sprite.frame_width*sprite.frame_height*sprite.frames*3;files++;}
    items.push({id:manifest.id,name:manifest.name,version:manifest.version,source:relative(repo,path),hidden:manifest.id==='music-player',pcSourceBytes:bytes(path),pcHostBytes:bytes(join(path,'pc')),deviceJsonBytes:compact(widget)+compact(buttons),deviceImageBytes:imageBytes,devicePayloadBytes:compact(widget)+compact(buttons)+imageBytes,deviceFiles:files,errors});
  }
}
const visible=items.filter(x=>!x.hidden);
const total=(rows,key)=>rows.reduce((sum,x)=>sum+x[key],0);
const app=join(repo,'firmware/.pio/build/esp32_p4_evboard_v3/firmware.bin');
const payload=total(visible,'devicePayloadBytes');
console.log(JSON.stringify({scope:'Current visible repo components, one version each; excluded retired components and hidden music-player. PC source bytes are unpacked files, not installer size. Device payload excludes filesystem overhead, user saves and OTA generations.',count:visible.length,pcSourceBytes:total(visible,'pcSourceBytes'),pcHostBytes:total(visible,'pcHostBytes'),devicePayloadBytes:payload,deviceFiles:total(visible,'deviceFiles'),twoDeviceGenerationsBytes:2*payload,firmwareAppBytes:existsSync(app)?statSync(app).size:null,appSlotBytes:0x380000,storagePartitionBytes:0x4e0000,configuredFlashBytes:32*1024*1024,items},null,2));
if(items.some(x=>x.errors.length))process.exitCode=1;
