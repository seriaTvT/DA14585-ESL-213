#!/usr/bin/env python3
"""Build a full SPI-flash image with our firmware in a SUOTA image bank.

This tag does NOT boot an AN-B-001 image from flash offset 0. Its secondary
bootloader lives in OTP (proved on hardware: after a full chip erase, forcing
the ROM to re-run its boot scan still produced the stock bootloader's vector
table in SysRAM, which erased flash cannot supply). That bootloader ignores
offset 0 and instead reads a *product header* to find two SUOTA image banks,
picks the valid one with the newest image id, checks its CRC, copies it to
SysRAM and jumps. So a bootable firmware has to be a SUOTA image in a bank,
not a raw image at offset 0 - see tools/mkbootimg.py for that other format,
which is the right one only when the ROM itself does the loading.

Layout on this tag (recovered from the stock 256 KiB dump):

    0x000000  AN-B-001 image (unused here - OTP wins)
    0x002000  image bank 1   <- we replace this one
    0x014000  image bank 2   (left as the stock app, so a bad build falls back)
    0x038000  product header: 0x7052, then bank1/bank2 offsets as LE uint32

Image header is 64 bytes (SDK's s_imageHeader, utilities/secondary_bootloader):
signature[2]=0x70,0x51 | validflag=0xAA | imageid | code_size | CRC |
version[16] | timestamp | encryption | reserved[31], payload follows at +0x40.

CRC is plain zlib crc32 over the payload - confirmed by recomputing both stock
banks' headers exactly. The payload is the raw linker .bin (vector table
first), NOT the AN-B-001-wrapped one.

Headers are SYNTHESISED, not copied from a stock image. Until 2026-08-12 this
tool copied the stock bank's header and patched only the fields we understood,
on the grounds that `encryption`, `reserved` and anything else the vendor's
bootloader might check should stay byte-identical to an image it already boots.
That caution is now known to be unnecessary: the OTP bootloader has been
disassembled and it reads only

    signature[2], validflag, imageid, code_size, CRC, encryption

from the 64-byte header, and only `0x7052` plus the two bank offsets from the
product header. `version`, `timestamp` and `reserved[31]` are never read by it,
and the product header's bytes 2-3 are never examined. Full derivation, with
addresses and the checks it does make, in

    hema-local/docs/BOOT_CONTRACT.md

So a stock dump is no longer a source of unknowns. It is still useful for one
thing - supplying a known-good image for the *other* bank, so a bad build falls
back to something that works - and that is what --fallback is for.

`version` and `timestamp` are still written, because the SUOTA *receiver* reads
them even though the bootloader does not. See build_header().

Usage:  mksuota.py [--fallback <stock_dump.bin>] [--bootloader <boot.bin>]
                   [--otp-boot] <firmware.bin> <out.bin> [bank]
        bank defaults to 1.

        mksuota.py --ota <firmware.bin> <out.img>

        mksuota.py <stock_dump.bin> <firmware.bin> <out.bin> [bank]
        mksuota.py --ota <stock_dump.bin> <firmware.bin> <out.img>
            the older positional forms; a leading dump is taken as --fallback.

--ota needs no stock image at all: it emits just the 64-byte header and the
payload, which is exactly what a SUOTA client sends.

A full flash image without --fallback is synthesised from scratch. That image
still needs a secondary bootloader at offset 0 on every board except Type 1,
whose bootloader lives in OTP - so pass --bootloader, or --otp-boot to say the
tag does not need one. Without either, this refuses rather than write an image
that cannot boot.

--ota writes just the bank contents - the 64-byte image header followed by the
payload - instead of a whole flash image. That is exactly what a SUOTA client
sends over the air: the receiver takes the header from the first block, sizes
the transfer from its code_size, and writes header and payload into whichever
bank it picked. So the two outputs are the same bytes for the same firmware,
and the difference is only whether they arrive over SWD or over BLE.

The header is built the same way for both, which is the point of doing it here
rather than in the client: `imageid` is the one field the receiver overwrites
(it assigns its own), and everything else - including the `encryption` and
`reserved` fields nobody has decoded - stays copied from a stock header the
bootloader already accepts.
"""
import struct
import sys
import zlib

HDR_LEN = 64
IMG_SIG = b'\x70\x51'
PROD_SIG = b'\x70\x52'
VALID = 0xAA

