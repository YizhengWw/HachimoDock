// Development-only render-failure injection; no native commands or user data.
import React, { useState } from "react";
import { createRoot } from "react-dom/client";
import ComponentCenterBoundary from "../../src/component-center/ComponentCenterBoundary.jsx";
import "../../src/styles.css";
if (!import.meta.env.DEV) throw new Error("Development fixture only");
function Page() {
  const [failed, setFailed] = useState(false);
  if (failed) throw new ReferenceError("synthetic component render failure");
  return <section><h2>模拟组件列表</h2><button onClick={() => setFailed(true)}>模拟打开详情时异常</button></section>;
}
function Fixture() {
  const [clicks, setClicks] = useState(0);
  return <main style={{ padding: 24 }}><button onClick={() => setClicks(n => n + 1)}>外层导航仍可用：{clicks}</button><ComponentCenterBoundary><Page /></ComponentCenterBoundary></main>;
}
createRoot(document.getElementById("root")).render(<Fixture />);
