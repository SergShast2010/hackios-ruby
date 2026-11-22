/*
 * STRICTLY READ-ONLY UFS probe for Redmi Note 12 Pro 5G (ruby, MediaTek MT6877).
 * Adapted from lky005/ruby-ios-bringup (src/ufs-probe/ufs.c + 0080-ruby-ufs.patch).
 *
 * SAFETY CONTRACT:
 *   - NO SCSI WRITE command exists in this file. Only TEST UNIT READY (0x00),
 *     READ CAPACITY(10) (0x25), and READ(10) (0x28) can ever be issued.
 *   - Reuses the already-running UFSHCI link left up by LK (0x11270000); refuses
 *     to touch the host if HCE/DP/UTRLRDY are not set or UTRLDBR is busy.
 *   - Saves and restores UTRLBA/UTRLBAU/UTRLRSR cleanly after probing LUN 0..2.
 *   - Supports ruby's GPT layout (EFI PART at byte 512 of 4K block 0, entries at
 *     byte 1024) as well as standard block-1 GPT.
 *
 * Boot arg: mtk_ufs_ro=1.
 */
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);

#define UFS_BASE_PA     0x11270000UL
#define UFS_MMIO_SIZE   0x1000UL

#define REG_CAP         0x00u
#define REG_VER         0x08u
#define REG_IS          0x20u
#define REG_HCS         0x30u
#define REG_HCE         0x34u
#define REG_UTRLBA      0x50u
#define REG_UTRLBAU     0x54u
#define REG_UTRLDBR     0x58u
#define REG_UTRLCLR     0x5cu
#define REG_UTRLRSR     0x60u

#define CAP_64AS        (1u << 24)
#define HCS_DP          (1u << 0)
#define HCS_UTRLRDY     (1u << 1)

struct ufs_utrd {
	uint32_t dw0;
	uint32_t dw1;
	uint32_t dw2;   /* [7:0] OCS */
	uint32_t dw3;
	uint32_t ucdba;
	uint32_t ucdbau;
	uint32_t resp;  /* [31:16] response offset (dwords), [15:0] len (dwords) */
	uint32_t prdt;  /* [31:16] PRDT offset (dwords), [15:0] entries */
};

struct ufs_prdt {
	uint32_t addr_lo;
	uint32_t addr_hi;
	uint32_t rsvd;
	uint32_t dbc;
};

struct ufs_ucd {
	uint8_t cmd[512];
	uint8_t resp[512];
	struct ufs_prdt prdt[2];
};

/* Kept inside single 16 KiB XNU pages so kvtophys() guarantees physical contiguity. */
struct ufs_dma_meta {
	struct ufs_utrd utrl[32] __attribute__((aligned(1024)));
	struct ufs_ucd  ucd      __attribute__((aligned(128)));
	uint8_t         capbuf[64] __attribute__((aligned(64)));
} __attribute__((aligned(16384)));

static struct ufs_dma_meta ufs_meta __attribute__((aligned(16384)));
static uint8_t ufs_data0[16384] __attribute__((aligned(16384)));
static uint8_t ufs_data1[16384] __attribute__((aligned(16384)));

static vm_offset_t ufs_mmio;
static uint8_t ufs_tag;

