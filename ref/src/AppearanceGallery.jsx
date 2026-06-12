/**
 * [Input] Read appearances persisted by `lib/appearance-store.js`.
 * [Output] Card grid with the built-in Terrier first, filled hover-play previews,
 *          compact import actions, cached local/Codex scans, cached initial render, unobstructed previews,
 *          gallery-only creation/import/detail management actions,
 *          and Codex/community import flows.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
 */

import React, { useCallback, useEffect, useRef, useState } from "react";
import {
  RefreshCw,
  Loader,
  AlertCircle,
  Download,
  X,
  Globe,
  ExternalLink,
  CheckCircle,
  CheckCircle2,
  Sparkles,
} from "lucide-react";
import { invoke } from "@tauri-apps/api/core";
import { getCachedAppearances, listAppearances } from "./lib/appearance-store.js";
import { listBuiltinAppearances } from "./lib/builtin-appearances.js";
import AppearancePreview from "./AppearancePreview.jsx";
import { resolveGalleryPreviewMedia } from "./lib/appearance-preview.js";
import {
  buildCodexPetSnapshot,
  findUpdatedCodexPets,
  formatCodexPetModifiedAt,
  parseCommunityPetImportInput,
  sortCodexPetsByModifiedAt,
} from "./lib/codex-community-import.js";
import {
  checkFfmpegAvailable,
  importCodexPet,
  installCodexCommunityPet,
  listCodexPets,
} from "./lib/codex-pets-client.js";
import {
  abortGenerationTask,
  subscribeGenerationTask,
} from "./lib/generation-task.js";
import PageShell from "./shell/PageShell.jsx";
import Card from "./shell/Card.jsx";
import { useDeviceContext } from "./shell/DeviceContext.jsx";

// Per-family stage labels surfaced inside the RunningTaskCard so the user can
// tell whether the slow phase is submission, polling, or download.
const STAGE_LABELS = {
  pending: "等待中",
  submitting: "提交中…",
  polling: "排队 / 生成中…",
  downloading: "下载中…",
  done: "已完成",
  failed: "失败",
};

const COMMUNITY_SOURCES = [
  {
    id: "codex-pets-net",
    url: "https://codex-pets.net",
    name: "Codex Pets",
  },
];

function codexPetPreviewSrc(pet) {
  return pet?.previewDataUrl || pet?.preview_data_url || "";
}

