/**
 * [Input] An appearanceId persisted by `lib/appearance-store.js`.
 * [Output] Side-by-side video preview, compressed configurable per-family WAV cue overrides, persistent pending audio OTA
 *          reminder with top-right downlink action, generated family grid, and normalized source/provider labels for built-in, custom, and codex pet appearances.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useMemo, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import {
  ArrowLeft,
  Trash2,
  AlertCircle,
  Loader,
  Upload,
  UploadCloud,
  Volume2,
} from "lucide-react";
import {
  deleteAppearance,
  getAppearance,
  readAudioAsBlobUrl,
  removeFamilyAudioCue,
  replaceFamilyAudioCue,
} from "./lib/appearance-store.js";
import AppearancePreview from "./AppearancePreview.jsx";
import { compressWavFileForBoard } from "./lib/audio-cue.js";
import { mediaFromFamily, mediaFromSourceImage, mediaFromSourcePreview } from "./lib/appearance-preview.js";
import { FAMILIES } from "./lib/avatar-pipeline/families.js";

const AUDIO_SYNC_DIRTY_PREFIX = "pet-manager.appearance-audio-dirty.";

function appearanceSourceLabel(record) {
  if (record.type === "builtin") return "内置形象";
  if (record.type === "codex-import") return "codex pet";
  return "自定义形象";
}

function hasTauriRuntime() {
  return typeof window !== "undefined" && Boolean(window.__TAURI_INTERNALS__);
}

function dirtyStorageKey(appearanceId) {
  return `${AUDIO_SYNC_DIRTY_PREFIX}${appearanceId}`;
}

function readAudioSyncDirty(appearanceId) {
  if (!appearanceId || typeof window === "undefined" || !window.localStorage) return false;
  return window.localStorage.getItem(dirtyStorageKey(appearanceId)) === "1";
}

function writeAudioSyncDirty(appearanceId, dirty) {
  if (!appearanceId || typeof window === "undefined" || !window.localStorage) return;
  const key = dirtyStorageKey(appearanceId);
  if (dirty) window.localStorage.setItem(key, "1");
  else window.localStorage.removeItem(key);
}

export default function AppearanceDetail({ appearanceId, onBack }) {
  const [record, setRecord] = useState(null);
  const [error, setError] = useState("");
  const [activeFamily, setActiveFamily] = useState("");
  const [audioUrl, setAudioUrl] = useState("");
  const [audioErr, setAudioErr] = useState("");
  const [audioState, setAudioState] = useState("idle");
  const [audioSyncDirty, setAudioSyncDirty] = useState(() => readAudioSyncDirty(appearanceId));
  const [audioSyncState, setAudioSyncState] = useState("idle"); // idle | syncing | success | error
  const [audioSyncMessage, setAudioSyncMessage] = useState("");
  const [deleteState, setDeleteState] = useState("idle"); // idle | confirm | deleting
  const [deleteError, setDeleteError] = useState("");

  const markAudioSyncDirty = useCallback((message) => {
    writeAudioSyncDirty(appearanceId, true);
    setAudioSyncDirty(true);
    setAudioSyncState("idle");
    setAudioSyncMessage(message || "音效已保存到客户端，需要通过板端音效 OTA 通道下发到设备后才会生效。");
  }, [appearanceId]);

  const reload = useCallback(async () => {
    setError("");
    try {
      const nextRecord = await getAppearance(appearanceId);
      setRecord(nextRecord);
      const firstOk = nextRecord.families.find((family) => family.ok);
      setActiveFamily(firstOk?.family || "");
    } catch (loadError) {
      setError(loadError?.message || String(loadError));
    }
  }, [appearanceId]);

  useEffect(() => {
    reload();
  }, [reload]);

  useEffect(() => {
    setDeleteState("idle");
    setDeleteError("");
    setAudioSyncDirty(readAudioSyncDirty(appearanceId));
    setAudioSyncState("idle");
    setAudioSyncMessage("");
  }, [appearanceId]);

  useEffect(() => {
    if (!record || !activeFamily) {
      setAudioUrl("");
      return undefined;
    }

    let cancelled = false;
    let createdObjectUrl = "";

    (async () => {
      setAudioErr("");
      setAudioUrl("");
      const family = record.families.find((item) => item.family === activeFamily);
      if (!family || !family.ok) return;

      if (family.audioSrc) {
        setAudioUrl(family.audioSrc);
        return;
      }

      if (!family.audioPath) return;

      try {
        const url = await readAudioAsBlobUrl(family.audioPath);
        if (cancelled) {
          URL.revokeObjectURL(url);
        } else {
          createdObjectUrl = url;
          setAudioUrl(url);
        }
      } catch (loadError) {
        if (!cancelled) setAudioErr(loadError?.message || String(loadError));
      }
    })();

    return () => {
      cancelled = true;
      if (createdObjectUrl) URL.revokeObjectURL(createdObjectUrl);
    };
  }, [record, activeFamily]);

  const handleDelete = useCallback(async () => {
    if (!record || deleteState === "deleting") return;
    if (deleteState !== "confirm") {
      setDeleteState("confirm");
      return;
    }
    setDeleteState("deleting");
    setDeleteError("");
    try {
      await deleteAppearance(record.id);
      onBack?.();
    } catch (deleteFailure) {
      console.error(deleteFailure);
      setDeleteError(deleteFailure?.message || String(deleteFailure));
      setDeleteState("idle");
    }
  }, [deleteState, record, onBack]);

  const handleAudioFileChange = useCallback(async (event) => {
    const file = event.target.files?.[0];
    event.target.value = "";
    if (!file || !record || !activeFamily || audioState === "saving") return;
    if (!file.name.toLowerCase().endsWith(".wav") && !/audio\/(x-)?wav/i.test(file.type || "")) {
      setAudioErr("请上传 WAV 格式文件");
      return;
    }
    setAudioState("saving");
    setAudioErr("");
    try {
      const audioBytes = await compressWavFileForBoard(file);
      const nextRecord = await replaceFamilyAudioCue({
        appearanceId: record.id,
        family: activeFamily,
        audioBytes,
      });
      setRecord(nextRecord);
      markAudioSyncDirty(`已更新 ${activeFamily} 的提示音，请通过音效通道下发到设备。`);
    } catch (saveFailure) {
      console.error(saveFailure);
      setAudioErr(saveFailure?.message || String(saveFailure));
    } finally {
      setAudioState("idle");
    }
  }, [activeFamily, audioState, markAudioSyncDirty, record]);

  const handleRemoveAudioCue = useCallback(async () => {
    if (!record || !activeFamily || audioState === "saving") return;
    setAudioState("saving");
    setAudioErr("");
    try {
      const nextRecord = await removeFamilyAudioCue({ appearanceId: record.id, family: activeFamily });
      setRecord(nextRecord);
      markAudioSyncDirty(`已移除 ${activeFamily} 的提示音，请通过音效通道下发到设备。`);
    } catch (removeFailure) {
      console.error(removeFailure);
      setAudioErr(removeFailure?.message || String(removeFailure));
    } finally {
      setAudioState("idle");
    }
  }, [activeFamily, audioState, markAudioSyncDirty, record]);

  const handleSyncAudioCues = useCallback(async () => {
    if (!record || audioSyncState === "syncing") return;
    if (!hasTauriRuntime()) {
      setAudioSyncState("error");
      setAudioSyncMessage("当前不是桌面客户端运行环境，无法通过板端音效 OTA 通道下发。");
      return;
    }

    setAudioSyncState("syncing");
    setAudioSyncMessage("正在通过板端音效 OTA 通道下发 WAV...");
    try {
      const result = await invoke("usb_sync_appearance", { appearanceId: record.id });
      if (!result?.ok) {
        throw new Error(result?.error || "设备未确认提示音下发");
      }
      writeAudioSyncDirty(record.id, false);
      setAudioSyncDirty(false);
      setAudioSyncState("success");
      setAudioSyncMessage(
        `音效已下发到设备（${result.fileCount || 0} 个素材，${result.byteCount || 0} bytes）。`,
      );
    } catch (syncFailure) {
      console.error(syncFailure);
      setAudioSyncState("error");
      setAudioSyncMessage(syncFailure?.message || String(syncFailure));
    }
  }, [audioSyncState, record]);

  const familyByName = useMemo(() => {
    const map = new Map();
    if (record) {
      for (const family of record.families) map.set(family.family, family);
    }
    return map;
  }, [record]);

  const generatedFamilies = useMemo(
    () => record?.families?.filter((family) => family.ok) || [],
    [record],
  );

  const audioFamilyNames = useMemo(
    () => generatedFamilies
      .filter((family) => family.audioPath || family.audioSrc)
      .map((family) => family.family),
    [generatedFamilies],
  );


  if (error) {
    return (
      <div className="page page-appearance-detail">
        <div className="page-toolbar">
          <button className="btn-ghost" onClick={onBack}>
            <ArrowLeft size={16} />
            返回宠物图册
          </button>
        </div>
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 读取形象失败：{error}
        </div>
      </div>
    );
  }

  if (!record) {
    return (
      <div className="page page-appearance-detail">
        <div className="empty-state">
          <Loader size={20} className="spin" />
          <div>
            <strong>正在加载形象…</strong>
          </div>
        </div>
      </div>
    );
  }

  const activeRecord = familyByName.get(activeFamily);
  const activePreviewMedia = activeRecord?.ok
    ? mediaFromFamily(activeRecord, record) ||
      mediaFromSourcePreview(record) ||
      mediaFromSourceImage(record)
    : null;
  const isBuiltin = record.type === "builtin";
  const canEditAudio = activeRecord?.ok;
  const audioInputId = `audio-cue-${record.id}-${activeFamily || "none"}`;

  return (
    <div className="page page-appearance-detail">
      <div className="page-toolbar">
        <button className="btn-ghost" onClick={onBack}>
          <ArrowLeft size={16} />
          返回宠物图册
        </button>
        <span className="grow" />
        {audioSyncDirty && (
          <button
            className="btn-primary detail-audio-sync-btn"
            type="button"
            onClick={handleSyncAudioCues}
            disabled={audioSyncState === "syncing"}
            title="通过板端音效 OTA 通道下发当前 WAV"
          >
            {audioSyncState === "syncing" ? (
              <Loader size={14} className="spin" />
            ) : (
              <UploadCloud size={14} />
            )}
            {audioSyncState === "syncing" ? "下发中..." : "下发音效"}
          </button>
        )}
        {!isBuiltin && (
          <button
            className="btn-ghost danger"
            onClick={handleDelete}
            disabled={deleteState === "deleting"}
          >
            {deleteState === "deleting" ? (
              <>
                <Loader size={14} className="spin" />
                删除中…
              </>
            ) : deleteState === "confirm" ? (
              <>
                <Trash2 size={14} />
                确认删除？
              </>
            ) : (
              <>
                <Trash2 size={14} />
                删除形象
              </>
            )}
          </button>
        )}
      </div>

      {deleteError && (
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 删除形象失败：{deleteError}
        </div>
      )}

      <div className="detail-grid">
        <div className="detail-media">
          {activeRecord?.ok && activePreviewMedia ? (
            <AppearancePreview
              media={activePreviewMedia}
              className="detail-media__video"
              emptyClassName="detail-media__video detail-media__video--empty"
              playing
            />
          ) : (
            <div
              className="detail-media__video"
              style={{ display: "grid", placeItems: "center", color: "#888", background: "#f4f5f7" }}
            >
              {generatedFamilies.length === 0 ? (
                <div style={{ textAlign: "center", padding: 16 }}>
                  <AlertCircle size={28} />
                  <div style={{ marginTop: 8 }}>暂无可用动画素材</div>
                  <div className="muted small" style={{ marginTop: 4, maxWidth: 360 }}>
                    这个形象没有成功生成的 family，请返回宠物图册重新创建。
                  </div>
                </div>
              ) : (
                <div>暂无可预览素材</div>
              )}
            </div>
          )}
          <div className="detail-media__label">
            {activeRecord?.family || "—"} · {activeRecord?.ok ? "已生成" : "暂无素材"}
          </div>
        </div>

        <div className="detail-meta">
          <h2>{record.name}</h2>
          {record.description && <div className="muted small">{record.description}</div>}
          <div style={{ display: "flex", gap: 8, alignItems: "center", marginTop: 6, flexWrap: "wrap" }}>
            <span className="pill" style={{ background: "var(--accent-soft)", color: "var(--accent-strong)" }}>
              {appearanceSourceLabel(record)}
            </span>
            <span className="muted small">
              {record.provider || "—"} · {record.model || "—"}
            </span>
          </div>
          <div className="path">{record.absolute_dir}</div>
          <div className="muted small">
            生成时间：{new Date(record.created_at).toLocaleString()}
          </div>
          {activeRecord?.ok && (
            <div className="detail-audio-config">
              <div className="detail-audio-config__header">
                <span>
                  <Volume2 size={15} />
                  状态提示音
                </span>
                <span className="muted small">{activeRecord.family}</span>
              </div>
              {audioUrl ? (
                <audio className="detail-audio-config__player" src={audioUrl} controls />
              ) : (
                <div className="muted small">
                  当前状态 {activeRecord.family} 还没有提示音。
                  {audioFamilyNames.length > 0
                    ? ` 已有提示音: ${audioFamilyNames.join(" / ")}。请选择对应状态试听，或为当前状态上传 WAV。`
                    : ""}
                </div>
              )}
              {audioErr && (
                <div className="message-banner message-banner--error">
                  <AlertCircle size={14} /> {audioErr}
                </div>
              )}
              {(audioSyncDirty || audioSyncMessage) && (
                <div
                  className={`message-banner detail-audio-sync-message ${
                    audioSyncState === "error"
                      ? "message-banner--error"
                      : audioSyncState === "success"
                        ? "message-banner--success"
                        : "message-banner--warning"
                  }`}
                >
                  {audioSyncState === "syncing" ? <Loader size={14} className="spin" /> : <UploadCloud size={14} />}
                  {audioSyncMessage || "音效已保存到客户端，需要通过板端音效 OTA 通道下发到设备后才会生效。"}
                </div>
              )}
              <div className="detail-audio-config__actions">
                <input
                  id={audioInputId}
                  className="detail-audio-config__input"
                  type="file"
                  accept="audio/wav,audio/x-wav,.wav"
                  onChange={handleAudioFileChange}
                  disabled={!canEditAudio || audioState === "saving"}
                />
                <label
                  className={`btn-ghost${canEditAudio ? "" : " is-disabled"}`}
                  htmlFor={canEditAudio ? audioInputId : undefined}
                >
                  {audioState === "saving" ? <Loader size={14} className="spin" /> : <Upload size={14} />}
                  上传 WAV
                </label>
                {audioUrl && canEditAudio && !activeRecord.audioDefault && (
                  <button
                    className="btn-ghost danger"
                    type="button"
                    onClick={handleRemoveAudioCue}
                    disabled={audioState === "saving"}
                  >
                    <Trash2 size={14} />
                    移除提示音
                  </button>
                )}
              </div>
              {isBuiltin && (
                <div className="muted small">
                  未上传时使用默认提示音；上传后会优先使用你的配置。
                </div>
              )}
              {!isBuiltin && activeRecord.audioDefault && (
                <div className="muted small">
                  未上传时使用默认提示音；上传后会优先使用你的配置。
                </div>
              )}
            </div>
          )}
        </div>
      </div>

      <div>
        <h4 style={{ margin: "18px 0 6px", fontSize: 13.5 }}>全部素材（{generatedFamilies.length}）</h4>
        {generatedFamilies.length === 0 ? (
          <div className="empty-state">
            <div>
              <strong>暂无可用素材</strong>
            </div>
            <div className="muted small">未成功生成的 family 已隐藏。</div>
          </div>
        ) : (
          <div className="detail-states">
            {generatedFamilies.map((familyRecord) => {
              const definition =
                FAMILIES.find((item) => item.family === familyRecord.family) || {
                  family: familyRecord.family,
                  label: familyRecord.family,
                };
              const isActive = activeFamily === familyRecord.family;
              return (
                <button
                  key={familyRecord.family}
                  type="button"
                  className={`state-card${isActive ? " active" : ""}`}
                  onClick={() => setActiveFamily(familyRecord.family)}
                >
                  <span className="state-card__thumb">
                    <AppearancePreview
                      media={mediaFromFamily(familyRecord)}
                      className="state-card__media"
                      emptyClassName="state-card__media state-card__media--empty"
                    />
                  </span>
                  <span className="state-card__family">{familyRecord.family}</span>
                  <span className="state-card__hint">{definition.label}</span>
                  {(familyRecord.audioPath || familyRecord.audioSrc) && (
                    <span className="state-card__sound">提示音</span>
                  )}
                </button>
              );
            })}
          </div>
        )}
      </div>
    </div>
  );
}
