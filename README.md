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
  circles, pixel inversion, three fonts, four screen rotations.
- **Renders Chinese and Japanese.** Text is UTF-8, and a 16×16 font carries the
  characters the faces actually use — `LOCALE(zh)` or `LOCALE(ja)` switches the
  weekday, month and AM/PM names over. See [fonts](#fonts).
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
- **Reads the panel's own temperature sensor** and renders it as `{T}`, and the
  battery as `{BAT}` (percent) and `{VCC}` (millivolts).
- **Updates its own firmware over BLE**: 40 KB in ~35 s, into whichever
  image bank is not running, so a failed update leaves the tag booting what it was
  already running. See [firmware update over BLE](#firmware-update-over-ble).

The web UI previews a face pixel-for-pixel before you push it, dithers and uploads
arbitrary images, and sets the clock. Faces are browsed as thumbnails rendered by
that same previewer, can be saved and exported, and the editor's contents survive
a reload.

---

## Does this fit my tag?

**One image runs on every one of them.** The firmware reads the board's own
16-byte record at flash `0x039000` when it starts, and takes its pin map, its
panel geometry and its default clock face from that. There is no tag type to
choose and no variant to state.

Four physically distinct tags have been handled, and the differences that used
to matter are now the tag's business rather than yours:

| Type | Board | Panel | Status |
|---|---|---|---|
| **1** | variant B | A53, 122×250 | driven |
| **2** | variant A | A53, 122×250 | **untested — suspected dead panel** |
| **3** | variant A | A41, 104×212 | driven |
| **4** | variant B | A41, 104×212 | driven |

Verified by building for one and running it on another: an image whose
compile-time seed is 122×250 drives a 104×212 tag with the right pins, the right
frame size and the right default face.

**What you must not lose is that record.** It is the only copy of the tag's
identity, and a full-flash write erases it — so `tools/flash.sh` refuses unless
you give it `--fallback <a stock dump of this tag>` or `--record <name>`. An
erased record reads as the built-in case, variant B and 122×250: right for a
Type 1, and a dark or garbled panel on the others with nothing to say why.

**Type 2 is untested because of a suspected dead panel, not a wrong build.** On
the single unit here the panel rail sits steady at 3.3 V, BUSY continuity is
good, the pin configuration reads back correct and a properly rendered
framebuffer reaches the tag — and the controller answers on no line at all. If
you have one, RST continuity from FPC pad 10 to `P1_0` was never measured here;
check that first.

### What the record says

| offset | meaning |
|---|---|
| `+0x00` | panel: `0x14` = A53 122×250, `0x09` = A41 104×212 |
| `+0x01` | pin map: `0x01` = the map below applies, `0xFF` = built-in |
| `+0x08`–`+0x0F` | the map, one byte per signal, packed `(port << 4) \| pin` |

Only a variant-A board carries a map; variant B is the built-in case and leaves
it erased. A factory Type 3 reads
`09 01 ff ff ff ff ff ff 21 22 10 01 20 07 11 23`.

This is the same mechanism the vendor uses — their retail firmware is one
byte-identical image across all four tags, with a runtime descriptor at
`0x07FD4320` holding the pin table, the geometry and the frame size.

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
tools/build.sh                    # Waveshare, 7 steps (the default)
tools/build.sh --lut-steps 10     # Waveshare, 10 steps
tools/build.sh --otp              # the panel's own waveform
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
tools/build.sh                # -> out/hema_epd_clock.bin
tools/build.sh --all          # after any driver change - see below
```

`build.sh` mirrors the repo's sources into that tree on every run, so from here
on you edit in the repo and never touch the copy. There is no tag type to pass;
the one option that still changes the image is the waveform, because that is
keyed to the panel's lot rather than to the board.

Use `--all` after changing the driver. A mixed-age `out/` is a real trap: a stale
image gives no hint of its age and reproduces a bug you already fixed. `--all`
builds all five of one vintage — the default, `--otp`, `--partial`, and both LUT
shapes. `tools/build.sh -h` lists the bench options.

### 3. Flash

```sh
tools/flash.sh --fallback stock_flash_512k.bin out/hema_epd_clock.bin
```

The dump must be **of this tag**: it supplies the board record that the write
would otherwise erase, and it fills the other bank. If you have no dump, state the tag
instead — `--record a53-b`, `a53-a`, `a41-a` or `a41-b`, naming the panel and
the pin map, which is what the record actually carries. `--type <n>` says the
same by tag number (1 = `a53-b`, 2 = `a53-a`, 3 = `a41-a`, 4 = `a41-b`,
6 = `a41-b`) and still works, but two types share one record: Types 4 and 6 are
different boards — the second has a socketed panel — and are indistinguishable
from flash. There is no Type 5; that number named the nRF52811 board until
2026-08-14 and was retired rather than reissued. Either way it says what the
**tag** is, not what to build; there is one image. `flash.sh` refuses without one rather than quietly erasing
the tag's identity, and that refusal is load-bearing: a flash really does erase
the record, tested, and the tag then comes up with a dead panel and nothing else
wrong. `--force` skips both, for bench work.

A secondary bootloader reads a product header at `0x038000` to find two image
banks and boots the newest valid one. This writes **bank 1** and leaves the stock
image in bank 2, so a bad build falls back to something that works rather than
bricking the tag — and it is why a raw `.bin` at offset 0 does not boot on this
board. `mksuota.py` builds the bank image and blanks the template store, so the
tag returns to the built-in default face. There is deliberately no tool for the
other format (AN-B-001, a raw image at offset 0): Types 1, 3, 4 and 6 boot from
OTP, which ignores offset 0 entirely.

**One board is the exception.** The Type 7's OTP is blank, so it boots the
AN-B-001 image at flash `0x000000` — the same bootloader, just stored in flash
instead of OTP — and that image is the only boot path it has. The bank format
above is unchanged; what changes is that offset 0 must be written rather than
left empty. Flash it with `--fallback <its own dump>` or `--bootloader`, never
with the synthesised default. See `hema-local/docs/BOOT_CONTRACT.md`.

`flash.sh` treats J-Link's error lines as fatal, because `JLinkExe` exits 0 even
when it never reached the probe.

Then **power-cycle the tag** — an SWD reset does not re-run the bootloader's bank
scan, so the old image keeps running until the power actually drops.

If a flash dies verifying RAMCode and the `Write:`/`Read:` lines differ by a bit
or two, that is the SWD link: reseat and retry with `--speed 1000`. If they differ
wholesale, power-cycle and flash as the *first* J-Link operation.

A flash can also fail *without* J-Link noticing: verify reads back over the same
link, so a link fault that corrupts writes corrupts the verify identically. Seen
once at the default 4 MHz, as a whole bank written one bit shifted — the tag then
booted the other bank and looked like a dead panel. `--speed 1000` fixed it. If a
freshly flashed tag misbehaves, suspect the flash before the firmware.

### Firmware update over BLE

Every build carries the standard SUOTA service unless you pass `--no-suota`, so a
tag can be updated over the air — which is what you want as soon as there are more
tags than J-Links. Note that an image built *without* it can only be replaced by
attaching SWD to that tag again, so `--no-suota` is a one-way door.

```sh
tools/build.sh --lut-steps 10 --partial
tools/mksuota.py --ota out/hema_epd_clock-s10-partial.bin update.img
```

Then push it with any SUOTA client — the tag advertises SUOTA's
`0xFEF5`, which is what standard clients scan for. Measured on a Type 4 tag:
**40 KB in ~35 s**, three consecutive updates, each rebooting into the image it
received. A client will ask for the flash wiring: MISO `P0_5`, MOSI `P0_6`, CS
`P0_3`, SCK `P0_0`, on both board variants. Blocks must be **512 bytes or fewer**.

The receiver writes whichever bank is *not* running and only marks it bootable
once the whole image has arrived and checksummed, so an interrupted transfer
leaves that bank invalid and the tag goes on booting what it was already running.
That was tested directly before the feature was trusted — three kinds of broken
bank, including one claiming to be valid and newer with a bad CRC, and the tag
recovered from all of them. Both banks end below the product header and the stored
face lives above it, so **an update never costs the tag its picture**.

Note the first successful update overwrites the vendor image, since that is the
bank not in use. From then on the tag is its own fallback: a failed update rolls
back to the previous version of *this* firmware. Keep the stock dump.

Every image stamps a compatibility identity into its header, and the tag reports
the same string on the DIS Firmware Revision characteristic:

```
U1-W10     universal generation, waveform, LUT steps
```

It names only what an update can still get wrong. The wiring and geometry are
not in it because the image no longer has an opinion about either — it reads
them off the tag. The waveform is, because that is keyed to the panel's lot and
nothing on the tag reports it: a 7-step table on a 10-step controller runs zero
frames and leaves the glass blank.

A client should read it and refuse a mismatch **before** transferring. The tag
does not enforce this itself, so a generic SUOTA app can still push the wrong
image; use a client that checks.

**Tags running a pre-`U1` image will refuse the first over-the-air update** —
their identity cannot be told apart from a genuinely incompatible one. Each
needs one flash over SWD to reach this firmware; after that OTA works normally.

### 4. Drive it

The tag comes up advertising as **`Tag-682F8D`** — the tail of its own MAC —
showing the built-in clock face and
reading `00:00` on 2000-01-01 until a host syncs it. Open the web UI and
**Connect**; the chooser filters on the tag's service UUID, so only tags running
this firmware are offered.

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
| `LOCALE(en\|zh\|ja)` | — |
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
- **`font=0`** is a 5×7 general font — all printable ASCII plus `°` — scaling in
  whole pixels. **`font=1`** is 16×24, digits and `:` only, for big time.
  **`font=2`** is 16×16 Chinese and Japanese, with ASCII at 8×16 so a mixed
  string lines up. A character a font lacks draws blank rather than falling
  back to another size; the preview names it.
- **`ROTATE`** takes degrees only; `90` and `270` are landscape. `ROTATE(3)` — the
  vendor's index form — is reported as an error rather than taken as 3 degrees.
- **`EVERY(n)`** sets minutes between repaints, 1 to 1440, and travels with the
  face that wants it. Boundaries are absolute, so `EVERY(1440)` lands at midnight
  rather than wherever the tag booted.
- **`LOCALE(code)`** picks the language `{W}`, `{M}` and `{P}` render in — an ISO
  639-1 code, not an index, so a face says what it means. It applies from where it
  appears, like `ROTATE`, and resets to `en` each run, so dropping it cannot leave
  the previous face's language standing. An unknown code is reported and changes
  nothing. Chinese and Japanese need `font=2`; at `font=0` they draw as gaps, and
  the preview says which characters.
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
| `{G}` | ISO week-numbering year | `{BAT}` | battery charge, 0–100 % |
| | | `{VCC}` | battery voltage, mV |

Four are text and work only inside a string: `{W}` (`SUN`…`SAT`), `{M}`
(`JAN`…`DEC`), `{P}` (`AM`/`PM`) and `{VER}` (`HEMA1`).

`{T}`, `{BAT}` and `{VCC}` render as the literal `{T}`/`{BAT}`/`{VCC}` until a
reading exists — on a build that takes none, a face says so on the panel instead
of showing a confident `0`. The web preview has a field for each; blank it to see
that case.

`{BAT}` is the number to put on a face: it comes from the SDK's CR2032 discharge
curve, which is not a straight line between two voltages. `{VCC}` is the raw
terminal voltage, useful for watching a cell age after the percentage has
flattened, and is uncalibrated per unit — trust it to a few tens of millivolts,
not to the digit.

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

Limits: a line is at most **128 bytes**, a script **3072 bytes**. A line is bytes,
not characters, so a CJK character costs three of them.

### Fonts

Three, selected with `font=` on `TEXT`:

| | Cell | Covers |
|---|---|---|
| `font=0` | 5×7 | all printable ASCII, plus `°` |
| `font=1` | 16×24 | digits and `:`, for big time |
| `font=2` | 16×16 | Chinese and Japanese, plus ASCII at 8×16 |

All three are generated into the firmware **and** the web preview by one command,
so the panel and the preview cannot disagree about what a character looks like:

```sh
python3 tools/genfont.py            # what would be built, and its size
python3 tools/genfont.py --show 年月日   # preview glyphs as ASCII art
python3 tools/genfont.py --emit     # write the tables
```

The 5×7 and 16×24 fonts are hand-drawn as ASCII art in `tools/font5.py` and
`tools/font16.py`. The CJK font is **a list of characters, not a font**: a full
Chinese font at this size is 120 KB and the image runs from 96 KiB of SysRAM, so
`tools/glyphs.txt` names the characters the faces actually draw — about 3 KB for
all of them today. Add a line there, re-run `--emit`, and it appears everywhere.

Chinese and Japanese mostly differ by *codepoint* rather than by shape, so one
table serves both: 时 and 時 are separate characters. Where they share a codepoint
and Noto draws them differently, `@lang` in `glyphs.txt` picks which design to
bake, and `--emit` lists every character where that choice was real. Glyphs come
from Noto Sans CJK (SIL OFL); do not repoint the generator at SimSun or MS YaHei,
which are Microsoft-licensed.

### Faces

The built-in faces are files, one per face, in `webui/faces/`. Every line that is
not a command is a `#` comment — which the language already ignores and the web
UI already strips before sending, so the prose costs the tag nothing:

```
# name: Calendar 中文
# category: Localised
# order: 80
#
# Calendar's skeleton with the header in the local language.

# --- panel: high
ROTATE(270)
CLEAR(1)
LOCALE(zh)
...

# --- panel: low
...
```

Both panels are required, and they are written separately rather than scaled: the
104×212 panel is not a smaller version of the same layout, and a line that fits
across 250 px often does not fit across 212.

`tools/genfaces.py --emit` bundles them into `webui/faces_data.js`, which is what
the editor imports — a static server cannot list a directory, so bundling avoids
a manifest that could drift from the files it lists. `webui/serve.py` regenerates
on startup, so editing a face is edit-and-refresh, and a test runs `--check` so a
stale bundle fails rather than silently serving the old face.

A `.face` file's panel section is a face on its own, so it feeds the renderer
directly:

```sh
sed -n '/panel: high/,/panel: low/p' webui/faces/calendar-zh.face \
  | firmware/hema_epd_clock/test/render 838944000 --temp 25 > fb.bin
```

---

## The BLE interface

**Discover by service UUID, not by name.** The advertisement carries the command
service below, and SUOTA's `0xFEF5` unless built `--no-suota`, which is what
identifies a tag running this firmware. The name is for whoever is choosing
between tags and carries no guarantee: it has changed twice, and each time it
broke every client that matched on it.

Names look like `Tag-682F8D` — the low three bytes of the tag's own BD address,
so several tags are distinguishable at a glance. It is filled in at boot from
the address the radio actually uses, so nothing is configured per unit. What a
tag IS, rather than which one it is, comes from the render-status characteristic
(bytes 10-13 give the real panel size). Because the
service UUIDs leave no room for it in the 31-byte advertisement, the name travels
in the scan response — visible to any active scan, which is what scanners do.

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
  faces/                the built-in faces, one .face file each
  faces_data.js         generated from faces/ - do not edit
tools/                  build, flash, SUOTA/boot image wrapping, project generation
  glyphs.txt            the characters font=2 carries - edit here, then --emit
  genfont.py            builds all three fonts into the firmware and the preview
  genfaces.py           builds webui/faces/ into the bundle the editor loads
out/                    built images, named by type (gitignored)
```

Third-party material — the SDK, the vendor's firmware images and web tool, flash
dumps, and the reverse-engineering record — is deliberately kept **outside** this
repository. Nothing here is derived from or linked against any of it. A few error
messages point at working documents that live beside the repo rather than in it.

### Tests

```sh
make -C firmware/hema_epd_clock/test
node --test webui/test.mjs
```

The C tests compile the pure modules natively against stubs — no SDK, no
toolchain, no tag. `make` also builds `render`, the host renderer, which the JS
suite shells out to for its byte-identity check; without it that check silently
skips. One binary covers both panels — `render <secs> --panel high|low` — the
same way one firmware image does.

`webui/epd.js` carries the same Bresenham, rotation transform, glyph tables and
`{}` expansion as the firmware. **Change a primitive in one and change it in the
other** — the preview's whole value is that it shows what the panel will. Several
pairs are pinned by tests: the default face against its face file byte for byte, the
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
- **A wrong-type image over the air is refused by the client, not by the tag.**
  The tag publishes what it is and every image says what it is for, but nothing on
  the tag compares them, so a generic SUOTA app can still push a Type 3 image to a
  Type 4 tag and leave the panel dead. Enforcing it on the tag needs a change to
  the SDK's receiver.
- **SUOTA has only been exercised on variant B.** Variant A should be easier — its
  panel and flash pins are disjoint, so there is no bus to arbitrate — but it is
  untested.

---

## Licensing

The original work here — the EPD driver, graphics layer, command parser, GATT
service definitions, web UI and tooling — is GPL-3.0, per [`LICENSE`](LICENSE).

## Disclaimer

This is unofficial, unaffiliated work on hardware you own. Reflashing an embedded
device can brick it. Dump the original SPI flash before you write anything
permanent — and note that this firmware deliberately writes only bank 1, so the
stock image in bank 2 remains the fallback.
