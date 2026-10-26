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

## Current state (2026-10-26, updated 21:45)

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
- **Clock-gate polarity fix (0084) + new patch 0086** (console mutes when
  SpringBoard execs) — commit `544cfd1`, CI run **40378590062** green
  (7m26s, zero warnings/errors in `sweet_fleur_touch.c`, `sweet_fleur_keys.c`,
  `kern_exec.c`; the only "skipped patch" lines are the pre-existing
  `dxb/patches/*` set)
- **Patch 0085 KP_SEL/KP_EN fix** — commit `38f5340`, CI run **40381465166**
  **green** (patches 0080–0086 all applied, `shim-fleur-touchkeys6.img`)
- **Ring tk6 answers three questions** (`/tmp/opencode/ring-tk6.txt`, 4666
  lines): I2C+FTS **do** work (`chip ID 0xb1`, zero `I2C fail`), patch 0086
  **does** mute the console (`SWEET console: muted on SpringBoard exec`), and
  the keypad now leaves `KP_SEL/KP_EN` untouched (`before` == `after`) with
  27 presses → 27 × `key 2 down` (slot 2 = VolumeDecrement, correct)
- **Touch confirmed working on screen by the user** (`shim-fleur-touchkeys7.img`
  from CI **40394561768**, flashed to `boot_b` by the watcher) — the
  point-count byte fix in `ce4747b` was the whole bug
- **Next build disables the synthetic Home press**: `sweethidtest=8` removed
  from `BASE` in `build-new-image.sh` (kernel unchanged, boot-args only) →
  `shim-fleur-nohome.img`, `sweettouch=1 sweetkeys=1` kept so volume/power can
  be tested live while the button path is fixed
- **CI build green** (run 40203488333, macos-14, Xcode 15.4)

### ⚠️ Missing (blockers for "usable")
| Feature | Hardware | What's needed |
|---------|----------|---------------|
| **Buttons** | MT6781 keypad matrix (GPIO80), PMIC power | volume-down **delivered** (tk6: 27 → 27, correct slot). `fk_slot()` already maps every matrix key the DT defines — hw 0 = VOLDN (`kpd-hw-init-map[0]=0x72`), hw 8 = POWER (`kpd-hw-pwrkey=0x08`), hw 17 = VOLUP (`kpd-hw-rstkey=0x11` + `kpd-sw-rstkey=0x73`) — so **volume-up only needs a press test**; power may be the MT6358 instead (`MT6358_PWRKEY_PRESS_STS` 0xa40 via pwrap) if hw 8 never appears. **Active — current fix target** (synthetic Home removed so buttons are judged honestly) |
| **Background/wallpaper** | CPU renderer path (no GPU) | QuartzCore CPU renderer + wallpaper service |
| **bluetoothd/fairplayd respawn** (83×) | root image | 3 hand-fixes lost in the v3 25 GiB rebuild — see below |

✅ **Touch is fixed and confirmed by the user** (`shim-fleur-touchkeys7.img`,
commit `ce4747b`) — FTS point count, `SweetHIDTouch` delivery and backboardd
all verified end-to-end.

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

### Ring from `shim-fleur-touchkeys5.img` (boot with 0084+0085, no 0086 yet)

This is the evidence every fix below is built on — all **observed**, not
guessed (ring pulled with `mtkclient ro 0x1dc9bf7000 0x401000`, decoded with
`tools/sweet-log.py`):

- **Keys init was already correct**: `mode80=0x1 pupd=0x6 r0=0xe r1=0x1`,
  `kp_sta=0x0000 mem=0xffff 0xffff 0xffff 0xffff 0x00ff deb=0x0400 en=0x0001`,
  `i2c_sel 3->0`. The keymap state matches the vendor reset value exactly.
- **Touch produced nothing because every I2C register read 0**: `timing=0x0000
  ltiming=0x0000 ctl=0x0000 div=0x0000 io=0x0000 intr=0x0000 dbg=0x0000`,
  `stat=0x0000`, `no answer from FTS yet`, 0 finger events.
