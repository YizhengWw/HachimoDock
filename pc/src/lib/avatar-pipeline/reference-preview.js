import { processImageForPipeline } from './image-processing.js';
import { bytesToDataUrl } from './image.js';
import { inspectReference } from './quality.js';

export const normalizeStyleDescription = value => String(value || '').trim().slice(0,1000);

export async function generateReferencePreview({imageFile,styleDescription,cloudBackground,maxDimension,signal}) {
  if(!imageFile)throw Error('请先选择参考图片');
  if(!cloudBackground?.apiKey)throw Error('请先在 API 配置中保存图片处理服务的 Key');
  await inspectReference(imageFile,signal);
  const result=await processImageForPipeline(imageFile,{styleDescription:normalizeStyleDescription(styleDescription),cloudBackground,maxDimension,signal});
  const bytes=result.processedBytes;
  const dataUrl=bytesToDataUrl(bytes,'image/png');
  return {bytes,mime:'image/png',base64:dataUrl.split(',')[1],dataUrl,filename:imageFile.name,master:result.masterImage};
}

// One explicit click = at most one request; ignore late results after edits/unmount.
export function createReferencePreviewController(onChange,generate=generateReferencePreview) {
  let revision=0,active=null;
  const invalidate=()=>{revision++;active?.abort();active=null;onChange({busy:false,result:null,error:''});};
  return {invalidate,async run(input){
    if(active)return;
    const token=++revision,controller=new AbortController();active=controller;
    onChange({busy:true,result:null,error:''});
    try {
      const result=await generate({...input,signal:controller.signal});
      if(token===revision&&!controller.signal.aborted)onChange({busy:false,result:{...result,source:input.imageFile,style:normalizeStyleDescription(input.styleDescription)},error:''});
    }catch(error){
      if(token===revision&&!controller.signal.aborted)onChange({busy:false,result:null,error:error?.message||'预览生成失败，请重试'});
    }finally{if(token===revision)active=null;}
  }};
}
