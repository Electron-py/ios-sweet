#!/usr/bin/env python3
"""Show or edit the boot-args of a SWEET payload (tools/mkpayload.py).

  payload-bootargs.py PAYLOAD.bin                     show the current cmdline
  payload-bootargs.py PAYLOAD.bin -o OUT.bin --set NAME=VALUE [NAME=VALUE ...]
  payload-bootargs.py PAYLOAD.bin -o OUT.bin --unset NAME [NAME ...]

The 608-byte cmdline lives at offset 0x48 in the payload header, so boot-args
can be tuned without rebuilding the kernel or the ADT (mkpayload's --cmdline
only applies to a fresh payload).  Values are written back into the same
zero-padded field; the tool refuses to overflow it.  `--unset sweetfb_noconsole`
is how a build gets a console that never mutes (verbose panel).
"""
import os
import struct
import sys

CMDLINE, BOOT_LINE_LENGTH = 0x48, 608


def main(argv):
    src = argv[0]
    buf = bytearray(open(src, 'rb').read())
    if buf[:8] != b'SWEETXNU':
        sys.exit('not a SWEETXNU payload')
    raw = bytes(buf[CMDLINE:CMDLINE + BOOT_LINE_LENGTH])
    cur = raw.split(b'\0')[0].decode()
    if not any(a in ('--set', '--unset') for a in argv):
        print(cur)
        return

    args = cur.split()
    out, want = None, None		# want: 'out' | 'set' | 'unset'
    for a in argv[1:]:
        if want == 'out':
            out, want = a, None
        elif want == 'set':
            if '=' not in a:
                sys.exit(f'expected NAME=VALUE, got {a!r}')
            name, value = a.split('=', 1)
            args = [x for x in args if x.split('=', 1)[0] != name]
            args.append(f'{name}={value}')
            want = None
        elif want == 'unset':
            args = [x for x in args if x.split('=', 1)[0] != a]
            want = None
        elif a == '-o':
            want = 'out'
        elif a == '--set':
            want = 'set'
        elif a == '--unset':
            want = 'unset'
        else:
            sys.exit(f'unexpected argument {a!r}')
    if want:
        sys.exit('-o, --set and --unset all need a value')
    if out is None:
        sys.exit('-o OUT.bin is required with --set/--unset')

    line = ' '.join(args).encode()
    if len(line) >= BOOT_LINE_LENGTH:
        sys.exit(f'boot-args too long: {len(line)} >= {BOOT_LINE_LENGTH}')
    buf[CMDLINE:CMDLINE + BOOT_LINE_LENGTH] = line.ljust(BOOT_LINE_LENGTH, b'\0')
    open(out, 'wb').write(buf)
    print(f'{out}: {os.path.basename(src)} ({len(buf)} bytes)')
    print(f'  {line.decode()}')


if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main(sys.argv[1:])
