#!/usr/bin/env bash
#
# flash.sh - build a full SPI-flash image and program it over SWD.
#
#   tools/flash.sh --type <n> <hema_epd_clock.bin> [bank]
#   tools/flash.sh --variant <a|b> <stock_dump.bin> <hema_epd_clock.bin> [bank]
#
# Wraps the two steps that actually put firmware on the tag: mksuota.py to wrap
# the raw linker .bin in a SUOTA image bank, and J-Link Commander to program it.
# See README.md for why a raw .bin at offset 0 does not boot on this board.
#
# The secondary bootloader picks the *newest valid* of two image banks, so
# writing bank 1 and leaving the stock image in bank 2 means a bad build falls
# back to something that works rather than bricking the tag. That fallback is a
# real feature of the bootloader, not a convention: on a CRC failure it loads
# and verifies the other bank before giving up.
#
# That bootloader lives in OTP. Types 1, 3 and 4 were each dumped on
# 2026-08-12 and all three carry OTP_HDR_OTP_CONTROL = 0xC0DEBABE and a
# byte-identical OTP bootloader, so the AN-B-001 image sitting at flash
# 0x000000 is present but never runs. TAG_VARIANTS.md still says Types 2/3/4
# boot from offset 0; that is wrong for 3 and 4, and untested for 2. The
# bootloader has been disassembled - the exact contract, including the
# wraparound rule for imageid, is in
# hema-local/docs/BOOT_CONTRACT.md
#
# Say which tag this is and everything else follows. --type takes the number
# from hema-local/docs/TAG_VARIANTS.md and picks that type's stock dump; the
# wiring is then read out of the image rather than typed, because the image
# knows. The older --variant form still works for an image built before the
# type stamp existed, and needs the dump named by hand.
#
# One of the two is mandatory and there is no default. Flashing a board with
# the other variant's wiring is the worst kind of wrong: the tag boots,
# advertises and takes connections exactly as normal, and only the panel stays
# dead - so it presents as a broken screen, not as a bad flash. It has cost a
# working tag twice, in both directions, and neither time was diagnosed from
# the symptom. A default is what let that happen by omission.
#
# Saying it is only half of it, since a typed number can be as wrong as a
# default. The firmware stamps its type, its wiring and its panel geometry into
# its own image, so what you said is checked against the binary about to be
# written and a mismatch stops the flash.
#
# The stock dumps live outside this repo (they are vendor images). Point
# HEMA_STOCK_DIR at them if they are not in ../hema-local/re.
#
# Requires the community J-Link device definition for the DA14585 QSPI bank
# (JLinkDevices.xml + Devices/jtag_programmer.axf in /opt/SEGGER/JLink) -
# without it `device DA14585` exposes no flash bank and loadbin silently has
# nowhere to write.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)          # tools/, where mksuota.py lives
STOCK_DIR=${HEMA_STOCK_DIR:-$(cd "$HERE/.." && pwd)/../hema-local/re}

usage() {
    cat >&2 <<'EOF'
usage: flash.sh --type <n> <hema_epd_clock.bin> [bank]
       flash.sh --variant <a|b> <stock_dump.bin> <hema_epd_clock.bin> [bank]

  --type <n>       which tag this is (see hema-local/docs/TAG_VARIANTS.md).
                   Picks that type's stock dump and checks the image was built
                   for it. Pass a dump before the firmware to override.
  --variant <a|b>  the older form: name the wiring and the dump by hand.
  --speed <kHz>    SWD clock, default 4000. Lower it (1000, or less) if a
                   flash fails verifying RAMCode by only a bit or two - that
                   is the link, not the target. Or set HEMA_SWD_SPEED.
  --unverified     flash an image that carries no stamp to check.
  --no-fallback    build the image from scratch instead of on top of a stock
                   dump. Removes the need for one, at the cost of the
                   known-good image in the other bank. Types 1/3/4 boot from
                   OTP so nothing else is needed; a Type 2 has never been
                   dumped, so it also needs --bootloader.
  --bootloader <f> the secondary bootloader for flash offset 0. Only needed
                   with --no-fallback on a board whose boot chain is unverified.
EOF
}

