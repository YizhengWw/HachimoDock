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

ReactDOM.createRoot(document.getElementById("root")).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
