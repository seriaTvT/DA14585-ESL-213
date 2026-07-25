# Hema 2.13" ESL Clock Firmware — Reverse-Engineering Notes

Findings from analyzing `5_hema_clock_down_high_V1.57.bin` / `..._low_V1.57.bin` (DA14585,
262144 bytes each), the companion Web Bluetooth control page in `webpage/esl_clock.php`,
and — as of this update — the vendor's own support site, which the firmware/tools
originally came from (found still online; a throwaway account got past its login wall).
That site turned out to be the authoritative, still-maintained home of this whole
project: official function docs, direct firmware downloads for several versions, and the
source JS for the image-encoding pipeline. Section 3 below is now ground truth from the
vendor's own docs, not inference.

> The vendor's URLs are deliberately not reproduced in this document. Everything below
> is a description of observed protocol behaviour, which is what's needed to write
> independent firmware; none of the vendor's code, binaries, or documentation is
> redistributed here.

## 1. Binary format

- Bytes `0x00–0x07`: 8-byte load header. `0x70 0x50` matches the documented DA1458x
  SPI-boot preamble (Renesas UM-B-119, "Booting from Serial Interfaces").
- Real Cortex-M0 vector table starts at file offset `0x08`, loaded to RAM base
  `0x07FC0000`. DA14585 has no internal flash — the ROM bootloader copies the image into
  SRAM and executes it from there.
  `RAM_addr = file_offset - 8 + 0x07FC0000`
- Built on Dialog's official SDK6 BLE stack ("Dialog Semiconductor" + "1.0.0.0-LE"
  strings present around file offset `0x187c0`).
- **High-res vs low-res**: identical byte-for-byte up to file offset `0x28FC5`
  (~167941). All divergence is after that point — panel-specific constants
  (dimensions, LUT/timing tables), not application logic or the command language.
  Confirmed low-res panel = 104×212px, high-res = 122×250px (from the site's FAQ).
- **SUOTA `.img` update packages use a different header** than the JLink `.bin` files —
  bytes `70 51 aa 01 48 ef 00 00 70 4a 73 f6 ff ff ff ff...`, then 0xFF-padding (erased
  flash) until a second Cortex-M0-style vector table appears around file offset `0x40`.
  This matches Dialog's multi-part-image / product-header SUOTA format (distinct from the
  plain single-image JLink header) — not fully decoded yet, but the padding gap is almost
  certainly the product-header + per-image-header structs UM-B-119 describes.

## 2. Panel hardware

Confirmed via the site's FAQ ("支持PCB及LCD型号" / "裸屏驱动代码"):

- **Low-res panel**: Good Display/Hixin `HINK-E0213A41-FPC` (also seen as `HINK-E0213A07-A1`),
  104×212px.
- **High-res panel**: `HINK-E0213A53-FPC-A0`, 122×250px.
- The site's own reference driver code (for a *different* host MCU, STM32F103C8T6) is
  named `EPD_2IN13_V2_Init/Display/TurnOnDisplay/ReadBusy` — i.e. it's explicitly adapted
  from **Waveshare's public `EPD_2IN13_V2` driver** (open source, part of Waveshare's
  e-Paper GitHub repo). That STM32 reference project is downloadable directly:
  `https://pan.baidu.com/s/1XmCKk2-8yKZGn1-shBDA7A?pwd=mxnc` (提取码/extraction code:
  `mxnc`) — Baidu Cloud, so you'll need the Baidu Netdisk app/web client with that code;
  I couldn't fetch it programmatically from this session.

## 3. Display driver — SSD1680-family e-paper controller

Found in `FUN_040182f0` in the existing Ghidra decompilation (`tmp/output/*.bin.c`),
identifiable via a debug string logged just before it (`"epd_init\n"`). The byte
sequence issued through two function-pointer callbacks (cmd-writer / data-writer)
matches the public SSD1680 4-wire SPI command set exactly — and now also matches the
Waveshare `EPD_2IN13_V2` reference driver named on the vendor's own FAQ page:

