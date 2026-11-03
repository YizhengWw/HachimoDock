/**
 * [Input] Consume app composition defined by `pc/src/App.jsx`[Pos] and shared styling defined by `pc/src/styles.css`[Pos].
 * [Output] Provide React bootstrap entry to downstream Vite runtime mounting.
 * [Pos] entry node in pc/src
 * [Sync] If this file changes, update this header and `pc/src/.folder.md`.
 */

import React from "react";
import ReactDOM from "react-dom/client";
import App from "./App.jsx";
import "./styles.css";
import { invoke, isTauri } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { startBubblePixelService } from "./lib/bubble-pixels.js";

if (isTauri() && getCurrentWindow().label === "main") {
  void startBubblePixelService({ listen, invoke }).catch(() => {
    console.warn("Device text images unavailable; retaining text rendering.");
  });
}

ReactDOM.createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
