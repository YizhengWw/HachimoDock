"""
[Input] PlatformIO project/build paths and the requested custom target.
[Output] Factory targets that bundle firmware, Terrier media, and built-in P4 components.
[Pos] PlatformIO integration for ESP32-P4 factory provisioning.
[Sync] If this file changes, update esp-p4-runtime/.folder.md and README.md.
"""

Import("env")

import os
import subprocess
import sys


def factory_command(environment, include_upload):
    project_dir = environment.subst("$PROJECT_DIR")
    build_dir = environment.subst("$BUILD_DIR")
    command = [
        sys.executable,
        os.path.join(project_dir, "tools", "build_factory_image.py"),
        "--project-dir",
        project_dir,
        "--build-dir",
        build_dir,
    ]
    if include_upload:
        upload_port = environment.subst("$UPLOAD_PORT").strip()
        if not upload_port:
            raise RuntimeError("factory_upload requires --upload-port")
        command.extend(
            [
                "--flash-port",
                upload_port,
                "--flash-baud",
                str(environment.get("UPLOAD_SPEED", 921600)),
            ]
        )
    return command


def build_factory(source, target, env):
    return subprocess.run(factory_command(env, False), check=False).returncode


def upload_factory(source, target, env):
    return subprocess.run(factory_command(env, True), check=False).returncode


firmware = "$BUILD_DIR/${PROGNAME}.bin"
env.AddCustomTarget(
    name="factory",
    dependencies=[firmware],
    actions=[build_factory],
    title="Factory image",
    description="Build one P4 image with default Terrier media and built-in components.",
    always_build=True,
)
env.AddCustomTarget(
    name="factory_upload",
    dependencies=[firmware],
    actions=[upload_factory],
    title="Factory image upload",
    description="Build and flash the destructive, fully preloaded P4 factory image.",
    always_build=True,
)
