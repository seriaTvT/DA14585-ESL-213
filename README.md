# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — decommissioned supermarket price tags built around a Dialog/Renesas
DA14585 BLE SoC driving an SSD1680-family e-paper panel — plus a browser control
panel for driving it.

These tags sell cheaply as surplus, but the factory firmware answers only to the
vendor's own tooling. This firmware is written from scratch against Renesas's
DA1458x SDK6 and takes the tag over completely. It is programmed into the tag's
SPI flash and boots standalone with no debugger attached: it keeps time, draws a
face you define, remembers that face across power cuts, and accepts a new one —
or an arbitrary image — over Bluetooth.

![Six faces rendered by the firmware's own renderer: a clock, a month grid, a
thermometer, a calendar, a month-progress bar and a framed
card](docs/img/faces.png)

Those are the shipped presets, rendered by the firmware's own C renderer on the
host — not mockups. Each is a short script the tag stores and re-runs on a timer,
which is what makes it a clock rather than a picture.

---

## Contents

- [What it does](#what-it-does)
- [Does this fit my tag?](#does-this-fit-my-tag)
- [Getting started](#getting-started)
- [The drawing language](#the-drawing-language)
- [The BLE interface](#the-ble-interface)
- [Repository layout](#repository-layout)
- [Known gaps](#known-gaps)
- [Licensing](#licensing)

---

## What it does

- **Drives the panel directly** — 1bpp framebuffer, points, lines, rects,
  circles, pixel inversion, two fonts, four screen rotations.
- **Runs a small drawing language.** Numeric arguments are integer expressions
  and date/time variables work inside them, so a face can *draw* itself rather
  than only label itself — a progress bar across the month, a hand that tracks
  the hour.
- **Keeps a software clock.** The DA14585 has no RTC, so a 1 Hz timer counts from
  a 2000-01-01 epoch and the host sets it on connect. It does not survive a power
  cut.
- **Repaints on the face's own schedule** — every minute for a clock, once a day
  for a calendar.
- **Refreshes only the rows that changed** (`--partial`): no flash on a minute
  tick, and a fraction of the drive. See [partial refresh](#partial-refresh).
- **Stores the face in SPI flash**, versioned, so a face written against an older
  language falls back to the built-in default rather than misdrawing.
- **Reports what it made of a script** over a status characteristic, so a typo is
  visible without a debugger.
- **Reads the panel's own temperature sensor** and renders it as `{T}`.

The web UI previews a face pixel-for-pixel before you push it, dithers and uploads
arbitrary images, and sets the clock.

---

## Does this fit my tag?

Four physically distinct tags have been handled. They vary along **three
independent axes** — board wiring, panel model, and which waveform the panel's
controller runs — and those axes do not move together.

| Type | Board | Panel | Panel bus | Status |
|---|---|---|---|---|
| **1** | variant B | A53, 122×250 | hardware SPI, shared with the boot flash | driven |
| **2** | variant A | A53, 122×250 | bit-banged, separate from the flash | **untested — see below** |
| **3** | variant A | A41, 104×212 | bit-banged, separate from the flash | driven |
| **4** | variant B | A41, 104×212 | hardware SPI, shared with the boot flash | driven |

The type number is the only thing you ever type. It selects the wiring and the
geometry (the table is in
[`src/config/tag_types.h`](firmware/hema_epd_clock/src/config/tag_types.h)) and is
stamped into the built image, so the flasher can refuse a mismatch before writing.

**Type 2 is untested because of a suspected dead panel, not because the build is
wrong.** On the single Type 2 unit here the panel rail sits steady at 3.3 V, BUSY
continuity is good, the pin configuration reads back correct and a properly
rendered framebuffer reaches the tag — and the controller answers on no line at
all. Everything measurable about the board is healthy, so the panel itself looks
damaged. The pairing is the right build on paper and nobody has yet seen it work.
If you have one, RST continuity from FPC pad 10 to `P1_0` was never measured here;
check that first, because an open reset line produces exactly this picture with a
healthy panel behind it.

> **Getting the type wrong is silent in the worst way.** The tag boots, advertises
> and takes connections exactly as normal, and only the panel stays dead — so it
> presents as a broken screen rather than a wrong image. That has cost a working
> tag twice. Establish the type before you flash.

### Identifying it

**Read the panel label off the flex.** `A53` → 122×250. `A41` or `A07` → 104×212.

![The four boards side by side. Types 2 and 3 carry an Alibaba Group silkscreen
and labelled RST/GND/URX/UTX/VBAT and SWDIO/SWCLK pads; Types 1 and 4 have an
unlabelled TP1–TP8 row instead](docs/img/boards.jpg)

At a glance the variant-A boards carry an **Alibaba Group** silkscreen and bring
SWD out on pads labelled `SWDIO`/`SWCLK`, while variant-B has an unlabelled
`TP1`–`TP8` row in roughly the same place. A first glance, not a verdict — confirm
by continuity-testing the panel FPC back to the DA14585:

| Panel signal | variant A (bit-banged) | variant B (hardware SPI) |
|---|---|---|
| SCK | `P0_1` | `P0_0` |
| SDA / MOSI | `P2_0` (bidirectional) | `P0_6` |
| D/C | `P0_7` | `P0_5` |
| RST | `P1_0` | `P0_7` |
| BUSY | `P1_1` | `P2_0` |
| CS | `P2_1` | `P2_1` |
| Panel power | `P2_3` | `P2_3` |
| Aux enable | `P2_2` | — |

On variant B the panel shares CLK/MOSI with the boot flash and `P0_5` is both the
panel's D/C *and* the flash's MISO, which is why the driver claims and releases the
bus. Variant A's panel pins are disjoint from the flash's.

**Do not** identify the variant by sampling GPIO modes at boot. It reads as variant
B on a variant-A board every time: the bit-banged pins are outputs only during a
transfer, e-paper is bistable so a tag need not refresh at boot, and the pins
driven early belong to both maps.

### The waveform, and the LUT's shape

Two init sequences are carried:

- **Waveshare** — a hand-written LUT via cmd `0x32`, from Waveshare's
  `EPD_2IN13_V2` reference. Fixed, temperature-independent, roughly **2.5×
  quicker**, and the only one that can do partial refresh.
- **OTP** — the panel's own waveform, loaded by the controller out of its OTP and
  temperature-compensated. Slower, and drives everything.

The hand-written table has to match the number of waveform **steps** the bonded
controller runs, and that varies by panel lot: 7 for most, **10** for some A41s.
A table of the wrong shape puts its timing groups where the controller reads
voltages, so every phase runs zero frames and the matrix sits completely inert
while the border electrode still flickers — which reads as a broken screen rather
than a wrong build.

```sh
tools/build.sh --type 4                    # Waveshare, 7 steps (the default)
tools/build.sh --type 4 --lut-steps 10     # Waveshare, 10 steps
tools/build.sh --type 4 --otp              # the panel's own waveform
tools/build.sh --all                       # every type, every variant
```

**Flash the default and look at the glass.** If the matrix never moved, try
`--lut-steps 10`, then `--otp`. Images stamp `HEMA-WAVEFORM-OTP` or `-WAVESHARE`
and `flash.sh` prints which is going on; write the panel's FPC lot code down
beside the result, because it is the only thing that has ever predicted this.

There is no panel here that *requires* OTP — only panels whose step count had not
been measured. OTP remains the universal fallback and the only
temperature-compensated option.

### Partial refresh

`--partial` repaints only the rows that changed. A minute tick stops flashing and
takes a fraction of the drive; a pushed face or uploaded image always paints
fully, and a full refresh is forced every 8 partials or every hour to sweep out
residue.

Waveshare lots only, at either step count — these panels hold no partial waveform
in OTP, so `build.sh` refuses `--otp --partial`. Faint ghosting between full
refreshes is inherent to a partial waveform, not a defect.

---

## Getting started

### The editor first — no hardware, no build

```sh
python3 webui/serve.py            # https on port 8443
```

Then open the printed URL. Chrome, Edge or Opera, desktop or Android; Safari and
Firefox do not implement Web Bluetooth, which on iOS means no browser works. It
serves over TLS because Web Bluetooth requires a secure context — `python3 -m
http.server` loads the page and leaves `navigator.bluetooth` undefined with no
hint why. One certificate click-through per device.

### What flashing needs

| Requirement | Notes |
|---|---|
| **A J-Link probe** | Genuine, a Renesas board's on-board J-Link, or an OB clone. Four wires: SWDIO, SWCLK, GND, VTref to the 3.3 V rail. |
| **Power on the tag** | Battery in, or bench 3.3 V on the battery rail. SWD does not power the board. |
| **A soldering iron** | SWD is on test pads. Variant A silkscreens them; variant B brings out `TP1`–`TP8` unlabelled, so beep them back to DA14585 pins 25 (SWDIO) and 26 (SWCLK). |
| **The community J-Link device definition** | `JLinkDevices.xml` plus Dialog's `jtag_programmer.axf` in `/opt/SEGGER/JLink`. Without it `device DA14585` exposes no flash bank and `loadbin` has nowhere to write. |
| **Renesas SDK 6.0.22.1401** | Account-gated; download it yourself. Not vendored here. |
| **A dump of your tag's flash** | Non-negotiable — see below. |

### 1. Dump the flash first

This is both your way back and a build input: `mksuota.py` reads the image bank
offsets out of your dump and copies the stock bank header. The offsets differ
between Type 1 and the rest, so another tag's dump is not a substitute.

```sh
JFlashExe -openprj<proj> -connect \
          -readrange0x04000000,0x0407FFFF -saveas stock_flash_512k.bin -exit
```

The `-readrange` separator is a **comma**; a dash fails *after* connecting with a
message that reads like a target fault. Read twice and compare — two identical
passes is the cheapest proof of a good dump.

### 2. Set up the SDK and build

The SDK ships a linker-script defect that hard-faults this firmware inside
`Reset_Handler` before `main()`: the startup tables carry byte counts where
CMSIS's `__cmsis_start()` expects word counts, so startup zeroes four times what
it should and walks off the top of SysRAM. It stays invisible until `.bss` grows
past the overshoot — which this firmware's 3 KiB script buffer does. The project
generator owns that patch; run it against your SDK.

SDK6 projects reference the SDK by relative path, so the project must live inside
the SDK tree at the same depth as the examples:

```sh
cp -r firmware/hema_epd_clock "$SDK/projects/target_apps/template/hema_epd_clock"
```

Import `…/hema_epd_clock/e2studio` into e² studio, select the **DA14585**
configuration and build once — that first build generates the makefiles. After
that e² studio is not needed again; it only supplied the compiler (LLVM Embedded
Toolchain for Arm) and those makefiles.

```sh
export LOCAL_PROJ="$SDK/projects/target_apps/template/hema_epd_clock"
tools/build.sh --type 3
tools/build.sh --all          # after any driver change - see below
```

`build.sh` mirrors the repo's sources into that tree on every run, so from here on
you edit in the repo and never touch the copy. The type is passed as
`-DHEMA_TAG_TYPE=n`, so switching types touches nothing git tracks, and the
finished binary's own stamp is **checked against what you asked for** before the
script reports success — every way this can go wrong yields a working image for
the wrong tag rather than an error.

Use `--all` after changing the driver. A mixed-age `out/` is a real trap: images
are correctly named for their tag type and give no hint of their age, so a stale
one reproduces a bug you already fixed. `--all` builds the whole set of one
vintage. `tools/build.sh -h` lists the bench options.

### 3. Flash

```sh
tools/flash.sh --type 3 stock_flash_512k.bin out/hema_epd_clock-type3.bin
```

Or drop the dump argument and keep dumps at
`$HEMA_STOCK_DIR/type<n>/stock_flash_512k.bin`.

A secondary bootloader reads a product header at `0x038000` to find two image
banks and boots the newest valid one. This writes **bank 1** and leaves the stock
image in bank 2, so a bad build falls back to something that works rather than
bricking the tag — and it is why a raw `.bin` at offset 0 does not boot on this
board. `mksuota.py` builds the bank image and blanks the template store, so the
tag returns to the built-in default face; `mkbootimg.py` is the other format
(AN-B-001) and not what you want.

`flash.sh` cross-checks the type, wiring and geometry stamped in the binary and
refuses on any mismatch. It treats J-Link's error lines as fatal, because
`JLinkExe` exits 0 even when it never reached the probe.

Then **power-cycle the tag** — an SWD reset does not re-run the bootloader's bank
scan, so the old image keeps running until the power actually drops.

If a flash dies verifying RAMCode and the `Write:`/`Read:` lines differ by a bit
or two, that is the SWD link: reseat and retry with `--speed 1000`. If they differ
wholesale, power-cycle and flash as the *first* J-Link operation.

### 4. Drive it

The tag comes up advertising as **`HemaEPD-Clock`**, showing the built-in clock
face and reading `00:00` on 2000-01-01 until a host syncs it. Open the web UI and
**Connect**.

---

## The drawing language

A face is a short ASCII script, one command per line, stored on the tag and re-run
on a timer so the `{}` variables re-expand and the picture keeps up with the clock.

```
ROTATE(270)
CLEAR(1)
TEXT(125,18,'{H:02d}:{N:02d}',font=1,scale=2,align=1)
TEXT(125,78,'{y}-{m:02d}-{d:02d}',scale=2,align=1)
TEXT(125,100,'{W}',scale=2,align=1)
```

That is the built-in default face verbatim — written in the DSL rather than drawn
in C, so it takes exactly the same path as anything you push.

### Commands

Required geometry is positional; everything else is named and optional, which lets
an option be added later without disturbing a face already stored on a tag.

| Command | Options (with defaults) |
|---|---|
| `CLEAR(color)` | — |
| `POINT(x, y)` | `color=0` |
| `LINE(x1, y1, x2, y2)` | `color=0`, `width=1` |
| `RECT(x1, y1, x2, y2)` | `color=0`, `width=1`, `fill=0` |
| `CIRCLE(x, y, r)` | `color=0`, `width=1`, `fill=0` |
| `INVERT(x, y, w, h)` | — |
| `TEXT(x, y, 'string')` | `color=0`, `bg=1`, `scale=1`, `align=0`, `font=0` |
| `ROTATE(0\|90\|180\|270)` | — |
| `EVERY(minutes)` | — |
| `TIME(seconds)` | — |
| `RESET()` | — |

- **`color`** is `0` black, `1` white.
- **`INVERT`** takes width and height, not a second corner, and flips every pixel
  in the box — how you highlight a calendar cell without knowing what is under it.
  Draw it *last*: the 5×7 font paints its whole glyph cell, so a number drawn
  afterwards blanks the part of the box it covers.
- **`TEXT`** is opaque, because `bg` fills the glyph cell; white-on-black is
  `color=1,bg=0`. `align=` moves the anchor — `0` left, `1` centre, `2` right — so
  centring is `align=1` at `x = width/2`.
- **`font=0`** is a 5×7 general font (digits, uppercase, clock punctuation),
  scaling in whole pixels. **`font=1`** is 16×24, digits and `:` only, for big
  time. A character `font=1` lacks draws blank rather than falling back.
- **`ROTATE`** takes degrees only; `90` and `270` are landscape. `ROTATE(3)` — the
  vendor's index form — is reported as an error rather than taken as 3 degrees.
- **`EVERY(n)`** sets minutes between repaints, 1 to 1440, and travels with the
  face that wants it. Boundaries are absolute, so `EVERY(1440)` lands at midnight
  rather than wherever the tag booted.
- **`TIME(seconds since 2000-01-01)`** and **`RESET()`** are applied on arrival and
  never stored, so a `TIME()`-only sync leaves the face alone.

### Variables

`{}` references expand inside strings *and* inside numeric arguments. Format with
`{name:0Nd}` — `{H:02d}` zero-padded, `{H:2d}` space-padded.

| Date | | Time and more | |
|---|---|---|---|
| `{y}` | full year | `{H}` | hour, 0–23 |
| `{m}` | month, 1–12 | `{h}` | hour, 1–12 |
| `{d}` | day of month | `{N}` | minute |
| `{D}` | days in this month | `{S}` | second |
| `{j}` | day of year | `{w}` | weekday, 0 = Sunday |
| `{J}` | days in this year | `{u}` | seconds since 2000-01-01 |
| `{V}` | ISO 8601 week number | `{T}` | panel temperature, °C |
| `{G}` | ISO week-numbering year | | |

Four are text and work only inside a string: `{W}` (`SUN`…`SAT`), `{M}`
(`JAN`…`DEC`), `{P}` (`AM`/`PM`) and `{VER}` (`HEMA1`).

Lower case is a position, upper case the length it runs against — `{d}`/`{D}`
within the month, `{j}`/`{J}` within the year — which makes a progress bar one
line.

### Expressions, and what goes wrong

Numeric arguments are integer expressions: `+ - * / %`, parentheses, unary minus,
variables. No functions, comparisons or floats — everything is int32 and
truncating.

```
RECT(4,4,4+{d}*8,12,color=0,fill=1)     how far through the month we are
LINE(60,60,60+{H}*2,60,color=0,width=2) a crude hour hand
```

There are no conditionals and no way to bind an intermediate value, so a face that
needs one re-derives it — the `Month grid` preset computes the weekday of the 1st
in all 31 of its placements and clips days 29–31 off-panel in short months with
`n/({D}+1)`.

**Nothing throws.** A malformed expression, an unknown variable and division by
zero all evaluate to 0; an unrecognised line is skipped. A shelf label with no
host in range has to keep drawing something.

Forgiveness is not silence: problems are counted and reported over the status
characteristic — unknown command, unknown option, line too long, script full, bad
argument — with the line number of the first. An unknown `{variable}` renders as
the literal `{name}` rather than vanishing.

Limits: a line is at most **128 bytes**, a script **3072 bytes**.

---

## The BLE interface

Advertised name `HemaEPD-Clock`. No service UUID is advertised; discover by name.

| Attribute | UUID |
|---|---|
| Command service | `677fb260-1fc0-42c5-ab6e-e64e0c591714` |
| └ command characteristic (write) | `c0339b97-4239-4aea-a775-988f9c4d2548` |
| └ status characteristic (read) | `f2edaa0b-ce5d-4897-ab67-d6f7a3cc453a` |
| Image service | `86c08205-f21a-4257-aabd-4602d25c2448` |
| └ image characteristic (write) | `855c0ea3-ae40-4bab-8a7a-52d86e9a5a2b` |

These began as the vendor web tool's UUIDs so that tool could drive early builds.
The language has since diverged, so a client written for the original firmware
would push happily and paint garbage — different UUIDs turn that into an honest
"service not found".

**Commands** are raw ASCII and need not align to command boundaries; an ATT write
carries only MTU−3 bytes while plenty of DSL lines are longer. Bytes are appended
to the stored script and executed together, and the panel is pushed once, 400 ms
after the last byte lands. Connecting arms a fresh script, but the clear is
deferred until drawing content arrives, so a client that connects and sends
nothing leaves the face intact.

**Images** are a raw framebuffer: row-major, MSB first, `1` = white, exactly
`EPD_BUF_SIZE` bytes (4000 for 122×250, 2756 for 104×212). No header and no
offset — the tag refreshes when exactly that many bytes have arrived, so a
transfer short by one byte never completes. An image holds the panel until a
command replaces it.

**Status** is 10 bytes: a format version, the first problem's code and line number,
a problem count, flags, the stored script length, and the repaint interval. Byte 0
is a format version, so append only and never renumber.

---

## Repository layout

```
firmware/hema_epd_clock/
  src/epd/              panel driver, graphics, the DSL parser, the clock
  src/platform/         pin setup, flash-backed template store
  src/custom_profile/   GATT service definitions
  src/config/           SDK and app configuration; tag_types.h is the tag table
  e2studio/             generated project files (tracked)
  test/                 host tests, and `render` — the firmware renderer on the host
webui/                  browser control panel: editor, live preview, image upload
tools/                  build, flash, SUOTA/boot image wrapping, project generation
out/                    built images, named by type (gitignored)
```

Third-party material — the SDK, the vendor's firmware images and web tool, flash
dumps, and the reverse-engineering record — is deliberately kept **outside** this
repository. Nothing here is derived from or linked against any of it. A few error
messages point at working documents that live beside the repo rather than in it.

### Tests

```sh
cd firmware/hema_epd_clock/test && make && make render render-low && cd -
node --test webui/test.mjs
```

The C tests compile the pure modules natively against stubs — no SDK, no
toolchain, no tag. Build `render` and `render-low` before the JS suite or its
byte-identity check silently skips.

`webui/epd.js` carries the same Bresenham, rotation transform, glyph tables and
`{}` expansion as the firmware. **Change a primitive in one and change it in the
other** — the preview's whole value is that it shows what the panel will. Several
pairs are pinned by tests: the default face against `presets.js` byte for byte, the
option tables, and the buffer limits read out of the C source. The 16×24 font is
generated into both from ASCII art in `tools/font16.py`; edit the art and re-emit
rather than hand-patching either copy.

---

## Known gaps

- **Type 2 has never been seen to work**, and the panel on the one unit here looks
  dead rather than the build wrong — see [above](#does-this-fit-my-tag).
- **Which LUT shape a controller wants is not predictable** from anything visible
  except the panel's lot code. It was gated on panel resolution once and on board
  variant once; both matched every tag available at the time and both were
  falsified by the next. The controller cannot be asked either — its identity
  registers differ by lot on variant B and are absent on variant A.
- **`epd_panel_present()` returns false on a working panel** — on variant A nothing
  drives the status register, so it reads `0xFF` against `0x00` and the boolean
  says "absent". The raw bytes are trustworthy; nothing uses the boolean.
- **The clock does not survive a power cut.** There is no RTC. A tag boots at
  `00:00` on 2000-01-01 until a host sends `TIME()`; the picture persists, the time
  does not.
- **No SUOTA.** Updating the firmware means SWD, every time.

---

## Licensing

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions, web UI and tooling — is GPL-3.0, per [`LICENSE`](LICENSE).

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an embedded
device can brick it. Dump the original SPI flash before you write anything
permanent — and note that this firmware deliberately writes only bank 1, so the
stock image in bank 2 remains the fallback.