export default function AppearanceGallery({ binding, onEnterWizard, onOpenDetail }) {
  const { currentDisplay } = useDeviceContext();

  const [items, setItems] = useState(() => getCachedAppearances() || null);
  const [error, setError] = useState("");
  const [refreshing, setRefreshing] = useState(false);

  // ── Codex import modal ──
  const [codexModalOpen, setCodexModalOpen] = useState(false);
  const [codexPets, setCodexPets] = useState(null);
  const [codexLoading, setCodexLoading] = useState(false);
  const [codexError, setCodexError] = useState("");
  const [importError, setImportError] = useState("");
  const [importingId, setImportingId] = useState("");

  // ── Community import modal ──
  const [communityModalOpen, setCommunityModalOpen] = useState(false);

  const reload = useCallback(async ({ force = false } = {}) => {
    setRefreshing(true);
    setError("");
    try {
      const records = await listAppearances({ force });
      setItems(records);
    } catch (err) {
      console.error(err);
      setError(err?.message || String(err));
      // Don't blank the grid on transient sync errors: keep whatever we already
      // had on screen, otherwise fall back to the built-in Westie/Terrier so
      // users never see an empty gallery.
      setItems((current) => (current && current.length > 0 ? current : listBuiltinAppearances()));
    } finally {
      setRefreshing(false);
    }
  }, []);

  useEffect(() => {
    reload();
  }, [reload]);


  // Subscribe to the global generation task so the in-progress card shows live
  // progress regardless of how the user reached the gallery. Auto-reload when
  // a run terminates — the task persists incrementally, so a fresh listing
  // pulls in the new appearance without the user pressing 刷新.
  const [task, setTask] = useState(null);
  const lastEpochRef = useRef(0);
  useEffect(() => {
    return subscribeGenerationTask((s) => {
      setTask(s);
      if (s.completionEpoch > lastEpochRef.current) {
        lastEpochRef.current = s.completionEpoch;
        reload({ force: true });
      }
    });
  }, [reload]);
  const taskRunning = task?.status === "running";

  const openCodexImport = useCallback(async () => {
    setCodexError("");
    setImportError("");
    setCodexPets(null);
    setCodexModalOpen(true);
    setCodexLoading(true);
    try {
      const hasFfmpeg = await checkFfmpegAvailable();
      if (!hasFfmpeg) {
        setCodexError(
          "未检测到 ffmpeg。请先安装:\n  macOS: brew install ffmpeg\n  Windows: winget install Gyan.FFmpeg\n  Linux: apt install ffmpeg",
        );
        return;
      }
      const pets = await listCodexPets();
      setCodexPets(sortCodexPetsByModifiedAt(pets));
    } catch (err) {
      console.error(err);
      setCodexError(err?.message || String(err));
    } finally {
      setCodexLoading(false);
    }
  }, []);

  const handleImportPet = useCallback(
    async (petId, { closeModal = "codex" } = {}) => {
      setImportingId(petId);
      setCodexError("");
      setImportError("");
      try {
        const result = await importCodexPet(petId);
        if (closeModal === "codex") setCodexModalOpen(false);
        else if (closeModal === "community") setCommunityModalOpen(false);
        await reload({ force: true });
        if (result?.appearanceId) onOpenDetail?.(result.appearanceId);
        return { ok: true, appearanceId: result?.appearanceId || "" };
      } catch (err) {
        console.error(err);
        const message = err?.message || String(err);
        setImportError(message);
        return { ok: false, error: message };
      } finally {
        setImportingId("");
      }
    },
    [reload, onOpenDetail],
  );

  const activeAppearanceId = currentDisplay.appearance?.id || "";

  const refreshButton = (
    <button
      key="refresh"
      type="button"
      className="btn-secondary btn-sm"
      onClick={() => reload({ force: true })}
      disabled={refreshing}
    >
      <RefreshCw size={14} className={refreshing ? "spin" : undefined} />
      刷新
    </button>
  );

  const addAppearanceActions = (
    <div key="add-actions" className="appearance-gallery-actions" aria-label="添加形象">
      <button type="button" className="btn-primary btn-sm" onClick={onEnterWizard}>
        <Sparkles size={14} /> 新建自定义形象
      </button>
      <button type="button" className="btn-secondary btn-sm" onClick={openCodexImport}>
        <Download size={14} /> 从 Codex 导入
      </button>
      <button type="button" className="btn-secondary btn-sm" onClick={() => setCommunityModalOpen(true)}>
        <Globe size={14} /> 从社区导入
      </button>
    </div>
  );

  return (
    <PageShell
      title="形象画廊"
      subtitle="浏览默认形象与你的自定义形象，进入详情可预览每个 family 的视频。"
      actions={[refreshButton, addAppearanceActions]}
    >
      {error && (
        <div className="message-banner message-banner--error">
          <AlertCircle size={14} /> 读取形象失败：{error}
        </div>
      )}

      {taskRunning && (
        <Card>
          <RunningTaskCard
            task={task}
            onAbort={abortGenerationTask}
            onOpenDetail={onOpenDetail}
          />
        </Card>
      )}

      {items === null ? (
        <div className="empty-state">
          <Loader size={20} className="spin" />
          <div>
            <strong>正在加载形象列表…</strong>
          </div>
        </div>
      ) : items.length === 0 ? (
        <div className="empty-state">
          <div>
            <strong>还没有自定义形象</strong>
          </div>
          <div className="muted small">
            点击上方「添加形象」上传一张图，生成属于你的 10 段桌宠动画。
          </div>
        </div>
      ) : (
        <div className="appearance-grid">
          {items.map((row) => (
            <AppearanceCard
              key={row.id}
              row={row}
              isActive={row.id === activeAppearanceId}
              onOpenDetail={onOpenDetail}
            />
          ))}
        </div>
      )}

      {codexModalOpen && (
        <CodexImportModal
          loading={codexLoading}
          pets={codexPets}
          error={codexError || importError}
          importingId={importingId}
          onClose={() => {
            if (importingId) return;
            setCodexModalOpen(false);
          }}
          onPick={(petId) => handleImportPet(petId, { closeModal: "codex" })}
        />
      )}

      {communityModalOpen && (
        <CommunityImportModal
          importingId={importingId}
          importError={importError}
          onClose={() => {
            if (importingId) return;
            setCommunityModalOpen(false);
          }}
          onImport={(petId) => handleImportPet(petId, { closeModal: "community" })}
        />
      )}

    </PageShell>
  );
}

