/** Read-only checks and explicit deletion intent; never issues a mutation. */
export async function syncAudioPreflight(request, boardDeviceId) {
  const reply=await request('usb_get_diagnostics',{expectedBoardDeviceId:boardDeviceId,componentSync:true});
  const audio=reply?.syncAudio;
  if(!audio||typeof audio.blocked!=='boolean')throw new Error('无法确认设备音频状态，尚未开始同步；请稍后重新检查。');
  return audio;
}
export function deviceDeletePolicy({clearSave=false,transport,capabilities={}}) {
  if(!clearSave&&(transport!=='usb'||capabilities.widgetDeletePreserveSave!==true))
    throw new Error('当前设备或连接不支持保留存档卸载；未删除。请升级固件，或明确勾选清除设备存档。');
  return {preserveSave:!clearSave,clearDeviceSaveConfirmed:clearSave};
}
export function componentTransferError(error) {
  const raw=typeof error==='string'?error:String(error);
  if(/audio.*busy|audio started/i.test(raw))return '设备音频仍忙，尚未确认同步成功。请停止播放或结束语音，再检查设备组件清单；不要直接重复提交。';
  if(/storage.*busy/i.test(raw))return '设备正在处理组件存储。请等待当前操作结束并检查设备清单，再决定是否重新同步。';
  if(/acknowledg.*timed out|commit.*(?:timeout|timed out)|回执.*超时/i.test(raw))return '设备回执超时，结果尚未确认。请先刷新设备组件清单核对结果；不会自动重发本次同步。';
  return raw;
}
