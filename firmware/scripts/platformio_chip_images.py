"""Generate bounded P4 app/bootloader images; fail on a stale target config."""

Import("env")

import re
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
