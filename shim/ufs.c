/*
 * Milestone 5 probe: can we talk to the internal UFS that LK booted from?
 *
 * Runs with the 16K EL1 MMU on and the image mapped cacheable, so the DMA
 * descriptors and buffers are cleaned before the doorbell and invalidated
 * after completion.  Makes sure the MT6781 UFS clock gates are enabled (note:
 * these INFRA gates are inverted - writing SET disables a clock, CLR enables
 * it, and the status bit reads 1 while gated), dumps what LK left in the
 * controller/MPHY, then - only if the host is enabled and the link is up -
 * takes over the transfer request list and issues READ-ONLY SCSI commands.
 * Nothing is ever written to the device: there is no SCSI WRITE in this file.
 *
 * Register layout and descriptor formats: JEDEC UFSHCI (JESD223).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x11270000UL	/* ufshci@11270000, fleur */
#define MPHY_BASE	0x11cc0000UL	/* phy@11cc0000, fleur */

#define REG_CAP		0x00
#define REG_VER		0x08
#define REG_IS		0x20
#define REG_HCS		0x30
#define REG_HCE		0x34
#define REG_UTRLBA	0x50
#define REG_UTRLBAU	0x54
#define REG_UTRLDBR	0x58
#define REG_UTRLCLR	0x5c
#define REG_UTRLRSR	0x60
#define REG_REFCLK	0x144		/* MTK vendor: xoufs req/ack */

/*
 * MT6781 infracfg_ao gate registers (drivers/clk/mediatek/clk-mt6781.c:
 * infra2_cg_regs, infra3_cg_regs; IDs from include/dt-bindings/clock/
 * mt6781-clk.h).  The UFS node's clocks are all gates in INFRA2.
 */
#define INFRA_AO	0x10001000UL
#define INFRA2_SET	(INFRA_AO + 0xa4)
#define INFRA2_CLR	(INFRA_AO + 0xa8)
#define INFRA2_STA	(INFRA_AO + 0xac)
#define INFRA3_SET	(INFRA_AO + 0xc0)
#define INFRA3_CLR	(INFRA_AO + 0xc4)
#define INFRA3_STA	(INFRA_AO + 0xc8)

/*
 * Polarity, from clk-gate.c: INFRA gates use mtk_clk_gate_ops_setclr, whose
 * is_enabled() is "status bit CLEARED".  Writing a 1 to SET *disables* the
 * gate, writing a 1 to CLR enables it.  A status bit of 1 means "clock off".
 */

#define G_UNIPRO_SCK	(1u << 11)	/* 0x3e, "ufs0-unipro-clk" */
#define G_UNIPRO_TICK	(1u << 12)	/* CLK_INFRA_UNIPRO_TICK, not in DT */
#define G_MP_SAP_BCLK	(1u << 13)	/* 0x40, "ufs0-mp-clk" */
#define G_AES_UFS	(1u << 29)	/* 0x75, "ufs0-aes-clk" */
#define G_UFS		(1u << 30)	/* 0x4f, "ufs0-clock" */
#define G_UFS_TICK	(1u << 31)	/* CLK_INFRA_UFS_TICK, not in DT */
#define G_UFS_AXI	(1u << 5)	/* INFRA3, DMA path */

/* infracfg_ao reset assert registers (ti,syscon-reset: BIT(index) set = held). */
#define RST_HCI		0x10001130UL	/* bit 15 */
#define RST_UNIPRO	0x10001140UL	/* bit 7 */
#define RST_CRYPTO	0x10001150UL	/* bit 21 */

/* MTK MPHY init bits (drivers/phy/mediatek/phy-mtk-ufs.c). */
#define MP_GLB_DIG_8C	0x008c
#define PLL_ISO_EN	(1u << 9)
#define FRC_PLL_ISO_EN	(1u << 8)
#define FRC_PWR_ON	(1u << 10)
#define PLL_PWR_ON	(1u << 11)
#define MP_LN_DIG_RX_9C	0xa09c
#define FSM_DIFZ_FRC	(1u << 18)
#define MP_LN_DIG_RX_AC	0xa0ac
#define FRC_RX_SQ_EN	(1u << 0)
#define RX_SQ_EN	(1u << 1)
#define MP_LN_RX_44	0xb044
#define FRC_CDR_PWR_ON	(1u << 17)
#define CDR_PWR_ON	(1u << 18)
#define FRC_CDR_ISO_EN	(1u << 19)
#define CDR_ISO_EN	(1u << 20)

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }
static inline uint32_t rd32a(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline void wr32a(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }

static void __attribute__((unused)) wait_ms(unsigned ms)
{
	uint64_t frq, end;

	__asm__ volatile("isb; mrs %0, cntfrq_el0" : "=r"(frq));
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(end));
	end += frq / 1000 * ms;
	for (uint64_t t;;) {
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
		if (t >= end)
			break;
	}
}

