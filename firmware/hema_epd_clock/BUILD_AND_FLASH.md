# Building & flashing hema_epd_clock (e² studio + J-Link)

Goal: get the firmware onto your tag and see the boot **test pattern** on the
e-paper — proving the whole SPI/GPIO/driver path end-to-end, before any BLE.

This is written for a beginner. Read it top to bottom once before starting.

---

## 0. The big picture (read this first)

- The DA14585 has **no internal flash**. It boots by copying code from the
  external SPI flash (chip **U3**) into RAM and running it there.
- For bring-up we do **NOT** touch that flash. We use the J-Link debugger to
  **download our code straight into RAM and run it**. This is:
  - **Fast** — rebuild, re-download, repeat in seconds.
  - **Non-destructive & reversible** — your original community firmware stays
    in the SPI flash untouched. Power-cycle the board *without* the debugger
    and it boots the old firmware again. You only overwrite the flash later,
    deliberately, once you're happy (Section 7).
- So the first goal is just: **RAM-download our build and see the test pattern.**

---

## 1. What you need

- A **J-Link** (SEGGER) probe. A genuine J-Link or an official Renesas dev
  board's on-board J-Link both work. (Cheap "J-Link OB" clones usually work too
  for SWD.)
- **4 jumper wires** from the J-Link to the board: SWDIO, SWCLK, GND, and a
  3.3 V reference (VTref).
- A way to **power the board** during flashing: either its battery installed,
  or a bench 3.3 V onto the battery rail. (SWD does not power the board.)
- e² studio, with the DA145xx SDK6 (`SDK_6.0.22.1401`) downloaded from Renesas
  and extracted somewhere. Referred to below as `$SDK`, meaning the directory
  that contains `projects/` and `sdk/` (i.e. `.../DA145xx_SDK/6.0.22.1401`).
- **SEGGER J-Link software** installed *inside the VM*.

### VM note (important)
e² studio is in a VM, but the J-Link is a USB device on the host. You must
**pass the J-Link USB device through to the VM**:
- VirtualBox: Devices → USB → check "SEGGER J-Link", and add a USB filter so it
  auto-attaches.
- VMware: VM → Removable Devices → SEGGER J-Link → Connect.
Then install the J-Link drivers/software **in the VM**. Confirm it's seen by
opening "J-Link Commander" (JLinkExe) in the VM — it should enumerate the probe.

---

## 2. Wire up the J-Link to the board

Your board already has two wires soldered to the test-point row (bottom edge).
Those are almost certainly **SWDIO** and **SWCLK** — but verify, don't assume.

**Identify the SWD pins** (battery out, multimeter in continuity mode, same
technique you already used):
- The DA14585 (chip **U2**) SWD pins are **SWDIO = package pin 25** and
  **SW_CLK = package pin 26** (right-hand edge of the QFN, near the middle).
- Beep each existing soldered wire to pin 25 and pin 26 to label which is which.
- Find a **GND** test point (beeps to the battery negative / ground pour).
- Find a **3.3 V** point (with the battery in and powered: a pad that reads
  ~3.0–3.3 V to GND — often the battery + terminal or a regulator output).

**Connect:**

| J-Link pin | → board |
|------------|---------|
| SWDIO (pin 7 on the 20-pin JTAG header) | SWDIO wire (DA14585 pin 25) |
| SWCLK (pin 9) | SWCLK wire (DA14585 pin 26) |
| GND (pin 4, 6, 8…) | board GND |
| VTref (pin 1) | board 3.3 V rail |

Then power the board (battery in). VTref lets the J-Link sense the 3.3 V logic
level — without it, connection often fails with a voltage/target error.

---

## 3. Put the project where the SDK expects it

SDK6 projects reference the SDK by **relative paths**, so the project must live
*inside* the SDK tree. Copy this project into the SDK's template folder:

```
cp -r firmware/hema_epd_clock \
      "$SDK/projects/target_apps/template/hema_epd_clock"
```

