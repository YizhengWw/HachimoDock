"""Pure ESP image-header validation shared by factory packaging and flashing."""


def image_revision_range(data: bytes, offset: int = 0) -> tuple[int, int]:
    header = data[offset:offset + 24]
    if len(header) != 24 or header[0] != 0xe9 or int.from_bytes(header[12:14], "little") != 18:
        raise ValueError("不是有效的 ESP32-P4 镜像")
    minimum = int.from_bytes(header[15:17], "little")
    maximum = int.from_bytes(header[17:19], "little")
    if minimum > maximum or not (maximum < 200 or 300 <= minimum <= maximum < 400):
        raise ValueError("固件必须限定 v1 或 v3 芯片范围，不能跨版本烧录")
    return minimum, maximum


def factory_revision_range(data: bytes) -> tuple[int, int]:
    # The board's existing partition layout is deliberately unchanged.
    boot = image_revision_range(data, 0x2000)
    app = image_revision_range(data, 0x10000)
    if boot != app:
        raise ValueError("Bootloader 与应用固件的芯片范围不一致")
    return app
