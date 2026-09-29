import test from "node:test";
import assert from "node:assert/strict";
import { motion, WoodenFishAudio } from "./wooden-fish.js";

test("rapid strikes continue at the current mallet position without teleporting", () => {
  let y=0,scale=1;
  for (const gap of [20,40,90,120,160,70,200,35,65]) {
    const before=motion(gap,y,scale);
    const after=motion(0,before.malletY,before.fishScale);
    assert.equal(after.malletY,before.malletY);
    assert.equal(after.fishScale,before.fishScale);
    y=after.malletY;scale=after.fishScale;
    assert(Math.abs(motion(55,y,scale).malletY-68)<.001);
  }
  assert.deepEqual(motion(900,y,scale),{malletY:0,fishScale:1,wave:1});
});

test("ambience starts on a gesture, loops once and shares the bounded voice player", async () => {
  const originalContext=globalThis.AudioContext, originalFetch=globalThis.fetch;
  const sources=[],requests=[];let contexts=0;
  globalThis.AudioContext=class {
    constructor(){contexts++;this.state="suspended";this.currentTime=0;this.destination={};}
    async resume(){this.state="running";}
    async close(){this.state="closed";}
    async decodeAudioData(data){return data;}
    createBufferSource(){const source={loop:false,started:false,stopped:false,
      connect(node){return node;},start(){this.started=true;},stop(){this.stopped=true;this.onended?.();}};sources.push(source);return source;}
    createGain(){return {gain:{setValueAtTime(){},linearRampToValueAtTime(){}},connect(node){return node;}};}
  };
  globalThis.fetch=async url=>{requests.push(url);return {ok:true,arrayBuffer:async()=>new ArrayBuffer(16)};};
  try {
    const player=new WoodenFishAudio();
    assert.equal(contexts,0,"no autoplay before user interaction");
    assert.equal(await player.strike(),true);
    assert.equal(sources.filter(s=>s.loop&&s.started).length,1);
    assert(requests.some(url=>url.includes("ambience.wav?v=4")));
    for(let i=0;i<8;i++)await player.strike();
    assert.equal(sources.filter(s=>s.loop).length,1,"do not stack background loops on every knock");
    assert.equal(player.voices.length,4);
    player.close();
    assert.equal(player.context.state,"closed");
    assert.equal(await player.strike(),false,"closed preview must not restart sound");
  } finally {globalThis.AudioContext=originalContext;globalThis.fetch=originalFetch;}
});
