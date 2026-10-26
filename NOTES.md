# iOS 16 on fleur (Redmi Note 11S, MT6781) - working notes

## Goal
Port the iOS 16.7.16 XNU userland stack to the Redmi Note 11S "fleur" (MT6781) and reach a usable SpringBoard UI; Wi-Fi/BLE may stay broken.

## Hardware facts (from Android DT + IPSW extraction)

- **SoC**: MediaTek MT6781 (6× A55 + 2× A75)
- **RAM**: 6 GB (LK patches `/memory` to `0x40000000 + 0x1_80000000`)
- **Display**: 1080×2400, FB base `0x7C010000`, pitch 4352 B (1088 px stride)
- **GICv3**: GICD `0x0C000000` (0x40000), GICR `0x0C040000` (0x200000)
- **Timer**: 13 MHz (`0xC65D40`); PPIs 13/14/11/10 → INTIDs 29/30/27/26
- **PSCI**: 1.1, `method = "smc"`; SYSTEM_RESET + CPU_ON supported
- **Storage**: UFS at `0x11270000` (vcc-supply = MT6358 LDO_VEMC)
- **PMIC**: MT6358 (via pwrap@1000d000)
- **Touch**: FocalTech FTS on I2C (`i2c@1100d000`/`i2c@11016000`)
  - IRQ GPIO 15, Reset GPIO 16, AVDD GPIO 58, max 10 fingers
- **Buttons**: Volume up/down + Power via MTK PMIC/GPIO keys

## Boot flow
- LK (no `fastboot fetch`) → shim flashed to `boot_b`
- Shim runs at EL1 (no EL2 drop needed), disables MTK toprgu WDT
- iOS root = HFS+ in `userdata` partition (LUN 2)
- Ring buffer at `0x1dc9bf7000` (4 MiB, header `SWEETLOG`)

## Current state (2026-10-26, updated 14:30)

### ✅ Working
- Full 8-core boot (`cpus=8`)
- HFS+ root mounts rw from `userdata`
- Launchd + userspace up
- SpringBoard **home screen reached** (lock screen clock → synthetic Home press → home screen)
- Console logging (persistent ring, `/dev/null` tee for SpringBoard/lsd/datamigrator/installd)
- SMP stack sampler (`sweetsample`), heap text dumper (`sweetscan`), crash reports
- ADT with `/product` node (iPhone10,3 identity) — unblocked `LSApplicationRecord`
- **Patch 0083 committed**: synthetic input boot-args (`sweeptouch`, `sweethidkey`)
- **Patch 0084 + 0085 compiled into the kernel** (CI runs **40324405863** and
  **40328758579** green: `CC sweet_fleur_touch.o`, `CC sweet_fleur_keys.o`)
- **CI build green** (run 40203488333, macos-14, Xcode 15.4)

### ⚠️ Missing (blockers for "usable")
| Feature | Hardware | What's needed |
|---------|----------|---------------|
| **Touch** | FocalTech FTS, I2C, IRQ=15, RST=16, AVDD=58 | **Patch 0084 in `touchkeys4`** — device test pending |
| **Buttons** | PMIC/GPIO (Vol+/Vol-/Power) | **Patch 0085 in `touchkeys4`** — device test pending |
| **Background/wallpaper** | CPU renderer path (no GPU) | QuartzCore CPU renderer + wallpaper service |

### 🛠 Tools written (committed in `tools/`)
- `payload-bootargs.py` — edit boot-args in a packed payload
- `replace-adt.py` — splice ADT in place, validates dram-base/size
- `mkadt.py` — builds fleur ADT (from `research/ipsw-x/d22ap.adt`)
- `rebuild-fleur-root.sh` / `mkiosroot.sh` — HFS+ root image with dyld cache
- `sparse-tail.py` — finishes sparse flash (LK has no `fetch`)
- `sweet-log.py` / `clean-console.py` — ring decode + ANSI strip

## Image variants (all share the same kernel + ADT, differ only in boot-args)

| Image | Boot-args | Purpose |
|-------|-----------|---------|
| `shim-fleur-gestalt.img` | `sweetfb_noconsole=1 sweetsample=… sweetnull=… sweetscan=…` | muted console, sampler+tee |
| `shim-fleur-verbose.img` | same + **no** `sweetfb_noconsole` | live console on screen |
| `shim-fleur-hidtest.img` | verbose + `sweethidtest=8` | synthetic Home press every 8 s |
| `shim-fleur-hidtest-8c.img` | hidtest + `cpus=8` | 8 cores + synthetic Home |
| `shim-fleur-touchkeys3.img` | ci2 line + **`ufslun=2`** | booted past the root, panicked in `ft_thread` |
| `shim-fleur-touchkeys4.img` | touchkeys3 + GPIO offset fix | candidate: touch/keys with mapping prints |

