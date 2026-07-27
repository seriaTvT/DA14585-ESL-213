#!/usr/bin/env bash
#
# sync.sh - push this repo's firmware sources to the build tree on the VM.
#
#   tools/sync.sh              sync, then verify
#   tools/sync.sh --check      verify only, change nothing
#   tools/sync.sh --build      sync, verify, then build over there
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
# Configure with the environment if your paths differ:
#   VM        ssh host for the build machine        (default: vm)
#   VM_PROJ   the hema_epd_clock dir inside the SDK on that machine
set -euo pipefail

VM=${VM:-vm}
VM_PROJ=${VM_PROJ:-/home/nina/Downloads/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401/projects/target_apps/template/hema_epd_clock}

HERE=$(cd "$(dirname "$0")/.." && pwd)
LOCAL_PROJ="$HERE/firmware/hema_epd_clock"

mode=sync
case "${1:-}" in
    --check) mode=check ;;
    --build) mode=build ;;
    "")      ;;
    *)       sed -n '3,8p' "$0" >&2; exit 2 ;;
esac

[ -d "$LOCAL_PROJ/src" ] || { echo "sync.sh: no $LOCAL_PROJ/src" >&2; exit 1; }

if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$VM" true 2>/dev/null; then
    echo "sync.sh: cannot reach '$VM' over ssh." >&2
    echo "         If this says 'No route to host', the VM is simply not" >&2
    echo "         running - that is the first thing to check, not the" >&2
    echo "         network. Start it with:" >&2
    echo "           virsh -c qemu:///system start ubuntu24.04" >&2
    exit 1
fi

ssh "$VM" "test -d '$VM_PROJ/src'" || {
    echo "sync.sh: $VM_PROJ/src does not exist on $VM." >&2
    echo "         Check VM_PROJ - it must be the project *inside* the SDK" >&2
    echo "         tree, which is the one the makefile builds from." >&2
    exit 1
}

# ---------------------------------------------------------------- transfer
# --delete is the whole reason this is rsync and not scp: a source file
# removed here has to disappear there too, or it keeps being compiled in.
# Sources only - the build's own output stays on the VM, since deleting it
# every sync would mean a full rebuild every time.
if [ "$mode" != check ]; then
    echo "-> $VM:$VM_PROJ/src"
    rsync -a --delete --itemize-changes \
          --include='*/' --include='*.c' --include='*.h' --exclude='*' \
          "$LOCAL_PROJ/src/" "$VM:$VM_PROJ/src/"

    # tools/ too: mksuota.py and flash.sh are run on the VM, and a stale copy
    # of a *script* is the same trap as a stale copy of a source. flash.sh was
    # fixed here once and kept failing there for exactly this reason.
    echo "-> $VM:$VM_PROJ/tools"
    rsync -a --delete --itemize-changes \
          --include='*/' --include='*.py' --include='*.sh' --include='*.jlink' \
          --exclude='*' \
          "$HERE/tools/" "$VM:$VM_PROJ/tools/"
fi

# ------------------------------------------------------------------ verify
# Checksums both ways, because "rsync exited 0" and "the two trees agree" are
# different claims and only the second one matters.
echo
echo "verifying..."
local_sums=$(cd "$LOCAL_PROJ/src" && find . \( -name '*.c' -o -name '*.h' \) \
             -exec md5sum {} + | sort -k2)
remote_sums=$(ssh "$VM" "cd '$VM_PROJ/src' && find . \\( -name '*.c' -o -name '*.h' \\) \
              -exec md5sum {} + | sort -k2")

if [ "$local_sums" = "$remote_sums" ]; then
    n=$(printf '%s\n' "$local_sums" | wc -l)
    echo "OK - $n source files identical on both sides"
else
    echo
    echo "MISMATCH - the two trees disagree. Left is here, right is $VM:" >&2
    diff <(printf '%s\n' "$local_sums") <(printf '%s\n' "$remote_sums") \
        | sed 's/^/    /' >&2
    echo >&2
    echo "Do not build until this is clean: the binary would not be built" >&2
    echo "from the source you are reading." >&2
    exit 1
fi

# ------------------------------------------------------------------- build
if [ "$mode" = build ]; then
    echo
    ssh "$VM" "cd '$VM_PROJ/e2studio/DA14585' && make -j4 all" | tail -5
    echo
    echo "Watch the text/data/bss line above. If it did not move after a real"
    echo "change, the build did not see it - which now means the makefile, not"
    echo "the sync, since the sync verified."
fi
