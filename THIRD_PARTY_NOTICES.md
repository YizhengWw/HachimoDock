# Third-Party Notices

HachimoDock's non-commercial license applies only to its own materials. It does not add restrictions to independently licensed third-party software, drivers or fonts. The original terms in individual source files and dependency packages remain applicable.

## Xiaomi MiLoCo — 智能家居

**Xiaomi MiLoCo** —— 感谢 Xiaomi MiLoCo 团队开放智能家居能力。本项目的家居控制云协议基于其开源 SDK `miloco-miot` 2026.8.6 的 `cloud.py`、`const.py` 适配为 Rust 实现，并参考 DeskBot V2 的设备能力调用方式；安装包不直接分发完整 Python SDK。
相关 MiLoCo 授权作品的版权归小米所有，适用 Xiaomi Miloco License；其版权标识、免责声明和许可证副本随相关实现一并保留。
相关源文件保留 Copyright (C) 2025 Xiaomi Corporation 标识。原始许可证见 [Xiaomi MiLoCo 许可证](licenses/Xiaomi-Miloco-LICENSE.md)，上游项目为 [XiaoMi/xiaomi-miloco](https://github.com/XiaoMi/xiaomi-miloco)。

本项目的发布不改变 MiLoCo 原始许可证，也不向第三方授予额外的商业、再许可或商标使用权。该许可证仅授权其规定范围内的非商业使用，并明确不自动授权开发其他 APP、Web 服务等用途；超出许可范围的使用及分发需另行取得权利人授权。“非商业”或保留致谢不替代该项授权。

实现不内置用户米家账号、设备凭据或公司网络地址，不打包上游摄像头库。账号凭据由用户在本机授权后保存：Windows 使用系统凭据存储，macOS 使用仅当前用户可访问的本地文件。

## MiSans font

`firmware/main/pet_p4_font_cn.c` embeds glyph data rendered from MiSans Medium. MiSans is provided by Xiaomi under the [MiSans Font Intellectual Property License Agreement](https://hyperos.mi.com/font-download/MiSans%E5%AD%97%E4%BD%93%E7%9F%A5%E8%AF%86%E4%BA%A7%E6%9D%83%E8%AE%B8%E5%8F%AF%E5%8D%8F%E8%AE%AE.pdf), not the HachimoDock license. Original font files are not distributed here. See the [official embedding FAQ](https://hyperos.mi.com/font/zh/faq/). Do not extract and redistribute the font as a standalone font product.

## ESP32-P4 board drivers and build dependencies

The duplex audio frontend uses ESP-SR 2.5.3 AEC, noise suppression and VAD on
Espressif ESP32-P4 hardware. Source and component distribution:
<https://github.com/espressif/esp-sr>. Its original hardware-scoped license is
preserved in [Espressif ESP-SR LICENSE](licenses/Espressif-ESP-SR-LICENSE.txt).
DSP and other transitive dependencies retain their upstream licenses and are
pinned independently for each silicon family in the firmware lockfiles.

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
