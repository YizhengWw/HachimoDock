/**
 * [Input] IMG.LY background-removal 1.7.0's prebundled ESM and the local CSP-safe ndarray implementation.
 * [Output] A Vite transform that removes Function-constructor use while preserving background-removal behavior.
 * [Pos] Build-time compatibility guard for the release WebView bundle.
 * [Sync] If IMG.LY is upgraded, revalidate markers, background-removal-csp.test.js, and ref/.folder.md.
 */

import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const VIRTUAL_NDARRAY_ID = "virtual:pet-manager-ndarray-csp";
const RESOLVED_VIRTUAL_NDARRAY_ID = `\0${VIRTUAL_NDARRAY_ID}`;
const NDARRAY_SOURCE = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "src/lib/ndarray-csp.js",
);
const IMG_LY_MODULE_SUFFIX = "/node_modules/@imgly/background-removal/dist/index.mjs";
const ORT_MODULE_SUFFIXES = new Set([
  "/node_modules/onnxruntime-web/dist/ort.bundle.min.mjs",
  "/node_modules/onnxruntime-web/dist/ort.webgpu.bundle.min.mjs",
]);
const NDARRAY_START = "// ../../node_modules/.pnpm/ndarray@1.0.19/node_modules/ndarray/ndarray.js";
const NDARRAY_END = "// ../../node_modules/.pnpm/lodash-es@4.17.21/node_modules/lodash-es/_freeGlobal.js";
const DYNAMIC_GLOBAL = 'var root = freeGlobal_default || freeSelf || Function("return this")();';
const STATIC_GLOBAL = "var root = freeGlobal_default || freeSelf || globalThis;";
const DYNAMIC_CODE = /(?:^|[^\w$])(?:new\s+)?Function\s*\(|(?:^|[^\w$])eval\s*\(/;
const EMITTED_ORT_MODULE = /^assets\/ort(?:\.webgpu)?\.bundle\.min-[^/]+\.mjs$/;

function replaceExactlyOnce(source, before, after, label) {
  const first = source.indexOf(before);
  const last = source.lastIndexOf(before);
  if (first < 0 || first !== last) {
    throw new Error(`IMG.LY CSP transform expected exactly one ${label}; dependency layout changed`);
  }
  return `${source.slice(0, first)}${after}${source.slice(first + before.length)}`;
}

function replaceOrtDynamicGlobal(source, label) {
  const transformed = replaceExactlyOnce(
    source,
    'Function("return this")()',
    "globalThis",
    label,
  );
  if (DYNAMIC_CODE.test(transformed)) {
    throw new Error(`ONNX Runtime CSP transform left dynamic execution in ${label}`);
  }
  return transformed;
}

export function cspSafeBackgroundRemovalPlugin() {
  return {
    name: "pet-manager-csp-safe-background-removal",
    enforce: "pre",
    resolveId(id) {
      return id === VIRTUAL_NDARRAY_ID ? RESOLVED_VIRTUAL_NDARRAY_ID : null;
    },
    load(id) {
      return id === RESOLVED_VIRTUAL_NDARRAY_ID
        ? readFileSync(NDARRAY_SOURCE, "utf8")
        : null;
    },
    transform(code, id) {
      const normalizedId = id.split("?", 1)[0].replaceAll("\\", "/");
      const ortModuleSuffix = [...ORT_MODULE_SUFFIXES].find((suffix) => normalizedId.endsWith(suffix));
      if (ortModuleSuffix) {
        const transformed = replaceOrtDynamicGlobal(
          code,
          `ONNX Runtime dynamic-global fallback in ${ortModuleSuffix}`,
        );
        return { code: transformed, map: null };
      }
      if (!normalizedId.endsWith(IMG_LY_MODULE_SUFFIX)) return null;

      const ndarrayStart = code.indexOf(NDARRAY_START);
      const ndarrayEnd = code.indexOf(NDARRAY_END, ndarrayStart + NDARRAY_START.length);
      if (ndarrayStart < 0 || ndarrayEnd < 0 || ndarrayEnd <= ndarrayStart) {
        throw new Error("IMG.LY CSP transform could not locate the pinned ndarray 1.0.19 bundle");
      }

      const replacement = `${NDARRAY_START}\nvar require_ndarray = __commonJS({\n  "ndarray-csp.js"(exports, module) {\n    module.exports = ndarrayCsp;\n  }\n});\n\n`;
      let transformed = `${code.slice(0, ndarrayStart)}${replacement}${code.slice(ndarrayEnd)}`;
      transformed = replaceExactlyOnce(
        transformed,
        DYNAMIC_GLOBAL,
        STATIC_GLOBAL,
        "lodash dynamic-global fallback",
      );
      transformed = `import ndarrayCsp from "${VIRTUAL_NDARRAY_ID}";\n${transformed}`;

      const dynamicMatch = transformed.match(DYNAMIC_CODE);
      if (dynamicMatch) {
        const offset = dynamicMatch.index ?? 0;
        const context = transformed.slice(Math.max(0, offset - 48), offset + 96);
        throw new Error(
          `IMG.LY CSP transform left dynamic JavaScript execution at byte ${offset}: ${JSON.stringify(context)}`,
        );
      }
      return { code: transformed, map: null };
    },
    generateBundle(_options, bundle) {
      for (const [fileName, output] of Object.entries(bundle)) {
        if (output.type !== "asset" || !EMITTED_ORT_MODULE.test(fileName)) continue;
        const source = typeof output.source === "string"
          ? output.source
          : new TextDecoder().decode(output.source);
        output.source = replaceOrtDynamicGlobal(
          source,
          `emitted ONNX Runtime worker asset ${fileName}`,
        );
      }
    },
  };
}
