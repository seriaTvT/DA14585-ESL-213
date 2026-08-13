#!/usr/bin/env bash
#
# flash.sh - program the firmware onto a tag over SWD.
#
#   tools/flash.sh out/hema_epd_clock.bin
#   tools/flash.sh out/hema_epd_clock-otp.bin 2      # into bank 2
#
# THERE IS NO TAG TYPE, AND NO VARIANT. There used to be both, and this script
# refused to program a tag whose type you had not named and matched, because a
# wrong-variant image was silent: the tag booted, advertised and took
# connections with a dead panel, and it cost a working tag twice. The image now
# reads its wiring, its panel geometry and its default face out of the board
# record at flash 0x039000, so there is no wrong tag to protect against.
#
# WHAT THIS DOES PROTECT is that record. It IS the tag's identity - the only
# copy - and a full-flash write would erase it. So the record is read off the
# tag before programming and written back into the image, every time. Two
# consequences worth knowing:
#
#   * A tag whose record is blank stays blank, and will come up as the built-in
#     case: variant B wiring, 122x250. Correct for a Type 1, wrong for the
#     others. Use --board to state what it is; see below.
#   * A stock dump is no longer needed to preserve identity. It is still useful
#     as the FALLBACK image for the other bank, which is a real feature of the
#     bootloader rather than a convention: on a CRC failure it loads and
#     verifies the other bank before giving up.
#
# The bootloader lives in OTP on every tag dumped (all carry
# OTP_HDR_OTP_CONTROL = 0xC0DEBABE and a byte-identical bootloader), so the
# AN-B-001 image at flash 0x000000 is present but never runs. The exact
# contract is in hema-local/docs/BOOT_CONTRACT.md.
#
#   --fallback <dump>  put this stock image in the other bank, so a bad build
#                      falls back to something that works. Recommended.
#   --board <spec>     write a board record instead of preserving the tag's.
#                      Only for a tag whose record is blank or wrong. One of:
#                        a53-b   122x250, variant B  (Type 1)
#                        a53-a   122x250, variant A  (Type 2)
#                        a41-a   104x212, variant A  (Type 3)
#                        a41-b   104x212, variant B  (Type 4)
#   --keep-record      accept a blank record without the warning. For a tag you
#                      know is a Type 1.
#   --force            program with no fallback image and no board record, and
#                      do not check the image is ours. For bench work.
#                      WHAT IT COSTS, so it is a choice rather than a habit:
#                        * no fallback in the other bank, so a bad build has
#                          nothing to fall back to and the tag needs SWD again;
#                        * an erased board record, so the tag comes up as the
#                          built-in case - variant B, 122x250. On a Type 1 that
#                          is right; on a Type 3 or 4 the panel goes dark and
#                          nothing says why. Reflash with --board to undo it.
#   --speed <kHz>      SWD clock, default 4000. Lower it (1000) if the loader
#                      fails to download - that is the link, not the target.
#   --bootloader <f>   secondary bootloader for flash offset 0. Not needed on
#                      any tag dumped so far, which all boot from OTP.
#
# Requires the community J-Link device definition for the DA14585 QSPI bank
# (JLinkDevices.xml + Devices/jtag_programmer.axf in /opt/SEGGER/JLink) -
# without it `device DA14585` exposes no flash bank and loadbin silently has
# nowhere to write.
#
# IF THE LOADER WILL NOT DOWNLOAD ("Failed to download RAMCode", "Verification
# of RAMCode failed"), the target is overwriting SysRAM faster than J-Link can
# place its loader there. Power-cycle the tag and run this as the FIRST J-Link
# operation. A tag whose banks are both invalid always programs, because
# nothing is running to clobber it.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/.." && pwd)

FLASH_BASE=0x04000000
REC_OFF=0x039000
REC_LEN=16

SPEED=${HEMA_SWD_SPEED:-4000}
FALLBACK=
BOOTLOADER=
BOARD=
KEEP_RECORD=0
FORCE=0
args=()

