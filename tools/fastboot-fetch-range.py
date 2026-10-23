#!/usr/bin/env python3
"""Fetch a byte range from a fastboot partition over USB (no whole-partition read).

`fastboot fetch PART OUT` can only fetch an entire partition.  The wire
protocol, however, supports ranges:

    fetch:PARTITION:0xOFFSET:0xSIZE

and answers with DATA<8 hex digits> + payload + OKAY.  This is what makes it
possible to read the SWEET persistent console ring at the end of the 107 GiB
`userdata` partition without downloading the whole thing.

Usage:
    tools/fastboot-fetch-range.py PARTITION OFFSET SIZE OUT [--vid 0x.. --pid 0x..]

OFFSET and SIZE accept 0x-prefixed hex.  The device must be in fastboot.
Needs pyusb and read access to /dev/bus/usb (the same udev rule fastboot uses).
"""

import argparse
import sys
import time

import usb.core
import usb.util

FASTBOOT_CLASS = 0xFF
FASTBOOT_SUBCLASS = 0x42
FASTBOOT_PROTOCOL = 0x03
TIMEOUT_MS = 30000


def find_fastboot(vid=None, pid=None):
    kw = {}
    if vid is not None:
        kw["idVendor"] = vid
    if pid is not None:
        kw["idProduct"] = pid
    devs = list(usb.core.find(find_all=True, **kw))
    for dev in devs:
        try:
            cfg = dev.get_active_configuration()
        except usb.core.USBError:
            continue
        for intf in cfg:
            if (intf.bInterfaceClass == FASTBOOT_CLASS
                    and intf.bInterfaceSubClass == FASTBOOT_SUBCLASS
                    and intf.bInterfaceProtocol == FASTBOOT_PROTOCOL):
                return dev, intf
    return None, None


def endpoints(intf):
    ep_in = ep_out = None
    for ep in intf:
        if usb.util.endpoint_direction(ep.bEndpointAddress) == usb.util.ENDPOINT_IN:
            ep_in = ep
        else:
            ep_out = ep
    if ep_in is None or ep_out is None:
        sys.exit("fastboot interface has no bulk endpoints")
    return ep_in, ep_out


_rx = bytearray()


def read_exact(ep, n):
    """Read exactly n bytes from a bulk endpoint.

    Always ask for at least one max-size packet: a shorter request makes
    libusb report "overflow" when the device's 512-byte packet arrives.
    """
    while len(_rx) < n:
        chunk = ep.read(max(512, n - len(_rx)), timeout=TIMEOUT_MS)
        if not chunk:
            raise IOError("short read from device")
        _rx.extend(chunk)
    out = bytes(_rx[:n])
    del _rx[:n]
    return out


def read_status(ep_in):
    """Return ('OKAY'|'FAIL'|'INFO', message) or ('DATA', length)."""
    hdr = read_exact(ep_in, 4)
    if hdr == b"DATA":
        return hdr, int(read_exact(ep_in, 8), 16)
    if hdr not in (b"OKAY", b"FAIL", b"INFO"):
        raise IOError(f"unexpected response {hdr!r}")
    n = int(read_exact(ep_in, 4), 16)
    msg = read_exact(ep_in, n) if n else b""
    return hdr, msg


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("partition")
    ap.add_argument("offset", type=lambda x: int(x, 0))
    ap.add_argument("size", type=lambda x: int(x, 0))
    ap.add_argument("out")
    ap.add_argument("--vid", type=lambda x: int(x, 0))
    ap.add_argument("--pid", type=lambda x: int(x, 0))
    ap.add_argument("--chunk", type=lambda x: int(x, 0), default=1 << 20,
                    help="max bytes per fetch request (default 1 MiB)")
    args = ap.parse_args()

    dev, intf = find_fastboot(args.vid, args.pid)
    if dev is None:
        sys.exit("no fastboot interface found (is the phone in fastboot?)")
    ep_in, ep_out = endpoints(intf)
    try:
        usb.util.claim_interface(dev, intf.bInterfaceNumber)
    except usb.core.USBError as e:
        if e.errno != 16:  # EBUSY: already claimed by us
            raise
    print(f"device {dev.idVendor:04x}:{dev.idProduct:04x} "
          f"if {intf.bInterfaceNumber} ep {ep_in.bEndpointAddress:#04x}/"
          f"{ep_out.bEndpointAddress:#04x}", file=sys.stderr)

    written = 0
    with open(args.out, "wb") as f:
        while written < args.size:
            n = min(args.chunk, args.size - written)
            cmd = f"fetch:{args.partition}:0x{args.offset + written:08x}:0x{n:08x}"
            ep_out.write(cmd.encode(), timeout=TIMEOUT_MS)
            while True:
                status, payload = read_status(ep_in)
                if status == b"INFO":
                    print(f"info: {payload.decode(errors='replace')}", file=sys.stderr)
                    continue
                break
            if status == b"FAIL":
                sys.exit(f"device refused {cmd}: {payload.decode(errors='replace')}")
            if status != b"DATA":
                sys.exit(f"unexpected {status!r} for {cmd}")
            if payload == 0:
                sys.exit("device returned an empty DATA section")
            remaining = payload
            while remaining:
                chunk = read_exact(ep_in, min(1 << 20, remaining))
                f.write(chunk)
                remaining -= len(chunk)
            status, payload2 = read_status(ep_in)
            while status == b"INFO":
                print(f"info: {payload2.decode(errors='replace')}", file=sys.stderr)
                status, payload2 = read_status(ep_in)
            if status != b"OKAY":
                sys.exit(f"fetch failed: {status!r} {payload2!r}")
            written += payload
            print(f"\r{written}/{args.size} bytes", end="", file=sys.stderr, flush=True)
    print(file=sys.stderr)


if __name__ == "__main__":
    main()
