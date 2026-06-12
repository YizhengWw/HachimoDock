/**
 * [Input] Preview media descriptors produced by `lib/appearance-preview.js`.
 * [Output] Resilient image/video preview element with local-image blob loading, metadata-only idle videos,
 *          asset URL fallback, source-image fallback, and controlled playback.
 * [Pos] component node in ref/src
 * [Sync] If this file changes, update `ref/src/.folder.md`.
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

export default function AppearancePreview({ media, className, emptyClassName, playing = false }) {
  const videoRef = useRef(null);
  const { url, assetFailed, blobFailed, preferLocalBlob, markAssetFailed } = useResolvedPreviewUrl(media);
  const fallbackMedia = media?.fallback;

  useEffect(() => {
    const video = videoRef.current;
    if (!video || media?.kind !== "video" || !url) return;
    if (playing) {
      const promise = video.play();
      if (promise && typeof promise.catch === "function") promise.catch(() => {});
      return;
    }
    video.pause();
    seekToPosterFrame(video);
  }, [media?.kind, playing, url]);

  const handleLoadedData = () => {
    if (!playing) seekToPosterFrame(videoRef.current);
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
        preload={playing ? "auto" : "metadata"}
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
