# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — the decommissioned supermarket price tags built around a Dialog/Renesas
DA14585 BLE SoC driving an SSD1680-family e-paper panel.

These tags are sold cheaply as surplus, but the factory firmware refuses control
from anything but the vendor's own tooling. This repository contains firmware
written from scratch against Renesas's official DA1458x SDK6 that takes the tag
over completely, plus the reverse-engineering notes that made it possible.

**Status: working on real hardware.** The firmware builds, runs from SysRAM, and
drives the panel — verified by rendering a test pattern on a physical tag.

<!-- Add a photo of the running tag here. -->

## What's here

| Path | |
| --- | --- |
| [`firmware/hema_epd_clock/`](firmware/hema_epd_clock/) | The firmware: SSD1680 driver, 1bpp framebuffer + drawing primitives, ASCII drawing-command parser, BLE peripheral with two custom GATT services. See its [README](firmware/hema_epd_clock/README.md). |
| [`firmware/hema_epd_clock/BUILD_AND_FLASH.md`](firmware/hema_epd_clock/BUILD_AND_FLASH.md) | Toolchain setup, SWD wiring, and how to load into RAM over J-Link without touching the stock firmware. |
| [`firmware/hema_epd_clock/GPIO_TRACING_GUIDE.md`](firmware/hema_epd_clock/GPIO_TRACING_GUIDE.md) | How the EPD pin map was continuity-traced from the FPC connector to the DA14585 package. |
| [`PROTOCOL_NOTES.md`](PROTOCOL_NOTES.md) | The reverse-engineering write-up: firmware image format, panel identification, the vendor's BLE GATT services and UUIDs, and the full ASCII drawing DSL. |

## Hardware

- **SoC:** DA14585 (Cortex-M0, no internal flash — executes from SysRAM, boots
  from an external SPI flash via the ROM bootloader)
- **Panel:** 2.13", SSD1680-family controller. Two board variants exist:
  122×250 (`HINK-E0213A53-FPC-A0`, the default here) and 104×212.
- **Debug:** SWD on package pins 25 (SWDIO) and 26 (SW_CLK)

## Quick start

Loading over SWD writes **only to RAM**, so the original firmware stays intact
and a power cycle restores the tag. That makes experimentation cheap — start
there before considering burning anything to the SPI flash.

```sh
# 1. Get SDK6 from Renesas (account required) and unpack it.
# 2. Generate the e² studio project:
DA1458X_SDK=~/DA145xx_SDK/6.0.22.1401 \
    python3 firmware/hema_epd_clock/tools/gen_e2studio_project.py

# 3. Copy the project into the SDK tree, build in e² studio, then:
JLinkExe -device Cortex-M0 -if SWD -speed 4000 -autoconnect 1 \
         -CommanderScript firmware/hema_epd_clock/tools/ram_load.jlink
```

Full detail, including the non-obvious failure modes, in
[`BUILD_AND_FLASH.md`](firmware/hema_epd_clock/BUILD_AND_FLASH.md).

## Not implemented yet

The drawing DSL is only partly covered (`CLEAR`, `RECT`, `LINE`, `CIRCLE`,
`POINT`, `FONT`). Still open: the remaining DSL commands, `{}` template
variables, the vendor's font/icon glyph formats, RTC timekeeping, and SUOTA OTA
updates. See the firmware README for the full list.

## Licensing and third-party material

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions, tooling, and documentation — is GPL-3.0, per
[`LICENSE`](LICENSE).

Some files under `firmware/hema_epd_clock/src/config/` and
`src/platform/user_periph_setup.c` derive from Renesas SDK6 example templates
and **retain their original Renesas copyright notices**; they are not covered by
the GPL and are included only as the minimal project scaffolding needed to build.
The SDK itself is not redistributable and is not vendored here.

Nothing is derived from or linked against the community firmware binary or the
vendor's web tool. Those were used strictly as references for protocol and
display-controller command sequences, as documented in
[`PROTOCOL_NOTES.md`](PROTOCOL_NOTES.md).

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an
embedded device can brick it — back up the original SPI flash contents before
writing anything permanent.
