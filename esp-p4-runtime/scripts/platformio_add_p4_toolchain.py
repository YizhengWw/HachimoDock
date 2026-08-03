Import("env")

import os

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
