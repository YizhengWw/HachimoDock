"""Expose the P4 toolchain and a portable component-lock project root."""

Import("env")

import os

project_dir = env.subst("$PROJECT_DIR")
env["ENV"]["PET_P4_PROJECT_DIR"] = project_dir

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
