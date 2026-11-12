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
import {
  TEXT_RENDERING_CHANGE_EVENT,
  textRenderingModeForTransport,
} from "./lib/text-rendering-settings.js";
import { createStartupHandshake } from "./lib/startup-native-ready.js";
const nativeStartup=createStartupHandshake(invoke,()=>Boolean(window.__TAURI_INTERNALS__));
// Capture before App import, not after a potentially slow render/navigation.
nativeStartup.capture().catch(()=>console.warn('[startup]','native_challenge_unavailable'));

if (isTauri() && getCurrentWindow().label === "main") {
  let stopHostTextRendering = null;
  let syncing = false;
  const syncHostTextRendering = async () => {
    if (syncing) return;
    syncing = true;
    try {
      const useApp = textRenderingModeForTransport("usb") === "app";
      if (useApp && !stopHostTextRendering) {
        stopHostTextRendering = await startBubblePixelService({ listen, invoke });
        void invoke("usb_log_client_event", { stage: "started", detail: "ok", ok: true }).catch(() => {});
      } else if (!useApp && stopHostTextRendering) {
        const stop = stopHostTextRendering;
        stopHostTextRendering = null;
        stop();
      }
    } catch (error) {
      stopHostTextRendering = null;
      console.warn("Device text rendering unavailable; retaining client rendering.", error);
      void invoke("usb_log_client_event", {
        stage: "start_failed",
        detail: String(error?.name || error?.message || error),
        ok: false,
      }).catch(() => {});
    } finally {
      syncing = false;
    }
  };
  void syncHostTextRendering();
  window.addEventListener(TEXT_RENDERING_CHANGE_EVENT, syncHostTextRendering);
  /* The device is usually not connected yet when this module runs, so the first
   * start attempt fails and the pixel renderer was never registered again --
   * every bubble then stayed on local P4 glyphs. Retry until it sticks. */
  /* Retry only when no service handle exists; never tear a live service down on
   * a transient flag, which restarted the renderer repeatedly and dropped every
   * pixel frame in flight. */
  window.setInterval(() => {
    if (textRenderingModeForTransport("usb") === "app" && !stopHostTextRendering) {
      void syncHostTextRendering();
    }
  }, 2000);
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
