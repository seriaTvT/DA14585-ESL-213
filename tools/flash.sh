#!/usr/bin/env bash
#
# flash.sh - build a full SPI-flash image and program it over SWD.
#
#   tools/flash.sh --variant <a|b> <stock_dump.bin> <hema_epd_clock.bin> [bank]
#
# Wraps the two steps that actually put firmware on the tag: mksuota.py to wrap
# the raw linker .bin in a SUOTA image bank, and J-Link Commander to program it.
# See README.md for why a raw .bin at offset 0 does not boot on this board.
#
# The tag's secondary bootloader is in OTP and picks the *newest valid* of two
# image banks, so writing bank 1 and leaving the stock image in bank 2 means a
# bad build falls back to something that works rather than bricking the tag.
#
# --variant is mandatory and has no default. Flashing a board with the other
# variant's wiring is the worst kind of wrong: the tag boots, advertises and
# takes connections exactly as normal, and only the panel stays dead - so it
# presents as a broken screen, not as a bad flash. It has cost a working tag
# twice, in both directions, and neither time was diagnosed from the symptom.
# A default is what let that happen by omission, so there is not one.
#
# Naming it is only half of it, since a typed letter can be as wrong as a
# default. The firmware stamps EPD_BOARD_VARIANT_TAG into its own image, so the
# letter is checked against the binary about to be written and a mismatch stops
# the flash.
#
# Requires the community J-Link device definition for the DA14585 QSPI bank
# (JLinkDevices.xml + Devices/jtag_programmer.axf in /opt/SEGGER/JLink) -
# without it `device DA14585` exposes no flash bank and loadbin silently has
# nowhere to write.
set -euo pipefail

VARIANT=
UNVERIFIED=0
args=()
while [ $# -gt 0 ]; do
    case "$1" in
        --variant)     VARIANT=${2:-}; shift 2 ;;
        --variant=*)   VARIANT=${1#*=}; shift ;;
        --unverified)  UNVERIFIED=1; shift ;;
        -h|--help)     sed -n '3,24p' "$0" >&2; exit 2 ;;
        -*)            echo "flash.sh: unknown option $1" >&2; exit 2 ;;
        *)             args+=("$1"); shift ;;
    esac
done

if [ ${#args[@]} -lt 2 ]; then
    sed -n '3,24p' "$0" >&2
    exit 2
fi

# Uppercase once here so the comparison below is against one canonical form,
# and so the message can echo back what was actually meant.
VARIANT=$(printf '%s' "$VARIANT" | tr '[:lower:]' '[:upper:]')
case "$VARIANT" in
    A|B) ;;
    "")  echo "flash.sh: --variant is required (a or b)." >&2
         echo "          Which wiring is this board? Type 1 is B, Type 2 is A." >&2
         echo "          See hema-local/docs/TAG_VARIANTS.md if unsure - guessing" >&2
         echo "          costs a tag, and the symptom will not tell you." >&2
         exit 2 ;;
    *)   echo "flash.sh: --variant must be a or b, not '$VARIANT'." >&2; exit 2 ;;
esac

STOCK=${args[0]}
FW=${args[1]}
BANK=${args[2]:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -t hema-flash-XXXXXX.bin)
SCRIPT=$(mktemp -t hema-flash-XXXXXX.jlink)
LOG=$(mktemp -t hema-flash-XXXXXX.log)
trap 'rm -f "$OUT" "$SCRIPT" "$LOG"' EXIT

for f in "$STOCK" "$FW"; do
    [ -r "$f" ] || { echo "flash.sh: cannot read $f" >&2; exit 1; }
done

# ------------------------------------------------------- variant cross-check
# Read the variant the image was actually built for, rather than trusting the
# letter on the command line. -a because the string lives in .rodata, not in a
# section `strings` would look at by default.
built=$(strings -a "$FW" | grep -om1 'HEMA-BOARD-VARIANT-[AB]' || true)
built=${built##*-}

if [ -z "$built" ]; then
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
elif [ "$built" != "$VARIANT" ]; then
    echo "flash.sh: REFUSING - variant mismatch, nothing was written." >&2
    echo "          you asked for : $VARIANT" >&2
    echo "          image is built : $built" >&2
    echo >&2
    echo "          Set EPD_BOARD_VARIANT_$VARIANT in src/config/user_config.h" >&2
    echo "          and rebuild, or flash a board of variant $built instead." >&2
    exit 1
else
    echo "variant $VARIANT confirmed against the image."
fi

python3 "$HERE/mksuota.py" "$STOCK" "$FW" "$OUT" "$BANK"

# `connect / r / h / loadbin` is all that is needed. Do NOT add a write to
# SYS_CTRL_REG (0x50000012) here: bit 7 is DEBUGGER_ENABLE, and clearing it
# switches off SWD until the tag is physically power-cycled.
cat > "$SCRIPT" <<EOF
si SWD
speed 4000
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
    echo "flash.sh: 'RAMCode' errors mean the loader could not be placed in" >&2
    echo "          SysRAM, which it shares with the running firmware. Power-" >&2
    echo "          cycle the tag and flash as the FIRST J-Link operation." >&2
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
