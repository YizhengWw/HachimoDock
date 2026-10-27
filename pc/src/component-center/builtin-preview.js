// Only build-owned resource declarations; never user's runtime data or private saves.
// The cover and installation payload now share the same source of truth.
import {initialApp,appDashboard} from './app-runtime.js';
import {COMPONENT_AUTHORS} from './component-authors.js';
import {sortComponentsByDefaultOrder} from './library-order.js';
const manifests=import.meta.glob('../../builtin-clawpkgs/*/component.json',{eager:true,import:'default'});
const runtimes=import.meta.glob('../../builtin-clawpkgs/*/runtime/widget.json',{eager:true,import:'default'});
const bindings=import.meta.glob('../../builtin-clawpkgs/*/buttons.json',{eager:true,import:'default'});
const previews=import.meta.glob('../../builtin-clawpkgs/*/negative-screen.json',{eager:true,import:'default'});
const applications=import.meta.glob('../../builtin-clawpkgs/*/pc/application.json',{eager:true,query:'?component-ui',import:'default'});
const extensions=import.meta.glob('../../builtin-clawpkgs/*/pc/extension.json',{eager:true,import:'default'});
const images=import.meta.glob('../../builtin-clawpkgs/*/assets/*.png',{eager:true,query:'?url',import:'default'});
export function latestBuiltinPreview(component){
  const root=`../../builtin-clawpkgs/${component.id}`,manifest=manifests[`${root}/component.json`],runtime=runtimes[`${root}/runtime/widget.json`];
  if(!manifest||!runtime)return component;
  const declarations=runtime.surface?.images||runtime.scene?.sprites||[];
  return {...component,
    builtin:true,author:COMPONENT_AUTHORS[component.id]||'哈基米',
    pcApplication:applications[`${root}/pc/application.json`]||null,
    pcExtension:extensions[`${root}/pc/extension.json`]||null,
    name:manifest.name,version:manifest.version,kind:manifest.kind,goal:manifest.description,
    runtimeEngine:runtime.engine,gameType:runtime.game?.type||'',scene:runtime.scene||null,
    dataSource:runtime.data?.source||'',mediaSource:runtime.media?.source||'',instrumentSource:runtime.instrument?.source||'',
    mediaSurface:runtime.surface||null,appRuntime:runtime.surface?runtime:null,
    dashboard:runtime.surface?.engine==='p4-app-surface-v1'?appDashboard(runtime,initialApp(runtime)):previews[`${root}/negative-screen.json`]?.dashboard||component.dashboard,
    defaultBindings:bindings[`${root}/buttons.json`]||[],
    sceneSprites:declarations.map(im=>({...im,frameWidth:im.frame_width,frameHeight:im.frame_height,dataUrl:images[`${root}/${im.asset}`]})),
  };
}
export function completeBuiltinCatalog(existing) {
  const ids=new Set(existing.map(c=>c.id));
  const added=Object.values(manifests).filter(m=>!ids.has(m.id)).map(m=>({
    id:m.id,name:m.name,kind:m.kind,status:'ready',builtin:true,
    category:m.id.startsWith('hachimi-')?'哈基米专属':m.kind==='game'?'小游戏':'实用工具',
  }));
  return sortComponentsByDefaultOrder([...existing,...added].map(component=>{
    try{return latestBuiltinPreview(component);}
    catch{return {id:component.id,name:component.name||component.id,kind:component.kind,valid:false,validationErrors:['内置组件预览结构无效，请更新客户端或选择其他来源。'],dashboard:{title:'组件资源待修复'},defaultBindings:[]};}
  }));
}
