# DA14585-ESL-213

Open-source replacement firmware for the 2.13" **Hema (盒马) electronic shelf
label** — the decommissioned supermarket price tags built around a
Dialog/Renesas DA14585 BLE SoC driving an SSD1680-family e-paper panel — plus a
browser control panel for driving it.

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
host (`firmware/hema_epd_clock/test/render`) — not mockups. Each one is a short
script the tag stores and re-runs on a timer; that is what makes it a clock
rather than a picture.

---

## Contents

- [What the firmware does](#what-the-firmware-does)
- [Does this fit my tag?](#does-this-fit-my-tag)
- [Getting started](#getting-started)
- [The drawing language](#the-drawing-language)
- [The BLE interface](#the-ble-interface)
- [Repository layout](#repository-layout)
- [Developing on it](#developing-on-it)
- [Known gaps](#known-gaps)
- [Licensing](#licensing)

---

## What the firmware does

- **Drives the panel directly** — 1bpp framebuffer, points, lines, rects,
  circles, pixel inversion, two fonts, four screen rotations.
- **Runs a small drawing language.** Numeric arguments are integer expressions
  and date/time variables work inside them, so a face can *draw* itself rather
  than only label itself — a progress bar across the month, a hand that tracks
  the hour.
- **Keeps a software clock.** The DA14585 has no RTC, so a 1 Hz timer counts
  from a 2000-01-01 epoch and the host sets it on connect. It does not survive a
  power cut.
- **Repaints on the face's own schedule** — every minute for a clock, once a day
  for a calendar. A full panel refresh is the most expensive thing the tag does.
- **Refreshes only the rows that changed**, where the panel supports it: no
  black-white flash on a minute tick, and a fraction of the drive. Opt-in with
  `--partial`; see [partial refresh](#partial-refresh).
- **Stores the face in SPI flash**, so the picture survives a power cut, and
  versions it so a face written against an older language falls back to the
  built-in default rather than misdrawing.
- **Reports what it made of a script** over a status characteristic, so a typo
  is visible without a debugger.
- **Exposes two GATT services**: one for the face, one for a raw framebuffer
  image.
- **Reads the panel's own temperature sensor** on every refresh, and renders it
  as `{T}`.

The web UI previews a face pixel-for-pixel before you push it, dithers and
uploads arbitrary images, and sets the clock — to this machine's time, or to any
date you type.

---

## Does this fit my tag?

Four physically distinct tags have been handled. They differ along **three
independent axes** — board wiring, panel model, and the waveform the panel
accepts — and those axes do not move together. Knowing a tag's board does not
tell you its panel.

| Type | Board wiring | Panel | Panel bus | Status |
|---|---|---|---|---|
| **1** | variant B | A53, 122×250 | hardware SPI, shared with the boot flash | driven — the reference board |
| **2** | variant A | A53, 122×250 | bit-banged, separate from the flash | **unverified** — see below |
| **3** | variant A | A41, 104×212 | bit-banged, separate from the flash | driven |
| **4** | variant B | A41, 104×212 | hardware SPI, shared with the boot flash | driven |

The type number is the only thing you ever type. It selects both the wiring and
the geometry (the table lives in
[`src/config/tag_types.h`](firmware/hema_epd_clock/src/config/tag_types.h)) and
is stamped into the built image, so the flasher can refuse a mismatch before
writing anything.

Type 2 is the right build on paper and has never been seen to work: the only tag
we have with that pairing has a panel that answers on no line at all, so the
combination is untested rather than broken. Everything below still applies to
it — you would be the first to find out.

> **Getting the type wrong is silent in the worst way.** The tag boots,
> advertises and takes connections exactly as normal, and only the panel stays
> dead — so it presents as a broken screen rather than a wrong image. That has
> cost a working tag twice. Establish the type before you flash, not after.

### Working out which one you have

**1. Read the panel label off the flex.** `A53` → 122×250. `A41` or `A07` →
104×212. It is printed on the FPC where it leaves the board.

![The four boards side by side. Types 2 and 3 carry an Alibaba Group silkscreen
and labelled RST/GND/URX/UTX/VBAT and SWDIO/SWCLK pads; Types 1 and 4 have an
unlabelled TP1–TP8 row instead. The panel FPC enters at the left of each, with
its HINK-E0213A53 or A41 part number printed on
it](docs/img/boards.jpg)

All four, same scale and orientation: panel flex at the left, DA14585 at the
right. Type 2 and Type 3 are the same board with a different panel fitted, and so
are Type 1 and Type 4.

At a glance the variant-A boards carry an **Alibaba Group** silkscreen and bring
SWD out on pads labelled `SWDIO`/`SWCLK`, while the variant-B boards have an
unlabelled `TP1`–`TP8` row in roughly the same place. A first glance, not a
verdict — confirm with the wiring.

**2. Tell variant A from variant B by the wiring.** The two pin maps are
disjoint enough to distinguish by continuity-testing the panel FPC back to the
DA14585 package:

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

On variant B the panel shares CLK/MOSI with the boot flash and `P0_5` is both
the panel's D/C *and* the flash's MISO, which is why the driver has to claim and
release the bus. On variant A the panel's pins are disjoint from the flash's and
there is no sharing to arrange.

On a tag still running factory firmware you can instead read the pin map out of
its runtime table in SysRAM — one run of eight distinct `(port, pin)` pairs, which
for variant A reads `P2_1 P2_2 P1_0 P0_1 P2_0 P0_7 P1_1 P2_3`. It is built at
runtime, so a flash dump will not contain it.

**Do not** identify the variant by sampling GPIO modes at boot. It reads as
variant B on a variant-A board every time: the bit-banged pins are outputs only
during a transfer, e-paper is bistable so a tag need not refresh at boot at all,
and the pins driven early belong to both maps.

### The waveform is a third axis, and it is per panel lot

Two panel init sequences are carried. The **Waveshare** table is a hand-written
LUT from Waveshare's `EPD_2IN13_V2` reference — roughly **2.5× faster**, and the
only one that can do partial refresh. The **OTP** sequence is the panel's own,
loaded by the controller out of its OTP and temperature-compensated: slower, but
it drives every panel we have.

Not every panel accepts the Waveshare table, and **the type number does not
identify the panel lot**. Two Type 4 tags — same board, same panel model —
disagree, and so do two Type 3s. When a panel rejects it the matrix stays
completely inert while the border electrode flickers, which reads as a broken
screen rather than a wrong build.

So: **every type defaults to Waveshare. Flash it, look at the glass, and rebuild
with `--otp` if the matrix never moved.**

```sh
tools/build.sh --type 4              # Waveshare, and partial-capable
tools/build.sh --type 4 --otp        # the panel's own waveform
tools/build.sh --all                 # both, plus a -partial image, for every type
```

Images stamp `HEMA-WAVEFORM-OTP` or `-WAVESHARE` and `flash.sh` prints which is
about to go on. Write down the lot code on the panel's FPC alongside the result —
it is the only thing that has ever predicted this.

### Partial refresh

`--partial` repaints only the rows that changed, using the partial waveform. A
minute tick stops flashing and takes a fraction of the drive; a pushed face or an
uploaded image always paints fully, and a full refresh is forced every 8 partials
or every hour to sweep out accumulated residue.

Two constraints:

- **Waveshare lots only.** These panels carry no partial waveform in their OTP —
  asking the controller to load one takes exactly as long as a full refresh, so
  there is nothing there. `build.sh` refuses `--otp --partial`.
- **Ghosting is inherent.** A partial waveform does not fully clear, so faint
  residue builds between full refreshes. That is the trade, not a defect.

---

## Getting started

### Try the editor first — no hardware, no build

The web UI runs the whole renderer in the browser. You can write faces, preview
them pixel-for-pixel against either panel, and load the presets, all without a
tag in the room:

```sh
python3 webui/serve.py            # https on port 8443
```

Then open the printed URL. Chrome, Edge or Opera on desktop or Android; Safari
and Firefox do not implement Web Bluetooth at all, which on iOS means no browser
will work.

`serve.py` exists because Web Bluetooth is gated behind a *secure context* —
`https://` or `http://localhost`, and nothing else. `python3 -m http.server` is
fine on the machine running it and silently useless from anywhere else: the page
loads, `navigator.bluetooth` is undefined, and the browser gives no hint why.
The self-signed certificate means one click-through per device.

### What flashing actually needs

| Requirement | Notes |
|---|---|
| **A J-Link probe** | Genuine, a Renesas dev board's on-board J-Link, or an OB clone. Four wires: SWDIO, SWCLK, GND, VTref to the 3.3 V rail. |
| **Power on the tag** | Battery in, or bench 3.3 V on the battery rail. SWD does not power the board. |
| **A soldering iron** | SWD comes out on test pads. The variant-A boards silkscreen them `SWDIO`/`SWCLK`; the variant-B boards bring out `TP1`–`TP8` with no functional labels, so beep them back to DA14585 package pins 25 (SWDIO) and 26 (SWCLK). The panel flex is soldered, not socketed. |
| **The community J-Link device definition** | `JLinkDevices.xml` plus Dialog's `jtag_programmer.axf`, installed into `/opt/SEGGER/JLink`. Without it `device DA14585` exposes no flash bank and `loadbin` silently has nowhere to write. |
| **Renesas SDK 6.0.22.1401** | Licensed and account-gated — download it yourself. Not vendored here. |
| **A dump of your own tag's flash** | See the next step. Non-negotiable. |

### 1. Dump your tag's flash before writing anything

This is both your only way back and a build input: `mksuota.py` reads the image
bank offsets out of your dump and copies the stock bank header, patching only
the fields it understands. The offsets differ between Type 1 and the rest, so
another tag's dump is not a substitute.

Use **J-Flash**, not a hand-driven reader — it does the whole read in one
connection and takes about 19 s for 512 KiB:

```sh
JFlashExe -openprj<proj> -connect \
          -readrange0x04000000,0x0407FFFF -saveas stock_flash_512k.bin -exit
```

The separator in `-readrange` is a **comma**; a dash fails with "Separator has
to be ','" *after* connecting, which reads like a target fault and is not one.

Read twice and compare. Two identical passes is the cheapest proof of a good
dump; a full-size file on its own is not proof.

### 2. Set the SDK up

The SDK ships a linker-script defect that will hard-fault this firmware inside
`Reset_Handler` before `main()`: the startup tables carry byte counts where
CMSIS's `__cmsis_start()` expects word counts, so startup zeroes four times the
bytes it should and walks off the top of SysRAM. It stays invisible until `.bss`
grows past the point where the overshoot leaves the 96 KiB of RAM — which this
firmware's 3 KiB script buffer does.

The project generator owns that patch. Run it against your SDK:

```sh
DA1458X_SDK=~/DA145xx_SDK/6.0.22.1401 python3 tools/gen_e2studio_project.py
```

It reports `ldscript : 4 byte->word count(s) fixed`, leaves a `.orig` alongside,
and is harmless to re-run. It also regenerates
[`firmware/hema_epd_clock/e2studio/`](firmware/hema_epd_clock/e2studio) — the
project files derived mechanically from the SDK's own `prox_reporter` example,
which are tracked here so you do not have to graft one together in the GUI.

### 3. Build

SDK6 projects reference the SDK by relative path, so the project has to live
*inside* the SDK tree at the same depth as the examples:

```sh
cp -r firmware/hema_epd_clock \
      "$SDK/projects/target_apps/template/hema_epd_clock"
```

Import `…/hema_epd_clock/e2studio` into e² studio (**File → Import → General →
Existing Projects into Workspace**), select the **DA14585** configuration and
build once. That first build is what generates the makefiles under
`e2studio/DA14585/`.

After that, e² studio is not needed again — it only ever supplied the compiler
(LLVM Embedded Toolchain for Arm, `clang --target=armv6m-none-eabi`) and the
makefiles. `tools/build.sh` drives them headlessly:

```sh
export LOCAL_PROJ="$SDK/projects/target_apps/template/hema_epd_clock"

tools/build.sh --type 3             # -> out/hema_epd_clock-type3.bin
tools/build.sh --type 3 --otp      # the panel's own waveform instead
tools/build.sh --type 3 --partial  # partial refresh, Waveshare lots only
tools/build.sh --all               # every type, every variant, one vintage
tools/build.sh --type 3 --clean
```

Use `--all` after changing anything in the driver. A mixed-age `out/` is a real
trap: the images are all correctly named for their tag type and give no hint of
their age, so a stale one reproduces a bug you already fixed.

The `cp -r` above is a one-time bootstrap: `build.sh` mirrors the repo's sources
into that tree on every run, so from here on you edit in the repo and never
touch the copy.

The type is passed to the compiler as `-DHEMA_TAG_TYPE=n`, so switching types
touches nothing git tracks. `build.sh` patches `$(TAG_DEFS)` into the generated
`subdir.mk` files on demand, cleans between types, and **checks the finished
binary's own type stamp against what you asked for** before reporting success —
every way this can go wrong yields a working image for the *wrong* tag rather than
an error, so the check has to be on the artefact.

### 4. Flash

```sh
tools/flash.sh --type 3 stock_flash_512k.bin out/hema_epd_clock-type3.bin
```

Or drop the dump argument and put dumps at `$HEMA_STOCK_DIR/type<n>/stock_flash_512k.bin`.

A secondary bootloader reads a product header at `0x038000` to find two image
banks and boots the *newest valid* one. This writes **bank 1** and leaves the stock
image in bank 2, so a bad build falls back to something that works rather than
bricking the tag. It is also why a raw `.bin` at offset 0 does not boot on this
board. `mksuota.py` builds the bank image and blanks the template store sector, so
the tag comes back on the built-in default face; `mkbootimg.py` is the *other*
format (AN-B-001) and not what you want here.

`flash.sh` cross-checks the type, wiring and panel geometry stamped in the binary
against what you typed and refuses on any mismatch. It also treats J-Link's error
lines as fatal, because `JLinkExe` exits 0 even when it never reached the probe.

Then **power-cycle the tag**. An SWD reset does not re-run the bootloader's bank
scan on this board, so the old image keeps running until the power actually
drops.

If a flash dies verifying RAMCode and the `Write:`/`Read:` lines differ by only
a bit or two, that is the SWD link, not the target — reseat the wires and retry
with `--speed 1000`. These probes are often OB clones and the wires are soldered
to test points, so it is the common case. If they differ wholesale, it is the
SysRAM clash the message suggests: power-cycle and flash as the *first* J-Link
operation.

### 5. Drive it

The tag comes up advertising as **`HemaEPD-Clock`**, showing the built-in clock
face and reading `00:00` on 2000-01-01 until a host syncs it. Open the web UI,
**Connect**, pick your panel in the Preview pane (nothing in the protocol says
which one a tag has — it is yours to know), **Sync time**, then edit and **Push
to tag**.

---

## The drawing language

A face is a short ASCII script, one command per line, stored on the tag and
re-run on a timer so the `{}` variables re-expand and the picture keeps up with
the clock.

```
ROTATE(270)
CLEAR(1)
TEXT(125,18,'{H:02d}:{N:02d}',font=1,scale=2,align=1)
TEXT(125,78,'{y}-{m:02d}-{d:02d}',scale=2,align=1)
TEXT(125,100,'{W}',scale=2,align=1)
```

That is the built-in default face, verbatim — written in the DSL rather than
drawn in C so it goes through exactly the same path as anything you push.

### Commands

Required geometry is positional; everything else is named and optional, which is
what lets an option be added later without disturbing a face already stored on a
tag.

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

- **`color`** is `0` for black, `1` for white.
- **`INVERT`** takes a width and height, not a second corner, and flips every
  pixel in the box — which is how you highlight a calendar cell without knowing
  what is under it. Draw it *last*: the 5×7 font paints its whole glyph cell, so
  a number drawn afterwards would blank the part of the box it covers.
- **`TEXT`** is opaque, because `bg` fills the glyph cell; white-on-black is
  `color=1,bg=0`. `align=` moves the anchor — `0` left edge, `1` centre, `2`
  right edge — so centring on the panel is `align=1` at `x = width/2`.
- **`font=0`** is a 5×7 general font (digits, uppercase, and the punctuation a
  clock or calendar needs), scaling up in whole pixels. **`font=1`** is a 16×24
  font of digits and `:` only, for the case that wants big time and would
  otherwise get a block of 5 px squares. A character `font=1` lacks draws blank
  rather than falling back, and the preview names it.
- **`ROTATE`** takes degrees only. `90` and `270` are landscape, which is how
  these sit on a shelf. `ROTATE(3)` — the vendor's index form — is *reported as
  an error* rather than silently taken as 3 degrees.
- **`EVERY(n)`** sets minutes between repaints, 1 to 1440, and rides along with
  the face that wants it. A calendar redrawing 31 identical numbers 1440 times a
  day to change nothing wants `EVERY(1440)`; boundaries are absolute, so that
  lands at midnight rather than wherever the tag booted.
- **`TIME(seconds since 2000-01-01)`** and **`RESET()`** are control commands.
  They are applied on arrival and never stored, so a `TIME()`-only sync leaves
  the face alone, and `RESET()` lets an editor push repeatedly on one
  connection.

### Variables

`{}` references expand inside strings *and* inside numeric arguments. Format
them with `{name:0Nd}` — `{H:02d}` is a zero-padded hour, `{H:2d}` a
space-padded one.

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

Four are text rather than numbers, and work only inside a string: `{W}` (`SUN`…
`SAT`), `{M}` (`JAN`…`DEC`), `{P}` (`AM`/`PM`) and `{VER}` (`HEMA1`).

Lower case is a position and upper case the length it runs against — `{d}`/`{D}`
within the month, `{j}`/`{J}` within the year — which is what makes a progress
bar one line.

### Expressions

Numeric arguments are integer expressions: `+ - * / %`, parentheses, unary
minus, and variables. No functions, no comparisons, no floats — everything is
int32 and truncating, matching the panel's coordinate space.

```
RECT(4,4,4+{d}*8,12,color=0,fill=1)     how far through the month we are
LINE(60,60,60+{H}*2,60,color=0,width=2) a crude hour hand
```

There are no conditionals and no way to bind an intermediate value, so a face that
needs one re-derives it — the `Month grid` preset computes the weekday of the 1st
in all 31 of its number placements, and clips days 29–31 off-panel in short months
with `n/({D}+1)`.

### What it will and will not tell you

**Nothing throws.** A malformed expression, an unknown variable and division by
zero all evaluate to 0; an unrecognised line is skipped. A shelf label with no host
in range has to keep drawing something.

Forgiveness is not silence, though: problems are counted and reported over the
status characteristic — unknown command, unknown option, line too long, script
full, bad argument — with the line number of the first. An unknown `{variable}`
renders as the literal `{name}` rather than vanishing.

Limits: a line is at most **128 bytes**, a whole script at most **3072 bytes**.
The panel is 122×250 (landscape 250×122) or 104×212 (landscape 212×104)
depending on the tag.

---

## The BLE interface

Advertised name `HemaEPD-Clock`. No service UUID is advertised; discover by
name. Two services, both with UUIDs of our own:

| Attribute | UUID |
|---|---|
| Command service | `677fb260-1fc0-42c5-ab6e-e64e0c591714` |
| └ command characteristic (write) | `c0339b97-4239-4aea-a775-988f9c4d2548` |
| └ status characteristic (read) | `f2edaa0b-ce5d-4897-ab67-d6f7a3cc453a` |
| Image service | `86c08205-f21a-4257-aabd-4602d25c2448` |
| └ image characteristic (write) | `855c0ea3-ae40-4bab-8a7a-52d86e9a5a2b` |

These began as the UUIDs the vendor's own web tool used, so that tool could
drive early builds. The language behind them has since diverged, so a client
written for the original firmware finding this service would push happily and
paint garbage. Different UUIDs turn that into an honest "service not found".

**Commands** are written as raw ASCII and need not align to command boundaries —
an ATT write carries only MTU−3 bytes (20 at the default MTU) while plenty of
DSL lines are longer. Bytes are appended to the stored script and executed
together; the panel is pushed once, 400 ms after the last byte lands. Connecting
arms a fresh script, but the clear is deferred until the first drawing content
arrives, so a client that connects and sends nothing leaves the face it found
intact.

**Images** are a raw framebuffer: row-major, MSB first, `1` = white, exactly
`EPD_BUF_SIZE` bytes (4000 for 122×250, 2756 for 104×212). There is no header
and no offset in the protocol — the tag refreshes when exactly that many bytes
have arrived, so a transfer short by one byte simply never completes. An image
holds the panel until a command replaces it.

**Status** is 10 bytes: a format version, the first problem's code and line
number, a problem count, flags, the stored script length, and the repaint
interval. Byte 0 is a format version so a client reads the fields it knows and
ignores the rest — append only, never renumber.

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
repository. Nothing here is derived from or linked against any of it. A few
error messages in `tools/` and `src/config/tag_types.h` point at working documents
under `hema-local/docs/` that live beside the repo rather than in it; the
[tag table above](#does-this-fit-my-tag) is what they say about types.

---

## Developing on it

### Tests

```sh
cd firmware/hema_epd_clock/test && make && make render render-low && cd -
node --test webui/test.mjs                # preview parity, preset fit
```

The C tests compile the pure modules natively against stubs — no SDK, no
toolchain, no tag. Build `render` and `render-low` before the JS suite or its
byte-identity check silently skips: it needs a firmware renderer per geometry to
diff the preview against.

### The preview is a port, not an approximation

`webui/epd.js` carries the same Bresenham, the same rotation transform, the same
glyph tables and the same `{}` expansion as `src/epd/epd_gfx.c` and
`src/epd/epd_cmdparser.c`. **If you change a primitive in the firmware, change it
there too** — anything that drifts is a bug, and the whole point of the preview
is that what you see is what the panel will show.

Several pairs are pinned by tests rather than good intentions: the built-in default
face against `presets.js` byte for byte, the option tables on both sides, and the
buffer limits, read out of the C source rather than repeated. The one intentional
difference is that the firmware silently ignores commands it does not implement
while the preview reports them.

`firmware/hema_epd_clock/test/render` is the tiebreaker. When the panel disagrees
with the preview there are three candidates — the firmware, the JS port, and
whatever rig is reading the tag — and comparing two of them cannot say which:

```sh
cd firmware/hema_epd_clock/test && make render
printf "ROTATE(270)\nCLEAR(1)\nTEXT(4,4,'HI',scale=2)\n" | ./render 838391825 > fb.bin
```

### Two more things that bite

The **16×24 font** exists twice — `src/epd/epd_gfx.c` and `webui/epd.js` — and
both are generated from ASCII art in `tools/font16.py`. Edit the art and re-emit
into both (`--emit`, `--js`) rather than hand-patching either.

**A fifth tag** that pairs an existing board with an existing panel is a row in
`src/config/tag_types.h` and nothing else; `--all` and the flasher's checks pick it
up from there. State how you established the board variant, the panel model from
the label, and which waveform you tested.

---

## Known gaps

- **Type 2 has never been seen to work.** Rail, BUSY continuity, pin
  configuration and framebuffer all check out, and the controller answers on no
  line. RST continuity from FPC pad 10 to `P1_0` was never measured — check that
  first on a second unit, since an open reset produces exactly this picture.
- **Which waveform a panel accepts is not predictable** from anything visible
  except the lot code on the FPC. It was gated on panel resolution once and on
  board variant once; both matched every tag available at the time and both were
  falsified by the next one. The controller cannot be asked either — its identity
  registers differ by lot on variant B and are absent on variant A.
- **Partial refresh is unavailable on OTP-only lots**, whose OTP holds no partial
  waveform to load. Those tags get full refreshes only.
- **`epd_panel_present()` returns false on a working panel** — on variant A
  nothing drives the status register, so it reads `0xFF` against `0x00` and the
  boolean says "absent". The raw bytes are trustworthy; nothing uses the boolean.
- **The clock does not survive a power cut.** There is no RTC. A tag boots at
  `00:00` on 2000-01-01 until a host sends `TIME()`; the picture persists, the time
  does not.
- **No SUOTA.** Updating the firmware means SWD, every time.

---

## Licensing

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions, web UI and tooling — is GPL-3.0, per [`LICENSE`](LICENSE).

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an
embedded device can brick it. Dump the original SPI flash before you write
anything permanent — and note that this firmware deliberately writes only bank
1, so the stock image in bank 2 remains the fallback.
