"""Generate bounded P4 app/bootloader images; fail on a stale target config."""

Import("env")

import re
import importlib.util
from pathlib import Path


def bound_chip_image(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    config = (project / ("sdkconfig." + env.subst("$PIOENV"))).read_text(encoding="utf-8")
    family = env.GetProjectOption("custom_p4_chip_family", "v1")
    expected = (300, 399) if family == "v3" else (1, 199)
    values = tuple(int(re.search(r"^CONFIG_ESP_REV_" + key + r"_FULL=(\d+)$", config, re.M)[1])
                   for key in ("MIN", "MAX"))
    if values != expected:
        raise RuntimeError(f"P4 {family} requires chip bounds {expected}, found {values}; regenerate sdkconfig")
    output = Path(str(target[0]))
    elf = output.with_suffix(".elf")
    if output.name == "firmware.bin":
        report = output.parent / "main" / "pet_p4_renderer.c.su"
        # These are nested in the main-task component/media path. Optimized-away
        # (inlined) functions are accounted for in their emitted caller's frame.
        frames = {}
        for line in report.read_text(encoding="utf-8").splitlines():
            fields = line.split("\t")
            if len(fields) >= 3:
                name = fields[0].rsplit(":", 1)[-1].split(".", 1)[0]
                frames[name] = max(frames.get(name, 0), int(fields[1]))
        chain = ["pet_p4_renderer_render", "render_miniapp_page", "render_media_page", "render_package_media"]
        if "pet_p4_renderer_render" not in frames or sum(frames.get(name, 0) for name in chain) > 4096:
            raise RuntimeError(f"Component/media render stack exceeds 4096-byte budget: {frames}")
    # PlatformIO's default ElfToBin omits revision bounds. Regenerate through
    # esptool (including its checksum/hash), never patch an already signed image.
    app_flags = " --elf-sha256-offset 0xb0" if output.name == "firmware.bin" else ""
    command = ("$ERASETOOL --chip esp32p4 elf2image"
               " --flash-mode ${__get_board_flash_mode(__env__)}"
               " --flash-freq ${__get_board_f_image(__env__)} --flash-size 32MB"
               f" --min-rev-full {values[0]} --max-rev-full {values[1]}"
               f'{app_flags} -o "{output}" "{elf}"')
    result = env.Execute(command)
    if result:
        raise RuntimeError("Failed to generate revision-bounded P4 image")
    header = output.read_bytes()[:24]
    if (header[0] != 0xe9 or int.from_bytes(header[12:14], "little") != 18
            or tuple(int.from_bytes(header[i:i+2], "little") for i in (15, 17)) != expected):
        raise RuntimeError("P4 image chip bounds verification failed")
    if output.name == "firmware.bin":
        # PlatformIO may have merged its convenience image before this action.
        spec = importlib.util.spec_from_file_location("firmware_budget", project / "tools/firmware_budget.py")
        budget = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(budget)
        size_tool = env.subst("$CC").replace("gcc", "size")
        budget.check(output, project / "partitions.csv", size_tool)
        # Re-merge from bounded inputs so no generated image keeps stale limits.
        merged = output.with_name("firmware.factory.bin")
        command = ("$ERASETOOL --chip esp32p4 merge-bin --flash-mode dio"
                   f' --flash-freq 80m --flash-size 32MB -o "{merged}"')
        for offset, path in env.get("FLASH_EXTRA_IMAGES", []):
            command += f' {offset} "{env.subst(path)}"'
        command += f' {env.subst("$ESP32_APP_OFFSET") or "0x10000"} "{output}"'
        if env.Execute(command):
            raise RuntimeError("Failed to merge revision-bounded P4 convenience image")


env.AddPostAction("$BUILD_DIR/bootloader.bin", bound_chip_image)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", bound_chip_image)
