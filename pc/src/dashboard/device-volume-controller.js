export const MASTER_VOLUME_ENGINE = 'p4-master-volume-v1';
export const EMPTY_VOLUME_STATE = Object.freeze({value:null,draft:null,known:false,busy:false,loading:false,persisted:false,error:'',available:false,reason:'请先连接设备'});
const percent = v => Number.isInteger(v) && v >= 0 && v <= 100;

export function createDeviceVolumeController({invoke,onState=()=>{}}) {
  let state={...EMPTY_VOLUME_STATE}, target=null, generation=0, disposed=false;
  const publish = patch => {state={...state,...patch};onState({...state});};
  const current = serial => !disposed && serial===generation;
  const valid = (reply,board) => reply?.ok===true && reply.engine===MASTER_VOLUME_ENGINE && reply.boardDeviceId===board && percent(reply.volume) && typeof reply.persisted==='boolean';
  const message = error => typeof error==='string' ? error : error?.message || '设备未返回有效的确认';
  async function request(value) {
    if (!target?.available || state.busy) return false;
    const setting=value!==undefined;
    if (setting && (!percent(value)||!state.known)) return false;
    if (setting && value===state.value && (state.persisted || state.savePending===true)) {publish({draft:value});return true;}
    const serial=generation, board=target.boardDeviceId;
    publish({busy:true,loading:!setting,error:'',...(setting?{draft:value}:{})});
    try {
      const reply=await invoke('usb_device_volume',{expectedBoardDeviceId:board,...(setting?{volume:value}:{})});
      if (!current(serial)) return false;
      if (!valid(reply,board) || (setting&&reply.volume!==value)) throw Error('设备音量确认不匹配');
      publish({value:reply.volume,draft:reply.volume,known:true,persisted:reply.persisted,persistState:reply.persistState||'',savePending:reply.savePending,error:''});
      return true;
    } catch(error) {
      if (current(serial)) publish({known:false,value:null,persisted:false,error:setting
        ? `设置结果尚未确认，请先读取设备音量；不会自动重复设置。${message(error)}`
        : `读取设备音量失败：${message(error)}`});
      return false;
    } finally {
      if (current(serial)) publish({busy:false,loading:false});
    }
  }
  return {
    getSnapshot:()=>({...state}),
    setTarget(value) {
      if (disposed) {disposed=false;target=null;}
      const key=JSON.stringify([value?.connected,value?.boardDeviceId,value?.bootId,value?.connectionId,value?.portName,value?.engine,value?.runtime]);
      if (key===target?.key) return Promise.resolve(false);
      generation++;
      const connected=value?.connected===true && Boolean(value.boardDeviceId);
      const supported=value?.runtime==='esp-p4' && value?.engine===MASTER_VOLUME_ENGINE;
      target={key,boardDeviceId:value?.boardDeviceId||'',available:connected&&supported};
      state={...EMPTY_VOLUME_STATE,available:target.available,reason:!connected?'请先连接设备':supported?'':'更新设备固件后可调节总音量'};
      onState({...state});
      return target.available ? request() : Promise.resolve(false);
    },
    refresh:()=>request(),
    setDraft(value) {if(percent(value)&&state.known&&!state.busy)publish({draft:value});},
    apply:(value=state.draft)=>request(value),
    dispose(){disposed=true;generation++;target=null;},
  };
}
