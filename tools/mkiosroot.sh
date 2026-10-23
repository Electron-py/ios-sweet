#!/bin/bash
# Build the SWEET iOS root: one case-sensitive HFS+ image holding an iOS OS
# root filesystem plus its cryptexes, for the userdata partition (rd=ufsroot0).
#
#   mkiosroot.sh OS.dmg SYSTEMOS.dmg APPOS.dmg OUT.img [SIZE]     (SIZE default 16G)
#
# The IPSW images are raw APFS with LZBITMAP-compressed files: they are read
# through apfs-fuse (decompressing), so the HFS+ copy is uncompressed. On a
# device /private/preboot is its own volume holding the cryptexes, which
# /System/Cryptexes/{OS,App} link to; here they live inside the one volume.
# Owners, modes, symlinks and hard links are preserved (rsync -aH as root).
#
# The volume is journaled (XNU replays the journal after a panic; Linux only
# mounts it read-write with -o force and leaves the journal empty). SWEET
# additions: /System/Library/Caches/com.apple.dyld hard-links the SystemOS
# cryptex shared cache (ignition_level=1 looks there), and
# /private/var/keybags/systembag.kb is the plain-plist system keybag that
# keybagd reads with /defaults/no-effaceable-storage (tools/mkadt.py).
set -euo pipefail

