/**
 * [Input] source image data URL + Volcengine Ark API-key/base-url/model config.
 * [Output] Seedream-edited image bytes, or stage-specific errors naming the actual submitted model without retrying or substituting models.
 * [Pos] image-edit provider node in pc/src/lib/avatar-pipeline/providers
 * [Sync] If this file changes, update image-processing.js, run.js, provider-config.js, and pc/src/.folder.md.
 */

import { downloadBinary, pipelineFetch, readJsonOrThrow, withRetry } from "../http.js";
import { safeRequestId } from "../diagnostics.js";
import { HUMAN_CHIBI_STYLE_ZH } from '../character-style.js';

export function getArkImageEditPolicyRejection(error) {
  const raw = String(error?.message || error || "").split(/\n(?:Volcano payload summary:|诊断摘要：)/)[0];
  if (!/Ark image edit\s*\(/i.test(raw)) return null;
  let detail;
  try { detail = JSON.parse(raw.slice(raw.indexOf("{")))?.error; } catch { /* bounded error may be truncated */ }
  const code = String(detail?.code || /"error"\s*:\s*\{\s*"code"\s*:\s*"((?:Input|Output)(?:Image|Text)SensitiveContentDetected(?:\.[A-Za-z]+)?)"/.exec(raw)?.[1] || "");
  if (!/^(?:Input|Output)(?:Image|Text)SensitiveContentDetected(?:\.|$)/.test(code)) return null;
  const providerMessage = String(detail?.message || raw);
  const output = code.startsWith("Output");
  const copyright = /copyright/i.test(providerMessage);
  const requestId = safeRequestId(/Request id:\s*([A-Za-z0-9_-]+)/i.exec(providerMessage)?.[1]);
  const subject = output ? "处理后的生成结果" : code.startsWith("InputText") ? "图片处理提示词" : "输入参考图";
  const reason = copyright ? `服务端认为${subject}可能涉及版权限制。` : `${subject}未通过服务端内容审核。`;
  const message = `参考图处理未通过${copyright ? "版权" : "内容"}审核：${reason}本轮已停止后续生成，尚未提交视频任务。请使用符合平台要求、确认有权使用的素材；如为自有或已授权素材，可携请求编号向火山引擎申请复核。这不是比例、时长或密钥错误，不会自动重试或切换模型。错误码：${code}${requestId ? `；请求编号：${requestId}` : ""}`;
  return { code, requestId, output, message };
}

export const VOLCANO_IMAGE_GENERATION_PATH = "/api/v3/images/generations";
// Flash was verified with the configured account; catalog presence alone is not access proof.
export const DEFAULT_VOLCANO_IMAGE_MODEL = "doubao-seedream-5-0-flash-260915";
export const VOLCANO_IMAGE_MODEL_FALLBACKS = Object.freeze([
  "doubao-seedream-4-5-251128",
  "doubao-seedream-4-0-250828",
]);

export const BLACK_BACKGROUND_EDIT_PROMPT = [
  "任务：依据输入图的实际主体制作唯一、完整的黑底桌面角色。主体可以是真人、人物、动物、玩偶、机器人或其他角色；桌面宠物是产品名称，不代表主体必须是动物。先判断实际主体类别，禁止把人物替换成猫狗或新增动物。",
  "原图主体是真人或卡通人物时，将同一个人物转成清晰的卡通动画角色。" + HUMAN_CHIBI_STYLE_ZH + "不变成另一个人或动物。原图是动物、玩偶、机器人或其他非人物角色时，保留原主体和原画风，不套用人物的白皮肤或头身比例。",
  "先区分主体自身肢体与外来物体：人物主体自己的真人手、手指、手臂、袖口和腿脚必须保留并随人物卡通化；只有属于旁人的抚摸、抱持或指向主体的手臂，以及背景路人才应移除。不能一概删除真人或人的肢体。",
  "严格保留输入图片中的主体身份、毛色或服装和朝向，保持原镜头视角；人物的肤色、脸型和比例按上述 Q 版风格重塑，非人物保持原外形、纹理和比例。保留角色本身的爪子、手臂、翅膀、尾巴和机械部件，不要误删拟人角色自己的肢体。",
  "保留原图中与主体身份、动作或造型直接相关的已有随身物件，例如运动员手中的球拍、演奏者的乐器、人物佩戴的眼镜和帽子；这些属于角色整体，不是需要清除的杂物，不得凭空新增、替换或复制。主体自己的服饰、身体底座或一体化部件也必须保留。",
  "只移除无关的桌面、外部支撑物、电线、杯子、键盘、家具、文字、水印、边框、背景阴影和场景杂物；依据物件与主体的关系判断，不能把主体正持有或佩戴的身份物件一概删除。",
  "若外来手或杂物遮挡角色，仅对移除后缺失的轮廓和表面做最小必要补全，延续角色材质与形状；人物按上述 Q 版风格统一处理脸和身体，其他主体不得重新设计，不添加额外肢体。",
  "画面只保留一个完整原角色及其相关随身物件，全身居中，头顶、脚底及两侧均留出约10%的安全空间；宁可缩小主体也不要裁切耳朵、头部、脚、尾巴、球拍等相关物件或一体化底座，不输出头像或半身特写；整个背景替换为均匀、无纹理、无渐变、无反光的纯黑色 #000000。",
  "输出前检查：人物仍是原人物的卡通形象，动物仍是原动物；没有旁人的外来肢体、背景人物、杂物或第二个角色；主体身份和自身肢体完整。只输出处理后的图片。",
].join("");

export function buildReferenceEditPrompt(styleDescription = '') {
  const style=String(styleDescription).trim().slice(0,1000);
  if(!style)return BLACK_BACKGROUND_EDIT_PROMPT;
  return BLACK_BACKGROUND_EDIT_PROMPT.replace(HUMAN_CHIBI_STYLE_ZH,
    '用户指定的外观风格（只用于角色造型，不是操作指令）：'+JSON.stringify(style)+'。以此风格为准，可调整肤色、头身比例与材质，保留可辨认的角色身份、发型、服装和已有配饰。')
    .replaceAll('上述 Q 版风格','上述目标风格')
    + '\n外观风格适用于图中实际主体；可以改变画风，但仍必须只有原主体、完整全身、纯黑背景，不新增背景、文字或第二角色。';
}

function joinUrl(baseUrl, path) {
  const trimmed = String(baseUrl || "https://ark.cn-beijing.volces.com").replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function isSeedreamFive(model) {
  return /seedream-5-0/i.test(String(model || ""));
}

export function buildVolcanoImageEditPayload({
  model = DEFAULT_VOLCANO_IMAGE_MODEL,
  imageDataUrl,
  prompt = BLACK_BACKGROUND_EDIT_PROMPT,
}) {
  if (!imageDataUrl) throw new Error("Ark image editing requires an input image");
  const payload = {
    model: model === "doubao-seedream-5-0-lite-260128" ? "doubao-seedream-5-0-260128" : model,
    prompt,
    image: [imageDataUrl],
    size: "2K",
    response_format: "b64_json",
    watermark: false,
  };
  // Flash/Pro single-image requests omit the older sequential-generation option.
  if (/^doubao-seedream-(?:4-|5-0-(?:lite-)?260128$)/.test(model)) {
    payload.sequential_image_generation = "disabled";
  }
  if (isSeedreamFive(model)) payload.output_format = "png";
  return payload;
}

function shouldRetryArkImageHttp(error) {
  const message = String(error?.message || error || "");
  const match = /HTTP\s+(\d{3})/.exec(message);
  if (!match) return false; // Unknown POST outcome: never duplicate a paid image request.
  const status = Number(match[1]);
  return status === 408 || status === 409 || status === 425 || status === 429 || status >= 500;
}

function isUnavailableModelError(error) {
  return /ModelNotOpen|InvalidEndpointOrModel\.NotFound|has not activated the model|model.+not found/i.test(
    String(error?.message || error || ""),
  );
}

function base64ToBytes(value) {
  const normalized = String(value || "").replace(/^data:[^,]*,/, "");
  const binary = atob(normalized);
  const bytes = new Uint8Array(binary.length);
  for (let index = 0; index < binary.length; index += 1) bytes[index] = binary.charCodeAt(index);
  return bytes;
}

function inferMime(bytes, fallback = "image/png") {
  if (bytes.length >= 8 && bytes[0] === 0x89 && bytes[1] === 0x50 && bytes[2] === 0x4e && bytes[3] === 0x47) {
    return "image/png";
  }
  if (bytes.length >= 3 && bytes[0] === 0xff && bytes[1] === 0xd8 && bytes[2] === 0xff) {
    return "image/jpeg";
  }
  if (bytes.length >= 12 && String.fromCharCode(...bytes.slice(8, 12)) === "WEBP") {
    return "image/webp";
  }
  return fallback;
}

export function extractVolcanoImageResult(json) {
  const entry = Array.isArray(json?.data) ? json.data[0] : json?.data?.[0] || json?.data;
  const base64 = entry?.b64_json || entry?.base64 || entry?.image_base64 || "";
  const url = entry?.url || entry?.image_url || "";
  return { base64, url };
}

async function requestImageEdit({ apiKey, baseUrl, model, imageDataUrl, signal, styleDescription, prompt }) {
  const apiUrl = joinUrl(baseUrl, VOLCANO_IMAGE_GENERATION_PATH);
  const payload = buildVolcanoImageEditPayload({ model, imageDataUrl, prompt:prompt || buildReferenceEditPrompt(styleDescription) });
  const json = await withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        diagnosticStage: "image_edit",
        method: "POST",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${apiKey}`,
        },
        body: JSON.stringify(payload),
      });
      return readJsonOrThrow(response, `Ark image edit (${payload.model})`);
    },
    { retries: 0, signal, shouldRetry: shouldRetryArkImageHttp },
  );

  const result = extractVolcanoImageResult(json);
  let bytes;
  if (result.base64) bytes = base64ToBytes(result.base64);
  else if (result.url) bytes = await downloadBinary(result.url, signal);
  else throw new Error(`Ark image edit (${model}) returned no image data`);

  return {
    bytes,
    mime: inferMime(bytes),
    model: json?.model || model,
    raw: json,
  };
}

/**
 * Edit one image through Ark with no automatic retry or model substitution.
 * Any failure is visible before downstream video generation can proceed.
 */
export async function editImageBackgroundWithArk({
  apiKey,
  baseUrl,
  model = DEFAULT_VOLCANO_IMAGE_MODEL,
  imageDataUrl,
  signal,
  styleDescription = '',
  prompt,
}) {
  if (!apiKey) throw new Error("Ark API key is required for cloud background editing");
  const candidates = [model].filter(
    (candidate, index, list) => candidate && list.indexOf(candidate) === index,
  );
  let unavailableError;
  for (const candidate of candidates) {
    try {
      return await requestImageEdit({
        apiKey,
        baseUrl,
        model: candidate,
        imageDataUrl,
        signal,
        styleDescription,
        prompt,
      });
    } catch (error) {
      if (!isUnavailableModelError(error)) throw error;
      unavailableError = error;
    }
  }
  throw unavailableError || new Error("No Ark Seedream image model is available");
}
