#!/bin/bash
# Build the fleur (Redmi Note 11S / MT6781) shim and wrap it in a boot.img v2
# identical in layout to the Mu-Silicium fleur images that LK accepts:
# gzipped kernel, stock ramdisk, MTK DTB table, pagesize 2048.
set -euo pipefail
cd "$(dirname "$0")"

MUSI=${MUSI:-/home/tom/Documents/musi/Mu-Silicium}
RAMDISK=${RAMDISK:-$MUSI/Resources/ramdisk}
# Optional argument: an XNU payload (tools/mkpayload.py) to put in the ramdisk
# slot.  The shim finds it via /chosen/linux,initrd-start and boots it; with no
# argument the stock Android ramdisk is used and the diagnostic report runs.
if [ $# -ge 1 ]; then
	RAMDISK=$1
fi
DTB=${DTB:-$MUSI/Resources/DTBs/fleur_stock_dtbtable.bin}
MKBOOTIMG=${MKBOOTIMG:-$MUSI/Resources/Scripts/mkbootimg.py}

CC=aarch64-linux-gnu-gcc
OBJCOPY=aarch64-linux-gnu-objcopy
CFLAGS="-O2 -ffreestanding -nostdlib -fno-pic -mgeneral-regs-only -mstrict-align -fno-stack-protector -Wall -Wextra"
mkdir -p out
for f in start.S shim.c console.c mmu.c fdt.c loader.c ufs.c; do
	$CC $CFLAGS -c $f -o out/${f%.*}.o
done
# The image runs wherever LK puts it with no relocation pass: only the
# header's image_size (start.o, linked at 0) may be an absolute address.
abs=$(aarch64-linux-gnu-readelf -r out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o | grep -c ABS64 || true)
[ "$abs" = 0 ] || { echo "error: $abs absolute relocations in C objects" >&2; exit 1; }
$CC $CFLAGS -static -T shim.ld -Wl,--build-id=none -Wl,--no-warn-rwx-segments -Wl,-N \
	out/start.o out/shim.o out/console.o out/mmu.o out/fdt.o out/loader.o out/ufs.o -o out/shim.elf
$OBJCOPY -O binary out/shim.elf out/Image
# LK copies the decompressed file; pad to image_size so .bss/stacks exist.
size=$(aarch64-linux-gnu-nm out/shim.elf | awk '$3=="_end"{print strtonum("0x"$1)}')
truncate -s "$size" out/Image

# Stock-style gzip stream (no FNAME/mtime, XFL=2, OS=3), as in Mu-fleur-white.img.
python3 - "$PWD/out/Image" "$PWD/out/Image.gz" <<'PY'
import sys, zlib
src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()
c = zlib.compressobj(9, zlib.DEFLATED, -zlib.MAX_WBITS)
body = c.compress(data) + c.flush()
gz = (b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\x03' + body +
      zlib.crc32(data).to_bytes(4, 'little') + len(data).to_bytes(4, 'little'))
open(dst, 'wb').write(gz)
print(f'shim: {len(data)} bytes -> gzip {len(gz)} bytes')
PY

python3 "$MKBOOTIMG" --kernel out/Image.gz --ramdisk "$RAMDISK" --dtb "$DTB" \
	--base 0x40078000 --kernel_offset 0x8000 --ramdisk_offset 0x07C08000 \
	--tags_offset 0x0BC08000 --dtb_offset 0x0BC08000 --pagesize 2048 \
	--header_version 2 --os_version 12.0 \
	--cmdline "bootopt=64S3,32N2,64N2 buildvariant=user" \
	-o out/shim-fleur.img
ls -l out/Image out/Image.gz out/shim-fleur.img
