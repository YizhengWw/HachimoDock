/** User-facing OTA outcomes; raw native errors and identifiers stay out of toasts. */
export function firmwareUpdateFailureMessage(error) {
 const text=typeof error==='string'?error:String(error?.message||error||'');
 if(/firmware begin failed:[\s\S]*timed out/i.test(text))
  return '准备升级时未收到设备确认。已停止后续传输，请保持 USB 连接，确认设备状态后再操作。';
 if(/broken pipe|not connected|connection[^\n]*(?:closed|changed|lost)|no such file or directory/i.test(text))
  return '升级过程中 USB 连接中断，本次结果尚未确认。请保持连接，检查设备当前版本和状态。';
 if(/chip revision|firmware chip|wrong chip/i.test(text))
  return '固件与设备芯片版本不匹配，本次升级已停止。请使用该设备对应的固件。';
 if(/headroom|exceeds[^\n]*OTA|insufficient[^\n]*OTA/i.test(text))
  return '设备的 OTA 分区空间不足，当前无法通过普通升级安装此固件。';
 if(/pending validation|pendingVerify|pending verification/i.test(text))
  return '设备尚在确认上次升级结果，请等待启动确认完成。';
 if(/appearance asset transfer is active|another firmware transfer is active/i.test(text))
  return '设备仍在处理已有同步或升级任务，请先确认该任务状态。';
 return '固件升级未完成。请保持 USB 连接，检查设备状态后再操作。';
}