# Must match EPD_STORE_ADDR / EPD_STORE_SECTOR in src/platform/epd_store.c.
STORE_ADDR = 0x03F000
STORE_SECTOR = 4096

# The retail flash layout, identical on every factory tag dumped so far
# (types 1, 2, 3 and 4). The community-reflashed Type 1's 0x002000/0x014000 is
# an artefact of that reflash, not a second vendor layout.
PROD_HDR_OFF = 0x038000
RETAIL_BANK1 = 0x004000
RETAIL_BANK2 = 0x01F000
# Bytes 2-3 are never examined by the bootloader - the community image's
# `00 00` boots exactly as well as retail's. Written to match retail anyway,
# because matching what the vendor writes costs nothing and "ignored today" is
# a weaker guarantee than "identical to something known to work".
PROD_HDR_MAGIC = b'\x12\x34'
# The board record: panel model, pin-map selector, and a packed pin map. Only
# a variant-A board carries one; variant B is the firmware's built-in default,
# so an erased record is meaningful rather than missing. Decoded by
# src/epd/epd_board.c; derivation in hema-local/docs/TAG_VARIANTS.md.
BOARD_REC_OFF = 0x039000


def bootloader_picks(id1: int, id2: int) -> int:
    """Which bank the OTP bootloader boots, given both banks' imageids.

    Transcribed from the decision function at 0x07FC01A0 (see BOOT_CONTRACT.md).
    Wraparound-aware newest-wins, bank 1 taking ties. Note that a plain
    "higher id wins" gets the 0xFF/0x00 pair backwards, which is not academic:
    the Type 2 factory tag ships exactly that pair and runs bank 2.

    Only meaningful when BOTH banks are valid; with one valid bank the
    bootloader takes it regardless of id.
    """
    if id1 == 0xFF and id2 == 0x00:
        return 2
    if id2 == 0xFF and id1 == 0x00:
        return 1
    return 2 if id1 < id2 else 1


def find_product_header(flash: bytes) -> tuple:
    """Locate the product header and return (offset, bank1, bank2).

    0x7052 also occurs by chance inside ARM code, so a candidate only counts if
    both offsets it declares are in range, sector-aligned, and actually point at
    image headers.
    """
    for off in range(0, len(flash) - 10, 2):
        if flash[off:off + 2] != PROD_SIG:
            continue
        b1, b2 = struct.unpack_from('<II', flash, off + 4)
        if not all(0 < b < len(flash) and b % 0x1000 == 0 for b in (b1, b2)):
            continue
        if flash[b1:b1 + 2] == IMG_SIG and flash[b2:b2 + 2] == IMG_SIG:
            return off, b1, b2
    raise ValueError("no product header found - is this a full flash dump?")


# Offsets into the 64-byte image header. From s_imageHeader in the SDK's
# utilities/secondary_bootloader/includes/bootloader.h, which is the definition
# the *bootloader* uses and therefore the one that matters:
#
#   0  signature[2]   2  validflag   3  imageid   4  code_size
#   8  CRC           12  version[16]            28  timestamp
#  32  encryption    33  reserved[31]
#
# Counted out here rather than left implicit because getting them wrong is not
# a visible failure. Writing the timestamp at 32 instead of 28 lands on
# `encryption`, and a non-zero `encryption` makes the bootloader run a decrypt
# pass over the whole payload (0x07FC1360) *before* it takes the CRC - so the
# image verifies perfectly against this tool, is garbled in RAM, fails its CRC
# on the tag and falls back to the other bank silently. Done exactly that.
VERSION_OFF = 12
VERSION_LEN = 16        # IMAGE_HEADER_VERSION_SIZE
TIMESTAMP_OFF = 28
ENCRYPTION_OFF = 32
COMPAT_PREFIX = b'HEMA-COMPAT-'


def compat_of(payload: bytes) -> bytes:
    """The build's own compatibility identity, read out of the binary.

    Read from the image rather than taken as an argument for the same reason
    tools/flash.sh reads its stamps there: the binary is the only thing that
    knows what it was built for, and anything typed alongside it can be wrong.
    See HEMA_COMPAT_STR in src/config/tag_types.h.
    """
    at = payload.find(COMPAT_PREFIX)
    if at < 0:
        return b''
    end = payload.index(b'\0', at)
    s = payload[at + len(COMPAT_PREFIX):end]
    if len(s) > VERSION_LEN:
        raise ValueError(f"compatibility string {s!r} is {len(s)} bytes; the "
                         f"image header field holds {VERSION_LEN}")
    return s


