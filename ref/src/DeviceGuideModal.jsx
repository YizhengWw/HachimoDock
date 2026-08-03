/**
 * [Input] Static content from `./lib/device-guide-content.js` + isOpen / onClose
 *         props from DeviceDashboard.
 * [Output] First-launch + re-openable modal explaining device controls
 *         (screen switch, button map, widget takeover) in a compact, responsive
 *         3-step workspace with hardware-shaped visual cues and shared controls.
 * [Pos] standalone modal rendered by DeviceDashboard
 * [Sync] If this component changes, update `ref/src/.folder.md` and
 *        `./lib/device-guide-content.js` headline copy where relevant.
 */
import React, { useEffect, useState } from "react";
import {
  ArrowLeftRight,
  BarChart3,
  ChevronLeft,
  ChevronRight,
  Dog,
  LayoutGrid,
  Settings2,
  SlidersHorizontal,
  X,
} from "lucide-react";
import { CARDS, P4_CARDS, CONTROLS, SCREENS } from "./lib/device-guide-content.js";
import {
  clearOnboardingSeen,
  markOnboardingSeen,
  ONBOARDING_PAGE_IDS,
} from "./lib/onboarding-state.js";
import Button from "./shell/Button.jsx";
import Switch from "./shell/Switch.jsx";

function ControlKey({ control }) {
  const meta = CONTROLS[control];
  return (
    <span className="device-guide-control-key" aria-hidden="true">
      {meta.shortLabel || meta.emoji}
    </span>
  );
}

function ScreenGlyph({ screen }) {
  if (screen.icon === "pet") return <Dog size={24} strokeWidth={1.8} />;
  if (screen.icon === "components") return <LayoutGrid size={24} strokeWidth={1.8} />;
  return <BarChart3 size={24} strokeWidth={1.8} />;
}