| Cmd  | Meaning                          |
|------|-----------------------------------|
| 0x01 | Driver Output Control             |
| 0x0C | Booster Soft Start Control         |
| 0x11 | Data Entry Mode Setting            |
| 0x12 | SW Reset (seen in the Waveshare reference code) |
| 0x18 | Temperature Sensor Control          |
| 0x2C | Write VCOM Register                 |
| 0x3A | Set Dummy Line Period                |
| 0x3B | Set Gate Line Width                   |
| 0x3C | Border Waveform Control                |
| 0x32 | Write LUT Register (30-byte table in this firmware; Waveshare's reference uses 70 bytes — see note below) |
| 0x44 / 0x45 | Set RAM X / Y address window       |
| 0x4E / 0x4F | Set RAM X / Y address counter       |
| 0x24 | Write RAM (B/W)                          |
| 0x22 + 0x20 | Display Update Control 2 + Master Activation |

Two distinct 30-byte LUT tables are selected by a mode flag (`DAT_040184b8` vs
`DAT_040184d8`) — almost certainly full-refresh vs. partial-refresh waveforms (the
Waveshare reference's LUTs are 70 bytes; this firmware's shorter 30-byte tables suggest a
trimmed/simplified waveform, worth cross-checking against the Baidu-hosted reference
source once retrieved). Both are directly extractable as raw bytes from the binary at
those file offsets if exact waveform-timing fidelity is wanted.

**Practical takeaway unchanged and now doubly confirmed**: adapt Waveshare's public
`EPD_2IN13_V2` driver (cited by name in the vendor's own docs) rather than
reverse-engineering the panel protocol from scratch. GPIO pin assignments (CS/DC/RST/BUSY)
for the DA14585 side specifically are still untraced — the Waveshare reference is for
STM32 pins, not DA14585 pins, so that mapping still needs to come from the `.bin` (or from
the vendor's JLink flashing guide, which didn't include a pinout in what was fetched).

## 4. On-device drawing command language — full official reference

The vendor's own developer documentation (login-gated) confirms every command token
found by grepping the firmware's dispatcher, **with exact parameter lists**. The command
reference below is a transcription of that behaviour; the vendor's document itself is not
redistributed here. Full function list:

```
FONT(x, y, g, font_id, fore_color, back_color, scale=1, 'text')
  Draw text. g = character spacing. font_id 0 = system default font.
  fore/back_color: 0=white, 1=black. scale: magnification, default 1.
  e.g. FONT(5,50,10,0,0,1,'Hello World')

CAL(x, y, g_x, g_y, font_id, fore_color, back_color)
  Draw a month calendar grid; today's cell gets a black box outline.
  e.g. CAL(5,20,16,12,0,0,1)

POINT(x, y, color, pix, type)
  Draw a single pixel. pix = size (1-8). e.g. POINT(50,50,0,5,0)

RPOINT(x, y, w, h, color, pix, type, count)
  Draw `count` random pixels inside rect (x,y,w,h).
  e.g. RPOINT(1,1,210,80,0,2,2,50)

LINE(x1, y1, x2, y2, color, pix, type)
  pix = thickness (1-8). type: 0=solid, 1=dashed.
  e.g. LINE(125,1,125,104,0,1,1)

RECT(x1, y1, x2, y2, color, pix, type)
  type: 0=outline only, 1=filled. e.g. RECT(1,1,120,18,0,1,1)

CIRCLE(x, y, r, color, pix, type)
  e.g. CIRCLE(60,60,30,0,1,1)

IMG(img_id, inv_flag)
  Show one of 7 stored images (id 0-6). inv_flag currently unused.
  e.g. IMG(0,0)

ICON(x, y, icon_id, font_id, fore_color, back_color, scale=1)
  icon_id 0-255, drawn from a separate icon font/glyph set (font_id 99 = built-in
  thermometer icon in the sample templates). e.g. ICON(190,20,0,99,0,1)

CLOCK(x, y, rad_h, rad_m, pix_h, pix_m, color, type)
  Analog clock HANDS ONLY (draw your own face via IMG/ICON/CIRCLE). type: 0=12h, 1=24h.
  "Currently only implemented in the 2.13" firmware."
  e.g. CLOCK(185,60,20,25,5,3,0,0)

TABLE(x, y, x_w, y_h, x_count, y_count, color, pix, type)
  Grid/table. type: 0=solid line, 1=dashed. Requires firmware >= 1.40.
  "Currently only implemented in the 2.13" firmware."
  e.g. TABLE(2,3,30,14,7,7,0,1,1)

DATE_OFF(day)
  Offsets the *displayed* date by `day` days (doesn't change RTC). DATE_OFF(0) to reset.

TIME_OFF(sec)
  Same idea but seconds; carries over into date if it crosses midnight.

ROTATE(rotate)
  0/1/2/3 = 0°/90°/180°/270°. Default 270. 90°/270° = tag mounted landscape.

MIRROR(mirror)
  0=none, 1=X-axis, 2=Y-axis, 3=both. Default 2.

SRAND(r)
  Reset the PRNG seed (e.g. SRAND({H}) to get the same "random" sequence at the same
  hour every day). Firmware >= 1.51.

RANDS(index, count, min, max, type)
  Bulk-fill `count` memory variables starting at `index` with random/sequential values
  in [min,max]. `type` is a bitfield: bit0 random(0)/sequential(1) assignment,
  bit1 unsorted(0)/sorted(1), bit2 ascending(0)/descending(1), bit3 allow(0)/forbid(1)
  duplicates. e.g. RANDS(0,5,1,36,10) — 10=0b1010 → random, sorted ascending, no dupes.

LET(index, data)
  Assign one memory variable. e.g. LET(0,{H})

SHOW(flag)
  Gates whether *subsequent* draw commands render, until the next SHOW(). Supports
  comparison expressions (>=1.55): = == != <> >= > <. Variables usable on either side.
  e.g. SHOW({w}==1) // only draws following commands on Mondays

INV(flag)
  Global inverted-display flag. 0=normal, 1=inverted.

CLEAR(color)
  Full-screen clear to color (0=black, 1=white).
```

**4.2" panel only** (not applicable to the 2.13" tags you have, included for completeness
since the firmware string table hinted at a broader shared codebase):
`CALC/CALu/CALj/CALc/CALs/CALb/CALws/CALwb(x,y,g_x,g_y,font_id,fore,back,show_flag)` —
lunar-calendar-aware calendar variants (solar terms, heavenly stems/earthly branches,
five-elements annotations) — and `REGION(x,y,w,h)` for partial-refresh region hints
(up to 10 regions, clock mode only).

**Note**: `EPD(` — found in the firmware's string table and dispatcher (§5, "Tier A") —
is *not* in the vendor's own public function list. It's almost certainly an
internal/diagnostic command (panel self-test or raw passthrough), not part of the
documented API.

### Variables (usable inside `{}` in `FONT`/`SHOW`/coordinate expressions)

Numeric: `{g}`/`{u}` (last-sync / current Unix-ish timestamp, epoch = 2000-01-01),
`{y}{m}{d}{H}{N}{S}` (date/time fields), `{Z}`/`{z}` (timezone, -12..12), `{b}`/`{B}`
(battery ADC 0-2046 / voltage 0-3.6V), `{c}`/`{C}` (panel temp ADC / °C), `{r}N` (random
0..N), `{R}N` (read memory var N), `{ly}{lm}{ld}` (lunar y/m/d indices), `{w}`/`{W}`
(weekday number/name).

String: `{mac}`/`{MAC}` (device MAC, lower six hex / full colon-separated upper),
`{VER}` (firmware version string), `{GKD}`/`{GKd}` (days-to-gaokao, string/number),
`{ls}{lb}{ly}{lm}{ld}{lT}` (lunar stem/branch/zodiac/month-name/day-name/next-solar-term),
`{h<X><n>}` countdown/birthday slots (up to 8): X ∈ {t,a,n,m,d} (name/age/days-left/
month/day), uppercase = stored order, lowercase = re-sorted by days-remaining, n = 1-8.

**Expressions**: coordinates and numeric variables support basic arithmetic, e.g.
`RECT(198,{-c+70},199,70,0,1,1)` (battery-gauge-style bar height driven by `{c}`),
`FONT(10,24,2,42,0,1,'洛杉矶 {H+9%24}:{N}')` (second-timezone clock),
`IMG({w},0)` (weekday-indexed image). Numeric text fields (`ymdHNC`) accept a
printf-style width suffix, confirmed from a real downloaded template:
`FONT(0,10,2,6,0,1,'{H:02d}')`.

### Dispatcher location in the existing decompilation

`tmp/output/5_hema_clock_down_high_V1.57.bin.c`, around line 6580
(`FUN_04008230`/`FUN_040083b4`), sequential prefix-compares (`FUN_04003df4`, a
`strncmp`-style primitive) against the literal ASCII tokens above. Tier-B handlers
(`FUN_040092bc` for `CAL`, `FUN_04009946` for `RECT`, etc.) each decompile to a bare
tail-call into a shared function (`FUN_04003dc0`) — Ghidra couldn't recover the actual
register arguments (a known artifact when it misreads a tail-call `B`/`BX` as "does not
return"). That's no longer a blocker for *using* the language (§4 above is now the
authoritative source), only for verifying the firmware's internal implementation
byte-for-byte if that's ever needed.

## 5. Real example templates (ground truth, downloaded from the vendor site)

The vendor's site ships downloadable default templates per resolution (14 low-res +
11 high-res: 4 clock layouts, 2 calendar layouts, 8 image-slot layouts per resolution,
roughly). One low-res clock template is quoted below as a short illustration of the
template syntax:

