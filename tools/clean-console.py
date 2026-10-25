#!/usr/bin/env python3
"""Turn the on-screen kernel console OFF inside a SWEET payload.

  clean-console.py PAYLOAD.bin OUT.bin

fbcon_probe() (xnu patch 0003) is the only reader of the ADT node
"sweet-fbcon": if it is missing the framebuffer console never registers, so
nothing is ever drawn over the GUI (the persistent ring in patch 0047 still
gets every line, but panic output no longer reaches the panel either - pull
the ring, or boot a payload without this change, to debug a hang).

Renaming the node in place keeps the payload byte-identical everywhere else:
the ADT blob keeps its size and offset, so the header and segment table need
no fixups.  The shim (shim/loader.c) notices that the node is gone and clears
the panel itself before jumping to xnu, because fbcon would have done it.

  tools/clean-console.py /tmp/opencode/payload-fleur-live.bin payload-clean.bin
"""
import os
import struct
import sys


def main(src, dst):
    buf = bytearray(open(src, 'rb').read())
    if buf[:8] != b'SWEETXNU':
        sys.exit('not a SWEETXNU payload')
    adt_off, adt_size = struct.unpack_from('<2Q', buf, 0x20)
    if adt_off + adt_size > len(buf):
        sys.exit('bad ADT range')
    adt = bytes(buf[adt_off:adt_off + adt_size])

    old, new = b'sweet-fbcon', b'sweet-fboff'
    assert len(old) == len(new)
    n = adt.count(old)
    if n != 1:
        sys.exit(f'expected exactly one "sweet-fbcon" in the ADT, found {n}')
    # Same length, so every offset in the payload stays valid.
    buf[adt_off + adt.find(old): adt_off + adt.find(old) + len(old)] = new

    # The ADT must still parse and must no longer carry the node.
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import adt  # noqa: E402
    root, end = adt.parse(bytes(buf[adt_off:adt_off + adt_size]))
    assert end == adt_size, (end, adt_size)
    names = []

    def walk(n):
        names.append(n.name)
        for c in n.children:
            walk(c)

    walk(root)
    assert 'sweet-fbcon' not in names and 'sweet-fboff' in names, names

    open(dst, 'wb').write(buf)
    print(f'{dst}: {len(buf)} bytes, ADT node renamed '
          f'({adt_size} bytes at {adt_off:#x}), {len(names)} nodes')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
