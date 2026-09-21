#!/usr/bin/env python3
"""
[Input] Validated bounded-component widget JSON plus referenced 8-bit PNG sprite sheets.
[Output] Frame-contiguous P4S1 RGB565-alpha files and the firmware sprite-pack checksum.
[Pos] Shared dependency-free sprite compiler for firmware built-in and factory-image tools.
[Sync] If this file changes, update firmware/.folder.md and sprite/factory tests.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path


PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
P4S_MAGIC = b"P4S1"
MAX_SPRITES = 4
MAX_FRAMES = 8
MAX_TOTAL_PIXELS = 4096
MAX_SOURCE_BYTES = 128 * 1024


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    left_distance = abs(estimate - left)
    above_distance = abs(estimate - above)
    upper_left_distance = abs(estimate - upper_left)
    if left_distance <= above_distance and left_distance <= upper_left_distance:
        return left
    if above_distance <= upper_left_distance:
        return above
    return upper_left


def decode_png_rgba(path: Path) -> tuple[int, int, bytes]:
    source = path.read_bytes()
    if len(source) > MAX_SOURCE_BYTES:
        raise ValueError(f"sprite source exceeds 128 KiB: {path}")
    if not source.startswith(PNG_SIGNATURE):
        raise ValueError(f"sprite source is not a PNG: {path}")

    width = height = bit_depth = color_type = interlace = None
    compressed = bytearray()
    palette = b""
    transparency = b""
    cursor = len(PNG_SIGNATURE)
    while cursor + 12 <= len(source):
        length = struct.unpack(">I", source[cursor : cursor + 4])[0]
        chunk_type = source[cursor + 4 : cursor + 8]
        data_start = cursor + 8
        data_end = data_start + length
        crc_end = data_end + 4
        if crc_end > len(source):
            raise ValueError(f"truncated PNG chunk in {path}")
        chunk = source[data_start:data_end]
        expected_crc = struct.unpack(">I", source[data_end:crc_end])[0]
        if zlib.crc32(chunk_type + chunk) & 0xFFFFFFFF != expected_crc:
            raise ValueError(f"PNG chunk checksum mismatch in {path}")
        if chunk_type == b"IHDR":
            if length != 13:
                raise ValueError(f"invalid PNG header in {path}")
            width, height, bit_depth, color_type, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", chunk
            )
            if compression != 0 or filtering != 0:
                raise ValueError(f"unsupported PNG compression/filter method in {path}")
        elif chunk_type == b"PLTE":
            palette = chunk
        elif chunk_type == b"tRNS":
            transparency = chunk
        elif chunk_type == b"IDAT":
            compressed.extend(chunk)
        elif chunk_type == b"IEND":
            break
        cursor = crc_end

    if not width or not height or bit_depth != 8 or interlace != 0:
        raise ValueError(f"sprite PNG must be 8-bit and non-interlaced: {path}")
    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    channels = channels_by_type.get(color_type)
    if channels is None:
        raise ValueError(f"unsupported PNG color type {color_type}: {path}")
    if color_type == 3 and (not palette or len(palette) % 3 != 0):
        raise ValueError(f"indexed PNG is missing a valid palette: {path}")

    try:
        inflated = zlib.decompress(bytes(compressed))
    except zlib.error as error:
        raise ValueError(f"cannot decompress PNG pixels in {path}: {error}") from error
    row_bytes = width * channels
    expected_length = height * (row_bytes + 1)
    if len(inflated) != expected_length:
        raise ValueError(f"unexpected PNG scanline size in {path}")

    raw = bytearray(width * height * channels)
    source_offset = 0
    for y in range(height):
        filter_type = inflated[source_offset]
        source_offset += 1
        if filter_type > 4:
            raise ValueError(f"unsupported PNG row filter {filter_type}: {path}")
        row_start = y * row_bytes
        for x in range(row_bytes):
            value = inflated[source_offset + x]
            left = raw[row_start + x - channels] if x >= channels else 0
            above = raw[row_start - row_bytes + x] if y > 0 else 0
            upper_left = (
                raw[row_start - row_bytes + x - channels]
                if y > 0 and x >= channels
                else 0
            )
            if filter_type == 1:
                value = (value + left) & 0xFF
            elif filter_type == 2:
                value = (value + above) & 0xFF
            elif filter_type == 3:
                value = (value + ((left + above) // 2)) & 0xFF
            elif filter_type == 4:
                value = (value + _paeth(left, above, upper_left)) & 0xFF
            raw[row_start + x] = value
        source_offset += row_bytes

    rgba = bytearray(width * height * 4)
    for pixel in range(width * height):
        source_pixel = pixel * channels
        target_pixel = pixel * 4
        if color_type == 0:
            gray = raw[source_pixel]
            red = green = blue = gray
            alpha = 0 if transparency == bytes((0, gray)) else 255
        elif color_type == 2:
            red, green, blue = raw[source_pixel : source_pixel + 3]
            alpha = 255
        elif color_type == 3:
            palette_index = raw[source_pixel]
            palette_offset = palette_index * 3
            if palette_offset + 3 > len(palette):
                raise ValueError(f"PNG palette index is out of range: {path}")
            red, green, blue = palette[palette_offset : palette_offset + 3]
            alpha = transparency[palette_index] if palette_index < len(transparency) else 255
        elif color_type == 4:
            gray, alpha = raw[source_pixel : source_pixel + 2]
            red = green = blue = gray
        else:
            red, green, blue, alpha = raw[source_pixel : source_pixel + 4]
        rgba[target_pixel : target_pixel + 4] = bytes((red, green, blue, alpha))
    return width, height, bytes(rgba)


def compile_component_sprites(package: Path, widget: dict) -> list[dict]:
    scene = widget.get("scene") if isinstance(widget, dict) else None
    declarations = scene.get("sprites", []) if isinstance(scene, dict) else []
    if not isinstance(declarations, list) or len(declarations) > MAX_SPRITES:
        raise ValueError(f"component has more than {MAX_SPRITES} sprites: {package}")

    compiled: list[dict] = []
    total_pixels = 0
    seen_ids: set[str] = set()
    for declaration in declarations:
        if not isinstance(declaration, dict):
            raise ValueError(f"component sprite declaration must be an object: {package}")
        sprite_id = str(declaration.get("id", ""))
        asset = str(declaration.get("asset", ""))
        frame_width = int(declaration.get("frame_width", 0))
        frame_height = int(declaration.get("frame_height", 0))
        frames = int(declaration.get("frames", 0))
        fps = int(declaration.get("fps", 0))
        if (
            not sprite_id
            or sprite_id in seen_ids
            or not asset.startswith("assets/")
            or Path(asset).name != asset.removeprefix("assets/")
            or not asset.endswith(".png")
            or not 8 <= frame_width <= 64
            or not 8 <= frame_height <= 64
            or not 1 <= frames <= MAX_FRAMES
            or not 1 <= fps <= 20
        ):
            raise ValueError(f"invalid component sprite declaration {sprite_id!r}: {package}")
        seen_ids.add(sprite_id)
        width, height, rgba = decode_png_rgba(package / asset)
        if width != frame_width * frames or height != frame_height:
            raise ValueError(
                f"sprite {sprite_id} must be {frame_width * frames}x{frame_height}, "
                f"got {width}x{height}"
            )
        total_pixels += frame_width * frame_height * frames
        if total_pixels > MAX_TOTAL_PIXELS:
            raise ValueError(f"component sprites exceed {MAX_TOTAL_PIXELS} decoded pixels")

        encoded = bytearray(P4S_MAGIC + bytes((frame_width, frame_height, frames, fps)))
        for frame in range(frames):
            for y in range(frame_height):
                for x in range(frame_width):
                    pixel = (y * width + frame * frame_width + x) * 4
                    red, green, blue, alpha = rgba[pixel : pixel + 4]
                    rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
                    encoded.extend(struct.pack("<H", rgb565))
                    encoded.append(alpha)
        compiled.append({"id": sprite_id, "data": bytes(encoded)})
    return compiled


def sprite_pack_checksum(sprites: list[dict]) -> int:
    if not sprites:
        return 0
    checksum = 2166136261

    def include(data: bytes) -> None:
        nonlocal checksum
        for value in data:
            checksum ^= value
            checksum = (checksum * 16777619) & 0xFFFFFFFF

    include(bytes((len(sprites),)))
    for sprite in sprites:
        data = sprite["data"]
        include(sprite["id"].encode("ascii") + b"\0")
        include(data[4:8])
        include(data[8:])
    return checksum