```
0
FONT(72,0,0,6,0,1,':')
FONT(0,10,2,6,0,1,'{H:02d}')
FONT(104,10,2,6,0,1,'{N:02d}')
FONT(5,1,0,1,0,1,'{y:04d}-{m:02d}-{d:02d} {W}')
FONT(150,0,0,0,0,1,'{VER}')
RECT(189,8,191,14,0,1,1)
RECT(192,6,210,18,0,1,1)
FONT(192,4,0,0,1,0,'{B}')
FONT(150,12,0,0,0,1,'{mac}')
ICON(190,20,0,99,0,1)
RECT(198,{-c+60},199,60,0,1,1)
FONT(192,70,0,0,0,1,'{c}C')
FONT(1,87,0,1,0,1,'{ls}{lb}{ly}年{lm}{ld}{lT}')
```
(A couple of Chinese characters render as mojibake above because the file is GB2312, not
UTF-8 — expected, matches the site's own warning about GB2312-only Chinese text.)

The leading `0` is presumably a per-template mode/flag byte, not itself a DSL command —
not yet confirmed against the parser.

## 6. Image encoding (from the vendor's own JS tools — `sec_dev/img2data.php`)

Fetched `js/dithering.js` and `js/utils.js` from the live site (now also copied into
`webpage/js/` locally so the existing `esl_clock.php`/`img2data.php`-style pages actually
render). This is the exact client-side algorithm used to turn a canvas image into the
byte stream sent over the image-upload BLE characteristic:

- **Dithering**: four selectable algorithms — none (flat threshold), 4×4 Bayer ordered
  dithering, Floyd–Steinberg, and Bill Atkinson's algorithm (the default-looking one,
  judging by branch order) — operating on a computed luminance channel
  (`0.299R + 0.587G + 0.114B`, standard NTSC luma weights).