## Next device cycle (automated)
1. **Flash**: Vol-Down + Power → fastboot → `/tmp/opencode/flash-watch.sh`
   flashes `boot_b` and reboots (console visible on this boot, so the
   `SWEET FLEUR TOUCH/KEYS` banner and any `SWEET UFS:` line can be read)
2. **Pull ring**: Power off (15 s), plug USB → BROM read → `tools/sweet-log.py`

## Environment notes
- `/tmp/opencode` was wiped by a machine reboot (2026-10-26 13:27). Recovered:
  CI buildtree artifact (`bt/buildtree.tgz`), ADT re-extracted from
  `shim/out/shim-fleur-touchkeys-ci2.img` → `fleur-product.adt`, Android DT →
  `fleur.dts`, `console14.txt` from the ring. Build script:
  `/tmp/opencode/build-new-image.sh <run-id> <name> [extra boot-args…]`.
- The `xnu-kernel` artifact now ships a `kernel.development.*.link/` directory
  too — pick the top-level regular file (`-maxdepth 1 -type f`).

## The `IOFindBSDRoot` panic (fixed 2026-10-26)

Boots of `shim-fleur-touchkeys-final.img` and `-ci2.img` died with:

```
panic(cpu 0 …): IOFindBSDRoot: SWEET UFS root ufsroot0 unavailable: 6 @IOKitBSDInit.cpp:778
```

`6` is `ENXIO`, returned by `sweet_ufs_init()`. Nothing in those kernels was
broken: patch **0082** made `su.lun` come from the `ufslun` boot-arg and it
**defaults to 0**, but the boot lines never carried `ufslun=2`. LUN 0 is a
4 MiB MTK boot LUN with no GPT, so `ufs_find_partition()` printed
`SWEET UFS: LUN 0 has no GPT` and returned ENXIO.

Why it worked before: the kernels of the good boots (`hidtest-8c`, `gestalt`,
`verbose`) predate 0082 — they contain no `ufslun` string at all — and
`tools/patch-fleur-ufslun.py` had rewritten their instructions so `su.lun = 2`
unconditionally. That tool only matches the pre-0082 instruction pattern, so
from 0082 onward the boot line is the only way to pick the LUN.

Two diagnostics that make this confusion easy to repeat:
- `tools/patch-fleur-ufslun.py` reports `found 0` (or `found 2`) sites — that
  means the kernel is not pre-0082 anymore; use `ufslun=2` instead.
- `ufslun` appears in the kernel binary iff patch 0082 is present:
  `python3 -c "print(open('kernel','rb').read().count(b'ufslun'))"`.

Also note: the ring (`sweetlog`) only starts once UFS init *succeeds*
(`sweet_log_start()` runs at the end of `sweet_ufs_init()`), so a root-mount
failure leaves **no** log — the framebuffer (or the panic screen) is the only
record. The panic's `printf`s above the backtrace are the ones to read.

**Fix: `ufslun=2` is now part of the base boot line in the image build.**

## The `ft_thread` data abort (fixed 2026-10-26)

With `ufslun=2` the root mounted (52 threads, stackshot, userspace up) and the
next panic was ours:

```
panic(cpu 2 …): Kernel data abort. at pc 0xfffffe00074eb62c, lr 0xfffffe00074eb680
```

