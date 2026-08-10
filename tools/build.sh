#!/usr/bin/env bash
#
# build.sh - build the firmware for one tag type, or for every type at once.
#
#   tools/build.sh --type 3        -> out/hema_epd_clock-type3.bin, on the
#                                     Waveshare waveform: ~2.5x faster, and
#                                     inert on some panel lots
#   tools/build.sh --type 3 --otp  -> the panel's own OTP waveform instead:
#                                     slower, and drives every lot we have
#   tools/build.sh --all           -> the whole set, all of one vintage: both
#                                     waveforms, both LUT shapes, and a -partial
#                                     image for each. Twenty builds, a few minutes.
#                                     Run it after any driver change.
#   tools/build.sh --type 3 --clean
#
# Flash the default, look at the glass, and reach for --otp if the matrix did
# not move. See HEMA_TAG_OTP_DEFAULT in src/config/tag_types.h for why that is
# the way round it is.
#
# Bench builds, for working out why a panel behaves as it does. Each gets its
# own name on disk, for the same reason --fast does.
#
#   --sweep          map the panel's OTP waveform against temperature. Blocks
#                    for ~a minute at boot; read epd_sweep_ms over SWD.
#   --lut-gain <n>   multiply the Waveshare waveform's drive by n. Separates
#                    "this lot needs more drive" from "our LUT is the wrong
#                    shape for this controller". Needs the Waveshare path.
#   --panel-id       read cmd 0x2F/0x2E/0x2D into epd_panel_id_* over SWD, to
#                    see whether the controller can say which lot it is.
#   --lut-steps <n>  which LUT shape the hand-written waveform is written for:
#                    7 (Waveshare's own) or 10 (measured on the A41 controller).
#                    Waveshare path only. Check s_poll_count, not just the glass:
#                    ~28 polls means the shape fits, ~4 means zero frames ran.
#   --lut-probe      measure the controller's LUT layout, by timing an update
#                    with one marker byte swept across the register. Blocks for
#                    minutes and never returns; read epd_lut_probe_ms over SWD.
#   --partial        repaint only changed rows with the partial waveform. Costs
#                    EPD_BUF_SIZE of RAM and has two unmeasured values in it;
#                    see EPD_PARTIAL in src/epd/epd_ssd1680.h before trusting it.
#   --no-suota       leave the SUOTA service out. On by default, so the tag can
#                    be updated over BLE rather than over SWD; this is how you
#                    opt out, and the image is named -nosuota so the choice is
#                    visible on disk. Think before using it: a tag running an
#                    image without SUOTA can only be updated by attaching SWD to
#                    it again.
#
# A tag type used to be two macros - the board variant and the panel size -
# edited by hand in src/config/user_config.h, and kept consistent with the
# letter passed to flash.sh by whoever remembered. That is one forgotten edit
# away from an image built for the wrong tag, and the wrong tag is silent: it
# boots, advertises and takes connections exactly as normal, and only the panel
# stays dead. It has cost a working tag twice.
#
# So the type is one number now. It is passed to the compiler from here rather
# than written into a source file, which means switching types touches nothing
# that git tracks and `--all` is just a loop. The image is then checked against
# the number before this script will say it built anything - see verify_stamp,
# and note that a silently-unpatched build tree would otherwise hand you four
# identical images with four different names.
#
# Local build tree only. The VM path is still `tools/sync.sh --build`; the
# toolchain has been on this machine since 2026-07-28 and produces a
# byte-identical image (hema-local/docs/LOCAL_BUILD.md).
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
TABLE=$HERE/firmware/hema_epd_clock/src/config/tag_types.h

usage() { sed -n '3,8p' "$0" >&2; }

# The set of types comes from the header rather than being listed here, so
# adding a tag stays a one-row change in one file and --all picks it up.
known_types() {
    grep -oE 'HEMA_TAG_TYPE == [0-9]+' "$TABLE" | grep -oE '[0-9]+$' | sort -un
}

types=
force_clean=0
wf=
also_alt=0
sweep=0
panel_id=0
partial=0
suota=1
lut_probe=0
lut_steps=
lut_gain=1
while [ $# -gt 0 ]; do
    case "$1" in
        --type)    types="${types}${types:+ }${2:-}"; shift 2 ;;
        --type=*)  types="${types}${types:+ }${1#*=}"; shift ;;
        --all)     types="$(known_types | tr '\n' ' ')"; also_alt=1; shift ;;
        # Both directions stay available and explicit. --fast is not a no-op
        # just because it is now the default: it pins the waveform in the image
        # name and in the build, so a command written down today still means the
        # same thing if a type's default is ever changed again.
        --fast)    wf=fast; shift ;;
        --otp)     wf=otp; shift ;;
        --clean)   force_clean=1; shift ;;
        --sweep)   sweep=1; shift ;;
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

[ -n "$types" ] || { usage; exit 2; }
[ -r "$TABLE" ] || { echo "build.sh: cannot read $TABLE" >&2; exit 1; }