function CodexImportModal({ loading, pets, error, importingId, onClose, onPick }) {
  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-card" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">从 Codex 导入形象</h3>
            <div className="modal-subtitle">从 ~/.codex/pets/ 读取已安装的桌宠</div>
          </div>
          <button
            className="icon-btn"
            onClick={onClose}
            disabled={!!importingId}
            aria-label="关闭"
          >
            <X size={16} />
          </button>
        </div>
        <div className="modal-body">
          {loading && (
            <div className="empty-state">
              <Loader size={20} className="spin" />
              <div className="muted small">正在读取 ~/.codex/pets/ …</div>
            </div>
          )}
          {!loading && error && (
            <div
              className="message-banner message-banner--error"
              style={{ whiteSpace: "pre-wrap" }}
            >
              <AlertCircle size={14} /> {error}
            </div>
          )}
          {!loading && !error && pets && pets.length === 0 && (
            <div className="empty-state">
              <div>
                <strong>未在 ~/.codex/pets/ 下找到可导入的宠物</strong>
              </div>
              <div className="muted small">先用 codex 生成一个宠物再回来导入。</div>
            </div>
          )}
          {!loading && !error && pets && pets.length > 0 && (
            <ul className="codex-pet-list">
              {pets.map((pet) => (
                <CodexPetRow
                  key={pet.id}
                  pet={pet}
                  importingId={importingId}
                  onPick={onPick}
                />
              ))}
            </ul>
          )}
        </div>
      </div>
    </div>
  );
}

function CodexPetRow({ pet, importingId, onPick, highlight }) {
  const busy = importingId === pet.id;
  const disabled = !!importingId && !busy;
  const modifiedAtText = formatCodexPetModifiedAt(pet.modifiedAt);
  const previewSrc = codexPetPreviewSrc(pet);
  return (
    <li className={`codex-pet-row${highlight ? " codex-pet-row--new" : ""}`}>
      <div className="codex-pet-preview" aria-hidden="true">
        {previewSrc ? (
          <img className="codex-pet-preview__image" src={previewSrc} alt="" />
        ) : (
          <span className="codex-pet-preview__empty" />
        )}
      </div>
      <div className="codex-pet-info">
        <div className="codex-pet-name">
          {pet.displayName}
          {highlight && <span className="codex-pet-tag">新</span>}
        </div>
        {pet.description && <div className="muted small">{pet.description}</div>}
        <div className="codex-pet-meta">
          <span>{pet.id}</span>
          {modifiedAtText && <span>更新 {modifiedAtText}</span>}
        </div>
      </div>
      <button
        className="btn-primary btn-sm"
        onClick={() => onPick(pet.id)}
        disabled={disabled || busy}
      >
        {busy ? (
          <>
            <Loader size={14} className="spin" /> 转换中…
          </>
        ) : (
          <>
            <Download size={14} /> 导入
          </>
        )}
      </button>
    </li>
  );
}

