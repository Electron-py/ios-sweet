#!/bin/bash
# Rebuild the fleur iOS 16.7.16 root with the current tools/mkiosroot.sh,
# apply tools/sweet-root-tweaks.py, fsck and sparsify it.
#
#   sudo tools/rebuild-fleur-root.sh [OUT.img]
#
# The previous image (ios16-root-20H392.img) was built by an older mkiosroot
# and has no cryptexes, no dyld shared cache and no system keybag, so launchd
# panics with "Library not loaded: /usr/lib/libSystem.B.dylib (no dyld cache)".
# This build must contain:
#   private/preboot/Cryptexes/{OS,App}
#   System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64
#   private/var/keybags/systembag.kb
# and the script refuses to finish without them.
set -euo pipefail

[[ $EUID == 0 ]] || { echo "run this with sudo" >&2; exit 1; }
here=$(cd "$(dirname "$0")/.." && pwd)
dl=/home/tom/Downloads/ipsw
# Prefer a current apfs-fuse with LZBITMAP support over Fedora's 2020 build
# (which returns EIO on most of the iOS 16 OS volume).
[[ -x /usr/local/bin/apfs-fuse ]] && export APFS_FUSE=/usr/local/bin/apfs-fuse
img=${1:-$dl/ios16-root-20H392-v2.img}
[[ $img == *.img ]] || { echo "OUT must end in .img" >&2; exit 1; }
simg=${img%.img}.simg
for d in 087-86698-044 087-86593-047 087-86715-047; do
	[[ -f $dl/$d.dmg ]] || { echo "missing $dl/$d.dmg" >&2; exit 1; }
done

[[ ! -e $simg ]] || { echo "refusing to overwrite $simg" >&2; exit 1; }
if [[ ${RESUME:-0} == 1 ]]; then
	# Finish an image whose mkiosroot run already completed the copy.
	[[ -f $img ]] || { echo "RESUME=1 but $img does not exist" >&2; exit 1; }
else
	[[ ! -e $img ]] || { echo "refusing to overwrite $img" >&2; exit 1; }
	"$here/tools/mkiosroot.sh" \
		"$dl/087-86698-044.dmg" "$dl/087-86593-047.dmg" "$dl/087-86715-047.dmg" \
		"$img" 16G
fi

echo "== checking the built image"
fsck.hfsplus -fy "$img"
mnt=$(mktemp -d /tmp/opencode/fleur-root.XXXXXX)
mount -t hfsplus -o loop,force "$img" "$mnt"
cleanup() { umount "$mnt" 2>/dev/null || true; rmdir "$mnt" 2>/dev/null || true; }
trap cleanup EXIT

cache="$mnt/System/Library/Caches/com.apple.dyld"
cryptex="$mnt/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld"
fail=0
[[ -f $cryptex/dyld_shared_cache_arm64 ]] || { echo "FAIL: SystemOS cryptex has no dyld cache" >&2; fail=1; }
[[ -f $cache/dyld_shared_cache_arm64 ]]  || { echo "FAIL: /System cache was not hard-linked" >&2; fail=1; }
[[ -f $mnt/private/var/keybags/systembag.kb ]] || { echo "FAIL: no system keybag" >&2; fail=1; }
[[ -d $mnt/private/preboot/Cryptexes/App ]] || { echo "FAIL: no App cryptex" >&2; fail=1; }
[[ $fail == 0 ]] || exit 1

echo "== applying sweet-root-tweaks.py"
python3 "$here/tools/sweet-root-tweaks.py" "$mnt"

umount "$mnt"
rmdir "$mnt"
trap - EXIT
fsck.hfsplus -fy "$img"

echo "== sparsifying"
img2simg "$img" "$simg"
sha256sum "$img" "$simg" | tee "${img%.img}.sha256"
ls -lh "$img" "$simg"
echo "== done: $simg"
