/**
 * [Input] Read DeviceScreenPreview.jsx source.
 * [Output] Static source coverage for classic slots, board-shared tool palette/icon
 *          presets, and viewport-bounded animated blocks/snake/flappy previews.
 * [Pos] test node in ref/src/component-center
 * [Sync] If this file changes, update `ref/src/component-center/.folder.md`.
 */

import test from "node:test";
import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { dirname, join } from "node:path";

const here = dirname(fileURLToPath(import.meta.url));
const source = readFileSync(join(here, "DeviceScreenPreview.jsx"), "utf8");
const styles = readFileSync(join(here, "../styles.css"), "utf8");

// 1. Default export
test("DeviceScreenPreview exports a default React function", () => {
  assert.match(source, /export default function DeviceScreenPreview\s*\(/);
});

// 2. Renders cds-title-badge from dashboard.title
test("DeviceScreenPreview renders cds-title-badge using dashboard.title", () => {
  assert.match(source, /cds-title-badge/);
  assert.match(source, /dashboard\.title/);
});

// 3. Renders progress bar
test("DeviceScreenPreview renders cds-progress and cds-progress__bar", () => {
  assert.match(source, /cds-progress/);
  assert.match(source, /cds-progress__bar/);
});

test("DeviceScreenPreview keeps progress in a separate bottom slot", () => {
  const classicPanel = source.indexOf('className="cds-metric-panel"');
  const classicProgress = source.lastIndexOf('className="cds-progress"');
  assert.notEqual(classicPanel, -1);
  assert.notEqual(classicProgress, -1);
  assert.ok(classicProgress > classicPanel);
  assert.match(source, /className="cds-bottom"/);
  assert.match(source, /\{dashboard\.footer && <div className="cds-footer"/);
});

// 4. Guards against bad progress values via normalizeProgress
test("DeviceScreenPreview normalizes progress and clamps to 0-100", () => {
  assert.match(source, /normalizeProgress/);
  assert.match(source, /typeof progress === "string"/);
  assert.match(source, /progress\.split\(":", 2\)/);
  assert.match(source, /Math\.max\(0, Math\.min\(100/);
});

// 5. Wraps everything in component-device-screen
test("DeviceScreenPreview uses component-device-screen as root class", () => {
  assert.match(source, /component-device-screen/);
  assert.match(source, /data-widget=\{component\.id\}/);
});

test("device screen and candidate preview styles reserve stable frame slots", () => {
  assert.match(styles, /\.component-device-screen\s*\{[\s\S]*grid-template-rows:/);
  assert.match(styles, /\.candidate-card__preview\s*\{[\s\S]*align-items:\s*center/);
  assert.match(styles, /\.candidate-card__preview\s*\{[\s\S]*min-height:/);
});

test("pixel mini-games use bounded bright palettes, layouts, and built-in sprites", () => {
  assert.match(source, /PixelGamePreview/);
  assert.match(source, /PIXEL_PALETTES/);
  assert.match(source, /PIXEL_LAYOUTS/);
  assert.match(source, /PIXEL_SPRITES/);
  assert.match(source, /blocks:\s*\[/);
  assert.match(source, /snake:\s*\[/);
  assert.match(source, /"mole-ready":\s*\[/);
  assert.match(source, /"mole-left":\s*\[/);
  assert.match(source, /"mole-center":\s*\[/);
  assert.match(source, /"mole-right":\s*\[/);
  assert.match(source, /data-wide=\{columns > 9/);
  assert.match(source, /cds-pixel-badge/);
  assert.match(source, /dashboard\.visualStyle === "pixel"/);
  assert.match(source, /data-palette=\{palette\}/);
  assert.match(styles, /\.component-device-screen--pixel\s*\{/);
  assert.match(styles, /\.cds-pixel-sprite\s*\{/);
  assert.match(styles, /--sprite-columns/);
  assert.match(styles, /\.cds-pixel-sprite\[data-wide="true"\]\s*\{/);
  assert.match(styles, /\.cds-pixel-badge\s*\{/);
  assert.match(styles, /--pixel-bg:\s*#5ad7ff/);
});

test("pixel tools use the board-shared palette accents and semantic sprite icons", () => {
  assert.match(source, /function PixelToolPreview/);
  assert.match(source, /dashboard\.visualLayout === "tool"/);
  assert.match(source, /PIXEL_TOOL_ACCENTS/);
  assert.match(source, /sunset:\s*"coral"/);
  assert.match(source, /mint:\s*"aqua"/);
  assert.match(source, /arcade:\s*"violet"/);
  assert.match(source, /PIXEL_TOOL_ICONS\[dashboard\.visualSprite\]/);
  assert.match(source, /PIXEL_TOOL_ICONS/);
  assert.match(source, /TimerReset/);
  assert.match(source, /Droplets/);
  assert.match(source, /Gauge/);
  assert.match(source, /coffee:\s*\[/);
  assert.match(source, /timer:\s*\[/);
  assert.match(source, /droplet:\s*\[/);
  assert.match(source, /gauge:\s*\[/);
  assert.match(styles, /\.component-device-screen--pixel-tool\s*\{/);
  assert.match(styles, /\.cds-tool-shell\s*\{[\s\S]*#050706/);
  assert.match(styles, /\.cds-tool-shell\[data-accent="coral"\]/);
  assert.match(styles, /\.cds-tool-shell\[data-accent="aqua"\]/);
  assert.match(styles, /\.cds-tool-shell\[data-accent="violet"\]/);
  assert.match(styles, /\.cds-tool-icon--main/);
  assert.match(styles, /\.cds-tool-metric/);
  assert.match(styles, /\.cds-tool-progress/);
});

test("legacy classic previews still reuse bounded palette and scoreboard presets", () => {
  assert.match(source, /data-palette=\{palette\}/);
  assert.match(source, /data-layout=\{layout\}/);
  assert.match(styles, /\.component-device-screen\[data-palette="sunset"\]/);
  assert.match(styles, /\.component-device-screen\[data-palette="mint"\]/);
  assert.match(styles, /\.component-device-screen\[data-palette="arcade"\]/);
  assert.match(styles, /--classic-page-ink:\s*#fffdf0/);
  assert.match(styles, /--classic-page-muted:\s*#3cf2d2/);
  assert.match(styles, /\.cds-title-badge[\s\S]*color:\s*var\(--classic-page-ink\)/);
  assert.match(styles, /\.cds-progress__meta[\s\S]*color:\s*var\(--classic-page-muted\)/);
  assert.match(styles, /\.component-device-screen\[data-layout="scoreboard"\] \.cds-metric-panel/);
});

test("animated game previews pause outside the visible document and viewport", () => {
  assert.match(source, /usePreviewAnimationAllowed/);
  assert.match(source, /IntersectionObserver/);
  assert.match(source, /visibilitychange/);
  assert.match(source, /ref=\{previewRef\}/);
  assert.match(source, /!gameType \|\| !active/);
  assert.match(source, /animate=\{animationActive\}/);
});

test("bounded blocks, snake, and flappy previews render moving game grids", () => {
  assert.match(source, /useGamePreviewStep/);
  assert.match(source, /blocksPreviewCells/);
  assert.match(source, /snakePreviewCells/);
  assert.match(source, /flappyPreviewCells/);
  assert.match(source, /PixelGameGrid/);
  assert.match(source, /<canvas/);
  assert.match(source, /getContext\("2d"/);
  assert.match(source, /context\.fillRect/);
  assert.doesNotMatch(source, /frame\.cells\.map/);
  assert.match(source, /\["blocks", "snake", "flappy"\]\.includes\(gameType\)/);
  assert.match(source, /window\.setInterval/);
  assert.match(source, /prefers-reduced-motion/);
  assert.match(styles, /\.cds-pixel-game-grid--blocks\s*\{/);
  assert.match(styles, /\.cds-pixel-game-grid--snake\s*\{/);
  assert.match(styles, /\.cds-pixel-game-grid--flappy\s*\{/);
  assert.match(styles, /\.cds-pixel-game-grid__cell--1/);
  assert.match(styles, /\.cds-pixel-shell\[data-game-type\] \.cds-pixel-stage/);
  assert.match(styles, /grid-template-columns:\s*minmax\(0,\s*4\.5fr\)\s*minmax\(46px,\s*0\.65fr\)/);
  assert.match(styles, /data-game-type="blocks"[\s\S]*max-height:\s*160px/);
  assert.match(styles, /data-game-type="snake"[\s\S]*max-height:\s*116px/);
  assert.match(styles, /data-game-type="flappy"[\s\S]*max-height:\s*124px/);
  assert.match(styles, /\.cds-pixel-playfield--game[\s\S]*padding:\s*2px/);
  assert.match(styles, /\.cds-pixel-playfield--game[\s\S]*border-width:\s*1px/);
  assert.match(styles, /\.cds-pixel-playfield--game[\s\S]*background:\s*color-mix/);
  assert.match(styles, /\.cds-pixel-game-grid[\s\S]*border:\s*1px solid var\(--pixel-ink\)/);
  assert.match(styles, /\.cds-pixel-game-grid[\s\S]*image-rendering:\s*pixelated/);
  assert.match(styles, /\.cds-pixel-game-grid[\s\S]*box-shadow:\s*none/);
  assert.match(source, /const birdYPath = \[5, 5, 4, 4, 4, 5, 5, 6, 6, 5\]/);
  assert.match(styles, /data-layout="arcade"[\s\S]*\.cds-pixel-score[\s\S]*align-self:\s*start/);
  assert.match(styles, /data-layout="arcade"[\s\S]*data-wide="true"[\s\S]*164px/);
  assert.match(styles, /\.cds-pixel-shell\[data-game-type\] \.cds-pixel-score strong/);
  assert.match(source, /!previewType && <b>/);
});

test("generic scene previews render bounded shapes with distinct clean themes", () => {
  assert.match(source, /drawScenePreviewEntity/);
  assert.match(source, /case "player-ship"/);
  assert.match(source, /case "enemy-ship"/);
  assert.match(source, /case "bullet"/);
  assert.match(source, /case "star"/);
  assert.match(source, /case "ball"/);
  assert.match(source, /dashboard\.visualStyle === "clean"/);
  assert.match(source, /data-style=\{visualStyle\}/);
  assert.match(styles, /\.cds-pixel-shell\[data-palette="ocean"\]/);
  assert.match(styles, /\.cds-pixel-shell\[data-palette="forest"\]/);
  assert.match(styles, /\.cds-pixel-shell\[data-palette="ember"\]/);
  assert.match(styles, /\.cds-pixel-shell\[data-palette="mono"\]/);
  assert.match(styles, /\.cds-pixel-shell\[data-style="clean"\]/);
  assert.match(styles, /image-rendering:\s*auto/);
});
