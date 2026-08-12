# Third-Party Notices

This file records third-party notices for source code incorporated into
HachimoDock. It does not replace the license selected for HachimoDock's own
code, firmware, documentation, or media assets.

## FFmpeg 8.1.2 — LGPL-2.1-or-later

- Project: <https://ffmpeg.org/>
- Source release: <https://ffmpeg.org/releases/ffmpeg-8.1.2.tar.xz>
- Source SHA-256:
  `464beb5e7bf0c311e68b45ae2f04e9cc2af88851abb4082231742a74d97b524c`
- License: GNU Lesser General Public License, version 2.1 or later

Pet Manager desktop packages include a separately invoked FFmpeg executable,
not a library linked into the proprietary application. The packaged executable
is built with `--disable-gpl --disable-nonfree`, does not enable libx264 or
OpenH264, and uses only the operating-system H.264 encoder: VideoToolbox on
macOS and Media Foundation on Windows. Exact configuration, source location,
binary checksum, the complete LGPL text, and the complete corresponding FFmpeg
8.1.2 source archive are installed under `tools/`.

Recipients may replace the separately installed executable with a compatible
modified LGPL build. Retain the packaged source archive and LGPL text when
redistributing Pet Manager packages.

## zlib 1.3.1 — zlib License

- Project and source: <https://zlib.net/>
- Copyright: Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler
- License: zlib License

The Windows LGPL FFmpeg executable statically incorporates zlib. This software
is provided “as-is”, without any express or implied warranty. Permission is
granted to use, alter, and redistribute it, subject to the zlib license terms:
do not misrepresent the origin, mark altered source versions plainly, and do
not remove or alter the license notice from source distributions.

## Clawd on Desk — MIT-era source code

- Upstream project: <https://github.com/rullerzhou-afk/clawd-on-desk>
- Copyright: Copyright (c) 2026 rullerzhou-afk
- Source-license boundary: upstream commit
  `19e8f82493b0993554df62b3eba419b1127fff14` is the last audited MIT commit;
  upstream commit `3b6277ff39b4473bd0b0a09d55a695b176c815e9` changes the source license to
  AGPL-3.0.
- HachimoDock only incorporates and modifies source from that audited MIT-era
  boundary. The public binary release does not include Clawd artwork or source
  imported from the later AGPL-licensed history.

The HachimoDock bridge includes and modifies source files published by Clawd
on Desk while that source was available under the MIT License. The audited
HachimoDock tree does not include byte-identical Clawd artwork.

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
