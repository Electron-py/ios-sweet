/*
 * Milestone 5 probe: can we talk to the internal UFS that LK booted from?
 *
 * Runs with the 16K EL1 MMU on and the image mapped cacheable, so the DMA
 * descriptors and buffers are cleaned before the doorbell and invalidated
 * after completion.  Enables the MT6781 UFS clock gates first (LK may gate
 * them at handoff, and a read of a gated block can wedge the interconnect),
 * dumps what LK left in the controller/MPHY, then - only if the host is
 * enabled and the link is up - takes over the transfer request list and
 * issues READ-ONLY SCSI commands.  Nothing is ever written to the device:
 * there is no SCSI WRITE in this file.
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
#define INFRA2_STA	(INFRA_AO + 0xac)
#define INFRA3_SET	(INFRA_AO + 0xc0)
#define INFRA3_STA	(INFRA_AO + 0xc8)

#define G_UNIPRO_SCK	(1u << 11)	/* 0x3e, "ufs0-unipro-clk" */
#define G_MP_SAP_BCLK	(1u << 13)	/* 0x40, "ufs0-mp-clk" */
#define G_AES_UFS	(1u << 29)	/* 0x75, "ufs0-aes-clk" */
#define G_UFS		(1u << 30)	/* 0x4f, "ufs0-clock" */
#define G_UFS_TICK	(1u << 31)	/* CLK_INFRA_UFS_TICK, not in DT */
#define G_UFS_AXI	(1u << 5)	/* INFRA3, DMA path */

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

static void wait_ms(unsigned ms)
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
 * LK reads the boot image from this UFS, so it must leave the controller
 * clocked; but if it gates the gates at handoff, any read of 0x11270000 can
 * wedge the interconnect.  Enable the four DT clocks first (idempotent) and
 * report what LK left behind.
 */
static void ufs_clocks(void)
{
	uint32_t b2 = rd32a(INFRA2_STA), b3 = rd32a(INFRA3_STA);

	con_hex2("CLK", "infra2=", b2, "infra3=", b3);
	wr32a(INFRA2_SET, G_UFS | G_UNIPRO_SCK | G_MP_SAP_BCLK | G_AES_UFS | G_UFS_TICK);
	wr32a(INFRA3_SET, G_UFS_AXI);
	__asm__ volatile("dsb sy" ::: "memory");
	con_hex2("CLK", "infra2a=", rd32a(INFRA2_STA), "infra3a=", rd32a(INFRA3_STA));
}

/* Read-only view of the MPHY: tells us whether LK left it powered. */
static void mphy_report(void)
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

/* One line per GPT entry of at least 64 MiB: name, first LBA, size in MiB (4K blocks). */
static void list_partitions(unsigned lun, uint32_t entry_lba, uint32_t nent)
{
	static uint8_t ents[128 * 128] __attribute__((aligned(4096)));
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	unsigned shown = 0;

	if (read_blocks(lun, entry_lba, blocks, ents)) {
		con_str("PART", "read error");
		return;
	}
	for (uint32_t i = 0; i < nent; i++) {
		const uint8_t *e = ents + i * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (!first)
			continue;
		char name[17];
		for (int c = 0; c < 16; c++) {
			uint8_t ch = e[56 + 2 * c];
			name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
		}
		name[16] = 0;
		uint32_t mib = (uint32_t)((last - first + 1) >> 8);
		if (mib < 64)		/* keep the screen to the big ones */
			continue;
		if (shown++ >= 10) {
			con_str("PART", "... more");
			break;
		}
		con_hex2(name, "lba=", (uint32_t)first, "MiB=", mib);
		if (same(name, "userdata") || same(name, "super"))
			con_str(" fs", fs_type(lun, (uint32_t)first));
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
	con_hex2("MBR", "sig=", data[510] | data[511] << 8, "lun=", lun);
	con_str("GPT", sig);
	con_hex2("GPT", "entries=", nent, "lba=", elba);
	if (esz != 128 || !nent || nent > 128 || !same(sig, "EFI PART"))
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
	con_str("SHIM", "fleur UFS probe (read-only)");
	con_skip();

	ufs_clocks();

	/* Print first, then wait: if the first read wedges the bus, the last
	 * visible line says exactly what we were about to do. */
	con_skip();
	con_str("UFS", "MPHY+HCI read in 1 s...");
	wait_ms(1000);

	mphy_report();
	con_hex2("REF", "144=", ufs_rd(REG_REFCLK), "rsr=", ufs_rd(REG_UTRLRSR));

	uint32_t ver = ufs_rd(REG_VER);
	uint32_t cap = ufs_rd(REG_CAP);
	uint32_t hce = ufs_rd(REG_HCE);
	uint32_t hcs = ufs_rd(REG_HCS);
	con_hex2("UFS", "ver=", ver, "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		con_str("NEXT", "clock/PHY were off; full re-init needed");
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
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
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
	unsigned gpt_lun = 0xffff;
	int have_gpt = 0;

	for (unsigned lun = 0; lun < 4; lun++) {
		uint32_t ocs = 0, st = 0, rc = 1;

		/* A first command may report a unit attention; retry a few times. */
		for (int i = 0; i < 3 && rc; i++)
			rc = scsi(lun, tur, sizeof(tur), 0, 0, &ocs, &st);
		char ln[] = "LUN0";
		ln[3] = '0' + lun;
		con_hex2(ln, "tur=", rc, "ocs/st=", ocs << 8 | st);
		if (rc)
			continue;

		ocs = st = 0;
		rc = scsi(lun, cap10, sizeof(cap10), data, 8, &ocs, &st);
		if (rc) {
			con_hex2("CAP", "lun=", lun, "rc=", rc);
			continue;
		}
		con_hex2("CAP", "lastlba=", be32(data), "blk=", be32(data + 4));
		if (!have_gpt && gpt_scan(lun) == 0) {
			have_gpt = 1;
			gpt_lun = lun;
		}
	}
	con_hex2("GPT", "found=", have_gpt, "lun=", gpt_lun);

	con_skip();
	con_str("STATUS", have_gpt ? "UFS read path works" : "no GPT found");
}
