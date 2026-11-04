# Optional native 32px font

`PET_FONT_NATIVE_32` defaults to 0. This patch does not enable a profile, alter a version, change OTA geometry, or replace the existing 16px/21,207-glyph bitmap. Enabling requires both reviewed packed-bitmap and packed-metrics flags.

## Exact coverage and layout

Only the 199 original PR12 glyphs (including printable ASCII) receive native 32px/2bpp ink at integer scale 2. Other glyphs retain the existing 16px/21,207-glyph 1bpp fallback with its normal scaling. This is not a complete 2bpp body font. Of “城市电台组件天气设置”, only 电/组/件/天/气 are in the native set. Scale 1, medium 1.5x, other integer scales, and glyph_width layout remain on font16.

PR12 source: head `85e194d9d970513c6a475a33ce8cabe0c5c72603`, original `firmware/main/pet_p4_font_cn_32.c`. The source fixture and generated C retain the original MiSans license/source notice. No source OTF, rasterization, glyph removal, bpp reduction or metric edits are introduced. `tools/compress_font_native32.py --check` pins source and bitmap SHA256 and verifies lossless identity assets.

32px line-height/baseline are 36/6; font16 is 18/3. Both yield a scale2 baseline of 30. Raw advances double exactly, but LVGL-rounded advances differ for 52/199 glyphs; glyph_width therefore deliberately stays on font16. Native ink boxes differ from nearest-neighbor 16px for 179/199 glyphs; native tops shift -3..+2 pixels. Font selection and baseline/box geometry are resolved before glyph draw-data release. Width compatibility does not imply identical ink extents.

## Startup and memory

Startup is latched and runs before tasks/render: reconstruct metric columns into the existing metrics array using the first 169,664 bytes of the existing bitmap buffer; rebuild kerning; inflate the unchanged bitmap; inflate native32. All stages reuse the same inflater, with no new scratch BSS or malloc. Any failure clears original bitmap/metrics/kerning and native32, and cannot retry. Native32 adds a 30,579-byte PSRAM bitmap plus two latch bytes and alignment.

Original bitmap stays 463,438 packed bytes. Original metrics stay on disk unchanged (57,581 bytes); gated columns are 27,793 bytes, saving 29,788. Native32 identity stream is 14,768 bytes; original descriptor/cmap arrays remain lossless and uncompressed. The .deflate file is a reproducibility artifact: its bytes enter firmware once through the gated C initializer, not through a second CMake binary embedding.

CMake intentionally embeds old and column metric inputs. Final V1 AND V3 linked ELF/map must prove old metrics absent when native32 is enabled, and columns/native32 absent when disabled. Isolated RISC-V section-GC verification is only supporting evidence: its stubbed ROM/font callbacks do not replace final firmware budget checks. Both OTA images must still satisfy actual slot/headroom budgets before any profile is enabled.

## Validation

`tests/p4_font_native32_test.py` compares all original pixels, 200 descriptors and 199 Unicode entries; exercises the actual parent startup via a host-zlib ROM adapter with ASan/UBSan, all three stage failures/length/checksum faults, full-buffer clearing, reentry/retry latches, exhaustive BMP lookup, and actual extracted draw/bounds/width functions. The release mock invalidates its descriptor. Existing packed-font, glyph, raster, renderer trace and renderer policy tests are retained.

For a real SDK object check, expand the PIO profile macros in addition to CCDB arguments, e.g. the existing `p4_storage_zero_bss_test._profile_cpp` for the target profile. CCDB alone can omit packed-font flags and correctly trigger the gate error. No claim of real P4 ROM timing, final linked budgets, visual or hardware acceptance is made by host tests.

## 2026-11-03 完整预览镜像结果

`0.7.103-p4-r165-font-preview` 的两种目标均已实际开启该功能构建：

| 目标 | 应用镜像 | 旧 2,621,440 B 槽余量 | 比 R164 缩小 |
|---|---:|---:|---:|
| V1 | 2,454,320 B | 167,120 B | 12,224 B |
| V3 | 2,469,408 B | 152,032 B | 12,096 B |

两种目标均保留原 131,072 B 最低余量门槛，通过内部静态 RAM 检查，已审阅渲染栈最大为 3,568/4,096 B。两份完整 ELF 均确认旧度量表 payload 不存活，新位图位于外部 BSS，列编码资源大小为 27,793 B。PlatformIO 的基础 `board_build.embed_files` 同时登记了列编码资源。

该结果属于离线候选验收。默认开关仍为 0；未刷机、未运行静默 HIL，实机字体效果及帧耗时尚未验收。