def build_header(payload: bytes, imageid: int) -> bytes:
    """Synthesise the 64-byte image header. No stock image involved.

    Every byte not written here stays zero. The bootloader reads none of them,
    and zero is what the vendor's own factory images carry in those fields.
    """
    h = bytearray(HDR_LEN)
    h[0:2] = IMG_SIG
    h[2] = VALID
    h[3] = imageid
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    struct.pack_into('<I', h, 4, len(payload))
    struct.pack_into('<I', h, 8, crc)

    # version[16] and timestamp are together what the SUOTA receiver compares
    # against the image already in the bank it is about to write
    # (app_read_image_headers: equal version AND equal timestamp is
    # IMAGE_HEADER_SAME_VERSION, which it refuses as SUOTAR_SAME_IMG_ERR). The
    # stock images leave both all-zero, so before this every image looked
    # identical to every other and the receiver refused every update. They are
    # not decoration.
    #
    # The two carry different questions on purpose:
    #   version   - what this image is FOR. Two builds sharing it are
    #               interchangeable as far as the panel is concerned.
    #   timestamp - which build it IS. crc32 of the payload, so it is derived
    #               from the content rather than the clock: builds stay
    #               reproducible, and re-pushing a byte-identical image is
    #               correctly refused while a rebuild with real changes is not.
    compat = compat_of(payload)
    h[VERSION_OFF:VERSION_OFF + VERSION_LEN] = compat.ljust(VERSION_LEN, b'\0')
    struct.pack_into('<I', h, TIMESTAMP_OFF, crc)

    # `encryption` must be zero, and now for a known reason rather than because
    # the stock image happened to have it zero: a non-zero byte here sends the
    # bootloader through its decrypt routine before the CRC, and nothing
    # downstream of this tool would notice. The fields either side of it are the
    # two written just above, so assert rather than trust the arithmetic.
    if h[ENCRYPTION_OFF] != 0:
        raise ValueError(
            f"header build set the encryption byte at offset {ENCRYPTION_OFF} "
            f"to 0x{h[ENCRYPTION_OFF]:02X}. The bootloader would decrypt the "
            f"payload, fail its CRC and fall back to the other bank, silently. "
            f"Check VERSION_OFF/TIMESTAMP_OFF against s_imageHeader.")
    if len(h) != HDR_LEN:
        raise ValueError(f"header is {len(h)} bytes, not {HDR_LEN}")
    return bytes(h)


def board_record(compat: bytes) -> bytes | None:
    """The 16-byte record at 0x039000, derived from the image's own identity.

    Reproduces exactly what a factory tag carries, which is worth stating
    because it is not the obvious layout. The panel byte is written on EVERY
    board; it is the SELECTOR that distinguishes the variants, and an erased
    selector is variant B's positive answer rather than an absence:

        Type 1  14 ff ff...              A53, built-in map  -> variant B
        Type 4  09 ff ff...              A41, built-in map  -> variant B
        Type 3  09 01 ff*6 21 22 10 01 20 07 11 23          -> variant A

    Returns None if the identity cannot be read, in which case the caller
    should leave the sector erased rather than guess - a wrong record is worse
    than none, because the firmware trusts it.
    """
    if not compat:
        return None
    s = compat.decode(errors='replace')          # e.g. "T3A-104x212-W7"
    try:
        variant = s[2]
        geom = s.split('-')[1]
    except (IndexError, ValueError):
        return None
    if variant not in 'AB' or geom not in ('104x212', '122x250'):
        return None

    rec = bytearray(b'\xFF' * 16)
    rec[0] = 0x09 if geom == '104x212' else 0x14
    if variant == 'A':
        rec[1] = 0x01
        rec[8:16] = bytes((0x21, 0x22, 0x10, 0x01, 0x20, 0x07, 0x11, 0x23))
    return bytes(rec)


