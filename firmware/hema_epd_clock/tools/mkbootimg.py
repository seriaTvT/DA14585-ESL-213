#!/usr/bin/env python3
"""Wrap a raw DA14585 image in the AN-B-001 SPI boot header.

The DA14585 has no internal flash: its boot ROM reads the external SPI flash,
looks for the 0x70 0x50 preamble, copies `len` bytes into SysRAM at 0x07FC0000
and jumps there. A raw .bin from the linker has no such header and will not
boot - the ROM finds no preamble and falls through to the other boot sources.

Header layout, straight from the SDK's own utilities/mkimage/image.h:

    struct an_b_001_spi_header {
        uint8_t preamble[2];   /* 0x70 0x50               */
        uint8_t empty[4];      /* zeros                   */
        uint8_t len[2];        /* payload length, BIG-endian */
    };

Note this is NOT the same as mkimage's `single` command, which builds a
versioned 0x70 0x51 header for SUOTA. This is the plain bootable format.

Usage:  mkbootimg.py <raw.bin> <out.bin>
"""
import sys

HEADER_LEN = 8
# len is 2 bytes, so the ROM can copy at most 64 KiB - 1 in this format.
MAX_PAYLOAD = 0xFFFF


def make_boot_image(payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(
            f"image is {len(payload)} bytes; the AN-B-001 header's length "
            f"field is 16-bit, so it cannot exceed {MAX_PAYLOAD}")
    return (bytes([0x70, 0x50, 0, 0, 0, 0,
                   (len(payload) >> 8) & 0xFF,
                   len(payload) & 0xFF]) + payload)


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    payload = open(sys.argv[1], 'rb').read()
    img = make_boot_image(payload)
    open(sys.argv[2], 'wb').write(img)
    print(f"{sys.argv[1]}: {len(payload)} bytes payload")
    print(f"{sys.argv[2]}: {len(img)} bytes total "
          f"(header len field = 0x{len(payload):04x})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
