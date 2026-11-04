import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import vm from "node:vm";
import { parseSync } from "@babel/core";
import traverseModule from "@babel/traverse";
const source = readFileSync(new URL("../ComponentCenter.jsx", import.meta.url), "utf8");
const ast = parseSync(source, { sourceType: "module", parserOpts: { plugins: ["jsx"] } });
const traverse = traverseModule.default || traverseModule;
function actual(name, context={}) {
 let declaration;
 traverse(ast,{ FunctionDeclaration(path){if(path.node.id?.name===name)declaration=path.node;} });
 assert.ok(declaration,name);
 return vm.runInNewContext(`(${source.slice(declaration.start,declaration.end)})`,context);
}
const check=actual("gameInstallBlockedReason");
test("optional touch state never prohibits installing a known package",()=>{
 const component={scene:{engine:"p4-touch-slice-v1",revision:2},sceneEngine:"p4-touch-slice-v1",runtimeEngine:"p4-bounded-runtime-v4"};
 for(const ready of [false,true,undefined,"true"]){assert.equal(check(component,{capabilities:{widgetSliceRevision:2,touchInput:{ready}}}),"");}
 assert.equal(check(component,undefined),"");
});
const optionalDeclarations=[
 {appRuntime:{host_context:{}}},{appRuntime:{navigation:{}}},{appRuntime:{form:{engine:"p4-form-v3"}}},
 {pcExtension:{discovery:{autoPlayOnEnter:true}}},{inputPolicy:{engine:"p4-input-v1"}},
 {appRuntime:{lifecycle:{pause_action:"pause"}}},{appRuntime:{surface:{canvas:{engine:"p4-canvas-v2"}}}},
 {mediaSurface:{viewport:{}}},{mediaSurface:{viewports:{}}},{mediaSurface:{fonts:[{cell:[32,48]}]}},
 {mediaSurface:{input:"p4-media-touch-v1"}},{mediaSurface:{images:[{frame_width:480}]}},
 {appRuntime:{game:{vertical_speed_percent:50}}},{mediaSurface:{game_view:{}}},{mediaSurface:{scene_view:{}}},
 {mediaSurface:{data_view:{}}},{mediaSurface:{engine:"p4-app-surface-v1",graphics:"p4-app-graphics-v1"}},
 {mediaSurface:{bindings:{}}},{appRuntime:{scene:{engine:"p4-side-scene-v1"}}},
 {mediaSurface:{typography:{},interaction:{}}},{appRuntime:{persist:{},host:{}}},
 {instrumentSource:"wood"},{defaultBindings:[{action:"media.lyrics"}]},{pcExtension:{}},{mediaSource:"audio.player"},{dataSource:"generic"},
 {runtimeEngine:"p4-bounded-runtime-v4",sceneEngine:"p4-scene-v1",gamePreset:"legacy"},
];
test("optional declarations do not become client installation vetoes",()=>{
 for(const c of optionalDeclarations){assert.equal(check(c,{}),"");assert.equal(check(c,{capabilities:{}}),"");}
});
function syncFixture(overrides={}) {
 const events=[],syncInFlight={current:false};let installs=0;
 const ctx={
  syncInFlight, usb:{connected:true,capabilities:{touchInput:{ready:false}}},
  bindingConflictForComponent:()=>"",localInstallBlockedReason:actual("localInstallBlockedReason"),
  gameInstallBlockedReason:check,liveInventoryTarget:{transport:"usb",boardDeviceId:"synthetic-board"},
  push:e=>events.push(e),devicePackageState:()=>({key:"current"}),setDowngradeComponent:()=>events.push({downgrade:true}),
  deviceInventory:{freshness:"live",supportsMultiple:true,maxInstalled:33,items:[]},installedIds:new Set(),
  installSelectedComponent:async()=>{installs++;},invoke:()=>{throw Error("unexpected touch query");},...overrides,
 };
 return {events,syncInFlight,run:actual("syncSelectedComponent",ctx),installs:()=>installs};
}
test("actual install entry reaches one native install despite absent readiness, with no touch query",async()=>{
 const f=syncFixture();await f.run({id:"synthetic-component",scene:{engine:"p4-touch-slice-v1"}});assert.equal(f.installs(),1);assert.equal(f.events.length,0);assert.equal(f.syncInFlight.current,false);
});
test("actual install entry preserves local package validation",async()=>{
 const f=syncFixture();await f.run({id:"synthetic-component",valid:false,validationErrors:["invalid schema"]});assert.equal(f.installs(),0);assert.match(f.events[0].message,/invalid schema/);
});
test("actual install entry preserves connection boundary",async()=>{
 const f=syncFixture({liveInventoryTarget:null});await f.run({id:"synthetic-component"});assert.equal(f.installs(),0);assert.match(f.events[0].title,/尚未连接/);
});
test("actual install entry preserves capacity boundary",async()=>{
 const f=syncFixture({deviceInventory:{freshness:"live",supportsMultiple:true,maxInstalled:1,items:[{}]}});await f.run({id:"synthetic-component"});assert.equal(f.installs(),0);assert.match(f.events[0].title,/已满/);
});
test("actual install entry preserves pending-transfer boundary",async()=>{
 const f=syncFixture({syncInFlight:{current:true}});await f.run({id:"synthetic-component"});assert.equal(f.installs(),0);
});
test("actual install failure is reported once without automatic retry",async()=>{
 let calls=0;const f=syncFixture({installSelectedComponent:async()=>{calls++;throw Error("synthetic transport unknown");}});await f.run({id:"synthetic-component"});assert.equal(calls,1);assert.equal(f.syncInFlight.current,false);assert.match(f.events[0].message,/transport unknown/);
});
function conflict(bindings,exitEvents=[]){return actual("bindingConflictForComponent",{
 usb:{connected:true,runtime:"esp-p4",boardDeviceId:"synthetic-board",capabilities:{touchInput:{ready:false}}},
 ...(source.includes("function deviceTouchReady(")?{deviceTouchReady:actual("deviceTouchReady")} : {}),
 globalExitBinding:{boardDeviceId:"synthetic-board",status:"ready"},globalExitEvents:exitEvents,
 resolveComponentBindings:()=>bindings,componentInputEventSlots:event=>[event],
})({});}
test("actual screen binding is not refused due to readiness",()=>{assert.equal(conflict([{label:"action",event:"screen.tap",controlLabel:"touch"}]),"");});
test("actual global exit and duplicate binding conflicts are retained",()=>{
 const b={label:"action",event:"screen.tap",controlLabel:"touch"};assert.match(conflict([b],["screen.tap"]),/全局退出/);assert.match(conflict([b,{...b,label:"other"}]),/都绑定/);
});
test("no implicit touch-refresh hook or forced query remains in component installation UI",()=>{
 assert.equal(source.includes("touchRefresh: true"),false);assert.equal(source.includes("previewTouchCheck"),false);assert.equal(source.includes("deviceTouchReady"),false);
});
test("ComponentCenter has no unresolved references across function scopes",()=>{
 const globals=new Set([...Object.getOwnPropertyNames(globalThis),"window","document","navigator","localStorage"]);const unknown=new Set();
 traverse(ast,{ReferencedIdentifier(path){if(!path.scope.hasBinding(path.node.name)&&!globals.has(path.node.name))unknown.add(path.node.name);}});assert.deepEqual([...unknown],[]);
});
test("the app isolates ComponentCenter itself, including modal prop evaluation",()=>{
 const app=readFileSync(new URL("../App.jsx",import.meta.url),"utf8");assert.match(app,/<ComponentCenterBoundary><ComponentCenter active=\{view === "components"\}\s*\/><\/ComponentCenterBoundary>/);
});
