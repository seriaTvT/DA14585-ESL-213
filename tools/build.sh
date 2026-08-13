#!/usr/bin/env bash
#
# build.sh - build the firmware. There is one image, and it runs on every tag.
#
#   tools/build.sh                 -> out/hema_epd_clock.bin
#   tools/build.sh --otp           -> the panel's own OTP waveform instead
#   tools/build.sh --all           -> every waveform/LUT/partial combination,
#                                     all of one vintage. Run after a driver
#                                     change. Five images, about a minute.
#   tools/build.sh --clean
#
# THERE IS NO --type. There used to be four, one per tag, and choosing wrong
# gave you a tag that booted, advertised and took connections with a dead panel.
# The image now reads its wiring, its panel geometry and its default face out of
# the record at flash 0x039000 at boot, so it fits any of them. Proven on
# hardware: an image built for a variant-A 104x212 tag drove a variant-B 122x250
# panel. See hema-local/docs/TAG_VARIANTS.md.
#
# WHAT STILL VARIES is the waveform, and only the waveform. It is keyed to the
# panel LOT, which nothing on the tag reports - two tags of the same type can
# disagree. Flash the default, look at the glass, and reach for --otp if the
# matrix did not move. See tag_types.h for the lot table.
#
#   --otp            the panel's own waveform: slower, temperature-compensated,
#                    and it drives every lot we have.
#   --fast           pin the Waveshare table explicitly. It is the default;
#                    passing it means a command written down today keeps its
#                    meaning if the default ever changes.
#   --lut-steps <n>  which LUT shape the hand-written table is written for: 7
#                    (Waveshare's own) or 10 (measured on the A41 controller).
#                    Waveshare path only. Check s_poll_count, not just the
#                    glass: ~28 polls means the shape fits, ~4 means zero frames.
#   --lut-gain <n>   multiply the Waveshare waveform's drive by n. Separates
#                    "this lot needs more drive" from "wrong shape".
#   --partial        repaint only changed rows. Costs EPD_BUF_SIZE_MAX of RAM
#                    and has two unmeasured values in it; see EPD_PARTIAL.
#   --no-suota       leave the SUOTA service out. Think before using it: a tag
#                    running such an image can only be updated over SWD again.
#
# Bench options, each of which renames the image because these look identical
# from the outside and one of them blocks for a minute at boot:
#
#   --tx-profile     time the frame write into epd_tx_us/epd_tx_bytes, read with
#                    hema-local/tools/tagread.py.
#   --tx-slow        the pre-2026-08-12 bit loop, via GPIO_SetActive(). The first
#                    thing to try if a panel stops latching.
#   --sweep          map the OTP waveform against temperature. Blocks ~a minute.
#   --panel-id       read cmd 0x2F/0x2E/0x2D into epd_panel_id_* over SWD.
#   --lut-probe      measure the controller's LUT layout. Blocks for minutes and
#                    never returns; read epd_lut_probe_ms over SWD.
#
# Local build tree only. The VM path is still `tools/sync.sh --build`.
#
# Configure with the environment if your paths differ:
#   LOCAL_PROJ  the hema_epd_clock dir inside the SDK (same var sync.sh uses)
#   OUT         where the named images land        (default: <repo>/out)
set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$HERE/.." && pwd)
LOCAL_PROJ=${LOCAL_PROJ:-$WORKSPACE/hema-local/toolchain/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/template/hema_epd_clock}
OUT=${OUT:-$HERE/out}
BUILD=$LOCAL_PROJ/e2studio/DA14585

usage() { sed -n '3,10p' "$0" >&2; }

