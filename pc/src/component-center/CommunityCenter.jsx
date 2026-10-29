/** [Input] Formal local library, real device inventory and existing configuration actions.
 * [Output] Community discovery, local/favorite/creator tabs, moderated publication and verified download.
 * [Sync] Update component-center/.folder.md and server API tests for behavior changes. */
import React, { useCallback, useEffect, useRef, useState } from "react";
import { invoke } from "@tauri-apps/api/core";
import { useExitProtection } from "../lib/exit-protection.js";
import {
  ArrowDownToLine,
  ArrowUpRight,
  Bookmark,
  Check,
  ChevronRight,
  Heart,
  MessageCircle,
  Package,
  Pencil,
  Radio,
  Search,
  ShieldCheck,
  Sparkles,
  Upload,
  UserRound,
  X,
} from "lucide-react";
import Button from "../shell/Button.jsx";
import DeviceScreenPreview from "./DeviceScreenPreview.jsx";
import ComponentCover from "./ComponentCover.jsx";
import {componentDisplayAuthor} from "./component-authors.js";
import {refreshGate} from "./community-refresh.js";
import {readCenterPreference,saveCenterPreference} from './catalog-state.js';
import {commentStorage,readCommentIdentity,saveCommentName} from './comment-identity.js';
import {
  COMMUNITY_TABS,
  COMMUNITY_CATEGORIES,
  FAVORITES_KEY,
  NAME_KEY,
  CONTACT_URL,
  readFavorites,
  updateFavorites,
  versionState,
  publicationStatus,
  readableBytes,
} from "./community-model.js";
import "./community.css";
import WorkflowConfirm from './WorkflowConfirm.jsx';
import CommunityWeb from "./CommunityWeb.jsx";

const request = (action, fields = {}) =>
  invoke("community_request", { input: { action, ...fields } });
const errorText = (error) =>
  typeof error === "string" ? error : error?.message || "操作未完成，请重试";
const date = (seconds) => new Date(seconds * 1000).toLocaleDateString("zh-CN");
function displayName() {
  try {
    return localStorage.getItem(NAME_KEY) || "";
  } catch {
    return "";
  }
}
function useResource(load, dependencies, identityDependencies = dependencies) {
  const [state, setState] = useState({ loading: true, data: null, error: "" });
  const [revision, setRevision] = useState(0);
  const identity=JSON.stringify(identityDependencies);
  const previousIdentity=useRef(identity);
  useEffect(() => {
    let current = true;
    const sameRequest=previousIdentity.current===identity;
    previousIdentity.current=identity;
    setState(old=>({data:sameRequest?old.data:null,loading:true,error:""}));
    Promise.resolve()
      .then(load)
      .then(
        (data) => current && setState({ loading: false, data, error: "" }),
        (error) =>
          current &&
          setState(old=>({...old,loading:false,error:errorText(error)})),
      );
    return () => {
      current = false;
    };
    // Each caller lists the complete identity of its request. Late responses are ignored.
  }, [...dependencies, revision]);
  return { ...state, reload: () => setRevision((r) => r + 1) };
}
function Notice({ resource, children }) {
  if (resource.loading && resource.data == null)
    return (
      <div className="community-empty" role="status">
        正在加载社区…
      </div>
    );
  if (resource.error && resource.data == null)
    return (
      <div className="community-empty" role="alert">
        <strong>暂时无法连接社区</strong>
        <p>{resource.error}</p>
        <Button size="small" onClick={resource.reload}>
          重试
        </Button>
        <small>你仍然可以在“我的组件”使用本地内容。</small>
      </div>
    );
  return <>{resource.loading&&<p role="status">正在后台更新，以下暂为上次结果…</p>}{resource.error&&<p role="alert" className="community-error">更新失败，保留上次结果。{resource.error} <Button size="small" onClick={resource.reload}>重试</Button></p>}{children}</>;
}
function Dialog({ title, onClose, busy = false, children, wide = false }) {
  const ref = useRef(null),
    closeRef = useRef(onClose),
    busyRef = useRef(busy);
  closeRef.current = onClose;
  busyRef.current = busy;
  useEffect(() => {
    const previous = document.activeElement;
    ref.current?.focus();
    const key = (event) => {
      if (!ref.current?.getClientRects().length) return;
      if (event.key === "Escape" && !busyRef.current && !event.target.closest?.('[data-community-nickname-editor]')) {
        event.stopImmediatePropagation();
        closeRef.current();
      }
      if (event.key !== "Tab") return;
      const items = [
        ...(ref.current?.querySelectorAll(
          "button:not(:disabled),input:not(:disabled),select:not(:disabled),textarea:not(:disabled),a[href]",
        ) || []),
      ].filter((el) => el.getClientRects().length);
      const first = items[0],
        last = items.at(-1);
      if (!items.length) {
        event.preventDefault();
        return;
      }
      if (
        event.shiftKey &&
        (document.activeElement === first ||
          document.activeElement === ref.current)
      ) {
        event.preventDefault();
        last.focus();
      }
      if (
        !event.shiftKey &&
        (document.activeElement === last ||
          document.activeElement === ref.current)
      ) {
        event.preventDefault();
        first.focus();
      }
    };
    document.addEventListener("keydown", key, true);
    return () => {
      document.removeEventListener("keydown", key, true);
      if (previous?.isConnected) previous.focus();
    };
  }, []);
  return (
    <div
      className="modal-backdrop community-backdrop"
      onClick={() => !busy && onClose()}
    >
      <section
        ref={ref}
        tabIndex={-1}
        role="dialog"
        aria-modal="true"
        aria-label={title}
        className={`community-dialog ${wide ? "is-wide" : ""}`}
        onClick={(e) => e.stopPropagation()}
      >
        <header>
          <h2>{title}</h2>
          <Button
            variant="icon"
            aria-label="关闭"
            disabled={busy}
            onClick={onClose}
          >
            <X size={18} />
          </Button>
        </header>
        {children}
      </section>
    </div>
  );
}