def synth_flash(size: int, boot: bytes, otp_boot: bool,
                compat: bytes = b'') -> bytearray:
    """A blank retail-layout flash image: erased, plus bootloader and header."""
    flash = bytearray(b'\xFF' * size)
    if boot:
        # AN-B-001: 0x7050, then the payload length as a big-endian u32 at +4.
        flash[0:8] = b'\x70\x50\x00\x00' + struct.pack('>I', len(boot))
        flash[8:8 + len(boot)] = boot
    struct.pack_into('<II', flash, PROD_HDR_OFF + 4, RETAIL_BANK1, RETAIL_BANK2)
    flash[PROD_HDR_OFF:PROD_HDR_OFF + 2] = PROD_SIG
    flash[PROD_HDR_OFF + 2:PROD_HDR_OFF + 4] = PROD_HDR_MAGIC
    print(f"synthesised flash: product header @ 0x{PROD_HDR_OFF:06x}, banks "
          f"0x{RETAIL_BANK1:06x}/0x{RETAIL_BANK2:06x}")
    # The board record. This used to be left erased with a warning, on the
    # grounds that the firmware picked its wiring at build time anyway. The
    # firmware now READS this record to decide which eight pins to drive, so an
    # erased sector on a variant-A tag is no longer a lost annotation - it is
    # that tag being told it is variant B, and driving variant B's pins into a
    # dead panel. Written from the image's own identity instead.
    rec = board_record(compat)
    if rec:
        flash[BOARD_REC_OFF:BOARD_REC_OFF + len(rec)] = rec
        print(f"  board record @ 0x{BOARD_REC_OFF:06x}: "
              + ' '.join(f'{b:02x}' for b in rec[:2]) + " ... "
              + ' '.join(f'{b:02x}' for b in rec[8:]))
    else:
        # Guessing here would be worse than not writing: the firmware trusts
        # this, so a wrong record is a wrong pin map.
        print(f"  WARNING: board record @ 0x{BOARD_REC_OFF:06x} left erased - "
              f"could not read the\n           image's HEMA-COMPAT stamp. The "
              f"firmware will read this tag as\n           variant B. Use "
              f"--fallback to keep the tag's own record.")
    print(f"  bootloader @ 0x000000: "
          + (f"{len(boot)} bytes" if boot
             else "NONE - relying on the OTP boot chain (--otp-boot)"))
    return flash


