// Reuse approved/checkpointed public-content work. This program makes no requests.
import fs from "node:fs";
import path from "node:path";
import {fileURLToPath} from "node:url";
import {inspectDraft} from "./rewrite-knowledge-corpus.mjs";
const pc=path.resolve(path.dirname(fileURLToPath(import.meta.url)),"..");
const [builds,packageRoot,reportFile]=process.argv.slice(2);
if(!builds||!packageRoot||!reportFile)throw Error("builds packageRoot reportFile required");
const read=p=>JSON.parse(fs.readFileSync(p,"utf8"));
const catalog=read(path.join(pc,"content/knowledge/catalog.json"));
const approved=new Map([...read(path.join(pc,"content/knowledge/editorial-sample.json")),...read(path.join(pc,"content/knowledge/editorial-final-four.json"))].map(x=>[x.id,x]));
const counts={},lengths=[],ids=new Set(),bodies=new Set(),provenance={};
const content=catalog.map(source=>{
  let draft=approved.get(source.id),origin="approved-or-final";
  for(const name of ["repaired","reviewed","full"]){
    if(draft)break;
    const file=path.join(builds,`knowledge-editorial-${name}-20261020`,`${source.id}.result.json`);
    if(fs.existsSync(file)){const d=read(file);if(!inspectDraft(d,d.finishReason).errors.length){draft=d;origin=name;}}
  }
  const checked=inspectDraft(draft);
  if(checked.errors.length)throw Error(`${source.id}: ${checked.errors.join(",")}`);
  if(ids.has(source.id)||bodies.has(checked.text))throw Error("Duplicate article");
  ids.add(source.id);bodies.add(checked.text);lengths.push(checked.han);
  counts[source.category]=(counts[source.category]||0)+1;
  provenance[origin]=(provenance[origin]||0)+1;
  return {...source,title:checked.title,text:checked.text};
});
if(content.length!==1000||Object.keys(counts).length!==10||Object.values(counts).some(n=>n!==100))throw Error("Incomplete corpus");
const appFile=path.join(packageRoot,"pc/application.json"),app=read(appFile);
app.content=content;
const encoded=JSON.stringify(app);
if(Buffer.byteLength(encoded)>4*1024*1024)throw Error("Host content exceeds limit");
fs.writeFileSync(appFile,encoded+"\n");
fs.writeFileSync(reportFile,JSON.stringify({count:content.length,categories:counts,minimumHan:Math.min(...lengths),maximumHan:Math.max(...lengths),provenance,networkRequests:0,bytes:Buffer.byteLength(encoded)},null,2)+"\n");
console.log(fs.readFileSync(reportFile,"utf8"));
