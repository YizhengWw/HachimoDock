# Hardware Reproduction

[Back to README](../README.en.md)

This page collects HachimoDock hardware reproduction materials, including the complete-device BOM, enclosure and assembly notes, and PCB process information. Please verify models, dimensions, and supply status against the project files before purchasing or batch production.

Hardware design files are released under `CERN-OHL-S-2.0`. Modifications and redistribution based on the hardware design files should follow that license and the notes in [LICENSE.md](../LICENSE.md).

## Hardware BOM

![Hardware overview](../assets/image_12.png)

### Option 1: One-click LCSC Order

Use this when ordering the custom board and electronic materials together. If a one-click order entry is provided later, follow the OSHWHub project page or the latest repository notes.

### Option 2: Buy Parts Manually

Use the table below to check and purchase the complete-device materials item by item.

| Category | Module / Part | Position / Interface | Qty / Unit | Qty / 50 Units | Notes |
| --- | --- | --- | ---: | ---: | --- |
| Dev board | [Raspberry Pi Zero 2 WH][buy-pi] | Insert into U1 | 1 | 50 | Pre-soldered 40-pin header version recommended |
| SD card | [microSD card][buy-sd] | TF card slot | 1 | 50 | 32 GB or above, A1/A2 preferred |
| Display | [2.8-inch 240x320 SPI TFT touch screen][buy-screen] | Insert into U2 | 1 | 50 | 11-pin XPT2046 touch version used as reference; 8-pin non-touch version can be an alternative; choose soldered header and ILI9341 version |
| Knob | [Pushable knob module / EC11 encoder module][buy-knob] | Connect to H2 | 1 | 50 | Confirm shaft length, knob height, installation direction, and mounting method with the enclosure design |
| Microphone module | [INMP441 microphone module][buy-mic] | Connect to H1 | 1 | 50 | External module can be used if SMT microphone cost is too high |
| Speaker | [1224 small cavity speaker, 1.25 connector, adhesive backing][buy-speaker] | Connect to CN7 | 1 | 50 | 1224, 8 ohm, 1-2 W, 1.25P |
| Wires | [Dupont wires][buy-wire] | Buttons / knob / microphone | 15 | 750 | Around 20 cm, female-to-female |
| Screws | [Self-tapping screw M2 * 8 mm][buy-screw] | Screen mounting | 4 | 200 | Used to mount the screen |
| Screws | [Self-tapping screw M2 * 5 mm][buy-screw] | Structure mounting | 8 | 400 | Used to mount other structure parts |
| Enclosure | [3D printed / CNC enclosure][buy-shell] | Whole device structure | 1 | 50 | Enclosure, back cover, and internal brackets |
| Adapter cable | [Micro-USB male to Type-C female cable][buy-micro-usb] | Internal board extension | 1 | 50 | 10 cm; MicroUSB male up-angle to Type-C female straight `[mic2-tpc1]` |
| Adapter cable | [Type-C to Type-A cable][buy-typec-a] | Device to computer | 1 | 50 | USB 2.0 is enough; must be 4-wire or above, not charge-only |
| Custom PCB baseboard | See the project PCB BOM | Interconnects modules and reduces Dupont wiring | - | - | Verify baseboard materials with the PCB project |

> Purchase note: links are provided as reproduction references only. Prices, stock, and after-sales service follow the corresponding platform. Verify models and supply before batch production.

## Enclosure and Assembly

Enclosure model files are provided with the project materials when available. If they are mirrored to MakerWorld or another model platform later, follow the latest repository link.

![Assembly overview](../assets/1aa4e315-fe70-44ae-853e-b72996ee1aae.png)

![Assembly detail](../assets/image_15.jpeg)

## PCB Process Information

![PCB front/back](../assets/image_16.png)

![PCB render](../assets/image_17.png)

| Item | Value |
| --- | --- |
| Thickness | 1.6 mm |
| Layers | 2-layer board |
| Size | 71 mm * 30.5 mm |
| Soldering | Audio-related parts such as amplifier and microphone may need a hot plate. Other parts can be soldered with an iron. If voice interaction is not required, a bare board with manually soldered headers can be used without affecting the core product flow. |

[buy-pi]: https://item.taobao.com/item.htm?id=693613248231
[buy-sd]: https://detail.tmall.com/item.htm?id=848065818893
[buy-screen]: https://item.taobao.com/item.htm?id=526024381409
[buy-knob]: https://e.tb.cn/h.iForjxnIRX1llEz
[buy-mic]: https://e.tb.cn/h.izOC4n5sjGeIoAm
[buy-speaker]: https://e.tb.cn/h.ixBS9SgI6gFXrIx
[buy-wire]: https://so.szlcsc.com/global.html
[buy-screw]: https://item.taobao.com/item.htm?id=39761471376
[buy-shell]: https://www.jlc-3dp.cn/
[buy-micro-usb]: https://detail.tmall.com/item.htm?id=867489662609
[buy-typec-a]: https://item.taobao.com/item.htm?id=726410843702
