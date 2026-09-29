export const MUSIC_SOURCE_URL = "https://music.gdstudio.xyz/";

// Web previews keep normal anchor navigation; desktop opens the system browser.
export async function openMusicSourceLink(event, { isDesktop, invokeExternal, onError }) {
  if (!isDesktop) return;
  event.preventDefault();
  onError("");
  try {
    await invokeExternal("open_external_url", { url: MUSIC_SOURCE_URL });
  } catch {
    onError("无法打开浏览器，请复制链接后访问：https://music.gdstudio.xyz/");
  }
}
