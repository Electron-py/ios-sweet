# Flashing iOS 16 onto fleur (Redmi Note 11S, MT6781)

Complete instructions for a second machine. Everything below has been run on
the reference device; steps marked **(reference device only)** were verified
there and may differ on another unit.

> **This erases Android's user data.** The iOS root is written into the
> `userdata` partition. Bootloaders, recovery, modem, persist and calibration
> are never touched, and nothing is written outside `userdata` and one boot
> partition.

## What actually gets flashed

| File | Written to | Purpose |
| --- | --- | --- |
| `shim-fleur-<name>.img` | `boot_b` (**or your active slot**) | Boot shim + XNU kernel + Apple device tree + boot-args, packed as an Android `boot.img` v2 |
| `ios16-root-20H392-v2.img.fixed*.simg` | `userdata` | One 25 GiB journaled HFS+ volume holding the iOS 16.7.16 root filesystem, cryptexes, dyld shared cache and system keybag. Delivered as a ~11.3 GiB sparse file; the `.fixedN` suffix is just the revision it was built from |

Only the first file is sent with the repo; it is **not** reproducible from git
alone (see the appendix). The root image is a device-independent 25 GiB sparse
file and is also not in git.

## What you need

- A **fleur** (Redmi Note 11S, MediaTek MT6781) with an **unlocked bootloader**
- A Linux PC with `fastboot` (Android platform-tools) and `python3`
- `shim-fleur-*.img` and the root image (both sent separately — neither is in
  git)
- Your own stock `boot.img` for the device — this is the only rollback path

Known-good shim checksums (sha256), so you can confirm the transfer:

```
c8da528d6b9cdd1f83e529535c59825bd12f927697a0602ece4c486ecc55dcf9  shim-fleur-nohome.img
c6da2eeff7153ec09c9b67edc4a65984c1d90c57489e7c0f604eaff5cc1d8b1f  shim-fleur-touchkeys7.img
```

```sh
fastboot oem device-info        # Xiaomi: "Device unlocked: true"
fastboot getvar product         # must be your device's codename
fastboot getvar current-slot    # a or b
```

**Do not use `fastboot boot`.** fleur's LK has no `fastboot fetch`, so it
cannot pull an image over USB — it only boots what is already in the slot.
Always `flash`, never `boot`.

## 1. Flash the root (destructive, once per device)

Skip this whole step if `userdata` already holds the iOS root (you are only
updating the shim).

```sh
ROOT=/path/to/ios16-root-20H392-v2.img.fixed6.simg   # the root image you were sent
fastboot -S 512M flash userdata "$ROOT"
```

- Takes a while; **do not unplug**.
- Xiaomi's fastboot often hangs after a large flash: it has finished writing —
  hold Power + Volume down to re-enter fastboot and continue.
- If the last chunk fails with `sparse image write failure`, flash the tail on
  its own (this is a known LK/libsparse interaction, see
  `tools/sparse-tail.py`):

```sh
python3 tools/sparse-tail.py "$ROOT" tail.simg
fastboot -S 512M flash userdata tail.simg
```

## 2. Flash the shim

The reference device boots **slot b**, so it uses `boot_b`. Flash the
partition belonging to your *active* slot from step 0 — the shim has nothing
slot-specific in it.

```sh
fastboot flash boot_b shim-fleur-nohome.img
fastboot reboot
```

That is the entire procedure. `boot_a`, recovery, modem and persist are never
written.

## 3. What you should see

1. Stock Xiaomi bootloader, then the **Apple logo with kernel console text** on
   top of it.
2. First boot is slow — several minutes on the logo. The root volume has to
   replay its journal and the shared cache is cold.
3. The console text **disappears exactly when SpringBoard starts**. That is
   deliberate: the kernel mutes the framebuffer console on SpringBoard exec
   (patch 0086) and keeps logging to the persistent ring instead. It is not a
   hang.
4. Lock screen appears → **swipe up with the finger** (touch works).
5. Volume up/down work. Power may or may not, see below.

Known state of the build: it presents as an iPhone X (`iPhone10,3`), so there
is no Home button; rendering is CPU-only at roughly 20–30 fps; there is no
Wi-Fi, Bluetooth, cellular, audio, camera or sensors; you cannot leave an app
yet (the home gesture is a stub).

Two shim variants exist and differ only in boot-args:

| Image | Difference |
| --- | --- |
| `shim-fleur-nohome.img` | **recommended** — nothing presses Home for you, so you judge the buttons honestly |
| `shim-fleur-touchkeys7.img` | adds `sweethidtest=8`: a synthetic Home press every 8 s, useful if the lock screen cannot be left |

Both have `sweettouch=1 sweetkeys=1` (touch and keypad drivers on). To turn a
driver off without rebuilding, edit the boot-args in the payload with
`tools/payload-bootargs.py`.