force_clean=0
wf=
also_alt=0
sweep=0
tx_profile=0
tx_slow=0
panel_id=0
partial=0
suota=1
lut_probe=0
lut_steps=
lut_gain=1
while [ $# -gt 0 ]; do
    case "$1" in
        --all)     also_alt=1; shift ;;
        # Both directions stay available and explicit. --fast is not a no-op
        # just because it is now the default: it pins the waveform in the image
        # name and in the build, so a command written down today still means the
        # same thing if a type's default is ever changed again.
        --fast)    wf=fast; shift ;;
        --otp)     wf=otp; shift ;;
        --clean)   force_clean=1; shift ;;
        --sweep)   sweep=1; shift ;;
        --tx-profile) tx_profile=1; shift ;;
        --tx-slow) tx_slow=1; shift ;;
        --panel-id) panel_id=1; shift ;;
        --partial) partial=1; shift ;;
        --suota)   suota=1; shift ;;            # the default; kept explicit
        --no-suota) suota=0; shift ;;
        --lut-probe) lut_probe=1; shift ;;
        --lut-steps)   lut_steps=${2:-}; shift 2 ;;
        --lut-steps=*) lut_steps=${1#*=}; shift ;;
        --lut-gain)   lut_gain=${2:-}; shift 2 ;;
        --lut-gain=*) lut_gain=${1#*=}; shift ;;
        -h|--help) usage; exit 2 ;;
        *)         echo "build.sh: unknown argument $1" >&2; usage; exit 2 ;;
    esac
done

case "$lut_steps" in
    ''|7|10) ;;
    *) echo "build.sh: --lut-steps must be 7 or 10, got '$lut_steps'" >&2; exit 2 ;;
esac

case "$lut_gain" in
    ''|*[!0-9]*) echo "build.sh: --lut-gain wants a whole number, got '$lut_gain'" >&2; exit 2 ;;
esac
[ "$lut_gain" -ge 1 ] || { echo "build.sh: --lut-gain is a multiplier; 1 means unmodified" >&2; exit 2; }

[ -d "$BUILD" ] || {
    echo "build.sh: no build tree at $BUILD" >&2
    echo "          Set LOCAL_PROJ, or set the tree up - see" >&2
    echo "          hema-local/docs/LOCAL_BUILD.md." >&2
    exit 1
}

