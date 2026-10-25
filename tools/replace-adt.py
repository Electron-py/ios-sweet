#!/usr/bin/env python3
"""Replace the ADT inside an existing SWEET payload, keeping everything else.

  replace-adt.py PAYLOAD.bin ADT.bin OUT.bin

The payload layout (tools/mkpayload.py) puts the ADT at adt_off/adt_size in
the header, as the very last content: every segment's file_off+file_size is
<= adt_off, and the ADT ends at EOF (give or take zero padding).  So a new
ADT can be swapped in without rebuilding the kernel or touching the segment
table - only the 8-byte adt_size field changes.

This is how the /product node of the real DeviceTree comes back after
research/ipsw-x/d22ap.adt was lost: without it mkadt.py emits an ADT with no
/product, and MobileGestalt then has no product-name, artwork-device-idiom,
artwork-scale-factor, graphics-featureset-class, ... to answer from.

The new ADT must parse completely and must keep dram-base/dram-size equal to
the payload's phys_base/mem_size, or the kernel would map the wrong RAM.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

MAGIC = b'SWEETXNU'
ADT_OFF, ADT_SIZE = 0x20, 0x28
PHYS_BASE, MEM_SIZE = 0x38, 0x40
SEG_TABLE, SEG_ENTRY = 0x2a8, 32


def u64(buf, off):
    return struct.unpack_from('<Q', buf, off)[0]


def main(argv):
    if len(argv) != 3:
        sys.exit(__doc__)
    src, adt_src, dst = argv
    buf = bytearray(open(src, 'rb').read())
    if buf[:8] != MAGIC:
        sys.exit('not a SWEETXNU payload')

    adt_off, adt_size = u64(buf, ADT_OFF), u64(buf, ADT_SIZE)
    nsegs, = struct.unpack_from('<I', buf, 0x0c)
    if adt_off + adt_size > len(buf):
        sys.exit('bad ADT range in the payload header')

    # Nothing may live behind the ADT, or splicing would shift it.
    for i in range(nsegs):
        vm, vs, fo, fs = struct.unpack_from('<4Q', buf, SEG_TABLE + i * SEG_ENTRY)
        if fo + fs > adt_off:
            sys.exit(f'seg{i} ({fo:#x}+{fs:#x}) reaches past adt_off {adt_off:#x}')
    trailing = bytes(buf[adt_off + adt_size:])
    if trailing.strip(b'\0'):
        sys.exit(f'{len(trailing)} bytes after the ADT, not all zero')

    new = open(adt_src, 'rb').read()
    _, end = adt.parse(new)
    if end != len(new):
        sys.exit(f'{adt_src}: parses to {end}, file is {len(new)} bytes')

    root, _ = adt.parse(new)

    def find(node, key):
        """First value of key anywhere in the tree (dram-base sits in a child)."""
        for n, f, v in node.props:
            if n == key:
                return v
        for c in node.children:
            got = find(c, key)
            if got is not None:
                return got
        return None

    base, size = find(root, 'dram-base'), find(root, 'dram-size')
    if base is None or size is None:
        sys.exit(f'{adt_src}: no dram-base/dram-size anywhere')
    # Values are packed little-endian u64 (mkadt's u64()).
    base, size = int.from_bytes(base, 'little'), int.from_bytes(size, 'little')
    if base != u64(buf, PHYS_BASE) or size != u64(buf, MEM_SIZE):
        sys.exit(f'dram-base/size {base:#x}/{size:#x} != payload '
                 f'{u64(buf, PHYS_BASE):#x}/{u64(buf, MEM_SIZE):#x}')

    buf = bytearray(bytes(buf[:adt_off]) + new)
    struct.pack_into('<Q', buf, ADT_SIZE, len(new))
    open(dst, 'wb').write(buf)

    # Re-read and check the result really carries the new ADT.
    chk = open(dst, 'rb').read()
    off, size = u64(chk, ADT_OFF), u64(chk, ADT_SIZE)
    root2, end2 = adt.parse(bytes(chk[off:off + size]))
    if end2 != size:
        sys.exit(f'{dst}: ADT does not parse back ({end2} != {size})')
    has_product = any(dict((k, v) for k, f, v in c.props).get('name') == b'product\0'
                      for c in root2.children)
    print(f'{os.path.basename(dst)}: {len(buf)} bytes '
          f'(ADT {adt_size} -> {len(new)} at {off:#x}, payload grew by '
          f'{len(buf) - len(open(src, "rb").read()):+d})')
    print(f'  children: {[dict((k, v) for k, f, v in c.props).get("name") for c in root2.children]}')
    print(f'  /product node: {"yes" if has_product else "NO"}')


if __name__ == '__main__':
    main(sys.argv[1:])