function Publish({ localItems, owned, onClose, onPublished, selectedPath }) {
  const dirty=useRef(false);
  const submitting=useRef(false);
  const [discard,setDiscard]=useState(null);
  const close=()=>{if(dirty.current)setDiscard({close:true});else onClose();};
  const [path, setPath] = useState(selectedPath || ""),
    [prepared, setPrepared] = useState(null);
  const [form, setForm] = useState({
    author: displayName(),
    summary: "",
    description: "",
    category: "tool",
    license: "",
    releaseNotes: "",
    sourceUrl: "",
  });
  const [covers, setCovers] = useState([]),
    [rights, setRights] = useState(false),
    [privacy, setPrivacy] = useState(false),
    [tested, setTested] = useState(false);
  const [busy, setBusy] = useState(false),
    [checking, setChecking] = useState(false),
    [error, setError] = useState("");
  const pending = useRef(null),
    prepareSeq = useRef(0);
  useExitProtection(busy ? "社区发布尚未完成，请勿重复提交" : dirty.current ? "组件发布内容尚未提交" : "");
  useEffect(() => {
    const seq = ++prepareSeq.current;
    setPrepared(null);
    setError("");
    setCovers([]);setRights(false);setPrivacy(false);setTested(false);pending.current=null;dirty.current=false;
    if (!path) {
      setChecking(false);
      return;
    }
    setChecking(true);
    request("prepare", { path })
      .then(
        (value) => {
          if (seq !== prepareSeq.current) return;
          setPrepared(value);
          const old = owned[value.id];
          setForm((f) => ({
            ...f,
            author: old?.author || displayName(),
            sourceUrl: old?.sourceUrl || '',
            summary: old?.summary || value.description?.slice(0, 160) || "",
            description: old?.description || value.description || "",
            license: old?.license || "",
            category: old?.category || "tool",
            releaseNotes: "",
          }));
        },
        (error) => seq === prepareSeq.current && setError(errorText(error)),
      )
      .finally(() => seq === prepareSeq.current && setChecking(false));
    return () => {
      prepareSeq.current++;
    };
  }, [path]);
  async function readCovers(files) {
    const seq=prepareSeq.current;
    setError("");
    setCovers([]);
    if (
      !files.length ||
      files.length > 3 ||
      files.some(
        (file) =>
          file.size > 2 * 1024 * 1024 ||
          !["image/png", "image/jpeg", "image/webp"].includes(file.type),
      )
    ) {
      setError("请选择 1–3 张 PNG、JPEG 或 WebP 截图，每张不超过 2 MB");
      return;
    }
    try {
      const next=await Promise.all(
          files.map(
            (file) =>
              new Promise((resolve, reject) => {
                const reader = new FileReader();
                reader.onload = () => resolve(String(reader.result));
                reader.onerror = reject;
                reader.readAsDataURL(file);
              }),
          ),
        );
      if(seq===prepareSeq.current)setCovers(next);
    } catch {
      setError("截图读取失败，请重新选择");
    }
  }
  async function submit(event) {
    event.preventDefault();
    if (busy || submitting.current || !prepared?.canPublish || !rights || !privacy || !tested) return;
    submitting.current=true;
    setBusy(true);
    setError("");
    try {
      // Identical retries retain the same request ID, including after a restart.
      // Persist only a digest and ID, never package contents or credentials.
      const bytes = new TextEncoder().encode(
        JSON.stringify([prepared.contentHash, form, covers]),
      );
      const fingerprint = [
        ...new Uint8Array(await crypto.subtle.digest("SHA-256", bytes)),
      ]
        .map((x) => x.toString(16).padStart(2, "0"))
        .join("");
      const key = `pet-manager:community-publication:${prepared.id}`;
      let old;
      try {
        old = JSON.parse(localStorage.getItem(key) || "null");
      } catch {
        /* corrupt hint is not a credential */
      }
      if (pending.current?.fingerprint !== fingerprint)
        pending.current =
          old?.fingerprint === fingerprint
            ? old
            : { fingerprint, requestId: crypto.randomUUID() };
      localStorage.setItem(key, JSON.stringify(pending.current));
      const result = await request("publish", {
        path,
        metadata: form,
        covers: covers.map((url) => url.split(",")[1]),
        expectedContentHash: prepared.contentHash,
        requestId: pending.current.requestId,
        rightsConfirmed: true,
        privacyConfirmed: true,
      });
      localStorage.setItem(NAME_KEY, form.author.trim());
      localStorage.removeItem(key);
      onPublished(result);
    } catch (error) {
      setError(errorText(error));
    } finally {
      submitting.current=false;
      setBusy(false);
    }
  }
  const field = (key, title, limit, multiline = false) => (
    <label>
      {title}
      {multiline ? (
        <textarea
          rows={3}
          maxLength={limit}
          value={form[key]}
          required
          onChange={(e) => setForm({ ...form, [key]: e.target.value })}
        />
      ) : (
        <input
          maxLength={limit}
          value={form[key]}
          required={key !== "sourceUrl"}
          onChange={(e) => setForm({ ...form, [key]: e.target.value })}
        />
      )}
    </label>
  );
  return (
    <Dialog title="发布我的玩法" onClose={close} busy={busy||Boolean(discard)} wide>
      <form className="community-publish" onSubmit={submit} onChange={()=>{dirty.current=true;}}>
        <fieldset disabled={busy} style={{border:0,padding:0,minWidth:0}}>
        <div className="community-publish-intro">
          <Upload size={22} />
          <div>
            <h3>把创作分享给更多桌面</h3>
            <p>
              从本地正式组件库提交。新作品和更新均需审核，审核通过后才会公开。
            </p>
          </div>
        </div>
        <label>
          选择组件
          <select
            aria-label="选择组件"
            value={path}
            onChange={(e) => {if(dirty.current)setDiscard({path:e.target.value});else setPath(e.target.value);}}
            disabled={busy}
            required
          >
            <option value="">选择已经验证过的本地作品</option>
            {localItems
              .filter((item) => item.isLocal && item.libraryPath)
              .map((item) => (
                <option key={item.libraryPath} value={item.libraryPath}>
                  {item.name} · {item.version || "待检查版本"}
                </option>
              ))}
          </select>
        </label>
        {checking && <p role="status">正在检查包结构、授权文件与敏感内容…</p>}
        {prepared && (
          <div className="community-package-check">
            <ShieldCheck size={20} />
            <div>
              <strong>
                {prepared.name} · {prepared.version}
              </strong>
              <p>
                {prepared.id} · {readableBytes(prepared.packageBytes)} ·{" "}
                {prepared.files.length} 个文件
              </p>
              <small>{prepared.warning}</small>
              <details>
                <summary>查看将公开的文件</summary>
                <ul>
                  {prepared.files.map((name) => (
                    <li key={name}>{name}</li>
                  ))}
                </ul>
              </details>
            </div>
          </div>
        )}
        <div className="community-form-grid">
          {field("author", "公开作者名", 40)}
          <label>
            分类
            <select
              value={form.category}
              onChange={(e) => setForm({ ...form, category: e.target.value })}
            >
              {COMMUNITY_CATEGORIES.filter(([key]) => key).map(
                ([key, title]) => (
                  <option key={key} value={key}>
                    {title}
                  </option>
                ),
              )}
            </select>
          </label>
          {field("summary", "一句话介绍", 160)}
          {field("license", "许可证名称（与包内 LICENSE.txt 一致）", 100)}
        </div>
        {field("description", "玩法与使用说明", 3000, true)}
        {field("releaseNotes", "本次更新说明", 2000, true)}
        {field("sourceUrl", "来源链接（选填，派生作品请注明公开来源）", 500)}
        <label>
          真实组件截图 · 1–3 张
          <input
            key={path}
            type="file"
            accept="image/png,image/jpeg,image/webp"
            multiple
            disabled={busy}
            onChange={(e) => readCovers([...e.target.files])}
          />
        </label>
        <div className="community-publish-shots">
          {covers.map((src, index) => (
            <img key={index} src={src} alt={`发布截图 ${index + 1}`} />
          ))}
        </div>
        <div className="community-publish-checks">
          <label className="community-check">
            <input
              type="checkbox"
              checked={tested}
              onChange={(e) => setTested(e.target.checked)}
            />
            已在设备测试主要玩法、按键和退出功能
          </label>
          <label className="community-check">
            <input
              type="checkbox"
              checked={rights}
              onChange={(e) => setRights(e.target.checked)}
            />
            我有权发布代码、图片和音频，已保留必要署名及许可证
          </label>
          <label className="community-check">
            <input
              type="checkbox"
              checked={privacy}
              onChange={(e) => setPrivacy(e.target.checked)}
            />
            确认公开以上作者名、截图和文件，不包含 Key、证书、内部接口或个人信息
          </label>
        </div>
        {error && (
          <p className="community-error" role="alert">
            {error}
          </p>
        )}
        <footer>
          <small>
            需要下架或删除社区作品，请联系开发者。更新不会自动覆盖用户设备。
          </small>
          <Button
            type="submit"
            variant="primary"
            disabled={
              !prepared?.canPublish ||
              !covers.length ||
              !rights ||
              !privacy ||
              !tested ||
              checking
            }
            loading={busy}
            loadingLabel="校验并提交…"
          >
            提交审核 <ArrowUpRight size={16} />
          </Button>
        </footer>
        </fieldset>
      </form>
      {discard&&<WorkflowConfirm title="放弃未提交修改？" description="这会清除本次填写的内容、截图和授权勾选。已发布的作品不受影响。" confirmLabel={discard.close?'放弃并关闭':'放弃并切换'} onCancel={()=>setDiscard(null)} onConfirm={()=>{const action=discard;setDiscard(null);dirty.current=false;if(action.close)onClose();else setPath(action.path);}}/>}
    </Dialog>
  );
}

