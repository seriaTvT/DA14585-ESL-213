#!/usr/bin/env bash
#
# build.sh - build the firmware for one tag type, or for every type at once.
#
#   tools/build.sh --type 3        -> out/hema_epd_clock-type3.bin
#   tools/build.sh --all           -> one image per known type, into out/
#   tools/build.sh --type 3 --clean
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
LOCAL_PROJ=${LOCAL_PROJ:-/home/nina/code/hema-local/toolchain/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/template/hema_epd_clock}
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
while [ $# -gt 0 ]; do
    case "$1" in
        --type)    types="${types}${types:+ }${2:-}"; shift 2 ;;
        --type=*)  types="${types}${types:+ }${1#*=}"; shift ;;
        --all)     types="$(known_types | tr '\n' ' ')"; shift ;;
        --clean)   force_clean=1; shift ;;
        -h|--help) usage; exit 2 ;;
        *)         echo "build.sh: unknown argument $1" >&2; usage; exit 2 ;;
    esac
done

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
    local t=$1 last=
    [ -r "$BUILD/.tag_type" ] && last=$(cat "$BUILD/.tag_type")

    # The type reaches every file through user_config.h, which is force-
    # included into every translation unit, so a switch rebuilds everything
    # anyway - and make cannot see a define change on its own. Clean rather
    # than trust it.
    if [ "$force_clean" = 1 ] || [ "$last" != "$t" ]; then
        rm -f "$BUILD/.tag_type"
        # Both streams: e2 studio's clean runs xargs -t, which echoes the whole
        # object list to stderr. Its recipe lines are all `-` prefixed, so it
        # reports nothing worth reading anyway.
        make -C "$BUILD" clean >/dev/null 2>&1
    fi

    echo
    echo "=== type $t ==="
    make -C "$BUILD" -j4 TAG_DEFS="-DHEMA_TAG_TYPE=$t" all | tail -3
    echo "$t" > "$BUILD/.tag_type"

    verify_stamp "$BUILD/hema_epd_clock.bin" "$t"

    mkdir -p "$OUT"
    cp "$BUILD/hema_epd_clock.bin" "$OUT/hema_epd_clock-type$t.bin"
    cp "$BUILD/hema_epd_clock.elf" "$OUT/hema_epd_clock-type$t.elf"

    # Echo back what the binary itself says, not what we asked for.
    printf '  %s  %s  %s  %s\n' \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-TAG-TYPE-[0-9]\+')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-BOARD-VARIANT-[AB]')" \
        "$(strings -a "$BUILD/hema_epd_clock.bin" | grep -om1 'HEMA-PANEL-[0-9x]\+')" \
        "-> out/hema_epd_clock-type$t.bin"
}

"$HERE/tools/sync.sh" --local
ensure_tag_defs
for t in $types; do build_one "$t"; done

cat <<EOF

Built: $(echo $types | tr ' ' ',')
Images are in $OUT, named by type. Flash one with:

  tools/flash.sh --type <n> $OUT/hema_epd_clock-type<n>.bin
EOF