- **Bit-packing** (`canvas2bytes()`): width rounded up to the next multiple of 8;
  processes row-major, one output byte per 8 horizontal pixels, **MSB-first**
  (bits are pushed into an array in scan order then joined and parsed as a base-2
  string, so the first pixel of each byte is the high bit); pixel value `1` = non-black
  (white), `0` = black.
- **Three-color (B/W/Red) panels**: a `bwrPalette` (black/white/red) exists alongside
  `bwPalette`, with a separate `ditheringCanvasByPalette()` using perceptual color
  distance (`getColorDistance`/`getNearColorV2`) to snap each pixel to the nearest
  palette entry, plus a red/black variant of Floyd-Steinberg (`bwr_floydsteinberg`).
  This confirms the site also serves 3-color 2.13" tags (see §8) — not what you have,
  but the same encoding pipeline family, so worth knowing the `'bwr'` code path exists in
  `canvas2bytes()` (packs based on pure-black-or-not per pixel; a full 3-color image
  needs two such bitplanes, black-plane and red-plane, sent separately — not confirmed
  from the JS alone, would need to watch an actual image-upload BLE transaction).

## 7. BLE GATT protocol (from `webpage/esl_clock.php`)

Three services:

- **Command service** (ASCII drawing commands, GB2312-encoded for Chinese text):
  service `00001f10-0000-1000-8000-00805f9b34fb`,
  characteristic `00001f1f-0000-1000-8000-00805f9b34fb`
