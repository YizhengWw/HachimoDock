export const EMPTY_MEDIA = {state:"idle",message:"",stations:[],history:[],volume:35,current:null,sleepMinutes:0,level:0};
export function mediaQueue(s=EMPTY_MEDIA) { return s.queue || s.stations || []; }
export function mediaReadiness(s=EMPTY_MEDIA,enabled=true) {
  if(!enabled)return {title:"组件服务已停用",message:"你已手动停用此服务，可重新开启；收藏和历史仍保留。"};
  if(!s.connected)return {title:"请连接设备 USB",message:s.automatic?"电台将自动备好，连接设备后进入组件即可收听。请保持 Pet Manager 运行。":"可以先搜索和收藏；连接设备后才能播放。请保持 Pet Manager 运行。"};
  if(s.supported===false)return {title:"设备固件需要更新",message:"当前固件不支持组件媒体播放，请先升级固件。"};
  if(s.automatic)return {title:s.discovering?"正在自动准备电台":"电台已自动备好",message:`${s.location||"识别城市中"} · ${mediaQueue(s).length} 台；进入设备组件收听，左右切台，主键播放/停止。`};
  if(s.discovering)return {title:"正在加载设备电台列表",message:"加载完成后，在设备打开列表选择；不会自动播放。"};
  if(!mediaQueue(s).length)return {title:"尚未加载电台",message:s.message||"在设备打开列表加载；也可在 PC 搜索补充电台。"};
  return {title:"在设备选台即可收听",message:"左右切台；副键打开列表，上下选择、中键收听。PC 搜索和收藏为可选操作。"};
}
export function rgb565(value) {
  const n=Number(value)||0;
  return `rgb(${Math.round(((n>>11)&31)*255/31)},${Math.round(((n>>5)&63)*255/63)},${Math.round((n&31)*255/31)})`;
}
export function mediaBindings(snapshot=EMPTY_MEDIA) {
  const s={...EMPTY_MEDIA,...snapshot};
  const queue=mediaQueue(s),index=queue.findIndex(t=>t.id===s.current?.id);
  return {title:s.current?.name||"选一台，听一会",subtitle:s.current?.city||"打开列表，发现好声音",message:s.message||"",
    status:({playing:"正在直播",buffering:"正在缓冲",error:"播放失败"})[s.state]||(queue.length?"等待收听":"等待选台"),
    volume:`${s.volume}%`,index:index<0?"00":String(index+1).padStart(2,"0"),count:String(queue.length).padStart(2,"0"),level:String(s.level||0)};
}
