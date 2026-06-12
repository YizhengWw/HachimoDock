/**
 * [Input] Consume app composition defined by `ref/src/App.jsx`[Pos] and shared styling defined by `ref/src/styles.css`[Pos].
 * [Output] Provide React bootstrap entry to downstream Vite runtime mounting.
 * [Pos] entry node in ref/src
 * [Sync] If this file changes, update this header and `ref/src/.folder.md`.
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