- **Image upload service** (raw bitmap blit data, packed as in §6):
  service `13187b10-eba9-a3ba-044e-83d3217d9a38`,
  characteristic `4b646063-6264-f3a7-8941-e65356ea82fe`
- **Legacy Dialog SUOTA OTA service** (present, commented out/unused on the web page —
  this is Dialog's real registered SUOTA UUID, standard for SDK6 firmware):
  service `0000fef5-0000-1000-8000-00805f9b34fb`,
  characteristics `457871e8-d516-4ca1-9116-57d0b17b9cb2` and
  `8082caa8-41a6-4021-91c6-56f9b954cc34`

Device advertises with name prefix `NRF-<mac>` (leftover from a Nordic reference stack
naming convention, though the actual chip is Dialog DA14585).

## 8. Firmware version history & archive (downloaded, live from vendor site)

Pulled from the vendor's login-gated JLink download table:

| Version | Variant | Notes |
|---|---|---|
| V1.56 | low/high | Fixed a 24-solar-terms (节气) lunar-calendar display bug |
| V1.57 | low/high | **What you already had.** Added auto-refresh |
| V1.58 | high, "华夏表"/Huaxia-table | Adds a "Huaxia table" feature; separate activation codes; **file 404'd when downloaded this session** — may have been taken down/moved |
| V0.04 | JLink + SUOTA `.img` | Marked "free"; explicitly does **not** support diagonal-line drawing (斜点) — likely older/simpler codebase, notably smaller file size (229400 bytes vs. the standard 262144) |

Downloaded and saved locally to `firmware_archive/`:
`5_hema_clock_down_low_V1.56.bin`, `5_hema_clock_down_high_V1.56.bin`,
`4_hema_clock_update_low_V1.57.img` (SUOTA package, different header — see §1),
`hema_2.13_V0.04.bin`, `hema_2.13_V0.04.img`.

V1.56 vs. V1.57 (high-res) differ across a large span (offsets `0x2004`–`0x1257B`,
~52k differing bytes) — expected for a compiled/linked binary where inserting one
feature (auto-refresh) shifts most downstream addresses; not yet diffed at the
decompiled-function level to isolate the actual feature code. **Next step if useful**:
decompile V1.56 the same way as V1.57 and diff function-by-function — the smaller,
`V0.04` "free" firmware (missing diagonal-line support) may also be an easier starting
point for a first read-through, being presumably less feature-complete.

Also discovered (not downloaded, out of scope for your 2.13" B/W tags, noted for
completeness): the same vendor site also hosts firmware/tools for a **2.13" black/white/
**red** tri-color variant** (`./213b`), a 2.9" tri-color tag (`./290b`, currently hidden),
a 4.2" Hema tag (`./420hema`) and a similarly-named "老五" 4.2" tag (`./420laowu`), an
ESP32-C3 driver board (`./esp32c3_ink`) and e-book (`./esp32c3_ebook`), a WiFi base
station (`./wifi_base`), and a 5.65" 7-color panel (`./565f`) — this looks like an
actively maintained hobbyist ecosystem site, not the original abandoned developer's page,
though it clearly descends from / absorbed that project (same command language, same
`esl_clock.php`/`sec_dev/` structure, same firmware file-naming convention as your
original two `.bin` files).

## 9. Font library (from FAQ "默认字库说明")

Custom fonts are built with `PCtoLCD2002.exe` (a common Chinese LCD font tool), max 52KB
total, max 2048 glyphs (1024 if Chinese) per font ID. Built-in font IDs relevant to the
sample templates: `0`/`1` = Song 12×12/16×16 (calendar/zodiac text), `2`/`5` = Song 56×56
digits+colon, `3`/`6` = Song 84×54 / 84×84 digits+colon (large clock digits), `4` = Song
24×24, `41`/`42` = 40×40/24×24 timezone city names, `43` = 36×36 heavenly-stems/earthly-
branches, `44` = 16×16 five-elements glyphs. `font_id 99` used in sample templates for
`ICON()` is a *built-in* thermometer icon, per the official doc's own example.

## 10. GPIO pin tracing — attempted, inconclusive (own firmware now in progress instead)

Since the goal shifted from "drive the existing firmware" to "write independent
firmware" (the original factory firmware has verification that blocks arbitrary
control — that's *why* the community built a replacement in the first place), exact
GPIO pin parity with the community `.bin` stopped being strictly necessary. Still,
traced as far as static analysis would go:

- Installed Ghidra properly (`pacman -S ghidra`, now at `/opt/ghidra`) and re-imported
  the high-res `.bin` with a **correct** DA14585 memory map this time — RAM base
  `0x07FC0000`, header-stripped, `ARM:LE:32:Cortex` language — using
  `ghidra_project/high_v157_ram_image.bin` (see `ghidra_project/*.java` for the
  headless scripts used).
- One concrete win: the properly-based project's string search turned up the vendor's
  own support-site hostname embedded in the firmware itself (RAM `0x07fd0e04`) — direct
  confirmation that the live site found earlier really is this firmware's own
  authoritative home, not just a related-looking site.
- The specific claim from the first shallow decompilation — that `epd_init` calls out
  through two function-pointer callbacks (`DAT_040184b0`/`b4`) for cmd vs. data SPI
  writes — did **not** hold up. The pointer values read from that memory location
  (`0x07FC1EF9`/`0x07FC1F19`) don't correspond to valid Thumb code in either the file's
  raw bytes (all `0xFF`, erased-flash pattern) or Ghidra's own disassembler ("bad
  instruction data" / "unable to resolve constructor" at that address, even after
  forcing disassembly). Likely explanation: that region was mis-segmented as data by
  the original shallow (no-memory-map) Ghidra pass, and the "two callback pointers"
  narrative doesn't reflect the actual code structure. The SPI **command bytes**
  themselves (0x01, 0x0C, 0x11, 0x2C, etc. — literal immediates passed to whatever the
  real call structure is) are still solid, since Ghidra decodes immediate constants
  correctly regardless of surrounding function-boundary mistakes; only the specific
  "which two functions do the actual GPIO toggling" question stayed unresolved.
