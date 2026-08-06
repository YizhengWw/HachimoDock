"""Expose the P4 toolchain and a portable component-lock project root."""

Import("env")

import os

project_dir = env.subst("$PROJECT_DIR")
env["ENV"]["PET_P4_PROJECT_DIR"] = project_dir


def normalize_component_lock(source, target, env):
    """Undo ESP-IDF's machine-specific lock rewrite after a successful build."""
    lock_path = os.path.join(project_dir, "dependencies.lock")
    if not os.path.isfile(lock_path):
        return
    with open(lock_path, "r", encoding="utf-8") as lock_file:
        current = lock_file.read()
    portable_root = "$PET_P4_PROJECT_DIR"
    candidates = {
        project_dir,
        project_dir.replace("\\", "/"),
        project_dir.replace("/", "\\"),
    }
    normalized = current
    for candidate in candidates:
        normalized = normalized.replace(candidate, portable_root)
    if normalized != current:
        with open(lock_path, "w", encoding="utf-8", newline="\n") as lock_file:
            lock_file.write(normalized)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", normalize_component_lock)

toolchain_dir = env.PioPlatform().get_package_dir("toolchain-riscv32-esp")
if toolchain_dir:
    candidates = [
        os.path.join(toolchain_dir, "bin"),
        os.path.join(toolchain_dir, "riscv32-esp-elf", "bin"),
    ]
    for candidate in candidates:
        if os.path.isdir(candidate):
            env.PrependENVPath("PATH", candidate)
            break
