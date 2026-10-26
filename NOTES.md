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

## Current state (2026-10-26, updated 17:00)

### ✅ Working
- Full 8-core boot (`cpus=8`)
- HFS+ root mounts rw from `userdata`
- Launchd + userspace up
- SpringBoard **home screen reached** (lock screen clock → synthetic Home press → home screen)
- **User sees the UI on screen** (ring flush #178 ≈ 30 min, no panic)
- Console logging (persistent ring, `/dev/null` tee for SpringBoard/lsd/datamigrator/installd)
- SMP stack sampler (`sweetsample`), heap text dumper (`sweetscan`), crash reports
- ADT with `/product` node (iPhone10,3 identity) — unblocked `LSApplicationRecord`
- **Patch 0083 committed**: synthetic input boot-args (`sweeptouch`, `sweethidkey`)
- **Patch 0084 + 0085 rewritten from the MT6781 kernel source** (commits `10a6eca`
  + `cae9e25`; CI runs **40362364957** and **40364755941** green, both patches
  applied with no `Skipping patch`, both `.o` files compile with zero errors)
- **CI build green** (run 40203488333, macos-14, Xcode 15.4)

### ⚠️ Missing (blockers for "usable")
| Feature | Hardware | What's needed |
|---------|----------|---------------|
| **Touch** | FocalTech FTS, I2C, IRQ=15, RST=16, AVDD=58 | register map rewritten (0084) — **boot test pending** |
| **Buttons** | MT6781 keypad matrix (GPIO80), PMIC power | register map rewritten (0085) — **boot test pending** |
| **Background/wallpaper** | CPU renderer path (no GPU) | QuartzCore CPU renderer + wallpaper service |
| **bluetoothd/fairplayd respawn** (83×) | root image | 3 hand-fixes lost in the v3 25 GiB rebuild — see below |

### Why touch/keys produced nothing (2026-10-26 analysis)

The first boot with 0084/0085 reached the home screen but printed only
`SWEET FLEUR TOUCH: no answer from FTS yet` / `I2C read failed, backing off`
and never a single `SWEET FLEUR KEYS:` line after the banner. Everything
below was **verified** against
`/home/tom/src-check/Xiaomi_Kernel_OpenSource-32022887f842ec64e317fc22745fedf35e982837`
— the earlier hand-guessed maps were wrong in both drivers:

- The controller is `mediatek,i2c` (`drivers/i2c/busses/i2c-mtk.c`), **not**
  `i2c-mt65xx.c`, and `i2c_common` declares `ver = [02]` → the V2 register
  map: `SOFTRESET 0x50`, `IO_CONFIG 0x34`, `LTIMING 0x2c`, `CLOCK_DIV 0x48`,
  `MCU_INTR 0x40`, `HS 0x30`, `DEBUGSTAT 0xe4`, `FIFO_STAT 0xf4`. The old map
  put `SOFT_RST` at 0x48 (= `CLOCK_DIV`) and `HS` at 0x44 (= `TRANSFER_LEN_AUX`).
- `INTR_STAT`: done is **bit 0** (`TRANSAC_COMP`), errors at bits 1/2/3/5/8.
  The old code polled bit 6 (`DMAERR`), which never sets, so every transfer
  hit the 40 ms timeout → `I2C read failed` on every single poll.
- `CONTROL` bits: `RS = 1<<1`, `CLK_EXT_EN = 1<<3`, `DIR_CHANGE = 1<<4`,
  `ACKERR_DET_EN = 1<<5`. There is **no MASTER bit** (old code used `1<<12`).
- The clock chain was never enabled: `infracfg_ao` `INFRA1_CG_SET` (0x10001088)
  bit 29 = `infra_imp_iic`, `imp_iic_wrap` `CG_SET` (0x11017E08) bit 0 =
  `imp_ap_i2c0`, and topckgen `CLK_CFG_6` (0x100000A0) bits 25:24 = `i2c_sel`
  forced to 0 (= `clk26m`) with update bit 27 written to 0x10000004.
  `INV_BIT` in `MUX_CLR_SET_UPD` is the *gate* index (-1 = no gate), not a
  value inversion, so mux value 0 really is `clk26m` → clk_src = 26 MHz/5.
- GPIO144/145 were never muxed to I2C. Vendor `gpio_init_default` says both
  are mode 1; mode lives at `0x300 + 0x10*(p/8)`, 4 bits at `4*(p%8)`, so
  both are bits 3:0 and 7:4 of `0x420`. Pull-up is the iocfg_lm `PU` register
  (`0x10002240`) bits 2/4, pull-down `0x10002230` bits 2/4.
- Speed setup now ports `mt_i2c_calculate_speed()` + `i2c_set_speed()`
  (duty 45/55, `cnt_constraint`), including `HW_TIMEOUT` and `CLOCK_DIV`.
  I2C registers are accessed **16-bit**, as `readw/writew` do.
- FTS reads are a write + repeated-start read (`fts_read()` uses `msg_num = 2`)
  → `I2C_MASTER_WRRD`: `CONTROL |= DIR_CHANGE|RS`, `TRANSFER_LEN` = wlen,
  `TRANSFER_LEN_AUX` = rlen, `TRANSAC_LEN` = 2, push wlen bytes to
  `DATA_PORT`, then `MCU_INTR = 1`, `START = 1`. Kept ≤ 8 bytes per
  transaction so the controller stays in FIFO mode (`total_len > 8 ||
  msg_aux_len > 8` switches to DMA, which needs a phys addr + cache handling
  we do not have); longer frames are chunked and the pointer auto-increments.
- Keypad: `KP_DEBOUNCE 0x18`, `KP_SCAN_TIMING 0x1C`, `KP_SEL 0x20`,
  `KP_EN 0x24` (the old map wrote the debounce value into `KP_MEM5`), and
  events come from `KP_MEM1..5` (`0x04..0x14`), **active low**, hw keycode =
  `(word << 4) | bit`. `KP_STA` has no keycode field. GPIO80 must be muxed to
  fn 1 (`kpdgpiodefault <0x5001>`) plus `bias-pull-up = <0x66>` → in the
  iocfg_bm bank `PUPD 0x10002840 = 0` (pull-up), `R0 0x10002860 = 0`,
  `R1 0x10002870 = 1`.
- Mapping used (to be **confirmed from the next ring**): hw 0 → VOLUMEDOWN
  (per `kpd-hw-init-map[0] = 0x72`), hw 8 → POWER (per `kpd-hw-pwrkey`),
  hw 17 → VOLUMEUP guess (per `kpd-hw-dl-key0 = 0x11`). Everything else
  is logged raw. **Guess, not observation.** Power may still come from the
  PMIC `mt6358` path, in which case `CONFIG_KPD_PWRKEY_USE_PMIC` means it
  never appears here — the ring will tell us.
- The vendor `gpio_init_default` table (210 7-tuples: pin15 in, pin16
  out/low, pin58 out/low, pin80 mode1, pin144/145 mode1) is **not applied by
  any kernel file we ship**, so we set those mode/dir/dout values ourselves.

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
| `shim-fleur-touchkeys4.img` | touchkeys3 + GPIO offset fix | booted to the home screen; touch dead, keys silent (wrong maps) |
| `shim-fleur-touchkeys5.img` | touchkeys4 + 0084/0085 from the kernel source | candidate: register readbacks + raw hw-key log |

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
- **Re-extract the baseline before *every* generator run**:
  `cd /tmp/opencode && rm -rf base/xnu base/build base/fakeroot && tar -xzf bt/buildtree.tgz -C base`
  (leftover edits in `base/xnu` from a previous run get baked into the diff).
- Always regenerate: `/tmp/opencode/gen-patches.py` applies the driver
  sources + hooks to `/tmp/opencode/base/xnu`, writes the diff with
  `git diff`, then verifies with `git apply --check` (`VERIFY: both patches
  apply cleanly`).
- Check every run for the word `Skipping patch … 0084/0085` before believing
  a kernel contains a driver; also grep the payload for a driver string
  (`SWEET FLEUR TOUCH: starting`) before flashing.
- `gh run watch <id> -R Electron-py/ios-sweet --exit-status` hangs with no
  output — poll with `gh run view <id>` instead, and read the log with
  `gh run view --job=<job-id> --log`.

## CI kernel builds
- Workflow: `.github/workflows/xnu.yml` (macos-14 runner, Xcode 15.x)
- Last run (39994505551) **cancelled** (runner brownout), not a code failure
- Fresh dispatch: `gh workflow run xnu.yml -R Electron-py/ios-sweet --ref fleur`

## Immediate plan
1. ✅ Commit patch **0083** (synthetic input: `sweeptouch`, `sweethidkey`) — tested via `sweethidtest`
2. ✅ Dispatch CI build to verify toolchain green (run 40203488333 **success**)
3. ✅ Write **patch 0084**: MTK I2C controller + FocalTech FTS touch driver (committed)
4. ✅ Write **patch 0085**: MTK keypad + PMIC power key driver (committed)
5. ✅ `touchkeys3` (CI 40324405863) booted to our own `ft_thread` data abort;
   GPIO macro fix → CI **40328758579** green → `shim-fleur-touchkeys4.img`
   **boots to the home screen** (user sees the UI) but touch and keys are dead
6. ✅ Diagnosed both drivers against the MT6781 kernel source (see
   "Why touch/keys produced nothing"), rewrote them, committed `10a6eca`
   (+ `cae9e25`) → CI **40362364957** green (no `Skipping patch`, no errors,
   two benign `-Wunused-function` warnings) → fixed those in `cae9e25` →
   CI **40364755941**
7. 🔜 Build `shim-fleur-touchkeys5.img` from 40364755941:
   `bash /tmp/opencode/build-new-image.sh 40364755941 touchkeys5`, then
   `grep` the payload for `i2c_sel`/`KP_MEM`/`SWEET FLEUR` to prove the
   drivers are inside
8. 🔜 Flash `boot_b` via `/tmp/opencode/flash-watch.sh` (user does
   Vol-Down+Power), pull the ring, and look for
   `mode pre/post 15/16/58/144`, `cg infra1/… i2c_sel 0->0`,
   `timing=… ltiming=… ctl=… div=… io=…`, the FTS chip ID,
   `I2C fail #n stat=…`, and `SWEET FLEUR KEYS: hw … down (mem…)`
9. Restore the 3 lost root hand-fixes (`disabled.plist` bluetoothd/fairplayd
   → True, `ManagedConfiguration.framework` symlink, bluetoothd/fairplayd.H2
   plists) — needs user `sudo`, kills the 83× respawn loop
10. Investigate CPU wallpaper renderer (QuartzCore `CA::OGL::SW`)

## Relevant files
- `/tmp/opencode/ring-ui.bin` / `ring-ui.txt` — last ring (UI boot, 3592 lines, flush #178)
- `/tmp/opencode/console14.txt` — earlier ring decode (172246 B)
- `/tmp/opencode/gen-patches.py` — regenerate 0084/0085 from the CI baseline
- `/tmp/opencode/src/sweet_fleur_touch.c` / `sweet_fleur_keys.c` — driver sources
- `/tmp/opencode/build-new-image.sh <run-id> <name>` — payload → shim image
- `/tmp/opencode/flash-watch.sh` — armed fastboot watcher (flashes `boot_b` only)
- `/tmp/opencode/console7.txt` — 8-core hidtest boot (248888 B, 27 samples, 24 swaps)
- `/home/tom/Documents/musi/fleur.dts` — Android DT (touch@0 = focaltech,fts)
- `/home/tom/src-check/Xiaomi_Kernel_OpenSource-32022887f842ec64e317fc22745fedf35e982837/` — authoritative register maps
- `/home/tom/Documents/musi/ios-sweet/xnu/patches/0083-sweet-synthetic-input.patch` — ready
- `/home/tom/Documents/musi/ios-sweet/tools/` — all tooling
- `/home/tom/Downloads/ipsw/…` — IPSW source (git-ignored)

## Flash / recovery
- `fastboot flash boot_b <img>` (slot b current)
- Restore Android: `fastboot flash boot_b ../Mu-Silicium/stockboot.img`
- `userdata` is the only destructive target; bootloaders/recovery/modem/persist preserved

---

*Updated 2026-10-26 17:00: UI milestone reached (user sees the SpringBoard home screen, ring flush #178 ≈ 30 min, no panic); touch + keys still dead because both drivers used hand-guessed register maps — both rewritten from the Xiaomi MT6781 kernel source (commits `10a6eca`, `cae9e25`, CI 40362364957 / 40364755941 green). Next: build `touchkeys5` from run 40364755941, flash `boot_b`, read the register readbacks from the ring.*