- Practical conclusion: further static GPIO-pin recovery has hit real diminishing
  returns without a live JTAG/SWD session against the physical chip (setting a
  watchpoint on the SPI/GPIO peripheral registers and single-stepping `epd_init` would
  settle this in minutes with real hardware, vs. hours of static-analysis guessing).
  **Recommended path**: physical continuity-test the EPD FPC connector pins back to the
  DA14585 package pins, cross-referenced against the DA14585 datasheet pinout (bundled
  in the SDK, see below) — see `firmware/hema_epd_clock/README.md` "Known unresolved
  issue" section.

## 11. Independent firmware project started

Given the factory-firmware verification issue, work has moved from "drive the existing
firmware" to writing genuinely independent firmware against Dialog/Renesas's official
SDK6 (self-registered and downloaded, since that requires accepting Renesas's own
license — not something automatable, and the reason the SDK is not vendored in this
repository). The firmware now lives at `firmware/hema_epd_clock/` — see its own `README.md` for what's
implemented (BLE peripheral + the two GATT services reusing the discovered UUIDs so the
existing `esl_clock.php` web tool keeps working + an SSD1680 driver + a 1bpp framebuffer
+ a 6-command subset of the DSL parser) versus what's still open (most of the DSL,
`{}` template variables, real fonts, GPIO pin confirmation, RTC). No code from the
community `.bin` is linked in — it was used only as a reference per this document.

