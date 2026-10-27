import test from "node:test";
import assert from "node:assert/strict";
import {
  compareVersions,
  versionState,
  readFavorites,
  updateFavorites,
  publicationStatus,
  COMMUNITY_CATEGORIES,
  COMMUNITY_TABS,
} from "./community-model.js";
test('my components leads community navigation',()=>{
  assert.deepEqual(COMMUNITY_TABS.slice(0,2),[['local','我的组件'],['discover','社区广场']]);
});
test("Hachimi collection is a real publication and discovery category", () => {
  assert.deepEqual(COMMUNITY_CATEGORIES[1], ["hachimi", "哈基米专属"]);
  assert.equal(new Set(COMMUNITY_CATEGORIES.map(([id]) => id)).size, COMMUNITY_CATEGORIES.length);
});
test("community separates published, local and verified device versions", () => {
  assert.equal(compareVersions("1.10.0", "1.9.0"), 1);
  assert.equal(compareVersions("unknown", "1.0.0"), null);
  const state = versionState(
    { id: "x", version: "2.0.0" },
    { version: "1.0.0" },
    { freshness: "live", items: [{ id: "x" }] },
  );
  assert.equal(state.updateAvailable, true);
  assert.equal(state.deviceLabel, "设备 版本未核验");
  assert.equal(versionState({ id: "x" }, null, {}).deviceLabel, "设备未核对");
});
test("favorites retain public metadata only and tolerate corrupt storage", () => {
  const result = updateFavorites([], {
    id: "demo",
    name: "test",
    path: "/private",
    token: "secret",
  });
  assert.equal(result[0].path, undefined);
  assert.equal(result[0].token, undefined);
  assert.deepEqual(updateFavorites(result, result[0]), []);
  assert.deepEqual(readFavorites({ getItem: () => "not json" }), []);
  assert.deepEqual(readFavorites({ getItem: () => '[{"id":"../bad"}]' }), []);
});
test("pending and rejected changes are not shown as published", () => {
  assert.equal(
    publicationStatus({ status: "pending", published: { available: true } }),
    "待审核",
  );
  assert.equal(publicationStatus({ status: "rejected" }), "需修改");
});
