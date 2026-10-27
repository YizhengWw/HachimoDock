import test from "node:test";
import assert from "node:assert/strict";
import { initializeProviderConfigs, loadProviderConfig, saveProviderConfig, STORAGE_KEY_PREFIX } from "./avatar-pipeline/provider-config.js";
import { navigateTabs } from "./tab-navigation.js";
import { setExitReason, installBrowserExitProtection } from "./exit-protection.js";
import { componentRequiresPc } from "../component-center/catalog-state.js";
import { componentUiMetadata } from "../../scripts/component-ui-metadata.mjs";

test("list metadata omits corpora without mutating resource packs or controls", () => {
  const value={service:"knowledge",content:[{text:"original"}],actions:{next:"random"},autoRead:true};
  const result=componentUiMetadata(value);
  assert.deepEqual(result.content,[]);assert.equal(value.content.length,1);
  assert.deepEqual(result.actions,value.actions);assert.equal(result.autoRead,true);
});

test("native migration commits before deletion, preserves failures and prefers native values", async () => {
  const old = globalThis.window;
  globalThis.window = { __TAURI_INTERNALS__: {} };
  try {
    const entries = new Map([[`${STORAGE_KEY_PREFIX}custom`, JSON.stringify({apiKey:"synthetic-legacy"})]]);
    const storage = {getItem:key=>entries.get(key),removeItem:key=>entries.delete(key)};
    await assert.rejects(initializeProviderConfigs(storage, async command=> {
      if(command === "load_generation_provider_configs") return {};
      throw Error("vault locked");
    }), /vault locked/);
    assert.equal(entries.size,1);
    assert.match(loadProviderConfig("custom").storageError,/vault locked/);
    await assert.rejects(saveProviderConfig("custom",{apiKey:"must-not-save"}),/原配置尚未读取/);
    let writes=0;
    await initializeProviderConfigs(storage, async (command,args)=> {
      if(command === "load_generation_provider_configs") return {};
      assert.equal(entries.size,1);assert.equal(args.config.apiKey,"synthetic-legacy");writes++;
    });
    assert.equal(writes,1);assert.equal(entries.size,0);
    assert.equal(loadProviderConfig("custom").apiKey,"synthetic-legacy");
    assert.equal(loadProviderConfig("custom").storageError,"");
    entries.set(`${STORAGE_KEY_PREFIX}custom`,JSON.stringify({apiKey:"outdated"}));
    await initializeProviderConfigs(storage, async command=>{
      assert.equal(command,"load_generation_provider_configs");return {custom:{apiKey:"synthetic-new"}};
    });
    assert.equal(loadProviderConfig("custom").apiKey,"synthetic-new");assert.equal(entries.size,0);
    entries.set(`${STORAGE_KEY_PREFIX}custom`,"{malformed");
    await assert.rejects(initializeProviderConfigs(storage,async()=>({})),/原数据已保留/);
    assert.equal(entries.size,1);
  } finally { if(old===undefined) delete globalThis.window;else globalThis.window=old; }
});

test("tab arrows wrap, Home/End focus the selected tab, Tab leaves normally", () => {
  for(const [key,current,expected] of [["ArrowLeft","a","c"],["ArrowRight","c","a"],["Home","c","a"],["End","a","c"]]) {
    let selected,focused,prevented=false;
    const ids=["a","b","c"];
    navigateTabs({key,preventDefault:()=>{prevented=true;},currentTarget:{closest:()=>({querySelectorAll:()=>ids.map(id=>({focus:()=>{focused=id;}}))})}},ids,current,id=>{selected=id;});
    assert.equal(selected,expected);assert.equal(focused,expected);assert.ok(prevented);
  }
  navigateTabs({key:"Tab",preventDefault:()=>assert.fail()},["a"],"a",()=>assert.fail());
});

test("exit guard survives overlapping identical reasons and cleans up after all finish", () => {
  const old=globalThis.window;let handler,removed=false;
  globalThis.window={addEventListener:(_,fn)=>{handler=fn;},removeEventListener:(_,fn)=>{removed=fn===handler;}};
  try {
    const dispose=installBrowserExitProtection();let prevented=0;
    const event={preventDefault:()=>prevented++};
    setExitReason("a","transfer");setExitReason("b","transfer");handler(event);
    setExitReason("a","");handler(event);assert.equal(prevented,2);
    setExitReason("b","");handler(event);assert.equal(prevented,2);
    dispose();assert.ok(removed);
  } finally {setExitReason("a","");setExitReason("b","");if(old===undefined)delete globalThis.window;else globalThis.window=old;}
});

test("PC dependence badge and filter use one contract including media", () => {
  assert.equal(componentRequiresPc({}),false);
  assert.equal(componentRequiresPc({id:"token-usage"}),true);
  for(const key of ["pcApplication","pcExtension","dataSource","mediaSource"]) assert.equal(componentRequiresPc({[key]:{}}),true);
  assert.equal(componentRequiresPc({appRuntime:{host:{}}}),true);
});
