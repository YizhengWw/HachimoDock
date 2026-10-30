import test from "node:test";
import assert from "node:assert/strict";
import { BUBBLE_STYLES, bubblePixelKey, bubbleChecksum, clampBubbleText,
  encodeBubbleAlpha, wrapBubbleText, wrapBubbleRuns, parseBubbleMarkdown, renderBubblePixels,
  collectBubbleTexts, startBubblePixelService } from "./bubble-pixels.js";

test("Markdown emphasis keeps real bold runs and literal code/escaped markers", () => {
  assert.deepEqual(parseBubbleMarkdown("普通 **重点**，__强调__；`**原样**`，\\*字\\*"), [
    {text:"普通 ",bold:false},{text:"重点",bold:true},{text:"，",bold:false},
    {text:"强调",bold:true},{text:"；**原样**，*字*",bold:false}]);
  assert.deepEqual(parseBubbleMarkdown("**正在接收"), [{text:"正在接收",bold:true}]);
  assert.equal(parseBubbleMarkdown("file_name foo__bar__baz 2 * 3").map(run=>run.text).join(""), "file_name foo__bar__baz 2 * 3");
  assert.equal(parseBubbleMarkdown("*斜体* ~~删除线~~").map(run=>run.text).join(""), "斜体 删除线");
  assert.deepEqual(parseBubbleMarkdown("***重点***"), [{text:"重点",bold:true}]);
});

test("small-screen Markdown preserves structure without URLs, fences or separator rows", () => {
  const text = "## 标题\n\n> 引用 **重点**\n- 项目\n- [x] 完成\n1. 顺序\n[文档](https://example.com) ![照片](a.png)\n```js\nconst s = '**literal**';\n```\n|名称|状态|\n|---|:---:|\n|USB|正常|\n---";
  assert.equal(parseBubbleMarkdown(text).map(run=>run.text).join(""),
    "标题\n引用 重点\n• 项目\n[x] 完成\n1. 顺序\n文档 照片\nconst s = '**literal**';\n名称 · 状态\nUSB · 正常");
  assert.equal(parseBubbleMarkdown("<https://example.com>").map(run=>run.text).join(""), "https://example.com");
  assert.equal(parseBubbleMarkdown("## C#\n[资料](https://a/b_(c))").map(run=>run.text).join(""), "C#\n资料");
});

test("rich wrapping measures bold ink before deciding line breaks", () => {
  const measure = (text, bold) => Array.from(text).length * (bold ? 15 : 10);
  assert.deepEqual(wrapBubbleRuns(parseBubbleMarkdown("甲**乙丙**丁"), measure, 40, 2), [
    [{text:"甲",bold:false},{text:"乙丙",bold:true}],[{text:"丁",bold:false}]]);
  const lines = wrapBubbleRuns(parseBubbleMarkdown("**abcdefghi**"), measure, 50, 1);
  assert.equal(lines[0].map(run=>run.text).join(""), "a...");
});

test("Canvas receives bold weights but the wire key still uses the original text", () => {
  const drawn = [], context = { font:"", measureText(text) { return {width:text.length*10,actualBoundingBoxAscent:20,actualBoundingBoxDescent:3}; },
    clearRect(){}, fillText(text,x){drawn.push({text,x,font:this.font});},
    getImageData(){return {data:new Uint8ClampedArray(496*54*4)};} };
  const image = renderBubblePixels(4,"普通**加粗**",()=>({getContext:()=>context}));
  assert.equal(image.id,bubblePixelKey(4,"普通**加粗**"));
  assert(drawn.some(run=>run.text==="加粗"&&run.font.startsWith("700 24px")));
  assert(drawn.some(run=>run.text==="普通"&&run.font.startsWith("500 24px")));
  assert(!drawn.some(run=>run.text.includes("**")));
});

test("wire keys preserve Unicode and separate title/body geometry", () => {
  assert.equal(clampBubbleText("AB你好😀", 8), "AB你好");
  assert.equal(clampBubbleText("😀好", 3), "");
  assert.equal(clampBubbleText("你好\0后续", 100), "你好");
  assert.equal(bubblePixelKey(2, "你好"), "41c0fcd4504ad4c3");
  assert.notEqual(bubblePixelKey(1, "你好"), bubblePixelKey(2, "你好"));
  assert.equal(bubbleChecksum([]), "811c9dc5");
});

test("alpha runs round-trip all gray levels, empty background and long runs", () => {
  const alpha = [...Array(48).fill(0), ...Array.from({length: 256}, (_, i) => i), ...Array(40).fill(255)];
  const rgba = new Uint8Array(alpha.length * 4);
  alpha.forEach((value, i) => rgba[i * 4 + 3] = value);
  const bytes = encodeBubbleAlpha(rgba), decoded = [];
  for (const byte of bytes) decoded.push(...Array((byte & 15) + 1).fill(byte >> 4));
  assert.deepEqual(decoded, alpha.map(value => Math.round(value / 17)));
  assert.equal(new Set(decoded).size, 16);
});

