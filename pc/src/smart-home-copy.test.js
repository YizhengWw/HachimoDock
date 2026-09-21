import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

test('smart home names MiLoCo and keeps connection copy concise', () => {
  const source = readFileSync(new URL('./SmartHome.jsx', import.meta.url), 'utf8');
  assert.match(source, /智能家居能力来自 Xiaomi MiLoCo/);
  assert.match(source, /连接账号后，即可通过实时对话或任务页面使用账号已授权的设备与场景。/);
  assert.doesNotMatch(source, /无需逐个勾选|登录凭据仅保存在本机|升级后若连接失效/);
});