import { navigateTabs } from "../lib/tab-navigation.js";
import { isWithdrawnComponent } from "./library-order.js";
export default function CommunityCenter({
  active = true,
  tab: controlledTab,
  onTabChange,
  localItems,
  inventory,
  onOpenLocal,
  onCreate,
  onImported,
  refreshKey,
  children,
}) {
  const [localTab,setLocalTab]=useState('local');
  const tab=controlledTab??localTab,setTab=onTabChange||setLocalTab;
  const [revision, setRevision] = useState(0),
    [detail, setDetail] = useState(""),
    [publish, setPublish] = useState(null),
    [notice, setNotice] = useState("");
  const provenance = useResource(() => request("local"), [revision], []);
  const created = useResource(
    () => (tab === "created" ? request("mine") : null),
    [tab, revision],
    [tab],
  );
  const changed = useCallback(() => setRevision((v) => v + 1), []);
  const refreshClock = useRef(null);
  if(!refreshClock.current)refreshClock.current=refreshGate();
  useEffect(()=>{
    if(!active || !["discover","favorites","created"].includes(tab))return;
    const refresh=()=>{if(document.visibilityState!=="hidden"&&refreshClock.current())changed();};
    refresh();
    const timer=setInterval(refresh,60_000);
    window.addEventListener("focus",refresh);
    document.addEventListener("visibilitychange",refresh);
    return()=>{clearInterval(timer);window.removeEventListener("focus",refresh);document.removeEventListener("visibilitychange",refresh);};
  },[active,tab,changed]);
  useEffect(() => {
    if (refreshKey) changed();
  }, [refreshKey, changed]);
  const owned = provenance.data?.owned || {};
  const publicationItems=localItems.flatMap(item=>item.sourceAlternatives?.filter(source=>source.isLocal) || (item.isLocal?[item]:[]));
  const unpublished = publicationItems.filter(
    (item) =>
      item.isLocal &&
      !provenance.data?.reservedIds?.includes(item.id) &&
      !owned[item.id] &&
      !provenance.data?.downloaded?.[item.id],
  );
  return (
    <div className="community-center">
      <div className="community-tabs" role="tablist" aria-label="组件中心栏目">
        {COMMUNITY_TABS.map(([id, label]) => (
          <button
            key={id}
            role="tab"
            aria-selected={tab === id}
            tabIndex={tab === id ? 0 : -1}
            onKeyDown={event => navigateTabs(event, COMMUNITY_TABS.map(([key]) => key), id, setTab)}
            onClick={() => setTab(id)}
          >
            {label}
          </button>
        ))}
        <Button variant="secondary" size="small" onClick={() => setPublish({})}>
          <Upload size={14} />
          发布作品
        </Button>
      </div>
      {notice && (
        <div className="community-notice" role="status">
          <Check size={17} />
          {notice}
          <Button
            variant="icon"
            size="small"
            onClick={() => setNotice("")}
            aria-label="关闭提示"
          >
            <X size={14} />
          </Button>
        </div>
      )}
      {["discover","favorites"].includes(tab) && <CommunityWeb key={tab} view={tab} localItems={localItems} inventory={inventory} onImported={onImported} onOpenLocal={onOpenLocal}/>}
      {tab === "local" && (
        <section role="tabpanel" aria-label="我的组件">
          <div className="community-section-title">
            <div>
              <h2>我的组件</h2>
              <p>本机内容与设备同步状态，按你的习惯配置。</p>
            </div>
            <span>{localItems.length} 个可用组件</span>
          </div>
          {children}
        </section>
      )}
      {tab === "created" && (
        <section role="tabpanel" aria-label="我的创作">
          <div className="community-section-title">
            <div>
              <h2>我的创作</h2>
              <p>在本地打磨，在社区分享。更新需使用更高版本号。</p>
            </div>
            <Button variant="primary" onClick={onCreate}>
              <Sparkles size={15} />
              创建新玩法
            </Button>
          </div>
          <Notice resource={created}>
            <div className="community-creations">
              {created.data?.items?.filter(work => !isWithdrawnComponent(work)).map((work) => {
                const local = publicationItems.find(
                  (item) => item.id === work.id && item.libraryPath,
                );
                return (
                  <article key={work.id}>
                    <div>
                      <h3>
                        {work.name}
                        <span
                          className={`community-status community-status--${work.status}`}
                        >
                          {publicationStatus(work)}
                        </span>
                      </h3>
                      <p>
                        {work.version} ·{" "}
                        {work.published && work.status !== "approved"
                          ? `公开版本仍为 ${work.published.version}`
                          : work.summary}
                      </p>
                      {work.reviewReason && (
                        <p className="community-fine">
                          审核反馈：{work.reviewReason}
                        </p>
                      )}
                    </div>
                    <Button
                      disabled={!local || work.status === "pending"}
                      onClick={() => setPublish({ path: local?.libraryPath })}
                    >
                      {work.status === "pending" ? "等待审核" : "提交更新"}
                    </Button>
                  </article>
                );
              })}
              {unpublished.map((item) => (
                <article key={item.id}>
                  <div>
                    <h3>
                      {item.name}
                      <span className="community-status">本地创作</span>
                    </h3>
                    <p>{item.version || "版本待检查"} · 尚未提交社区</p>
                  </div>
                  <Button
                    onClick={() => setPublish({ path: item.libraryPath })}
                  >
                    检查并发布
                  </Button>
                </article>
              ))}
            </div>
            {!created.data?.items?.length && !unpublished.length && (
              <div className="community-empty">
                <Sparkles size={28} />
                <strong>从一个小想法开始</strong>
                <p>点击创建，让 Agent 使用 PetUI 生成组件；先验证，再分享。</p>
              </div>
            )}
          </Notice>
          <p className="community-fine">
            作品所有权由本机安全凭据识别，改显示名不会转移所有权。不提供权限导入导出；换机、凭据丢失或申请下架请联系开发者。
          </p>
          <Button
            variant="ghost"
            onClick={() =>
              invoke("open_external_url", { url: CONTACT_URL }).catch((e) =>
                setNotice(errorText(e)),
              )
            }
          >
            联系开发者 <ArrowUpRight size={14} />
          </Button>
          <Button variant="ghost" onClick={changed}>
            刷新审核状态
          </Button>
        </section>
      )}
      {publish && (
        <Publish
          localItems={publicationItems.filter(
            (item) =>
              !provenance.data?.reservedIds?.includes(item.id) &&
              (!provenance.data?.downloaded?.[item.id] || owned[item.id]),
          )}
          owned={owned}
          selectedPath={publish.path}
          onClose={() => setPublish(null)}
          onPublished={(work) => {
            setPublish(null);
            setTab("created");
            setNotice(`「${work.name}」已提交审核，通过后会公开上架。`);
            changed();
          }}
        />
      )}
    </div>
  );
}
