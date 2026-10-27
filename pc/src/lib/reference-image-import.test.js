import test from "node:test";
import assert from "node:assert/strict";
import { File } from "node:buffer";
import { createReferenceImportSession, referenceFileFromEvent, validateReferenceFile, MAX_REFERENCE_BYTES, visibleReferenceRect } from "./reference-image-import.js";

test("normalized images exclude source filename and accept only matching session", async () => {
  const event = { token: "t", status: "ready", mime: "image/png", name: "/private/personal.heic", base64: btoa("pixels") };
  assert.equal(referenceFileFromEvent(event, "old", File), null);
  const file = referenceFileFromEvent(event, "t", File);
  assert.equal(file.name, "reference.png"); assert.equal(await file.text(), "pixels");
  assert.throws(() => referenceFileFromEvent({ ...event, mime: "text/html" }, "t", File));
  assert.throws(() => referenceFileFromEvent({ ...event, base64: "%%%" }, "t", File));
  assert.equal(referenceFileFromEvent({ ...event, status: "cancelled" }, "t", File), null);
});
test("empty, oversize and non-image selections fail; HEIC and TIFF can enter decoding", () => {
  for (const name of ["photo.HEIC", "photo.heif", "photo.tiff", "photo.jpg"]) assert.doesNotThrow(() => validateReferenceFile({ name, size: 1, type: "" }));
  for (const file of [{name:"x.txt",size:1}, {name:"x.png",size:0}, {name:"x.png",size:MAX_REFERENCE_BYTES+1}]) assert.throws(() => validateReferenceFile(file));
  assert.throws(() => validateReferenceFile({name:"x.png",size:100,type:"image/svg+xml"}));
});
test("listen precedes native mount, events isolate sessions and unmount unsubscribes", async () => {
  const calls = [], events = []; let handler, stops = 0;
  const session = createReferenceImportSession({ token:"t", onEvent:e=>events.push(e), listen:async (_, cb)=>{handler=cb; calls.push("listen"); return ()=>stops++;}, invoke:async (cmd,args)=>{calls.push([cmd,args]); return true;} });
  await session.mount([0,0,100,100,1200]); await session.action("photos");
  handler({payload:{token:"wrong",status:"ready"}}); handler({payload:{token:"t",status:"loading"}});
  assert.equal(events.length,1); assert.equal(calls[0],"listen");
  await session.dispose(); handler({payload:{token:"t",status:"ready"}});
  assert.equal(events.length,1); assert.equal(stops,1); assert.equal(calls.at(-1)[1].action,"unmount");
  await session.mount([0,0,1,1,1]); assert.equal(calls.at(-1)[1].action,"unmount");
});
test("dispose during listener startup or in-flight mount cannot leave native overlay", async () => {
  let resolveListen, stops=0; const calls=[];
  const session = createReferenceImportSession({token:"t", onEvent:()=>{}, listen:()=>new Promise(r=>resolveListen=r), invoke:async(cmd,args)=>{calls.push([cmd,args]);return true;} });
  const mount = session.mount([0,0,10,10,20]); const end=session.dispose();
  resolveListen(()=>stops++); await Promise.all([mount,end]); assert.equal(stops,1); assert.equal(calls.length,0);
  let finishMount;
  const second = createReferenceImportSession({token:"t2",onEvent:()=>{},listen:async()=>()=>{}, invoke:async(cmd,args)=>{calls.push([cmd,args]);if(cmd==="reference_image_mount")return new Promise(r=>finishMount=r);} });
  const mounting=second.mount([0,0,10,10,20]); await new Promise(r=>setImmediate(r));
  const ending=second.dispose(); finishMount(true); await Promise.all([mounting,ending]);
  assert.equal(calls.at(-1)[1].action,"unmount");
});
test("failed native action does not prevent cleanup", async () => {
  const calls=[];
  const session=createReferenceImportSession({token:"t", onEvent:()=>{},listen:async()=>()=>{},invoke:async(cmd,args)=>{calls.push(args);if(args.action==="photos")throw Error("failed");return true;} });
  await session.mount([0,0,1,1,1]); await assert.rejects(session.action("photos")); await session.dispose(); assert.equal(calls.at(-1).action,"unmount");
});
test("native drop zone clips to scroll containers and viewport", () => {
  const original=globalThis.getComputedStyle;
  globalThis.getComputedStyle=()=>({overflowX:"hidden",overflowY:"auto"});
  try {
    const parent={getBoundingClientRect:()=>({left:10,top:30,right:200,bottom:100}),parentElement:null};
    const node={getBoundingClientRect:()=>({left:0,top:0,right:300,bottom:200}),parentElement:parent};
    assert.deepEqual(visibleReferenceRect(node,250,150),[10,30,190,70,250]);
  } finally { globalThis.getComputedStyle=original; }
});