## 4. If it does not boot: read the log

The kernel keeps a persistent 4 MiB ring, header `SWEETLOG`, at
`0x1dc9bf7000`, flushed every 10 s. It survives a reboot, so you can read it
after the fact.

Panics are always drawn on the panel regardless of the console mute, so look at
the screen first: an `IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6`
panic means step 1 was skipped or the root image is wrong.

To pull the ring, power the phone off completely (15 s), unplug, then use
`mtkclient` in BROM mode (needs `preloader_fleur.bin` from a dump of a fleur —
device-family specific, keep it out of git):

```sh
python3 mtk.py --preloader images/preloader_fleur.bin \
    --loader mtkclient/Loader/MTK_DA_V6.bin \
    ro 0x1dc9bf7000 0x401000 ring.bin
python3 tools/sweet-log.py ring.bin ring.txt
grep -a 'SWEET\|panic\|CRASH' ring.txt | tail -50
```

`Handshake failed` while retrying is normal until the phone is fully off;
`Auth file is required` in BROM mode is expected and harmless.

## 5. Going back to Android

```sh
fastboot flash boot_b /path/to/your/stock-boot.img
fastboot reboot
```

Android will then find an HFS+ `userdata` it cannot mount and drop into
recovery asking for a factory reset, which reformats `userdata` and finishes
the rollback (expected; not tested on a second unit). Have your stock images
handy: `boot`, `dtbo`, `vbmeta`, `persist`, `modemst1`, `modemst2`, `fsg`,
`fsc` are per-device and must never be taken from another unit.

## Troubleshooting

| Symptom | Cause | Fix |
| --- | --- | --- |
| `fastboot devices` is empty | Cable/port, or not in fastboot | Hold Volume down + Power; use a USB 2 port |
| Flashed, but Android still boots | Wrong partition for your slot | `fastboot getvar current-slot`, flash that slot's boot partition |
| Panic `SWEET UFS root ufsroot0 unavailable: 6` | No iOS root in `userdata` | Run step 1 |
| Sits on the logo > 6 min | Something in the boot path stalled | Pull the ring (step 4) |
| `sparse image write failure` at the end of the root flash | Known LK/libsparse tail bug | Flash the tail alone with `tools/sparse-tail.py` |
| Bootloader rejects the modified boot image | Device-specific verified boot | Device-specific: `fastboot flash vbmeta <img> --disable-verity --disable-verification`. Never needed on the reference device |
| Touch dead but the banner shows `SWEET FLEUR TOUCH` | Old shim | Flash a current image (the point-count fix landed in `ce4747b`) |

## Appendix: rebuilding from the repo

Everything below needs artifacts that are deliberately **not** in git (Apple
firmware, device dumps, generated build trees).

1. **Kernel** — GitHub Actions, `workflow_dispatch` on `.github/workflows/xnu.yml`:

   ```sh
   gh workflow run xnu.yml -R <your-fork>/ios-sweet --ref fleur \
       -f kernel_config=DEVELOPMENT -f machine_config=SWEET -f macos_version=13.5
   ```

   ~17 minutes. Download the `xnu-kernel` artifact and take the top-level
   `kernel.development.*` file.

2. **Payload** (kernel + device tree + boot-args):

   ```sh
   python3 tools/mkpayload.py kernel.development.<arch> fleur-product.adt payload.bin \
       --cmdline 'debug=0xc serial=3 cpus=8 keepsyms=1 ... sweettouch=1 sweetkeys=1'
   python3 tools/payload-bootargs.py payload.bin     # verify
   ```

   The full boot-arg line lives in the handoff notes (`NOTES.md`) and in the
   local `build-new-image.sh`. `ufslun=2` is mandatory: the patch series makes
   the LUN boot-arg driven and it defaults to 0, but fleur's GPT and the log
   ring live on LUN 2.

3. **Shim** (needs `aarch64-linux-gnu-gcc`, `mkbootimg`, the stock ramdisk and
   the stock MTK DTB table):

   ```sh
   shim/build-fleur.sh payload.bin     # -> shim/out/shim-fleur.img
   ```

4. **Root** (needs `sudo`, `apfs-fuse` with LZBITMAP support, and the three
   IPSW DMGs):

   ```sh
   sudo tools/rebuild-fleur-root.sh    # -> ios16-root-20H392-v2.img + .simg
   ```

   The script refuses to finish unless the dyld shared cache, both cryptexes
   and the system keybag are present — without them launchd dies immediately
   with `Library not loaded: /usr/lib/libSystem.B.dylib`.

## Further reading

- `README.md` — project overview, what works and what does not
- `NOTES.md` — engineering log: every boot, crash, register map and decision,
  including the touch/keypad bring-up evidence
- `docs/QEMU_TESTING.md` — kernel-only test boots without hardware
