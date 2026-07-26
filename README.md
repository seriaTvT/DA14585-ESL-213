# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — the decommissioned supermarket price tags built around a Dialog/Renesas
DA14585 BLE SoC driving an SSD1680-family e-paper panel.

These tags are sold cheaply as surplus, but the factory firmware refuses control
from anything but the vendor's own tooling. This repository contains firmware
written from scratch against Renesas's official DA1458x SDK6 that takes the tag
over completely.

**Status: running standalone on real hardware.** The firmware builds, is
programmed into the tag's SPI flash, and boots and drives the panel on its own
after a power cycle — no debugger attached.

## Hardware

- **SoC:** DA14585 (Cortex-M0, no internal flash — executes from SysRAM)
- **Panel:** 2.13", SSD1680-family controller. Two board variants exist:
  122×250 (`HINK-E0213A53-FPC-A0`, the default here) and 104×212.
- **Debug:** SWD on package pins 25 (SWDIO) and 26 (SW_CLK)

## Web UI

[`webui/`](webui/) is a browser front-end for a flashed tag: it edits the clock
face, previews it, and pushes it over Bluetooth. No build step, no dependencies.

```sh
python3 -m http.server -d webui 8000   # same machine: http://localhost:8000
python3 webui/serve.py                 # phone/LAN:    https://<your-ip>:8443
```

Web Bluetooth is gated behind a **secure context**, which means `https://` or
the `localhost` special case — nothing else. Plain `http.server` is therefore
fine on the machine running it and useless from any other device: the page
loads and `navigator.bluetooth` is simply undefined. `serve.py` serves the same
directory over TLS with a self-signed certificate to satisfy that rule; the
first visit from each device shows a certificate warning to click through.

A Chromium-based browser is also required — Safari and Firefox do not implement
Web Bluetooth at all, which on iOS means no browser can. On Linux the browser
additionally needs to reach BlueZ over D-Bus. The page says which of these is
the problem instead of failing blankly.

The preview is a direct port of the firmware's own renderer — same Bresenham,
same rotation transform, same 5×7 glyph table — so what it draws is what the
panel will show. `node --test webui/test.mjs` guards that parity, including a
byte-for-byte diff of the built-in face against `DEFAULT_FACE[]` in the C source.

## Documentation

Development is still in progress, so the write-up is deliberately **not
published yet** — a half-accurate hardware guide is worse than none. The
reverse-engineering notes, build/flash guide and GPIO tracing guide are kept
locally and will be rewritten and pushed once the firmware is feature-complete.

## Licensing and third-party material

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions and tooling — is GPL-3.0, per [`LICENSE`](LICENSE).

Some files under `firmware/hema_epd_clock/src/config/` and
`src/platform/user_periph_setup.c` derive from Renesas SDK6 example templates
and **retain their original Renesas copyright notices**; they are not covered by
the GPL and are included only as the minimal project scaffolding needed to build.
The SDK itself is not redistributable and is not vendored here.

Nothing is derived from or linked against the community firmware binary or the
vendor's web tool. Those were used strictly as references for protocol and
display-controller command sequences.

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an
embedded device can brick it — back up the original SPI flash contents before
writing anything permanent.
