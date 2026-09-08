"""Flash a complete P4 image only after checking the connected silicon revision.

Requires esptool 5.2.x. Keeps one serial connection from chip detection through
erase/write/verify; no hardware is touched until the caller explicitly runs it.
"""
import argparse
from pathlib import Path

from p4_image_compat import factory_revision_range


def flash(image: Path, port: str, baud: int = 921600):
    import esptool
    from esptool.cmds import run_stub, write_flash

    data = image.read_bytes()
    minimum, maximum = factory_revision_range(data)
    with esptool.detect_chip(port, baud=115200) as connected:
        if connected.CHIP_NAME != "ESP32-P4":
            raise ValueError("设备不是 ESP32-P4，已取消烧录，未擦除数据")
        revision = connected.get_chip_revision()
        if not minimum <= revision <= maximum:
            raise ValueError(f"芯片 {revision // 100}.{revision % 100} 与固件不匹配，已取消烧录，未擦除数据")
        device = run_stub(connected)
        device.change_baud(baud)
        # esptool performs image checks, complete erase, write, and verification.
        write_flash(device, [(0, data)], erase_all=True, compress=True)
        device.hard_reset()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="验证芯片版本后烧录完整 P4 镜像（将清空设备）")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=921600)
    args = parser.parse_args()
    flash(args.image, args.port, args.baud)