VARIANT=
TYPE=
UNVERIFIED=0
NO_FALLBACK=0
BOOTLOADER=
# The secondary bootloader payload, identical on every retail tag dumped so far
# (types 1, 2, 3 and 4) - the AN-B-001 record at flash offset 0 with its 8-byte
# header stripped. re/type2/bootloader.bin is a correct copy.
RETAIL_BOOT_SHA=c3220667e31492ae8ac5a77a5ddbf98ace65505e98094aeb63362922ef57c491
# SWD clock. 4 MHz is fine over a short, well-soldered link and is what this
# used to hardcode. Drop it when the probe is a J-Link OB clone, when the wires
# are long, or after a "Verification of RAMCode failed" whose write and read
# differ by only a bit or two - that is signal integrity, not the SysRAM
# contention the error message suggests, and it usually clears at 1000 or below.
SPEED=${HEMA_SWD_SPEED:-4000}
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --variant)     VARIANT=${2:-}; shift 2 ;;
        --variant=*)   VARIANT=${1#*=}; shift ;;
        --type)        TYPE=${2:-}; shift 2 ;;
        --type=*)      TYPE=${1#*=}; shift ;;
        --speed)       SPEED=${2:-}; shift 2 ;;
        --speed=*)     SPEED=${1#*=}; shift ;;
        --unverified)  UNVERIFIED=1; shift ;;
        --no-fallback) NO_FALLBACK=1; shift ;;
        --bootloader)  BOOTLOADER=${2:-}; shift 2 ;;
        --bootloader=*) BOOTLOADER=${1#*=}; shift ;;
        -h|--help)     usage; exit 2 ;;
        -*)            echo "flash.sh: unknown option $1" >&2; exit 2 ;;
        *)             args+=("$1"); shift ;;
    esac
done

# Uppercase once here so the comparison below is against one canonical form,
# and so the message can echo back what was actually meant.
VARIANT=$(printf '%s' "$VARIANT" | tr '[:lower:]' '[:upper:]')