- **Every button press lit a whole column**: 30 presses, each producing
  `hw 0, 9, 18, 27, 36, 45, 54, 63` (`mem1=0xfdfe mem2=0xf7fb mem3=0xdfef
  mem4=0x7fbf`) and nothing else — and **column 8 never read once**.

### Why the I2C registers were all zero (fixed in 0084)

`drivers/clk/mediatek/clk-gate.c` defines **bit cleared = clock ON**:
`mtk_cg_is_enabled()` returns `mtk_cg_bit_is_cleared()`, the enable path
writes `clr_ofs`, the disable path writes `set_ofs`. We were writing to
`INFRA1_CG_SET` (0x10001088) and `imp_iic_wrap CG_SET` (0x11017E08), i.e.
switching `infra_imp_iic` (INFRA1 bit 29) and `imp_ap_i2c0` (bit 0) **off**.
0084 now writes `INFRA1_CG_CLR 0x8C` / `IMP_CG_CLR 0xE04` and prints the
status registers before and after (`INFRA1_CG_STA 0x94`, `IMP_CG_STA 0xE00`)
so the next ring shows bit 29 / bit 0 cleared.

### Why only one volume button could ever work (fixed in 0085)

The 8 lit positions are `hw = row*9 + col` with **col = 0 and row 0..7** —
the layout is 8 rows × 9 columns, confirmed by Android's own
`for (i = 17; i < KPD_NUM_KEYS; i += 9) kpd_keymap[i] = 0;` which zeroes
column 8 of every row on top of `kpd_keymap[8] = 0` (the PMIC power key).
The three buttons are:

| button | hw | row/col | how Android maps it |
|--------|----|---------|---------------------|
| volume down | 0 | r0/c0 | `kpd-hw-init-map[0] = 0x72` → `KEY_VOLUMEDOWN` |
| power | 8 | r0/c8 | `kpd-hw-pwrkey`, but `CONFIG_KPD_PWRKEY_USE_PMIC` zeroes it |
| volume up | 17 | r1/c8 | `kpd-hw-rstkey = 0x11` + `kpd-sw-rstkey = 0x73` = `KEY_VOLUMEUP` (also `kpd-hw-dl-key0`) |

So vol-up is **hw 17**, in column 8 — and column 8 was never read in 30
presses, while a vol-down press lit all 8 rows of column 0. A scan that
resolves neither rows nor the other columns makes both buttons look like
hw 0 → both go down. Root cause of the degenerate scan: **we wrote
`KP_SEL = 0` and `KP_EN = 1`, which Android never does.** Audited every
hardware write in `mt6781/hal_kpd.c` + `kpd.c`:

- `probe()` → `kpd_set_debounce(0x400)` → `request_irq()` — that is all
- `kpd_open()` → `return 0` (no-op)
- `enable_kpd()` → only from `kpd_wakeup_src_setting()` in suspend/resume
  (`call_status` is a sysfs-only variable, never set at boot)
- `kpd_double_key_enable()` → only if double-key DT props exist; fleur has none
- `KP_SCAN_TIMING` / `KP_COL0..2_SEL` → **never written anywhere** (dead defines)

`KP_EN = 1` is the prime suspect for "only column 0 readable" (it looks like
a column-enable mask). 0085 now dumps `sta/sel/scan/en/deb/mem` before *and*
after, writes only `KP_DEBOUNCE`, touches `KP_EN` only if it reads 0, logs
`r%u/c%u` per hw code plus a one-line `down[N]:` summary whenever the held set
changes, and polls at 5 ms instead of 20 ms.

### Console mute at SpringBoard start (new patch 0086)

