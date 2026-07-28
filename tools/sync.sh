#!/usr/bin/env bash
#
# sync.sh - push this repo's firmware sources to a build tree, VM or local.
#
#   tools/sync.sh              sync to the VM, then verify
#   tools/sync.sh --check      verify only, change nothing
#   tools/sync.sh --build      sync, verify, then build over there
#
#   tools/sync.sh --local            same three, against the build tree on
#   tools/sync.sh --local --check    this machine instead of the VM
#   tools/sync.sh --local --build
#
# The build compiles from inside the SDK tree, not from this repo, so every
# edit has to be copied across before it means anything. Doing that by hand
# with scp has gone wrong twice in the ways this script exists to prevent:
#
#   - A file edited here and never copied. The build succeeds, the binary is
#     unchanged, and the symptom is a fix that "did not work" - so the next
#     hour goes into re-fixing something that was already right.
#   - A file deleted or renamed here and left behind there. It keeps compiling
#     into the image, which is worse than the first case because the evidence
#     actively contradicts the source you are reading.
#
# So this mirrors with --delete rather than copying, and then verifies by
# comparing checksums both ways rather than trusting that rsync did what it
# said. The verify is the point; the copy is the easy half.
#
# --local exists because the toolchain now lives on this machine too (see
# hema-local/docs/LOCAL_BUILD.md). That build tree holds its own *copy* of
# src/, so it is open to exactly the same stale-copy trap as the VM's and
# deserves exactly the same mirror-and-verify treatment. Nothing about the
# hazard is specific to the copy being on the far side of an ssh connection,
# so the only thing that changes below is how a command gets run.
#
# Configure with the environment if your paths differ:
#   VM          ssh host for the build machine      (default: vm)
#   VM_PROJ     the hema_epd_clock dir inside the SDK on that machine
#   LOCAL_PROJ  the same thing on this machine, for --local
set -euo pipefail

VM=${VM:-vm}
VM_PROJ=${VM_PROJ:-/home/nina/Downloads/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/template/hema_epd_clock}
LOCAL_PROJ=${LOCAL_PROJ:-/home/nina/code/hema-local/toolchain/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/template/hema_epd_clock}

HERE=$(cd "$(dirname "$0")/.." && pwd)
SRC_PROJ="$HERE/firmware/hema_epd_clock"

mode=sync
target=vm
for arg in "$@"; do
    case "$arg" in
        --local) target=local ;;
        --check) mode=check ;;
        --build) mode=build ;;
        *)       sed -n '3,11p' "$0" >&2; exit 2 ;;
    esac
done

if [ "$target" = local ]; then
    PROJ=$LOCAL_PROJ
    where=$PROJ            # rsync destination and the name shown in messages
else
    PROJ=$VM_PROJ
    where=$VM:$PROJ
fi

# Run a command on whichever side we are targeting. The string is parsed by
# exactly one shell either way - a remote one under ssh, bash -c here - so a
# command that quotes correctly for one quotes correctly for the other, and
# the verify below can build a single command string for both.
run_there() {
    if [ "$target" = local ]; then bash -c "$1"; else ssh "$VM" "$1"; fi
}

# rsync wants "host:path" for the VM and a bare path locally.
dest() { if [ "$target" = local ]; then printf '%s' "$1"; else printf '%s:%s' "$VM" "$1"; fi; }

[ -d "$SRC_PROJ/src" ] || { echo "sync.sh: no $SRC_PROJ/src" >&2; exit 1; }

if [ "$target" = vm ] &&
   ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$VM" true 2>/dev/null; then
    echo "sync.sh: cannot reach '$VM' over ssh." >&2
    echo "         If this says 'No route to host', the VM is simply not" >&2
    echo "         running - that is the first thing to check, not the" >&2
    echo "         network. Start it with:" >&2
    echo "           virsh -c qemu:///system start ubuntu24.04" >&2
    echo "         Or build on this machine instead: tools/sync.sh --local" >&2
    exit 1
fi

run_there "test -d '$PROJ/src'" || {
    echo "sync.sh: $PROJ/src does not exist${target:+ }on ${where%%:*}." >&2
    if [ "$target" = local ]; then
        echo "         Set LOCAL_PROJ, or set the tree up first - see" >&2
        echo "         hema-local/docs/LOCAL_BUILD.md." >&2
    else
        echo "         Check VM_PROJ - it must be the project *inside* the SDK" >&2
        echo "         tree, which is the one the makefile builds from." >&2
    fi
    exit 1
}

# ---------------------------------------------------------------- transfer
# --delete is the whole reason this is rsync and not scp: a source file
# removed here has to disappear there too, or it keeps being compiled in.
# Sources only - the build's own output stays on the VM, since deleting it
# every sync would mean a full rebuild every time.
if [ "$mode" != check ]; then
    echo "-> $where/src"
    rsync -a --delete --itemize-changes \
          --include='*/' --include='*.c' --include='*.h' --exclude='*' \
          "$SRC_PROJ/src/" "$(dest "$PROJ/src/")"

    # tools/ too: mksuota.py and flash.sh are run on the build machine, and a
    # stale copy of a *script* is the same trap as a stale copy of a source.
    # flash.sh was fixed here once and kept failing there for exactly this
    # reason.
    echo "-> $where/tools"
    rsync -a --delete --itemize-changes \
          --include='*/' --include='*.py' --include='*.sh' --include='*.jlink' \
          --exclude='*' \
          "$HERE/tools/" "$(dest "$PROJ/tools/")"
fi

# ------------------------------------------------------------------ verify
# Checksums both ways, because "rsync exited 0" and "the two trees agree" are
# different claims and only the second one matters.
echo
echo "verifying..."
# Built as a function rather than a printf format: the command contains
# backslash escapes for find's parentheses, and printf would eat them.
sum_cmd() {
    echo "cd '$1' && find . \\( -name '*.c' -o -name '*.h' \\)" \
         "-exec md5sum {} + | sort -k2"
}
here_sums=$(bash -c "$(sum_cmd "$SRC_PROJ/src")")
there_sums=$(run_there "$(sum_cmd "$PROJ/src")")

if [ "$here_sums" = "$there_sums" ]; then
    n=$(printf '%s\n' "$here_sums" | wc -l)
    echo "OK - $n source files identical on both sides"
else
    echo
    echo "MISMATCH - the two trees disagree. Left is the repo, right is $where:" >&2
    diff <(printf '%s\n' "$here_sums") <(printf '%s\n' "$there_sums") \
        | sed 's/^/    /' >&2
    echo >&2
    echo "Do not build until this is clean: the binary would not be built" >&2
    echo "from the source you are reading." >&2
    exit 1
fi

# ------------------------------------------------------------------- build
if [ "$mode" = build ]; then
    echo
    run_there "cd '$PROJ/e2studio/DA14585' && make -j4 all" | tail -5
    echo
    echo "Watch the text/data/bss line above. If it did not move after a real"
    echo "change, the build did not see it - which now means the makefile, not"
    echo "the sync, since the sync verified."
fi