/* DMA to our cacheable image needs clean-before / invalidate-after. */
static uint32_t dc_line(void)
{
	uint64_t ctr;

	__asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
	return 4u << ((ctr >> 16) & 0xf);
}

static void dc_civac(void *p, unsigned n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)(dc_line() - 1);

	for (; a < (uintptr_t)p + n; a += dc_line())
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static void dc_ivac(void *p, unsigned n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)(dc_line() - 1);

	for (; a < (uintptr_t)p + n; a += dc_line())
		__asm__ volatile("dc ivac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * LK reads the boot image from this UFS, so it normally leaves the controller
 * clocked.  The INFRA gate status bits were observed clear (= clock on) at
 * handoff.  Make sure they are on anyway: idempotent, and a read of a gated
 * block wedges the interconnect.
 */
static void ufs_clocks(void)
{
	uint32_t b2 = rd32a(INFRA2_STA), b3 = rd32a(INFRA3_STA);

	con_hex2("CLK", "infra2=", b2, "infra3=", b3);
	/* Enable = write 1 to CLR; the status bit then reads 0. */
	wr32a(INFRA2_CLR, G_UFS | G_UFS_TICK | G_UNIPRO_SCK | G_UNIPRO_TICK |
	    G_MP_SAP_BCLK | G_AES_UFS);
	wr32a(INFRA3_CLR, G_UFS_AXI);
	__asm__ volatile("dsb sy" ::: "memory");
	b2 = rd32a(INFRA2_STA);
	b3 = rd32a(INFRA3_STA);
	con_hex2("CLK", "infra2a=", b2, "infra3a=", b3);
	if (b2 & (G_UFS | G_UNIPRO_SCK | G_MP_SAP_BCLK))
		con_hex2("CLK", "still gated2=", b2 & (G_UFS | G_UNIPRO_SCK | G_MP_SAP_BCLK),
		    "3=", b3 & G_UFS_AXI);
}

/* Are LK's UFS resets left asserted?  1 = held in reset. */
static void __attribute__((unused)) ufs_reset_state(void)
{
	con_hex2("RST", "hci=", (rd32a(RST_HCI) >> 15) & 1,
	    "unipro=", (rd32a(RST_UNIPRO) >> 7) & 1);
	con_hex2("RST", "crypto=", (rd32a(RST_CRYPTO) >> 21) & 1,
	    "130=", rd32a(RST_HCI) & 0x8000);
}

/* Read-only view of the MPHY: tells us whether LK left it powered. */
static void __attribute__((unused)) mphy_report(void)
{
	uint32_t a = rd32a(MPHY_BASE + MP_GLB_DIG_8C);
	uint32_t b = rd32a(MPHY_BASE + MP_LN_DIG_RX_9C);
	uint32_t c = rd32a(MPHY_BASE + MP_LN_DIG_RX_AC);
	uint32_t d = rd32a(MPHY_BASE + MP_LN_RX_44);

	con_hex2("PHY", "8c=", a, "9c=", b);
	con_hex2("PHY", "ac=", c, "44=", d);
}

/* Linux ufs_mtk_phy_set_active(), same bits and order. */
static void __attribute__((unused)) mphy_set_active(void)
{
	wr32a(MPHY_BASE + MP_GLB_DIG_8C, rd32a(MPHY_BASE + MP_GLB_DIG_8C) | PLL_PWR_ON);
	wr32a(MPHY_BASE + MP_GLB_DIG_8C, rd32a(MPHY_BASE + MP_GLB_DIG_8C) & ~FRC_PWR_ON);
	wr32a(MPHY_BASE + MP_GLB_DIG_8C, rd32a(MPHY_BASE + MP_GLB_DIG_8C) & ~PLL_ISO_EN);
	wr32a(MPHY_BASE + MP_GLB_DIG_8C, rd32a(MPHY_BASE + MP_GLB_DIG_8C) & ~FRC_PLL_ISO_EN);

	wr32a(MPHY_BASE + MP_LN_RX_44, rd32a(MPHY_BASE + MP_LN_RX_44) | CDR_PWR_ON);
	wr32a(MPHY_BASE + MP_LN_RX_44, rd32a(MPHY_BASE + MP_LN_RX_44) & ~FRC_CDR_PWR_ON);
	wr32a(MPHY_BASE + MP_LN_RX_44, rd32a(MPHY_BASE + MP_LN_RX_44) & ~CDR_ISO_EN);
	wr32a(MPHY_BASE + MP_LN_RX_44, rd32a(MPHY_BASE + MP_LN_RX_44) & ~FRC_CDR_ISO_EN);

	wr32a(MPHY_BASE + MP_LN_DIG_RX_AC, rd32a(MPHY_BASE + MP_LN_DIG_RX_AC) | RX_SQ_EN);
	wr32a(MPHY_BASE + MP_LN_DIG_RX_AC, rd32a(MPHY_BASE + MP_LN_DIG_RX_AC) & ~FRC_RX_SQ_EN);
	for (volatile int i = 0; i < 1000; i++)
		;			/* >1 us: wait DIFZ stable */
	wr32a(MPHY_BASE + MP_LN_DIG_RX_9C, rd32a(MPHY_BASE + MP_LN_DIG_RX_9C) & ~FSM_DIFZ_FRC);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;		/* [31:28] CT, [26:25] DD, [24] I */
	uint32_t dw1;
	uint32_t dw2;		/* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;		/* command descriptor base, 128-byte aligned */
	uint32_t ucdbau;
	uint32_t resp;		/* [31:16] response UPIU offset, [15:0] length (DWORDs) */
	uint32_t prdt;		/* [31:16] PRDT offset, [15:0] entries (DWORDs/entries) */
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;	/* dbc = byte count - 1 */
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));
static uint8_t data[2 * BLOCK] __attribute__((aligned(4096)));

