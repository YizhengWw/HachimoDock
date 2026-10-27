import {editImageBackgroundWithArk} from './providers/volcano-image.js';
import {processImageForPipeline} from './image-processing.js';
import {bytesToDataUrl,uint8ToBase64} from './image.js';
import {checkpointedReview} from './quality.js';

export const ACTION_START_POSES=Object.freeze({
 welcome:'面向观众，身体略向左转，一只空闲手或对应肢体已经抬到肩部准备招手，露出欢迎笑容',
 'idle.playing':'身体朝左侧倾斜，微微蹲低，双脚前后错开，空闲肢体展开，呈即将侧跳玩耍的姿势',
 'idle.wandering':'身体转向画面右侧约四十五度，一只脚向前迈出，头向右侧张望，呈开始散步的姿势',
 working:'身体略向前倾，视线向左下方集中，空闲肢体靠近自身身体，呈认真思考的姿势，不添加桌子或工具',
 waiting_user:'头向右歪，面向观众，空闲手掌或对应肢体朝上摊开，表情期待，呈等待用户回答的姿势',
 done:'身体挺起、重心偏向一侧，空闲手臂或对应肢体举高庆祝，露出开心笑容',
 error:'肩部稍缩、头向左下低垂，身体略向后仰，空闲肢体低位摊开，露出轻微困惑表情',
 'touch.lick':'身体稍向前倾但保持全身可见，面带亲近温柔笑容，空闲肢体放在身体后侧',
 'touch.what':'头向左侧明显歪斜、眉毛抬起，身体向右倾，空闲肢体靠近脸侧，呈好奇观察姿势',
 'idle.default':'身体轻松站立，双肩自然下沉，空闲肢体垂在身侧，重心落在一侧，平静柔和表情',
 'idle.jumping':'双膝明显弯曲、身体蹲低，空闲肢体收起准备起跳，头保持抬起，全身和原有配饰均可见',
});

export function buildActionStartPrompt(family){
 const pose=ACTION_START_POSES[family];if(!pose)throw Error('动作缺少独立首帧设计：'+family);
 return '根据参考图绘制同一个角色的一张全新动作起始关键帧，不是视频或多格拼图。当前动作：'+family+'。起始姿势：'+pose+'。必须让身体轮廓、朝向、肢体位置和表情明显符合该动作，不能简单复制参考图的站姿。角色身份、脸部特征、发型、肤色、头身比例、画风、材质、服装和原有随身物件严格不变；只改变姿势和表情。动物或没有手脚的角色请用其实际拥有的爪、翼、耳、尾或机械部件表达同等姿态，禁止凭空添加肢体。原有球拍、乐器等物件必须保持，动作使用空闲肢体，不添加任何新物件、文字、第二角色或特效。完整全身和配饰居中并留足安全边距，固定镜头和尺度，纯黑背景。只输出单张图片。';
}

export async function ensureActionStartFrame({owner,actions,family,reference,cloudBackground,maxDimension,thinking,signal,commit,retryReview=false,generate=editImageBackgroundWithArk,normalize}){
 const identityUrl=reference.master?.bytes?.byteLength?bytesToDataUrl(reference.master.bytes,reference.master.mime):reference.dataUrl;
 if(owner.startFrameApproved&&owner.startFrame?.bytes?.byteLength)return owner.startFrame;
 if(!owner.startFrame){
  if(!owner.startFrameRaw){
   if(owner.startFrameIntent)throw Error('该动作首帧上次提交结果未知，已停止自动重试以避免重复收费；已有动画已保留。');
   if(!cloudBackground?.apiKey)throw Error('生成独立动作首帧需要已配置的图片处理服务');
   owner.startFrameIntent=true;await commit({});
   const result=await generate({...cloudBackground,imageDataUrl:identityUrl,prompt:buildActionStartPrompt(family)+' 优先核对并保留参考图中实际持有的物件：持球拍的手继续明确握住完整球拍，持乐器的手继续持有乐器。让物件随身体自然改变朝向，禁止把手持物隐藏、丢弃或变为空手。只有空闲肢体用于新动作。',signal});
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