/**
 * Community-import flow.
 *
 * Step 1 — user opens the modal. We snapshot the pet IDs and modified times
 *          already present in `~/.codex/pets/` so we can diff later. We also
 *          probe ffmpeg up front (the actual import requires it).
 * Step 2 — user can paste a codex-pets.net URL / curl / CLI command for direct
 *          install+import, or open the community source in a browser.
 * Step 3 — for browser installs, the user returns and scans `~/.codex/pets/`.
 */
function CommunityImportModal({ importingId, importError, onClose, onImport }) {
  // Snapshot of codex pet IDs and mtimes at modal open; a later rescan treats
  // new IDs and newer mtimes as freshly installed community assets.
  const [baseline, setBaseline] = useState(null); // Map<string, number> | null
  const [baselineError, setBaselineError] = useState("");
  const [ffmpegOk, setFfmpegOk] = useState(true);

  const [scanning, setScanning] = useState(false);
  const [scanError, setScanError] = useState("");
  const [newPets, setNewPets] = useState(null); // CodexPetSummary[] | null
  const [allPets, setAllPets] = useState(null); // CodexPetSummary[] | null — fallback list
  const [directInput, setDirectInput] = useState("");
  const [directError, setDirectError] = useState("");
  const [directInstalling, setDirectInstalling] = useState(false);

  // Take baseline + ffmpeg check on mount.
  useEffect(() => {
    let cancelled = false;
    (async () => {
      try {
        const [ok, pets] = await Promise.all([
          checkFfmpegAvailable(),
          listCodexPets(),
        ]);
        if (cancelled) return;
        setFfmpegOk(!!ok);
        setBaseline(buildCodexPetSnapshot(pets));
      } catch (err) {
        if (cancelled) return;
        console.error(err);
        setBaselineError(err?.message || String(err));
        setBaseline(buildCodexPetSnapshot());
      }
    })();
    return () => {
      cancelled = true;
    };
  }, []);

  const openSource = useCallback(async (url) => {
    try {
      await invoke("open_external_url", { url });
    } catch (err) {
      console.error(err);
      // fall back to window.open — works in pure-web dev, may be a no-op in
      // a Tauri webview but doesn't hurt to try.
      try {
        window.open(url, "_blank", "noopener,noreferrer");
      } catch (_) {
        /* noop */
      }
    }
  }, []);

  const handleScan = useCallback(async () => {
    if (!baseline) return;
    setScanning(true);
    setScanError("");
    try {
      const pets = (await listCodexPets({ force: true })) || [];
      setAllPets(sortCodexPetsByModifiedAt(pets));
      setNewPets(findUpdatedCodexPets(baseline, pets));
    } catch (err) {
      console.error(err);
      setScanError(err?.message || String(err));
    } finally {
      setScanning(false);
    }
  }, [baseline]);

  const directParsed = directInput.trim() ? parseCommunityPetImportInput(directInput) : null;

  const handleDirectImport = useCallback(async () => {
    const parsed = parseCommunityPetImportInput(directInput);
    if (!parsed.ok) {
      setDirectError(parsed.error);
      return;
    }
    if (!ffmpegOk) {
      setDirectError("未检测到 ffmpeg，无法完成导入到 HachimoDock。");
      return;
    }

    setDirectError("");
    setScanError("");
    setDirectInstalling(true);
    try {
      await installCodexCommunityPet(parsed.petId);
      const result = await onImport(parsed.petId);
      if (result?.ok === false) {
        setDirectError(result.error || "安装已完成，但导入到 HachimoDock 失败。");
      }
    } catch (err) {
      console.error(err);
      setDirectError(err?.message || String(err));
    } finally {
      setDirectInstalling(false);
    }
  }, [directInput, ffmpegOk, onImport]);

  const initialLoading = baseline === null;
  const showResults = newPets !== null;

  return (
    <div className="modal-backdrop" onClick={onClose}>
      <div className="modal-card modal-card--wide" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <div>
            <h3 className="modal-title">从社区导入形象</h3>
            <div className="modal-subtitle">
              先去社区选择形象，再按一种方式导入。
            </div>
          </div>
          <button
            className="icon-btn"
            onClick={onClose}
            disabled={!!importingId}
            aria-label="关闭"
          >
            <X size={16} />
          </button>
        </div>

        <div className="modal-body">
          {initialLoading ? (
            <div className="empty-state">
              <Loader size={20} className="spin" />
              <div className="muted small">正在准备……</div>
            </div>
          ) : (
            <>
              {!ffmpegOk && (
                <div
                  className="message-banner message-banner--error"
                  style={{ whiteSpace: "pre-wrap", marginTop: 0 }}
                >
                  <AlertCircle size={14} /> 未检测到 ffmpeg，导入到 HachimoDock 时会失败。
                  {"\n  macOS: brew install ffmpeg\n  Windows: winget install Gyan.FFmpeg\n  Linux: apt install ffmpeg"}
                </div>
              )}
              {baselineError && (
                <div className="message-banner message-banner--error" style={{ marginTop: 0 }}>
                  <AlertCircle size={14} /> 读取本地 Codex pets 失败：{baselineError}
                </div>
              )}

              {!showResults && (
                <>
                  <section className="community-source-intro">
                    <div className="community-source-intro__copy">
                      <div className="community-direct__head">
                        <Globe size={16} />
                        <span>先打开社区网站</span>
                      </div>
                      <div className="community-step-desc">
                        在社区里挑选形象，然后使用下方任意一种方式导入到 HachimoDock。
                      </div>
                    </div>
                    <div className="community-source-list">
                      {COMMUNITY_SOURCES.map((src) => (
                        <button
                          key={src.id}
                          type="button"
                          className="community-source"
                          onClick={() => openSource(src.url)}
                        >
                          <div className="community-source__icon">
                            <Globe size={18} />
                          </div>
                          <div className="community-source__body">
                            <div className="community-source__title">{src.name}</div>
                            <div className="community-source__url">{src.url}</div>
                          </div>
                          <div className="community-source__cta">
                            <ExternalLink size={14} />
                            <span>打开</span>
                          </div>
                        </button>
                      ))}
                    </div>
                  </section>

                  <div className="community-methods">
                    <section className="community-method">
                      <div className="community-method__head">
                        <span className="community-method__label">方式一</span>
                        <span>社区安装后扫描</span>
                      </div>
                      <div className="community-step-desc">
                        在社区点击 “Install in Codex” 完成安装，回到这里扫描新增形象。
                      </div>
                      <div className="community-actions community-actions--inline">
                        <button
                          type="button"
                          className="btn-primary"
                          onClick={handleScan}
                          disabled={scanning || !!importingId}
                        >
                          {scanning ? (
                            <>
                              <Loader size={14} className="spin" /> 扫描中…
                            </>
                          ) : (
                            <>
                              <CheckCircle2 size={14} /> 扫描最新形象
                            </>
                          )}
                        </button>
                      </div>
                    </section>

                    <section className="community-method community-direct">
                      <div className="community-method__head">
                        <span className="community-method__label">方式二</span>
                        <span>粘贴链接或命令导入</span>
                      </div>
                      <div className="community-step-desc">
                        支持社区页面链接、curl 安装命令或
                        <code>npx codex-pets add sakura-jk</code>。
                      </div>
                      <textarea
                        className="community-import-input"
                        value={directInput}
                        onChange={(event) => {
                          setDirectInput(event.target.value);
                          setDirectError("");
                        }}
                        placeholder={
                          "https://codex-pets.net/pets/sakura-jk\ncurl -fsSL https://codex-pets.net/install/sakura-jk | sh\nnpx codex-pets add sakura-jk"
                        }
                        rows={3}
                        spellCheck={false}
                      />
                      {directParsed?.ok && (
                        <div className="community-direct-status">
                          将安装并导入：<strong>{directParsed.petId}</strong>
                        </div>
                      )}
                      {(directError || importError) && (
                        <div className="message-banner message-banner--error community-inline-error">
                          <AlertCircle size={14} /> {directError || importError}
                        </div>
                      )}
                      <div className="community-actions community-actions--inline">
                        <button
                          type="button"
                          className="btn-primary"
                          onClick={handleDirectImport}
                          disabled={!directInput.trim() || directInstalling || !!importingId}
                        >
                          {directInstalling || importingId ? (
                            <>
                              <Loader size={14} className="spin" /> 安装并导入中…
                            </>
                          ) : (
                            <>
                              <Download size={14} /> 安装并导入
                            </>
                          )}
                        </button>
                      </div>
                    </section>
                  </div>

                  <div className="community-actions">
                    <button
                      type="button"
                      className="btn-secondary"
                      onClick={onClose}
                      disabled={!!importingId}
                    >
                      取消
                    </button>
                  </div>
                </>
              )}

              {showResults && (
                <>
                  {scanError && (
                    <div
                      className="message-banner message-banner--error"
                      style={{ marginTop: 0 }}
                    >
                      <AlertCircle size={14} /> 扫描失败：{scanError}
                    </div>
                  )}

                  {importError && (
                    <div
                      className="message-banner message-banner--error"
                      style={{ marginTop: 0 }}
                    >
                      <AlertCircle size={14} /> 导入失败：{importError}
                    </div>
                  )}

                  {newPets.length > 0 ? (
                    <>
                      <div className="community-result-head">
                        <CheckCircle2 size={16} className="community-result-head__icon" />
                        <div>
                          <div className="community-result-title">
                            检测到 {newPets.length} 个新增或更新的形象
                          </div>
                          <div className="muted small">
                            选择一个导入到 HachimoDock，导入过程会在本地用 ffmpeg
                            将精灵图转换为视频。
                          </div>
                        </div>
                      </div>
                      <ul className="codex-pet-list">
                        {newPets.map((pet) => (
                          <CodexPetRow
                            key={pet.id}
                            pet={pet}
                            importingId={importingId}
                            onPick={onImport}
                            highlight
                          />
                        ))}
                      </ul>
                    </>
                  ) : (
                    <div className="empty-state">
                      <div>
                        <strong>没有检测到新的形象</strong>
                      </div>
                        <div className="muted small">
                        请确认已经通过社区安装入口或 npx 命令完成安装。如果只是复制了链接或命令，
                        可以返回上一步粘贴导入。
                      </div>
                    </div>
                  )}

                  {allPets && allPets.length > 0 && newPets.length === 0 && (
                    <details className="community-fallback">
                      <summary>查看本机所有 Codex pets ({allPets.length})</summary>
                      <ul className="codex-pet-list" style={{ marginTop: 10 }}>
                        {allPets.map((pet) => (
                          <CodexPetRow
                            key={pet.id}
                            pet={pet}
                            importingId={importingId}
                            onPick={onImport}
                          />
                        ))}
                      </ul>
                    </details>
                  )}

                  <div className="community-actions">
                    <button
                      type="button"
                      className="btn-secondary"
                      onClick={() => {
                        setNewPets(null);
                        setAllPets(null);
                        setScanError("");
                      }}
                      disabled={!!importingId || scanning}
                    >
                      返回
                    </button>
                    <button
                      type="button"
                      className="btn-secondary"
                      onClick={handleScan}
                      disabled={scanning || !!importingId}
                    >
                      {scanning ? (
                        <>
                          <Loader size={14} className="spin" /> 扫描中…
                        </>
                      ) : (
                        <>
                          <RefreshCw size={14} /> 重新扫描
                        </>
                      )}
                    </button>
                  </div>
                </>
              )}
            </>
          )}
        </div>
      </div>
    </div>
  );
}

