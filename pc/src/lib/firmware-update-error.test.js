import test from 'node:test';
import assert from 'node:assert/strict';
import {firmwareUpdateFailureMessage as format} from './firmware-update-error.js';
test('begin timeout stays unconfirmed and transport failure is distinguished',()=>{
 assert.match(format('firmware begin failed: firmware OTA acknowledgement timed out at begin; no replay'),/准备升级.*未收到设备确认/);
 assert.match(format(new Error('Broken pipe')),/USB 连接中断.*尚未确认/);
 assert.match(format('firmware chip revision mismatch'),/芯片版本不匹配/);
 assert.match(format('insufficient OTA headroom'),/分区空间不足/);
});
test('unknown native failures do not expose private identifiers or promise retry/success',()=>{
 const message=format('native-failure /Users/private/profile Bearer private-token board-private');
 assert.doesNotMatch(message,/private|Bearer|成功|自动重试/);
});