function ControlRow({ control, rows }) {
  const meta = CONTROLS[control];
  return (
    <div className="device-guide-control-block">
      <div className="device-guide-control-head">
        <ControlKey control={control} />
        <span className="device-guide-control-name">{meta.name}</span>
      </div>
      <div className="device-guide-control-rows">
        {rows.map((row) => (
          <div key={`${row.gesture}-${row.action}`} className="device-guide-control-row">
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
  const screenIds = card.screenIds || ["main", "stats"];
  const screens = screenIds.map((id) => SCREENS[id]).filter(Boolean);
  const gestures = card.gestures || [
    { gesture: "短按", action: card.canonicalActionText },
    ...card.otherWays.map((way) => ({
      gesture: CONTROLS[way.control].name,
      action: way.text,
    })),
  ];

  return (
    <div className="device-guide-card">
      <div className="device-guide-intro">
        <span className="device-guide-intro__icon" aria-hidden="true">
          <ArrowLeftRight size={18} />
        </span>
        <p className="device-guide-headline">{card.headline}</p>
      </div>
      <div className="device-guide-screens">
        {screens.map((screen, screenIndex) => (
          <React.Fragment key={screen.id}>
            {screenIndex > 0 && (
              <div className="device-guide-screens-arrow" aria-hidden="true">
                <ChevronLeft size={13} />
                <span />
                <ChevronRight size={13} />
              </div>
            )}
            <div className="device-guide-screen">
              <div className="device-guide-screen-icon" aria-hidden="true">
                <ScreenGlyph screen={screen} />
              </div>
              <div>
                <div className="device-guide-screen-name">{screen.name}</div>
                <div className="device-guide-screen-id">{screen.id}</div>
              </div>
            </div>
          </React.Fragment>
        ))}
      </div>
      <div className="device-guide-gesture-grid">
        {gestures.map((item) => (
          <div key={`${item.gesture}-${item.action}`} className="device-guide-gesture-card">
            <span className="device-guide-gesture-card__label">{item.gesture}</span>
            <strong>{item.action}</strong>
          </div>
        ))}
      </div>
      {card.supportingText && (
        <p className="device-guide-supporting">{card.supportingText}</p>
      )}
    </div>
  );
}

function ControlsCard({ card }) {
  return (
    <div className="device-guide-card">
      {card.headline && <p className="device-guide-headline">{card.headline}</p>}
      <div className="device-guide-control-grid">
        {card.controls.map((block) => (
          <ControlRow key={block.control} control={block.control} rows={block.rows} />
        ))}
      </div>
    </div>
  );
}

function WidgetTakeoverCard({ card }) {
  return (
    <div className="device-guide-card">
      <div className="device-guide-intro">
        <span className="device-guide-intro__icon" aria-hidden="true">
          <SlidersHorizontal size={18} />
        </span>
        <p className="device-guide-headline">{card.headline}</p>
      </div>
      <div className="device-guide-example">
        <div className="device-guide-example-name">
          <Settings2 size={14} aria-hidden="true" />
          {card.example.name}
        </div>
        <div className="device-guide-control-rows">
          {card.example.rows.map((row) => (
            <div key={`${row.control}-${row.gesture}`} className="device-guide-control-row">
              <span className="device-guide-gesture">
                <ControlKey control={row.control} />
                {row.gesture}
              </span>
              <span className="device-guide-action">{row.action}</span>
            </div>
          ))}
        </div>
      </div>
      <ul className="device-guide-footnotes">
        {card.footnotes.map((note) => (
          <li key={note}>{note}</li>
        ))}
      </ul>
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

export default function DeviceGuideModal({ isOpen, onClose, runtime = "" }) {
  const [index, setIndex] = useState(0);
  const [dontShow, setDontShow] = useState(true);

  useEffect(() => {
    if (isOpen) {
      setIndex(0);
      setDontShow(true);
    }
  }, [isOpen, runtime]);

  if (!isOpen) return null;

  const cards = String(runtime).toLowerCase() === "esp-p4" ? P4_CARDS : CARDS;
  const card = cards[index];
  const isFirst = index === 0;
  const isLast = index === cards.length - 1;

  const handleClose = () => {
    if (dontShow) markOnboardingSeen(ONBOARDING_PAGE_IDS.DEVICE);
    else clearOnboardingSeen(ONBOARDING_PAGE_IDS.DEVICE);
    onClose();
  };

  return (
    <div className="modal-backdrop" onClick={handleClose}>
      <div
        className="modal-card device-guide-modal"
        role="dialog"
        aria-modal="true"
        aria-labelledby="device-guide-title"
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <div className="device-guide-title-block">
            <span className="device-guide-step-count">设备操作 · {index + 1}/{cards.length}</span>
            <h2 id="device-guide-title" className="modal-title">{card.title}</h2>
          </div>
          <button type="button" className="icon-btn" onClick={handleClose} aria-label="关闭引导">
            <X size={18} />
          </button>
        </div>

        <div className="device-guide-steps" role="tablist" aria-label="设备操作教程">
          {cards.map((candidate, candidateIndex) => (
            <button
              key={candidate.id}
              type="button"
              role="tab"
              className={`device-guide-step${candidateIndex === index ? " device-guide-step--active" : ""}`}
              onClick={() => setIndex(candidateIndex)}
              aria-selected={candidateIndex === index}
            >
              <span>{candidateIndex + 1}</span>
              {candidate.shortTitle || candidate.title}
            </button>
          ))}
        </div>

        <div className="modal-body" role="tabpanel" aria-labelledby="device-guide-title">
          <div key={card.id} className="device-guide-card-stage">
            <CardBody card={card} />
          </div>
        </div>

        <div className="device-guide-footer">
          <Switch
            checked={dontShow}
            onCheckedChange={setDontShow}
            label="下次不再自动弹出"
            className="device-guide-dontshow"
          />
          <div className="device-guide-nav">
            <Button
              variant="secondary"
              size="small"
              onClick={() => setIndex((current) => Math.max(0, current - 1))}
              disabled={isFirst}
            >
              <ChevronLeft size={14} /> 上一页
            </Button>
            {isLast ? (
              <Button variant="primary" size="small" onClick={handleClose}>
                完成
              </Button>
            ) : (
              <Button
                variant="primary"
                size="small"
                onClick={() => setIndex((current) => Math.min(cards.length - 1, current + 1))}
              >
                下一页 <ChevronRight size={14} />
              </Button>
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
