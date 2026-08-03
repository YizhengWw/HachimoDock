#!/usr/bin/env python3
"""Validate and atomically publish a petui component into the formal library."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional

from validate_generated_widget import load_capabilities, validate_widget


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def new_job_id() -> str:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    return f"{stamp}-{uuid.uuid4().hex[:8]}"


def atomic_write_json(path: Path, value: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temp.write_text(
        json.dumps(value, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    os.replace(temp, path)


def package_files(root: Path) -> list[Path]:
    return sorted(
        (path for path in root.rglob("*") if path.is_file()),
        key=lambda path: path.relative_to(root).as_posix(),
    )


def content_hash(root: Path) -> str:
    digest = hashlib.sha256()
    for path in package_files(root):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(relative)
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")
    return digest.hexdigest()[:16]


def default_pet_home() -> Path:
    override = os.environ.get("CLAW_PET_HOME", "").strip()
    return Path(override).expanduser() if override else Path.home() / ".claw-pet"


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def publish_widget(
    source: Path,
    *,
    pet_home: Path,
    source_agent: str,
    capabilities: dict[str, Any],
) -> dict[str, Any]:
    source = source.expanduser().resolve()
    pet_home = pet_home.expanduser().resolve()
    components_root = pet_home / "components"
    staging_root = components_root / ".staging"
    library_root = components_root / "library"
    logs_root = pet_home / "logs" / "component-generation"
    job_id = new_job_id()
    job_root = staging_root / job_id
    staged_package = job_root / "package"
    log_path = logs_root / f"{job_id}.json"
    job: dict[str, Any] = {
        "schemaVersion": 1,
        "jobId": job_id,
        "status": "staging",
        "sourceAgent": source_agent or "unknown",
        "sourcePath": str(source),
        "createdAt": utc_now(),
        "updatedAt": utc_now(),
    }
    staging_root.mkdir(parents=True, exist_ok=True)
    library_root.mkdir(parents=True, exist_ok=True)
    logs_root.mkdir(parents=True, exist_ok=True)
    atomic_write_json(log_path, job)
    try:
        if not source.is_dir():
            raise ValueError(f"组件工作目录不存在: {source}")
        if is_relative_to(source, library_root):
            raise ValueError("拒绝从正式 library 内重新发布；请使用独立工作目录")
        if source == job_root or is_relative_to(job_root, source):
            raise ValueError("组件工作目录不能包含发布 staging 根目录")
        shutil.copytree(source, staged_package, symlinks=True)
        job.update(status="validating", updatedAt=utc_now(), stagingPath=str(staged_package))
        atomic_write_json(log_path, job)
        errors = validate_widget(staged_package, capabilities)
        if errors:
            raise ValueError("组件校验失败: " + "；".join(errors))
        manifest = json.loads((staged_package / "component.json").read_text(encoding="utf-8"))
        component_id = manifest["id"]
        version_hash = content_hash(staged_package)
        component_root = library_root / component_id
        destination = component_root / version_hash
        component_root.mkdir(parents=True, exist_ok=True)
        reused = destination.exists()
        if reused:
            if content_hash(destination) != version_hash:
                raise RuntimeError(f"正式目录哈希冲突: {destination}")
            shutil.rmtree(job_root)
        else:
            os.replace(staged_package, destination)
            shutil.rmtree(job_root)
        job.update(
            status="published",
            updatedAt=utc_now(),
            publishedAt=utc_now(),
            componentId=component_id,
            componentName=manifest.get("name", component_id),
            componentVersion=manifest.get("version", ""),
            versionHash=version_hash,
            publishedPath=str(destination),
            reusedExistingVersion=reused,
        )
        job.pop("stagingPath", None)
        atomic_write_json(log_path, job)
        return {"ok": True, **job, "logPath": str(log_path)}
    except Exception as error:
        job.update(
            status="failed",
            updatedAt=utc_now(),
            error=str(error),
            stagingPath=str(staged_package),
        )
        atomic_write_json(log_path, job)
        raise RuntimeError(json.dumps({"ok": False, **job, "logPath": str(log_path)}, ensure_ascii=False)) from error


def main() -> int:
    parser = argparse.ArgumentParser(description="原子发布 petui 组件到正式本地组件库")
    parser.add_argument("widget_dir", type=Path, help="已经生成的组件工作目录")
    parser.add_argument("--source-agent", default="unknown", help="codex/claude/gemini/cursor 等来源")
    parser.add_argument("--pet-home", type=Path, help="覆盖 ~/.claw-pet（主要用于测试）")
    parser.add_argument("--capabilities", type=Path, help="可选设备能力 JSON")
    parser.add_argument("--pretty", action="store_true", help="缩进输出 JSON")
    args = parser.parse_args()
    try:
        result = publish_widget(
            args.widget_dir,
            pet_home=args.pet_home or default_pet_home(),
            source_agent=args.source_agent.strip(),
            capabilities=load_capabilities(args.capabilities),
        )
    except (OSError, UnicodeError, json.JSONDecodeError, ValueError, RuntimeError) as error:
        message = str(error)
        try:
            result = json.loads(message)
        except json.JSONDecodeError:
            result = {"ok": False, "error": message}
        print(json.dumps(result, ensure_ascii=False, indent=2 if args.pretty else None), file=sys.stderr)
        return 1
    print(json.dumps(result, ensure_ascii=False, indent=2 if args.pretty else None))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