def main() -> int:
    args = []
    ota = False
    fallback = boot_path = None
    otp_boot = False
    argv = sys.argv[1:]
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--ota':
            ota = True
        elif a == '--fallback':
            i += 1; fallback = argv[i]
        elif a == '--bootloader':
            i += 1; boot_path = argv[i]
        elif a == '--otp-boot':
            otp_boot = True
        elif a.startswith('-'):
            print(f"unknown option: {a}\n"); print(__doc__); return 2
        else:
            args.append(a)
        i += 1

    # Legacy positional forms put the stock dump first, so they carry one more
    # positional than the new ones: <dump> <fw> <out> [bank] against
    # <fw> <out> [bank]. Only three-without---ota is ambiguous, and there the
    # trailing bank number settles it. --ota used to take a dump it never
    # actually needed; accept it and use it as the fallback if one is wanted.
    if fallback is None:
        if ota and len(args) == 3:
            fallback = args.pop(0)
        elif not ota and len(args) == 4:
            fallback = args.pop(0)
        elif not ota and len(args) == 3 and not args[2].isdigit():
            fallback = args.pop(0)

    if len(args) not in (2, 3) or (ota and len(args) != 2):
        print(__doc__)
        return 2
    payload = open(args[0], 'rb').read()
    out_path = args[1]
    bank = int(args[2]) if len(args) == 3 else 1

    if ota:
        flash, ph, b1, b2 = None, None, None, None
    elif fallback:
        flash = bytearray(open(fallback, 'rb').read())
        ph, b1, b2 = find_product_header(flash)
        print(f"fallback image: {fallback}")
        print(f"product header @ 0x{ph:06x}: bank1=0x{b1:06x} bank2=0x{b2:06x}")
    else:
        boot = open(boot_path, 'rb').read() if boot_path else b''
        if not boot and not otp_boot:
            print("mksuota.py: refusing to build a full flash image with no "
                  "bootloader at offset 0.\n"
                  "  Types 2, 3 and 4 boot from there and would not come up. "
                  "Pass --bootloader\n"
                  "  <boot.bin> (re/type2/bootloader.bin is byte-identical on "
                  "every retail tag),\n"
                  "  or --otp-boot for a Type 1, whose bootloader is in OTP "
                  "and which ignores\n  offset 0. Or --fallback <dump> to base "
                  "the image on a stock one.", file=sys.stderr)
            return 2
        # compat_of() again here rather than reusing the value computed further
        # down: the board record has to be written while the image is being
        # laid out, and that happens before the header work.
        flash = synth_flash(0x80000, boot, otp_boot, compat_of(payload))
        ph, b1, b2 = PROD_HDR_OFF, RETAIL_BANK1, RETAIL_BANK2

    if ota:
        # The receiver assigns its own imageid and picks the bank itself, so
        # nothing here is tied to either.
        imageid, other_id = 1, None
    else:
        target, other = (b1, b2) if bank == 1 else (b2, b1)

        # The bank must not run into whatever follows it.
        limit = min(x for x in (b1, b2, ph, len(flash)) if x > target)
        need = HDR_LEN + len(payload)
        if need > limit - target:
            raise ValueError(f"image needs {need} bytes, bank {bank} holds "
                             f"{limit - target} (up to 0x{limit:06x})")

        # Outrank the other bank. With the bootloader's rule in hand this is
        # exact rather than heuristic: +1 wins outright, except against 0xFF
        # where 0 wins by the wraparound case. Both hold for either bank.
        other_valid = (bytes(flash[other:other + 2]) == IMG_SIG
                       and flash[other + 2] == VALID)
        other_id = flash[other + 3]
        if not other_valid:
            imageid = 1        # sole valid bank: the bootloader takes it
        else:
            imageid = 0 if other_id == 0xFF else other_id + 1
            # Check the choice against the bootloader's own rule rather than
            # trusting the arithmetic. Getting this wrong boots the OTHER bank,
            # which looks like the flash silently not having taken.
            ids = (imageid, other_id) if bank == 1 else (other_id, imageid)
            if bootloader_picks(*ids) != bank:
                raise ValueError(
                    f"imageid {imageid} would not win bank {bank} against "
                    f"{other_id}: the bootloader picks bank "
                    f"{bootloader_picks(*ids)}. See bootloader_picks().")

    hdr = build_header(payload, imageid)

    compat = compat_of(payload)
    if not compat:
        print("WARNING: no HEMA-COMPAT- stamp in this firmware, so the image "
              "header carries\n         no identity. A client cannot tell "
              "which tag it suits, and the\n         receiver will refuse it "
              "as a duplicate of anything else unstamped.\n"
              "         Rebuild with tools/build.sh.")
    else:
        print(f"compatibility: {compat.decode()}")

    if ota:
        # No padding: the receiver reads code_size out of the header and stops
        # there, and every byte sent is a byte over the air.
        open(out_path, 'wb').write(hdr + payload)
        print(f"{out_path}: {HDR_LEN + len(payload)} bytes "
              f"({HDR_LEN} header + {len(payload)} payload), "
              f"crc=0x{zlib.crc32(payload) & 0xFFFFFFFF:08x}")
        print("  imageid is a placeholder - the receiver assigns its own, and "
              "picks the bank\n  itself, so this image is not tied to either.")
        return 0

    flash[target:limit] = (hdr + payload).ljust(limit - target, b'\xFF')

    # Blank the sector epd_store.c persists the display template into, so a
    # freshly flashed tag starts with no stored face and comes up on the
    # built-in one. Carrying the stock firmware's bytes here would leave a
    # record that fails its magic/CRC check anyway - blanking just makes the
    # "nothing saved yet" case explicit rather than an error path.
    flash[STORE_ADDR:STORE_ADDR + STORE_SECTOR] = b'\xFF' * STORE_SECTOR
    print(f"store sector @ 0x{STORE_ADDR:06x}: blanked ({STORE_SECTOR} bytes)")

    open(out_path, 'wb').write(flash)
    other_desc = (f"other bank has {other_id}" if other_valid
                  else "other bank is not a valid image")
    print(f"bank {bank} @ 0x{target:06x}: {len(payload)} bytes, "
          f"imageid={imageid} ({other_desc}), "
          f"crc=0x{zlib.crc32(payload) & 0xFFFFFFFF:08x}")
    if not other_valid:
        print("  NOTE: no fallback. A bad build here has nothing to fall back "
              "to.\n        Pass --fallback <stock_dump.bin> to keep a "
              "known-good image in the\n        other bank.")
    print(f"{out_path}: {len(flash)} bytes")
    return 0


if __name__ == '__main__':
    sys.exit(main())
