/** Recover only complete known image results. Never invent an image job query. */
import { sleep, downloadBinary } from './http.js';
const RETRYABLE_429 = new Set(['ModelAccountIpmRateLimitExceeded','ModelAccountConcurrentRequestsLimitExceeded','AccountRateLimitExceeded','ServerOverloaded','RequestBurstTooFast']);
export const IMAGE_DOWNLOAD_MAX_BYTES = 64 * 1024 * 1024;
export const IMAGE_DOWNLOAD_TOTAL_MS = 45_000;
export const IMAGE_DOWNLOAD_ATTEMPT_MS = 15_000;
const aborted = signal => { if(signal?.aborted) throw new DOMException('aborted','AbortError'); };
export function shouldRetryImagePost(error) {
  return error?.name !== 'AbortError' && error?.status === 429
    && error?.completeProviderResponse === true && RETRYABLE_429.has(error?.providerCode);
}
export function imageResultReceipt(result, model, receivedAtMs = Date.now()) {
  const url = typeof result?.url === 'string' ? result.url : '';
  const base64 = typeof result?.base64 === 'string' ? result.base64 : '';
  if(url) {
    let parsed;try{parsed=new URL(url);}catch{throw Error('图片结果链接无效；已停止，不会重新生成');}
    if(url.length>16384 || parsed.protocol!=='https:' || parsed.username || parsed.password) throw Error('图片结果链接不安全；不会重新生成');
    return {schema:1,model:String(model||''),receivedAtMs,url};
  }
  if(base64 && base64.length <= IMAGE_DOWNLOAD_MAX_BYTES) return {schema:1,model:String(model||''),receivedAtMs,base64};
  throw Error('图片服务未返回可保存的完整结果；提交结果未知，不会自动重新生成');
}
export function validateImageReceipt(receipt) {
  if(receipt?.schema!==1 || !Number.isFinite(receipt.receivedAtMs)) throw Error('已保存的图片结果记录无效；不会重新生成');
  return imageResultReceipt(receipt,receipt.model,receipt.receivedAtMs);
}
export function shouldRetryImageGet(error) {
  if(error?.name==='AbortError')return false;
  const d=error?.diagnostic;const status=error?.status??d?.status;
  if(d?.outcome==='invalid_output')return false;
  if(status>=400 && status<500 && ![408,429].includes(status))return false;
  return [408,429,500,502,503,504].includes(status) || ['timeout','transport_error'].includes(d?.outcome);
}
export async function submitImageWithKnownRejectionRetries(submit,{signal,onStage,sleepFn=sleep,totalMs=120_000}={}) {
  const deadline=performance.now()+Math.min(600_000,Math.max(1,totalMs));
  for(let attempt=0;;attempt++) {
    aborted(signal);
    const remaining=Math.floor(deadline-performance.now());
    if(remaining<=0)throw Error("图片生成已到总等待上限；结果未知，不会自动重发");
    try { return await submit(remaining); }
    catch(error) {
      aborted(signal);
      if(attempt>=2 || !shouldRetryImagePost(error))throw error;
      onStage?.('cloud_retrying',`服务明确限流，等待${2**attempt}秒后重试（${attempt+1}/2）；未重复未知请求`);
      const delay=1000*2**attempt;
      if(performance.now()+delay>=deadline)throw error;
      await sleepFn(delay,signal);
    }
  }
}
export async function downloadKnownImage(receipt,{signal,onStage,download=downloadBinary,totalMs=IMAGE_DOWNLOAD_TOTAL_MS,attemptMs=IMAGE_DOWNLOAD_ATTEMPT_MS,sleepFn=sleep}={}) {
  const saved=validateImageReceipt(receipt);if(!saved.url)throw Error('图片记录没有下载链接');
  aborted(signal);
  const total=Math.min(IMAGE_DOWNLOAD_TOTAL_MS,Math.max(1,totalMs));
  const controller=new AbortController();let deadlineReached=false;
  const relay=()=>controller.abort();signal?.addEventListener('abort',relay,{once:true});
  const timer=setTimeout(()=>{deadlineReached=true;controller.abort();},total);
  const deadline=performance.now()+total;
  try {
    for(let attempt=0;attempt<3;attempt++) {
      aborted(signal);aborted(controller.signal);
      const budget=Math.max(1,Math.min(IMAGE_DOWNLOAD_ATTEMPT_MS,attemptMs,Math.floor(deadline-performance.now())));
      onStage?.('cloud_downloading',`图片已生成并保存结果链接，正在下载（${attempt+1}/3）；失败只重取文件`);
      try { return await download(saved.url,controller.signal,{timeoutMs:budget,maxBytes:IMAGE_DOWNLOAD_MAX_BYTES}); }
      catch(error) {
        aborted(signal);
        if(deadlineReached)throw Error('图片结果下载超过45秒；链接已保留，继续时只下载、不重新生成');
        if(controller.signal.aborted || attempt===2 || !shouldRetryImageGet(error))throw error;
        const remaining=deadline-performance.now();if(remaining<=0)throw error;
        await sleepFn(Math.min(250*(attempt+1),remaining),controller.signal);
      }
    }
  } finally {clearTimeout(timer);signal?.removeEventListener('abort',relay);}
}
export async function recoverableImageEdit({resumeResult,submit,onResult,signal,onStage,download,sleepFn}) {
  aborted(signal);
  let receipt;
  if(resumeResult)receipt=validateImageReceipt(resumeResult);
  else {
    const started=performance.now();
    const timer=setInterval(()=>onStage?.('cloud_waiting',`等待图片服务完整响应 ${Math.floor((performance.now()-started)/1000)}秒；可取消，未知请求不会重发`),1000);
    try {receipt=await submitImageWithKnownRejectionRetries(submit,{signal,onStage,sleepFn});}
    finally {clearInterval(timer);}
    receipt=validateImageReceipt(receipt);
  }
  aborted(signal);
  // Durable callback completes before the first GET, including explicit resume.
  await onResult?.(receipt);
  aborted(signal);
  if(receipt.base64)return {receipt,base64:receipt.base64};
  return {receipt,bytes:await downloadKnownImage(receipt,{signal,onStage,download,sleepFn})};
}
