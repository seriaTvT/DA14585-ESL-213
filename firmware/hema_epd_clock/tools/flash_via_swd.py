#!/usr/bin/env python3
"""Program the tag's SPI flash over SWD, using our own flasher stub.

Runs on the machine the J-Link is attached to. See flash_writer.h for why this
exists rather than SmartSnippets or a J-Link flash bank.

Sequence: RAM-load the flasher build, chip-erase, stream the bootable image in
4 KiB chunks through the mailbox, then read it back and compare.

Data goes in with `w4` - never `loadbin`, which performs an implicit reset and
would restart the stub (and zero its .bss mailbox) mid-transfer. ~1024 w4
commands per chunk runs in well under a second, so this is not the bottleneck.

Usage:  flash_via_swd.py <boot_image.bin> <flasher.elf> <flasher.map>
"""
import re
import subprocess
import sys

CHUNK = 4096

FW_CMD_NONE, FW_CMD_INFO, FW_CMD_ERASE = 0, 1, 2
FW_CMD_WRITE, FW_CMD_READ, FW_CMD_SECTOR = 3, 4, 5
FW_ST_BUSY, FW_ST_OK, FW_ST_ERR = 0, 1, 2
FW_MAGIC = 0x464C5357

# Mailbox field offsets, mirroring fw_mailbox_t.
OFF_MAGIC, OFF_CMD, OFF_ADDR, OFF_LEN, OFF_STATUS, OFF_INFO = 0, 4, 8, 12, 16, 20
OFF_DATA = 24


def jlink(script: str) -> str:
    """Run one J-Link Commander script and return its output.

    NB: J-Link Commander parses numeric arguments as HEX, with or without an
    0x prefix - so a decimal `4096` is read as 0x4096. Every value this script
    emits is written with an explicit 0x and formatted as hex.

    Everything must happen in a SINGLE invocation: when JLinkExe exits it
    leaves the core reset/halted, so the stub does not survive between runs.
    Splitting this into per-step invocations looks like the stub silently
    dying (magic reads back 0 while the command sits unconsumed).
    """
    open('/tmp/fvs.jlink', 'w').write(script + '\nq\n')
    r = subprocess.run(
        ['JLinkExe', '-device', 'Cortex-M0', '-if', 'SWD', '-speed', '4000',
         '-autoconnect', '1', '-CommanderScript', '/tmp/fvs.jlink'],
        capture_output=True, text=True, timeout=1800)
    return r.stdout + r.stderr


def read_blocks(out: str):
    """Return every mem32 result in the transcript, in order, as word lists.

    Keyed by order rather than address: the same address is read many times
    over a run, so an address-keyed dict would collapse them all into the
    last one.
    """
    blocks, cur = [], None
    for line in out.splitlines():
        s = line.strip()
        if s.startswith('J-Link>'):
            if cur is not None:
                blocks.append(cur)
                cur = None
            if s.startswith('J-Link>mem32'):
                cur = []
            continue
        if cur is None:
            continue
        m = re.match(r'^[0-9A-Fa-f]{8} = (.*)$', s)
        if m:
            cur += [int(t, 16) for t in m.group(1).split()
                    if re.fullmatch(r'[0-9A-Fa-f]{8}', t)]
    if cur is not None:
        blocks.append(cur)
    return blocks


