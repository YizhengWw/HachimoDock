/**
 * [Input] library-order.js component creation-time helpers.
 * [Output] Node regression coverage for mixed builtin/custom newest-first ordering and stable ties.
 * [Pos] test node in pc/src/component-center
 * [Sync] If this file changes, update `pc/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import {
  componentCreatedAtMs,
  sortComponentsByCreatedAt,
  mergeComponentCatalog,
  isComponentVisible,
  DEFAULT_COMPONENT_ORDER,
  sortComponentsByDefaultOrder,
} from "./library-order.js";
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";
import { readdirSync } from "node:fs";

test('promoted local-delights revisions use builtin placement, not recent-install placement',()=>{
  const center=readFileSync(new URL('../ComponentCenter.jsx',import.meta.url),'utf8');
  for(const id of ['decision-capsule','answer-book','hope-train']) {
    const root=new URL(`../../builtin-clawpkgs/${id}/`,import.meta.url);
    const list=(dir,prefix='')=>readdirSync(dir,{withFileTypes:true}).flatMap(e=>e.isDirectory()?list(new URL(`${e.name}/`,dir),`${prefix}${e.name}/`):[`${prefix}${e.name}`]);
    const hash=createHash('sha256');
    for(const file of list(root).sort()) hash.update(file).update('\0').update(readFileSync(new URL(file,root))).update('\0');
    assert.ok(center.includes(`"${hash.digest('hex').slice(0,16)}"`),`${id}: update the exact promoted revision when changing the builtin`);
  }
});

test("release hides builtin and already-installed music without deleting records", () => {
  const source = [{id:"stock-watchlist"}, {id:"music-player"}, {id:"wooden-fish"}];
  assert.deepEqual(mergeComponentCatalog([], source).map(x => x.id), ["stock-watchlist", "wooden-fish"]);
  assert.deepEqual(mergeComponentCatalog([{id:"music-player"}], source).map(x => x.id), ["stock-watchlist", "wooden-fish"]);
  assert.equal(source.length, 3);
  assert.equal(isComponentVisible({id:"custom-player", mediaSource:"audio.player"}), false);
  assert.equal(isComponentVisible({id:"upcoming-todos"}), true);
  const center=readFileSync(new URL("../ComponentCenter.jsx", import.meta.url),"utf8");
  assert.ok(center.includes("isComponentVisible(activeComponentRecord)"));
  assert.ok(center.includes("isComponentVisible(item) && !catalogIds.has(item.id)"));
});

test("PC and firmware share the curated default component order", () => {
  const factory = JSON.parse(readFileSync(new URL("../../../firmware/factory-config.json", import.meta.url)));
  const firstTen=['city-radio','cloud-hop','stock-watchlist','wooden-fish','daily-news','book-pocket','block-combo','tomato-clock','fresh-fruit-slice','hachimi-sounds','snake-turn'];
  assert.deepEqual(DEFAULT_COMPONENT_ORDER.slice(0,11),firstTen);
  assert.deepEqual(factory.components.ids,DEFAULT_COMPONENT_ORDER);
  assert.equal(new Set(DEFAULT_COMPONENT_ORDER).size,29);
  assert.deepEqual(DEFAULT_COMPONENT_ORDER.slice(-3), ['decision-capsule','answer-book','hope-train']);
  assert.equal(factory.components.defaultActiveId,'city-radio');
  assert.ok(!factory.components.ids.includes('music-player'));
  const items=[{id:'custom-b'},...firstTen.toReversed().map(id=>({id})),{id:'custom-a'}];
  const snapshot=[...items];
  assert.deepEqual(sortComponentsByDefaultOrder(items).map(x=>x.id),[...firstTen,'custom-b','custom-a']);
  assert.deepEqual(items,snapshot);
});

test("component library sorts mixed records by creation time newest first", () => {
  const sorted = sortComponentsByCreatedAt([
    { id: "builtin-old", createdAt: "2026-05-24T15:17:05+08:00" },
    { id: "draft-new", createdAtMs: Date.parse("2026-07-28T10:00:00+08:00") },
    { id: "builtin-new", createdAt: "2026-07-23T19:19:17+08:00" },
  ]);

  assert.deepEqual(sorted.map((item) => item.id), [
    "draft-new",
    "builtin-new",
    "builtin-old",
  ]);
});

test("factory copies keep curated order after loading; actual user revisions stay recent",()=>{
  const builtins=[{id:"city-radio",version:"1.0"},{id:"cloud-hop",version:"1.0"},{id:"wooden-fish",version:"1.0"}];
  const locals=[{...builtins[2],isBundledCopy:true},{...builtins[0],isBundledCopy:true},{...builtins[1],isBundledCopy:true}];
  assert.deepEqual(mergeComponentCatalog(locals,builtins).map(v=>v.id),mergeComponentCatalog([],builtins).map(v=>v.id));
  const custom={id:"wooden-fish",version:"2.0",isBundledCopy:false};
  const out=mergeComponentCatalog([custom,...locals.slice(1)],builtins);
  assert.equal(out[0].id,"wooden-fish");assert.equal(out[0].version,"2.0");assert.equal(out[0].sourceAlternatives[0],custom);
});

test("recent installs precede curated defaults without losing custom source revisions", () => {
  const stock = { id: "stock-watchlist", version: "1.1.0" };
  const builtins = [stock, { id: "pong" }, { id: "frog" }];
  const published = [{ id: "custom-new" }, { id: "stock-watchlist", version: "1.0.0" }, { id: "frog", version: "custom" }];
  const result = mergeComponentCatalog(published, builtins);
  assert.deepEqual(result.map((item) => item.id), ["custom-new", "stock-watchlist", "frog", "pong"]);
  assert.equal(result[1].version, stock.version);
  assert.deepEqual(result[1].sourceAlternatives,[published[1],stock]);
  assert.equal(result[2].version, "custom");
  assert.deepEqual(mergeComponentCatalog([], builtins), builtins);
});

test("renamed packages preserve identity and synchronize all display titles", () => {
  for(const [id,name,version]of [['wooden-fish','静心木鱼','1.0.5'],['book-pocket','每天一本书','1.0.2']]){
    const root=new URL(`../../builtin-clawpkgs/${id}/`,import.meta.url);
    const read=file=>JSON.parse(readFileSync(new URL(file,root)));
    assert.deepEqual([read('component.json').id,read('component.json').name,read('component.json').version],[id,name,version]);
    assert.equal(read('runtime/widget.json').dashboard.title,name);
    assert.equal(read('negative-screen.json').dashboard.title,name);
    assert.equal(read('share.json').title,name);
  }
});

test("component library keeps source order for equal or missing creation times", () => {
  const items = [
    { id: "first", createdAtMs: 100 },
    { id: "second", createdAtMs: 100 },
    { id: "missing-a" },
    { id: "missing-b" },
  ];

  assert.deepEqual(
    sortComponentsByCreatedAt(items).map((item) => item.id),
    items.map((item) => item.id),
  );
  assert.equal(componentCreatedAtMs({ createdAt: "not-a-date" }), 0);
});
