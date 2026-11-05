"""Expose the P4 toolchain, target firmware contract, and portable project root."""

Import("env")

import os
import re
import atexit

project_dir = env.subst("$PROJECT_DIR")
# Keep exact per-environment SDK compilation contexts available to host checks.
env.Replace(COMPILATIONDB_INCLUDE_TOOLCHAIN=True,
            COMPILATIONDB_PATH=os.path.join(env.subst("$BUILD_DIR"), "platformio_compile_commands.json"))
env["ENV"]["PET_P4_PROJECT_DIR"] = project_dir
env["ENV"]["PET_P4_CHIP_FAMILY"] = env.GetProjectOption("custom_p4_chip_family", "v1")
env["ENV"]["PET_P4_PROJECT_VER_OVERRIDE"] = env.GetProjectOption(
    "custom_p4_project_version", "0.7.57-p4"
)
env["ENV"]["PET_P4_UART_BAUD_OVERRIDE"] = env.GetProjectOption(
    "custom_p4_uart_baud", "4000000"
)


def normalize_component_lock(source, target, env):
    """Restore only this project's local component source paths, not versions."""
    lock_name = "dependencies.v3.lock" if env.GetProjectOption("custom_p4_chip_family", "v1") == "v3" else "dependencies.lock"
    lock_path = os.path.join(project_dir, lock_name)
    if not os.path.isfile(lock_path):
        return
    with open(lock_path, "r", encoding="utf-8") as lock_file:
        current = lock_file.read()
    portable_root = "$PET_P4_PROJECT_DIR"
    prefix=project_dir.replace("\\","/").rstrip("/")+"/components/"
    def portable_path(match):
        value=match.group(2).strip()
        if len(value)>1 and value[0]==value[-1] and value[0] in ("'",'"'):
            value=value[1:-1]
        value=value.replace("\\","/")
        if not value.startswith(prefix):return match.group(0)
        component=value[len(prefix):]
        if not component or component in (".","..") or "/" in component or not os.path.isdir(os.path.join(project_dir,"components",component)):
            raise RuntimeError("Unexpected project-local component dependency path")
        return match.group(1)+portable_root+"/components/"+component
    normalized=re.sub(r"(?m)^([ \t]+path:[ \t]+)([^\r\n]+)$",portable_path,current)
    if normalized != current:
        with open(lock_path, "w", encoding="utf-8", newline="\n") as lock_file:
            lock_file.write(normalized)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", normalize_component_lock)
# ESP-IDF also rewrites the lock when an incremental build decides the binary
# is already up to date; its .bin post-action is then skipped. Restore generated
# local paths at graceful SCons exit as well. Idempotent, and versions/hashes stay
# unchanged. Interrupted/abrupt exits still require the normal source hash gate.
atexit.register(normalize_component_lock,None,None,env)

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
