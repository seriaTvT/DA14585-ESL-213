# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — the decommissioned supermarket price tags built around a Dialog/Renesas
DA14585 BLE SoC driving an SSD1680-family e-paper panel.

These tags are sold cheaply as surplus, but the factory firmware refuses control
from anything but the vendor's own tooling. This repository contains firmware
written from scratch against Renesas's official DA1458x SDK6 that takes the tag
over completely, plus a browser control panel for driving it.

**Status: the language is its own.** The firmware is programmed into the tag's
SPI flash and boots and runs standalone — no debugger attached. It keeps time,
draws a configurable face, remembers it across power cuts, and accepts new faces
or arbitrary images over Bluetooth.

## Hardware

- **SoC:** DA14585 (Cortex-M0, no internal flash — executes from SysRAM)
- **Panel:** 2.13", SSD1680-family controller. Two board variants exist:
  122×250 (`HINK-E0213A53-FPC-A0`, the default here) and 104×212.
- **Debug:** SWD on package pins 25 (SWDIO) and 26 (SW_CLK)

## What the firmware does

- Drives the panel directly — framebuffer, lines, rects, circles, pixel
  inversion, two fonts, and four screen rotations.
- Runs a small drawing language with integer expressions and date/time
  variables, so a face can draw itself rather than only label itself.
- Keeps a software clock. The DA14585 has no RTC, so a 1 Hz timer counts from a
  2000-01-01 epoch and the host sets it on connect; it resets on power loss.
- Repaints on the face's own schedule — every minute for a clock, once a day
  for a calendar. A full panel refresh is the most expensive thing it does.
- Stores the face in SPI flash, so it survives a power cut, and versions it so
  a face written against an older language falls back rather than misdraws.
- Reports what it made of a script, so a typo is visible without a debugger.
- Exposes two BLE services: one for the face, one for a raw image.

## Licensing

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions and tooling — is GPL-3.0, per [`LICENSE`](LICENSE).

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an
embedded device can brick it — back up the original SPI flash contents before
writing anything permanent.
