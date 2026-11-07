/**
 * [Input] App composition and shared styles; independent startup guard in index.html.
 * [Output] Visible startup/render failure recovery without mutation replay.
 * [Pos] React bootstrap; App imports asynchronously so root-module failures are caught.
 * [Sync] pc/src/.folder.md.
 */
import React from "react";
import ReactDOM from "react-dom/client";
import "./styles.css";
import { invoke, isTauri } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";
import { getCurrentWindow } from "@tauri-apps/api/window";
import { startBubblePixelService } from "./lib/bubble-pixels.js";
import { createStartupHandshake } from "./lib/startup-native-ready.js";

// Tauri v2 WebView swallows plain <a target="_blank"> navigation, so help
// links like the Volcengine API key docs silently do nothing. Route them
// through the app's own open_external_url command (ShellExecuteW) instead.
if (isTauri()) {
  document.addEventListener("click", (event) => {
    const anchor = event.target?.closest?.("a[href]");
    if (!anchor || !anchor.target?.includes("_blank")) return;
    const url = anchor.href;
    if (!/^https?:\/\//i.test(url)) return;
    event.preventDefault();
    invoke("open_external_url", { url }).catch((error) => {
      console.warn("[opener]", "external link failed", url, error);
    });
  }, true);
}
const nativeStartup=createStartupHandshake(invoke,()=>Boolean(window.__TAURI_INTERNALS__));
// Capture before App import, not after a potentially slow render/navigation.
nativeStartup.capture().catch(()=>console.warn('[startup]','native_challenge_unavailable'));

if (isTauri() && getCurrentWindow().label === "main") {
  void startBubblePixelService({ listen, invoke }).catch(() => {
    console.warn("Device text images unavailable; retaining text rendering.");
  });
}

class RootBoundary extends React.Component {
  state={failed:false};
  static getDerivedStateFromError(){return {failed:true};}
  componentDidCatch(error){window.__PET_STARTUP__?.fail('render',error?.name);}
  render(){return this.state.failed?null:this.props.children;}
}
function Ready({children}) {
  React.useEffect(()=>{
    // A mounted React tree owns the loading view even if the independent guard
    // failed to load. Never leave its fixed overlay covering a working app.
    document.getElementById('startup-loading')?.remove();
    window.__PET_STARTUP__?.ready();
    nativeStartup.confirm().then(accepted=>{
      if(accepted!==true)console.warn('[startup]','native_ready_stale');
    }).catch(()=>console.warn('[startup]','native_ready_unconfirmed'));
  },[]);
  return children;
}
import('./App.jsx').then(({default:App})=>{
  ReactDOM.createRoot(document.getElementById('root')).render(
    <RootBoundary><React.StrictMode><Ready><App/></Ready></React.StrictMode></RootBoundary>
  );
}).catch(error=>window.__PET_STARTUP__?.fail('javascript',error?.name));