static uint64_t ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static void zero(void *p, unsigned n)
{
	volatile uint8_t *b = p;
	for (unsigned i = 0; i < n; i++)
		b[i] = 0;
}

static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/*
 * Issue one SCSI command (read direction or no data) in slot 0.
 * Returns 0 on success, otherwise a code describing the failure stage.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	/* Command UPIU: header (12 bytes), expected length, CDB. */
	ucd.cmd[0] = 0x01;			/* transaction type: COMMAND */
	ucd.cmd[1] = len ? 0x40 : 0x00;		/* flags: R (data in) */
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);	/* UFS storage, dev->host */
	utrl[0].dw2 = 0x0f;			/* OCS: invalid until the host updates it */
	utrl[0].ucdba = (uint32_t)ucd_pa;
	utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	utrl[0].resp = (512u / 4) << 16 | (512u / 4);
	if (len) {
		uintptr_t buf_pa = (uintptr_t)buf;
		ucd.prdt[0].addr_lo = (uint32_t)buf_pa;
		ucd.prdt[0].addr_hi = (uint32_t)(buf_pa >> 32);
		ucd.prdt[0].dbc = len - 1;
		utrl[0].prdt = (1024u / 4) << 16 | 1;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* The image is mapped cacheable: push descriptors/data to the PoC. */
	dc_civac(&ucd, sizeof ucd);
	dc_civac(&utrl[0], sizeof utrl[0]);
	if (len)
		dc_civac(buf, len);

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 19200000) {	/* 1 s */
			ufs_wr(REG_UTRLCLR, ~1u);	/* clear slot 0 (write 0 to the bit) */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	/* Drop stale lines so the controller's writes are visible. */
	dc_ivac(&utrl[0], sizeof utrl[0]);
	dc_ivac(&ucd, sizeof ucd);
	if (len)
		dc_ivac(buf, len);
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];		/* SCSI status in the response UPIU */
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)		/* RESPONSE UPIU */
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(unsigned lun, uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(lun, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static int same(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

/* Name the filesystem whose superblock starts at block lba (f2fs/ext4). */
static const char *fs_type(unsigned lun, uint32_t lba)
{
	if (read_blocks(lun, lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

/* Decode a GPT entry's UTF-16 name into ASCII (unknown chars become '?'). */
static void part_name(const uint8_t *e, char *name)
{
	for (int c = 0; c < 16; c++) {
		uint8_t ch = e[56 + 2 * c];
		name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
	}
	name[16] = 0;
}

/* One line: name, first LBA, size in MiB (4K blocks). */
static void show_part(unsigned lun, const uint8_t *e, int detail)
{
	char name[17];
	uint64_t first = le64(e + 32), last = le64(e + 40);
	uint32_t mib = (uint32_t)((last - first + 1) >> 8);

	part_name(e, name);
	con_hex2(name, "lba=", (uint32_t)first, "MiB=", mib);
	if (detail)
		con_str("FS", fs_type(lun, (uint32_t)first));
}

/*
 * userdata goes first and is always shown with its filesystem: it is where
 * the iOS root will live.  The remaining entries fill what is left of the
 * 49-row console; the full map can be read later from recovery if needed.
 */
static void list_partitions(unsigned lun, uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	unsigned shown = 0;
	int have_ud = 0;

	if (read_blocks(lun, entry_lba, blocks, ents)) {
		con_str("PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		char name[17];

		if (!le64(e + 32))
			continue;
		part_name(e, name);
		if (same(name, "userdata")) {
			show_part(lun, e, 1);
			have_ud = 1;
		}
	}
	if (!have_ud)
		con_str("USERDATA", "not found");
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		char name[17];

		if (!le64(e + 32))
			continue;
		part_name(e, name);
		if (same(name, "userdata"))
			continue;
		if (shown++ >= 32) {
			con_str("PART", "... more");
			break;
		}
		show_part(lun, e, 0);
	}
}

/* Read LBA 0/1 of one LUN and, if the GPT signature is there, list it. */
static int gpt_scan(unsigned lun)
{
	static const uint8_t rd10[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 2, 0 };
	uint32_t ocs = 0, st = 0;
	uint32_t rc = scsi(lun, rd10, sizeof(rd10), data, 2 * BLOCK, &ocs, &st);

	if (rc) {
		con_hex2("READ", "lun=", lun, "rc=", rc);
		return rc;
	}
	const uint8_t *gpt = data + BLOCK;
	char sig[9];
	for (int i = 0; i < 8; i++)
		sig[i] = (gpt[i] >= 32 && gpt[i] < 127) ? gpt[i] : '.';
	sig[8] = 0;

	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16;
	uint32_t esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8 | gpt[74] << 16 | gpt[75] << 24;
	if (!same(sig, "EFI PART")) {
		con_hex2("GPT", "lun=", lun, "sig=", be32(gpt));
		return -1;
	}
	con_hex2("GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || !nent || nent > 128)
		return -1;
	list_partitions(lun, elba, nent);
	return 0;
}

void test_ufs(void)
{
	/*
	 * The previous milestone's GIC+SMP report fills the whole 49-row
	 * console (there is no scrolling: rows past the bottom are clipped),
	 * so the first UFS probe was invisible even if it worked.  Give UFS
	 * its own screen.
	 */
	con_clear(BLUE);
	con_str("SHIM", "fleur UFS partition map (read-only)");
	con_skip();

	ufs_clocks();
	con_skip();

	uint32_t ver = ufs_rd(REG_VER);
	uint32_t cap = ufs_rd(REG_CAP);
	uint32_t hce = ufs_rd(REG_HCE);
	uint32_t hcs = ufs_rd(REG_HCS);
	con_hex2("UFS", "ver=", ver, "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	if (ver == 0 || ver == 0xffffffffu) {
		con_str("UFS", "no valid VER: not a real HCI read");
		return;
	}
	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/link down: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	/* Take over the transfer request list: stop, point at ours, run. */
	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	/*
	 * Never ring a doorbell unless the host really uses our list: otherwise
	 * it would execute whatever LK left in its own slot 0 (maybe a write).
	 */
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	static const uint8_t tur[6] = { 0x00 };
	static const uint8_t cap10[10] = { 0x25 };	/* READ CAPACITY(10) */
	int have_gpt = 0;

	for (unsigned lun = 0; lun < 4; lun++) {
		uint32_t ocs = 0, st = 0, rc = 1;
		char ln[] = "LUN0";
		ln[3] = '0' + lun;

		/* A first command may report a unit attention; retry a few times. */
		for (int i = 0; i < 3 && rc; i++)
			rc = scsi(lun, tur, sizeof(tur), 0, 0, &ocs, &st);
		if (rc) {
			con_hex2(ln, "tur=", rc, "ocs/st=", ocs << 8 | st);
			continue;
		}

		ocs = st = 0;
		rc = scsi(lun, cap10, sizeof(cap10), data, 8, &ocs, &st);
		if (rc) {
			con_hex2("CAP", "lun=", lun, "rc=", rc);
			continue;
		}
		con_hex2(ln, "lastlba=", be32(data), "blk=", be32(data + 4));
		if (!have_gpt && gpt_scan(lun) == 0) {
			have_gpt = 1;
		}
	}
	if (!have_gpt)
		con_str("GPT", "no GPT on any LUN");

	con_skip();
	con_str("STATUS", "done");
}
