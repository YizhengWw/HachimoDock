/** Strict bounded model output. Never eval, guess verdicts, or combine reasoning with answers. */
export function modelOutputText(raw) {
  if (typeof raw?.output_text === 'string') return raw.output_text;
  return (Array.isArray(raw?.output) ? raw.output : [])
    .filter(item => item.type === 'message' || (!item.type && Array.isArray(item.content)))
    .flatMap(item => item.content || [])
    .filter(part => part.type === 'output_text' || (!part.type && typeof part.text === 'string'))
    .map(part => part.text || '').join('\n');
}

export function parseStrictModelJson(text) {
  if (typeof text !== 'string' || text.length > 128 * 1024) throw new Error('model JSON size');
  const trimmed = text.trim().replace(/^\uFEFF/, '').trim();
  const fence = trimmed.match(/^```(?:json)?\s*([\s\S]*?)\s*```$/i);
  const value = JSON.parse(fence ? fence[1] : trimmed);
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error('model JSON object required');
  return value;
}

// Only bounded response data is checkpointed privately; not headers, input images or prompts.
export function reviewSnapshot(raw) {
  const text = modelOutputText(raw);
  return { output_text: text.slice(0, 128 * 1024),
    status: ['completed','incomplete','failed'].includes(raw?.status) ? raw.status : undefined,
    truncated: text.length > 128 * 1024 };
}
