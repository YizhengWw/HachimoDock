/**
 * [Input] component { dashboard, scene?, gameType? } from builtins or a validated formal local .clawpkg.
 * [Output] Renders the shared device preview, including palette/layout-aware
 *          classic dashboards, palette-keyed pixel tool panels, static pixel
 *          presets, and viewport-bounded P4 blocks/snake/flappy previews.
 * [Pos] component node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import React, { useEffect, useMemo, useRef, useState } from "react";
import {
  Activity,
  Clock3,
  Coffee,
  Cpu,
  Droplets,
  Focus,
  Gauge,
  Keyboard,
  TimerReset,
} from "lucide-react";

const PIXEL_PALETTES = new Set([
  "candy", "sunset", "mint", "arcade", "ocean", "forest", "ember", "mono",
]);
const PIXEL_LAYOUTS = new Set(["arcade", "scoreboard", "tool"]);
const PIXEL_TOOL_ACCENTS = {
  candy: "amber",
  sunset: "coral",
  mint: "aqua",
  arcade: "violet",
  ocean: "aqua",
  forest: "coral",
  ember: "coral",
  mono: "aqua",
};
const PIXEL_TOOL_ICONS = {
  timer: {
    MainIcon: TimerReset,
    ContextIcon: Focus,
  },
  droplet: {
    MainIcon: Droplets,
    ContextIcon: Clock3,
  },
  gauge: {
    MainIcon: Gauge,
    ContextIcon: Cpu,
  },
  coffee: {
    MainIcon: Coffee,
    ContextIcon: Clock3,
  },
};
const PIXEL_TOOL_SPRITE_ICONS = {
  coffee: Coffee,
  timer: TimerReset,
  droplet: Droplets,
  gauge: Gauge,
  target: Focus,
};
const PIXEL_SPRITES = {
  target: [
    "000222000",
    "002111200",
    "021222120",
    "212111212",
    "212131212",
    "212111212",
    "021222120",
    "002111200",
    "000222000",
  ],
  trophy: [
    "001111100",
    "211111112",
    "221111122",
    "022111220",
    "002111200",
    "000111000",
    "000111000",
    "001111100",
    "011111110",
  ],
  star: [
    "000100000",
    "000100000",
    "010111010",
    "001111100",
    "111141111",
    "001111100",
    "010111010",
    "000101000",
    "000100000",
  ],
  bolt: [
    "000111000",
    "001111000",
    "011110000",
    "111111000",
    "001111100",
    "000111110",
    "000011100",
    "000011000",
    "000010000",
  ],
  coffee: [
    "001010000",
    "000101000",
    "011111020",
    "011111022",
    "011111020",
    "011111000",
    "001111000",
    "000000000",
    "011111110",
  ],
  timer: [
    "000111000",
    "001111100",
    "000111000",
    "011111110",
    "110000011",
    "110030011",
    "110000011",
    "011111110",
    "001111100",
  ],
  droplet: [
    "000010000",
    "000111000",
    "001111100",
    "011111110",
    "111111111",
    "111111111",
    "011111110",
    "001111100",
    "000111000",
  ],
  gauge: [
    "001111100",
    "011111110",
    "110000011",
    "100000001",
    "100030001",
    "100010001",
    "110111011",
    "011111110",
    "001111100",
  ],
  blocks: [
    "110022200",
    "110002000",
    "000002000",
    "033333000",
    "000330000",
    "000330044",
    "000000044",
    "111122220",
    "111122220",
  ],
  snake: [
    "000000000",
    "011100000",
    "010100000",
    "010111000",
    "000001000",
    "000001330",
    "000000303",
    "000222300",
    "000222000",
  ],
  flappy: [
    "000000000",
    "000000000",
    "000011100",
    "000111110",
    "001114440",
    "011112220",
    "001111100",
    "000011000",
    "000000000",
  ],
  "mole-ready": [
    "000000000000000",
    "000000000000000",
    "000000000000000",
    "000000000000000",
    "000000000000000",
    "000000000000000",
    "033000330003300",
    "333303333033330",
    "033000330003300",
  ],
  "mole-left": [
    "100100000000000",
    "111100000000000",
    "141400000000000",
    "122100000000000",
    "011000000000000",
    "011000000000000",
    "033000330003300",
    "333303333033330",
    "033000330003300",
  ],
  "mole-center": [
    "000001001000000",
    "000001111000000",
    "000001414000000",
    "000001221000000",
    "000000110000000",
    "000000110000000",
    "033000330003300",
    "333303333033330",
    "033000330003300",
  ],
  "mole-right": [
    "000000000010010",
    "000000000011110",
    "000000000014140",
    "000000000012210",
    "000000000001100",
    "000000000001100",
    "033000330003300",
    "333303333033330",
    "033000330003300",
  ],
};

function normalizeProgress(progress) {
  if (!progress) return null;
  const [rawValue, rawLabel] = typeof progress === "string"
    ? progress.split(":", 2)
    : [progress.value, progress.label];
  const raw = Number(rawValue);
  if (!Number.isFinite(raw)) return null;
  return { value: Math.max(0, Math.min(100, raw)), label: rawLabel };
}

function normalizePixelPreset(value, allowed, fallback) {
  return allowed.has(value) ? value : fallback;
}

function PixelSprite({ name }) {
  const rows = PIXEL_SPRITES[name] || PIXEL_SPRITES.target;
  const columns = rows[0]?.length || 9;
  return (
    <div
      className="cds-pixel-sprite"
      data-wide={columns > 9 || undefined}
      style={{
        "--sprite-columns": columns,
        "--sprite-rows": rows.length,
        aspectRatio: `${columns} / ${rows.length}`,
      }}
      aria-hidden="true"
    >
      {rows.flatMap((row, y) => Array.from(row, (tone, x) => (
        <i
          className={`cds-pixel-sprite__cell cds-pixel-sprite__cell--${tone}`}
          key={`${x}-${y}`}
        />
      )))}
    </div>
  );
}

function blocksPreviewCells(step) {
  const width = 10;
  const height = 16;
  const cells = Array(width * height).fill(0);
  const floor = [
    [0, 15, 1], [1, 15, 1], [2, 15, 2], [3, 15, 2],
    [6, 15, 3], [7, 15, 3], [8, 15, 4], [9, 15, 4],
    [0, 14, 1], [3, 14, 2], [7, 14, 3], [9, 14, 4],
  ];
  floor.forEach(([x, y, tone]) => { cells[y * width + x] = tone; });
  const phase = step % 18;
  const y = Math.min(11, Math.floor(phase / 2));
  const x = phase < 12 ? 3 : 5;
  const rotation = Math.floor(step / 6) % 2;
  const piece = rotation
    ? [[1, 0], [1, 1], [1, 2], [2, 2]]
    : [[0, 1], [1, 1], [2, 1], [2, 0]];
  piece.forEach(([px, py]) => {
    const cellY = y + py;
    if (cellY < height) cells[cellY * width + x + px] = 1;
  });
  return { width, height, cells };
}

const SNAKE_PREVIEW_PATH = [
  [2, 2], [3, 2], [4, 2], [5, 2], [6, 2], [7, 2], [8, 2], [9, 2],
  [10, 2], [11, 2], [12, 2], [13, 2], [13, 3], [13, 4], [13, 5],
  [13, 6], [13, 7], [12, 7], [11, 7], [10, 7], [9, 7], [8, 7],
  [7, 7], [6, 7], [5, 7], [4, 7], [3, 7], [2, 7], [2, 6], [2, 5],
  [2, 4], [2, 3],
];

function snakePreviewCells(step) {
  const width = 16;
  const height = 10;
  const cells = Array(width * height).fill(0);
  const head = step % SNAKE_PREVIEW_PATH.length;
  for (let offset = 0; offset < 6; offset += 1) {
    const pathIndex = (head - offset + SNAKE_PREVIEW_PATH.length) % SNAKE_PREVIEW_PATH.length;
    const [x, y] = SNAKE_PREVIEW_PATH[pathIndex];
    cells[y * width + x] = offset === 0 ? 3 : 1;
  }
  const [foodX, foodY] = SNAKE_PREVIEW_PATH[(head + 7) % SNAKE_PREVIEW_PATH.length];
  cells[foodY * width + foodX] = 2;
  return { width, height, cells };
}

function flappyPreviewCells(step) {
  const width = 16;
  const height = 10;
  const cells = Array(width * height).fill(0);
  const birdYPath = [5, 5, 4, 4, 4, 5, 5, 6, 6, 5];
  const birdY = birdYPath[step % birdYPath.length];
  const pipeOffset = step % 18;
  const pipes = [
    { x: 14 - pipeOffset, gapTop: 2 },
    { x: 23 - pipeOffset, gapTop: 4 },
    { x: 32 - pipeOffset, gapTop: 1 },
  ];
  pipes.forEach(({ x, gapTop }) => {
    for (let px = 0; px < 2; px += 1) {
      const pipeX = x + px;
      if (pipeX < 0 || pipeX >= width) continue;
      for (let y = 0; y < height; y += 1) {
        if (y >= gapTop && y < gapTop + 4) continue;
        const cap = y === gapTop - 1 || y === gapTop + 4;
        cells[y * width + pipeX] = cap ? 2 : 1;
      }
    }
  });
  cells[birdY * width + 2] = 4;
  cells[birdY * width + 3] = 3;
  return { width, height, cells };
}

function scenePreviewCells(scene, step) {
  const width = Math.max(4, Math.min(16, Number(scene?.grid?.width) || 8));
  const height = Math.max(4, Math.min(16, Number(scene?.grid?.height) || 8));
  const rows = Array.isArray(scene?.grid?.rows) ? scene.grid.rows : [];
  const cells = Array(width * height).fill(0);
  const entities = [];
  rows.slice(0, height).forEach((row, y) => {
    Array.from(String(row).slice(0, width)).forEach((tone, x) => {
      cells[y * width + x] = Math.max(0, Math.min(4, Number(tone) || 0));
    });
  });
  const previewStep = Math.max(0, step % 64);
  (Array.isArray(scene?.entities) ? scene.entities : []).forEach((entity) => {
    if (entity?.active === false) return;
    const entityWidth = Math.max(1, Math.min(8, Number(entity?.width) || 1));
    const entityHeight = Math.max(1, Math.min(8, Number(entity?.height) || 1));
    const maxX = Math.max(0, width - entityWidth);
    const maxY = Math.max(0, height - entityHeight);
    const originX = Math.max(0, Math.min(maxX, Number(entity?.x) || 0));
    const originY = Math.max(0, Math.min(maxY, Number(entity?.y) || 0));
    const vx = Math.max(-4, Math.min(4, Number(entity?.vx) || 0));
    const vy = Math.max(-4, Math.min(4, Number(entity?.vy) || 0));
    const resolveAxis = (origin, velocity, maximum) => {
      if (!velocity || maximum <= 0) return origin;
      const raw = origin + velocity * previewStep;
      if (entity?.bounds === "wrap") return ((raw % (maximum + 1)) + maximum + 1) % (maximum + 1);
      if (entity?.bounds === "bounce") {
        const period = maximum * 2;
        const phase = ((raw % period) + period) % period;
        return phase > maximum ? period - phase : phase;
      }
      return Math.max(0, Math.min(maximum, raw));
    };
    const x = resolveAxis(originX, vx, maxX);
    const y = resolveAxis(originY, vy, maxY);
    const tone = Math.max(1, Math.min(4, Number(entity?.tone) || 1));
    entities.push({
      x,
      y,
      width: entityWidth,
      height: entityHeight,
      tone,
      shape: entity?.shape || "rect",
    });
  });
  return { width, height, cells, entities };
}

function usePreviewAnimationAllowed(containerRef, enabled) {
  const [nearViewport, setNearViewport] = useState(false);
  const [documentVisible, setDocumentVisible] = useState(
    () => typeof document === "undefined" || document.visibilityState === "visible",
  );

  useEffect(() => {
    if (typeof document === "undefined") return undefined;
    const handleVisibilityChange = () => {
      setDocumentVisible(document.visibilityState === "visible");
    };
    document.addEventListener("visibilitychange", handleVisibilityChange);
    return () => document.removeEventListener("visibilitychange", handleVisibilityChange);
  }, []);

  useEffect(() => {
    if (!enabled) {
      setNearViewport(false);
      return undefined;
    }
    const container = containerRef.current;
    if (!container || typeof IntersectionObserver !== "function") {
      setNearViewport(Boolean(container));
      return undefined;
    }

    setNearViewport(false);
    const observer = new IntersectionObserver(
      ([entry]) => setNearViewport(Boolean(entry?.isIntersecting)),
      { rootMargin: "160px 0px" },
    );
    observer.observe(container);
    return () => observer.disconnect();
  }, [containerRef, enabled]);

  return enabled && documentVisible && nearViewport;
}

function useGamePreviewStep(gameType, active) {
  const [step, setStep] = useState(0);
  useEffect(() => {
    setStep(0);
    if (!gameType || !active || typeof window === "undefined") return undefined;
    const reducedMotion = window.matchMedia?.("(prefers-reduced-motion: reduce)")?.matches;
    if (reducedMotion) return undefined;
    const delay = gameType === "flappy" ? 120 : gameType === "snake" ? 180 : 260;
    const timer = window.setInterval(() => setStep((value) => value + 1), delay);
    return () => window.clearInterval(timer);
  }, [active, gameType]);
  return step;
}

const pixelCanvasColorCache = new Map();

function resolvePixelCanvasColors(canvas, type) {
  const shell = canvas.closest(".cds-pixel-shell") || canvas;
  const shellStyle = getComputedStyle(shell);
  const paletteKey = [
    type,
    shellStyle.getPropertyValue("--pixel-bg"),
    shellStyle.getPropertyValue("--pixel-primary"),
    shellStyle.getPropertyValue("--pixel-secondary"),
    shellStyle.getPropertyValue("--pixel-ink"),
    shellStyle.getPropertyValue("--pixel-light"),
  ].join("|");
  const cached = pixelCanvasColorCache.get(paletteKey);
  if (cached) return cached;

  const candidates = type === "flappy"
    ? ["#8fd8f7", "#37a849", "#9be45b", "#f3a51f", "#fff1a6"]
    : [
        "color-mix(in srgb, var(--pixel-bg) 75%, var(--pixel-light))",
        "var(--pixel-primary)",
        "var(--pixel-secondary)",
        "var(--pixel-ink)",
        "var(--pixel-light)",
      ];
  const probe = document.createElement("span");
  probe.hidden = true;
  shell.appendChild(probe);
  const colors = candidates.map((candidate) => {
    probe.style.color = candidate;
    return getComputedStyle(probe).color;
  });
  probe.remove();
  pixelCanvasColorCache.set(paletteKey, colors);
  return colors;
}

function roundedCanvasRect(context, x, y, width, height, radius) {
  const boundedRadius = Math.max(0, Math.min(radius, width / 2, height / 2));
  context.beginPath();
  context.moveTo(x + boundedRadius, y);
  context.lineTo(x + width - boundedRadius, y);
  context.quadraticCurveTo(x + width, y, x + width, y + boundedRadius);
  context.lineTo(x + width, y + height - boundedRadius);
  context.quadraticCurveTo(x + width, y + height, x + width - boundedRadius, y + height);
  context.lineTo(x + boundedRadius, y + height);
  context.quadraticCurveTo(x, y + height, x, y + height - boundedRadius);
  context.lineTo(x, y + boundedRadius);
  context.quadraticCurveTo(x, y, x + boundedRadius, y);
  context.closePath();
}

function drawScenePreviewEntity(context, entity, cellWidth, cellHeight, gap, colors, cleanStyle) {
  const x = entity.x * (cellWidth + gap) + (cleanStyle ? 1.5 : 1);
  const y = entity.y * (cellHeight + gap) + (cleanStyle ? 1.5 : 1);
  const width = entity.width * cellWidth + (entity.width - 1) * gap - (cleanStyle ? 3 : 2);
  const height = entity.height * cellHeight + (entity.height - 1) * gap - (cleanStyle ? 3 : 2);
  if (width < 2 || height < 2) return;
  const centerX = x + width / 2;
  const centerY = y + height / 2;
  const color = colors[entity.tone] || colors[1];
  const detail = entity.tone === 4 ? colors[3] : colors[4];
  context.save();
  context.fillStyle = color;
  switch (entity.shape) {
    case "player-ship":
      context.beginPath();
      context.moveTo(centerX, y);
      context.lineTo(x + width, y + height);
      context.lineTo(x, y + height);
      context.closePath();
      context.fill();
      context.fillStyle = detail;
      context.beginPath();
      context.ellipse(centerX, y + height * 0.43, Math.max(1, width * 0.08), Math.max(1, height * 0.12), 0, 0, Math.PI * 2);
      context.fill();
      context.fillStyle = colors[2];
      context.fillRect(centerX - 1, y + height - 2, 2, 2);
      break;
    case "enemy-ship":
      context.beginPath();
      context.moveTo(x, y);
      context.lineTo(x + width, y);
      context.lineTo(centerX, y + height);
      context.closePath();
      context.fill();
      context.fillStyle = detail;
      context.beginPath();
      context.ellipse(centerX, y + height * 0.35, Math.max(1, width * 0.08), Math.max(1, height * 0.12), 0, 0, Math.PI * 2);
      context.fill();
      break;
    case "bullet": {
      const bulletWidth = Math.max(2, Math.min(8, width / 3));
      roundedCanvasRect(context, centerX - bulletWidth / 2, y, bulletWidth, height, bulletWidth / 2);
      context.fill();
      break;
    }
    case "star": {
      const outer = Math.min(width, height) / 2;
      const inner = outer * 0.44;
      context.beginPath();
      for (let point = 0; point < 10; point += 1) {
        const radius = point % 2 === 0 ? outer : inner;
        const angle = -Math.PI / 2 + point * Math.PI / 5;
        const px = centerX + Math.cos(angle) * radius;
        const py = centerY + Math.sin(angle) * radius;
        if (point === 0) context.moveTo(px, py);
        else context.lineTo(px, py);
      }
      context.closePath();
      context.fill();
      break;
    }
    case "paddle":
      roundedCanvasRect(context, x, y, width, height, Math.min(width, height) / 3);
      context.fill();
      break;
    case "ball":
      context.beginPath();
      context.ellipse(centerX, centerY, width / 2, height / 2, 0, 0, Math.PI * 2);
      context.fill();
      break;
    case "rect":
    default:
      if (cleanStyle) {
        roundedCanvasRect(context, x, y, width, height, Math.min(5, width / 5, height / 5));
        context.fill();
      } else {
        context.fillRect(x, y, width, height);
      }
      break;
  }
  context.restore();
}

function PixelGameGrid({ type, step, scene, visualStyle }) {
  const canvasRef = useRef(null);
  const frame = useMemo(
    () => (
      scene
        ? scenePreviewCells(scene, step)
        : type === "snake"
        ? snakePreviewCells(step)
        : type === "flappy"
          ? flappyPreviewCells(step)
          : blocksPreviewCells(step)
    ),
    [scene, type, step],
  );

  useEffect(() => {
    const canvas = canvasRef.current;
    if (!canvas || typeof document === "undefined") return;
    const style = getComputedStyle(canvas);
    const horizontalPadding = parseFloat(style.paddingLeft || "0")
      + parseFloat(style.paddingRight || "0");
    const verticalPadding = parseFloat(style.paddingTop || "0")
      + parseFloat(style.paddingBottom || "0");
    const cssWidth = Math.max(1, canvas.clientWidth - horizontalPadding);
    const cssHeight = Math.max(1, canvas.clientHeight - verticalPadding);
    const pixelRatio = Math.min(2, Math.max(1, window.devicePixelRatio || 1));
    const bitmapWidth = Math.max(1, Math.round(cssWidth * pixelRatio));
    const bitmapHeight = Math.max(1, Math.round(cssHeight * pixelRatio));
    if (canvas.width !== bitmapWidth) canvas.width = bitmapWidth;
    if (canvas.height !== bitmapHeight) canvas.height = bitmapHeight;

    const context = canvas.getContext("2d", { alpha: false });
    if (!context) return;
    context.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
    const colors = resolvePixelCanvasColors(canvas, type);
    const cleanStyle = visualStyle === "clean";
    const gap = cleanStyle ? 0 : 1;
    const cellWidth = Math.max(0.5, (cssWidth - gap * (frame.width - 1)) / frame.width);
    const cellHeight = Math.max(0.5, (cssHeight - gap * (frame.height - 1)) / frame.height);
    context.fillStyle = colors[3];
    context.fillRect(0, 0, cssWidth, cssHeight);
    frame.cells.forEach((tone, index) => {
      const x = index % frame.width;
      const y = Math.floor(index / frame.width);
      context.fillStyle = colors[tone] || colors[0];
      context.fillRect(
        x * (cellWidth + gap),
        y * (cellHeight + gap),
        Math.ceil(cellWidth),
        Math.ceil(cellHeight),
      );
    });
    (frame.entities || []).forEach((entity) => {
      drawScenePreviewEntity(context, entity, cellWidth, cellHeight, gap, colors, cleanStyle);
    });
  }, [frame, type, visualStyle]);

  return (
    <canvas
      ref={canvasRef}
      className={`cds-pixel-game-grid cds-pixel-game-grid--${type}`}
      style={scene?.grid ? { aspectRatio: `${scene.grid.width} / ${scene.grid.height}` } : undefined}
      aria-hidden="true"
    />
  );
}
function PixelGamePreview({ dashboard, progress, gameType, scene, animate }) {
  const palette = normalizePixelPreset(dashboard.visualPalette, PIXEL_PALETTES, "candy");
  const layout = normalizePixelPreset(dashboard.visualLayout, PIXEL_LAYOUTS, "arcade");
  const visualStyle = dashboard.visualStyle === "clean" ? "clean" : "pixel";
  const sprite = Object.hasOwn(PIXEL_SPRITES, dashboard.visualSprite)
    ? dashboard.visualSprite
    : "target";
  const boundedGameType = ["blocks", "snake", "flappy"].includes(gameType) ? gameType : "";
  const hasScene = Boolean(scene?.grid && Array.isArray(scene?.entities));
  const previewType = hasScene ? "bounded" : boundedGameType;
  const previewStep = useGamePreviewStep(previewType, animate);
  return (
    <div
      className="cds-pixel-shell"
      data-palette={palette}
      data-layout={layout}
      data-style={visualStyle}
      data-game-type={previewType || undefined}
    >
      <div className="cds-pixel-cloud cds-pixel-cloud--one" aria-hidden="true" />
      <div className="cds-pixel-cloud cds-pixel-cloud--two" aria-hidden="true" />
      <header className="cds-pixel-topbar">
        <strong>{dashboard.title || "PIXEL GAME"}</strong>
        <span>
          {previewType
            ? (dashboard.headline || dashboard.note || "READY")
            : (dashboard.note || dashboard.eyebrow || "READY")}
        </span>
      </header>
      <div className="cds-pixel-stage">
        <section className={`cds-pixel-playfield ${previewType ? "cds-pixel-playfield--game" : ""}`}>
          {previewType
            ? <PixelGameGrid type={previewType} step={previewStep} scene={hasScene ? scene : null} visualStyle={visualStyle} />
            : <PixelSprite name={sprite} />}
          {!previewType && <b>{dashboard.headline || "待开始"}</b>}
        </section>
        <section
          className="cds-pixel-score"
          data-metric-length={String(dashboard.metricValue || "0").length}
        >
          {dashboard.badge && <em className="cds-pixel-badge">{dashboard.badge}</em>}
          <span>{dashboard.metricLabel || "SCORE"}</span>
          <strong>{dashboard.metricValue || "0"}</strong>
          {dashboard.metricUnit && <small>{dashboard.metricUnit}</small>}
        </section>
      </div>
      {progress && (
        <div className="cds-pixel-progress" aria-label={progress.label || "进度"}>
          <span style={{ width: `${progress.value}%` }} />
        </div>
      )}
      <footer>{dashboard.footer || "点击开始"}</footer>
    </div>
  );
}

function PixelToolPreview({ component, dashboard, progress }) {
  const visual = PIXEL_TOOL_ICONS[dashboard.visualSprite] || {
    MainIcon: PIXEL_TOOL_SPRITE_ICONS[dashboard.visualSprite] || Activity,
    ContextIcon: Clock3,
  };
  const accent = PIXEL_TOOL_ACCENTS[dashboard.visualPalette] || "amber";
  const { MainIcon, ContextIcon } = visual;
  const showBadge = !["", "0", "—", "-"].includes(String(dashboard.badge ?? ""));
  return (
    <div className="cds-tool-shell" data-accent={accent}>
      <header className="cds-tool-topbar">
        <div className="cds-tool-title">
          <span className="cds-tool-icon cds-tool-icon--main" aria-hidden="true">
            <MainIcon />
          </span>
          <strong>{dashboard.title || component.name || "工具组件"}</strong>
        </div>
        <div className="cds-tool-status">
          {dashboard.headline && (
            <span>
              <Activity aria-hidden="true" />
              {dashboard.headline}
            </span>
          )}
          {showBadge && <b aria-label={`计数 ${dashboard.badge}`}>{dashboard.badge}</b>}
        </div>
      </header>

      {(dashboard.metricLabel || dashboard.metricValue) && (
        <section className="cds-tool-metric">
          {dashboard.metricLabel && (
            <div className="cds-tool-metric__label">
              <ContextIcon aria-hidden="true" />
              <span>{dashboard.metricLabel}</span>
            </div>
          )}
          <div className="cds-tool-metric__value">
            <strong>{dashboard.metricValue || "—"}</strong>
            {dashboard.metricUnit && <span>{dashboard.metricUnit}</span>}
          </div>
        </section>
      )}

      <div className="cds-tool-bottom">
        {progress && (
          <div className="cds-tool-progress" aria-label={progress.label || "进度"}>
            <div>
              <span>{progress.label || "进度"}</span>
              <b>{Math.round(progress.value)}%</b>
            </div>
            <i><span style={{ width: `${progress.value}%` }} /></i>
          </div>
        )}
        {dashboard.footer && (
          <footer>
            <Keyboard aria-hidden="true" />
            <span>{dashboard.footer}</span>
          </footer>
        )}
      </div>
    </div>
  );
}

export default function DeviceScreenPreview({ component, className = "" }) {
  const previewRef = useRef(null);
  const animationActive = usePreviewAnimationAllowed(previewRef, Boolean(component));
  if (!component) return null;
  const dashboard = component.dashboard || {};
  const progress = normalizeProgress(dashboard.progress);
  const isPixel = dashboard.visualStyle === "pixel";
  const isClean = dashboard.visualStyle === "clean";
  const hasCanvasGame = Boolean(component.scene || component.gameType);
  const usesCanvasShell = isPixel || (isClean && hasCanvasGame);
  const isPixelTool = isPixel && dashboard.visualLayout === "tool";
  const palette = PIXEL_PALETTES.has(dashboard.visualPalette)
    ? dashboard.visualPalette
    : undefined;
  const layout = PIXEL_LAYOUTS.has(dashboard.visualLayout)
    ? dashboard.visualLayout
    : undefined;
  return (
    <div
      ref={previewRef}
      className={`component-device-screen ${usesCanvasShell ? "component-device-screen--pixel" : ""} ${isClean ? "component-device-screen--clean" : ""} ${isPixelTool ? "component-device-screen--pixel-tool" : ""} ${className}`.trim()}
      data-widget={component.id}
      data-visual-style={isPixel ? "pixel" : (isClean ? "clean" : "classic")}
      data-palette={palette}
      data-layout={layout}
      aria-label={`${component.name || ""} 设备屏预览`}
    >
      {usesCanvasShell ? (
        isPixelTool ? (
          <PixelToolPreview component={component} dashboard={dashboard} progress={progress} />
        ) : (
          <PixelGamePreview
            dashboard={dashboard}
            progress={progress}
            gameType={component.gameType}
            scene={component.scene}
            animate={animationActive}
          />
        )
      ) : (
        <>
          <div className="cds-row-top">
            {dashboard.title && <div className="cds-title-badge">{dashboard.title}</div>}
            <div className="cds-top-status">
              {dashboard.headline && <div className="cds-headline">{dashboard.headline}</div>}
              {dashboard.badge && <div className="cds-badge-circle">{dashboard.badge}</div>}
            </div>
          </div>
          {dashboard.eyebrow && <div className="cds-eyebrow">{dashboard.eyebrow}</div>}
          {(dashboard.metricLabel || dashboard.metricValue) && (
            <div className="cds-metric-panel">
              {dashboard.metricLabel && <div className="cds-metric-label">{dashboard.metricLabel}</div>}
              <div className="cds-metric-row">
                {dashboard.metricValue && <span className="cds-metric-value">{dashboard.metricValue}</span>}
                {dashboard.metricUnit && <span className="cds-metric-unit">{dashboard.metricUnit}</span>}
              </div>
              {dashboard.note && <div className="cds-note">{dashboard.note}</div>}
            </div>
          )}
          {(progress || dashboard.footer) && (
            <div className="cds-bottom">
              {progress && (
                <div className="cds-progress" aria-label={progress.label || "进度"}>
                  <div className="cds-progress__meta">
                    <span>{progress.label || "进度"}</span>
                    <span>{Math.round(progress.value)}%</span>
                  </div>
                  <div className="cds-progress__bar"><span style={{ width: `${progress.value}%` }} /></div>
                </div>
              )}
              {dashboard.footer && <div className="cds-footer">{dashboard.footer}</div>}
            </div>
          )}
        </>
      )}
    </div>
  );
}