# ------------------------------------------------------------------ TAG_DEFS
# e2 studio bakes the whole compiler invocation into each subdir.mk, with no
# variable to hook, so -DHEMA_TAG_TYPE has nowhere to go until one is added.
# Idempotent, and re-applied on demand rather than once by hand, because the
# build tree is not tracked by git and a regenerated one would silently lose
# it - silently being the operative word, since an unpatched tree still builds
# perfectly well, just always at the header's default type.
ensure_tag_defs() {
    local f n=0
    for f in "$BUILD"/*/subdir.mk; do
        [ -e "$f" ] || continue
        grep -q 'TAG_DEFS' "$f" && continue
        sed -i 's|@clang -c |@clang -c $(TAG_DEFS) |g' "$f"
        grep -q 'TAG_DEFS' "$f" || {
            echo "build.sh: could not patch $f - the compile line is not the" >&2
            echo "          '@clang -c ...' this expects. Look at it and add" >&2
            echo "          \$(TAG_DEFS) after -c by hand." >&2
            exit 1
        }
        n=$((n + 1))
    done
    [ "$n" -gt 0 ] && echo "patched $n subdir.mk for \$(TAG_DEFS)"
    return 0
}

# --------------------------------------------------------------------- build
# The image says which tag it is for; that is the only claim worth trusting,
# since every way this can go wrong (an unpatched tree, objects left over from
# the previous type, a make that decided nothing needed rebuilding) produces a
# working image for the *wrong* tag rather than an error.
verify_stamp() {
    local bin=$1 want_waveshare=$2 got want
    got=$(strings -a "$bin" | grep -om1 'HEMA-WAVEFORM-[A-Z]\+' || true)
    [ "$want_waveshare" = 1 ] && want=HEMA-WAVEFORM-WAVESHARE || want=HEMA-WAVEFORM-OTP
    if [ "$got" != "$want" ]; then
        echo >&2
        echo "build.sh: REFUSING - asked for $want, the image says" >&2
        echo "          '${got:-nothing at all}'." >&2
        echo "          Either the build tree is not honouring \$(TAG_DEFS)," >&2
        echo "          or this is not our firmware. Try --clean." >&2
        exit 1
    fi
}

build_one() {
    local want=$1 defs suffix label last= is_waveshare
    defs=
    suffix=

    # Pin the waveform, or leave it to the header. Either way the image carries
    # the choice in its name, because the two are indistinguishable on disk and
    # the wrong one is a screen that does not move.
    case "$want" in
        fast) defs="$defs -DEPD_INIT_FROM_OTP=0"; suffix="-fast" ;;
        otp)  defs="$defs -DEPD_INIT_FROM_OTP=1"; suffix="-otp"  ;;
    esac

    # Which waveform this build will actually end up on, for the --lut-gain
    # check below. Asking the header rather than assuming, so this stays right
    # if a type's default changes.
    if [ "$want" = otp ]; then
        is_waveshare=0
    else
        is_waveshare=1          # the default, and --fast pins it explicitly
    fi

    # Bench options. Each one changes the suffix as well as the defines: these
    # images look identical to a working build from the outside and one of them
    # blocks for a minute at boot, so an unlabelled copy on disk is a trap.
    if [ "$sweep" = 1 ]; then
        defs="$defs -DEPD_TEMP_SWEEP=1"
        suffix="$suffix-sweep"
    fi
    if [ "$tx_slow" = 1 ]; then
        defs="$defs -DEPD_TX_FAST=0"
        suffix="$suffix-txslow"
    fi
    if [ "$tx_profile" = 1 ]; then
        defs="$defs -DEPD_TX_PROFILE=1"
        suffix="$suffix-txprof"
    fi
    if [ "$panel_id" = 1 ]; then
        defs="$defs -DEPD_PANEL_ID=1"
        suffix="$suffix-id"
    fi
    if [ -n "$lut_steps" ]; then
        # The tables are compiled only on the Waveshare path, so an OTP build
        # would take the flag and ignore it - the same silent no-op --lut-gain is
        # refused for.
        if [ "$is_waveshare" != 1 ]; then
            echo "build.sh: --lut-steps picks the shape of the hand-written table," >&2
            echo "          which an OTP build does not use. Drop --otp." >&2
            exit 2
        fi
        defs="$defs -DEPD_LUT_STEPS=$lut_steps"
        suffix="$suffix-s$lut_steps"
    fi
    if [ "$lut_probe" = 1 ]; then
        defs="$defs -DEPD_LUT_PROBE=1"
        suffix="$suffix-lutprobe"
    fi
    if [ "$partial" = 1 ]; then
        # Measured 2026-08-09 on the SLH1904 tag: these panels hold no partial
        # waveform in OTP, so there is nothing for a partial refresh to use.
        # 0x22 <- 0xFF asks the controller to load the LUT with Display Mode 2
        # selected, and the refresh takes 73 polls - the full waveform's exact
        # duration, three reads running. Duration is a property of the waveform,
        # so an identical duration means an identical waveform, and no amount of
        # display-option configuration can conjure a second one into OTP.
        #
        # A build like that is not broken, just pointless: every "partial" runs a
        # full waveform, the image is correct, and the only costs are 2760 bytes
        # of RAM and an epd_last_paint that says 1 while lying. Refused rather
        # than shipped, because the next person to see "partial" in a filename
        # would reasonably believe it.
        #
        # If a lot ever turns up whose OTP does hold a second waveform, this is
        # the guard to delete - and hema-local/docs/PANEL_LOTS.md is where the
        # evidence lives.
        if [ "$is_waveshare" != 1 ]; then
            echo "build.sh: --partial needs the Waveshare waveform. These panels" >&2
            echo "          carry no partial waveform in OTP - measured, see" >&2
            echo "          PANEL_LOTS.md - so an OTP build would run a full" >&2
            echo "          refresh every time and call it a partial." >&2
            echo "          Drop --otp, or accept full refreshes on this lot." >&2
            exit 2
        fi
        defs="$defs -DEPD_PARTIAL=1"
        suffix="$suffix-partial"
    fi
    # SUOTA is ON by default, and --no-suota is the flag that gets a suffix.
    #
    # It was the other way round for exactly one afternoon, and that was wrong:
    # `--all` then built twenty images, none of which could ever be updated over
    # the air. Flashing one is a one-way door - the tag has no SUOTA service
    # afterwards, so the only way to put SUOTA back is SWD, on a bench, per tag -
    # and the whole reason this feature exists is that there is one J-Link and
    # many tags. A default whose cost is "you must physically revisit every tag
    # you used it on" is not a safe default.
    #
    # It costs +4.9 KB of flash and +548 bytes of RAM. The image is ~41 KB and the
    # smallest bank on any tag is 73664 bytes, so there is no pressure here.
    if [ "$suota" = 1 ]; then
        defs="$defs -DEPD_SUOTA=1"
    else
        suffix="$suffix-nosuota"
    fi
    if [ "$lut_gain" != 1 ]; then
        # The gain scales the hand-written table, which only exists on the
        # Waveshare path - an OTP build compiles it out entirely, so the flag
        # would be accepted and do nothing. That failure costs a flash and a
        # look at a screen to notice, so refuse it here instead.
        if [ "$is_waveshare" != 1 ]; then
            echo "build.sh: --lut-gain scales the Waveshare waveform, and this" >&2
            echo "          build is on the OTP one, which has no table to scale." >&2
            echo "          Drop --otp to put this build on the Waveshare path." >&2
            exit 2
        fi
        defs="$defs -DEPD_LUT_GAIN=$lut_gain"
        suffix="$suffix-gain$lut_gain"
    fi

    label="${suffix:-default}"
    [ -r "$BUILD/.tag_type" ] && last=$(cat "$BUILD/.tag_type")

    # The defines reach every file through user_config.h, which is force-
    # included into every translation unit, so a change rebuilds everything
    # anyway - and make cannot see a define change on its own. Clean rather
    # than trust it.
    if [ "$force_clean" = 1 ] || [ "$last" != "$label" ]; then
        rm -f "$BUILD/.tag_type"
        # Both streams: e2 studio's clean runs xargs -t, which echoes the whole
        # object list to stderr. Its recipe lines are all `-` prefixed, so it
        # reports nothing worth reading anyway.
        make -C "$BUILD" clean >/dev/null 2>&1
    fi

    echo
    echo "=== $label ==="
    make -C "$BUILD" -j4 TAG_DEFS="$defs" all | tail -3
    echo "$label" > "$BUILD/.tag_type"

    verify_stamp "$BUILD/hema_epd_clock.bin" "$is_waveshare"

    mkdir -p "$OUT"
    cp "$BUILD/hema_epd_clock.bin" "$OUT/hema_epd_clock$suffix.bin"
    cp "$BUILD/hema_epd_clock.elf" "$OUT/hema_epd_clock$suffix.elf"

    # Echo back what the binary itself says, not what we asked for.
    printf '  %s  %s\n      -> out/hema_epd_clock%s.bin\n' \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-WAVEFORM-[A-Z]\+')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-COMPAT-[A-Za-z0-9-]\+')" \
        "$suffix"
}

"$HERE/tools/sync.sh" --local
# After the sync, so a source file added in the repo is on disk to be found,
# and before ensure_tag_defs, so a rule this just cloned still gets patched if
# it came from a tree that predates the patch.
python3 "$HERE/tools/register_sources.py" "$BUILD"
ensure_tag_defs
build_one "$wf"

# --all is "the whole set, all of one vintage".
#
# It exists because a mixed-age out/ is genuinely dangerous: on 2026-08-09 two
# already-fixed bugs were re-reported from tags flashed with -partial images
# built before the fixes, sitting beside freshly built defaults under names that
# gave no hint of their age.
#
# It used to be twenty images, four tag types by five options. The types are
# gone, so it is five: the default, the other waveform, the partial, and both
# LUT shapes. One command to run after a driver change.
if [ "$also_alt" = 1 ] && [ -z "$wf" ]; then
    build_one otp

    if [ "$partial" != 1 ]; then
        partial=1
        build_one ""
        partial=0

        # And both again at ten steps. Two of the five panels in hand run a
        # ten-step controller, so leaving these out would recreate exactly the
        # mixed-age out/ this arrangement exists to prevent.
        if [ -z "$lut_steps" ]; then
            lut_steps=10
            build_one ""
            partial=1
            build_one ""
            partial=0
            lut_steps=
        fi
    fi
fi

cat <<EOF

Images are in $OUT. There is no tag type to choose - flash any of them to any
tag and it will read what it needs off the board:

  tools/flash.sh $OUT/hema_epd_clock.bin

If the matrix does not move, the panel lot wants the other waveform:

  tools/flash.sh $OUT/hema_epd_clock-otp.bin
EOF