test("wrapping preserves newlines, emoji, exact fits and bounded ellipsis", () => {
  const measure = text => Array.from(text).length * 10;
  assert.deepEqual(wrapBubbleText("你好世界", measure, 20, 4), ["你好", "世界"]);
  assert.deepEqual(wrapBubbleText("A\n\n😀好", measure, 20, 4), ["A", "", "😀好"]);
  assert.deepEqual(wrapBubbleText("abcdefghijk", measure, 50, 1), ["ab..."]);
  assert.deepEqual(wrapBubbleText("12345", measure, 50, 1), ["12345"]);
  assert.deepEqual(wrapBubbleText("Hello world", measure, 80, 2), ["Hello ", "world"]);
});

test("all displayed sessions and spoken subtitles get independent text requests", () => {
  assert.deepEqual(collectBubbleTexts("session/current", null), []);
  assert.doesNotThrow(() => collectBubbleTexts("session/current", { sessions: [null, 3, {}] }));
  const requests = collectBubbleTexts("session/current", {sessionId:"a",sessions:[
    {id:"a",title:"当前会话",content:"正文😀"},{id:"b",title:"后台会话",state:"working"}]});
  assert(requests.some(r => r.style === 1 && r.text === "当前会话"));
  assert(requests.some(r => r.style === 4 && r.text === "working"));
  assert(collectBubbleTexts("ui/conversation", {role:"user",name:"小西",text:"你好"})
    .some(r => r.style === 1 && r.text === "你"));
  assert(collectBubbleTexts("audio/play_subtitle", {text:"当前播放字幕"})
    .some(r => r.style === 2 && r.text === "当前播放字幕"));
  assert.equal(BUBBLE_STYLES[2].lines, 2);
  const long = "字".repeat(160);
  const session = collectBubbleTexts("speech/text", {sessionId:"a",text:long});
  assert(session.some(r=>r.style===4&&new TextEncoder().encode(r.text).length===381));
});

test("service coalesces streaming updates, retries busy transfers and resets on reconnect", async () => {
  let handler, unlistened = false, failures = 1;
  const sent = [];
  const stop = await startBubblePixelService({ interval: 1,
    listen: async (_event, fn) => { handler = fn; return () => { unlistened = true; }; },
    invoke: async (command, args) => {
      if (command !== "usb_send_bubble_pixels") return;
      if (failures-- > 0) throw Error("busy");
      sent.push(args.frame);
    },
    render: (style, text) => ({style,text,id:bubblePixelKey(style,text)}),
  });
  const event = (content, connectionId = 1) => ({payload:{boardDeviceId:"board",connectionId,
    topic:"speech/text",payload:{displayTitle:"Agent",text:content}}});
  handler(event("old")); handler(event("new"));
  const waitUntil = async condition => {
    for (let i = 0; i < 100 && !condition(); i++) await new Promise(resolve => setTimeout(resolve, 3));
    assert(condition());
  };
  try {
    await waitUntil(() => sent.some(f => f.style === 4 && f.text === "new"));
    assert(!sent.some(f => f.text === "old"));
    handler(event("new", 2));
    await waitUntil(() => sent.some(f => f.style === 4 && f.text === "new" && f.connectionId === 2));
  } finally { stop(); }
  assert(unlistened);
});

test("an old in-flight reply cannot mark a reconnected device's image as sent", async () => {
  let handler, releaseOld, started;
  const firstStarted = new Promise(resolve => { started = resolve; });
  const oldReply = new Promise(resolve => { releaseOld = resolve; });
  const sent = [];
  const stop = await startBubblePixelService({ interval: 1,
    listen: async (_event, fn) => { handler = fn; return () => {}; },
    render: (style, text) => ({style,text,id:bubblePixelKey(style,text)}),
    invoke: async (command, args) => {
      if (command !== "usb_send_bubble_pixels") return;
      sent.push(args.frame);
      if (sent.length === 1) { started(); await oldReply; }
    },
  });
  const emit = connectionId => handler({payload:{topic:"speech/text",payload:{text:"新正文"},
    boardDeviceId:"board",connectionId}});
  try {
    emit(1); await firstStarted;
    emit(2); releaseOld();
    for (let i=0;i<100 && !sent.some(f=>f.connectionId===2&&f.text==="新正文");i++)
      await new Promise(resolve=>setTimeout(resolve,3));
    assert(sent.some(f=>f.connectionId===2&&f.text==="新正文"));
  } finally { releaseOld(); stop(); }
});

test("an older native client without the ready command leaves no listener behind", async () => {
  let cleaned = false;
  await assert.rejects(startBubblePixelService({
    listen: async () => () => { cleaned = true; },
    invoke: async () => { throw Error("unknown command"); },
  }));
  assert(cleaned);
});
