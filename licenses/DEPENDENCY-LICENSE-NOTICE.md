# Third-party dependency licensing and corresponding source

These notices apply to the identified third-party components, independently of
HachimoDock's first-party terms. They are license/redistribution notices, not
claims that software vulnerabilities or runtime behavior have been verified.

## Waveshare ESP32-P4 BSP — Apache-2.0

Local scope: firmware/components/esp32_p4_wifi6_touch_lcd_4_3/.
The local idf_component.yml identifies Waveshare's BSP version 1.0.1. The
security scanner's esp-bsp/benchmark-latest fingerprint is not a resolved
upstream version or a license grant. This is a locally modified Waveshare board
adaptation using the ESP-BSP APIs, not an unmodified copy of the esp-bsp benchmark.

Publisher release and actual license:
- https://components.espressif.com/components/waveshare/esp32_p4_wifi6_touch_lcd_4_3/versions/1.0.1/license
- https://components.espressif.com/api/downloads/?object_id=9673bb0f-440a-40e9-a87a-afadf5cb4844&object_type=component
- Registry-linked source: https://github.com/waveshareteam/Waveshare-ESP32-components/tree/e9fabe5caadd850a815093376f07a1d6b4361ce8/bsp/esp32_p4_wifi6_touch_lcd_4_3

The published release contains the Apache License 2.0. Its LICENSE matches the
existing local component LICENSE byte-for-byte. A redistribution copy is included
as Espressif-Waveshare-BSP-LICENSE.txt, SHA256
cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30.
Publisher archive SHA256:
0b5d8eaaa366124e4c13ab36a64cf03210ccf7860f86b9515056b3e7fb3686d3.

The publisher archive contains no NOTICE file. Its corresponding display.h,
esp32_p4_wifi6_touch_lcd_4_3.h and .c have no per-file copyright header.
No copyright owner or year has been invented or removed here. The local files
now identify the component's existing Apache-2.0 license and explicitly mark
the local adaptation. Existing source comments and all executable/preprocessor
content are preserved. Local changes include the desk-pet display configuration,
MIPI startup handling and related board integration; this is not a claim of
byte-identical upstream source. Other drivers retain their own licenses.

## unescaper 0.1.8 — MIT alternative selected

HachimoDock elects the MIT alternative offered by this upstream version.
Cargo.toml.orig at its published VCS commit declares GPL-3.0/MIT, and the same
crate includes both LICENSE-GPL3 and LICENSE-MIT. The latter is copied unchanged
as unescaper-0.1.8-MIT-LICENSE.txt, including Copyright (c) 2023 Hack Ink.

- Versioned source: https://static.crates.io/crates/unescaper/unescaper-0.1.8.crate
- Source/archive SHA256: 4064ed685c487dbc25bd3f0e9548f2e34bab9d18cefc700f9ec2dba74ba1138e
- Manifest: https://github.com/hack-ink/unescaper/blob/cafde7ff710b227f18fb8f1209459d75133ffc00/Cargo.toml
- MIT grant: https://github.com/hack-ink/unescaper/blob/cafde7ff710b227f18fb8f1209459d75133ffc00/LICENSE-MIT
- MIT text SHA256: 6e0bad263cbd2aa6235d78e3c26bcfa2668cf32f6b274ae21ce0dd620114c7b3

All 17 files in the local published-crate source matched its locked archive.
This downstream selection does not relabel upstream metadata or remove the
upstream GPL option. serialport declares unescaper for Linux non-musl targets;
presence in Cargo.lock alone does not prove inclusion in a Mac/Windows binary.

## serialport 4.9.0 — MPL-2.0, corresponding source available

serialport is licensed under Mozilla Public License 2.0. Its original
LICENSE.txt notice is copied unchanged as serialport-4.9.0-LICENSE-NOTICE.txt.
The complete Mozilla-published license is included as MPL-2.0.txt.

Corresponding Source Code Form for the unchanged covered library is publicly
available without an account or fee:
- Exact published source: https://static.crates.io/crates/serialport/serialport-4.9.0.crate
- Archive SHA256: a4d91116f97173694f1642263b2ff837f80d933aa837e2314969f6728f661df3
- Source tree: https://github.com/serialport/serialport-rs/tree/2a62b225b2dd043fa982f91d0fb54c7e26fb221e
- Original notice: https://github.com/serialport/serialport-rs/blob/2a62b225b2dd043fa982f91d0fb54c7e26fb221e/LICENSE.txt
- Complete license: https://www.mozilla.org/media/MPL/2.0/index.txt

The locked archive was verified against the public download, and all 50
published files matched the local crate source without modification. The
covered library source remains available under MPL-2.0. HachimoDock's terms do
not restrict those source rights. No serialport source file is changed here.

MPL sections 3.1/3.2 require availability of the covered Source Code Form and
informing executable recipients how to obtain it. Section 3.3 permits a Larger
Work under other terms while preserving the MPL-covered code's rights; this
does not by itself require disclosure of unrelated first-party application
files. If covered files are modified later, the matching modified source must
be made available too; an unchanged-upstream link would not describe that change.
Official explanation: https://www.mozilla.org/en-US/MPL/2.0/FAQ/ (Q8, Q10, Q11).

Original notice SHA256:
00a4d69750686b8fcd2369b17176b4389d67f275fd22b9eab6c66155b72bd4d2.
Complete license SHA256:
3f3d9e0024b1921b067d6f7f88deb4a60cbe7a78e76c64e3f1d7fc3b779b9d04.

## Redistribution

Distribute this notice and the four accompanying license/notice texts together,
alongside the existing third-party licenses and MiSans license carriers.
The shared Tauri resources and Windows cross-bundler use an explicit ten-file
public-license collection under licenses/. Source URLs and license rights are
available to binary recipients. These changes prepare subsequent distributions
and do not retroactively change an already-built installer.

## Existing MiSans 16px/32px carriers

The unchanged official MiSans Font Intellectual Property License Agreement is
included as MiSans-Font-License-Agreement.pdf (79535 bytes), SHA256:
4a93a27cd2bd81b3b5ecfd0a853144a876fa26938a93a68443c67d74172fcb86.
MiSans-Embedded-Fonts-NOTICE.txt explicitly identifies the 16px/1bpp embedded
font and the 32px/2bpp 199-glyph embedded font, SHA256:
5511b1367b17f089d554b138c3a08e98e36ce5e5675dd049419492d65a282b11.
Both are required public resources, independently of the first-party license.
No original OTF/TTF font file is added. Official source and embedding FAQ:
https://hyperos.mi.com/font/zh/ and https://hyperos.mi.com/font/zh/faq/.
