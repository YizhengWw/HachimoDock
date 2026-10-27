// Format reviewed editorial samples for human approval; no network or live corpus writes.
import fs from "node:fs/promises";
import path from "node:path";
const [contentDir, outputDir] = process.argv.slice(2);
if (!contentDir || !outputDir) throw Error("content and output paths required");
const original=JSON.parse(await fs.readFile(path.join(contentDir,"catalog.json"),"utf8"));
const edits=JSON.parse(await fs.readFile(path.join(contentDir,"editorial-sample.json"),"utf8"));
if(edits.length!==10||new Set(edits.map(c=>c.id)).size!==10)throw Error("Expected ten distinct samples");
const cards=edits.map(edit=>{
  const source=original.find(c=>c.id===edit.id);
  const han=(edit.text.match(/[\u3400-\u9fff]/g)||[]).length;
  if(!source||han<300||han>550||!edit.title.endsWith("？")||Buffer.byteLength(edit.title)>120)throw Error("Invalid sample");
  return {...source,...edit};
});
if(new Set(cards.map(c=>c.category)).size!==10)throw Error("All categories required");
const text=["每日新知 · 十类试读稿",
  "这是10条人工精修样稿，不是1000条已全部改写完成。正文按原知识条目改写，原来源与 CC BY-SA 4.0 许可保留；标题与讲述结构已修改。",
  ...cards.map((c,i)=>`${i+1}. ${c.category}｜${c.title}\n${c.text}\n来源：${c.source}\n${c.url}\n许可：${c.license}`),
  "补充事实核对（不参与朗读）：",
  "汗液与散热：https://my.clevelandclinic.org/health/body/sweat",
  "月相并非日常被地影遮挡：https://science.nasa.gov/moon/moon-phases/",
  "罗塞塔石碑的不同书写形式：https://www.britishmuseum.org/learn/schools/ages-7-11/ancient-egypt/examine-rosetta-stone",
  "模型试写：10次请求；输入6490 tokens，输出22705 tokens，总计29195 tokens。7次输出在推理阶段耗尽预算，3条返回草稿，其中1条不足字数。之后未追加付费重试，十条由主助手重新编写与精修。货币费用以服务商账单为准。"
].join("\n\n")+"\n";
await fs.writeFile(path.join(outputDir,"每日新知-10条试读稿.txt"),text,{flag:"wx"});
await fs.writeFile(path.join(outputDir,"reviewed-sample.json"),JSON.stringify(cards,null,2)+"\n",{flag:"wx"});
console.log(JSON.stringify({count:cards.length,minHan:Math.min(...cards.map(c=>(c.text.match(/[\u3400-\u9fff]/g)||[]).length)),maxHan:Math.max(...cards.map(c=>(c.text.match(/[\u3400-\u9fff]/g)||[]).length))}));