for t in $types; do
    if ! known_types | grep -qx -- "$t"; then
        echo "build.sh: '$t' is not a known tag type." >&2
        echo "          Known: $(known_types | tr '\n' ' ')" >&2
        echo "          A new one is a row in $TABLE" >&2
        echo "          and a row in hema-local/docs/TAG_VARIANTS.md." >&2
        exit 2
    fi
done

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
    local bin=$1 want=$2 got
    got=$(strings -a "$bin" | grep -om1 'HEMA-TAG-TYPE-[0-9]\+' || true)
    if [ "$got" != "HEMA-TAG-TYPE-$want" ]; then
        echo >&2
        echo "build.sh: REFUSING - asked for type $want, the image says" >&2
        echo "          '${got:-nothing at all}'." >&2
        echo "          Either the build tree is not honouring \$(TAG_DEFS)," >&2
        echo "          or this is not our firmware. Try --clean." >&2
        exit 1
    fi
}

build_one() {
    local t=$1 want=$2 defs suffix label last= is_waveshare
    defs="-DHEMA_TAG_TYPE=$t"
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
    elif [ "$want" = fast ]; then
        is_waveshare=1
    elif type_defaults_to_otp "$t"; then
        is_waveshare=0
    else
        is_waveshare=1
    fi

    # Bench options. Each one changes the suffix as well as the defines: these
    # images look identical to a working build from the outside and one of them
    # blocks for a minute at boot, so an unlabelled copy on disk is a trap.
    if [ "$sweep" = 1 ]; then
        defs="$defs -DEPD_TEMP_SWEEP=1"
        suffix="$suffix-sweep"
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

    label="$t$suffix"
    [ -r "$BUILD/.tag_type" ] && last=$(cat "$BUILD/.tag_type")

    # The type reaches every file through user_config.h, which is force-
    # included into every translation unit, so a switch rebuilds everything
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
    echo "=== type $label ==="
    make -C "$BUILD" -j4 TAG_DEFS="$defs" all | tail -3
    echo "$label" > "$BUILD/.tag_type"

    verify_stamp "$BUILD/hema_epd_clock.bin" "$t"

    mkdir -p "$OUT"
    cp "$BUILD/hema_epd_clock.bin" "$OUT/hema_epd_clock-type$t$suffix.bin"
    cp "$BUILD/hema_epd_clock.elf" "$OUT/hema_epd_clock-type$t$suffix.elf"

    # Echo back what the binary itself says, not what we asked for.
    printf '  %s  %s  %s  %s\n      -> out/hema_epd_clock-type%s%s.bin\n' \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-TAG-TYPE-[0-9]\+')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-BOARD-VARIANT-[AB]')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-PANEL-[0-9x]\+')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-WAVEFORM-[A-Z]\+')" \
        "$t" "$suffix"
}

# Which waveform a type takes when nothing overrides it. Read from the header so
# there is one source of truth: flipping a default there changes what --all
# builds, with no matching edit needed here.
type_defaults_to_otp() {
    grep -A4 "HEMA_TAG_TYPE == $1\$" "$TABLE" \
        | grep -qE 'HEMA_TAG_OTP_DEFAULT[[:space:]]+1'
}

"$HERE/tools/sync.sh" --local
# After the sync, so a source file added in the repo is on disk to be found,
# and before ensure_tag_defs, so a rule this just cloned still gets patched if
# it came from a tree that predates the patch.
python3 "$HERE/tools/register_sources.py" "$BUILD"
ensure_tag_defs
for t in $types; do
    build_one "$t" "$wf"
    # --all also builds the OTHER waveform for each type, so the fallback is
    # already on disk when a panel turns out not to take the default. Which one
    # that is depends on the type's default, hence the lookup rather than a
    # hardcoded "also build fast".
    if [ "$also_alt" = 1 ] && [ -z "$wf" ]; then
        if type_defaults_to_otp "$t"; then
            build_one "$t" fast
        else
            build_one "$t" otp
        fi

        # And the partial image, for the waveform that can actually do one.
        #
        # Here because a mixed-age out/ is genuinely dangerous: on 2026-08-09 two
        # already-fixed bugs were re-reported from tags flashed with -partial
        # images built before the fixes, sitting beside freshly built defaults
        # under names that gave no hint of their age. The images were right about
        # which tag they were for and silently wrong about everything else.
        #
        # So --all means "the whole set, all of one vintage". One command to run
        # after a driver change, and nothing to remember per variant.
        if [ "$partial" != 1 ] && ! type_defaults_to_otp "$t"; then
            partial=1
            build_one "$t" ""
            partial=0

            # And both again at ten steps. Two of the five panels in hand run a
            # ten-step controller, so leaving these out of --all would recreate
            # exactly the mixed-age out/ this whole arrangement exists to prevent:
            # the -s10 images would be whatever vintage they were last built at,
            # under names that give no hint of it.
            if [ -z "$lut_steps" ]; then
                lut_steps=10
                build_one "$t" ""
                partial=1
                build_one "$t" ""
                partial=0
                lut_steps=
            fi
        fi
    fi
done

cat <<EOF

Built: $(echo $types | tr ' ' ',')
Images are in $OUT, named by type. Flash one with:

  tools/flash.sh --type <n> $OUT/hema_epd_clock-type<n>.bin
EOF