def find_symbol(mapfile: str, name: str) -> int:
    for line in open(mapfile):
        f = line.split()
        if len(f) >= 2 and f[-1] == name:
            return int(f[0], 16)
    raise SystemExit(f'symbol {name} not found in {mapfile}')


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    image = open(sys.argv[1], 'rb').read()
    elf, mapfile = sys.argv[2], sys.argv[3]
    mb = find_symbol(mapfile, 'fw_mb')
    print(f'image {len(image)} bytes, fw_mb @ 0x{mb:08x}')

    offsets = list(range(0, len(image), CHUNK))

    # Same dance as tools/ram_load.jlink: loadfile resets *before* it
    # downloads, so a second reset afterwards makes the core latch MSP/PC from
    # the freshly written vector table.
    def load_stub():
        return ['r', 'h', 'w2 0x50003300, 0x0008',
                f'loadfile {elf}',
                'r', 'h',
                'w2 0x50003300, 0x0008',
                'w2 0x50003102, 0x0001',
                'w2 0x50003100, 0x0005',
                'w2 0x50000012, 0x00A2',
                'g', 'sleep 600']

    # One J-Link process per chunk.
    #
    # A page program leaves the core unreachable by the debugger - "CPU could
    # not be halted" at any interface speed. It is NOT a crash and NOT a reset:
    # RAM trace markers show the whole chunk programming and the handler
    # returning cleanly (A4, done=0x1000), and a boot counter in reset-proof
    # RAM reads 1, so the part booted exactly once. Erase and read never do
    # this; only program does. Root cause unresolved.
    #
    # Since a reset always restores the debugger, the way through is to never
    # talk to the core after a program: do one chunk per process, then let the
    # process exit (which resets the part) and start clean for the next one.
    # Exactly ONE flash operation per process, and no status read afterwards.
    # A *real* sector erase (one with data to clear, rather than a no-op on
    # already-blank flash) kills the debugger just like a program does, so the
    # erase and the write cannot share a session either. Correctness is
    # established by the read-back pass instead of per-step status.
    def erase_chunk(off):
        lines = load_stub() + [
            'h', f'mem32 0x{mb:x}, 6',                       # stub alive?
            f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
            f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_SECTOR:x}',
            'g', 'sleep 2500']
        blocks = read_blocks(jlink('\n'.join(lines)))
        if not blocks or len(blocks[0]) < 6 or blocks[0][0] != FW_MAGIC:
            raise SystemExit(f'0x{off:05x}: stub did not start (erase)')

    def write_chunk(off):
        n = len(image[off:off + CHUNK])
        blk = image[off:off + CHUNK] + b'\xff' * (-n % 4)
        lines = load_stub() + ['h', f'mem32 0x{mb:x}, 6']    # stub alive?
        # Buffer fill with the core halted: a long w4 burst against a running
        # core dies ~100 words in and leaves the link broken.
        lines += [f'w4 0x{mb + OFF_DATA + i:x}, '
                  f'0x{int.from_bytes(blk[i:i + 4], "little"):08x}'
                  for i in range(0, len(blk), 4)]
        lines += [f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
                  f'w4 0x{mb + OFF_LEN:x}, 0x{n:x}',
                  f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_WRITE:x}',
                  'g', 'sleep 2500']
        out = jlink('\n'.join(lines))
        blocks = read_blocks(out)
        if not blocks or len(blocks[0]) < 6 or blocks[0][0] != FW_MAGIC:
            raise SystemExit(f'0x{off:05x}: stub did not start (write)')
        nfail = out.count('Failed to write memory')
        nhalt = out.count('could not be halted') + out.count('is not halted')
        if nfail or nhalt:
            print(f'    [warn] 0x{off:05x}: {nfail} failed writes, '
                  f'{nhalt} halt errors')
        return n

    def verify_chunk(off):
        n = len(image[off:off + CHUNK])
        lines = load_stub() + [
            'h',
            f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
            f'w4 0x{mb + OFF_LEN:x}, 0x{n:x}',
            f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_READ:x}',
            'g', 'sleep 1000', 'h',
            f'mem32 0x{mb:x}, 6',
            f'mem32 0x{mb + OFF_DATA:x}, 0x{(n + 3) // 4:x}',
        ]
        blocks = read_blocks(jlink('\n'.join(lines)))
        if len(blocks) < 2 or len(blocks[0]) < 6 or blocks[0][0] != FW_MAGIC:
            return None
        if blocks[0][1] != FW_CMD_NONE or blocks[0][4] != FW_ST_OK:
            return None
        raw = b''.join(w.to_bytes(4, 'little') for w in blocks[1])[:n]
        return raw

    print(f'programming {len(offsets)} chunks, one op per J-Link session...')
    for off in offsets:
        erase_chunk(off)
        n = write_chunk(off)
        print(f'  0x{off:05x}: erased + wrote {n} bytes')

    print('verifying...')
    bad = 0
    for off in offsets:
        n = len(image[off:off + CHUNK])
        raw = verify_chunk(off)
        if raw is None:
            bad += 1
            print(f'  0x{off:05x}: read-back failed')
        elif raw != image[off:off + n]:
            bad += 1
            first = next(k for k in range(n)
                         if raw[k:k + 1] != image[off + k:off + k + 1])
            print(f'  0x{off:05x}: MISMATCH at +0x{first:x} '
                  f'(got {raw[first]:#04x}, want {image[off + first]:#04x})')
        else:
            print(f'  0x{off:05x}: ok ({n} bytes)')
    if bad:
        raise SystemExit(f'{bad} of {len(offsets)} chunks bad')
    print(f'verified all {len(offsets)} chunks - power-cycle the tag to boot it')
    return 0


