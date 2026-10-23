/*
 * Fleur (MT6781) bring-up shim.
 *   - first image: prove the bootloader jumped to us, find the console and
 *     report the CPU/exception environment (no MMIO except the framebuffer)
 *   - later: MMU, PSCI/SMCCC, GICv3, MTK UFS, XNU payload
 * Any unexpected exception paints a blue panic screen.
 */
#include <stdint.h>
#include "console.h"
#include "mmu.h"
#include "fdt.h"
#include "loader.h"
#include "ufs.h"

#define GICD_BASE		0x0c000000UL	/* fleur gic: interrupt-controller@c000000 */
#define GICR_BASE		0x0c040000UL	/* gic: ...@c040000 */
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x20000UL
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define GICR_ICFGR1		(GICR_SGI + 0xc04)
#define VTIMER_INTID		27	/* fleur DT PPI 11 + 16 (virtual timer) */
#define PTIMER_INTID		30	/* fleur DT PPI 14 + 16 (NS physical timer) */
#define SGI_INTID		1

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_SUSPEND	0xc4000001UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_MIGRATE_INFO_TYPE	0x84000006UL
#define PSCI_SYSTEM_RESET	0x84000009UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_SUSPEND	0xc400000eUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
/* 64-byte aligned so the boot CPU can invalidate only these lines after the
 * secondaries (MMU off, Device writes) have updated them. */
static volatile uint64_t sec_mpidr[NR_CPUS] __attribute__((aligned(64)));
static volatile uint64_t sec_probe[4] __attribute__((aligned(64)));
static volatile uint64_t sec_any __attribute__((aligned(64)));
static volatile uint64_t sec_ctx __attribute__((aligned(64)));
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

/*
 * Invalidate (do not clean) a VA range to PoC. The secondary cores run with
 * the MMU off, so their stores are Device and bypass the boot CPU's D-cache;
 * drop the boot CPU's clean copies before reading what they wrote. The shared
 * lines are 64-byte aligned and never dirty.
 */
