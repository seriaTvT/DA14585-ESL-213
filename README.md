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

## Building and flashing

No IDE required — the e² studio project is only a wrapper around a makefile.

**1. Get the SDK.** Download DA145xx SDK6 (6.0.22.1401) from Renesas; it is not
redistributable, so it is not vendored here. Then generate the project, which
copies this repo's sources into the SDK tree where the build expects them:

```sh
export DA1458X_SDK=~/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401
python3 tools/gen_e2studio_project.py
```

**2. Build.** The makefile lives under the generated project:

```sh
make -C "$DA1458X_SDK/projects/target_apps/template/hema_epd_clock/e2studio/DA14585" all -j4
```

Note the build compiles from **inside the SDK tree**, not from this repo. Edit
here, re-run the generator (or copy the changed file across), then build —
otherwise you get a clean, successful build of unchanged code.

Output is `hema_epd_clock.bin` — a raw linker image, vector table first.

**3. Flash.** A raw `.bin` at flash offset 0 will **not** boot this tag. Its
secondary bootloader is in OTP, ignores offset 0, and instead reads a product
header to find two SUOTA image banks, picks the newest valid one and copies it
to SysRAM. So the image has to be wrapped in a bank:

```sh
tools/flash.sh <stock_dump.bin> <path/to>/hema_epd_clock.bin
```

That wraps the build into bank 1 (`tools/mksuota.py`), leaves the stock image in
bank 2 as a fallback, and programs the lot with J-Link Commander. Then
**power-cycle the tag** — an SWD reset does not re-run the bootloader's bank
scan, so the previous image keeps running until the power actually drops.

Flashing needs the community J-Link device definition that exposes the DA14585's
QSPI bank (`JLinkDevices.xml` plus Dialog's `jtag_programmer.axf`, installed into
`/opt/SEGGER/JLink`). Without it `device DA14585` has no flash bank at all.

**Iterating without flashing.** For a quick edit/test loop, load straight into
SysRAM instead — non-destructive, and it reverts on the next power cut:

```sh
# edit the loadfile path inside the script first
JLinkExe -device Cortex-M0 -if SWD -speed 4000 -autoconnect 1 \
         -CommanderScript tools/ram_load.jlink
```

Order matters there and is counter-intuitive: `loadfile` performs an implicit
reset, and any reset clears the address-0 remap, so the remap writes have to
come **after** the download. Cortex-M0 has no VTOR, so the vector table must
physically live at address 0 — without the remap every interrupt vectors into
ROM and the BLE stack never runs.

> Anything loaded into RAM is gone on the next power cut. If new firmware seems
> to be ignored — variables rendering as literal `{M}` text, say — the tag is
> almost certainly booting the older image still in its flash.

## Templates

A face is a short script — `CLEAR`, `LINE`, `RECT`, `CIRCLE`, `FONT`, `ROTATE` —
stored on the tag and re-run every minute. That is what makes it a clock rather
than a picture. `{}` variables expand to the date and time, and they work in
**numeric arguments** as well as in text: arguments are integer expressions with
`+ - * / %`, parentheses and unary minus. So a face can draw itself rather than
only label itself:

```
RECT(4,70,245,82,0,1,0)             a fixed frame
RECT(4,70,4+{d}*241/{D},82,0,1,1)   filled to how far into the month we are
```

Lower case is a position, upper case the length it runs against — `{d}` of `{D}`
days this month, `{j}` of `{J}` days this year — so a progress bar retargets
from month to year by changing the case of two letters.

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