if [ -n "$TYPE" ]; then
    case "$TYPE" in
        ''|*[!0-9]*) echo "flash.sh: --type takes a number, not '$TYPE'." >&2
                     exit 2 ;;
    esac
    # A dump given explicitly wins; otherwise it follows from the type. Getting
    # this wrong matters more than it looks: the bank offsets differ between
    # Type 1 and the rest, and mksuota.py reads them out of whichever dump it
    # is handed.
    if [ ${#args[@]} -ge 2 ]; then
        STOCK=${args[0]}; FW=${args[1]}; BANK=${args[2]:-1}
    elif [ ${#args[@]} -eq 1 ]; then
        STOCK=$STOCK_DIR/type$TYPE/stock_flash_512k.bin
        FW=${args[0]}; BANK=1
    else
        usage; exit 2
    fi
    if [ $NO_FALLBACK -eq 1 ]; then
        STOCK=
    elif [ ! -r "$STOCK" ]; then
        echo "flash.sh: no stock dump for type $TYPE at" >&2
        echo "          $STOCK" >&2
        echo "          Set HEMA_STOCK_DIR, or name the dump before the" >&2
        echo "          firmware. Do not substitute another type's - it is" >&2
        echo "          the fallback image the tag boots if this build is" >&2
        echo "          bad, so it must be one that suits this board." >&2
        echo "          Or pass --no-fallback to build without one." >&2
        exit 1
    fi
elif [ ${#args[@]} -ge 2 ]; then
    case "$VARIANT" in
        A|B) ;;
        "")  echo "flash.sh: say which tag this is: --type <n>, or --variant" >&2
             echo "          <a|b> for an image with no type stamp." >&2
             echo "          Type 1 is B, Type 2 is A, Type 3 is A, Type 4 is B." >&2
             echo "          See hema-local/docs/TAG_VARIANTS.md if unsure -" >&2
             echo "          guessing costs a tag, and the symptom will not" >&2
             echo "          tell you." >&2
             exit 2 ;;
        *)   echo "flash.sh: --variant must be a or b, not '$VARIANT'." >&2
             exit 2 ;;
    esac
    STOCK=${args[0]}; FW=${args[1]}; BANK=${args[2]:-1}
else
    usage; exit 2
fi

OUT=$(mktemp -t hema-flash-XXXXXX.bin)
SCRIPT=$(mktemp -t hema-flash-XXXXXX.jlink)
LOG=$(mktemp -t hema-flash-XXXXXX.log)
trap 'rm -f "$OUT" "$SCRIPT" "$LOG"' EXIT

for f in ${STOCK:+"$STOCK"} "$FW"; do
    [ -r "$f" ] || { echo "flash.sh: cannot read $f" >&2; exit 1; }
done

# --------------------------------------------------------- image cross-check
# Read what the image says it is, rather than trusting what was typed. -a
# because the strings live in .rodata, not in a section `strings` would look at
# by default.
stamp() { strings -a "$FW" | grep -om1 "$1" || true; }
built_type=$(stamp 'HEMA-TAG-TYPE-[0-9]\+');      built_type=${built_type##*-}
built_var=$(stamp 'HEMA-BOARD-VARIANT-[AB]');     built_var=${built_var##*-}
built_panel=$(stamp 'HEMA-PANEL-[0-9x]\+');       built_panel=${built_panel#HEMA-PANEL-}
built_wave=$(stamp 'HEMA-WAVEFORM-[A-Z]\+');      built_wave=${built_wave#HEMA-WAVEFORM-}
# Present only in an image that carries the SUOTA service - see HEMA-SUOTA-1 in
# src/epd/epd_ssd1680.c. Absence is the signal, so this is a test for "", not a
# value to compare.
built_suota=$(stamp 'HEMA-SUOTA-[0-9]\+')

# Type 0 is what an image built outside the normal path is stamped with - see
# HEMA_TAG_TYPE_TAG in epd_ssd1680.h. It is not a tag, so treat it as unstamped
# rather than as a mismatch.
[ "$built_type" = 0 ] && built_type=

if [ -n "$TYPE" ]; then
    if [ -z "$built_type" ]; then
        if [ "$UNVERIFIED" != 1 ]; then
            echo "flash.sh: $FW carries no type stamp." >&2
            echo "          Every image from tools/build.sh has one, so this" >&2
            echo "          is either an older build or not our firmware, and" >&2
            echo "          there is no way to tell from here which tag it" >&2
            echo "          expects." >&2
            echo "          Rebuild it with tools/build.sh --type $TYPE, or" >&2
            echo "          pass --unverified to flash it anyway." >&2
            exit 1
        fi
        echo "flash.sh: WARNING - no type stamp in $FW; flashing unverified."
    elif [ "$built_type" != "$TYPE" ]; then
        echo "flash.sh: REFUSING - type mismatch, nothing was written." >&2
        echo "          you asked for  : type $TYPE" >&2
        echo "          image is built : type $built_type" \
             "(variant $built_var, $built_panel)" >&2
        echo >&2
        echo "          Build the right one - tools/build.sh --type $TYPE -" >&2
        echo "          or flash a Type $built_type tag instead." >&2
        exit 1
    fi
    # The wiring is the image's to state, not the operator's. Only cross-check
    # it if it was also typed, which is the one case where the two can differ.
    if [ -n "$VARIANT" ] && [ -n "$built_var" ] && [ "$VARIANT" != "$built_var" ]; then
        echo "flash.sh: REFUSING - you passed --variant $VARIANT but type" \
             "$TYPE" >&2
        echo "          is variant $built_var. Nothing was written." >&2
        exit 1
    fi
    if [ -n "$built_type" ]; then
        echo "type $TYPE confirmed against the image:" \
             "variant $built_var, panel $built_panel${built_wave:+, waveform $built_wave}."
        # Worth saying out loud rather than leaving to the filename. The
        # Waveshare table is the fast one and does not drive every panel; when
        # it does not, the matrix stays dead and only the border moves, which
        # looks like a broken screen rather than a wrong image.
        if [ "$built_wave" = WAVESHARE ]; then
            echo "  note: the fast waveform. If the panel goes dead but its"
            echo "        border still flickers, this is why - reflash the"
            echo "        plain type $TYPE image."
        fi
        # Said out loud because the filename no longer settles it: SUOTA became
        # the default, so an image built before that has none and is named the
        # same. And this is a one-way door - a tag running an image without SUOTA
        # can only be updated by coming back with the J-Link.
        if [ -n "$built_suota" ]; then
            echo "  SUOTA: yes - this tag will be updatable over the air."
        else
            echo "  SUOTA: NO. This tag will only be updatable over SWD, and"
            echo "         putting SUOTA back means flashing it here again."
            echo "         Rebuild without --no-suota if that is not intended."
        fi
    fi
    VARIANT=${built_var:-$VARIANT}
elif [ -z "$built_var" ]; then
    if [ "$UNVERIFIED" != 1 ]; then
        echo "flash.sh: $FW carries no variant stamp." >&2
        echo "          Every build since EPD_BOARD_VARIANT_TAG was added has" >&2
        echo "          one, so this is either an older image or not our" >&2
        echo "          firmware at all - and there is no way to tell from" >&2
        echo "          here which wiring it expects." >&2
        echo "          Rebuild it, or pass --unverified to flash it anyway." >&2
        exit 1
    fi
    echo "flash.sh: WARNING - no variant stamp in $FW; flashing unverified."
elif [ "$built_var" != "$VARIANT" ]; then
    echo "flash.sh: REFUSING - variant mismatch, nothing was written." >&2
    echo "          you asked for : $VARIANT" >&2
    echo "          image is built : $built_var" >&2
    echo >&2
    echo "          Build the right one - tools/build.sh --type <n> - or" >&2
    echo "          flash a board of variant $built_var instead." >&2
    exit 1
else
    echo "variant $VARIANT confirmed against the image."
fi

# mksuota.py synthesises the headers itself now, so the stock dump is no longer
# a source of bank offsets or of header fields nobody had decoded. It is the
# fallback image for the other bank and nothing else - which is worth having by
# default, since the bootloader really does fall back on a CRC failure, but is
# no longer a hard requirement.
MKSUOTA=(--fallback "$STOCK")
if [ $NO_FALLBACK -eq 1 ]; then
    # Types 1, 3 and 4 were each dumped and each boots from OTP, so flash
    # offset 0 is ignored and no bootloader is needed there. Type 2 has never
    # been dumped - it is probably the same, but "probably" is not a reason to
    # write an image that would not boot, so it still has to be told.
    case "$TYPE" in
        1|3|4) MKSUOTA=(--otp-boot)
           echo "no fallback: Type $TYPE boots from OTP (verified 2026-08-12);"
           echo "             flash offset 0 left erased." ;;
        2)
           # The retail bootloader payload is byte-identical on every retail
           # tag dumped so far, so it is pinned by hash rather than taken on
           # trust from a filename. That is not pedantry: re/type1 and re/type3
           # each hold a bootloader.bin that was sliced from offset 0 instead
           # of 8, so it carries the AN-B-001 header and is truncated by eight
           # bytes at the tail. Both are the right size and the wrong bytes,
           # and flashing one would leave a tag that does not boot.
           if [ -z "$BOOTLOADER" ]; then
               for c in "$STOCK_DIR"/type*/bootloader.bin; do
                   [ -r "$c" ] || continue
                   if [ "$(sha256sum <"$c" | cut -d' ' -f1)" = "$RETAIL_BOOT_SHA" ]; then
                       BOOTLOADER=$c; break
                   fi
               done
               if [ -z "$BOOTLOADER" ]; then
                   echo "flash.sh: --no-fallback on a Type 2 needs a bootloader for" >&2
                   echo "          flash offset 0, because no Type 2 has been dumped" >&2
                   echo "          and its boot chain is unverified. Nothing under" >&2
                   echo "          $STOCK_DIR/type*/bootloader.bin matches the known" >&2
                   echo "          retail payload ($RETAIL_BOOT_SHA)." >&2
                   echo "          Extract it from a stock dump - it is bytes 8 to" >&2
                   echo "          8+len, where len is the big-endian u32 at offset" >&2
                   echo "          4 - and pass it with --bootloader." >&2
                   exit 1
               fi
           elif [ "$(sha256sum <"$BOOTLOADER" | cut -d' ' -f1)" != "$RETAIL_BOOT_SHA" ]; then
               echo "flash.sh: WARNING - $BOOTLOADER is not the known retail" >&2
               echo "          bootloader payload. Expected sha256" >&2
               echo "          $RETAIL_BOOT_SHA." >&2
               echo "          Check it is the payload with the 8-byte AN-B-001" >&2
               echo "          header stripped, not the whole record. Proceeding" >&2
               echo "          because you named it explicitly." >&2
           fi
           MKSUOTA=(--bootloader "$BOOTLOADER")
           echo "no fallback: bootloader for offset 0 from $BOOTLOADER" ;;
        *) if [ -z "$BOOTLOADER" ] || [ ! -r "$BOOTLOADER" ]; then
               echo "flash.sh: --no-fallback needs to know whether this board" >&2
               echo "          boots from OTP or from flash offset 0, and for" >&2
               echo "          ${TYPE:+type $TYPE}${TYPE:-the --variant form} that is not recorded." >&2
               echo "          Pass --bootloader <file> if it boots from offset" >&2
               echo "          0; use --type 1 if its bootloader is in OTP." >&2
               exit 1
           fi
           MKSUOTA=(--bootloader "$BOOTLOADER")
           echo "no fallback: bootloader for offset 0 from $BOOTLOADER" ;;
    esac
    echo "             the other bank is left erased, so a bad build here has"
    echo "             nothing to fall back to."
fi
python3 "$HERE/mksuota.py" "${MKSUOTA[@]}" "$FW" "$OUT" "$BANK"

# `connect / r / h / loadbin` is all that is needed. Do NOT add a write to
# SYS_CTRL_REG (0x50000012) here: bit 7 is DEBUGGER_ENABLE, and clearing it
# switches off SWD until the tag is physically power-cycled.
cat > "$SCRIPT" <<EOF
si SWD
speed $SPEED
device DA14585
connect
r
h
loadbin $OUT, 0x04000000
q
EOF

JLinkExe -CommanderScript "$SCRIPT" | tee "$LOG"

# JLinkExe exits 0 even when it never reached the probe, so `set -e` cannot see
# a flash that was never written - it just prints "FAILED: Cannot connect" for
# every command and returns success. So the log has to be checked instead.
#
# A bare "^O.K." is NOT enough on its own, though this script used to accept it:
# J-Link prints one for `connect` too, so a run whose loadbin died with
# "Failed to download RAMCode!" still matched and the script cheerfully
# reported "Programmed." over a tag whose flash was untouched. That cost an
# afternoon of testing a build that was never on the tag, and every symptom
# pointed at the firmware rather than at the flasher.
#
# Require positive evidence of programming, and treat J-Link's own error lines
# as fatal regardless of what else it said.
if grep -qE '^\*+ Error:|Failed to (download|prepare|preserve|read back)' "$LOG"; then
    echo >&2
    echo "flash.sh: FAILED - J-Link reported an error, flash NOT written:" >&2
    grep -E '^\*+ Error:|Failed to ' "$LOG" | sed 's/^/          /' >&2
    echo >&2
    if grep -q 'Verification of RAMCode failed' "$LOG"; then
        echo "flash.sh: compare the Write:/Read: lines above before doing" >&2
        echo "          anything else. If they differ by only a bit or two," >&2
        echo "          the loader was placed fine and the SWD link corrupted" >&2
        echo "          it in transit - reseat the wires, try another USB" >&2
        echo "          cable, and retry with --speed 1000. These probes are" >&2
        echo "          often OB clones and the wires are soldered to test" >&2
        echo "          points, so this is the common case, and no amount of" >&2
        echo "          power-cycling fixes it." >&2
        echo "          If they differ wholesale, it IS the SysRAM clash" >&2
        echo "          below." >&2
        echo >&2
    fi
    echo "flash.sh: 'RAMCode' errors otherwise mean the loader could not be" >&2
    echo "          placed in SysRAM, which it shares with the running" >&2
    echo "          firmware. Power-cycle the tag and flash as the FIRST" >&2
    echo "          J-Link operation." >&2
    exit 1
fi

# "Flash download:" is printed only when J-Link actually programmed something.
# Note it is legitimately absent when every range already matches, so an
# unchanged image is not a failure - hence the separate O.K. check below.
if ! grep -q 'Flash download:' "$LOG" && ! grep -q '^O\.K\.' "$LOG"; then
    echo >&2
    echo "flash.sh: FAILED - the flash was NOT written." >&2
    if grep -q 'Cannot connect to the probe' "$LOG"; then
        echo "flash.sh: J-Link did not enumerate. A JLinkGUIServerExe left over" >&2
        echo "          from an earlier failed run holds the probe; kill it with" >&2
        echo "          pkill -f 'JLinkGUIServer[E]xe' and retry." >&2
    fi
    exit 1
fi

if grep -q 'Flash download:' "$LOG"; then
    echo
    grep 'Flash download:' "$LOG" | sed 's/^/  /'
else
    echo
    echo "  (no ranges programmed - the flash already held this image)"
fi

cat <<'EOF'

Programmed. Power-cycle the tag to boot it: SWD reset does not re-run the
bootloader's bank scan on this board, so the old image keeps running until the
power actually drops.

The template store sector is blanked by mksuota.py, so the tag comes back with
the built-in default face until a new one is pushed.
EOF