static void dc_ivac_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* Clean+invalidate: used by a secondary whose stores may be cacheable. */
static void dc_civac_range(uintptr_t start, uintptr_t end)
{
	uint64_t line = 4UL << ((rsr("ctr_el0") >> 16) & 0xf);

	for (uintptr_t a = start & ~(line - 1); a < end; a += line)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void __attribute__((noreturn)) reset(unsigned after_s)
{
	con_skip();
	con_str("RESET", "PSCI SYSTEM_RESET");
	delay_ms(after_s * 1000);
	smc(PSCI_SYSTEM_RESET, 0, 0, 0);
	con_str("RESET", "PSCI returned; halted");
	for (;;)
		__asm__ volatile("wfe");
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	else if (id == PTIMER_INTID)
		wsr("cntp_tval_el0", rsr("cntfrq_el0") / 100);
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	con_clear(BLUE);
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", rsr("mpidr_el1"));
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_hex("X30", regs[29]);	/* stashed lr is just above the frame */
	reset(40);
}

/* ---- SMP ---- */

static uintptr_t find_gicr(void);

/* Device write to the framebuffer: visible proof that a secondary ran. */
static void sec_mark(void)
{
	volatile uint32_t *fb = (volatile uint32_t *)FB_BASE;

	for (int y = 0; y < 16; y++)
		for (int x = 0; x < 16; x++)
			fb[(FB_H - 24 + y) * (FB_STRIDE / 4) +
			   (FB_W - 24 + x)] = WHITE;
}

void secondary_main(uint64_t ctx)
{
	uint64_t mpidr = rsr("mpidr_el1");
	uint64_t idx = (mpidr >> 8) & 0xff;	/* fleur DT regs are 0x100..0x700 */

	/* Fixed heartbeat before anything that could fault or hang. The context
	 * value from this firmware is unreliable (observed 0), so key on MPIDR. */
	sec_any = 0x5ec0de0000000000UL | idx;
	sec_ctx = ctx;
	if (idx < NR_CPUS)
		sec_mpidr[idx] = mpidr | (1ULL << 63);
	sec_mark();

	/* PSCI should start us with the MMU off (Device stores), but clean to
	 * PoC anyway in case this firmware leaves the secondary cacheable. */
	dc_civac_range((uintptr_t)&sec_any, (uintptr_t)&sec_any + 8);
	dc_civac_range((uintptr_t)&sec_ctx, (uintptr_t)&sec_ctx + 8);
	dc_civac_range((uintptr_t)sec_mpidr, (uintptr_t)(sec_mpidr + NR_CPUS));

	if (idx == 1) {
		/* CPU1 GICR probe disabled: probing the redistributor can hang on
		 * some firmwares. The boot CPU has already probed the GIC. */
		sec_probe[0] = 0xbad;
		sec_probe[1] = 0xbad;
		sec_probe[2] = 0xbad;
		sec_probe[3] = 0xbad;
		dc_civac_range((uintptr_t)sec_probe, (uintptr_t)(sec_probe + 4));
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* Park here: the boot CPU only needs the report. Avoid CPU_OFF while
	 * we are still probing whether the PSCI power-down path is sane. */
}

static void test_smp(void)
{
	stage = "smp";
	int64_t ret1[NR_CPUS] = {0}, ret2[NR_CPUS] = {0};

	for (int i = 0; i < NR_CPUS; i++)
		sec_mpidr[i] = 0;
	for (int i = 0; i < 4; i++)
		sec_probe[i] = 0;
	sec_any = 0;
	sec_ctx = 0;
	dc_ivac_range((uintptr_t)sec_mpidr, (uintptr_t)(sec_mpidr + NR_CPUS));
	dc_ivac_range((uintptr_t)sec_probe, (uintptr_t)(sec_probe + 4));
	dc_ivac_range((uintptr_t)&sec_any, (uintptr_t)&sec_any + 8);
	dc_ivac_range((uintptr_t)&sec_ctx, (uintptr_t)&sec_ctx + 8);

	con_hex("ENTRY", (uint64_t)(uintptr_t)secondary_entry);

	/* Boot CPU MPIDR is 0x81000000, so try the full target first. */
	for (int i = 1; i < NR_CPUS; i++) {
		uint64_t target = 0x81000000UL | ((uint64_t)i << 8);

		con_hex2("SMP", "on=", (uint32_t)target, "cpu=", i);
		ret1[i] = smc(PSCI_CPU_ON, target, (uintptr_t)secondary_entry, i);
		con_hex2("SMP", "ret=", (uint32_t)ret1[i], "cpu=", i);
		if (ret1[i] < 0) {
			ret2[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8,
				      (uintptr_t)secondary_entry, i);
			con_hex2("SMP", "dtb=", (uint32_t)ret2[i], "cpu=", i);
		}
	}
	__asm__ volatile("sev" ::: "memory");
	con_str("SMP", "waiting 400 ms");
	delay_ms(400);
	con_str("SMP", "waited");
	dc_ivac_range((uintptr_t)sec_mpidr, (uintptr_t)(sec_mpidr + NR_CPUS));
	dc_ivac_range((uintptr_t)sec_probe, (uintptr_t)(sec_probe + 4));
	dc_ivac_range((uintptr_t)&sec_any, (uintptr_t)&sec_any + 8);
	dc_ivac_range((uintptr_t)&sec_ctx, (uintptr_t)&sec_ctx + 8);

	con_hex2("SMP", "any=", (uint32_t)sec_any, "ctx=", (uint32_t)sec_ctx);
	for (int i = 0; i < NR_CPUS; i++) {
		char label[] = "CPU0";

		if (i == 0)
			label[0] = 'I';		/* IDX0, not the boot CPU */
		else
			label[3] = '0' + i;
		con_hex2(label, "mpidr=", (uint32_t)sec_mpidr[i],
			 "hi=", (uint32_t)(sec_mpidr[i] >> 32));
		if (i)
			con_hex2(label, "r1=", (uint32_t)ret1[i],
				 "r2=", (uint32_t)ret2[i]);
	}
	con_hex2("SMP", "probe0=", (uint32_t)sec_probe[0],
		 "probe1=", (uint32_t)sec_probe[1]);
	con_hex2("SMP", "probe2=", (uint32_t)sec_probe[2],
		 "probe3=", (uint32_t)sec_probe[3]);
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/*
 * Read-only GICv3 view. No GICD/GICR writes: if LK left the GIC in the secure
 * state we still get its real values instead of silently writing to WI bits.
 */
static void gic_report(void)
{
	stage = "gic sre";
	con_hex2("GIC", "sre=", (uint32_t)rsr("icc_sre_el1"),
		 "pmr=", (uint32_t)rsr("icc_pmr_el1"));
	con_hex2("GIC", "grp1=", (uint32_t)rsr("icc_igrpen1_el1"),
		 "ctlr=", (uint32_t)rsr("icc_ctlr_el1"));

	stage = "gic dist";
	con_hex2("GICD", "ctlr=", rd32(GICD_BASE), "typer=", rd32(GICD_BASE + 0x4));
	con_hex2("GICD", "iidr=", rd32(GICD_BASE + 0x8), "typer2=", rd32(GICD_BASE + 0xc));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	con_hex2("GICR", "base=", (uint32_t)r, "waker=", rd32(r + GICR_WAKER));
	con_hex("GICRT", rd64(r + GICR_TYPER));
	con_hex2("GICR", "grp=", rd32(r + GICR_IGROUPR0),
		 "en=", rd32(r + GICR_ISENABLER0));
	con_hex2("GICR", "pri16=", rd32(r + GICR_IPRIORITYR + 16),
		 "pri24=", rd32(r + GICR_IPRIORITYR + 24));

	int n = 0;
	for (uintptr_t off = 0; off + 0x18 <= GICR_SIZE; off += GICR_PE_SIZE) {
		n++;
		if (rd64(GICR_BASE + off + GICR_TYPER) & (1UL << 4))
			break;
	}
	con_hex2("GICR", "count=", n, "size=", GICR_SIZE);
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");
	uint32_t pend = 0, act = 0, isr = 0, hppi = 0x3ff, ist = 0;

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	} else if (src == PTIMER_INTID) {
		wsr("cntp_tval_el0", f / 100);
		wsr("cntp_ctl_el0", 1);
	}
	isb();
	/* sample state with IRQs still masked, after the first period */
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	ist = src == PTIMER_INTID ? rsr("cntp_ctl_el0") : rsr("cntv_ctl_el0");
	pend = rd32(r + GICR_ISPENDR0);
	act = rd32(r + GICR_ISACTIVER0);
	isr = rsr("isr_el1");
	hppi = rsr("icc_hppir1_el1");

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	wsr("cntp_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "act=", act);
	con_hex2(" pre", "isr=", isr, "hppi=", hppi);
	con_hex2(" pre", "tctl=", ist, "last=", irq_last);
}

static void __attribute__((unused)) test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();
	con_hex2("GICD", "was=", dctlr, "now=", rd32(GICD_BASE));

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	uint32_t grp = rd32(r + GICR_IGROUPR0);
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	uint32_t en_before = rd32(r + GICR_ISENABLER0);
	uint32_t pri_before = rd32(r + GICR_IPRIORITYR + 16);	/* INTIDs 16..19 */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << PTIMER_INTID) | (1U << SGI_INTID));
	con_hex2("ENABLE", "was=", en_before, "now=", rd32(r + GICR_ISENABLER0));
	(void)grp;
	(void)pri_before;

	uint64_t ctlr = rsr("icc_ctlr_el1");
	wsr("icc_ctlr_el1", ctlr & ~2ULL);	/* EOImode 0: EOIR also deactivates */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();
	con_hex2("ICC", "ctlr=", ctlr, "grpen1=", rsr("icc_igrpen1_el1"));

	stage = "gic vtimer";
	irq_window("VTIMER", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

/* ---- main ---- */

static void __attribute__((unused)) psci_line(const char *label, uint64_t fn)
{
	con_hex2(label, "feat=", smc(PSCI_FEATURES, fn, 0, 0), 0, 0);
}

static uint64_t scratch[8192];	/* 64KB, lives in our (later cacheable) image */

static uint64_t __attribute__((unused)) ticks_fill(uint32_t c)
{
	uint64_t t = now();
	con_clear(c);
	return now() - t;
}

static uint64_t __attribute__((unused)) ticks_mem(void)
{
	uint64_t t = now();
	for (int pass = 0; pass < 16; pass++)
		for (int i = 0; i < 8192; i++)
			scratch[i] = scratch[i] * 3 + i;
	return now() - t;
}

static uint32_t __attribute__((unused)) to_us(uint64_t ticks)
{
	uint64_t f = rsr("cntfrq_el0");
	return f ? (uint32_t)(ticks * 1000000 / f) : 0;
}

static uint64_t be64_bytes(const uint8_t *b)
{
	return (uint64_t)b[0] << 56 | (uint64_t)b[1] << 48 |
	       (uint64_t)b[2] << 40 | (uint64_t)b[3] << 32 |
	       (uint64_t)b[4] << 24 | (uint64_t)b[5] << 16 |
	       (uint64_t)b[6] << 8 | b[7];
}

/* Report what LK actually left in the DTB (it patches /memory from DRAM). */
static void dtb_report(uintptr_t dtb)
{
	uint8_t reg[16];
	uint32_t len;

	const char *model = fdt_get_str(dtb, "/", "model");
	con_str("MODEL", model ? model : "(none)");

	int mem_ok = fdt_get(dtb, "/memory", "reg", reg, sizeof reg, &len);
	if (!mem_ok)
		mem_ok = fdt_get(dtb, "/memory@40000000", "reg", reg, sizeof reg, &len);
	if (mem_ok && len >= 16) {
		con_hex("MEMBASE", be64_bytes(reg));
		con_hex("MEMSIZE", be64_bytes(reg + 8));
	} else {
		con_str("MEM", "no /memory reg");
	}

	if (fdt_get(dtb, "/ufshci@11270000", "reg", reg, sizeof reg, &len) && len >= 16) {
		con_hex("UFSBASE", be64_bytes(reg));
		con_hex("UFSSIZE", be64_bytes(reg + 8));
	} else {
		con_str("UFS", "no /ufshci node");
	}

	uint64_t vcc;
	if (fdt_get_u64(dtb, "/ufshci@11270000", "vcc-supply", &vcc))
		con_hex("UFSVCC", vcc);

	if (fdt_get(dtb, "/ufs_mphy@11cc0000", "reg", reg, sizeof reg, &len) && len >= 16)
		con_hex("MPHY", be64_bytes(reg));
}

struct probe {
	char name[5];		/* inline: no absolute pointer relocations */
	uintptr_t va;
};

static void mmu_report(struct mmu_info *mi, uintptr_t dtb)
{
	static const struct probe probes[] = {
		{ "IMG",  0x40080000UL },
		{ "FB",   0x7c010000UL },
		{ "DTB",  0x4bcb0000UL },
		{ "UFS",  0x11270000UL },
		{ "GIC",  0x0c000000UL },
		{ "NONE", 0x200000000UL },
	};

	con_hex("SCTLRa", mi->sctlr_after);
	con_hex("TCRa", rsr("tcr_el1"));
	con_hex("TTBR0a", rsr("ttbr0_el1"));
	(void)dtb;

	for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
		uint64_t par = mmu_probe(probes[i].va);
		if (par & 1)
			con_hex2(probes[i].name, "par=", (uint32_t)par,
				 "fst=", (uint32_t)((par >> 1) & 0x3f));
		else
			con_hex2(probes[i].name, "par=", (uint32_t)par,
				 "pa=", (uint32_t)(par & 0xfffffffff000ULL));
	}
	con_str("MMU", "console alive after translation");
}

void shim_main(uint64_t dtb, uint64_t boot_el)
{
	/*
	 * XNU mode: when LK's ramdisk is a SWEETXNU payload (tools/mkpayload.py,
	 * passed with build-fleur.sh PAYLOAD), act as iBoot and jump to it with
	 * the MMU and caches still off.  A diagnostic image's ramdisk is the
	 * stock Android one, so this probe falls through to the bring-up report.
	 */
	if (loader_payload_present(dtb)) {
		con_clear(BLUE);
		con_str("SHIM", "booting SWEETXNU payload");
		const char *err = loader_boot(dtb);
		con_str("XNU", err);
		con_str("HALT", "payload boot failed");
		for (;;)
			__asm__ volatile("wfe");
	}

	/*
	 * Fleur milestone 1: touch nothing except the framebuffer. The border
	 * makes a wrong stride obvious (the side lines shear), and every value
	 * printed comes from a CPU register, so a hang here is unambiguous.
	 */
	stage = "diag";
	con_clear(RED);

	for (int x = 0; x < FB_W; x++) {
		con_fb[x] = WHITE;
		con_fb[(FB_H - 1) * (FB_STRIDE / 4) + x] = WHITE;
	}
	for (int y = 0; y < FB_H; y++) {
		con_fb[y * (FB_STRIDE / 4)] = WHITE;
		con_fb[y * (FB_STRIDE / 4) + FB_W - 1] = WHITE;
	}

	con_str("SHIM", "fleur bring-up (dtb+mmu)");
	con_skip();
	con_hex2("BOOT", "el=", (uint32_t)boot_el, "now=", (uint32_t)(rsr("currentel") >> 2));
	con_hex2("MIDR", "lo=", (uint32_t)rsr("midr_el1"), "hi=", 0);
	con_hex2("MPIDR", "lo=", (uint32_t)rsr("mpidr_el1"), "hi=", (uint32_t)(rsr("mpidr_el1") >> 32));
	con_hex("DTB", dtb);
	con_hex2("DTB", "magic=", dtb ? (uint32_t)__builtin_bswap32(rd32(dtb)) : 0,
		 "ok=", dtb && __builtin_bswap32(rd32(dtb)) == 0xd00dfeed);
	con_skip();
	con_hex("CNTFRQ", rsr("cntfrq_el0"));
	con_hex("MMFR0", rsr("id_aa64mmfr0_el1"));
	con_hex("PFR0", rsr("id_aa64pfr0_el1"));
	con_hex("SCTLR", rsr("sctlr_el1"));
	con_hex("TCR", rsr("tcr_el1"));
	con_hex("TTBR0", rsr("ttbr0_el1"));
	con_skip();
	con_hex2("FB", "base=", (uint32_t)FB_BASE, "stride=", FB_STRIDE);
	con_hex2("FB", "w=", FB_W, "h=", FB_H);
	con_skip();
	con_hex2("PSCI", "ver=", (uint32_t)smc(PSCI_VERSION, 0, 0, 0),
		 "smccc=", (uint32_t)smc(SMCCC_VERSION, 0, 0, 0));
	con_hex2("PSCI", "reset=", (uint32_t)smc(PSCI_FEATURES, PSCI_SYSTEM_RESET, 0, 0),
		 "on=", (uint32_t)smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0));

	con_skip();
	dtb_report(dtb);

	con_skip();
	stage = "mmu";
	con_str("MMU", "enabling 16K granule...");
	struct mmu_info mi;
	if (mmu_enable(&mi)) {
		con_str("MMU", "FAILED: image spans >2 blocks");
	} else {
		mmu_report(&mi, dtb);
	}

	/* Milestone 3: read-only GICv3 view, then PSCI CPU_ON. */
	con_clear(BLUE);
	con_str("SHIM", "fleur GIC/SMP");
	con_skip();
	gic_report();
	con_skip();
	con_str("SMP", "starting CPU1..7 via CPU_ON");
	test_smp();

	/* Milestone 4: UFS.  test_ufs() clears the screen: the SMP report
	 * above fills all 49 visible rows and the console does not scroll. */
	test_ufs();
	con_str("HALT", "hold power to reboot");

	for (;;)
		__asm__ volatile("wfe");
}
