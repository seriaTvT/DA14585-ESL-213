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


def build_header(stock_hdr: bytes, payload: bytes, imageid: int) -> bytes:
    h = bytearray(stock_hdr)
    h[0:2] = IMG_SIG
    h[2] = VALID
    h[3] = imageid
    struct.pack_into('<I', h, 4, len(payload))
    struct.pack_into('<I', h, 8, zlib.crc32(payload) & 0xFFFFFFFF)
    return bytes(h)


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(__doc__)
        return 2
    flash = bytearray(open(sys.argv[1], 'rb').read())
    payload = open(sys.argv[2], 'rb').read()
    bank = int(sys.argv[4]) if len(sys.argv) == 5 else 1

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
    flash[target:limit] = (hdr + payload).ljust(limit - target, b'\xFF')

    # Blank the sector epd_store.c persists the display template into, so a
    # freshly flashed tag starts with no stored face and comes up on the
    # built-in one. Carrying the stock firmware's bytes here would leave a
    # record that fails its magic/CRC check anyway - blanking just makes the
    # "nothing saved yet" case explicit rather than an error path.
    flash[STORE_ADDR:STORE_ADDR + STORE_SECTOR] = b'\xFF' * STORE_SECTOR
    print(f"store sector @ 0x{STORE_ADDR:06x}: blanked ({STORE_SECTOR} bytes)")

    open(sys.argv[3], 'wb').write(flash)
    print(f"bank {bank} @ 0x{target:06x}: {len(payload)} bytes, "
          f"imageid={imageid} (other bank has {other_id}), "
          f"crc=0x{zlib.crc32(payload) & 0xFFFFFFFF:08x}")
    print(f"{sys.argv[3]}: {len(flash)} bytes")
    return 0


if __name__ == '__main__':
    sys.exit(main())