/**
 * Live progress card for the background generation task. Reads from the global
 * generation-task subscription (handed in via props from the gallery), shows
 * per-family stage, and lets the user open the partially-saved appearance or
 * abort the run.
 */
function RunningTaskCard({ task, onAbort, onOpenDetail }) {
  const progress = task?.progress;
  const completed = progress?.completed ?? 0;
  const total = progress?.total ?? 0;
  const families = progress?.families ? Object.entries(progress.families) : [];
  const currentFamily = families.find(
    ([, v]) => v.status === "submitting" || v.status === "polling" || v.status === "downloading",
  );
  const stageMessage = progress?.message;
  const pct = total > 0 ? Math.min(100, Math.round((completed / total) * 100)) : 0;
  const partialId = task?.appearanceId;

  return (
    <div className="running-task-card">
      <div className="running-task-card__head">
        <Loader size={16} className="spin" />
        <div className="running-task-card__title">
          正在生成「{task?.appearanceName || "未命名形象"}」
        </div>
        <span className="muted small">{completed}/{total || "?"}</span>
      </div>
      <div className="running-task-card__bar">
        <div className="running-task-card__bar-fill" style={{ width: `${pct}%` }} />
      </div>
      <div className="muted small running-task-card__sub">
        {currentFamily
          ? `${currentFamily[0]} · ${STAGE_LABELS[currentFamily[1].status] || ""}`
          : stageMessage || "正在准备…"}
      </div>
      <div className="running-task-card__actions">
        {partialId && (
          <button className="btn-secondary btn-sm" onClick={() => onOpenDetail?.(partialId)}>
            <CheckCircle size={14} /> 查看已生成部分
          </button>
        )}
        <button className="btn-ghost btn-sm" onClick={onAbort}>
          <X size={14} /> 取消生成
        </button>
      </div>
      <div className="muted small running-task-card__hint">
        生成可后台进行，你可以切换页面或继续操作。关闭应用会中断生成。
      </div>
    </div>
  );
}

