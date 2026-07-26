# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — the decommissioned supermarket price tags built around a Dialog/Renesas
DA14585 BLE SoC driving an SSD1680-family e-paper panel.

These tags are sold cheaply as surplus, but the factory firmware refuses control
from anything but the vendor's own tooling. This repository contains firmware
written from scratch against Renesas's official DA1458x SDK6 that takes the tag
over completely, plus a browser control panel for driving it.

**Status: phase one complete.** The firmware is programmed into the tag's SPI
flash and boots and runs standalone — no debugger attached. It keeps time, draws
a configurable clock face, remembers it across power cuts, and accepts new faces
or arbitrary images over Bluetooth.

## Hardware

- **SoC:** DA14585 (Cortex-M0, no internal flash — executes from SysRAM)
- **Panel:** 2.13", SSD1680-family controller. Two board variants exist:
  122×250 (`HINK-E0213A53-FPC-A0`, the default here) and 104×212.
- **Debug:** SWD on package pins 25 (SWDIO) and 26 (SW_CLK)

## What the firmware does

- Drives the panel directly — framebuffer, lines, rects, circles, a 5×7 font,
  and four screen rotations.
- Keeps a software clock. The DA14585 has no RTC, so a 1 Hz timer counts from a
  2000-01-01 epoch and the host sets it on connect; it resets on power loss.
- Stores the clock face in SPI flash, so it survives a power cut.
- Exposes two BLE services: one for the template, one for a raw image.

## Templates

A face is a short script — `CLEAR`, `LINE`, `RECT`, `CIRCLE`, `FONT`, `ROTATE` —
stored on the tag and re-run every minute. That is what makes it a clock rather
than a picture. `{}` variables expand to the date and time, and they work in
**numeric arguments** as well as in text: arguments are integer expressions with
`+ - * / %`, parentheses and unary minus. So a face can draw itself rather than
only label itself:

```
RECT(4,70,245,82,0,1,0)             a fixed frame
RECT(4,70,4+{d}*241/{L},82,0,1,1)   filled to how far into the month we are
```

Nothing throws — a malformed expression, an unknown variable and division by
zero all evaluate to 0. A shelf label has nowhere to report an error to, so it
should degrade to a wrong-looking face rather than a hung one. The preview
behaves identically, then flags it.

## Web UI

[`webui/`](webui/) edits the face, previews it live, and pushes it over
Bluetooth. No build step, no dependencies.

```sh
python3 -m http.server -d webui 8000   # same machine: http://localhost:8000
python3 webui/serve.py                 # phone/LAN:    https://<your-ip>:8443
```

Web Bluetooth needs a **secure context** — `https://` or the `localhost` special
case, nothing else — so plain `http.server` works on the machine running it and
not from any other device. `serve.py` covers that with a self-signed
certificate. A Chromium-based browser is also required; Safari and Firefox do
not implement Web Bluetooth, which on iOS means no browser can. The page says
which of these is the problem rather than failing blankly.

The **Image** tab takes any picture — dropped, picked or pasted — and reduces it
to one bit per pixel with Floyd–Steinberg, Atkinson, ordered Bayer or a plain
threshold. Two firmware behaviours worth knowing: an image replaces the clock
until a template is pushed again (they share one framebuffer, so whoever wrote
last wins), and an image is not kept in flash, so a power cut brings back the
clock.

## Tests

```sh
node --test webui/test.mjs                     # web UI and JS renderer
make -C firmware/hema_epd_clock/test           # firmware modules, on the host
make -C firmware/hema_epd_clock/test render    # + cross-language parity
```

The preview is a line-by-line port of the firmware's renderer, so it is tested
as one: build `render` and the JS suite gains a test that runs every preset
through the **actual** `epd_cmdparser.c` and `epd_gfx.c` compiled natively and
diffs the framebuffers byte-for-byte. It is also the tie-breaker when the panel
disagrees with the preview — firmware, JS port and SWD test rig are three
suspects, and comparing any two cannot say which is wrong.

The firmware tests need no SDK, no cross toolchain and no tag. They exist mainly
for the calendar arithmetic: ISO 8601 week numbering is wrong only around New
Year and only in some years, so it is pinned to GNU `date`'s `%V/%G/%j`.

## Documentation

Development is ongoing, so the write-up is deliberately **not published yet** —
a half-accurate hardware guide is worse than none. The reverse-engineering
notes, build/flash guide and GPIO tracing guide are kept locally and will be
rewritten and pushed once the firmware is feature-complete.

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
