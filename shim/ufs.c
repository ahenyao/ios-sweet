/*
 * Milestone 5 probe: talk to the internal UFS that ABL booted from.
 * Runs with the MMU off (all memory Device, so DMA is coherent).
 * Modified for Exynos 2200 (Galaxy S22+ / g0s).
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define UFS_BASE	0x11100000UL	/* ufs@0x11100000 on g0s */

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

#define HCS_DP		(1u << 0)
#define HCS_UTRLRDY	(1u << 1)
#define HCS_UCRDY	(1u << 3)

#define BLOCK		4096u

static inline uint32_t ufs_rd(uint32_t off) { return *(volatile uint32_t *)(UFS_BASE + off); }
static inline void ufs_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(UFS_BASE + off) = v; }

/* UTP transfer request descriptor (32 bytes). */
struct utrd {
	uint32_t dw0;
	uint32_t dw1;
	uint32_t dw2;
	uint32_t dw3;
	uint32_t ucdba;
	uint32_t ucdbau;
	uint32_t resp;
	uint32_t prdt;
};

/* UTP command descriptor: command UPIU, response UPIU, PRDT. */
struct ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct {
		uint32_t addr_lo, addr_hi, rsvd, dbc;
	} prdt[1];
};

static struct utrd utrl[32] __attribute__((aligned(1024)));
static struct ucd ucd __attribute__((aligned(128)));

/* Allocate DMA buffer in the shim's bootloader-approved .bss to prevent Exynos SMMU faults */
static uint8_t data[8 * BLOCK] __attribute__((aligned(4096)));

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
 * Issue one SCSI command. Returns 0 on success.
 */
