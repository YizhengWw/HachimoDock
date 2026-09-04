# Third-Party Notices

HachimoDock's non-commercial license applies only to its own materials. It does not add restrictions to independently licensed third-party software, drivers or fonts. The original terms in individual source files and dependency packages remain applicable.

## MiSans font

`firmware/main/pet_p4_font_cn.c` embeds glyph data rendered from MiSans Medium. MiSans is provided by Xiaomi under the [MiSans Font Intellectual Property License Agreement](https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf), not the HachimoDock license. Original font files are not distributed here. See the [official embedding FAQ](https://hyperos.mi.com/font/zh/faq/). Do not extract and redistribute the font as a standalone font product.

## ESP32-P4 board drivers and build dependencies

The board support code in `firmware/components/esp32_p4_wifi6_touch_lcd_4_3/` and display driver in `firmware/components/esp_lcd_st7701/` preserve their Apache-2.0 licenses and original copyright headers. The local `esp_lvgl_adapter` directory is a project build shim. ESP-IDF, LVGL, TinyUSB and other downloaded components retain their respective licenses in their distributions; exact resolved versions are recorded in `firmware/dependencies.lock`.

Desktop npm and Rust dependency versions are recorded in package lockfiles and `pc/src-tauri/Cargo.lock`. Their licenses accompany the upstream packages and remain independent of HachimoDock's first-party license.

This file records third-party notices for source code incorporated into
HachimoDock. It does not replace the license selected for HachimoDock's own
code, firmware, documentation, or media assets.

## FFmpeg 8.1.2 — LGPL-2.1-or-later

- Project: <https://ffmpeg.org/>
- Source release: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256:
  `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- License: GNU Lesser General Public License, version 2.1 or later

Pet Manager desktop packages include a separately invoked FFmpeg executable.
The corresponding source archive and LGPL license are distributed with the
official release and installed package.

## zlib 1.3.1 — zlib License

- Project and source: <https://zlib.net/>
- Copyright: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
- License: zlib License

The Windows LGPL FFmpeg executable statically incorporates zlib. This software
is provided “as-is”, without any express or implied warranty. Permission is
granted to use, alter, and redistribute it, subject to the zlib license terms:
do not misrepresent the origin, mark altered source versions plainly, and do
not remove or alter the license notice from source distributions.

## Clawd on Desk — MIT License

- Upstream project: <https://github.com/rullerzhou-afk/clawd-on-desk>
- Copyright: Copyright (c) 2026 rullerzhou-afk

HachimoDock includes portions of software distributed under the MIT License.

### MIT License

Copyright (c) 2026 rullerzhou-afk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
