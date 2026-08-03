#!/usr/bin/env python3
"""Render stats and bounded component dashboards to RGB565 framebuffer frames.

Classic tool dashboards and pixel mini-games share the same safe palette and
layout presets; package-authored drawing code is never executed.
"""

import argparse
import os
import re
import struct
import sys
from pathlib import Path


FONT = {
    " ": ["00000", "00000", "00000", "00000", "00000", "00000", "00000"],
    "!": ["00100", "00100", "00100", "00100", "00100", "00000", "00100"],
    ".": ["00000", "00000", "00000", "00000", "00000", "01100", "01100"],
    ":": ["00000", "01100", "01100", "00000", "01100", "01100", "00000"],
    "-": ["00000", "00000", "00000", "11111", "00000", "00000", "00000"],
    "+": ["00000", "00100", "00100", "11111", "00100", "00100", "00000"],
    "/": ["00001", "00010", "00100", "01000", "10000", "00000", "00000"],
    "0": ["01110", "10001", "10011", "10101", "11001", "10001", "01110"],
    "1": ["00100", "01100", "00100", "00100", "00100", "00100", "01110"],
    "2": ["01110", "10001", "00001", "00010", "00100", "01000", "11111"],
    "3": ["11110", "00001", "00001", "01110", "00001", "00001", "11110"],
    "4": ["00010", "00110", "01010", "10010", "11111", "00010", "00010"],
    "5": ["11111", "10000", "10000", "11110", "00001", "00001", "11110"],
    "6": ["00110", "01000", "10000", "11110", "10001", "10001", "01110"],
    "7": ["11111", "00001", "00010", "00100", "01000", "01000", "01000"],
    "8": ["01110", "10001", "10001", "01110", "10001", "10001", "01110"],
    "9": ["01110", "10001", "10001", "01111", "00001", "00010", "11100"],
    "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
    "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
    "C": ["01110", "10001", "10000", "10000", "10000", "10001", "01110"],
    "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
    "E": ["11111", "10000", "10000", "11110", "10000", "10000", "11111"],
    "F": ["11111", "10000", "10000", "11110", "10000", "10000", "10000"],
    "G": ["01110", "10001", "10000", "10111", "10001", "10001", "01110"],
    "H": ["10001", "10001", "10001", "11111", "10001", "10001", "10001"],
    "I": ["01110", "00100", "00100", "00100", "00100", "00100", "01110"],
    "J": ["00111", "00010", "00010", "00010", "10010", "10010", "01100"],
    "K": ["10001", "10010", "10100", "11000", "10100", "10010", "10001"],
    "L": ["10000", "10000", "10000", "10000", "10000", "10000", "11111"],
    "M": ["10001", "11011", "10101", "10101", "10001", "10001", "10001"],
    "N": ["10001", "11001", "10101", "10011", "10001", "10001", "10001"],
    "O": ["01110", "10001", "10001", "10001", "10001", "10001", "01110"],
    "P": ["11110", "10001", "10001", "11110", "10000", "10000", "10000"],
    "Q": ["01110", "10001", "10001", "10001", "10101", "10010", "01101"],
    "R": ["11110", "10001", "10001", "11110", "10100", "10010", "10001"],
    "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    "T": ["11111", "00100", "00100", "00100", "00100", "00100", "00100"],
    "U": ["10001", "10001", "10001", "10001", "10001", "10001", "01110"],
    "V": ["10001", "10001", "10001", "10001", "10001", "01010", "00100"],
    "W": ["10001", "10001", "10001", "10101", "10101", "10101", "01010"],
    "X": ["10001", "10001", "01010", "00100", "01010", "10001", "10001"],
    "Y": ["10001", "10001", "01010", "00100", "00100", "00100", "00100"],
    "Z": ["11111", "00001", "00010", "00100", "01000", "10000", "11111"],
}


def pack565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def safe_ascii(value: str) -> str:
    value = re.sub(r"[^\x20-\x7e]", " ", value)
    return re.sub(r"\s+", " ", value).strip().upper()