`xnu/patches/0086-sweet-console-sb.patch` sets `sweet_console_muted = 1`
inside `set_proc_name()` in `bsd/kern/kern_exec.c` when `p->p_name` compares
equal to `SpringBoard` (`strncmp(..., sizeof("SpringBoard"))` includes the NUL,
so it is an exact match). Boot diagnostics therefore stay on screen until
SpringBoard execs, then the GUI owns the screen. **Nothing is lost**: patch
0067 calls `sweet_log_append()` *before* the mute test, so the whole boot is
still in the persistent ring, and `panic_active()` still draws a panic.

`set_proc_name()` is the single writer of `p->p_name` (only the `bcopy` at
`bsd/kern/kern_exec.c:913`), and the ring proves it stores the basename
(`exec SpringBoard`, `exec xpcproxy`, …), so the hook fires for every exec.


### Ring from `shim-fleur-touchkeys6.img` (0084 + 0085 + 0086) and the touch root cause

Evidence: `/tmp/opencode/ring-tk6.bin` → `ring-tk6.txt` (4666 lines).

**Observed in the ring:**
- `cg infra1 0x00000800->0x00000800 imp 0x00000000->0x00000000 i2c_sel 3->0`
  — clocks ungated (bit clear = clock on) and `i2c_sel` forced to clk26m
- `timing=0x0007 ltiming=0x0008 ctl=0x0028 div=0x0404 io=0x0003` — controller
  programmed, open-drain
- `chip ID 0xb1` — the panel answers on I2C
- **0** `I2C fail` lines in the entire ring → every poll succeeded
- exactly **two** touch lines: `finger 0 down at 433,433` + `finger 0 up`,
  both *before* SpringBoard exec (line 512 = the mute) — one phantom event at
  driver start, then nothing for the rest of the boot
- `SWEET console: muted on SpringBoard exec` — **0086 works**
- keypad: `sel=0x1c70 scan=0x0000 en=0x0001 deb=0x0400` byte-identical before
  and after the run (0085 writes only `KP_DEBOUNCE`), and 27 presses → 27 ×
  `key 2 down` (slot 2 = `kHIDUsage_Csmr_VolumeDecrement`, correct)
- **HID side (patch 0058) is wired up and consumed**: `SWEET HID: touch
  registered` and `SWEET HID: buttons registered`, `sweethidtest` Menu pairs go
  out as `key 0x40 down (0 clients)` ×4 during boot and `(1 clients)` ×26 once
  backboardd attaches, and the 27 volume presses are `key 0xea down (1
  clients)` — so consumer usage 0xEA (VolumeDecrement) reaches a live client.
  The one phantom touch also produced `SWEET HID: touch 0 down at 433,433`,
  i.e. `sweet_hid_touch()` → `SweetHIDTouch::touch()` → `dispatchEvent()` works
  end-to-end; only the point count ever reached 0.
- the phantom column dump `hw 0,9,18,27,36,45,54,63` is cosmetic: fleur's
  `kpd-hw-init-map` is all zeros except `[0] = 0x72` (KEY_VOLUMEDOWN), the
  vendor's own loop zeroes the rest, and `fk_slot()` only maps 0/8/17

**Cause:** the poll loop calls `ft_read_points()` at least every 5th poll
(50 ms) regardless of the IRQ line, and no read ever failed — so the driver was
reading the panel continuously and being told "0 points" every time. The count
byte was simply in the wrong place.

Vendor index vs our index (`fts_read_touchdata()` in
`ft3418_i2c/focaltech_core.c`: `buf[0] = 0x01`, then 62 data bytes, so vendor
index *k* = data byte *k − 1*):

| field | vendor `buf[]` | our index (= data byte) | we read |
|---|---|---|---|
| point count (`FTS_TOUCH_POINT_NUM` = 2) | 2 | **1** | index 0 ✗ |
| point i `event<<6 \| x_h` (`FTS_TOUCH_EVENT_POS` = 3) | 3 + 6i | **2 + 6i** | ✓ (accident) |
| `x_l` | 4 + 6i | 3 + 6i | ✓ |
| `id<<4 \| y_h` (`FTS_TOUCH_ID_POS` = 5) | 5 + 6i | 4 + 6i | ✓ |
| `y_l` | 6 + 6i | 5 + 6i | ✓ |