static inline uint32_t
ufs_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(ufs_mmio + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
ufs_wr(uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(ufs_mmio + off) = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
ufs_dcache_civac(const void *addr, size_t len)
{
	uintptr_t p = ((uintptr_t)addr) & ~(uintptr_t)63u;
	uintptr_t end = (uintptr_t)addr + len;
	while (p < end) {
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
		p += 64u;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t ufs_be32(const uint8_t *p) {
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint32_t ufs_le32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t ufs_le64(const uint8_t *p) {
	return (uint64_t)ufs_le32(p) | ((uint64_t)ufs_le32(p + 4) << 32);
}

static uint64_t
ufs_ticks(void)
{
	uint64_t t;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
	return t;
}

static uint64_t
ufs_freq(void)
{
	uint64_t f;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	return f ? f : 13000000ULL;
}

/* Strictly read-only SCSI transfer (direction is always NONE or DATA-IN). */
static uint32_t
ufs_scsi_ro(uint8_t lun, const uint8_t *cdb, unsigned cdb_len,
            void *buf0, uint32_t len0, void *buf1, uint32_t len1,
            uint32_t *ocs_out, uint32_t *st_out)
{
	uint32_t total = len0 + len1;
	bzero(&ufs_meta.utrl[0], sizeof(ufs_meta.utrl[0]));
	bzero(&ufs_meta.ucd, sizeof(ufs_meta.ucd));

	ufs_meta.ucd.cmd[0] = 0x01;                     /* COMMAND UPIU */
	ufs_meta.ucd.cmd[1] = total ? 0x40 : 0x00;      /* R (data-in) or none */
	ufs_meta.ucd.cmd[2] = lun;
	ufs_meta.ucd.cmd[3] = ++ufs_tag;
	ufs_meta.ucd.cmd[12] = (uint8_t)(total >> 24);
	ufs_meta.ucd.cmd[13] = (uint8_t)(total >> 16);
	ufs_meta.ucd.cmd[14] = (uint8_t)(total >> 8);
	ufs_meta.ucd.cmd[15] = (uint8_t)total;
	for (unsigned i = 0; i < cdb_len && i < 16u; i++) {
		ufs_meta.ucd.cmd[16 + i] = cdb[i];
	}

	uint64_t ucd_pa = kvtophys((vm_offset_t)&ufs_meta.ucd);
	ufs_meta.utrl[0].dw0 = (1u << 28) | ((total ? 2u : 0u) << 25) | (1u << 24);
	ufs_meta.utrl[0].dw2 = 0x0fu;
	ufs_meta.utrl[0].ucdba  = (uint32_t)ucd_pa;
	ufs_meta.utrl[0].ucdbau = (uint32_t)(ucd_pa >> 32);
	ufs_meta.utrl[0].resp   = ((512u / 4u) << 16) | (512u / 4u);

	uint32_t nprdt = 0;
	if (len0 && buf0) {
		uint64_t p0 = kvtophys((vm_offset_t)buf0);
		ufs_meta.ucd.prdt[nprdt].addr_lo = (uint32_t)p0;
		ufs_meta.ucd.prdt[nprdt].addr_hi = (uint32_t)(p0 >> 32);
		ufs_meta.ucd.prdt[nprdt].dbc = len0 - 1u;
		nprdt++;
		ufs_dcache_civac(buf0, len0);
	}
	if (len1 && buf1) {
		uint64_t p1 = kvtophys((vm_offset_t)buf1);
		ufs_meta.ucd.prdt[nprdt].addr_lo = (uint32_t)p1;
		ufs_meta.ucd.prdt[nprdt].addr_hi = (uint32_t)(p1 >> 32);
		ufs_meta.ucd.prdt[nprdt].dbc = len1 - 1u;
		nprdt++;
		ufs_dcache_civac(buf1, len1);
	}
	if (nprdt) {
		ufs_meta.utrl[0].prdt = ((1024u / 4u) << 16) | nprdt;
	}
	ufs_dcache_civac(&ufs_meta.utrl[0], sizeof(ufs_meta.utrl[0]));
	ufs_dcache_civac(&ufs_meta.ucd, sizeof(ufs_meta.ucd));

	ufs_wr(REG_UTRLDBR, 1u);
	uint64_t start = ufs_ticks();
	uint64_t limit = ufs_freq(); /* 1 s timeout */
	while (ufs_rd(REG_UTRLDBR) & 1u) {
		if (ufs_ticks() - start > limit) {
			ufs_wr(REG_UTRLCLR, ~1u);
			return 1u;
		}
	}

	ufs_dcache_civac(&ufs_meta.utrl[0], sizeof(ufs_meta.utrl[0]));
	ufs_dcache_civac(&ufs_meta.ucd.resp, sizeof(ufs_meta.ucd.resp));
	if (len0 && buf0) ufs_dcache_civac(buf0, len0);
	if (len1 && buf1) ufs_dcache_civac(buf1, len1);

	*ocs_out = ufs_meta.utrl[0].dw2 & 0xffu;
	*st_out  = ufs_meta.ucd.resp[7];
	if (*ocs_out) return 2u;
	if (ufs_meta.ucd.resp[0] != 0x21u) return 3u;
	return *st_out ? 4u : 0u;
}

static uint32_t
ufs_tur(uint8_t lun, uint32_t *ocs, uint32_t *st)
{
	static const uint8_t cdb[6] = { 0x00 };
	return ufs_scsi_ro(lun, cdb, sizeof(cdb), NULL, 0, NULL, 0, ocs, st);
}

static uint32_t
ufs_capacity(uint8_t lun, uint32_t *last_lba, uint32_t *blk_sz, uint32_t *ocs, uint32_t *st)
{
	static const uint8_t cdb[10] = { 0x25 };
	uint32_t rc = ufs_scsi_ro(lun, cdb, sizeof(cdb), ufs_meta.capbuf, 8u, NULL, 0, ocs, st);
	if (!rc) {
		*last_lba = ufs_be32(ufs_meta.capbuf);
		*blk_sz   = ufs_be32(ufs_meta.capbuf + 4);
	}
	return rc;
}

static uint32_t
ufs_read10_32k(uint8_t lun, uint32_t lba, uint16_t blocks, uint32_t *ocs, uint32_t *st)
{
	uint8_t cdb[10] = {
		0x28, 0,
		(uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
		0, (uint8_t)(blocks >> 8), (uint8_t)blocks, 0
	};
	return ufs_scsi_ro(lun, cdb, sizeof(cdb), ufs_data0, 16384u, ufs_data1, 16384u, ocs, st);
}

static uint8_t
ufs_byte(uint32_t off)
{
	return (off < 16384u) ? ufs_data0[off] : ufs_data1[off - 16384u];
}

static void
ufs_copy_bytes(uint8_t *dst, uint32_t off, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		dst[i] = ufs_byte(off + i);
	}
}

static void
ufs_parse_gpt(uint32_t lun, uint32_t blk_sz)
{
	uint8_t hdr[92];
	uint32_t hdr_off = 0, ent_off = 0;

	ufs_copy_bytes(hdr, 512u, sizeof(hdr));
	if (memcmp(hdr, "EFI PART", 8) == 0) {
		hdr_off = 512u;
		ent_off = 1024u;
	} else {
		ufs_copy_bytes(hdr, 4096u, sizeof(hdr));
		if (memcmp(hdr, "EFI PART", 8) == 0) {
			hdr_off = 4096u;
			ent_off = (uint32_t)(ufs_le64(hdr + 72) * blk_sz);
		} else {
			printf("mtk_ufs_ro: LUN%u no GPT in first 32 KiB\n", lun);
			return;
		}
	}

	uint32_t nent = ufs_le32(hdr + 80);
	uint32_t esz  = ufs_le32(hdr + 84);
	if (esz != 128u || nent == 0u) {
		printf("mtk_ufs_ro: LUN%u unsupported GPT (%u x %u)\n", lun, nent, esz);
		return;
	}

	uint32_t max_in_buf = (ent_off < 32768u) ? ((32768u - ent_off) / esz) : 0u;
	uint32_t scan = nent < max_in_buf ? nent : max_in_buf;
	uint32_t valid = 0;
	uint32_t lba_unit = (hdr_off == 512u) ? 512u : blk_sz;

	printf("mtk_ufs_ro: LUN%u GPT OK (hdr@%u ent@%u nent=%u)\n", lun, hdr_off, ent_off, nent);
	for (uint32_t i = 0; i < scan; i++) {
		uint8_t e[128];
		ufs_copy_bytes(e, ent_off + i * esz, 128u);
		uint64_t first = ufs_le64(e + 32);
		uint64_t last  = ufs_le64(e + 40);
		if (!first && !last) {
			continue;
		}
		valid++;
		char name[20];
		unsigned c;
		for (c = 0; c < 19u; c++) {
			uint8_t ch = e[56 + 2 * c];
			if (!ch) break;
			name[c] = (ch >= 32 && ch < 127) ? (char)ch : '?';
		}
		name[c] = 0;
		if (strcmp(name, "boot_a") == 0 || strcmp(name, "boot_b") == 0 ||
		    strcmp(name, "super") == 0 || strcmp(name, "userdata") == 0 ||
		    strcmp(name, "vbmeta_a") == 0 || strcmp(name, "metadata") == 0) {
			uint64_t mib = ((last - first + 1ULL) * lba_unit) >> 20;
			printf("mtk_ufs_ro: part %-10s lba=0x%llx size=%llu MiB\n",
			    name, (unsigned long long)first, (unsigned long long)mib);
		}
	}
	printf("mtk_ufs_ro: LUN%u scanned=%u active_parts=%u (READ-ONLY)\n", lun, scan, valid);
}

void
hakios_mtk_ufs_ro_init(void)
{
	int enabled = 0;
	if (!PE_parse_boot_argn("mtk_ufs_ro", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}

	ufs_mmio = ml_io_map(UFS_BASE_PA, UFS_MMIO_SIZE);
	if (!ufs_mmio) {
		printf("mtk_ufs_ro: ml_io_map(0x%lx) failed\n", UFS_BASE_PA);
		return;
	}

	uint32_t cap = ufs_rd(REG_CAP);
	uint32_t ver = ufs_rd(REG_VER);
	uint32_t hce = ufs_rd(REG_HCE);
	uint32_t hcs = ufs_rd(REG_HCS);
	uint32_t dbr = ufs_rd(REG_UTRLDBR);
	printf("mtk_ufs_ro: ver=0x%08x cap=0x%08x hce=0x%x hcs=0x%x dbr=0x%x\n",
	    ver, cap, hce, hcs, dbr);

	if (!(hce & 1u) || !(hcs & HCS_DP) || !(hcs & HCS_UTRLRDY)) {
		printf("mtk_ufs_ro: host not ready; skip\n");
		return;
	}
	if (dbr) {
		printf("mtk_ufs_ro: outstanding requests (dbr=0x%x); skip\n", dbr);
		return;
	}

	uint64_t list_pa = kvtophys((vm_offset_t)ufs_meta.utrl);
	uint64_t d0_pa   = kvtophys((vm_offset_t)ufs_data0);
	uint64_t d1_pa   = kvtophys((vm_offset_t)ufs_data1);
	if (!list_pa || !d0_pa || !d1_pa) {
		printf("mtk_ufs_ro: kvtophys failed\n");
		return;
	}
	if (((list_pa | d0_pa | d1_pa) >= 0x100000000ULL) && !(cap & CAP_64AS)) {
		printf("mtk_ufs_ro: DMA PA >4G but CAP.64AS=0; skip\n");
		return;
	}

	/* Save LK's UTRL registers and restore them when our read-only probe finishes. */
	uint32_t orig_lo  = ufs_rd(REG_UTRLBA);
	uint32_t orig_hi  = ufs_rd(REG_UTRLBAU);
	uint32_t orig_rsr = ufs_rd(REG_UTRLRSR);

	ufs_wr(REG_UTRLRSR, 0u);
	ufs_wr(REG_UTRLBA,  (uint32_t)list_pa);
	ufs_wr(REG_UTRLBAU, (uint32_t)(list_pa >> 32));
	ufs_wr(REG_UTRLRSR, 1u);

	if (ufs_rd(REG_UTRLBA) != (uint32_t)list_pa ||
	    ufs_rd(REG_UTRLBAU) != (uint32_t)(list_pa >> 32) ||
	    !(ufs_rd(REG_UTRLRSR) & 1u)) {
		printf("mtk_ufs_ro: UTRLBA not accepted (pa=0x%llx)\n", (unsigned long long)list_pa);
		goto restore;
	}

	for (uint32_t lun = 0; lun < 3u; lun++) {
		uint32_t ocs = 0, st = 0, rc = 0;
		for (int t = 0; t < 3; t++) {
			rc = ufs_tur((uint8_t)lun, &ocs, &st);
			if (!rc) break;
		}
		uint32_t last_lba = 0, blk_sz = 0;
		uint32_t crc = ufs_capacity((uint8_t)lun, &last_lba, &blk_sz, &ocs, &st);
		if (crc || !blk_sz) {
			printf("mtk_ufs_ro: LUN%u tur=%u cap_rc=%u ocs=%u st=%u\n", lun, rc, crc, ocs, st);
			continue;
		}
		uint64_t total_mb = (((uint64_t)last_lba + 1ULL) * blk_sz) >> 20;
		printf("mtk_ufs_ro: LUN%u blk=%u last=0x%x (%llu MiB)\n",
		    lun, blk_sz, last_lba, (unsigned long long)total_mb);

		if (lun == 2u && blk_sz == 4096u) {
			uint32_t rrc = ufs_read10_32k((uint8_t)lun, 0u, 8u, &ocs, &st);
			if (rrc) {
				printf("mtk_ufs_ro: LUN2 read failed rc=%u ocs=%u st=%u\n", rrc, ocs, st);
			} else {
				ufs_parse_gpt(lun, blk_sz);
			}
		}
	}

restore:
	ufs_wr(REG_UTRLRSR, 0u);
	ufs_wr(REG_UTRLBA,  orig_lo);
	ufs_wr(REG_UTRLBAU, orig_hi);
	if (orig_rsr & 1u) {
		ufs_wr(REG_UTRLRSR, 1u);
	}
}
