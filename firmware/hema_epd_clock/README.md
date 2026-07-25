# hema_epd_clock — from-scratch DA14585 firmware for the Hema 2.13" ESL tag

Independent firmware for the decommissioned Hema (盒马) electronic shelf label,
built against Renesas/Dialog's official DA1458x SDK6 rather than the
community `.bin`. The community binary and its companion web tool were used
only as a **reference** (reverse-engineered protocol/display driver, see
`../PROTOCOL_NOTES.md`) — no code or binary data from it is linked into this
project.

## What's implemented

- BLE peripheral advertising as `HemaEPD-Clock` (`src/config/user_config.h`)
- Two custom GATT services, reusing the UUIDs discovered from the vendor's
  own `esl_clock.php` web tool, so that tool works against this firmware
  unmodified (`src/custom_profile/user_custs1_def.{h,c}`):
  - Command service (`00001f10`/`00001f1f`) — ASCII drawing commands
  - Image service (`13187b10-...`/`4b646063-...`) — raw framebuffer upload
- SSD1680 e-paper controller driver (`src/epd/epd_ssd1680.{h,c}`) — init
  sequence, full-refresh display update, sleep
- 1bpp framebuffer + drawing primitives matching the vendor's own pixel
  packing (`src/epd/epd_gfx.{h,c}`) — `CLEAR`/`POINT`/`LINE`/`RECT`/`CIRCLE`
  plus a minimal built-in fallback font for `FONT()`
- ASCII command parser for the above six commands
  (`src/epd/epd_cmdparser.{h,c}`)
- SPI + GPIO peripheral wiring (`src/platform/user_periph_setup.c`)

## What's NOT implemented yet

See `PROTOCOL_NOTES.md` section 4 for the full vendor command reference.
Not yet built:
- `CAL`, `CLOCK`, `TABLE`, `IMG`, `ICON`, `ROTATE`, `MIRROR`, `SHOW`, `INV`,
  `LET`, `SRAND`, `RANDS`, `DATE_OFF`, `TIME_OFF`
- `{}` template-variable substitution ({y}{m}{H}{N}{c}{b} etc.) — needs an
  RTC/battery-ADC/temperature-ADC read plus a small expression evaluator
- The vendor's own font/icon glyph format (font_id 0-44, see
  `PROTOCOL_NOTES.md` section 9) — currently a single fixed 5x7 fallback font
- SUOTA OTA update service — flash via J-Link only for now
- Real-time clock keeping (no RTC read/set wired up at all yet)

## Hardware status: verified

This firmware has been built with the Renesas LLVM ARM toolchain in e² studio,
downloaded into SysRAM over SWD with a J-Link, and confirmed to run on a real
tag: it drives the panel and renders its boot test pattern correctly.

### Pin map (confirmed)

Recovered from the community firmware, then continuity-tested from the EPD FPC
connector back to the DA14585 package, then proven by driving the panel.
Defined in `src/epd/epd_ssd1680.h`; every one of them must also be declared in
`GPIO_reservations()` (`src/platform/user_periph_setup.c`) — see the gotcha
below.

| Signal | GPIO  | QFN40 pin | Notes                          |
| ------ | ----- | --------- | ------------------------------ |
| SCK    | P0_0  | 1         | hardware `PID_SPI_CLK`         |
| MOSI   | P0_6  | 8         | hardware `PID_SPI_DO`          |
| CS     | P2_1  | 8         | **plain GPIO**, not `PID_SPI_EN` |
| DC     | P0_5  | 7         | plain GPIO                     |
| RST    | P0_7  | 10        | plain GPIO                     |
| BUSY   | P2_0  | 40        | plain GPIO input, active-high while busy |
| PWR    | P2_3  | 18        | plain GPIO, panel power enable |

CS is driven directly with `GPIO_SetActive`/`GPIO_SetInactive` rather than the
SPI block's hardware `SPI_EN` function, because the driver needs to hold CS low
across a multi-byte command+data burst.

[`GPIO_TRACING_GUIDE.md`](GPIO_TRACING_GUIDE.md) documents the continuity-test
method used, in case you need to redo it for a different board revision.

## Building and flashing

Full walkthrough in [`BUILD_AND_FLASH.md`](BUILD_AND_FLASH.md). Short version:

1. Download SDK6 (`SDK_6.0.22.1401`) from Renesas and unpack it. It is not
   redistributable, so it is not vendored here.
2. Generate the e² studio project, which is derived from the SDK's own
   validated `prox_reporter` project:
   ```sh
   DA1458X_SDK=~/DA145xx_SDK/6.0.22.1401 python3 tools/gen_e2studio_project.py
   ```
3. Copy this whole project directory to
   `<SDK>/projects/target_apps/template/hema_epd_clock/` — the generated
   `.project` uses `PARENT-N-PROJECT_LOC` relative links, so the depth in the
   SDK tree matters.
4. Import into e² studio and build the `DA14585` configuration.
5. Load into RAM over SWD (nothing is written to flash, so the stock firmware
   stays intact and a power cycle fully recovers the tag):
   ```sh
   JLinkExe -device Cortex-M0 -if SWD -speed 4000 -autoconnect 1 \
            -CommanderScript tools/ram_load.jlink
   ```

## Gotchas worth knowing

These each cost real debugging time:

- **Unreserved GPIO looks like a hang.** Under `DEVELOPMENT_DEBUG` the SDK's
  pin-allocation monitor runs `__BKPT(0)` on the first pin passed to
  `GPIO_ConfigurePin()` that wasn't declared via `RESERVE_GPIO()` in
  `GPIO_reservations()`. With a debugger attached the core just halts, so the
  symptom is firmware stuck at a fixed PC — easily misread as a driver or
  wiring fault. Resolve a halted PC with
  `llvm-symbolizer --obj=hema_epd_clock.elf <pc>` before suspecting hardware.
- **`loadfile` resets the core *before* downloading**, so it latches MSP/PC
  from whatever image was previously in RAM. `tools/ram_load.jlink` resets a
  second time *after* the download so the core re-latches from the new vector
  table; that also makes the script build-independent (no hardcoded entry
  address to update each rebuild).
- **Cortex-M0 has no VTOR**, so the address-0 → SysRAM remap
  (`SYS_CTRL_REG = 0xA2`) is mandatory, not cosmetic. It lives in the
  always-on domain and survives `SYSRESETREQ`, which is what makes the second
  reset safe.
- **Freeze the watchdog** (`SET_FREEZE_REG`) before running, or a fault
  reboots the tag into the stock SPI-flash firmware and destroys the evidence.
