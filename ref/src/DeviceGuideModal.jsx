/**
 * [Input] Static content from `./lib/device-guide-content.js` + isOpen / onClose
 *         props from DeviceDashboard.
 * [Output] First-launch + re-openable modal explaining device controls
 *         (screen switch, button map, widget takeover) in a 3-card carousel.
 * [Pos] standalone modal rendered by DeviceDashboard
 * [Sync] If this component changes, update `ref/src/.folder.md` and
 *        `./lib/device-guide-content.js` headline copy where relevant.
 */
import React, { useEffect, useState } from "react";
import { ChevronLeft, ChevronRight, ChevronDown, X } from "lucide-react";
import { CARDS, CONTROLS, SCREENS, DEVICE_GUIDE_SEEN_KEY } from "./lib/device-guide-content.js";

function ControlRow({ control, rows }) {
  const meta = CONTROLS[control];
  return (
    <div className="device-guide-control-block">
      <div className="device-guide-control-head">
        <span className="device-guide-control-emoji">{meta.emoji}</span>
        <span className="device-guide-control-name">{meta.name}</span>
      </div>
      <div className="device-guide-control-rows">
        {rows.map((row, i) => (
          <div key={i} className="device-guide-control-row">
            <span className="device-guide-gesture">{row.gesture}</span>
            <span className={`device-guide-action${row.warning ? " device-guide-action--warning" : ""}`}>
              {row.action}
            </span>
          </div>
        ))}
      </div>
    </div>
  );
}

function ScreenSwitchCard({ card }) {
  const [otherOpen, setOtherOpen] = useState(false);
  const main = SCREENS.main;
  const stats = SCREENS.stats;
  const canonical = CONTROLS[card.canonicalControl];
  return (
    <div className="device-guide-card">
      <p className="device-guide-headline">{card.headline}</p>
      <div className="device-guide-screens">
        <div className="device-guide-screen">
          <div className="device-guide-screen-emoji">{main.emoji}</div>
          <div className="device-guide-screen-name">{main.name}</div>
          <div className="device-guide-screen-id">({main.id})</div>
        </div>
        <div className="device-guide-screens-arrow">◀──▶</div>
        <div className="device-guide-screen">
          <div className="device-guide-screen-emoji">{stats.emoji}</div>
          <div className="device-guide-screen-name">{stats.name}</div>
          <div className="device-guide-screen-id">({stats.id})</div>
        </div>
      </div>
      <div className="device-guide-canonical">
        <span className="device-guide-control-emoji">{canonical.emoji}</span>
        <span>{card.canonicalActionText}</span>
      </div>
      <button
        type="button"
        className="device-guide-other-toggle"
        onClick={() => setOtherOpen((v) => !v)}
        aria-expanded={otherOpen}
      >
        <ChevronDown
          size={14}
          className={`device-guide-other-chevron${otherOpen ? " device-guide-other-chevron--open" : ""}`}
        />
        其他切换方式{otherOpen ? "" : "（点击展开）"}
      </button>
      {otherOpen && (
        <ul className="device-guide-other-list">
          {card.otherWays.map((way, i) => (
            <li key={i}>
              <span className="device-guide-control-emoji">{CONTROLS[way.control].emoji}</span>
              <span>{way.text}</span>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}

function ControlsCard({ card }) {
  return (
    <div className="device-guide-card">
      {card.controls.map((block) => (
        <ControlRow key={block.control} control={block.control} rows={block.rows} />
      ))}
    </div>
  );
}

function WidgetTakeoverCard({ card }) {
  return (
    <div className="device-guide-card">
      <p className="device-guide-headline">{card.headline}</p>
      <div className="device-guide-example">
        <div className="device-guide-example-name">例：{card.example.name}</div>
        <div className="device-guide-control-rows">
          {card.example.rows.map((row, i) => (
            <div key={i} className="device-guide-control-row">
              <span className="device-guide-gesture">
                <span className="device-guide-control-emoji">{CONTROLS[row.control].emoji}</span>
                {row.gesture}
              </span>
              <span className="device-guide-action">→ {row.action}</span>
            </div>
          ))}
        </div>
      </div>
      {card.footnotes.map((note, i) => (
        <p key={i} className="device-guide-footnote">{note}</p>
      ))}
    </div>
  );
}

function CardBody({ card }) {
  switch (card.id) {
    case "screen-switch":
      return <ScreenSwitchCard card={card} />;
    case "controls":
      return <ControlsCard card={card} />;
    case "widget-takeover":
      return <WidgetTakeoverCard card={card} />;
    default:
      return null;
  }
}

export default function DeviceGuideModal({ isOpen, onClose }) {
  const [index, setIndex] = useState(0);
  const [dontShow, setDontShow] = useState(true);

  // Reset to card 1 whenever the modal re-opens. Keep the "don't show again"
  // pre-checked since most users want it dismissed after reading once.
  useEffect(() => {
    if (isOpen) {
      setIndex(0);
      setDontShow(true);
    }
  }, [isOpen]);

  if (!isOpen) return null;

  const card = CARDS[index];
  const isFirst = index === 0;
  const isLast = index === CARDS.length - 1;

  const handleClose = () => {
    if (dontShow) {
      try {
        localStorage.setItem(DEVICE_GUIDE_SEEN_KEY, "1");
      } catch {
        // localStorage可能在 Tauri 隔离场景下失败 — 忽略,modal 下次照常弹
      }
    }
    onClose();
  };

  return (
    <div className="modal-backdrop" onClick={handleClose}>
      <div className="modal-card device-guide-modal" onClick={(e) => e.stopPropagation()}>
        <div className="modal-header">
          <div className="modal-title">{card.title}</div>
          <button type="button" className="icon-btn" onClick={handleClose} aria-label="关闭">
            <X size={16} />
          </button>
        </div>
        <div className="modal-body">
          <CardBody card={card} />
        </div>
        <div className="device-guide-dots" role="tablist" aria-label="教程页">
          {CARDS.map((c, i) => (
            <button
              key={c.id}
              type="button"
              className={`device-guide-dot${i === index ? " device-guide-dot--active" : ""}`}
              onClick={() => setIndex(i)}
              aria-label={`第 ${i + 1} 页`}
              aria-selected={i === index}
            />
          ))}
        </div>
        <div className="device-guide-footer">
          <button type="button" className="device-guide-skip" onClick={handleClose}>
            跳过
          </button>
          <div className="device-guide-nav">
            <button
              type="button"
              className="device-guide-nav-btn"
              onClick={() => setIndex((i) => Math.max(0, i - 1))}
              disabled={isFirst}
            >
              <ChevronLeft size={14} /> 上一张
            </button>
            {isLast ? (
              <button type="button" className="device-guide-nav-btn device-guide-nav-btn--primary" onClick={handleClose}>
                完成
              </button>
            ) : (
              <button
                type="button"
                className="device-guide-nav-btn device-guide-nav-btn--primary"
                onClick={() => setIndex((i) => Math.min(CARDS.length - 1, i + 1))}
              >
                下一张 <ChevronRight size={14} />
              </button>
            )}
          </div>
          <label className="device-guide-dontshow">
            <input
              type="checkbox"
              checked={dontShow}
              onChange={(e) => setDontShow(e.target.checked)}
            />
            <span>不再显示</span>
          </label>
        </div>
      </div>
    </div>
  );
}
