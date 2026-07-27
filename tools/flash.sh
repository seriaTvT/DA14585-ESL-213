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
trap 'rm -f "$OUT" "$SCRIPT"' EXIT

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

JLinkExe -CommanderScript "$SCRIPT"

cat <<'EOF'

Programmed. Power-cycle the tag to boot it: SWD reset does not re-run the
bootloader's bank scan on this board, so the old image keeps running until the
power actually drops.

The template store sector is blanked by mksuota.py, so the tag comes back with
the built-in default face until a new one is pushed.
EOF