Our x/y bytes land on the vendor's x/y bytes by accident (same 6-byte stride,
x read at record-relative +1/+2), so **only the count byte was wrong**:
`data[0]` is held at 0 after boot, `data[1]` is the count. That is exactly the
symptom — one phantom event at start when `data[0]` was briefly non-zero, then
zero touches forever with no I2C errors.

**Fix — commit `ce4747b` (0084 regenerated by `gen-patches.py` from the
buildtree baseline), CI run `40394561768`:**
- `np = buf[1] & 0x0F`, record at `base = 2 + 6*i`
- `event = buf[base] >> 6` (0 down / 1 up / 2 contact), `id = buf[base+2] >> 4`,
  drop the point when `id >= FTS_MAX_ID (0x0A)`
- read `2 + 10*6 = 62` bytes (was 61: the last point's area byte was outside
  the buffer)
- reset 1 ms low / **200 ms** high (`fts_reset_proc(200)`); our old 5/50 ms
  sampled the chip before its FW came up
- chip id: read `0xA3` **and** `0x9F`, retry 10 × 100 ms, compare against
  `0x5452` (`FTS_CHIP_TYPE_MAPPING` for `FTS_CHIP_TYPE = _FT3418`); on mismatch
  send `eb aa 09`, exactly what `fts_get_ic_information()` → `fts_hid2std()`
  does
- raw dump of the first frames so the next ring shows the layout directly

**Open question:** `0xb1` (read 0xA3 after only 50 ms) does not match the
vendor's `0x5452`. Either the reset window was too short (now fixed) or the
panel is in HID mode and needs `eb aa 09` (now also sent). The next ring's
`chip ID 0x..0x.. (expect 0x5452)` line settles it.

**Still unproven:** `sweethidtest=8` exercises the **key** path only —
`SweetHIDTouch` (0058) has never been driven end-to-end. If touch still does
nothing with a correct frame, the next suspect is service publication /
normalisation in 0058, not the panel.

**Correction on power/volume-up (DT, `fleur.dts` kpd node):** the matrix *does*
carry all three keys — `kpd-hw-init-map[0] = 0x72` (KEY_VOLUMEDOWN) → hw 0,
`kpd-hw-pwrkey = 0x08` → hw 8, `kpd-hw-rstkey = 0x11` with
`kpd-sw-rstkey = 0x73` (KEY_VOLUMEUP) → hw 17 — and `fk_slot()` already maps
0/8/17 to VOLDN/POWER/VOLUP, so volume-up should work as soon as it is
pressed. Only `kpd-pwrkey-eint-gpio = 0` (no GPIO for the power key) leaves open
that **power** is sourced from the MT6358 instead: `mediatek,mt-pmic`
interrupt-names include `pwrkey`/`pwrkey_r`, and `MT6358_PWRKEY_PRESS_STS =
PMIC_BASE + 0xa40` is the press/release status register
(`PMIC_PUP_PKEY_RELEASE_ADDR`), readable through `pwrap_read16()` at
`0x1000d000` (`mtk-pmic-wrap.c:3051`). The generic handler
(`mtk_pmic_keys.c:166`) does `regmap_read(deb_reg); pressed = !(val & mask)`.
If hw 8 never shows up in a ring, that read-only PWRAP driver is the fix.


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
| `shim-fleur-touchkeys5.img` | touchkeys4 + 0084/0085 from the kernel source | register readbacks + raw hw-key log |
| `shim-fleur-touchkeys6.img` | touchkeys5 + 0086 (console mute) + KP_SEL/KP_EN fix | ring pulled 2026-10-26 19:xx — mute ✓, keys 27/27 ✓, touch count byte ✗ |
| `shim-fleur-touchkeys7.img` | touchkeys6 + 0084 point-count/frame fix (`ce4747b`) | **touch confirmed working by the user** |
| `shim-fleur-nohome.img` | touchkeys7 + `sweethidtest=8` **removed** from `BASE` | active: buttons testable without a synthetic Home press every 8 s |

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

