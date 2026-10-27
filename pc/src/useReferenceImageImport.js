import { useCallback, useEffect, useRef, useState } from "react";
import { createReferenceImportSession, referenceFileFromEvent, validateReferenceFile, visibleReferenceRect } from "./lib/reference-image-import.js";

export default function useReferenceImageImport(onFileChange) {
  const zoneRef = useRef(null), sessionRef = useRef(null), callbackRef = useRef(onFileChange), sequence = useRef(0);
  callbackRef.current = onFileChange;
  const [native, setNative] = useState(false), [busy, setBusy] = useState(false), [error, setError] = useState("");
  const [diagnostic, setDiagnostic] = useState(null);
  useEffect(() => {
    let disposed = false, observer, frame, session;
    const sync = () => {
      cancelAnimationFrame(frame);
      frame = requestAnimationFrame(() => {
        if (!disposed && zoneRef.current) session?.mount(visibleReferenceRect(zoneRef.current)).catch(() => {});
      });
    };
    const token = crypto.randomUUID?.() || Array.from(crypto.getRandomValues(new Uint8Array(16)), byte => byte.toString(16).padStart(2, "0")).join("");
    (async () => {
      if (!window.__TAURI_INTERNALS__ || !/Mac/i.test(navigator.platform)) return;
      const [{ invoke }, { listen }] = await Promise.all([import("@tauri-apps/api/core"), import("@tauri-apps/api/event")]);
      if (disposed) return;
      session = createReferenceImportSession({ invoke, listen, token, onEvent: event => {
        if (event.status === "loading") { sequence.current++; setBusy(true); setError(""); setDiagnostic(null); return; }
        setBusy(false);
        setDiagnostic(event.diagnostic || null);
        if (event.status === "cancelled") { setError(""); return; }
        if (event.status === "error") { setError(event.error || "图片导入失败，请重试。"); return; }
        try { const file = referenceFileFromEvent(event, token); if (file) { callbackRef.current(file); setError(""); } }
        catch (err) { setError(err.message); }
      } });
      sessionRef.current = session;
      const enabled = await session.mount(visibleReferenceRect(zoneRef.current));
      if (disposed) return;
      setNative(enabled);
      observer = new ResizeObserver(sync); observer.observe(zoneRef.current); observer.observe(document.documentElement);
      window.addEventListener("resize", sync); window.addEventListener("scroll", sync, true);
      window.addEventListener("transitionend", sync, true); window.addEventListener("animationend", sync, true);
    })().catch(() => { if (!disposed) setError("图库导入暂不可用；可先导出 JPEG，再选择文件。"); });
    return () => {
      disposed = true; sequence.current++;
      observer?.disconnect(); cancelAnimationFrame(frame);
      window.removeEventListener("resize", sync); window.removeEventListener("scroll", sync, true);
      window.removeEventListener("transitionend", sync, true); window.removeEventListener("animationend", sync, true);
      session?.dispose().catch(() => {}); sessionRef.current = null;
    };
  }, []);
  const acceptFile = useCallback(async file => {
    if (!file) return;
    const id = ++sequence.current;
    setBusy(true); setError(""); setDiagnostic(null);
    let url;
    try {
      validateReferenceFile(file);
      url = URL.createObjectURL(file);
      const image = new Image(); image.src = url; await image.decode();
      if (id !== sequence.current) return;
      if (image.naturalWidth * image.naturalHeight > 100000000) throw new Error("图片超过一亿像素，请缩小后重试。");
      if (/\.(heic|heif|tiff?|bmp)$/i.test(file.name) || /image\/(heic|heif|tiff|bmp)/i.test(file.type)) {
        const scale = Math.min(1, 2048 / Math.max(image.naturalWidth, image.naturalHeight));
        const canvas = document.createElement("canvas");
        canvas.width = Math.max(1, Math.round(image.naturalWidth * scale)); canvas.height = Math.max(1, Math.round(image.naturalHeight * scale));
        canvas.getContext("2d").drawImage(image, 0, 0, canvas.width, canvas.height);
        const blob = await new Promise(resolve => canvas.toBlob(resolve, "image/png"));
        if (!blob || blob.size > 16 * 1024 * 1024) throw new Error("图片转换失败或过大，请导出 JPEG 后重试。");
        file = new File([blob], "reference.png", { type: "image/png" });
      }
      if (id === sequence.current) callbackRef.current(file);
    } catch (err) {
      if (id === sequence.current) setError(err.name === "EncodingError" ? "当前系统无法解码这张图片，请从“照片”导出 JPEG 或 PNG 后重试。" : err.message);
    } finally { if (url) URL.revokeObjectURL(url); if (id === sequence.current) setBusy(false); }
  }, []);
  const action = useCallback(source => sessionRef.current?.action(source).catch(() => setError("无法打开图片选择器，请重试。")), []);
  const drop = useCallback(event => {
    event.preventDefault(); if (busy) return;
    const files = event.dataTransfer?.files;
    if (files?.length === 1) acceptFile(files[0]);
    else setError(files?.length > 1 ? "每次请选择一张参考图。" : "未取得图片文件。请用“从照片图库选择”，或将照片导出为 JPEG 后拖入。");
  }, [acceptFile, busy]);
  const cancel = useCallback(() => { sequence.current++; setBusy(false); setError(""); setDiagnostic(null); action("cancel"); }, [action]);
  return { zoneRef, native, busy, error, diagnostic, acceptFile, action, drop, cancel };
}