static uint32_t scsi(uint8_t lun, const uint8_t *cdb, unsigned cdb_len, void *buf, uint32_t len,
    uint32_t *ocs_out, uint32_t *status_out)
{
	static uint8_t tag;
	zero(&utrl[0], sizeof(utrl[0]));
	zero(&ucd, sizeof(ucd));

	ucd.cmd[0] = 0x01;
	ucd.cmd[1] = len ? 0x40 : 0x00;
	ucd.cmd[2] = lun;
	ucd.cmd[3] = ++tag;
	ucd.cmd[12] = len >> 24;
	ucd.cmd[13] = len >> 16;
	ucd.cmd[14] = len >> 8;
	ucd.cmd[15] = len;
	for (unsigned i = 0; i < cdb_len; i++)
		ucd.cmd[16 + i] = cdb[i];

	uintptr_t ucd_pa = (uintptr_t)&ucd;
	utrl[0].dw0 = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);
	utrl[0].dw2 = 0x0f;
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

	ufs_wr(REG_UTRLDBR, 1);
	uint64_t start = ticks();
	while (ufs_rd(REG_UTRLDBR) & 1) {
		if (ticks() - start > 25600000) {	/* 1 s at 25.6 MHz */
			ufs_wr(REG_UTRLCLR, 1u);	/* Write 1 to clear slot 0 */
			return 1;			/* timeout */
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	*ocs_out = utrl[0].dw2 & 0xff;
	*status_out = ucd.resp[7];
	if (*ocs_out)
		return 2;
	if (ucd.resp[0] != 0x21)
		return 3;
	return *status_out ? 4 : 0;
}

static uint32_t read_blocks(uint32_t lba, uint32_t count, void *buf)
{
	uint8_t cdb[10] = { 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, count >> 8, count, 0 };
	uint32_t ocs, st;
	return scsi(0, cdb, sizeof(cdb), buf, count * BLOCK, &ocs, &st);
}

static uint64_t le64(const uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = v << 8 | p[i];
	return v;
}

static const char *fs_type(uint32_t lba)
{
	/* Read 1 block to avoid SMMU faults */
	if (read_blocks(lba, 1, data))
		return "read error";
	const uint8_t *sb = data + 1024;
	if (sb[0] == 0x10 && sb[1] == 0x20 && sb[2] == 0xf5 && sb[3] == 0xf2)
		return "f2fs";
	if (sb[56] == 0x53 && sb[57] == 0xef)
		return "ext4";
	return "other";
}

static void list_partitions(uint32_t entry_lba, uint32_t nent)
{
	uint8_t *ents_buf = data + 8192; /* Protect buffer from fs_type calls */
	uint32_t blocks = (nent * 128 + BLOCK - 1) / BLOCK;
	
	for (uint32_t b = 0; b < blocks; b++) {
		/* Strictly read 1 block (4KB) at a time to satisfy Exynos SMMU */
		if (read_blocks(entry_lba + b, 1, ents_buf)) {
			con_str("UFS PART", "read error");
			return;
		}
		
		uint32_t entries_in_block = BLOCK / 128; /* 32 */
		for (uint32_t i = 0; i < entries_in_block; i++) {
			uint32_t global_i = b * entries_in_block + i;
			if (global_i >= nent) break;
			
			const uint8_t *e = ents_buf + (i * 128);
			uint64_t first = le64(e + 32), last = le64(e + 40);
			if (!first)
				continue;
				
			char name[17];
			for (int c = 0; c < 16; c++) {
				uint8_t ch = e[56 + 2 * c];
				name[c] = (ch >= 32 && ch < 127) ? ch : (ch ? '?' : 0);
			}
			name[16] = 0;
			if (((last - first + 1) >> 8) < 64)
				continue;
				
			con_hex2(name, "lba=", (uint32_t)first, "MiB=", (uint32_t)((last - first + 1) >> 8));
			if (!__builtin_strcmp(name, "userdata") || !__builtin_strcmp(name, "cust") ||
			    !__builtin_strcmp(name, "cache"))
				con_str(" fs", fs_type((uint32_t)first));
		}
	}
}

void test_ufs(void)
{
	con_str("UFS", "milestone 5 probe (read-only)");
	uint32_t cap = ufs_rd(REG_CAP), hcs = ufs_rd(REG_HCS), hce = ufs_rd(REG_HCE);
	con_hex2("UFS", "ver=", ufs_rd(REG_VER), "cap=", cap);
	con_hex2("UFS", "hce=", hce, "hcs=", hcs);
	con_hex2("UFS", "is=", ufs_rd(REG_IS), "rsr=", ufs_rd(REG_UTRLRSR));
	con_hex2("UFS", "utrlba=", ufs_rd(REG_UTRLBA), "hi=", ufs_rd(REG_UTRLBAU));
	con_hex2("UFS", "dbr=", ufs_rd(REG_UTRLDBR), "nutrs=", (cap & 0x1f) + 1);

	if (!(hce & 1) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		con_str("UFS", "host not enabled/ready: stop");
		return;
	}
	if (ufs_rd(REG_UTRLDBR)) {
		con_str("UFS", "requests outstanding: stop");
		return;
	}

	ufs_wr(REG_UTRLRSR, 0);
	uintptr_t list = (uintptr_t)utrl;
	ufs_wr(REG_UTRLBA, (uint32_t)list);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list >> 32));
	ufs_wr(REG_UTRLRSR, 1);
	con_hex2("UFS", "ours=", (uint32_t)list, "rsr=", ufs_rd(REG_UTRLRSR));
	
	if (ufs_rd(REG_UTRLBA) != (uint32_t)list || ufs_rd(REG_UTRLBAU) != (uint32_t)(list >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1)) {
		con_str("UFS", "list base not taken: stop");
		return;
	}

	uint32_t ocs = 0, st = 0, rc;
	static const uint8_t req_sense[6] = { 0x03, 0, 0, 0, 18, 0 };
	static const uint8_t start_unit[6] = { 0x1b, 0, 0, 0, 0x11, 0 };
	static const uint8_t tur[6] = { 0x00 };

	/* 1. Wake global controller via W-LUN 0xD0 */
	rc = scsi(0xD0, req_sense, sizeof(req_sense), data, 18, &ocs, &st);
	rc = scsi(0xD0, start_unit, sizeof(start_unit), 0, 0, &ocs, &st);
	con_hex2("UFS WAKE 0xD0", "rc=", rc, "ocs/st=", ocs << 8 | st);

	for (volatile int d = 0; d < 20000000; d++);

	/* 2. Verify LUN 0 is ready */
	for (int i = 0; i < 3; i++) {
		rc = scsi(0, tur, sizeof(tur), 0, 0, &ocs, &st);
		con_hex2("UFS TUR", "rc=", rc, "ocs/st=", ocs << 8 | st);
		if (!rc) break;
		scsi(0, req_sense, sizeof(req_sense), data, 18, &ocs, &st);
	}

	/* 3. Read Capacity */
	static const uint8_t cap10[10] = { 0x25 };
	rc = scsi(0, cap10, sizeof(cap10), data, 8, &ocs, &st);
	con_hex2("UFS CAP", "rc=", rc, "ocs/st=", ocs << 8 | st);
	if (!rc)
		con_hex2("UFS LUN0", "lastlba=", be32(data), "blk=", be32(data + 4));

	/* 4. Read LBA 0 (1 block = 4K) */
	rc = read_blocks(0, 1, data);
	con_hex2("UFS READ LBA0", "rc=", rc, "ocs/st=", 0);
	if (rc) return;
	
	con_hex2("UFS MBR", "sig=", data[510] | (data[511] << 8), "type=", data[450]);

	/* Find GPT: Check 512-byte offset, or fetch LBA 1 if standard 4K blocks */
	const uint8_t *gpt = 0;
	if (data[512] == 'E' && data[513] == 'F' && data[514] == 'I' && data[515] == ' ') {
		gpt = data + 512;
	} else {
		rc = read_blocks(1, 1, data + 4096);
		if (rc) { con_hex2("UFS READ LBA1", "rc=", rc, 0, 0); return; }
		if (data[4096] == 'E' && data[4097] == 'F' && data[4098] == 'I' && data[4099] == ' ') {
			gpt = data + 4096;
		}
	}

	if (!gpt) {
		con_str("UFS GPT", "not found");
		return;
	}

	char sig[9];
	for (int i = 0; i < 8; i++) sig[i] = gpt[i];
	sig[8] = 0;
	con_str("UFS GPT", sig);

	uint32_t nent = gpt[80] | gpt[81] << 8 | gpt[82] << 16;
	uint32_t esz = gpt[84] | gpt[85] << 8;
	uint32_t elba = gpt[72] | gpt[73] << 8;
	con_hex2("UFS GPT", "entries=", nent, "lba=", elba);

	if (esz != 128 || nent > 128 || nent == 0) return;

	/* List the partitions found in the GPT */
	list_partitions(elba, nent);
}