function AppearanceCard({
  row,
  isActive,
  onOpenDetail,
}) {
  const okCount = row.families?.filter?.((f) => f.ok).length || 0;
  const totalCount = row.families?.length || 0;
  const isCodex = row.type === "codex-import";
  const isBuiltin = row.type === "builtin";
  const previewMedia = resolveGalleryPreviewMedia(row);

  return (
    <article
      className={"appearance-card appearance-card--clickable" + (isActive ? " appearance-card--active is-active" : "")}
    >
      <div
        className={"appearance-channel-preview appearance-card-preview" + (isCodex ? " appearance-card-preview--codex" : "")}
        role="button"
        tabIndex={0}
        onClick={() => onOpenDetail?.(row.id)}
        onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onOpenDetail?.(row.id); } }}
      >
        <AppearancePreview
          media={previewMedia}
          className="appearance-channel-preview__media"
          emptyClassName="appearance-channel-preview__empty"
          playing={previewMedia.kind === "video"}
        />
        {isActive && (
          <span className="appearance-card__badge appearance-card__badge--active">
            <CheckCircle2 size={12} /> 使用中
          </span>
        )}
      </div>
      <div className="appearance-card-body">
        <div
          className="appearance-card-main"
          role="button"
          tabIndex={0}
          onClick={() => onOpenDetail?.(row.id)}
          onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); onOpenDetail?.(row.id); } }}
        >
          <h4>{row.name}</h4>
          <p>{row.description || (row.provider + " / " + (row.model || "未知模型"))}</p>
          <div className="muted small">
            {okCount}/{totalCount} 个动画可用 · {new Date(row.created_at).toLocaleString()}
          </div>
          <div className="appearance-card-tags">
            <span className="appearance-thumb__badge">
              {isBuiltin ? "内置" : isCodex ? "Codex" : "自定义"}
            </span>
          </div>
        </div>
      </div>
    </article>
  );
}