class Canvas:
    def __init__(self, width: int, height: int, bg=(5, 7, 6)):
        self.width = width
        self.height = height
        self.data = bytearray(width * height * 2)
        self.fill(bg)

    def pixel(self, x: int, y: int, color) -> None:
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return
        offset = (y * self.width + x) * 2
        struct.pack_into("<H", self.data, offset, pack565(*color))

    def rect(self, x: int, y: int, w: int, h: int, color) -> None:
        x0 = max(0, x)
        y0 = max(0, y)
        x1 = min(self.width, x + w)
        y1 = min(self.height, y + h)
        if x1 <= x0 or y1 <= y0:
            return
        packed = struct.pack("<H", pack565(*color))
        row = packed * (x1 - x0)
        for yy in range(y0, y1):
            start = (yy * self.width + x0) * 2
            self.data[start:start + len(row)] = row

    def fill(self, color) -> None:
        self.rect(0, 0, self.width, self.height, color)

    def border(self, x: int, y: int, w: int, h: int, color, thickness: int = 1) -> None:
        self.rect(x, y, w, thickness, color)
        self.rect(x, y + h - thickness, w, thickness, color)
        self.rect(x, y, thickness, h, color)
        self.rect(x + w - thickness, y, thickness, h, color)

    def text_width(self, text: str, scale: int) -> int:
        return len(text) * 6 * scale

    def text(self, x: int, y: int, text: str, color=(250, 241, 204), scale: int = 2, max_width=None) -> None:
        text = safe_ascii(text)
        if max_width:
            max_chars = max(1, max_width // (6 * scale))
            text = text[:max_chars]
        cursor = x
        for ch in text:
            glyph = FONT.get(ch, FONT[" "])
            for row, bits in enumerate(glyph):
                for col, bit in enumerate(bits):
                    if bit == "1":
                        self.rect(cursor + col * scale, y + row * scale, scale, scale, color)
            cursor += 6 * scale

    def centered_text(self, y: int, text: str, color=(250, 241, 204), scale: int = 2) -> None:
        text = safe_ascii(text)
        x = max(0, (self.width - self.text_width(text, scale)) // 2)
        self.text(x, y, text, color, scale, self.width - x)


def parse_payload(text: str):
    values = {
        "agent": "Codex",
        "lunch": "0.0",
        "metricValue": "0",
        "metricUnit": "TOKEN",
        "alerts": "0",
        "completed": "0",
        "breakdown": "",
        "sources": "",
    }
    if not text.startswith("STATS_DASHBOARD_V1"):
        return values
    for line in text.splitlines()[1:]:
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        if key in values:
            values[key] = value.strip()
    return values


def read_fb_size(fb_path: str):
    fb_name = os.path.basename(fb_path)
    if fb_name.startswith("fb"):
        size_path = Path("/sys/class/graphics") / fb_name / "virtual_size"
        try:
            width, height = size_path.read_text().strip().split(",", 1)
            return int(width), int(height)
        except (OSError, ValueError):
            pass
    return 480, 640


def normalize_rotation(value) -> int:
    try:
        rotation = int(value)
    except (TypeError, ValueError):
        return 0
    rotation %= 360
    return rotation if rotation in (0, 180) else 0


def rotate_frame_rgb565(frame: bytes, width: int, height: int, rotation: int) -> bytes:
    rotation = normalize_rotation(rotation)
    if rotation != 180:
        return frame
    row_bytes = width * 2
    output = bytearray(len(frame))
    for y in range(height):
        src_start = y * row_bytes
        dst_start = (height - 1 - y) * row_bytes
        dst = dst_start
        for src in range(src_start + row_bytes - 2, src_start - 1, -2):
            output[dst] = frame[src]
            output[dst + 1] = frame[src + 1]
            dst += 2
    return bytes(output)


def render(values, width: int, height: int) -> bytes:
    compact = height <= 120 or width <= 180
    small_landscape = width <= 360 or height <= 260
    c = Canvas(width, height)
    c.rect(0, 0, width // 3, height, (10, 31, 22))
    c.rect(width - width // 4, 0, width // 4, height, (36, 25, 10))
    for y in range(0, height, 4):
        c.rect(0, y, width, 1, (16, 27, 19))

    margin = max(4, width // 16)
    if compact:
        c.text(margin, margin, values["agent"], (225, 241, 231), 1, width - margin * 2)
        c.centered_text(height // 2 - 14, values["metricValue"], (255, 163, 31), 3)
        c.centered_text(height // 2 + 12, values["metricUnit"], (158, 164, 160), 1)
        c.text(margin, height - 14, "LUNCH " + values["lunch"], (85, 193, 133), 1, width - margin * 2)
        return bytes(c.data)

    if small_landscape:
        badge_w = min(width - margin * 2, 142)
        c.rect(margin, 8, badge_w, 34, (13, 39, 25))
        c.border(margin, 8, badge_w, 34, (85, 193, 133), 1)
        agent_scale = 2 if c.text_width(values["agent"], 2) <= badge_w - 18 else 1
        c.text(margin + 9, 18, values["agent"], (225, 241, 231), agent_scale, badge_w - 18)

        c.text(margin, 52, "EQUIV LUNCH", (158, 164, 160), 2, width - margin * 2)
        lunch = safe_ascii(values["lunch"])
        lunch_scale = 4 if c.text_width(lunch, 4) <= 138 else 3
        c.text(margin, 74, lunch, (255, 163, 31), lunch_scale, 150)
        label_x = margin + min(c.text_width(lunch, lunch_scale) + 12, width - margin * 2 - 100)
        c.text(label_x, 84, "WORK LUNCH", (255, 209, 114), 2, width - label_x - margin)
        c.rect(margin, 115, width - margin * 2, 1, (74, 74, 62))

        panel_y = 126
        panel_h = min(102, height - panel_y - 12)
        panel_w = width - margin * 2
        c.rect(margin, panel_y, panel_w, panel_h, (11, 14, 13))
        c.border(margin, panel_y, panel_w, panel_h, (55, 66, 54), 1)
        c.text(margin + 12, panel_y + 13, "TODAY TOKEN", (158, 164, 160), 2, panel_w - 24)

        value = safe_ascii(values["metricValue"])
        value_scale = 4
        while value_scale > 2 and c.text_width(value, value_scale) > panel_w - 98:
            value_scale -= 1
        value_y = panel_y + 38
        value_w = c.text_width(value, value_scale)
        c.text(margin + 12, value_y, value, (250, 241, 204), value_scale, panel_w - 24)
        unit_x = margin + 12 + value_w + 10
        if unit_x + c.text_width(values["metricUnit"], 2) <= width - margin - 8:
            c.text(unit_x, value_y + 10, values["metricUnit"], (158, 164, 160), 2, width - unit_x - margin)
        else:
            c.text(margin + 12, value_y + 32, values["metricUnit"], (158, 164, 160), 1, panel_w - 24)

        detail = values["breakdown"] or values["sources"]
        if detail and panel_h >= 92:
            c.text(margin + 12, panel_y + panel_h - 18, detail, (164, 169, 160), 1, panel_w - 24)
        return bytes(c.data)

    badge_h = max(48, height // 8)
    c.rect(margin, margin, min(width // 2, 250), badge_h, (13, 39, 25))
    c.border(margin, margin, min(width // 2, 250), badge_h, (85, 193, 133), 2)
    c.text(margin + 16, margin + 14, values["agent"], (225, 241, 231), 3, min(width // 2, 250) - 24)

    c.text(margin, height // 4, "EQUIV LUNCH", (158, 164, 160), 2, width - margin * 2)
    c.text(margin, height // 4 + 34, values["lunch"] + " WORK LUNCH", (255, 163, 31), 4, width - margin * 2)
    c.rect(margin, height // 2 - 12, width - margin * 2, 2, (74, 74, 62))

    panel_y = height // 2 + 16
    panel_h = max(110, height // 4)
    c.rect(margin, panel_y, width - margin * 2, panel_h, (11, 14, 13))
    c.border(margin, panel_y, width - margin * 2, panel_h, (55, 66, 54), 2)
    c.text(margin + 18, panel_y + 18, "TODAY TOKEN", (158, 164, 160), 2, width - margin * 2 - 36)
    c.text(margin + 18, panel_y + 52, values["metricValue"], (250, 241, 204), 5, width - margin * 2 - 36)
    unit_x = margin + 18 + min(c.text_width(values["metricValue"], 5) + 18, width - margin * 2 - 80)
    c.text(unit_x, panel_y + 78, values["metricUnit"], (158, 164, 160), 2, width - unit_x - margin)

    detail = values["breakdown"] or values["sources"]
    if detail:
        c.text(margin, min(height - 44, panel_y + panel_h + 28), detail, (164, 169, 160), 2, width - margin * 2)
    return bytes(c.data)


################################################################################
# COMPONENT_DASHBOARD_V1 — 10 content + 4 bounded visual slots (CJK + emoji via Pillow)
#
# Magic + payload format is shared with the C `fb_speech_overlay` binary:
#   COMPONENT_DASHBOARD_V1
#   title=<...>
#   eyebrow=<...>
#   ... (all content slots optional + bounded visual preset ids)
#
# The original ASCII-only path above (STATS_DASHBOARD_V1) stays untouched.
# Dispatcher: detect magic on the first line, route to the matching render path.
#
# Why a fresh Pillow path instead of extending the bitmap font:
#   - The widget format is meant for arbitrary user-generated content (skill →
#     clawpkg → device), so CJK + future emoji support is non-optional
#   - Pillow + NotoSansCJK / wqy-zenhei is on the Pi base image already
#   - Layout is generic (no widget-specific hardcoded labels like "EQUIV LUNCH"),
#     so any clawpkg renders the same way
################################################################################

COMPONENT_DASHBOARD_V1_SLOTS = [
    "title", "eyebrow", "headline",
    "metricLabel", "metricValue", "metricUnit",
    "badge", "note", "footer", "progress",
    "visualStyle", "visualPalette", "visualLayout", "visualSprite",
]

COMPONENT_TOOL_SPRITES = {
    "coffee": [
        "001010000", "000101000", "011111020", "011111022", "011111020",
        "011111000", "001111000", "000000000", "011111110",
    ],
    "timer": [
        "000111000", "001111100", "000111000", "011111110", "110000011",
        "110030011", "110000011", "011111110", "001111100",
    ],
    "droplet": [
        "000010000", "000111000", "001111100", "011111110", "111111111",
        "111111111", "011111110", "001111100", "000111000",
    ],
    "gauge": [
        "001111100", "011111110", "110000011", "100000001", "100030001",
        "100010001", "110111011", "011111110", "001111100",
    ],
}


def parse_component_dashboard(text: str):
    """Parse COMPONENT_DASHBOARD_V1 payload into {slot: value} dict (all strs)."""
    values = {slot: "" for slot in COMPONENT_DASHBOARD_V1_SLOTS}
    if not text.startswith("COMPONENT_DASHBOARD_V1"):
        return values
    for line in text.splitlines()[1:]:
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        key = key.strip()
        if key in values:
            values[key] = value.strip()
    return values


def _resolve_cjk_font_path(bold: bool = False):
    """Pick the best CJK-capable TTF/TTC available on the Pi base image. Fall
    back to DejaVuSans (Latin-only) if no CJK font is installed — text will
    render as tofu for CJK but the layout will still draw."""
    if bold:
        candidates = [
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
        ]
    else:
        candidates = [
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
            "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        ]
    for p in candidates:
        if os.path.exists(p):
            return p
    return None


# emoji codepoint ranges. Not exhaustive — covers the symbols users typically
# emit (clock, calendar, fire, check, tomato, water, etc.). Anything outside
# falls through to the regular CJK font (and renders as tofu if the CJK font
# doesn't have it, which is fine).
_EMOJI_RANGES = [
    (0x1F300, 0x1F9FF),  # Misc Symbols + Pictographs + Emoticons + Transport + Supplemental + Symbols and Pictographs
    (0x1FA00, 0x1FAFF),  # Symbols and Pictographs Extended-A
    (0x2600, 0x27BF),    # Misc Symbols + Dingbats
    (0x2300, 0x23FF),    # Misc Technical (⏰ ⏱ ⏳)
]

NOTO_COLOR_EMOJI_PATH = "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"
# NotoColorEmoji is a bitmap-strike-only font, only renders at size 109.
# We render to a 109×109 RGBA tile then scale down to the target line height.
NOTO_COLOR_EMOJI_NATIVE_SIZE = 109


def _is_emoji_codepoint(ch: str) -> bool:
    cp = ord(ch)
    return any(lo <= cp <= hi for lo, hi in _EMOJI_RANGES)


def _split_emoji_runs(text: str):
    """Yield (is_emoji, substring) runs from a string. Coalesces consecutive
    same-kind chars so we draw whole substrings instead of glyph-by-glyph."""
    if not text:
        return
    cur_kind = _is_emoji_codepoint(text[0])
    cur = [text[0]]
    for ch in text[1:]:
        k = _is_emoji_codepoint(ch)
        if k == cur_kind:
            cur.append(ch)
        else:
            yield cur_kind, "".join(cur)
            cur_kind = k
            cur = [ch]
    if cur:
        yield cur_kind, "".join(cur)


def _emoji_tile(ch: str, target_px: int, _cache={}):
    """Render a single emoji codepoint to an RGBA Image of size (target_px,
    target_px). Cached because the resize is expensive on the Pi. Returns
    None if NotoColorEmoji isn't installed."""
    from PIL import Image, ImageDraw, ImageFont
    if not os.path.exists(NOTO_COLOR_EMOJI_PATH):
        return None
    key = (ch, target_px)
    if key in _cache:
        return _cache[key]
    try:
        font = ImageFont.truetype(NOTO_COLOR_EMOJI_PATH, NOTO_COLOR_EMOJI_NATIVE_SIZE)
    except Exception:
        return None
    big = Image.new("RGBA", (NOTO_COLOR_EMOJI_NATIVE_SIZE, NOTO_COLOR_EMOJI_NATIVE_SIZE), (0, 0, 0, 0))
    bd = ImageDraw.Draw(big)
    try:
        # Pillow ≥9.2 supports embedded_color for color emoji rendering
        bd.text((0, 0), ch, font=font, embedded_color=True)
    except Exception:
        try:
            bd.text((0, 0), ch, font=font)
        except Exception:
            return None
    scaled = big.resize((target_px, target_px), Image.LANCZOS)
    _cache[key] = scaled
    return scaled


def _draw_mixed(img, draw, xy, text, color, font, line_height=None):
    """Like draw.text, but emoji codepoints are rendered with NotoColorEmoji
    (color bitmap, scaled) and pasted onto `img`; other characters go through
    the regular CJK font. Returns the x advance after the rendered text."""
    if not text:
        return xy[0]
    cur_x, cur_y = xy
    target_emoji_px = line_height or (font.size if hasattr(font, "size") else 16)
    for is_emoji, run in _split_emoji_runs(text):
        if is_emoji:
            for ch in run:
                tile = _emoji_tile(ch, target_emoji_px)
                if tile is None:
                    # font missing — fall back to drawing as text (will be tofu)
                    draw.text((cur_x, cur_y), ch, fill=color, font=font)
                    bb = draw.textbbox((cur_x, cur_y), ch, font=font)
                    cur_x += max(target_emoji_px, bb[2] - bb[0])
                else:
                    img.paste(tile, (cur_x, cur_y), tile)
                    cur_x += target_emoji_px
        else:
            draw.text((cur_x, cur_y), run, fill=color, font=font)
            bb = draw.textbbox((cur_x, cur_y), run, font=font)
            cur_x += bb[2] - bb[0]
    return cur_x


def _draw_volume_hud(draw, width: int, height: int, volume_pct) -> None:
    """Phone-style centered volume HUD: rounded card + drawn speaker icon with
    sound-wave arcs + rounded progress bar + percentage. Shared by the negative
    screen (drawn over the widget) and the pet screen (drawn over a dark frame
    via the --serve "hud" command, since nothing can overlay the live ffmpeg
    pet animation). Self-contained (loads its own font/colors) so it works from
    the persistent --serve loop without the dashboard renderer's closures."""
    from PIL import ImageFont
    vp = max(0, min(100, int(volume_pct)))
    IVORY = (250, 241, 204)
    ORANGE = (255, 163, 31)
    compact = width <= 360
    fp = _resolve_cjk_font_path(bold=True) or _resolve_cjk_font_path(bold=False)

    def _font(px):
        if fp:
            try:
                return ImageFont.truetype(fp, px)
            except Exception:
                pass
        return ImageFont.load_default()

    cw = int(width * 0.78)
    ch = 56 if compact else 84
    cx = (width - cw) // 2
    cy = (height - ch) // 2
    radius = 14 if compact else 20
    draw.rounded_rectangle([cx + 2, cy + 4, cx + cw + 2, cy + ch + 4], radius=radius, fill=(0, 0, 0))
    draw.rounded_rectangle([cx, cy, cx + cw, cy + ch], radius=radius, fill=(24, 28, 26))
    draw.rounded_rectangle([cx, cy, cx + cw, cy + ch], radius=radius, outline=(70, 78, 73), width=1)
    cmid = cy + ch // 2
    s = 7 if compact else 11
    ix = cx + (20 if compact else 30)
    draw.rectangle([ix - s, cmid - s // 2, ix - s // 3, cmid + s // 2], fill=IVORY)
    draw.polygon([(ix - s // 3, cmid - s // 2), (ix + s // 2, cmid - s),
                  (ix + s // 2, cmid + s), (ix - s // 3, cmid + s // 2)], fill=IVORY)
    wave = ORANGE if vp > 0 else (90, 96, 92)
    draw.arc([ix + 1, cmid - s, ix + s + 4, cmid + s], -55, 55, fill=wave, width=2)
    if vp > 45:
        draw.arc([ix + 1, cmid - s - 5, ix + s + 11, cmid + s + 5], -55, 55, fill=wave, width=2)
    f_pct = _font(16 if compact else 24)
    pct_str = f"{vp}"
    pb = draw.textbbox((0, 0), pct_str, font=f_pct)
    pw = pb[2] - pb[0]
    ph = pb[3] - pb[1]
    right_pad = 16 if compact else 24
    draw.text((cx + cw - right_pad - pw, cmid - ph // 2 - pb[1]), pct_str, fill=IVORY, font=f_pct)
    bx = ix + s + (16 if compact else 24)
    bh = 8 if compact else 12
    by = cmid - bh // 2
    bw = (cx + cw - right_pad - pw - 12) - bx
    if bw < bh:
        bw = bh
    draw.rounded_rectangle([bx, by, bx + bw, by + bh], radius=bh // 2, fill=(50, 54, 51))
    fw = int(bw * vp / 100)
    if fw >= bh:
        draw.rounded_rectangle([bx, by, bx + fw, by + bh], radius=bh // 2, fill=ORANGE)
    elif fw > 0:
        draw.rectangle([bx, by, bx + fw, by + bh], fill=ORANGE)


COMPONENT_VISUAL_PALETTES = {
    "candy": {
        "background": (90, 215, 255), "background_alt": (112, 225, 255),
        "surface": (255, 243, 184), "ink": (43, 35, 103),
        "primary": (255, 79, 139), "secondary": (255, 216, 61),
        "light": (255, 253, 240), "shadow": (103, 80, 164),
    },
    "sunset": {
        "background": (255, 155, 115), "background_alt": (255, 176, 132),
        "surface": (255, 234, 182), "ink": (85, 39, 95),
        "primary": (233, 63, 134), "secondary": (255, 210, 63),
        "light": (255, 248, 232), "shadow": (132, 63, 114),
    },
    "mint": {
        "background": (120, 230, 189), "background_alt": (151, 239, 204),
        "surface": (244, 255, 208), "ink": (23, 63, 95),
        "primary": (255, 95, 119), "secondary": (255, 209, 102),
        "light": (249, 255, 248), "shadow": (44, 122, 120),
    },
    "arcade": {
        "background": (70, 59, 143), "background_alt": (83, 70, 163),
        "surface": (255, 240, 168), "ink": (32, 25, 79),
        "primary": (255, 79, 184), "secondary": (60, 242, 210),
        "light": (255, 253, 240), "shadow": (33, 25, 73),
    },
}


def render_pixel_component_dashboard_pillow(values, width: int, height: int, volume_pct=None) -> bytes:
    """Render the bounded pixel-game preset with bright palette + built-in sprite."""
    from PIL import Image, ImageDraw, ImageFont

    palettes = COMPONENT_VISUAL_PALETTES
    sprites = {
        "target": [
            "000222000", "002111200", "021222120", "212111212", "212131212",
            "212111212", "021222120", "002111200", "000222000",
        ],
        "trophy": [
            "001111100", "211111112", "221111122", "022111220", "002111200",
            "000111000", "000111000", "001111100", "011111110",
        ],
        "star": [
            "000100000", "000100000", "010111010", "001111100", "111141111",
            "001111100", "010111010", "000101000", "000100000",
        ],
        "bolt": [
            "000111000", "001111000", "011110000", "111111000", "001111100",
            "000111110", "000011100", "000011000", "000010000",
        ],
        **COMPONENT_TOOL_SPRITES,
        "blocks": [
            "110022200", "110002000", "000002000", "033333000", "000330000",
            "000330044", "000000044", "111122220", "111122220",
        ],
        "snake": [
            "000000000", "011100000", "010100000", "010111000", "000001000",
            "000001330", "000000303", "000222300", "000222000",
        ],
        "mole-ready": [
            "000000000000000", "000000000000000", "000000000000000",
            "000000000000000", "000000000000000", "000000000000000",
            "033000330003300", "333303333033330", "033000330003300",
        ],
        "mole-left": [
            "100100000000000", "111100000000000", "141400000000000",
            "122100000000000", "011000000000000", "011000000000000",
            "033000330003300", "333303333033330", "033000330003300",
        ],
        "mole-center": [
            "000001001000000", "000001111000000", "000001414000000",
            "000001221000000", "000000110000000", "000000110000000",
            "033000330003300", "333303333033330", "033000330003300",
        ],
        "mole-right": [
            "000000000010010", "000000000011110", "000000000014140",
            "000000000012210", "000000000001100", "000000000001100",
            "033000330003300", "333303333033330", "033000330003300",
        ],
    }
    palette = palettes.get(values.get("visualPalette"), palettes["candy"])
    img = Image.new("RGB", (width, height), palette["background"])
    draw = ImageDraw.Draw(img)
    sx = width / 640.0
    sy = height / 480.0
    s = lambda value: max(1, int(round(value * min(sx, sy))))
    px = lambda value: int(round(value * sx))
    py = lambda value: int(round(value * sy))
    for yy in range(0, height, s(32)):
        for xx in range(0, width, s(32)):
            color = palette["background_alt"] if ((xx // s(32)) + (yy // s(32))) % 2 else palette["background"]
            draw.rectangle([xx, yy, xx + s(32), yy + s(32)], fill=color)

    regular = _resolve_cjk_font_path(bold=False)
    bold = _resolve_cjk_font_path(bold=True) or regular

    def font(size, is_bold=False):
        path = bold if is_bold else regular
        if path:
            try:
                return ImageFont.truetype(path, s(size))
            except Exception:
                pass
        return ImageFont.load_default()

    def panel(x, y, panel_width, panel_height):
        x0, y0, w0, h0 = px(x), py(y), px(panel_width), py(panel_height)
        shadow = s(6); border = s(4)
        draw.rectangle([x0 + shadow, y0 + shadow, x0 + w0 + shadow, y0 + h0 + shadow], fill=palette["shadow"])
        draw.rectangle([x0, y0, x0 + w0, y0 + h0], fill=palette["ink"])
        draw.rectangle([x0 + border, y0 + border, x0 + w0 - border, y0 + h0 - border], fill=palette["surface"])

    def centered(text, center_x, box_y, box_h, max_width, size, color, is_bold=False):
        if not text:
            return
        current = font(size, is_bold)
        bbox = draw.textbbox((0, 0), text, font=current)
        text_width = bbox[2] - bbox[0]
        while text_width > px(max_width) and size > 8:
            size -= 1
            current = font(size, is_bold)
            bbox = draw.textbbox((0, 0), text, font=current)
            text_width = bbox[2] - bbox[0]
        text_height = bbox[3] - bbox[1]
        draw.text((px(center_x) - text_width // 2, py(box_y) + (py(box_h) - text_height) // 2 - bbox[1]),
                  text, fill=color, font=current)

    panel(28, 18, 584, 62)
    title = values.get("title") or "PIXEL GAME"
    draw.text((px(44), py(34)), title, fill=palette["ink"], font=font(24, True))
    time_text = values.get("note") or values.get("eyebrow") or ""
    if time_text:
        draw.rectangle([px(452), py(32), px(592), py(64)], fill=palette["secondary"])
        centered(time_text, 522, 32, 32, 128, 16, palette["ink"], True)

    scoreboard_first = values.get("visualLayout") == "scoreboard"
    play_x = 248 if scoreboard_first else 28
    score_x = 28 if scoreboard_first else 410
    panel(play_x, 104, 364, 248)
    panel(score_x, 104, 202, 248)
    if values.get("eyebrow"):
        centered(values["eyebrow"], play_x + 182, 116, 24, 330, 15, palette["ink"], True)

    rows = sprites.get(values.get("visualSprite"), sprites["target"])
    columns = len(rows[0])
    cell = s(12 if columns > 9 else 16)
    sprite_x = px(play_x + 182) - columns * cell // 2
    sprite_y = py(144)
    sprite_colors = {
        "1": palette["primary"], "2": palette["secondary"],
        "3": palette["ink"], "4": palette["light"],
    }
    for row_index, row in enumerate(rows):
        for column_index, tone in enumerate(row):
            if tone == "0":
                continue
            x0 = sprite_x + column_index * cell
            y0 = sprite_y + row_index * cell
            draw.rectangle([x0, y0, x0 + cell, y0 + cell], fill=sprite_colors[tone])
    centered(values.get("headline") or "READY", play_x + 182, 294, 44, 324, 24, palette["primary"], True)

    draw.rectangle([px(score_x + 24), py(132), px(score_x + 178), py(162)], fill=palette["ink"])
    centered(values.get("metricLabel") or "SCORE", score_x + 101, 132, 30, 146, 15, palette["light"], True)
    centered(values.get("metricValue") or "0", score_x + 101, 178, 104, 174, 62, palette["primary"], True)
    if values.get("metricUnit"):
        centered(values["metricUnit"], score_x + 101, 288, 30, 174, 15, palette["ink"], True)
    if values.get("badge"):
        draw.rectangle(
            [px(score_x + 148), py(116), px(score_x + 182), py(150)],
            fill=palette["secondary"],
        )
        centered(values["badge"], score_x + 165, 116, 34, 30, 14, palette["ink"], True)

    progress = values.get("progress") or ""
    if progress:
        try:
            pct = max(0, min(100, int(progress.split(":", 1)[0])))
        except ValueError:
            pct = 0
        draw.rectangle([px(28), py(374), px(612), py(388)], fill=palette["ink"])
        if pct:
            draw.rectangle([px(32), py(378), px(32 + 576 * pct / 100), py(384)], fill=palette["primary"])

    panel(28, 406, 584, 54)
    centered(values.get("footer") or "点击开始", 320, 410, 46, 548, 21, palette["ink"], True)
    if volume_pct is not None:
        _draw_volume_hud(draw, width, height, int(volume_pct))
    return _pack_rgb565(img, width, height)


def render_component_dashboard_pillow(values, width: int, height: int, volume_pct=None) -> bytes:
    """Render the classic 10 content slots to an RGB565LE frame using Pillow.

    Layout (compact 320×240; scales for larger screens):
      ┌────────────────────────────────┐
      │ title          [badge circle]  │  ← top row
      │ eyebrow                        │
      │                                │
      │   headline (large)             │  ← middle
      │   ┌─ metricLabel ─────────┐    │
      │   │  metricValue metricUnit│   │  ← metric panel
      │   │  note                  │   │
      │   │  [progress bar (opt)]  │   │
      │   └────────────────────────┘   │
      │ footer (small, hardware hints) │  ← bottom
      └────────────────────────────────┘
    """
    pixel_tool = (
        values.get("visualStyle") == "pixel"
        and values.get("visualLayout") == "tool"
    )
    if values.get("visualStyle") == "pixel" and not pixel_tool:
        return render_pixel_component_dashboard_pillow(values, width, height, volume_pct)

    from PIL import Image, ImageDraw, ImageFont

    BG = (5, 7, 6)
    PANEL = (11, 14, 13)
    DIM = (158, 164, 160)
    IVORY = (250, 241, 204)
    ORANGE = (255, 163, 31)
    GREEN = (85, 193, 133)
    PROGRESS_BG = (40, 40, 40)
    FOOTER = (111, 183, 142)
    BADGE_INK = (19, 18, 13)
    TOP_STRIPE = (13, 39, 25)
    PAGE_INK = IVORY
    PAGE_MUTED = DIM
    palette = COMPONENT_VISUAL_PALETTES.get(values.get("visualPalette"))
    if pixel_tool and palette:
        ORANGE = palette["primary"]
        GREEN = palette["primary"]
        FOOTER = palette["primary"]
        TOP_STRIPE = palette["primary"]
    elif palette:
        BG = palette["background"]
        PANEL = palette["surface"]
        DIM = palette["shadow"]
        IVORY = palette["ink"]
        ORANGE = palette["primary"]
        GREEN = palette["secondary"]
        PROGRESS_BG = palette["background_alt"]
        arcade_page = values.get("visualPalette") == "arcade"
        PAGE_INK = palette["light"] if arcade_page else palette["ink"]
        PAGE_MUTED = palette["secondary"] if arcade_page else palette["shadow"]
        FOOTER = PAGE_INK
        BADGE_INK = palette["ink"]
        TOP_STRIPE = palette["shadow"]

    img = Image.new("RGB", (width, height), BG)
    draw = ImageDraw.Draw(img)
    # subtle top accent, tinted by the selected bounded palette.
    draw.rectangle([0, 0, width, 2], fill=TOP_STRIPE)

    regular_font_path = _resolve_cjk_font_path(bold=False)
    bold_font_path = _resolve_cjk_font_path(bold=True) or regular_font_path

    def font(px: int, bold: bool = False):
        fp = bold_font_path if bold else regular_font_path
        if fp:
            try:
                return ImageFont.truetype(fp, px)
            except Exception:
                pass
        return ImageFont.load_default()

    # Adaptive sizing tuned for 320×240. Reduced metric_value (46→36) +
    # smaller headline (22→18) + bumped note (11→12) so the panel breathes
    # and lines don't visually collide. f_unit measured with font.getmetrics()
    # so unit baseline aligns to value baseline (eliminates the "unit floats
    # in the middle of value" look).
    compact = width <= 360
    if compact:
        f_title = font(18, bold=True); f_eyebrow = font(11); f_headline = font(18, bold=True)
        f_label = font(12); f_value = font(36, bold=True); f_unit = font(13)
        f_score_value = font(46, bold=True)
        f_note = font(12); f_footer = font(12); f_badge = font(13, bold=True)
        f_top_headline = font(13, bold=True)
        # margin = safe inset on all 4 sides. The 320×240 LCD bezel rounds
        # the corners and crops ~6-8px each side on the physical panel —
        # 14px keeps text visibly clear of the bezel even at the corners.
        margin = 14
    else:
        f_title = font(30, bold=True); f_eyebrow = font(18); f_headline = font(32, bold=True)
        f_label = font(20); f_value = font(60, bold=True); f_unit = font(20)
        f_score_value = font(78, bold=True)
        f_note = font(16); f_footer = font(18); f_badge = font(20, bold=True)
        f_top_headline = font(22, bold=True)
        margin = 20

    title = values["title"]
    eyebrow = values["eyebrow"]
    headline = values["headline"]
    metric_label = values["metricLabel"]
    metric_value = values["metricValue"]
    metric_unit = values["metricUnit"]
    badge = values["badge"]
    note = values["note"]
    footer = values["footer"]
    progress = values["progress"]  # "value:label" or "value" or ""
    if pixel_tool:
        eyebrow = ""
        note = ""
    scoreboard = values.get("visualLayout") == "scoreboard" or pixel_tool

    y = margin
    # Title is plain top-left text, matching the client preview and avoiding
    # the cramped bordered pill on the small physical LCD. Badge remains a
    # compact top-right circle when value is meaningful.
    title_h = f_title.size if hasattr(f_title, "size") else 20
    badge_show = badge and badge not in ("0", "—", "-", "")
    title_w = 0
    title_x = margin
    if pixel_tool:
        sprite = COMPONENT_TOOL_SPRITES.get(values.get("visualSprite"))
        if sprite:
            cell = 2 if compact else 3
            icon_y = y + max(0, (title_h - len(sprite) * cell) // 2)
            for row_index, row in enumerate(sprite):
                for column_index, tone in enumerate(row):
                    if tone != "0":
                        draw.rectangle(
                            [
                                margin + column_index * cell,
                                icon_y + row_index * cell,
                                margin + (column_index + 1) * cell - 1,
                                icon_y + (row_index + 1) * cell - 1,
                            ],
                            fill=ORANGE,
                        )
            title_x += len(sprite[0]) * cell + (6 if compact else 10)
    if title:
        bb = draw.textbbox((0, 0), title, font=f_title)
        title_w = bb[2] - bb[0]
        emoji_count = sum(1 for ch in title if _is_emoji_codepoint(ch))
        if emoji_count:
            title_w += emoji_count * (title_h - 4)
        title_w += title_x - margin
        _draw_mixed(img, draw, (title_x, y), title,
                    PAGE_INK, f_title, line_height=title_h)
        title_advance = title_h
    else:
        title_advance = title_h

    headline_top_drawn = False
    if badge_show:
        bx = width - margin - (14 if compact else 24)
        by = y + (title_advance // 2 if title else title_h // 2)
        r = 13 if compact else 24
        draw.ellipse([bx - r, by - r, bx + r, by + r], fill=GREEN)
        draw.ellipse([bx - r + 1, by - r + 1, bx + r - 1, by + r - 1], outline=PAGE_INK, width=1)
        tb_bbox = draw.textbbox((0, 0), badge, font=f_badge)
        tw = tb_bbox[2] - tb_bbox[0]; th = tb_bbox[3] - tb_bbox[1]
        draw.text((bx - tw // 2, by - th // 2 - 2), badge, fill=BADGE_INK, font=f_badge)

        if headline:
            head_h = f_top_headline.size if hasattr(f_top_headline, "size") else 13
            hb = draw.textbbox((0, 0), headline, font=f_top_headline)
            head_w = hb[2] - hb[0]
            emoji_n = sum(1 for ch in headline if _is_emoji_codepoint(ch))
            if emoji_n:
                head_w += emoji_n * head_h
            callout_right = bx - (13 if compact else 24) - (8 if compact else 12)
            callout_left = max(margin + title_w + (10 if compact else 18), width // 2)
            if callout_right - callout_left > 40:
                hx = max(callout_left, callout_right - head_w)
                _draw_mixed(img, draw, (hx + 1, y + 3), headline, (0, 0, 0),
                            f_top_headline, line_height=head_h)
                _draw_mixed(img, draw, (hx, y + 2), headline, ORANGE,
                            f_top_headline, line_height=head_h)
                headline_top_drawn = True
    y += max(title_advance, 26 if compact else 48) + 6

    if eyebrow:
        _draw_mixed(img, draw, (margin, y), eyebrow, PAGE_MUTED, f_eyebrow,
                    line_height=f_eyebrow.size if hasattr(f_eyebrow, "size") else 11)
        y += (f_eyebrow.size if hasattr(f_eyebrow, "size") else 11) + 4
    else:
        y += 4

    headline_h = f_headline.size if hasattr(f_headline, "size") else 22
    if headline and not headline_top_drawn:
        # text-shadow effect: draw a 1px-offset dark shadow first, then the
        # real ivory/orange text on top. Matches client .cds-headline's
        # text-shadow: 0 1px 0 rgba(0,0,0,0.4); — gives the text more depth
        # on the LCD so it doesn't disappear into the dark background.
        _draw_mixed(img, draw, (margin + 1, y + 1), headline, (0, 0, 0), f_headline, line_height=headline_h)
        _draw_mixed(img, draw, (margin, y), headline, ORANGE, f_headline, line_height=headline_h)
        y += headline_h + 4

    # metric panel — matches client .cds-metric-panel:
    #   rounded dark bg (8px corner), no border, no left stripe (cleaner look).
    panel_x = margin
    panel_y = y + 2
    panel_w = width - margin * 2
    panel_h = height - panel_y - margin - (f_footer.size if hasattr(f_footer, "size") else 12) - 10
    if panel_h > 30:
        try:
            draw.rounded_rectangle(
                [panel_x, panel_y, panel_x + panel_w, panel_y + panel_h],
                radius=4 if scoreboard else 8,
                fill=PANEL,
                outline=IVORY if scoreboard else None,
                width=2 if scoreboard else 1,
            )
        except Exception:
            draw.rectangle([panel_x, panel_y, panel_x + panel_w, panel_y + panel_h], fill=PANEL)

        # Layout v3 — strictly stacked with generous gaps. Each section gets
        # at least 6px of breathing room from its neighbor. Unit sits flush
        # right of value with a wider 10px gap and is properly baseline-aligned
        # via Pillow font.getmetrics() (ascent computes where the actual text
        # baseline lands inside the draw.text() bounding box).
        py = panel_y + 12
        inner_x = panel_x + 14
        if scoreboard:
            def draw_centered_mixed(text, top, text_font, color, line_height):
                bbox = draw.textbbox((0, 0), text, font=text_font)
                text_width = bbox[2] - bbox[0]
                text_width += sum(1 for char in text if _is_emoji_codepoint(char)) * line_height
                left = max(inner_x, (width - text_width) // 2)
                _draw_mixed(
                    img, draw, (left, top), text, color, text_font,
                    line_height=line_height,
                )

            if metric_label:
                label_h = f_label.size if hasattr(f_label, "size") else 12
                draw_centered_mixed(metric_label, py, f_label, DIM, label_h)
                py += label_h + (8 if compact else 12)
            if metric_value:
                value_h = f_score_value.size if hasattr(f_score_value, "size") else 46
                draw_centered_mixed(metric_value, py, f_score_value, IVORY, value_h)
                py += value_h + (5 if compact else 10)
            if metric_unit:
                unit_h = f_unit.size if hasattr(f_unit, "size") else 13
                draw_centered_mixed(metric_unit, py, f_unit, DIM, unit_h)
                py += unit_h + 6
            if note:
                note_h = f_note.size if hasattr(f_note, "size") else 12
                draw_centered_mixed(note, py, f_note, DIM, note_h)
        else:
            if metric_label:
                label_h = f_label.size if hasattr(f_label, "size") else 12
                _draw_mixed(img, draw, (inner_x, py), metric_label, DIM, f_label, line_height=label_h)
                py += label_h + 6
            if metric_value:
                value_h = f_value.size if hasattr(f_value, "size") else 36
                end_x = _draw_mixed(img, draw, (inner_x, py), metric_value, IVORY, f_value, line_height=value_h)
                if metric_unit:
                    unit_h = f_unit.size if hasattr(f_unit, "size") else 13
                    v_asc = f_value.getmetrics()[0] if hasattr(f_value, "getmetrics") else value_h
                    u_asc = f_unit.getmetrics()[0] if hasattr(f_unit, "getmetrics") else unit_h
                    _draw_mixed(img, draw, (end_x + 10, py + v_asc - u_asc), metric_unit, DIM, f_unit,
                                line_height=unit_h)
                py += value_h + 10  # generous gap below the big number
            if note:
                note_h = f_note.size if hasattr(f_note, "size") else 12
                _draw_mixed(img, draw, (inner_x, py), note, DIM, f_note, line_height=note_h)
                py += note_h + 4

        # progress bar — "value" or "value:label"
        if progress:
            colon = progress.find(":")
            pct_text = progress[:colon] if colon >= 0 else progress
            label_text = progress[colon + 1:] if colon >= 0 else ""
            try:
                pct = max(0, min(100, int(pct_text)))
            except ValueError:
                pct = -1
            if pct >= 0:
                bar_h = 4 if compact else 8
                bar_y = panel_y + panel_h - bar_h - 8
                bar_x = inner_x
                bar_w = panel_w - 24
                if bar_w < 1: bar_w = 1
                draw.rectangle([bar_x, bar_y, bar_x + bar_w, bar_y + bar_h], fill=PROGRESS_BG)
                fill_w = (bar_w * pct) // 100
                if fill_w > 0:
                    draw.rectangle([bar_x, bar_y, bar_x + fill_w, bar_y + bar_h], fill=ORANGE)
                note_h = f_note.size if hasattr(f_note, "size") else 11
                if label_text:
                    _draw_mixed(img, draw, (bar_x, bar_y - note_h - 2), label_text, DIM, f_note,
                                line_height=note_h)
                pct_str = f"{pct}%"
                pb_bbox = draw.textbbox((0, 0), pct_str, font=f_note)
                pw = pb_bbox[2] - pb_bbox[0]
                draw.text((bar_x + bar_w - pw, bar_y - note_h - 2), pct_str, fill=IVORY, font=f_note)

    if footer:
        # Footer: centered + dashed top divider, matches client .cds-footer
        footer_h = f_footer.size if hasattr(f_footer, "size") else 12
        footer_y = height - margin - footer_h
        # dashed divider above (segments of 4px ON / 4px OFF)
        divider_y = footer_y - 5
        for sx in range(margin, width - margin, 8):
            draw.rectangle([sx, divider_y, sx + 4, divider_y + 1], fill=PAGE_MUTED)
        # measure footer text width to center
        # _draw_mixed needs total advance — quick approx via bbox + emoji count
        fb = draw.textbbox((0, 0), footer, font=f_footer)
        ftw = fb[2] - fb[0]
        emoji_n = sum(1 for ch in footer if _is_emoji_codepoint(ch))
        ftw += emoji_n * footer_h
        fx = max(margin, (width - ftw) // 2)
        _draw_mixed(img, draw, (fx, footer_y), footer, FOOTER,
                    f_footer, line_height=footer_h)

    # Transient volume HUD (drawn last, on top) — see _draw_volume_hud. Shown on
    # the negative screen here; the same routine draws it over a dark frame on
    # the pet screen via the --serve "hud" command (board can't overlay ffmpeg).
    if volume_pct is not None:
        _draw_volume_hud(draw, width, height, int(volume_pct))

    # convert RGB → RGB565LE bytes
    return _pack_rgb565(img, width, height)


def _pack_rgb565(img, width: int, height: int) -> bytes:
    """RGB888 PIL image → RGB565 little-endian bytes.

    numpy-vectorized when available — the previous per-pixel python double loop
    took ~1.5s/frame on the Zero 2 W and was the dominant latency for the volume
    HUD and every negative-screen refresh. Falls back to the loop if numpy or
    the conversion is unavailable, so behavior is unchanged on minimal images.
    """
    try:
        import numpy as np
        arr = np.asarray(img.convert("RGB"), dtype=np.uint16)
        r = arr[:, :, 0]
        g = arr[:, :, 1]
        b = arr[:, :, 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        return v.astype("<u2").tobytes()
    except Exception:
        raw = bytearray(width * height * 2)
        px = img.load()
        for yy in range(height):
            for xx in range(width):
                r, g, b = px[xx, yy]
                v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                off = (yy * width + xx) * 2
                raw[off] = v & 0xFF
                raw[off + 1] = (v >> 8) & 0xFF
        return bytes(raw)


def _resolve_input_path(root, input_path_arg):
    """Explicit --input wins; else the active widget's .widget-display, else the
    legacy .stats-display (board-server Codex/Claude token usage)."""
    if input_path_arg:
        return Path(input_path_arg)
    root = Path(root)
    widget_display = root / ".widget-display"
    widget_id = ""
    try:
        widget_id = (root / ".active-widget").read_text(encoding="utf-8").strip()
    except OSError:
        pass
    return widget_display if (widget_id and widget_display.exists()) else (root / ".stats-display")


def _read_volume_pct(root, require_fresh=True):
    """Parse .volume-display ('<pct>\\n<epoch_ms>'). When require_fresh, only
    return the pct if the write is < 2s old (negative-screen overlay gating);
    otherwise return the raw pct (pet-screen HUD, whose timing the caller owns)."""
    try:
        vlines = (Path(root) / ".volume-display").read_text().strip().splitlines()
        if vlines and vlines[0].strip():
            import time
            vp = int(vlines[0].strip())
            if not require_fresh:
                return vp
            window_ms = int(os.environ.get("PET_CLAW_FB_VOLUME_HUD_WINDOW_MS", "1000"))
            if len(vlines) >= 2 and 0 <= int(time.time() * 1000) - int(vlines[1].strip()) <= window_ms:
                return vp
    except (OSError, ValueError):
        pass
    return None


def _render_frame(root, input_path_arg, width, height, rotate, hud_only=False):
    """Render one RGB565 frame (rotation applied). hud_only draws just the volume
    HUD over a dark background — used on the pet screen, where the live ffmpeg
    animation cannot be overlaid, so fb-display pauses it and shows this."""
    if hud_only:
        from PIL import Image, ImageDraw
        img = Image.new("RGB", (width, height), (8, 10, 9))
        vp = _read_volume_pct(root, require_fresh=False)
        _draw_volume_hud(ImageDraw.Draw(img), width, height, 0 if vp is None else vp)
        frame = _pack_rgb565(img, width, height)
    else:
        path = _resolve_input_path(root, input_path_arg)
        try:
            payload = path.read_text()
        except OSError:
            payload = "STATS_DASHBOARD_V1\n"
        volume_pct = _read_volume_pct(root, require_fresh=True)
        if payload.startswith("COMPONENT_DASHBOARD_V1"):
            frame = render_component_dashboard_pillow(parse_component_dashboard(payload), width, height, volume_pct)
        else:
            frame = render(parse_payload(payload), width, height)
    return rotate_frame_rgb565(frame, width, height, normalize_rotation(rotate))


def _write_fb(fb_path, frame):
    with open(fb_path, "r+b", buffering=0) as fb:
        fb.seek(0)
        fb.write(frame)


def main() -> int:
    parser = argparse.ArgumentParser(description="Render STATS_DASHBOARD_V1 or COMPONENT_DASHBOARD_V1 to an RGB565 framebuffer frame.")
    parser.add_argument("root", nargs="?", default=".", help="runtime root, default: current directory")
    parser.add_argument("--input", dest="input_path", help="stats payload path, default: ROOT/.stats-display")
    parser.add_argument("--fb", default=os.environ.get("PET_CLAW_FB_DEV", "/dev/fb0"), help="framebuffer path")
    parser.add_argument("--output-frame", help="write raw RGB565 frame to this file instead of framebuffer")
    parser.add_argument("--width", type=int, help="frame width")
    parser.add_argument("--height", type=int, help="frame height")
    parser.add_argument("--rotate", default=os.environ.get("PET_CLAW_FB_ROTATE", "0"), help="frame rotation: 0 or 180")
    parser.add_argument("--serve", action="store_true",
                        help="persistent mode: read render commands from stdin (render [path] | hud | quit), "
                             "writing each frame to --fb. Avoids per-frame python/PIL/numpy startup (~1.2s).")
    args = parser.parse_args()

    width, height = (args.width, args.height) if args.width and args.height else read_fb_size(args.fb)

    if args.serve:
        import sys
        for line in sys.stdin:
            cmd = line.strip()
            if not cmd:
                continue
            parts = cmd.split(None, 1)
            op = parts[0]
            if op == "quit":
                break
            try:
                if op == "hud":
                    _write_fb(args.fb, _render_frame(args.root, None, width, height, args.rotate, hud_only=True))
                elif op == "render":
                    inp = parts[1] if len(parts) > 1 else None
                    _write_fb(args.fb, _render_frame(args.root, inp, width, height, args.rotate))
                sys.stdout.write("ok\n")
            except Exception as e:  # keep the daemon alive across transient errors
                sys.stderr.write("fb-stats serve render error: %s\n" % e)
                sys.stdout.write("err\n")
            sys.stdout.flush()
        return 0

    frame = _render_frame(args.root, args.input_path, width, height, args.rotate)
    if args.output_frame:
        Path(args.output_frame).write_bytes(frame)
        return 0
    _write_fb(args.fb, frame)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
