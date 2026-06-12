/**
 * [Input] image payload + Volcengine Ark API-key/model-name config.
 * [Output] structured persona + per-family prompts via Ark Responses with Doubao 2.0 multimodal by default.
 * [Pos] lib node in ref/src/lib/avatar-pipeline
 * [Sync] If this file changes, update this header.
 */

import { pipelineFetch, withRetry, readJsonOrThrow } from "./http.js";
import { buildUserPrompt, parseModelJson } from "./prompts.js";
import { SYSTEM_PROMPT_ZH } from "./system-prompt.js";
import { FAMILIES } from "./families.js";

export const DEFAULT_THINKING_MODEL = "doubao-seed-2-0-pro-260215";
export const DEFAULT_THINKING_API_URL = "https://ark.cn-beijing.volces.com/api/v3/responses";
const TEXT_ONLY_THINKING_MODELS = new Set(["deepseek-v3-2-251201"]);

function joinUrl(baseUrl, path) {
  if (!baseUrl) return path;
  const trimmed = baseUrl.replace(/\/+$/, "");
  return path.startsWith("/") ? `${trimmed}${path}` : `${trimmed}/${path}`;
}

function collectResponseText(value) {
  if (!value) return [];
  if (typeof value === "string") return [value];
  if (Array.isArray(value)) return value.flatMap((item) => collectResponseText(item));
  if (typeof value !== "object") return [];
  if (typeof value.output_text === "string") return [value.output_text];
  if (typeof value.text === "string") return [value.text];
  if (typeof value.content === "string") return [value.content];
  if (Array.isArray(value.content)) return collectResponseText(value.content);
  if (Array.isArray(value.output)) return collectResponseText(value.output);
  return [];
}

function extractMessageContent(responseJson) {
  if (typeof responseJson?.output_text === "string") return responseJson.output_text;
  const outputText = collectResponseText(responseJson?.output).join("\n").trim();
  if (outputText) return outputText;
  const dataOutputText = collectResponseText(responseJson?.data?.output).join("\n").trim();
  if (dataOutputText) return dataOutputText;

  const message = responseJson?.choices?.[0]?.message;
  const content = message?.content;
  if (typeof content === "string") return content;
  if (Array.isArray(content)) {
    return content
      .map((part) => {
        if (typeof part === "string") return part;
        return part?.text || part?.content || "";
      })
      .join("\n")
      .trim();
  }
  throw new Error("Thinking model response did not contain message content");
}

function supportsImageInput(thinking) {
  if (typeof thinking?.supportsImageInput === "boolean") return thinking.supportsImageInput;
  const model = String(thinking?.model || DEFAULT_THINKING_MODEL).trim().toLowerCase();
  if (!model) return false;
  if (TEXT_ONLY_THINKING_MODELS.has(model)) return false;
  if (model.startsWith("deepseek-") || model.includes("deepseek")) return false;
  return true;
}

export function buildThinkingModelRequest({
  thinking,
  image,
  appearanceName = "",
  personality = "",
}) {
  const apiUrl =
    thinking?.apiUrlOverride ||
    (thinking?.baseUrl ? joinUrl(thinking.baseUrl, "/api/v3/responses") : DEFAULT_THINKING_API_URL);
  const userPrompt = buildUserPrompt({ families: FAMILIES, appearanceName, personality });
  const sendImage = Boolean(image?.dataUrl && supportsImageInput(thinking));
  const textOnlyNotice = sendImage
    ? ""
    : "\n\n注意：当前 Thinking 模型为纯文本模型，不支持图片输入。请基于用户填写的形象名称、性格描述和通用桌宠动画规范生成 prompt，不要声称已经看到了图片细节。";
  const inputText = `${SYSTEM_PROMPT_ZH}\n\n${userPrompt}${textOnlyNotice}`;
  const content = [];
  if (sendImage) {
    content.push({ type: "input_image", image_url: image.dataUrl });
  }
  content.push({ type: "input_text", text: inputText });
  const body = {
    model: thinking?.model || DEFAULT_THINKING_MODEL,
    input: [
      {
        role: "user",
        content,
      },
    ],
  };
  if (thinking?.maxTokens) body.max_output_tokens = thinking.maxTokens;
  if (typeof thinking?.temperature === "number") body.temperature = thinking.temperature;
  if (thinking?.reasoningEffort) body.reasoning_effort = thinking.reasoningEffort;
  return { apiUrl, body };
}

/**
 * Call the thinking model once and return parsed { persona, prompts }.
 *
 * @param {object} args
 * @param {{ apiKey: string, baseUrl?: string, model?: string, apiUrlOverride?: string, reasoningEffort?: string, maxTokens?: number, temperature?: number }} args.thinking
 * @param {{ dataUrl: string }} args.image
 * @param {string} [args.appearanceName]
 * @param {string} [args.personality]
 * @param {AbortSignal} [args.signal]
 * @returns {Promise<{ persona: object, prompts: Array<{ family: string, prompt: string, variation_notes?: string }>, raw: object }>}
 */
export async function callThinkingModel({
  thinking,
  image,
  appearanceName = "",
  personality = "",
  signal,
}) {
  if (!thinking?.apiKey) throw new Error("Missing thinking-model API key");
  const { apiUrl, body } = buildThinkingModelRequest({
    thinking,
    image,
    appearanceName,
    personality,
  });

  const responseJson = await withRetry(
    async () => {
      const response = await pipelineFetch(apiUrl, {
        method: "POST",
        signal,
        headers: {
          "Content-Type": "application/json",
          Authorization: `Bearer ${thinking.apiKey}`,
        },
        body: JSON.stringify(body),
      });
      return readJsonOrThrow(response, "thinking model");
    },
    {
      retries: 3,
      signal,
      shouldRetry: (err) => !/aborted/i.test(String(err?.message || err)),
    },
  );

  const text = extractMessageContent(responseJson);
  const parsed = parseModelJson(text);
  return {
    persona: parsed.persona || {},
    prompts: Array.isArray(parsed.prompts) ? parsed.prompts : [],
    raw: responseJson,
  };
}
