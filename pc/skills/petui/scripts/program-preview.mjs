#!/usr/bin/env node
// Explicit local synthetic authoring input on stdin only. Never reads user
// configuration/files, contacts a device/provider or creates a package.
import {encodeProgram,inspectProgram,createProgramPreview} from './program-core.mjs';
try{
 const chunks=[];let size=0;
 for await(const chunk of process.stdin){size+=chunk.length;if(size>65536)throw Error();chunks.push(chunk);}
 const input=new TextDecoder('utf-8',{fatal:true}).decode(Buffer.concat(chunks));
 const p=JSON.parse(input),keys=['program','events','seed','quantum','outputLimits'];
 if(!p||typeof p!=='object'||Array.isArray(p)||Object.keys(p).some(k=>!keys.includes(k))||!Object.hasOwn(p,'program')||!Array.isArray(p.events)||p.events.length<1||p.events.length>32)throw Error();
 const bytes=encodeProgram(p.program),info=inspectProgram(bytes),quantum=Object.hasOwn(p,'quantum')?p.quantum:64;
 const preview=createProgramPreview(bytes,{seed:p.seed??null,quantum,outputLimits:p.outputLimits??null}),reports=[];
 let stopped=false;
 for(const [i,event]of p.events.entries()){
  if(preview.enqueue(i+1,event)!=='ok')throw Error();
  let report;
  for(let step=0;step<Math.ceil(info.fuel/quantum)+3;step++){
   const result=preview.pump();
   if(result==='complete'){report=preview.takeReport();break;}
   if(result!=='yielded')throw Error();
  }
  if(!report)throw Error();reports.push(report);
  if(report.result!=='committed'){stopped=true;break;}
 }
 process.stdout.write(JSON.stringify({engine:'p4-program-preview-result-v1',scope:'offline_pure_logic_only',deviceExecutionEnabled:false,hardwareVerified:false,info,reports,snapshot:preview.snapshot(),stopped})+'\n');
}catch{
 process.stdout.write(JSON.stringify({error:'program_preview_input_or_execution_invalid',deviceExecutionEnabled:false})+'\n');process.exitCode=1;
}
