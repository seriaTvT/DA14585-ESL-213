#!/usr/bin/env bash
#
# flash.sh - build a full SPI-flash image and program it over SWD.
#
#   tools/flash.sh <stock_dump.bin> <hema_epd_clock.bin> [bank]
#
# Wraps the two steps that actually put firmware on the tag: mksuota.py to wrap
# the raw linker .bin in a SUOTA image bank, and J-Link Commander to program it.
# See README.md for why a raw .bin at offset 0 does not boot on this board.
#
# The tag's secondary bootloader is in OTP and picks the *newest valid* of two
# image banks, so writing bank 1 and leaving the stock image in bank 2 means a
# bad build falls back to something that works rather than bricking the tag.
#
# Requires the community J-Link device definition for the DA14585 QSPI bank
# (JLinkDevices.xml + Devices/jtag_programmer.axf in /opt/SEGGER/JLink) -
# without it `device DA14585` exposes no flash bank and loadbin silently has
# nowhere to write.
set -euo pipefail

if [ $# -lt 2 ]; then
    sed -n '3,8p' "$0" >&2
    exit 2
fi

STOCK=$1
FW=$2
BANK=${3:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -t hema-flash-XXXXXX.bin)
SCRIPT=$(mktemp -t hema-flash-XXXXXX.jlink)
LOG=$(mktemp -t hema-flash-XXXXXX.log)
trap 'rm -f "$OUT" "$SCRIPT" "$LOG"' EXIT

for f in "$STOCK" "$FW"; do
    [ -r "$f" ] || { echo "flash.sh: cannot read $f" >&2; exit 1; }
done

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