while [ $# -gt 0 ]; do
    case "$1" in
        --fallback)     FALLBACK=${2:-}; shift 2 ;;
        --fallback=*)   FALLBACK=${1#*=}; shift ;;
        --board)        BOARD=${2:-}; shift 2 ;;
        --board=*)      BOARD=${1#*=}; shift ;;
        --keep-record)  KEEP_RECORD=1; shift ;;
        --force)        FORCE=1; shift ;;
        --speed)        SPEED=${2:-}; shift 2 ;;
        --speed=*)      SPEED=${1#*=}; shift ;;
        --bootloader)   BOOTLOADER=${2:-}; shift 2 ;;
        --bootloader=*) BOOTLOADER=${1#*=}; shift ;;
        -h|--help)      sed -n '3,8p' "$0" >&2; exit 2 ;;
        -*)             echo "flash.sh: unknown option $1" >&2; exit 2 ;;
        *)              args+=("$1"); shift ;;
    esac
done

[ ${#args[@]} -ge 1 ] || { sed -n '3,8p' "$0" >&2; exit 2; }
FW=${args[0]}
BANK=${args[1]:-1}
[ -r "$FW" ] || { echo "flash.sh: cannot read $FW" >&2; exit 1; }

case "$BOARD" in
    ''|a53-b|a53-a|a41-a|a41-b) ;;
    *) echo "flash.sh: --board must be one of a53-b a53-a a41-a a41-b" >&2; exit 2 ;;
esac

# The waveform is the one thing a flash can still get wrong, so it is said out
# loud rather than checked: both are legitimate, and which one a panel needs is
# a property of its lot that nothing here can read.
WAVE=$(strings -a "$FW" | grep -om1 'HEMA-WAVEFORM-[A-Z]\+' || true)
COMPAT=$(strings -a "$FW" | grep -om1 'HEMA-COMPAT-[A-Za-z0-9-]\+' || true)
if [ -z "$COMPAT" ]; then
    if [ "$FORCE" != 1 ]; then
        echo "flash.sh: $FW carries no HEMA-COMPAT stamp." >&2
        echo "          Either it is not our firmware, or it predates the" >&2
        echo "          stamp. Rebuild it with tools/build.sh, or --force." >&2
        exit 1
    fi
    echo "          WARNING: no HEMA-COMPAT stamp; forced."
fi
echo "image:    $FW"
echo "          ${WAVE:-waveform not stated}   $COMPAT"

WORK=$(mktemp -d -t hema-flash-XXXXXX)
trap 'rm -rf "$WORK"' EXIT
IMG=$WORK/flash.bin
REC=$WORK/record.bin

jlink() {
    JLinkExe -NoGui 1 -CommanderScript "$1" 2>&1
}

# ---- 1. establish where the board record comes from -------------------------
# It cannot come from the tag. Reading the QSPI bank needs the same RAM loader
# that programming does, and it is the less reliable of the two - "Could not
# read memory" where a write of the same region succeeds. Depending on it would
# make every flash contingent on the flakier operation.
#
# So the record must come from one of two places, and this refuses rather than
# guess. Guessing means writing 0xFF, and an erased record is not neutral: the
# firmware reads it as the built-in case - variant B wiring, 122x250 - which is
# right for a Type 1 and silently wrong for every other tag.
if [ "$FORCE" = 1 ] && { [ -n "$FALLBACK" ] || [ -n "$BOARD" ]; }; then
    echo "flash.sh: --force means no fallback and no board record; passing" >&2
    echo "          --fallback or --board with it asks for both." >&2
    exit 2
fi

if [ "$FORCE" = 1 ]; then
    echo "record:   NOT WRITTEN (--force). This tag will read as the built-in"
    echo "          case: variant B, 122x250. Correct only for a Type 1."
