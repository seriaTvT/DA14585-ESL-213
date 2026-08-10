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

Rather than synthesise a header from scratch, the stock bank's header is copied
and only the fields we understand are patched. That keeps `encryption`,
`reserved` and anything else the vendor's bootloader may check byte-identical
to an image it already boots.

Usage:  mksuota.py <stock_flash_dump.bin> <firmware.bin> <out.bin> [bank]
        bank defaults to 1.

        mksuota.py --ota <stock_flash_dump.bin> <firmware.bin> <out.img>

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
# `encryption`, and the bootloader rejects a non-zero encryption *before* it
# checks the CRC - so the image verifies perfectly against this tool, boots
# nothing, and the tag silently falls back to the other bank. Done exactly that.
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


def build_header(stock_hdr: bytes, payload: bytes, imageid: int) -> bytes:
    h = bytearray(stock_hdr)
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

    # The bootloader refuses an image whose `encryption` byte is set, and it
    # refuses it *before* the CRC check, so nothing downstream of here would
    # notice: the image passes every check this tool makes and the tag quietly
    # boots the other bank instead. Since the fields either side of it are the
    # two written just above, assert rather than trust the arithmetic.
    if h[ENCRYPTION_OFF] != stock_hdr[ENCRYPTION_OFF]:
        raise ValueError(
            f"header build disturbed the encryption byte at offset "
            f"{ENCRYPTION_OFF} (0x{stock_hdr[ENCRYPTION_OFF]:02X} -> "
            f"0x{h[ENCRYPTION_OFF]:02X}). The bootloader would reject this "
            f"image and fall back to the other bank, silently. Check "
            f"VERSION_OFF/TIMESTAMP_OFF against s_imageHeader.")
    if len(h) != HDR_LEN:
        raise ValueError(f"header is {len(h)} bytes, not {HDR_LEN}")
    return bytes(h)


def main() -> int:
    args = sys.argv[1:]
    ota = False
    if args and args[0] == '--ota':
        ota = True
        args = args[1:]
    if len(args) not in (3, 4) or (ota and len(args) != 3):
        print(__doc__)
        return 2
    flash = bytearray(open(args[0], 'rb').read())
    payload = open(args[1], 'rb').read()
    out_path = args[2]
    bank = int(args[3]) if len(args) == 4 else 1

    ph, b1, b2 = find_product_header(flash)
    target, other = (b1, b2) if bank == 1 else (b2, b1)
    print(f"product header @ 0x{ph:06x}: bank1=0x{b1:06x} bank2=0x{b2:06x}")

    # The bank must not run into whatever follows it.
    limit = min(x for x in (b1, b2, ph, len(flash)) if x > target)
    need = HDR_LEN + len(payload)
    if need > limit - target:
        raise ValueError(f"image needs {need} bytes, bank {bank} holds "
                         f"{limit - target} (up to 0x{limit:06x})")

    # Outrank the other bank so findlatest() picks ours (it returns bank 1 on a
    # tie, and has special cases only for the 0xFF/0 wraparound pair).
    other_id = flash[other + 3]
    imageid = 1 if other_id == 0 else (other_id + 1) & 0xFF
    if imageid in (0, 0xFF):
        imageid = 1

    hdr = build_header(bytes(flash[target:target + HDR_LEN]), payload, imageid)

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
    print(f"bank {bank} @ 0x{target:06x}: {len(payload)} bytes, "
          f"imageid={imageid} (other bank has {other_id}), "
          f"crc=0x{zlib.crc32(payload) & 0xFFFFFFFF:08x}")
    print(f"{out_path}: {len(flash)} bytes")
    return 0


if __name__ == '__main__':
    sys.exit(main())
