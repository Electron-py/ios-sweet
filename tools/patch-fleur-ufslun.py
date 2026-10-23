#!/usr/bin/env python3
"""Hardcode LUN 2 in a SWEET kernel built before patch 0082.

sweet_ufs.c sets su.lun = 0 (sweet's GPT is LUN 0); fleur's GPT is LUN 2.
Patch 0082 adds the `ufslun` boot arg, but a macOS-14 CI brownout can make a
rebuild impossible for hours, so this rewrites the four instructions in
sweet_ufs_rootdev instead:

    str  x8, [x21, #0x10]        su.dma_pa
    strb wzr, [x21, #0x18]       su.lun = 0
    mov  w8, #0x200
    str  w8, [x21, #0x30]        su.secsize = DEV_BSIZE

into

    mov  w9, #2
    stp  x8, x9, [x21, #0x10]    dma_pa and lun = 2 (tag + padding zeroed)
    mov  w8, #0x200
    str  w8, [x21, #0x30]

Finding the pattern by value keeps this independent of the image layout.  Only
use it on a kernel built from patches 0001-0081 (before 0082); once the real
CI kernel with the boot arg exists, boot with ufslun=2 instead.
"""
import sys

OLD = bytes.fromhex('a80a00f9 bf620039 08408052 a83200b9'.replace(' ', ''))
NEW = bytes.fromhex('49008052 a82601a9 08408052 a83200b9'.replace(' ', ''))


def main() -> None:
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} KERNEL OUT")
    src, dst = sys.argv[1], sys.argv[2]
    buf = bytearray(open(src, 'rb').read())
    hits = [i for i in range(len(buf) - len(OLD) + 1)
            if buf[i:i + len(OLD)] == OLD]
    if len(hits) != 1:
        sys.exit(f"expected exactly one su.lun site in {src}, found {len(hits)}")
    off = hits[0]
    buf[off:off + len(OLD)] = NEW
    with open(dst, 'wb') as f:
        f.write(buf)
    print(f"{dst}: su.lun = 2 (file offset 0x{off:x})")


if __name__ == '__main__':
    main()