elif [ -n "$FALLBACK" ] && [ -n "$BOARD" ]; then
    echo "flash.sh: --fallback and --board both supply the board record." >&2
    echo "          Pick one. The dump carries the record of the tag it came" >&2
    echo "          from; --board writes one you state." >&2
    exit 2
fi

if [ -n "$FALLBACK" ]; then
    [ -r "$FALLBACK" ] || { echo "flash.sh: cannot read $FALLBACK" >&2; exit 1; }
    REC_HEX=$(xxd -p -s $((REC_OFF)) -l $REC_LEN "$FALLBACK")
    echo "record:   $REC_HEX   (from $FALLBACK)"
    if [ "$REC_HEX" = "ffffffffffffffffffffffffffffffff" ]; then
        echo >&2
        echo "flash.sh: that dump's board record is BLANK, so it cannot tell" >&2
        echo "          this tag apart from a Type 1. If the tag really is a" >&2
        echo "          Type 1 this is correct - pass --keep-record. Otherwise" >&2
        echo "          use --board." >&2
        [ "$KEEP_RECORD" = 1 ] || exit 1
    fi
elif [ -n "$BOARD" ]; then
    :   # mksuota writes it; it prints what it wrote
elif [ "$FORCE" = 1 ]; then
    :   # said its piece above
else
    cat >&2 <<'EOF'
flash.sh: nothing to take the board record from, refusing.

          That record at 0x039000 is the tag's identity - which wiring, which
          panel - and it is the only copy. A full-flash write erases it, and an
          erased record reads as the built-in case: variant B, 122x250. On a
          Type 1 that is right. On a Type 3 or 4 the panel goes dark or draws
          garbled, with nothing to say why.

          Give it one of:
            --fallback <dump>   a stock dump OF THIS TAG. Also puts a working
                                image in the other bank, which is worth having.
            --board <spec>      a53-b (Type 1), a53-a (Type 2),
                                a41-a (Type 3), a41-b (Type 4)
EOF
    exit 2
fi

# ---- 2. build the flash image ---------------------------------------------
MK=("$HERE/mksuota.py")
[ -n "$FALLBACK" ] && MK+=(--fallback "$FALLBACK")
if [ -n "$BOOTLOADER" ]; then
    MK+=(--bootloader "$BOOTLOADER")
else
    MK+=(--otp-boot)
fi
[ -n "$BOARD" ] && MK+=(--board "$BOARD")
python3 "${MK[@]}" "$FW" "$IMG" "$BANK"

# ---- 3. program ------------------------------------------------------------
# `connect / r / h / loadbin` and nothing else. Do NOT add a write to
# SYS_CTRL_REG (0x50000012): bit 7 is DEBUGGER_ENABLE, and clearing it switches
# off SWD until the tag is physically power-cycled.
cat > "$WORK/prog.jlink" <<EOF
si SWD
speed $SPEED
device DA14585
connect
r
h
loadbin $IMG, $FLASH_BASE
q
EOF
LOG=$WORK/prog.log
jlink "$WORK/prog.jlink" | tee "$LOG"

# JLinkExe exits 0 even when it never reached the probe, so `set -e` cannot see
# a flash that was never written - it just prints its complaint and leaves.
if grep -qiE '\*\*\*\*\*\* Error|Cannot connect|Failed to' "$LOG"; then
    echo >&2
    echo "flash.sh: FAILED - J-Link reported an error, flash NOT written:" >&2
    grep -iE '\*\*\*\*\*\* Error|Cannot connect|Failed to' "$LOG" | head -3 \
        | sed 's/^/          /' >&2
    echo >&2
    echo "          If this is a RAMCode error, power-cycle the tag and run" >&2
    echo "          this again as the first J-Link operation. See the note at" >&2
    echo "          the top of this script." >&2
    exit 1
fi

cat <<EOF

Programmed. Power-cycle the tag to boot it: an SWD reset does not re-run the
bootloader's bank scan on this board, so the old image keeps running until the
power actually drops.

The template store sector is blanked, so the tag comes back on its built-in
default face until a new one is pushed.
EOF