(Do all your editing/building on that copy from here on, or keep editing the
original and re-copy — your choice, just don't lose track of which is which.)

---

## 4. First: validate your toolchain with a known-good example

Before fighting with our custom project, prove that **e² studio + J-Link +
your board** all work, using an SDK example that already has an e² studio
project. `prox_reporter` is the one that ships with e² studio files.

1. e² studio → **File → Import → General → Existing Projects into Workspace**.
2. Browse to
   `…/6.0.22.1401/projects/target_apps/ble_examples/prox_reporter/e2studio`,
   import it.
3. Select the **DA14585** build target and **Build** (hammer icon). It should
   compile with no errors. *If it doesn't build, stop here and fix the
   toolchain/SDK setup first — nothing else will work until this does.*
4. **Download to RAM — use J-Link directly, not the SDK's .launch file.**

   > **Do not** try to import `config/RAM_DA14585.launch` into stock e² studio.
   > That file is launch type `com.dialog.SmartBond_680_jlink` with
   > `ilg.gnumcueclipse.*` attributes — it belongs to the **GNU MCU Eclipse +
   > Dialog SmartBond** plugins that ship in *SmartSnippets Studio*, not in
   > e² studio. It also references a GNU-ARM build-config ID that doesn't exist
   > in the Renesas-LLVM `.cproject`. Symptoms if you try: there is no
   > **MCU** page under Window → Preferences, no "SmartBond 680 J-Link"
   > category in Debug Configurations, and the imported launch is unresolvable.

   Since the build already produces a good `.elf`, drive J-Link directly —
   nothing to install beyond the J-Link package itself:

   ```bash
   JLinkExe -device Cortex-M0 -if SWD -speed 4000 -autoconnect 1 \
            -CommanderScript tools/ram_load.jlink
   ```

   `tools/ram_load.jlink` (in this repo) does the DA14585-specific dance:
   `loadfile` the ELF, **remap address 0 to SysRAM**, then set MSP/PC and `g`.
   Edit the `loadfile` path inside it to point at your build output.

   **Order matters, and it's counter-intuitive.** `loadfile` performs an
   *implicit reset & halt*, and any reset clears the remap. So the remap writes
   must come **after** `loadfile`, not before. If you do them first they are
   silently undone and the chip simply re-boots the old SPI-flash firmware —
   which looks like "nothing happened".

   Cortex-M0 has **no VTOR register**, so the vector table must physically live
   at address 0. That's why the remap is mandatory rather than cosmetic:
   without it every interrupt vectors into ROM, and BLE is interrupt-driven.

   After `loadfile`, read the vector table (`mem32 0x07FC0000, 2`): word 0 is
   the initial MSP, word 1 is the reset handler with bit 0 set as the Thumb
   flag. Write those into `MSP` and `R15` (clearing bit 0) before `g`. These
   values change whenever the image changes, so re-read them per build.

   Setting the PC is **mandatory, not optional**. `loadfile` resets the core
   *before* it downloads, so the core latches MSP/PC from whatever image was
   already in RAM — usually the stock firmware. After the download those values
   point into the middle of a different program. Running from there crashes,
   the watchdog resets the chip, and the boot ROM reloads the SPI-flash
   firmware. The visible symptom is the panel refreshing with the *original*
   UI, which is easily misread as "the load didn't happen" — the load was fine,
   the jump wasn't.

   Older J-Link OB firmware rejects both `wreg PC` and `wreg R15` (its own help
   text lists "R15 (PC)" but neither spelling parses). Use the dedicated
   **`SetPC <addr>`** command instead. If your J-Link build lacks `SetPC`, the
   fallback is to issue `r` then `h` *after* `loadfile` — the core then reloads
   MSP/PC from the newly-written vector table — and re-apply the four `w2`
   writes afterwards, since the reset clears them.

   **Telling the two images apart:** the MSP/PC latched at reset is a reliable
   fingerprint, because each image has its own vector table. For this board,
   `PC=07FC0524 / MSP=07FCFF60` is the stock firmware, and
   `PC=07FC45C4 / MSP=07FD4578` is the SDK `prox_reporter` build.

   **Reading the PC is not a reliable check of which image is running.**
   `0x07F0xxxx` is ROM — and that's where the BLE stack legitimately lives, so
   a healthy SDK6 app spends most of its time there. Both our firmware and the
   stock firmware are SDK6-based and look alike by PC alone. Use an
   application-level signal instead: a BLE scan for the expected device name,
   or (for our firmware) the e-paper actually refreshing.
5. With a phone BLE scanner app, confirm you see the example advertising
   (`DLG-PROXR` or similar). If you do → **your whole chain works.** 🎉

Only once this works should you move on. This step turns "did I mess up the
firmware or the setup?" from a guess into a known.

---

## 5. Build OUR firmware

The base template (empty_peripheral_template) ships **Keil-only**, so this repo
now carries a ready-made `e2studio/` project, derived mechanically from the
`prox_reporter` one you just validated. **No GUI grafting required.**

### 5.1 Put it where the paths resolve

The project references sources with Eclipse `PARENT-N-PROJECT_LOC` links:
`PARENT-1` must be the project folder and `PARENT-5` must be the SDK root.
That works out **only** at the same tree depth prox_reporter sits at, so copy
the whole folder to:

```
<SDK>/projects/target_apps/template/hema_epd_clock
```

(`template/…` is the same depth as `ble_examples/…`, so the links transfer
unchanged.) Anywhere else and every SDK include silently fails to resolve.

### 5.2 Import and build

1. **File → Import → General → Existing Projects into Workspace**, browse to
   `<SDK>/projects/target_apps/template/hema_epd_clock/e2studio`.
2. Select the **DA14585** build configuration.
3. **Build.**

### What the generated project already contains

Beyond prox_reporter's SDK file set, it adds — all five build configurations
kept in sync:

- our sources in virtual folders `user_app`, `user_config`, `user_platform`,
  plus **`user_profile`** (`src/custom_profile/*`) and **`user_epd`**
  (`src/epd/*` — the display driver);
- the SDK sources the **custs1** custom profile needs, which prox_reporter
  does not compile: `app_customs.c`, `app_customs_common.c`,
  `app_customs_task.c`, `custom_common.c`, `custs1.c`, `custs1_task.c`;
- **`attm_db_128.c`** — `custs1.c` calls `attm_svc_create_db_128()` to build a
  128-bit-UUID service database. prox_reporter uses only 16-bit SIG profiles,
  so it never links this one;
- **`systick.c`** — `systick_wait()`, used for the EPD driver's delays;
- include paths `../src/epd` and `../src/custom_profile`;
- matching entries in the linker's **Linkage Order List** — easy to miss, and
  the failure mode is confusing: the files compile fine but the link fails with
  undefined references;
- `user_proxr.c` and its object entry removed.

> Note: `gpio.h`/`spi.h` "file not found" markers in a plain editor are just
> because those live in the SDK — inside the project they resolve fine.

---

## 6. Flash to RAM and look at the screen

1. Load into RAM exactly as in Section 4 — same `tools/ram_load.jlink` script,
   just point its `loadfile` line at `hema_epd_clock.elf` instead.
2. Within a second or two, the panel should do a full refresh (~2 s, flashing
   black/white) and settle showing the **test pattern**: a border, two
   diagonals, a filled + an outline rectangle, a circle, and `0123456789`
   in a black strip near the bottom.

**Reading the result:**

| What you see | Meaning / fix |
|--------------|---------------|
| Correct test pattern | 🎉 Everything works — pins, SPI, driver, LUTs all good. |
| Pattern but **inverted** (white shapes on black) | Pixel polarity flipped. In `src/epd/epd_ssd1680.c` set `EPD_INVERT_OUTPUT` to `0`, rebuild, re-download. |
| Panel flashes/refreshes but stays **blank/all-black/all-white** | SPI is probably reaching the panel but data/mode is off. Double-check `EPD_DC`/`EPD_CS` pins and that `EPD_PWR` (P2_3) is asserted. Also try the partial-vs-full path. |
| **Nothing at all** (no refresh flash) | Panel not being driven. Recheck CS/CLK/MOSI wiring vs. the recovered pins; verify BUSY (P2_0) polarity — if `epd_wait_busy` returns instantly or never, BUSY may be inverted. As a last resort try the "variant A" pin set (see `epd_ssd1680.h`). |
| Garbled / shifted image | Likely wrong panel size. Confirm it's built high-res (default) — your panel is HINK-E0213A53 = 122×250. |

Iterate here freely — every retry is just another RAM download, and your
original firmware is still safe in the flash.

---

## 7. (Later) Make it permanent — burn to SPI flash

Only once the RAM test looks right, and once the BLE/command side is doing what
you want, write the image to the external SPI flash so it boots standalone:

- Use Renesas **SmartSnippets Toolbox** → **SPI Flash Programmer**. Point it at
  your built image, let it detect the flash (chip U3), and program. It writes
  the AN-B-001 boot header + your image so the boot ROM loads it on power-up.
- This **overwrites the community firmware**. Before you do, consider keeping a
  copy: SmartSnippets can *read back* the current flash contents to a file
  first — a good idea so you can restore the original if you ever want to.

That's the whole loop. First target is just Section 6 showing the test pattern.
Once you're there, the hard bring-up is done and everything else is software.