[[ $# -ge 4 ]] || { sed -n '2,12p' "$0"; exit 2; }
os=$(realpath "$1") sysos=$(realpath "$2") appos=$(realpath "$3") out=$4 size=${5:-16G}
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

# apfs-fuse must know LZBITMAP (iOS 16 compresses most files with it).
# Fedora's 2020 package (0-33) does not and returns EIO on ~3/4 of the OS
# volume; sgan81/apfs-fuse from 2021 on does. Point APFS_FUSE at such a build.
apfs_fuse=${APFS_FUSE:-$(command -v apfs-fuse || true)}

# Pre-flight tool check
missing_tools=()
[[ -n $apfs_fuse ]] || missing_tools+=("apfs-fuse")
for tool in mkfs.hfsplus rsync; do
	command -v "$tool" >/dev/null 2>&1 || missing_tools+=("$tool")
done

if [[ ${#missing_tools[@]} -gt 0 ]]; then
	echo "Error: missing required host utilities: ${missing_tools[*]}" >&2
	echo "  Install instructions:" >&2
	echo "    Arch Linux:    sudo pacman -S apfs-fuse hfsprogs rsync" >&2
	echo "    Ubuntu/Debian: sudo apt install apfs-fuse hfsprogs rsync" >&2
	echo "    Fedora:        sudo dnf install apfs-fuse hfsplus-tools rsync" >&2
	exit 1
fi

work=$(mktemp -d)
mounts=()
cleanup() {
	local busy=0
	for m in "${mounts[@]}"; do
		if mountpoint -q "$m"; then
			sudo umount "$m" 2>/dev/null || true
		fi
		mountpoint -q "$m" && busy=1
	done
	# Never recurse into a directory that is still mounted (the root is writable).
	if [[ $busy == 0 ]]; then
		rm -f "$work"/*.dmg
		rmdir "$work"/* "$work" 2>/dev/null || true
	else
		echo "still mounted under $work: left in place" >&2
	fi
}
trap cleanup EXIT

# apfs-fuse splits its options on ',' (e.g. in iPhone10,3 paths): use links.
# Keep the .dmg name: apfs-fuse only parses UDIF images (the OS one is UDIF,
# the cryptexes are raw APFS) when the file name ends in .dmg.
apfs_mount() {
	ln -s "$1" "$work/$2.dmg"
	mkdir "$work/$2"
	# As root: a user's FUSE mount is unreadable to the root rsync below.
	sudo "$apfs_fuse" "$work/$2.dmg" "$work/$2"
	mounts+=("$work/$2")
}
apfs_mount "$os" os
apfs_mount "$sysos" sysos
apfs_mount "$appos" appos

truncate -s "$size" "$out"
mkfs.hfsplus -s -J -v iOSRoot "$out"
mkdir "$work/root"
sudo mount -t hfsplus -o loop,force "$out" "$work/root"
mounts=("$work/root" "${mounts[@]}")

copy() {
	echo "copying $1 -> ${2#"$work/root"}"
	sudo mkdir -p "$2"
	# apfs-fuse 0.2 returns EIO on some inline/LZBITMAP files (the zoneinfo
	# tree among them).  rsync keeps going and exits 23; recover those exact
	# files from the DMG with 7z below.
	set +e
	sudo rsync -aH --numeric-ids "$1/" "$2/" 2> >(sudo tee -a "$work/rsync-failed.log" >&2)
	local rc=$?
	set -e
	[[ $rc == 0 || $rc == 23 ]] || return "$rc"
}
copy "$work/os/root" "$work/root"
copy "$work/sysos/root" "$work/root/private/preboot/Cryptexes/OS"
copy "$work/appos/root" "$work/root/private/preboot/Cryptexes/App"

# Fill the files apfs-fuse could not read. 7z decodes the same APFS image
# without this bug; take uid/gid/mode from the (readable) source metadata.
# One 7z run per DMG keeps the index parse (several seconds) amortised.
if [[ -s $work/rsync-failed.log ]]; then
	echo "== recovering files apfs-fuse could not read"
	mkdir -p "$work/recover"
	sed -n 's/.*send_files failed to open "\([^"]*\)".*/\1/p' "$work/rsync-failed.log" |
		sort -u > "$work/failed.all"
	: > "$work/failed.os"; : > "$work/failed.sysos"; : > "$work/failed.appos"
	while IFS= read -r srcfile; do
		case $srcfile in
		"$work/os/root"/*)    echo "${srcfile#"$work/os/root"/}"    >> "$work/failed.os" ;;
		"$work/sysos/root"/*) echo "${srcfile#"$work/sysos/root"/}" >> "$work/failed.sysos" ;;
		"$work/appos/root"/*) echo "${srcfile#"$work/appos/root"/}" >> "$work/failed.appos" ;;
		*) echo "  unknown source: $srcfile" >&2 ;;
		esac
	done < "$work/failed.all"

	recover() { # LIST DMG SRCRoot DEST
		[[ -s $1 ]] || return 0
		local -a all
		mapfile -t all < "$1"
		local n=${#all[@]} i tmp=$work/recover/$(basename "$2" .dmg)
		echo "  $(basename "$2"): $n files"
		# One 7z command line holds at most ~2 MB, so batch the paths. A
		# fresh temp dir per batch keeps tmpfs usage bounded.
		for ((i = 0; i < n; i += 1500)); do
			local -a chunk=("${all[@]:i:1500}")
			rm -rf "$tmp"; mkdir -p "$tmp"
			# 7z may return 2 for warnings (e.g. symlinks it refuses) while
			# still extracting every wanted file; check each one below.
			sudo 7z x -y -o"$tmp" "$2" -- "${chunk[@]}" >/dev/null || true
			for rel in "${chunk[@]}"; do
				[[ -f $tmp/$rel ]] || { echo "  FAILED to recover $rel" >&2; exit 1; }
				read -r uid gid mode < <(sudo stat -c '%u %g %a' "$3/$rel")
				sudo mkdir -p "$4/$(dirname "$rel")"
				sudo install -o "$uid" -g "$gid" -m "$mode" "$tmp/$rel" "$4/$rel"
			done
			rm -rf "$tmp"
		done
		echo "  recovered $n files into ${4#"$work/root"}"
	}
	recover "$work/failed.os"    "$os"    "$work/os/root"    "$work/root"
	recover "$work/failed.sysos" "$sysos" "$work/sysos/root" "$work/root/private/preboot/Cryptexes/OS"
	recover "$work/failed.appos" "$appos" "$work/appos/root" "$work/root/private/preboot/Cryptexes/App"
fi

# Shared cache where dyld looks at ignition level 1: hard links, no extra space.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
sudo mkdir -p "$work/root/System/Library/Caches/com.apple.dyld"
for f in "$work/root/$dsc"/*; do
	sudo ln "$f" "$work/root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# dyld does not search the cryptexes at ignition level 1: hard-link every
# cryptex file that has no counterpart in the OS root (libobjc-trampolines,
# libffi-trampolines, libstdc++, ...) to its root path.
for c in OS App; do
	(cd "$work/root/private/preboot/Cryptexes/$c" && sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! sudo test -e "$work/root/$f" && ! sudo test -L "$work/root/$f"; then
			sudo mkdir -p "$work/root/$(dirname "$f")"
			src="$work/root/private/preboot/Cryptexes/$c/$f"
			# Linux hfsplus cannot hard-link symlinks: copy those.
			if sudo test -L "$src"; then sudo cp -P "$src" "$work/root/$f"; else sudo ln "$src" "$work/root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# logd's log stores, normally made at restore: it checks owner _logd (272)
# and mode, plus a .migrated marker, else asks a helper and crashes
# ("BUG IN LIBTRACE: Failed to migrate directory").
db="$work/root/private/var/db"
sudo install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
sudo install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo install -o 272 -g 272 -m 0644 /dev/null "$db/$d/.migrated"
done

# System keybag for the built-in AppleKeyStore (no passcode).
sudo install -d -m 0700 -o 0 -g 0 "$work/root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo install -m 0600 -o 0 -g 0 /dev/stdin "$work/root/private/var/keybags/systembag.kb"

df -h "$work/root"
sudo umount "$work/root"
# Linux hfsplus decomposes some non-ASCII names (localized Shortcuts examples)
# differently from Apple's tables; let fsck_hfs rewrite them, then verify.
fsck.hfsplus -f -y "$out" | grep -v 'Illegal name' || true
# A clean -y can still be followed by a -n that wants to replay the journal
# transaction fsck itself wrote; XNU replays that at mount, so not fatal.
if ! fsck.hfsplus -f -n "$out"; then
	if fsck.hfsplus -f -y "$out" >/dev/null; then
		echo "warning: fsck -n still reports pending journal repairs; continuing" >&2
	else
		echo "error: HFS+ volume is corrupt after repair" >&2
		exit 1
	fi
fi
