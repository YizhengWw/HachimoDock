import { readFileSync } from "node:fs";
import { test } from "node:test";
import assert from "node:assert/strict";
const read = (path) => readFileSync(new URL(path, import.meta.url), "utf8");
test("both build flavors use per-computer networking, never embedded proxy or credentials", () => {
  const llm = read("../src-tauri/src/persona_llm.rs");
  const network = read("../src-tauri/src/llm_network.rs");
  assert.match(llm, /crate::llm_network::client/);
  assert.doesNotMatch(llm + network, /option_env!\("PET_MANAGER_INTERNAL_(?:HTTPS_PROXY|CA_PEM)"\)/);
  assert.doesNotMatch(llm + network, /127\.0\.0\.1:7890|danger_accept_invalid_certs/);
  assert.match(network, /use_native_tls\(\)/);
  assert.match(network, /ProxyMode::Direct => builder.no_proxy\(\)/);
  assert.match(network, /Policy::none\(\)/);
  assert.match(network, /#\[cfg\(not\(feature = "internal-network"\)\)\]\s*const BUNDLED_CA: &str = ""/);
  assert.match(network, /certificates\(BUNDLED_CA\)/);
});
test("API settings exposes saved-config probes and private per-machine certificate import", () => {
  const ui = read("./LlmNetworkSettings.jsx");
  assert.match(read("./ApiSettings.jsx"), /<LlmNetworkSettings \/>/);
  for (const command of ["load_llm_network_settings", "save_llm_network_settings", "test_llm_network_settings"]) {
    assert.ok(ui.includes(command));
    assert.ok(read("../src-tauri/src/lib.rs").includes(`llm_network::${command}`));
  }
  assert.match(ui, /busy \|\| dirty/);
  assert.match(ui, /不能导入私钥/);
  assert.match(ui, /PAC/);
});

test("all product cloud routes share trust while LAN stays direct", () => {
  const lib = read("../src-tauri/src/lib.rs");
  const http = lib.slice(lib.indexOf("async fn download_bytes("), lib.indexOf("// ── Codex pet importer"));
  assert.match(http, /llm_network::download_client/);
  assert.match(http, /llm_network::client/);
  assert.doesNotMatch(http, /Client::builder|spawn_blocking|\{url\}|log_url/);
  for (const file of ["doubao_tts.rs", "volcengine_asr.rs"]) {
    const source = read(`../src-tauri/src/${file}`);
    assert.match(source, /llm_network::connect_websocket/);
    assert.doesNotMatch(source, /connect_async\(/);
  }
  const lan = lib.slice(lib.indexOf("fn lan_http_client("), lib.indexOf("fn resolve_usb_inject_agent_id("));
  assert.match(lan, /\.no_proxy\(\)/);
  assert.doesNotMatch(lan, /llm_network/);
  assert.match(read("../src-tauri/src/codex_import.rs"), /command.env\("NODE_EXTRA_CA_CERTS", file.path\(\)\)/);
});

test("cloud UI and diagnostics describe shared networking without signed URL logs", () => {
  const ui = read("./LlmNetworkSettings.jsx");
  assert.match(ui, /云服务网络/);
  for (const label of ["模型列表", "图片与视频生成", "素材下载", "语音识别与合成"]) assert.ok(ui.includes(label));
  const http = read("./lib/avatar-pipeline/http.js");
  assert.doesNotMatch(http, /console\.error\([^\n]*\$\{url\}/);
  assert.match(http, /safeError.message/);
});