## Patch workflow (0084/0085/0086 and anything new)

- Hand-written diff hunks are unreliable — CI silently *skips* a malformed
  patch and the run still goes green. Only a compile error fails a run.
- **Re-extract the baseline before *every* generator run**:
  `cd /tmp/opencode && rm -rf base/xnu base/build base/fakeroot && tar -xzf bt/buildtree.tgz -C base`
  (leftover edits in `base/xnu` from a previous run get baked into the diff).
- Always regenerate: `/tmp/opencode/gen-patches.py` applies the driver
  sources + hooks to `/tmp/opencode/base/xnu`, writes the diff with
  `git diff`, then verifies with `git apply --check` (`VERIFY: all three
  patches apply cleanly`). It rebuilds **0084, 0085 and 0086** and nothing
  else; edit `/tmp/opencode/src/sweet_fleur_*.c` for 0084/0085, edit the
  generator's `# ---- 0086 ----` block for 0086.
- The baseline is the tree *after* patch 0083, so 0084/0085/0086 apply on
  top of the rest of the stack in CI order; the generator's verify step
  replays them in that order from a pristine `git archive HEAD~3`.
- Check every run for the word `Skipping patch … 008[456]` before believing
  a kernel contains a driver; also grep the payload for a driver string
  (`SWEET FLEUR TOUCH: starting`) before flashing. The four `Skipping patch`
  lines that are always present are the upstream `dxb/patches/*` set — not
  ours — and so are the ~11 `error:` lines they emit.
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
7. ✅ Built `shim-fleur-touchkeys5.img` from 40364755941, flashed `boot_b`,
   booted: keys init OK but **all I2C registers read 0** (we wrote `*_CG_SET`,
   i.e. gated the clocks) and **every press lit a whole column** (see "Ring
   from `shim-fleur-touchkeys5.img`")
8. ✅ Regenerated 0084 with the clock-gate polarity fix, added **0086**
   (console mutes at SpringBoard exec) — `544cfd1`, CI **40378590062** green;
   regenerated 0085 without the `KP_SEL`/`KP_EN` writes — `38f5340`,
   CI **40381465166** dispatched
9. ✅ Built `shim-fleur-touchkeys6.img` from **40381465166**, flashed `boot_b`,
   pulled the ring → `/tmp/opencode/ring-tk6.txt`. Result: console mute ✓,
   keypad registers untouched ✓, 27/27 volume-down ✓, **touch still dead** —
   the panel reported 0 points on every one of hundreds of successful reads
   (see "Ring from `shim-fleur-touchkeys6.img`")
10. ✅ Found the cause in `ft3418_i2c/focaltech_core.c`: the point count is
    **data byte 1**, not 0 (vendor `buf[0]` holds the register address). Fixed
    0084 + adopted the vendor frame/reset/id sequence → **`ce4747b`**,
    CI **40394561768 green**, built `shim-fleur-touchkeys7.img`, armed
    `flash-watch.sh` for `boot_b`
11. ✅ `shim-fleur-touchkeys7.img` flashed by the watcher and the **user
    confirmed touch works on screen** — driver frame, `SweetHIDTouch`
    delivery and backboardd all verified end-to-end. (The tk7 ring was not
    pulled: it needs a power-off. Optional later — it would show
    `frame cnt=N raw …`, `chip ID 0x..0x.. (expect 0x5452)` and
    `finger N id=… ev=… down at x,y`.)
12. ✅ `shim-fleur-nohome.img` built from **40394561768** with `sweethidtest=8`
    removed from `BASE` (kernel unchanged, boot-args only); flash watcher
    armed. Purpose: judge the buttons without a synthetic Home press every 8 s.
13. 🔜 **Fix buttons** (current target): check volume-up (hw 17) and power
    (hw 8) on screen with the nohome image, add the HakiOS **two-look** filter
    to `sweet_fleur_keys.c`, and only if hw 8 never appears add the read-only
    PWRAP read of `MT6358_PWRKEY_PRESS_STS` (0xa40) via `pwrap_read16()`
14. Restore the 3 lost root hand-fixes (`disabled.plist` bluetoothd/fairplayd
    → True, `ManagedConfiguration.framework` symlink, bluetoothd/fairplayd.H2
    plists) — needs user `sudo`, kills the 83× respawn loop
15. Investigate CPU wallpaper renderer (QuartzCore `CA::OGL::SW`)

## Relevant files
- `/tmp/opencode/ring-tk6.bin` / `ring-tk6.txt` — last ring (`touchkeys6` boot,
  4666 lines) — the evidence for the point-count-byte fix (`ce4747b`)
- `/tmp/opencode/ring-tk5.bin` / `ring-tk5.txt` — earlier ring (`touchkeys5` boot,
  3974 lines, flush #26, 546 `SWEET FLEUR` lines) — the evidence for 0084/0085
- `/tmp/opencode/ring-ui.bin` / `ring-ui.txt` — earlier ring (UI boot, 3592 lines, flush #178)
- `/tmp/opencode/console14.txt` — earlier ring decode (172246 B)
- `/tmp/opencode/gen-patches.py` — regenerate 0084/0085/**0086** from the CI baseline
- `/tmp/opencode/src/sweet_fleur_touch.c` / `sweet_fleur_keys.c` — driver sources
- `/tmp/opencode/build-new-image.sh <run-id> <name>` — payload → shim image
- `/tmp/opencode/flash-watch.sh` — armed fastboot watcher (flashes `boot_b` only)
- `/tmp/opencode/console7.txt` — 8-core hidtest boot (248888 B, 27 samples, 24 swaps)
- `/home/tom/Documents/musi/fleur.dts` — Android DT (touch@0 = focaltech,fts; kpd node ~line 3168)
- `/home/tom/src-check/Xiaomi_Kernel_OpenSource-32022887f842ec64e317fc22745fedf35e982837/` — authoritative register maps
- `/home/tom/Documents/musi/ios-sweet/xnu/patches/0083-sweet-synthetic-input.patch` — ready
- `/home/tom/Documents/musi/ios-sweet/xnu/patches/0086-sweet-console-sb.patch` — console mute at SpringBoard exec
- `/home/tom/Documents/musi/ios-sweet/tools/` — all tooling
- `/home/tom/Downloads/ipsw/…` — IPSW source (git-ignored)

## Flash / recovery
- `fastboot flash boot_b <img>` (slot b current)
- Restore Android: `fastboot flash boot_b ../Mu-Silicium/stockboot.img`
- `userdata` is the only destructive target; bootloaders/recovery/modem/persist preserved

---

*Updated 2026-10-26 19:30: two root causes found from the `touchkeys5` ring. (1) Touch: we wrote `*_CG_SET` where `clk-gate.c` treats **bit cleared = clock ON**, so `infra_imp_iic` / `imp_ap_i2c0` were switched *off* and every I2C register read 0 — fixed in 0084 (CI 40378590062 green). (2) Keys: every press lit all 8 rows of column 0 and column 8 (hw 8 power, hw 17 volume-up) never read once, because we wrote `KP_SEL = 0` and `KP_EN = 1`, neither of which Android ever writes at boot — fixed in 0085 (commit `38f5340`, CI 40381465166 in flight). Also added **0086**, which mutes the on-screen console when SpringBoard execs so the next test shows the UI without kernel text while the ring keeps everything. Next: build `touchkeys6` from 40381465166, flash `boot_b`, read the `before/after` register dumps.*