`llvm-symbolizer --obj=kern/kernel.development.sweet` puts both in **`ft_thread`**
(the artifact's kernel + dSYM symbolicate on Linux, no Xcode needed), and the
disassembly says:

```
74eb624:  dmb   oshst
74eb628:  lsl   x8, x0, #1        ; base * 2
74eb62c:  str   wzr, [x8, #0x13c0]  ; <-- fault
```

The `GPIO_*` macros already contained the base (`GPIO_MODE(base,pin) = base +
0x1000 + 0x40*pin`) while `ft_wr()`/`ft_rd()` add it again, so **every GPIO
access went to `2 * base + off`**. `0x13c0 = 0x1000 + 0x40*15` is IRQ GPIO 15 —
the first hardware write after mapping. Doubling a mapping from `io_submap`
(`0xfffffe401e2a0000 - 0xfffffe401eaa0000`, printed at boot) lands in unmapped
space → translation fault (ESR `0x96000045`, DFSC level 1).

The keys driver was unaffected (its `KP_*` constants are plain offsets), which
is why it ran first without dying. Fixed: GPIO macros now yield offsets only,
and both drivers print their `ml_io_map()` results and refuse a non-canonical
mapping instead of panicking.

Beware of the on-screen panic text: it wraps at ~34 characters and the
transcribed registers can lose digits (`x26` printed as `0xfffffe00000528a8`
where the code says `0xfffffe00080528a8`). Trust the disassembly over a
hand-copied register dump.

## iOS root volume size

- The HFS+ root is created by `tools/mkiosroot.sh` (`truncate -s SIZE`,
  `mkfs.hfsplus -s -J`) and **now defaults to 25 GiB** (was 16 GiB;
  `rebuild-fleur-root.sh` passes it explicitly).
- userdata is 107.29 GiB, so 25 GiB still leaves ~82 GiB untouched and stays
  far below the sweetlog ring (last 4 MiB of userdata).
- Rebuild needs root (apfs-fuse, loop mounts, rsync):
  `sudo tools/rebuild-fleur-root.sh /home/tom/Downloads/ipsw/<new-name>.img`
  (it refuses to overwrite an existing output), then `img2simg` runs inside it.
  The result is only on the device after a **destructive `userdata` flash** —
  confirm with the user first.

## Patch workflow (0084/0085 and anything new)

- Hand-written diff hunks are unreliable — CI silently *skips* a malformed
  patch and the run still goes green. Only a compile error fails a run.
- Always regenerate: `/tmp/opencode/gen-patches.py` extracts the exact CI
  buildtree baseline (`gh run download <id> -n xnu-buildtree`), applies the
  driver sources + hooks, writes the diff with `git diff`, then verifies with
  `git apply --check`.
- Check every run for the word `Skipping patch … 0084/0085` before believing
  a kernel contains a driver; also grep the payload for a driver string
  (`SWEET FLEUR TOUCH: starting`) before flashing.

## CI kernel builds
- Workflow: `.github/workflows/xnu.yml` (macos-14 runner, Xcode 15.x)
- Last run (39994505551) **cancelled** (runner brownout), not a code failure
- Fresh dispatch: `gh workflow run xnu.yml -R Electron-py/ios-sweet --ref fleur`

## Immediate plan
1. ✅ Commit patch **0083** (synthetic input: `sweeptouch`, `sweethidkey`) — tested via `sweethidtest`
2. ✅ Dispatch CI build to verify toolchain green (run 40203488333 **success**)
3. ✅ Write **patch 0084**: MTK I2C controller + FocalTech FTS touch driver (committed)
4. ✅ Write **patch 0085**: MTK keypad + PMIC power key driver (committed)
5. ✅ Build & test kernel with 0084+0085 on device — `touchkeys3` (CI
   40324405863) booted to the point of our own `ft_thread` data abort;
   **CI 40328758579 green with the GPIO fix → `shim-fleur-touchkeys4.img`
   built, flash armed, boot pending**
6. Rebuild the iOS root at **25 GiB** (`sudo tools/rebuild-fleur-root.sh <new>.img`),
   then flash `userdata` (destructive — ask first)
7. Investigate CPU wallpaper renderer (QuartzCore `CA::OGL::SW`)

## Relevant files
- `/tmp/opencode/console14.txt` — last ring decode (172246 B)
- `/tmp/opencode/gen-patches.py` — regenerate 0084/0085 from the CI baseline
- `/tmp/opencode/console7.txt` — 8-core hidtest boot (248888 B, 27 samples, 24 swaps)
- `/tmp/opencode/fleur-dt/fleur.dts` — Android DT (touch@0 = focaltech,fts)
- `/home/tom/Documents/musi/ios-sweet/xnu/patches/0083-sweet-synthetic-input.patch` — ready
- `/home/tom/Documents/musi/ios-sweet/tools/` — all tooling
- `/home/tom/Downloads/ipsw/…` — IPSW source (git-ignored)

## Flash / recovery
- `fastboot flash boot_b <img>` (slot b current)
- Restore Android: `fastboot flash boot_b ../Mu-Silicium/stockboot.img`
- `userdata` is the only destructive target; bootloaders/recovery/modem/persist preserved

---

*Updated 2026-10-26 14:30: panic #1 = missing `ufslun=2` (patch 0082's boot arg); panic #2 = GPIO macros double-adding the base in `ft_thread`; both fixed, CI 40328758579 green, `touchkeys4` ready to flash; root volume raised 16 → 25 GiB (rebuild pending).*