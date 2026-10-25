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

## Current state (2026-10-25, updated 16:15)

### ✅ Working
- Full 8-core boot (`cpus=8`)
- HFS+ root mounts rw from `userdata`
- Launchd + userspace up
- SpringBoard **home screen reached** (lock screen clock → synthetic Home press → home screen)
- Console logging (persistent ring, `/dev/null` tee for SpringBoard/lsd/datamigrator/installd)
- SMP stack sampler (`sweetsample`), heap text dumper (`sweetscan`), crash reports
- ADT with `/product` node (iPhone10,3 identity) — unblocked `LSApplicationRecord`
- **Patch 0083 committed**: synthetic input boot-args (`sweeptouch`, `sweethidkey`)
- **Patch 0084 committed**: MTK I2C + FocalTech FTS touch driver (in CI build tree)
- **CI build green** (run 40203488333, macos-14, Xcode 15.4)

### ⚠️ Missing (blockers for "usable")
| Feature | Hardware | What's needed |
|---------|----------|---------------|
| **Touch** | FocalTech FTS, I2C, IRQ=15, RST=16, AVDD=58 | **Patch 0084 written** — needs kernel build + test on device |
| **Buttons** | PMIC/GPIO (Vol+/Vol-/Power) | MTK PMIC/GPIO key driver (patch 0085 next) |
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

## Next device cycle (automated)
1. **Pull ring**: Power off (15 s), plug USB → BROM read → `/tmp/opencode/console8.txt`
2. **Flash next**: Vol-Down + Power → fastboot → watcher flashes new image

## CI kernel builds
- Workflow: `.github/workflows/xnu.yml` (macos-14 runner, Xcode 15.x)
- Last run (39994505551) **cancelled** (runner brownout), not a code failure
- Fresh dispatch: `gh workflow run xnu.yml -R Electron-py/ios-sweet --ref fleur`

## Immediate plan
1. ✅ Commit patch **0083** (synthetic input: `sweeptouch`, `sweethidkey`) — tested via `sweethidtest`
2. ✅ Dispatch CI build to verify toolchain green (run 40203488333 **success**)
3. ✅ Write **patch 0084**: MTK I2C controller + FocalTech FTS touch driver (committed)
4. **Next**: Write **patch 0085**: MTK PMIC/GPIO keys driver
5. **Next**: Build & test kernel with 0084 on device (ring pull → flash → boot)
6. Investigate CPU wallpaper renderer (QuartzCore `CA::OGL::SW`)

## Relevant files
- `/tmp/opencode/console8.txt` — next ring (armed)
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

*Updated 2026-10-25 after home-screen milestone (sweethidtest=8 unlocked to home screen).*