def _unused_single_session(image, mb, elf, offsets, load_stub):
    """Superseded by the per-chunk driver above; kept out of the path."""

    # Same dance as tools/ram_load.jlink: loadfile resets *before* it
    # downloads, so a second reset afterwards makes the core latch MSP/PC from
    # the freshly written vector table.
    def load_stub():
        return ['r', 'h', 'w2 0x50003300, 0x0008',
                f'loadfile {elf}',
                'r', 'h',
                'w2 0x50003300, 0x0008',
                'w2 0x50003102, 0x0001',
                'w2 0x50003100, 0x0005',
                'w2 0x50000012, 0x00A2',
                'g', 'sleep 400']

    # --- phase 1: erase + write, reloading the stub for every chunk ---------
    #
    # After a FW_CMD_WRITE completes the core stops answering the debugger -
    # "CPU could not be halted", and reads fail at any interface speed. It is
    # NOT a crashed write: RAM trace markers that survive a reset show the
    # whole chunk programmed and the handler returning cleanly (A4 = loop
    # finished, done = 4096). The core simply becomes unreachable afterwards.
    #
    # So don't try to talk to it after a write. Issue the write, give it time,
    # then reset and reload the stub for the next chunk - a reset always
    # recovers the part. Verification happens separately in phase 2, where
    # reads work normally.
    #
    # Sector erase, not FW_CMD_ERASE: a whole-chip erase drops the SWD link
    # mid-operation and is unrecoverable. We only occupy the first few sectors.
    lines = []
    for off in offsets:
        n = len(image[off:off + CHUNK])
        blk = image[off:off + CHUNK] + b'\xff' * (-n % 4)   # pad to a word
        lines += load_stub()
        lines += ['h',
                  f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
                  f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_SECTOR:x}',
                  'g', 'sleep 1500', 'h']
        # Buffer fill happens with the core HALTED: while it spins on the
        # mailbox, a long w4 burst dies ~100 words in and the link stays broken.
        lines += [f'w4 0x{mb + OFF_DATA + i:x}, '
                  f'0x{int.from_bytes(blk[i:i + 4], "little"):08x}'
                  for i in range(0, len(blk), 4)]
        lines += [f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
                  f'w4 0x{mb + OFF_LEN:x}, 0x{n:x}',
                  f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_WRITE:x}',
                  'g', 'sleep 1500']

    # --- phase 2: one fresh stub, read everything back ----------------------
    lines += load_stub()

    for off in offsets:                                 # then read-back pairs
        n = len(image[off:off + CHUNK])
        lines += ['h',
                  f'w4 0x{mb + OFF_ADDR:x}, 0x{off:x}',
                  f'w4 0x{mb + OFF_LEN:x}, 0x{n:x}',
                  f'w4 0x{mb + OFF_CMD:x}, 0x{FW_CMD_READ:x}',
                  'g', 'sleep 800', 'h',
                  f'mem32 0x{mb:x}, 6',
                  f'mem32 0x{mb + OFF_DATA:x}, 0x{(n + 3) // 4:x}', 'g']

    print(f'running {len(lines)} J-Link commands in one session...')
    out = jlink('\n'.join(lines))
    blocks = read_blocks(out)

    # Phase 1 reads nothing back, so every block belongs to phase 2: one
    # status + one data block per chunk.
    expected = 2 * len(offsets)
    if len(blocks) < expected:
        print(out[-3000:])
        raise SystemExit(f'expected {expected} mem32 blocks, got {len(blocks)}')

    bad = 0
    for i, off in enumerate(offsets):
        n = len(image[off:off + CHUNK])
        blk = blocks[2 * i]
        if len(blk) < 6 or blk[0] != FW_MAGIC:
            bad += 1
            print(f'  0x{off:05x}: stub not responding on read-back')
            continue
        _, cmd, _, _, status, _ = blk[:6]
        words = blocks[2 * i + 1]
        raw = b''.join(w.to_bytes(4, 'little') for w in words)[:n]
        if cmd != FW_CMD_NONE or status != FW_ST_OK:
            bad += 1
            print(f'  0x{off:05x}: read failed (cmd={cmd} status={status})')
        elif raw != image[off:off + n]:
            bad += 1
            first = next(k for k in range(n) if raw[k:k+1] != image[off+k:off+k+1])
            print(f'  0x{off:05x}: MISMATCH, first bad byte at +0x{first:x} '
                  f'(got {raw[first]:#04x}, want {image[off+first]:#04x})')
        else:
            print(f'  0x{off:05x}: ok ({n} bytes)')
    if bad:
        raise SystemExit(f'{bad} of {len(offsets)} chunks bad')
    print(f'verified all {len(offsets)} chunks - power-cycle the tag to boot it')
    return 0


if __name__ == '__main__':
    sys.exit(main())
