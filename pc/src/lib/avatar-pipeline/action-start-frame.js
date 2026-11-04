import {SUBJECT_MOTION_POLICY_ZH} from './motion-semantics.js';
import {editImageBackgroundWithArk} from './providers/volcano-image.js';
import {processImageForPipeline} from './image-processing.js';
import {bytesToDataUrl,uint8ToBase64} from './image.js';
import {checkpointedReview} from './quality.js';

export const ACTION_START_POSES=Object.freeze({
 "welcome": "清晰友善的迎接起势，可轻微倾斜、舒展、摆动或起伏，让人读出欢迎和关注",
 "idle.playing": "活泼轻快的玩耍准备姿态，可整体倾斜、压缩、旋转或准备一次小幅弹动",
 "idle.wandering": "悠闲探索的起势，可整体缓慢转向或轻微游移，让人读出观察和漫游",
 "working": "注意力集中的准备状态，以收敛的姿态或有规律的小幅自身运动表达专注",
 "waiting_user": "温和停顿并期待回应的姿态，以节奏、倾斜或自身可动部分表达询问和等待",
 "done": "轻快舒展或上扬的完成姿态，以整体变化或已有部件表达认可和庆祝",
 "error": "短暂迟疑或轻微回缩的姿态，以动作节奏和自身变化表达困惑或抱歉",
 "touch.lick": "温柔亲近的回应姿态，可整体略向观众靠近或轻轻摆动，保持原有完整外形",
 "touch.what": "好奇探寻的回应姿态，可整体轻倾、轻转或适度形变，让人读出观察和疑问",
 "idle.default": "安静松弛的自然起势，以原有外形的轻微节奏变化表达待机，不要求眼睛或呼吸器官",
 "idle.jumping": "小幅弹起前的蓄势，可整体压缩或降低位置，不要求膝盖、脚或任何关节"
});

export function buildActionStartPrompt(family){
 const pose=ACTION_START_POSES[family];if(!pose)throw Error('动作缺少独立首帧设计：'+family);
 return '根据参考图绘制同一个角色的一张动作起始关键帧，不是视频或多格拼图。当前动作：'+family+'。动作语义与起势建议：'+pose+'。'+SUBJECT_MOTION_POLICY_ZH+' 起势应有可辨认的状态含义，不能简单复制参考图的站姿，但不要求复刻特定人体动作或朝向。角色身份、原有外形特征、画风、材质、服装和原有随身物件保持一致，允许符合角色结构的姿态和自然形变。原有物件随主体或实际存在的部件一起运动，不要求有空闲手脚。不得新增文字、第二角色或特效。完整主体和配饰居中并留足安全边距，固定镜头，纯黑背景。只输出单张图片。';
}

export async function ensureActionStartFrame({owner,actions,family,reference,cloudBackground,maxDimension,thinking,signal,commit,retryReview=false,generate=editImageBackgroundWithArk,normalize}){
 const identityUrl=reference.master?.bytes?.byteLength?bytesToDataUrl(reference.master.bytes,reference.master.mime):reference.dataUrl;
 if(owner.startFrameApproved&&owner.startFrame?.bytes?.byteLength)return owner.startFrame;
 if(!owner.startFrame){
  if(!owner.startFrameRaw){
   if(owner.startFrameIntent&&!owner.startFrameResult)throw Error('该动作首帧上次提交结果未知，已停止自动重试以避免重复收费；已有动画已保留。');
   if(!cloudBackground?.apiKey&&!owner.startFrameResult)throw Error('生成独立动作首帧需要已配置的图片处理服务');
   if(!owner.startFrameResult){owner.startFrameIntent=true;await commit({});}
   const result=await generate({...cloudBackground,imageDataUrl:identityUrl,prompt:buildActionStartPrompt(family)+' 保留参考图中已有物件及其原有连接或携带关系；有手持关系时继续持握，无手臂时沿用原有支撑或连接结构，不新增手脚。物件可以随主体整体运动，禁止把原有物件隐藏或丢弃。',signal,resumeResult:owner.startFrameResult,onResult:async result=>{owner.startFrameResult=result;await commit({});}});
   owner.startFrameRaw={bytes:result.bytes,mime:result.mime};await commit({});
  }
  const raw=owner.startFrameRaw;
  const bytes=normalize?await normalize(raw): (await processImageForPipeline(new Blob([raw.bytes],{type:raw.mime}),{maxDimension,allowOriginalFallback:true,signal})).processedBytes;
  // A provider occasionally returns the input unchanged; never call that a new pose.
  const same=(a,b)=>a?.byteLength===b?.byteLength&&a.every((v,i)=>v===b[i]);
  if(same(bytes,reference.bytes)||Object.values(actions).some(a=>a!==owner&&same(bytes,a.startFrame?.bytes)))throw Error('动作首帧与已有图片完全重复，结果已保留，未提交此动作视频。');
  owner.startFrame={bytes,mime:'image/png',base64:uint8ToBase64(bytes),dataUrl:bytesToDataUrl(bytes,'image/png'),filename:family+'.png'};await commit({});
 }
 const verdict=await checkpointedReview({owner,key:'startFrameQuality',commit,retryReview,thinking,images:[identityUrl,owner.startFrame.dataUrl],kind:'action_reference',signal,styleDescription:buildActionStartPrompt(family)});
 if(!verdict.pass)throw Error('动作首帧检查未通过：'+verdict.issues.join('；')+'。图片和已有动画已保留，未重复生成。');
 owner.startFrameApproved=true;await commit({});return owner.startFrame;
}