## 12. Other still-open items

1. Decode the SUOTA `.img` header fully (§1) if OTA-compatible updates matter later.
2. Function-level diff of V1.56 vs V1.57 vs the "free" V0.04 to isolate individual
   feature implementations (auto-refresh, diagonal-line support, solar-term calendar fix)
   — not needed for the from-scratch firmware, but useful if closer behavioral parity
   with the community firmware is ever wanted.
3. ~~Grab the Baidu-hosted Waveshare-derived STM32 reference source~~ — **done**
   (downloaded locally as `STM32-F103ZET6/`; not redistributed here). It's the genuine Waveshare
   `EPD_2IN13_V2` driver the vendor's FAQ named. `firmware/hema_epd_clock/src/epd/epd_ssd1680.c`
   now uses its verified 76-byte full/partial LUT tables and full/partial init sequences
   verbatim instead of the earlier all-zero placeholders, and confirms BUSY is
   active-high-while-busy. One real discrepancy worth knowing: the community DA14585
   `.bin`'s own `epd_init` sends only a 30-byte LUT via cmd `0x32` with different VCOM/
   dummy-line/gate-width tuning than Waveshare's 70-byte version — likely a simplified
   or re-tuned waveform specific to whichever exact panel batch the community developer
   had. The from-scratch firmware now uses Waveshare's proven values wholesale rather
   than mixing the two, since Waveshare's is verified-working for this exact panel
   family (STM32-F103ZET6/User/e-Paper/EPD_2in13_V2.c has the full write-up, including
   an English readme confirming the standard 8-pin EPD FPC signal order: VCC, GND, DIN,
   CLK, CS, DC, RST, BUSY — useful for the physical continuity test since it tells you
   what each FPC pad on the panel side *is*, even though the STM32 demo board's own
   port/pin numbers don't transfer to the DA14585).

## 13. GPIO pin map RECOVERED from firmware (resolves §10) — high confidence

The §10 "inconclusive" result is now **superseded**. Redoing the extraction with a
correctly memory-mapped Ghidra project (RAM base 0x07FC0000, header stripped) and
scanning for DA14585 GPIO/SPI peripheral-register constants (P0/P1/P2 DATA/SET/RESET/MODE
at 0x50003000-0x50003094, SPI_CTRL at 0x50001200) surfaced the pin-config function and,
crucially, the actual command-writer / data-writer / reset routines. Two independent
parts of the code agree, and the physical package pins line up (control signals on
adjacent QFN pins 7/8/9/10), so this is high-confidence — not a guess.

Key functions (RAM addresses in the corrected project):
- `FUN_07fc4f58` — SPI/EPD pin configurator. Calls a `GPIO_ConfigurePin(port,pin,mode,
  func,high)` helper with SPI function codes (7=SPI_CLK, 6=SPI_DO), selecting between two
  board variants via a config byte at `struct+0x11`.
- `FUN_07fd5f30` — command byte writer: sets P0_5 **low** (D/C=command) + pulses P2_1
  (CS) low/high around the SPI byte.
- `FUN_07fd5f50` — data byte writer: sets P0_5 **high** (D/C=data) + pulses P2_1 (CS).
- `FUN_07fd82e8` — epd_init: the reset sequence toggles P0_SET/P0_RESET bit 7 (P0_7 low
  then high) = EPD hardware reset.

**Variant B (config byte == 0) — the confirmed layout for the photographed board:**

| EPD signal    | DA14585 GPIO | QFN40 pin |
|---------------|--------------|-----------|
| SCK (clock)   | P0_0         | 1         |
| SDA (MOSI/DIN)| P0_6         | 9         |
| D/C           | P0_5         | 7         |
| CS            | P2_1         | 8         |
| RST           | P0_7         | 10        |
| BUSY (input)  | P2_0         | 40        |
| PWR-enable    | P2_3 (high)  | 18        |
| aux (unknown) | P2_2 (low)   | 13        |

**Variant A (config byte != 0):** SCK=P0_1(pin2), MOSI=P2_0(pin40), D/C=P0_7(pin10),
CS=P2_1(pin8), BUSY=P1_1(pin24). Scattered pins → less likely to be a real board layout;
kept as the fallback if a variant-B build shows nothing.

QFN40 pin numbers are from the DA14585 datasheet (rev 3.4) Figure 4. Note pins to NOT
confuse: physical pin 17 = chip RST (hardware reset of the SoC, not the EPD); SWDIO=pin25,
SW_CLK=pin26 are the J-Link/SWD debug pins (the two wires already soldered to the board's
test points are almost certainly these + power, for flashing).

Pin-sharing note: the DA14585 boot ROM's SPI-flash pins are CLK=P0_0, MISO=P0_5,
MOSI=P0_6, CS=P0_3. The board reuses **P0_5 as both flash-MISO and EPD-D/C** — the stock
firmware time-shares it because the flash (U3, the 8-pin SOIC) and the panel have separate
chip-selects (flash CS=P0_3, EPD CS=P2_1). CLK (P0_0) and MOSI (P0_6) are a genuinely
shared SPI bus. For write-only EPD firmware, MISO isn't needed — just don't let the SPI
driver's DI pad reclaim P0_5.

The from-scratch firmware (`firmware/hema_epd_clock/`) now hardcodes variant B in
`src/epd/epd_ssd1680.h` (DC/RST/BUSY + a P2_3 power-enable) and `src/config/user_periph_setup.h`
(CS=P2_1, CLK=P0_0, MOSI=P0_6). Photographed panel is **HINK-E0213A53-FPC-A0 = high-res
122x250**, so build with `EPD_PANEL_HIGH_RES` defined.

### 13a. Pin map PHYSICALLY CONFIRMED by continuity test (2026-07-25)

User continuity-tested the photographed board (HINK-E0213A53-FPC-A0, high-res). Probing
each recovered DA14585 pin to the U4 display connector (pads numbered from the pad nearest
the "U4" silkscreen = pad 1) gave six distinct, CONSECUTIVE pads — textbook e-paper
control block, fully confirming the variant-B map:

| U4 pad | signal | DA14585 pin |
|--------|--------|-------------|
| 11     | SDA/MOSI | P0_6 (pin 9)  |
| 12     | SCK      | P0_0 (pin 1)  |
| 13     | CS       | P2_1 (pin 8)  |
| 14     | D/C      | P0_5 (pin 7)  |
| 15     | RST      | P0_7 (pin 10) |
| 16     | BUSY     | P2_0 (pin 40) |

(U4 pads 1-10 and 17-24 are the panel's power/booster rails — VDD/VSS/VGH/VGL/VSH/VSL/
VCOM and charge-pump cap nets, matching the C14-C22 cluster by the connector — not driven
by the MCU.) The firmware's variant-B pin defines are therefore correct as-is; no change
needed. Board is confirmed variant B, high-res.
