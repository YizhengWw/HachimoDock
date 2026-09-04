/**
 * [Input] Preview media descriptors produced by `lib/appearance-preview.js`.
 * [Output] Resilient image/video preview element with local-image blob loading, viewport-aware video playback,
 *          asset URL fallback, source-image fallback, and controlled media cleanup.
 * [Pos] component node in pc/src
 * [Sync] If this file changes, update `pc/src/.folder.md`.
 */

import React, { useEffect, useRef, useState } from "react";
import { readAppearanceFileAsBlobUrl } from "./lib/appearance-store.js";

function useResolvedPreviewUrl(media) {
  const [assetFailed, setAssetFailed] = useState(false);
  const [blobUrl, setBlobUrl] = useState("");
  const [blobFailed, setBlobFailed] = useState(false);
  const key = `${media?.kind || ""}:${media?.src || ""}:${media?.path || ""}`;
  const preferLocalBlob = Boolean(media?.kind === "image" && media?.path);

  useEffect(() => {
    setAssetFailed(false);
    setBlobUrl("");
    setBlobFailed(false);
  }, [key]);

  useEffect(() => {
    if (!media?.path || (media.src && !assetFailed && !preferLocalBlob)) return undefined;
    let cancelled = false;
    let created = "";

    readAppearanceFileAsBlobUrl(media.path, media.mime)
      .then((url) => {
        if (cancelled) {
          URL.revokeObjectURL(url);
        } else {
          created = url;
          setBlobUrl(url);
        }
      })
      .catch(() => {
        if (!cancelled) setBlobFailed(true);
      });

    return () => {
      cancelled = true;
      if (created) URL.revokeObjectURL(created);
    };
  }, [assetFailed, media?.mime, media?.path, media?.src, preferLocalBlob]);

  return {
    url: preferLocalBlob && !blobFailed ? blobUrl : assetFailed ? blobUrl : media?.src || blobUrl,
    preferLocalBlob,
    assetFailed,
    blobFailed,
    markAssetFailed: () => {
      if (media?.path && !assetFailed) setAssetFailed(true);
      else if (!media?.path) setAssetFailed(true);
    },
  };
}

function seekToPosterFrame(video) {
  if (video && video.currentTime < 0.01) {
    try {
      video.currentTime = 0.01;
    } catch {
      /* Some WebViews disallow early seek before enough metadata is ready. */
    }
  }
}

function usePreviewPlaybackAllowed(videoRef, requested, mediaKey) {
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
    if (!requested) {
      setNearViewport(false);
      return undefined;
    }
    const video = videoRef.current;
    if (!video || typeof IntersectionObserver !== "function") {
      setNearViewport(true);
      return undefined;
    }

    setNearViewport(false);
    const observer = new IntersectionObserver(
      ([entry]) => setNearViewport(Boolean(entry?.isIntersecting)),
      { rootMargin: "160px 0px" },
    );
    observer.observe(video);
    return () => observer.disconnect();
  }, [mediaKey, requested, videoRef]);

  return requested && documentVisible && nearViewport;
}

export default function AppearancePreview({ media, className, emptyClassName, playing = false }) {
  const videoRef = useRef(null);
  const { url, assetFailed, blobFailed, preferLocalBlob, markAssetFailed } = useResolvedPreviewUrl(media);
  const fallbackMedia = media?.fallback;
  const mediaKey = `${media?.kind || ""}:${media?.src || ""}:${media?.path || ""}`;
  const shouldPlay = usePreviewPlaybackAllowed(
    videoRef,
    playing && media?.kind === "video" && Boolean(url),
    mediaKey,
  );

  useEffect(() => {
    const video = videoRef.current;
    if (!video || media?.kind !== "video" || !url) return;
    if (shouldPlay) {
      const promise = video.play();
      if (promise && typeof promise.catch === "function") promise.catch(() => {});
      return;
    }
    video.pause();
    seekToPosterFrame(video);
  }, [media?.kind, shouldPlay, url]);

  const handleLoadedData = () => {
    if (!shouldPlay) seekToPosterFrame(videoRef.current);
  };

  if ((assetFailed || (preferLocalBlob && blobFailed)) && fallbackMedia && (!media?.path || blobFailed)) {
    return (
      <AppearancePreview
        media={fallbackMedia}
        className={className}
        emptyClassName={emptyClassName}
        playing={playing}
      />
    );
  }

  if (media?.kind === "video" && url) {
    return (
      <video
        ref={videoRef}
        className={className}
        src={url}
        muted
        loop
        playsInline
        preload={shouldPlay ? "auto" : "metadata"}
        onLoadedData={handleLoadedData}
        onError={markAssetFailed}
      />
    );
  }

  if (media?.kind === "image" && url) {
    return (
      <img
        className={className}
        src={url}
        alt={media.label}
        loading="lazy"
        decoding="async"
        onError={markAssetFailed}
      />
    );
  }

  return <span className={emptyClassName || className} aria-hidden="true" />;
}
