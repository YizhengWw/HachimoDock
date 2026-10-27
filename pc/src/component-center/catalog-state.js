// Shared PC catalog semantics. Unknown versions must never mean “up to date”.
export function componentRequiresPc(component) {
  // Legacy Token telemetry is host-fed without a data.source declaration.
  return Boolean(component?.id === "token-usage" || component?.pcApplication || component?.pcExtension || component?.dataSource || component?.mediaSource || component?.appRuntime?.host);
}
export function compareVersions(a, b) {
  const parse = value => {
    const m = String(value || '').trim().match(/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([\da-zA-Z-]+(?:\.[\da-zA-Z-]+)*))?(?:\+[\da-zA-Z-]+(?:\.[\da-zA-Z-]+)*)?$/);
    if (!m) return null;
    const pre=m[4]?.split('.') || [];
    if(pre.some(part=>/^0\d+$/.test(part)))return null;
    return {main:m.slice(1,4).map(BigInt),pre};
  };
  const x=parse(a),y=parse(b); if(!x||!y)return null;
  for(let i=0;i<3;i++)if(x.main[i]!==y.main[i])return x.main[i]>y.main[i]?1:-1;
  if(!x.pre.length||!y.pre.length)return x.pre.length===y.pre.length?0:x.pre.length?-1:1;
  for(let i=0;i<Math.max(x.pre.length,y.pre.length);i++){
    const l=x.pre[i],r=y.pre[i]; if(l===r)continue;
    if(l===undefined||r===undefined)return l===undefined?-1:1;
    const ln=/^\d+$/.test(l),rn=/^\d+$/.test(r);
    if(ln&&rn){if(BigInt(l)!==BigInt(r))return BigInt(l)>BigInt(r)?1:-1;}
    else if(ln!==rn)return ln?-1:1;
    else return l>r?1:-1;
  }
  return 0;
}
export function devicePackageState(component, inventory) {
  if(inventory?.freshness!=='live')return {key:'unknown',label:'设备待核对',action:'配置与同步'};
  const installed=inventory.items?.find(x=>x.id===component.id);
  if(!installed)return {key:'available',label:'未安装',action:'同步到设备'};
  if(component.isDeviceOnly)return {key:'device-only',label:'仅设备 · 缺少本地源',action:'查看设备组件'};
  const order=compareVersions(component.version,installed.version);
  if(order===1)return {key:'update',label:`可更新 · 设备 ${installed.version}`,action:'查看更新'};
  if(order===-1)return {key:'newer',label:`设备版本更高 · ${installed.version}`,action:'查看版本'};
  return {key:order===0?'installed':'unknown',label:order===0?`已安装 ${installed.version}`:'已安装 · 版本待核对',action:'配置与同步'};
}
export function readCenterPreference(key, fallback) {
  try{
    const value=JSON.parse(localStorage.getItem(`pet-manager:center:${key}`));
    if(value===null||typeof value!==typeof fallback)return fallback;
    const choices={kind:['all','game','tool'],status:['all','installed','update','pc','offline'],tab:['discover','local','favorites','created']};
    return choices[key]&&!choices[key].includes(value)?fallback:value;
  }catch{return fallback;}
}
export function saveCenterPreference(key,value) {
  try{localStorage.setItem(`pet-manager:center:${key}`,JSON.stringify(value));}catch{/* noncritical view preference */}
}
