/*
 * MediaTek MT6877 (Dimensity 1080 / ruby) Mali-G68 MC4 (Valhall v9, Job Manager) kernel driver:
 *   Stage 1 (v0.62a):
 *     1. PMIF_SPI (0x10026000) -> MT6359P PMIC rails:
 *        - ldo_vsram_md (VSRAM_GPU): 0.75 V (sel 0x28, 500000 + sel*6250 uV)
 *        - buck_vproc2  (VGPU):      0.70 V (sel 0x30, 400000 + sel*6250 uV)
 *          (keeps 0 <= VSRAM - VGPU <= 200 mV)
 *     2. TOPCKGEN (0x10000000) CLK_CFG_20: park MFG on 26 MHz (tck_26m_mx9_ck)
 *     3. SPM (0x10006000) + INFRACFG_AO (0x10001000):
 *        - Power on MFG0 -> MFG1 (release Bus Protect Step 2 & Step 1) -> MFG2..MFG5
 *        - Verify XPU_PWR_STATUS / XPU_PWR_STATUS_2ND and SRAM_PDN_ACK before any 0x13xxxxxx access
 *     4. GPU_PLL_CTRL (0x13FA0000) MFGPLL1 + MFGCFG (0x13FBF000) BG3D clock gate:
 *        - Enable MFGPLL1 at safe frequency (default 450 MHz, override via mali_freq=MHz)
 *        - Switch CLK_CFG_20 mfg_internal2_sel -> MFGPLL1, ungate BG3D
 *     5. Mali-G68 MMIO (0x13000000, 16 KiB):
 *        - Read GPU_ID, features, *_PRESENT
 *        - Issue SOFT_RESET, wait RESET_COMPLETED
 *        - Power on L2, Tiler and Shader cores, verify *_READY
 *   Stage 2 (v0.62b):
 *     6. GICv3 SPI interrupts (MT6877 ruby_live.dts):
 *        - GPU = SPI 453 (INTID 485), MMU = SPI 454 (INTID 486), JOB = SPI 455 (INTID 487)
 *        - Level-high with spurious-IRQ auto-mask guard + GPU cache-flush IRQ verification
 *     7. AArch64_4K MMU (4-level for 48-bit VA on Mali-G68, 3-level fallback for <=39-bit VA)
 *        + 64 KiB granule GPU VA allocator [16 MiB .. 4 GiB)
 *     8. Job Slots JS0 (fragment) / JS1 (vertex/tiler/compute) + worker queue + recovery
 *     9. 3-stage hardware self-test (identity mode -> AArch64_4K MMU -> full allocator/worker API)
 *        + exported mtk_mali_* C API for Stage 3 IOKit HakiosMali.cpp
 *
 * Persistent telemetry: sysctl kern.hakios.mali
 * Boot args: mali=1 [mali_freq=450] [mali_irq=1] [mali_fastmap=1] [mali_walk_wb=0]
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern vm_offset_t phystokv(uint64_t pa);
extern void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);
extern void thread_set_thread_name(thread_t th, const char *name);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern wait_result_t assert_wait_deadline(event_t event,
    wait_interrupt_t interruptible, uint64_t deadline);
extern kern_return_t clear_wait(thread_t thread, wait_result_t result);
extern int proc_selfpid(void);
extern void proc_selfname(char *buf, int size);

SYSCTL_DECL(_kern_hakios);

/* Physical MMIO bases verified against live ruby FDT & MT6877 Linux kernel */
#define MT6877_TOPCKGEN_PA      0x10000000UL
#define MT6877_INFRACFG_PA      0x10001000UL
#define MT6877_SPM_PA           0x10006000UL
#define MT6877_PMIF_PA          0x10026000UL
#define MT6877_MALI_PA          0x13000000UL
#define MT6877_GPU_PLL_PA       0x13FA0000UL
#define MT6877_MFGCFG_PA        0x13FBF000UL

/* GICv3 INTIDs from ruby_live.dts: mali@13000000 SPI 0x1c5..0x1c7, level-high (0x04) */
#define MALI_IRQ_GPU            (453u + 32u)    /* 485 */
#define MALI_IRQ_MMU            (454u + 32u)    /* 486 */
#define MALI_IRQ_JOB            (455u + 32u)    /* 487 */
#define MALI_IRQ_SPURIOUS_LIMIT 64u

/* PMIF_SPI (PWRAP ARB v3, SWINF_2) */
#define PMIF_INIT_DONE          0x000u
#define PMIF_SWINF_2_ACC        0x880u
#define PMIF_SWINF_2_WDATA      0x884u
#define PMIF_SWINF_2_RDATA      0x894u
#define PMIF_SWINF_2_VLD_CLR    0x8A4u
#define PMIF_SWINF_2_STA        0x8A8u
#define PMIF_FSM_IDLE           0u
#define PMIF_FSM_WFVLDCLR       6u

/* MT6359P PMIC 16-bit register addresses (MT6359_PMIC_REG_BASE = 0x0) */
#define MT6359_HWCID            0x0008u
#define MT6359_SWCID            0x000Au
/* buck_vproc2 (VGPU): 400000 + sel * 6250 uV */
#define MT6359_VPROC2_CON0      0x1788u  /* RG_BUCK_VPROC2_EN: bit 0 */
#define MT6359_VPROC2_DBG0      0x17A6u  /* DA_VPROC2_VOSEL: [6:0] */
#define MT6359_VPROC2_DBG1      0x17A8u  /* DA_VPROC2_EN: bit 0 */
#define MT6359_VPROC2_ELR0      0x17B2u  /* RG_BUCK_VPROC2_VOSEL: [6:0] */
/* ldo_vsram_md (VSRAM_GPU): 500000 + sel * 6250 uV */
#define MT6359_VSRAM_MD_ELR     0x1B4Au  /* RG_LDO_VSRAM_MD_VOSEL: [6:0] */
#define MT6359_VSRAM_MD_CON0    0x1F2Eu  /* RG_LDO_VSRAM_MD_EN: bit 0 */
#define MT6359_VSRAM_MD_MON     0x1F32u  /* DA_VSRAM_MD_B_EN: bit 0 */
#define MT6359_VSRAM_MD_VOSEL1  0x1F36u  /* DA_VSRAM_MD_VOSEL: [14:8] */

#define VSRAM_TARGET_SEL        0x28u    /* 500000 + 40*6250 = 750000 uV (0.75 V) */
#define VGPU_TARGET_SEL         0x30u    /* 400000 + 48*6250 = 700000 uV (0.70 V) */

/* TOPCKGEN CLK_CFG_20 */
#define TOP_CLK_CFG_20          0x0120u
#define TOP_CLK_CFG_20_SET      0x0124u
#define TOP_CLK_CFG_20_CLR      0x0128u
#define TOP_MFG_INT2_SEL        (1u << 18)  /* 0=mfg_internal1_ck, 1=mfgpll1_ck */
#define TOP_MFG_INT1_SEL        (1u << 19)  /* 0=26MHz, 1=mfgpll4_ck */

/* SPM MTCMOS */
#define SPM_POWERON_CONFIG_EN   0x0000u
#define SPM_PROJECT_CODE        (0x0B16u << 16)
#define SPM_MFG0_PWR_CON        0x0E80u
#define SPM_XPU_PWR_STATUS      0x0EF8u
#define SPM_XPU_PWR_STATUS_2ND  0x0EFCu

#define PWR_RST_B               (1u << 0)
#define PWR_ISO                 (1u << 1)
#define PWR_ON                  (1u << 2)
#define PWR_ON_2ND              (1u << 3)
#define PWR_CLK_DIS             (1u << 4)
#define MFG_SRAM_PDN            (1u << 8)
#define MFG_SRAM_PDN_ACK        (1u << 12)

/* INFRACFG_AO Bus Protect for MFG1 */
#define INFRA_PROT_STA1         0x0228u
#define INFRA_PROT_SET          0x02A0u
#define INFRA_PROT_CLR          0x02A4u
#define INFRA_PROT2_SET         0x0714u
#define INFRA_PROT2_CLR         0x0718u
#define INFRA_PROT2_STA1        0x0724u
#define MFG1_PROT_STEP1_MASK    ((1u << 5) | (1u << 6))
#define MFG1_PROT_STEP2_MASK    ((1u << 21) | (1u << 22))

/* GPU_PLL_CTRL (0x13FA0000) MFGPLL1 */
#define MFGPLL1_CON0            0x0008u
#define MFGPLL1_CON1            0x000Cu
#define MFGPLL1_CON3            0x0014u
#define PLL_BASE_EN             (1u << 0)
#define PLL_PWR_ON              (1u << 0)
#define PLL_ISO_EN              (1u << 1)
#define PLL_PCW_CHG             (1u << 31)

/* MFGCFG (0x13FBF000) BG3D clock gate */
#define MFG_CG_CON              0x0000u
#define MFG_CG_SET              0x0004u
#define MFG_CG_CLR              0x0008u

/* Mali-G68 (Valhall v9 Job Manager) registers at 0x13000000 */
#define MALI_GPU_ID             0x0000u
#define MALI_L2_FEATURES        0x0004u
#define MALI_CORE_FEATURES      0x0008u
#define MALI_TILER_FEATURES     0x000Cu
#define MALI_MEM_FEATURES       0x0010u
#define MALI_MMU_FEATURES       0x0014u
#define MALI_AS_PRESENT         0x0018u
#define MALI_JS_PRESENT         0x001Cu
#define MALI_GPU_IRQ_RAWSTAT    0x0020u
#define MALI_GPU_IRQ_CLEAR      0x0024u
#define MALI_GPU_IRQ_MASK       0x0028u
#define MALI_GPU_IRQ_STAT       0x002Cu
#define MALI_GPU_COMMAND        0x0030u
#define MALI_GPU_STATUS         0x0034u
#define MALI_AFBC_FEATURES      0x004Cu
#define MALI_THREAD_MAX_THREADS 0x00A0u
#define MALI_THREAD_MAX_WG      0x00A4u
#define MALI_THREAD_MAX_BARRIER 0x00A8u
#define MALI_THREAD_FEATURES    0x00ACu
#define MALI_TEX_FEATURES_0     0x00B0u
#define MALI_TEX_FEATURES_1     0x00B4u
#define MALI_TEX_FEATURES_2     0x00B8u
#define MALI_JS_FEATURES_0      0x00C0u
#define MALI_SHADER_PRESENT_LO  0x0100u
#define MALI_SHADER_PRESENT_HI  0x0104u
#define MALI_TILER_PRESENT_LO   0x0110u
#define MALI_L2_PRESENT_LO      0x0120u
#define MALI_SHADER_READY_LO    0x0140u
#define MALI_TILER_READY_LO     0x0150u
#define MALI_L2_READY_LO        0x0160u
#define MALI_SHADER_PWRON_LO    0x0180u
#define MALI_TILER_PWRON_LO     0x0190u
#define MALI_L2_PWRON_LO        0x01A0u
#define MALI_COHERENCY_FEATURES 0x0300u
#define MALI_COHERENCY_ENABLE   0x0304u
#define MALI_THREAD_TLS_ALLOC   0x0310u
#define MALI_STACK_PRESENT_LO   0x0E00u

#define MALI_CMD_SOFT_RESET         1u
#define MALI_CMD_CLEAN_CACHES       7u
#define MALI_CMD_CLEAN_INV_CACHES   8u
#define MALI_IRQ_RESET_DONE         (1u << 8)
#define MALI_IRQ_CLEAN_CACHES_DONE  (1u << 17)

/* Job Control & Job Slots */
#define MALI_JOB_IRQ_RAWSTAT    0x1000u
#define MALI_JOB_IRQ_CLEAR      0x1004u
#define MALI_JOB_IRQ_MASK       0x1008u
#define MALI_JOB_IRQ_STAT       0x100Cu

#define MALI_JS0                0x1800u  /* fragment jobs */
#define MALI_JS1                0x1880u  /* vertex / tiler / compute / write-value */
#define MALI_JS2                0x1900u  /* compute-only */
#define JS_STATUS               0x0024u
#define JS_HEAD_NEXT_LO         0x0040u
#define JS_HEAD_NEXT_HI         0x0044u
#define JS_AFFINITY_NEXT_LO     0x0050u
#define JS_AFFINITY_NEXT_HI     0x0054u
#define JS_CONFIG_NEXT          0x0058u
#define JS_COMMAND_NEXT         0x0060u
#define JS_CMD_START            1u
/* AS 0, start clean+inv L2, end clean+inv L2, priority 8 */
#define JS_CONFIG_DEFAULT       ((3u << 8) | (3u << 12) | (8u << 16))

/* MMU Control & Address Space 0 */
#define MALI_MMU_IRQ_RAWSTAT    0x2000u
#define MALI_MMU_IRQ_CLEAR      0x2004u
#define MALI_MMU_IRQ_MASK       0x2008u
#define MALI_MMU_IRQ_STAT       0x200Cu

#define MALI_AS0                0x2400u
#define AS_TRANSTAB_LO          0x0000u
#define AS_TRANSTAB_HI          0x0004u
#define AS_MEMATTR_LO           0x0008u
#define AS_MEMATTR_HI           0x000Cu
#define AS_LOCKADDR_LO          0x0010u
#define AS_LOCKADDR_HI          0x0014u
#define AS_COMMAND              0x0018u
#define AS_FAULTSTATUS          0x001Cu
#define AS_FAULTADDRESS_LO      0x0020u
#define AS_FAULTADDRESS_HI      0x0024u
#define AS_STATUS               0x0028u
#define AS_TRANSCFG_LO          0x0030u
#define AS_TRANSCFG_HI          0x0034u

#define AS_CMD_UPDATE           1u
#define AS_CMD_LOCK             2u
#define AS_CMD_UNLOCK           3u
#define AS_CMD_FLUSH_PT         4u
#define AS_CMD_FLUSH_MEM        5u

#define AS_TRANSCFG_ADRMODE_UNMAPPED    1u
#define AS_TRANSCFG_ADRMODE_IDENTITY    2u
#define AS_TRANSCFG_ADRMODE_AARCH64_4K  6u
#define AS_TRANSCFG_INA_BITS(x)         (((uint32_t)(x) & 0xFFu) << 6)
#define AS_TRANSCFG_PTW_MEMATTR_NC      (1u << 24)
#define AS_TRANSCFG_PTW_MEMATTR_WB      (2u << 24)
#define AS_TRANSCFG_PTW_RA              (1u << 30)
/* AArch64 MEMATTR: inner/outer WB (2<<6), Midgard inner shareable (0<<4), impl alloc (2<<2) = 0x88 */
#define AS_MEMATTR_WB_ALL               0x88888888u

/* AArch64 Stage-1 4 KiB page table descriptors */
#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_AF                  (1ull << 10)
#define PTE_SH_INNER            (3ull << 8)
#define PTE_AP_RW               (1ull << 6)
#define PTE_ADDRESS             0x0000FFFFFFFFF000ull

/* GPU VA allocator: [16 MiB .. 4 GiB) in 64 KiB granules (+1 guard granule per buffer) */
#define GPU_ADDRESS_FIRST       0x01000000ull
#define GPU_ADDRESS_END         (1ull << 32)
#define GPU_GRANULE_SHIFT       16u
#define GPU_GRANULES            ((GPU_ADDRESS_END - GPU_ADDRESS_FIRST) >> GPU_GRANULE_SHIFT)
#define FIRST_JOB_GPU           0x00100000ull
#define MALI_RECENT             1024
#define MALI_QUEUE              128

/* Locking hierarchy: mali_job_lock -> mali_lock (never reversed) */
static LCK_GRP_DECLARE(mali_lock_group, "hakios-mali");
static LCK_MTX_DECLARE(mali_lock, &mali_lock_group);
static LCK_MTX_DECLARE(mali_job_lock, &mali_lock_group);
static LCK_MTX_DECLARE(mali_wait_lock, &mali_lock_group);
static LCK_MTX_DECLARE(mali_queue_lock, &mali_lock_group);

static int mali_wait_event;
static int mali_gpu_event;

struct mali_selftest_res {
	bool ok;
	uint32_t word;
	uint32_t rawstat;
	uint32_t js_status;
	uint32_t job_hdr_status;
	uint32_t mmu_fault;
	uint64_t mmu_fault_addr;
};

static struct {
	bool started;
	int stage;
	const char *state;
	uint32_t target_mhz;
	uint32_t fastmap;
	uint32_t walk_wb;
	uint32_t want_irq;
	uint32_t spin_us;
	uint32_t nap_us;

	volatile uint32_t *topckgen;
	volatile uint32_t *infracfg;
	volatile uint32_t *spm;
	volatile uint32_t *pmif;
	volatile uint32_t *gpu_pll;
	volatile uint32_t *mfgcfg;
	volatile uint32_t *mali;

	/* PMIF / MT6359P telemetry */
	uint32_t pmif_done;
	uint16_t hwcid, swcid;
	uint16_t vsram_con0_b, vsram_mon_b, vsram_elr_b, vsram_da_b;
	uint16_t vsram_mon_a, vsram_da_a;
	uint16_t vgpu_con0_b, vgpu_dbg1_b, vgpu_elr_b, vgpu_da_b;
	uint16_t vgpu_dbg1_a, vgpu_da_a;

	/* SPM / INFRACFG telemetry */
	uint32_t spm_cfg_b;
	uint32_t xpu_b, xpu2_b, xpu_a, xpu2_a;
	uint32_t mfg_ok_mask;
	uint32_t prot1_a, prot2_a;

	/* Clock / PLL / CG telemetry */
	uint32_t clk20_b, clk20_a;
	uint32_t pll_con0_b, pll_con1_b, pll_con3_b;
	uint32_t pll_con0_a, pll_con1_a, pll_con3_a;
	uint32_t cg_b, cg_a;

	/* Mali hardware telemetry */
	uint32_t gpu_id;
	uint32_t l2_feat, core_feat, tiler_feat, mem_feat, mmu_feat;
	uint32_t as_present, js_present;
	uint32_t max_threads, max_wg, thread_feat;
	uint32_t tex0, tex1, tex2;
	uint32_t afbc_feat, coh_feat, coh_en, tls_alloc, stack_present;
	uint32_t shader_present_lo, shader_present_hi;
	uint32_t tiler_present_lo, l2_present_lo;
	uint32_t shader_ready_lo, tiler_ready_lo, l2_ready_lo;
	uint32_t reset_ok;

	/* MMU config */
	uint32_t va_bits;
	uint32_t pa_bits;
	uint32_t pt_levels;
	uint32_t transcfg_active;

	/* IRQ telemetry */
	volatile uint32_t gpu_irq_ok, mmu_irq_ok, job_irq_ok;
	volatile uint64_t gpu_irq_cnt, mmu_irq_cnt, job_irq_cnt;
	volatile uint64_t gpu_irq_spurious, mmu_irq_spurious, job_irq_spurious;
	uint32_t gpu_irq_test_ok;

	/* Self-test results */
	struct mali_selftest_res st_id;
	struct mali_selftest_res st_mmu;
	bool st_api_ok;
	uint32_t st_api_word;
	uint64_t st_api_seq;
	uint64_t st_api_va;

	/* Runtime stats & last failure */
	uint64_t submits_total;
	uint64_t submits_failed;
	uint32_t last_fail_slot;
	uint32_t last_fail_raw;
	uint32_t last_fail_status;
	uint32_t last_fail_mmu;
	uint32_t last_fail_fault;
	uint64_t last_fail_addr;
	uint64_t last_fail_chain;
	uint64_t last_fail_walk[4];
} g_mali = {
	.state = "not_started",
	.target_mhz = 450,
	.fastmap = 1,
	.walk_wb = 0,
	.want_irq = 1,
	.spin_us = 200,
	.nap_us = 200,
};

/* Page table root & VA allocator state */
static uint64_t *pt_top;
static uint64_t pt_top_phys;
static uint64_t gpu_granule_used[(GPU_GRANULES + 63) / 64];
static uint64_t gpu_granule_next;
static uint64_t gpu_granules_taken;

static struct {
	uint64_t address, size;
	uint32_t freed, kind;
	uint32_t width, height, row_bytes, format, offset;
	int made_pid, freed_pid;
	char made_name[17], freed_name[17];
	uint64_t freed_at;
} mali_recent[MALI_RECENT];
static uint32_t mali_recent_next;

/* Job worker queue */
static struct {
	uint64_t chain, seq, queued;
	bool fragment;
	int pid;
	char name[17];
} mali_queue[MALI_QUEUE];
static unsigned mali_queue_head, mali_queue_count;
static uint64_t mali_queued_seq, mali_done_seq;
static bool mali_worker_started;

static inline uint32_t
mmio_r32(volatile uint32_t *base, uint32_t ofs)
{
	uint32_t v = base[ofs >> 2];
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mmio_w32(volatile uint32_t *base, uint32_t ofs, uint32_t val)
{
	__asm__ volatile("dsb st" ::: "memory");
	base[ofs >> 2] = val;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
mali_udelay(uint32_t us)
{
	delay(us);
}

static bool
wait_reg(volatile uint32_t *base, uint32_t ofs, uint32_t mask, uint32_t want, uint32_t max_ms)
{
	for (uint32_t i = 0; i < max_ms * 100u; i++) {
		if ((mmio_r32(base, ofs) & mask) == want) {
			return true;
		}
		mali_udelay(10);
	}
	return (mmio_r32(base, ofs) & mask) == want;
}

/* =========================================================================
 * Section 1: PMIF_SPI (MT6359P VSRAM_GPU / VGPU rails)
 * ========================================================================= */

static bool
pmif_wait_fsm(uint32_t want)
{
	for (uint32_t i = 0; i < 10000; i++) {
		uint32_t sta = mmio_r32(g_mali.pmif, PMIF_SWINF_2_STA);
		uint32_t fsm = (sta >> 1) & 0x7u;
		if (fsm == want) {
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static bool
pmic_read16(uint16_t addr, uint16_t *out)
{
	if (!pmif_wait_fsm(PMIF_FSM_IDLE)) {
		return false;
	}
	/* Read command: write=0, bytecnt=1 (2 bytes -> (1<<16)), addr[15:0] */
	uint32_t cmd = (1u << 16) | (uint32_t)addr;
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_ACC, cmd);

	if (!pmif_wait_fsm(PMIF_FSM_WFVLDCLR)) {
		return false;
	}
	uint32_t rdata = mmio_r32(g_mali.pmif, PMIF_SWINF_2_RDATA);
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_VLD_CLR, 1u);
	*out = (uint16_t)(rdata & 0xFFFFu);
	return true;
}

static bool
pmic_write16(uint16_t addr, uint16_t val)
{
	if (!pmif_wait_fsm(PMIF_FSM_IDLE)) {
		return false;
	}
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_WDATA, (uint32_t)val);
	/* Write command: write=1 (1<<29), bytecnt=1 (2 bytes -> (1<<16)), addr[15:0] */
	uint32_t cmd = (1u << 29) | (1u << 16) | (uint32_t)addr;
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_ACC, cmd);

	return pmif_wait_fsm(PMIF_FSM_IDLE);
}

static bool
pmic_update16(uint16_t addr, uint16_t mask, uint16_t val)
{
	uint16_t cur = 0;
	if (!pmic_read16(addr, &cur)) {
		return false;
	}
	uint16_t next = (uint16_t)((cur & ~mask) | (val & mask));
	if (next == cur) {
		return true;
	}
	return pmic_write16(addr, next);
}

static bool
mt6877_pmic_power_on(void)
{
	g_mali.pmif_done = mmio_r32(g_mali.pmif, PMIF_INIT_DONE);
	if (!(g_mali.pmif_done & 1u)) {
		g_mali.state = "pmif_not_init";
		return false;
	}

	if (!pmic_read16(MT6359_HWCID, &g_mali.hwcid) ||
	    !pmic_read16(MT6359_SWCID, &g_mali.swcid)) {
		g_mali.state = "pmic_id_fail";
		return false;
	}
	if ((g_mali.swcid >> 8) != 0x59u) {
		g_mali.state = "pmic_not_mt6359";
		return false;
	}

	/* Snapshot before */
	(void)pmic_read16(MT6359_VSRAM_MD_CON0,   &g_mali.vsram_con0_b);
	(void)pmic_read16(MT6359_VSRAM_MD_MON,    &g_mali.vsram_mon_b);
	(void)pmic_read16(MT6359_VSRAM_MD_ELR,    &g_mali.vsram_elr_b);
	(void)pmic_read16(MT6359_VSRAM_MD_VOSEL1, &g_mali.vsram_da_b);

	(void)pmic_read16(MT6359_VPROC2_CON0, &g_mali.vgpu_con0_b);
	(void)pmic_read16(MT6359_VPROC2_DBG1, &g_mali.vgpu_dbg1_b);
	(void)pmic_read16(MT6359_VPROC2_ELR0, &g_mali.vgpu_elr_b);
	(void)pmic_read16(MT6359_VPROC2_DBG0, &g_mali.vgpu_da_b);

	/* Ensure VSRAM_GPU (ldo_vsram_md) is on & >= 0.75 V before VGPU */
	uint16_t cur_vsram_sel = (uint16_t)((g_mali.vsram_da_b >> 8) & 0x7Fu);
	if (!(g_mali.vsram_mon_b & 1u) || cur_vsram_sel < VSRAM_TARGET_SEL) {
		if (!pmic_update16(MT6359_VSRAM_MD_ELR, 0x007Fu, VSRAM_TARGET_SEL)) {
			g_mali.state = "vsram_vosel_fail";
			return false;
		}
		if (!pmic_update16(MT6359_VSRAM_MD_CON0, 0x0001u, 0x0001u)) {
			g_mali.state = "vsram_en_fail";
			return false;
		}
		mali_udelay(300);
	}

	/* Ensure VGPU (buck_vproc2) is on & >= 0.70 V (0 <= VSRAM - VGPU <= 200 mV) */
	uint16_t cur_vgpu_sel = (uint16_t)(g_mali.vgpu_da_b & 0x7Fu);
	if (!(g_mali.vgpu_dbg1_b & 1u) || cur_vgpu_sel < VGPU_TARGET_SEL) {
		if (!pmic_update16(MT6359_VPROC2_ELR0, 0x007Fu, VGPU_TARGET_SEL)) {
			g_mali.state = "vgpu_vosel_fail";
			return false;
		}
		if (!pmic_update16(MT6359_VPROC2_CON0, 0x0001u, 0x0001u)) {
			g_mali.state = "vgpu_en_fail";
			return false;
		}
		mali_udelay(300);
	}

	/* Snapshot after */
	(void)pmic_read16(MT6359_VSRAM_MD_MON,    &g_mali.vsram_mon_a);
	(void)pmic_read16(MT6359_VSRAM_MD_VOSEL1, &g_mali.vsram_da_a);
	(void)pmic_read16(MT6359_VPROC2_DBG1,     &g_mali.vgpu_dbg1_a);
	(void)pmic_read16(MT6359_VPROC2_DBG0,     &g_mali.vgpu_da_a);

	if (!(g_mali.vsram_mon_a & 1u) || !(g_mali.vgpu_dbg1_a & 1u)) {
		g_mali.state = "pmic_rail_off";
		return false;
	}
	return true;
}

/* =========================================================================
 * Section 2: SPM MTCMOS (MFG0..5), Bus Protect, MFGPLL1 & BG3D
 * ========================================================================= */

static bool
spm_wait_xpu_on(uint32_t bit_mask)
{
	for (uint32_t i = 0; i < 10000; i++) {
		uint32_t s1 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
		uint32_t s2 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
		if ((s1 & bit_mask) == bit_mask && (s2 & bit_mask) == bit_mask) {
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static bool
spm_mfg_power_on_one(uint32_t idx)
{
	uint32_t reg = SPM_MFG0_PWR_CON + idx * 4u;
	uint32_t bit = 1u << idx;

	uint32_t s1 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	uint32_t s2 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
	uint32_t con = mmio_r32(g_mali.spm, reg);

	/* Already cleanly powered on and SRAM un-pdn'd? */
	if ((s1 & bit) && (s2 & bit) &&
	    !(con & PWR_ISO) && !(con & PWR_CLK_DIS) && (con & PWR_RST_B) &&
	    !(con & MFG_SRAM_PDN) && !(con & MFG_SRAM_PDN_ACK)) {
		g_mali.mfg_ok_mask |= bit;
		return true;
	}

	con |= PWR_ON;
	mmio_w32(g_mali.spm, reg, con);
	con |= PWR_ON_2ND;
	mmio_w32(g_mali.spm, reg, con);

	if (!spm_wait_xpu_on(bit)) {
		return false;
	}

	con = mmio_r32(g_mali.spm, reg);
	con &= ~PWR_CLK_DIS;
	mmio_w32(g_mali.spm, reg, con);

	con &= ~PWR_ISO;
	mmio_w32(g_mali.spm, reg, con);

	con |= PWR_RST_B;
	mmio_w32(g_mali.spm, reg, con);

	con &= ~MFG_SRAM_PDN;
	mmio_w32(g_mali.spm, reg, con);

	for (uint32_t i = 0; i < 10000; i++) {
		if (!(mmio_r32(g_mali.spm, reg) & MFG_SRAM_PDN_ACK)) {
			g_mali.mfg_ok_mask |= bit;
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static bool
infra_clear_bus_prot(uint32_t clr_reg, uint32_t sta_reg, uint32_t mask)
{
	mmio_w32(g_mali.infracfg, clr_reg, mask);
	for (uint32_t i = 0; i < 10000; i++) {
		if ((mmio_r32(g_mali.infracfg, sta_reg) & mask) == 0u) {
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static uint32_t
mt6877_freq_to_con1(uint32_t mhz)
{
	/* Clamp to safe range [300..850] MHz; Stage 1/2 default is 450 MHz */
	if (mhz < 300u) mhz = 300u;
	if (mhz > 850u) mhz = 850u;

	uint32_t posdiv_bits;
	uint32_t posdiv_mul;
	if (mhz >= 750u) {
		posdiv_bits = 1u; /* /2 -> VCO 1500..1700 MHz */
		posdiv_mul  = 2u;
	} else {
		posdiv_bits = 2u; /* /4 -> VCO 1200..3000 MHz */
		posdiv_mul  = 4u;
	}
	/* pcw (22-bit: 8 int + 14 frac) = (mhz * posdiv_mul * 16384) / 26 */
	uint64_t num = (uint64_t)mhz * (uint64_t)posdiv_mul * 16384ULL + 13ULL;
	uint32_t pcw = (uint32_t)(num / 26ULL) & 0x3FFFFFu;
	return PLL_PCW_CHG | (posdiv_bits << 24) | pcw;
}

static bool
mt6877_mfg_power_and_clk(void)
{
	/* Step A: Unlock SPM register writes */
	g_mali.spm_cfg_b = mmio_r32(g_mali.spm, SPM_POWERON_CONFIG_EN);
	mmio_w32(g_mali.spm, SPM_POWERON_CONFIG_EN, SPM_PROJECT_CODE | 1u);

	g_mali.xpu_b  = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	g_mali.xpu2_b = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);

	/* Step B: Park MFG clock on 26 MHz before changing power domains or PLL */
	g_mali.clk20_b = mmio_r32(g_mali.topckgen, TOP_CLK_CFG_20);
	mmio_w32(g_mali.topckgen, TOP_CLK_CFG_20_CLR, TOP_MFG_INT2_SEL | TOP_MFG_INT1_SEL);
	mali_udelay(10);

	/* Step C: Power on MFG0 -> MFG1 -> release Bus Protect (Step 2 then Step 1) -> MFG2..5 */
	if (!spm_mfg_power_on_one(0)) {
		g_mali.state = "spm_mfg0_fail";
		goto out_snap;
	}
	if (!spm_mfg_power_on_one(1)) {
		g_mali.state = "spm_mfg1_fail";
		goto out_snap;
	}
	if (!infra_clear_bus_prot(INFRA_PROT_CLR, INFRA_PROT_STA1, MFG1_PROT_STEP2_MASK)) {
		g_mali.state = "prot_step2_fail";
		goto out_snap;
	}
	if (!infra_clear_bus_prot(INFRA_PROT2_CLR, INFRA_PROT2_STA1, MFG1_PROT_STEP1_MASK)) {
		g_mali.state = "prot_step1_fail";
		goto out_snap;
	}
	for (uint32_t i = 2; i <= 5; i++) {
		if (!spm_mfg_power_on_one(i)) {
			g_mali.state = "spm_mfg2_5_fail";
			goto out_snap;
		}
	}

	g_mali.xpu_a  = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	g_mali.xpu2_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
	g_mali.prot1_a = mmio_r32(g_mali.infracfg, INFRA_PROT2_STA1);
	g_mali.prot2_a = mmio_r32(g_mali.infracfg, INFRA_PROT_STA1);
	g_mali.stage = 1;

	/* Step D: Enable MFGPLL1 at target_mhz (0x13FA0000 is inside MFG power domain) */
	g_mali.pll_con0_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON0);
	g_mali.pll_con1_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON1);
	g_mali.pll_con3_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON3);

	uint32_t con3 = g_mali.pll_con3_b;
	if (!(con3 & PLL_PWR_ON)) {
		con3 |= PLL_PWR_ON;
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON3, con3);
		mali_udelay(5);
	}
	if (con3 & PLL_ISO_EN) {
		con3 &= ~PLL_ISO_EN;
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON3, con3);
		mali_udelay(5);
	}

	uint32_t want_con1 = mt6877_freq_to_con1(g_mali.target_mhz);
	mmio_w32(g_mali.gpu_pll, MFGPLL1_CON1, want_con1);

	uint32_t con0 = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON0);
	if (!(con0 & PLL_BASE_EN)) {
		con0 |= PLL_BASE_EN;
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON0, con0);
	}
	mali_udelay(50);

	/* Switch MFG mux to MFGPLL1 (bit 18 = 1) */
	mmio_w32(g_mali.topckgen, TOP_CLK_CFG_20_SET, TOP_MFG_INT2_SEL);
	mali_udelay(10);

	g_mali.clk20_a    = mmio_r32(g_mali.topckgen, TOP_CLK_CFG_20);
	g_mali.pll_con0_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON0);
	g_mali.pll_con1_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON1);
	g_mali.pll_con3_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON3);

	/* Step E: Ungate BG3D clock in MFGCFG (0x13FBF000) */
	g_mali.cg_b = mmio_r32(g_mali.mfgcfg, MFG_CG_CON);
	if (g_mali.cg_b & 1u) {
		mmio_w32(g_mali.mfgcfg, MFG_CG_CLR, 1u);
		mali_udelay(5);
	}
	g_mali.cg_a = mmio_r32(g_mali.mfgcfg, MFG_CG_CON);
	return true;

out_snap:
	g_mali.xpu_a  = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	g_mali.xpu2_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
	g_mali.prot1_a = mmio_r32(g_mali.infracfg, INFRA_PROT2_STA1);
	g_mali.prot2_a = mmio_r32(g_mali.infracfg, INFRA_PROT_STA1);
	return false;
}

/* =========================================================================
 * Section 3: Mali-G68 Probe, Soft-Reset & Core Power-On
 * ========================================================================= */

static bool
mali_hw_reset_and_pwron(void)
{
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, 0xFFFFFFFFu);

	mmio_w32(g_mali.mali, MALI_GPU_COMMAND, MALI_CMD_SOFT_RESET);
	bool reset_ok = wait_reg(g_mali.mali, MALI_GPU_IRQ_RAWSTAT,
	    MALI_IRQ_RESET_DONE, MALI_IRQ_RESET_DONE, 100);
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);
	if (!reset_ok) {
		return false;
	}

	mmio_w32(g_mali.mali, MALI_L2_PWRON_LO, g_mali.l2_present_lo);
	mmio_w32(g_mali.mali, MALI_TILER_PWRON_LO, g_mali.tiler_present_lo);
	mmio_w32(g_mali.mali, MALI_SHADER_PWRON_LO, g_mali.shader_present_lo);

	for (uint32_t i = 0; i < 50000; i++) {
		g_mali.l2_ready_lo     = mmio_r32(g_mali.mali, MALI_L2_READY_LO);
		g_mali.tiler_ready_lo  = mmio_r32(g_mali.mali, MALI_TILER_READY_LO);
		g_mali.shader_ready_lo = mmio_r32(g_mali.mali, MALI_SHADER_READY_LO);
		if (g_mali.l2_ready_lo == g_mali.l2_present_lo &&
		    g_mali.tiler_ready_lo == g_mali.tiler_present_lo &&
		    g_mali.shader_ready_lo == g_mali.shader_present_lo) {
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static bool
mt6877_mali_probe_and_reset(void)
{
	g_mali.gpu_id = mmio_r32(g_mali.mali, MALI_GPU_ID);
	if (g_mali.gpu_id == 0u || g_mali.gpu_id == 0xFFFFFFFFu) {
		g_mali.state = "mali_id_invalid";
		return false;
	}

	g_mali.l2_feat     = mmio_r32(g_mali.mali, MALI_L2_FEATURES);
	g_mali.core_feat   = mmio_r32(g_mali.mali, MALI_CORE_FEATURES);
	g_mali.tiler_feat  = mmio_r32(g_mali.mali, MALI_TILER_FEATURES);
	g_mali.mem_feat    = mmio_r32(g_mali.mali, MALI_MEM_FEATURES);
	g_mali.mmu_feat    = mmio_r32(g_mali.mali, MALI_MMU_FEATURES);
	g_mali.as_present  = mmio_r32(g_mali.mali, MALI_AS_PRESENT);
	g_mali.js_present  = mmio_r32(g_mali.mali, MALI_JS_PRESENT);
	g_mali.max_threads = mmio_r32(g_mali.mali, MALI_THREAD_MAX_THREADS);
	g_mali.max_wg      = mmio_r32(g_mali.mali, MALI_THREAD_MAX_WG);
	g_mali.thread_feat = mmio_r32(g_mali.mali, MALI_THREAD_FEATURES);
	g_mali.tex0        = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_0);
	g_mali.tex1        = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_1);
	g_mali.tex2        = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_2);
	g_mali.afbc_feat   = mmio_r32(g_mali.mali, MALI_AFBC_FEATURES);
	g_mali.coh_feat    = mmio_r32(g_mali.mali, MALI_COHERENCY_FEATURES);
	g_mali.coh_en      = mmio_r32(g_mali.mali, MALI_COHERENCY_ENABLE);
	g_mali.tls_alloc   = mmio_r32(g_mali.mali, MALI_THREAD_TLS_ALLOC);
	g_mali.stack_present = mmio_r32(g_mali.mali, MALI_STACK_PRESENT_LO);

	g_mali.shader_present_lo = mmio_r32(g_mali.mali, MALI_SHADER_PRESENT_LO);
	g_mali.shader_present_hi = mmio_r32(g_mali.mali, MALI_SHADER_PRESENT_HI);
	g_mali.tiler_present_lo  = mmio_r32(g_mali.mali, MALI_TILER_PRESENT_LO);
	g_mali.l2_present_lo     = mmio_r32(g_mali.mali, MALI_L2_PRESENT_LO);

	g_mali.va_bits   = g_mali.mmu_feat & 0xFFu;
	g_mali.pa_bits   = (g_mali.mmu_feat >> 8) & 0xFFu;
	g_mali.pt_levels = (g_mali.va_bits > 39u) ? 4u : 3u;

	if (!mali_hw_reset_and_pwron()) {
		g_mali.reset_ok = 0;
		g_mali.state = "soft_reset_or_pwron_fail";
		return false;
	}
	g_mali.reset_ok = 1;
	g_mali.stage = 2;
	g_mali.state = "mali_ready";
	return true;
}

/* =========================================================================
 * Section 4: GICv3 SPI Interrupts (485 GPU, 486 MMU, 487 JOB)
 * ========================================================================= */

static void
mali_gpu_irq_handler(__unused void *ctx, uint32_t intid)
{
	uint32_t stat = mmio_r32(g_mali.mali, MALI_GPU_IRQ_STAT);
	if (stat == 0u) {
		if (++g_mali.gpu_irq_spurious > MALI_IRQ_SPURIOUS_LIMIT) {
			mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, 0u);
			gic_v3_spi_mask(intid);
			g_mali.gpu_irq_ok = 0;
		}
		return;
	}
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, stat);
	g_mali.gpu_irq_cnt++;
	wakeup((caddr_t)&mali_gpu_event);
}

static void
mali_mmu_irq_handler(__unused void *ctx, uint32_t intid)
{
	uint32_t stat = mmio_r32(g_mali.mali, MALI_MMU_IRQ_STAT);
	if (stat == 0u) {
		if (++g_mali.mmu_irq_spurious > MALI_IRQ_SPURIOUS_LIMIT) {
			mmio_w32(g_mali.mali, MALI_MMU_IRQ_MASK, 0u);
			gic_v3_spi_mask(intid);
			g_mali.mmu_irq_ok = 0;
		}
		return;
	}
	g_mali.last_fail_mmu   = stat;
	g_mali.last_fail_fault = mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTSTATUS);
	g_mali.last_fail_addr  = (uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_LO) |
	    ((uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_HI) << 32);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_CLEAR, stat);
	g_mali.mmu_irq_cnt++;
}

static void
mali_job_irq_handler(__unused void *ctx, uint32_t intid)
{
	uint32_t stat = mmio_r32(g_mali.mali, MALI_JOB_IRQ_STAT);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	if (stat == 0u) {
		if (++g_mali.job_irq_spurious > MALI_IRQ_SPURIOUS_LIMIT) {
			gic_v3_spi_mask(intid);
			g_mali.job_irq_ok = 0;
		}
		return;
	}
	g_mali.job_irq_cnt++;
	wakeup((caddr_t)&mali_wait_event);
}

static void
mt6877_mali_attach_irqs(void)
{
	if (!g_mali.want_irq) {
		g_mali.stage = 3;
		return;
	}

	mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, 0xFFFFFFFFu);

	g_mali.gpu_irq_ok = (gic_v3_spi_attach(MALI_IRQ_GPU, false, mali_gpu_irq_handler, NULL) == KERN_SUCCESS);
	g_mali.mmu_irq_ok = (gic_v3_spi_attach(MALI_IRQ_MMU, false, mali_mmu_irq_handler, NULL) == KERN_SUCCESS);
	g_mali.job_irq_ok = (gic_v3_spi_attach(MALI_IRQ_JOB, false, mali_job_irq_handler, NULL) == KERN_SUCCESS);

	/* Give spurious-line guard 10 ms to detect any inverted polarity */
	mali_udelay(10000);

	/* Verify GPU IRQ path with CLEAN_INV_CACHES */
	if (g_mali.gpu_irq_ok) {
		uint64_t before = g_mali.gpu_irq_cnt;
		mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, MALI_IRQ_CLEAN_CACHES_DONE);
		mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, MALI_IRQ_CLEAN_CACHES_DONE);
		mmio_w32(g_mali.mali, MALI_GPU_COMMAND, MALI_CMD_CLEAN_INV_CACHES);
		for (uint32_t i = 0; i < 5000; i++) {
			if (g_mali.gpu_irq_cnt > before) {
				g_mali.gpu_irq_test_ok = 1;
				break;
			}
			mali_udelay(10);
		}
		mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, 0u);
		mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);
	}

	g_mali.stage = 3;
}

/* =========================================================================
 * Section 5: AArch64_4K MMU (4-level for 48-bit VA) & 64 KiB VA Allocator
 * ========================================================================= */

static uint32_t
mali_transcfg_aarch64(void)
{
	uint32_t va = (g_mali.va_bits >= 32u && g_mali.va_bits <= 48u) ? g_mali.va_bits : 48u;
	uint32_t ina = 55u - va;
	uint32_t cfg = AS_TRANSCFG_ADRMODE_AARCH64_4K | AS_TRANSCFG_INA_BITS(ina);
	if (g_mali.fastmap && !g_mali.walk_wb) {
		cfg |= AS_TRANSCFG_PTW_MEMATTR_NC;
	} else {
		cfg |= AS_TRANSCFG_PTW_MEMATTR_WB | AS_TRANSCFG_PTW_RA;
	}
	return cfg;
}

static void
as0_update(uint64_t transtab, uint32_t transcfg)
{
	mmio_w32(g_mali.mali, MALI_AS0 + AS_TRANSTAB_LO, (uint32_t)transtab);
	mmio_w32(g_mali.mali, MALI_AS0 + AS_TRANSTAB_HI, (uint32_t)(transtab >> 32));
	mmio_w32(g_mali.mali, MALI_AS0 + AS_TRANSCFG_LO, transcfg);
	mmio_w32(g_mali.mali, MALI_AS0 + AS_TRANSCFG_HI, 0u);
	mmio_w32(g_mali.mali, MALI_AS0 + AS_MEMATTR_LO, AS_MEMATTR_WB_ALL);
	mmio_w32(g_mali.mali, MALI_AS0 + AS_MEMATTR_HI, AS_MEMATTR_WB_ALL);
	mmio_w32(g_mali.mali, MALI_AS0 + AS_COMMAND, AS_CMD_UPDATE);
	(void)wait_reg(g_mali.mali, MALI_AS0 + AS_STATUS, 1u, 0u, 50);
	g_mali.transcfg_active = transcfg;
}

static uint64_t *
pt_page(uint64_t *phys_out)
{
	vm_offset_t page = 0;
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return NULL;
	}
	*phys_out = kvtophys(page);
	FlushPoC_DcacheRegion(page, PAGE_SIZE);
	return (uint64_t *)page;
}

static uint64_t *
pt_below(uint64_t *entry)
{
	if (*entry & 1ull) {
		return (uint64_t *)phystokv(*entry & PTE_ADDRESS);
	}
	uint64_t phys = 0;
	uint64_t *table = pt_page(&phys);
	if (table == NULL) {
		return NULL;
	}
	*entry = (phys & PTE_ADDRESS) | PTE_TABLE;
	FlushPoC_DcacheRegion((vm_offset_t)entry, sizeof(*entry));
	return table;
}

/* Map one 4 KiB page at GPU VA `gpu` -> physical address `phys` (caller holds mali_lock or during init). */
static bool
map_page_locked(uint64_t gpu, uint64_t phys)
{
	if (pt_top == NULL && (pt_top = pt_page(&pt_top_phys)) == NULL) {
		return false;
	}
	uint64_t *tbl = pt_top;
	if (g_mali.pt_levels == 4u) {
		tbl = pt_below(&tbl[(gpu >> 39) & 0x1FFu]);
		if (tbl == NULL) return false;
		tbl = pt_below(&tbl[(gpu >> 30) & 0x1FFu]);
		if (tbl == NULL) return false;
	} else {
		tbl = pt_below(&tbl[(gpu >> 30) & 0x1Fu]);
		if (tbl == NULL) return false;
	}
	tbl = pt_below(&tbl[(gpu >> 21) & 0x1FFu]);
	if (tbl == NULL) return false;

	uint64_t *leaf = &tbl[(gpu >> 12) & 0x1FFu];
	*leaf = (phys & PTE_ADDRESS) | PTE_AF | PTE_SH_INNER | PTE_AP_RW | PTE_PAGE;
	FlushPoC_DcacheRegion((vm_offset_t)leaf, sizeof(*leaf));
	return true;
}

static void
unmap_page_locked(uint64_t gpu)
{
	if (pt_top == NULL) return;
	uint64_t *tbl = pt_top;
	uint64_t e;
	if (g_mali.pt_levels == 4u) {
		e = tbl[(gpu >> 39) & 0x1FFu];
		if (!(e & 1ull)) return;
		tbl = (uint64_t *)phystokv(e & PTE_ADDRESS);
		e = tbl[(gpu >> 30) & 0x1FFu];
		if (!(e & 1ull)) return;
		tbl = (uint64_t *)phystokv(e & PTE_ADDRESS);
	} else {
		e = tbl[(gpu >> 30) & 0x1Fu];
		if (!(e & 1ull)) return;
		tbl = (uint64_t *)phystokv(e & PTE_ADDRESS);
	}
	e = tbl[(gpu >> 21) & 0x1FFu];
	if (!(e & 1ull)) return;
	tbl = (uint64_t *)phystokv(e & PTE_ADDRESS);
	uint64_t *leaf = &tbl[(gpu >> 12) & 0x1FFu];
	if (*leaf != 0ull) {
		*leaf = 0ull;
		FlushPoC_DcacheRegion((vm_offset_t)leaf, sizeof(*leaf));
	}
}

static void
walk_tables_locked(uint64_t gpu, uint64_t walk[4])
{
	walk[0] = walk[1] = walk[2] = walk[3] = 0;
	if (pt_top == NULL) return;
	uint64_t *tbl = pt_top;
	unsigned idx = 0;
	if (g_mali.pt_levels == 4u) {
		walk[idx] = tbl[(gpu >> 39) & 0x1FFu];
		if (!(walk[idx] & 1ull)) return;
		tbl = (uint64_t *)phystokv(walk[idx] & PTE_ADDRESS);
		idx++;
		walk[idx] = tbl[(gpu >> 30) & 0x1FFu];
	} else {
		walk[idx] = tbl[(gpu >> 30) & 0x1Fu];
	}
	if (!(walk[idx] & 1ull)) return;
	tbl = (uint64_t *)phystokv(walk[idx] & PTE_ADDRESS);
	idx++;
	walk[idx] = tbl[(gpu >> 21) & 0x1FFu];
	if (!(walk[idx] & 1ull)) return;
	tbl = (uint64_t *)phystokv(walk[idx] & PTE_ADDRESS);
	idx++;
	walk[idx] = tbl[(gpu >> 12) & 0x1FFu];
}

/* 64 KiB granule VA allocator */
static uint64_t
gpu_granules_of(uint64_t size)
{
	return ((size + (1ull << GPU_GRANULE_SHIFT) - 1ull) >> GPU_GRANULE_SHIFT) + 1ull;
}

static bool
gpu_granule_is_used(uint64_t g)
{
	return ((gpu_granule_used[g / 64] >> (g % 64)) & 1ull) != 0;
}

static void
gpu_granules_set(uint64_t first, uint64_t count, bool used)
{
	for (uint64_t g = first; g < first + count && g < GPU_GRANULES; g++) {
		if (used) {
			gpu_granule_used[g / 64] |= 1ull << (g % 64);
		} else {
			gpu_granule_used[g / 64] &= ~(1ull << (g % 64));
		}
	}
	gpu_granules_taken = used ? (gpu_granules_taken + count)
	                          : (gpu_granules_taken - (count < gpu_granules_taken ? count : gpu_granules_taken));
}

static uint64_t
gpu_granules_find(uint64_t count)
{
	for (int pass = 0; pass < 2; pass++) {
		uint64_t g = (pass == 0) ? gpu_granule_next : 0ull;
		uint64_t end = (pass == 0) ? GPU_GRANULES : (gpu_granule_next + count);
		if (end > GPU_GRANULES) end = GPU_GRANULES;

		while (g + count <= end) {
			if (gpu_granule_used[g / 64] == ~0ull && (g % 64) == 0) {
				g += 64;
				continue;
			}
			uint64_t run = 0;
			while (run < count && !gpu_granule_is_used(g + run)) {
				run++;
			}
			if (run == count) {
				return g;
			}
			g += run + 1;
		}
	}
	return GPU_GRANULES;
}

/* =========================================================================
 * Section 6: Job Slots JS0/JS1, Waiter, Worker Queue & Self-Tests
 * ========================================================================= */

static bool
wait_job(uint32_t raw_mask, uint32_t ms)
{
	uint32_t spins = g_mali.spin_us / 10u;
	for (uint32_t i = 0; i < spins; i++) {
		if ((mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT) & raw_mask) != 0u) {
			return true;
		}
		mali_udelay(10);
	}
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT) & raw_mask) != 0u) {
			return true;
		}
		uint64_t nap;
		if (g_mali.job_irq_ok) {
			clock_interval_to_deadline(2000, NSEC_PER_USEC, &nap);
			(void)assert_wait_deadline((event_t)&mali_wait_event, THREAD_UNINT, nap);
			if ((mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT) & raw_mask) != 0u) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				return true;
			}
			(void)thread_block(THREAD_CONTINUE_NULL);
			continue;
		}
		uint32_t us = (g_mali.nap_us >= 10u && g_mali.nap_us <= 10000u) ? g_mali.nap_us : 200u;
		clock_interval_to_deadline(us, NSEC_PER_USEC, &nap);
		lck_mtx_lock(&mali_wait_lock);
		(void)lck_mtx_sleep_deadline(&mali_wait_lock, LCK_SLEEP_DEFAULT,
		    (event_t)&mali_wait_event, THREAD_UNINT, nap);
		lck_mtx_unlock(&mali_wait_lock);
	}
	return (mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT) & raw_mask) != 0u;
}

static void
build_write_value_job(vm_offset_t page, uint64_t gpu_va, uint32_t magic)
{
	volatile uint32_t *job = (volatile uint32_t *)page;
	bzero((void *)page, 4096);
	/* Job Header (32 B): type=2 (WRITE_VALUE) << 1 | is_64b (1) | index=1 << 16 */
	job[4]  = (2u << 1) | 1u | (1u << 16);
	/* Payload at +0x20 (word 8): 64-bit target addr, type=6 (IMMEDIATE_32), value */
	uint64_t dst = gpu_va + 0x800ull;
	job[8]  = (uint32_t)dst;
	job[9]  = (uint32_t)(dst >> 32);
	job[10] = 6u;
	job[12] = magic;
	FlushPoC_DcacheRegion(page, 4096);
}

static bool
run_selftest_write_job(vm_offset_t page, uint64_t gpu_va, uint32_t magic,
    struct mali_selftest_res *res, const char *label)
{
	volatile uint32_t *job    = (volatile uint32_t *)page;
	volatile uint32_t *target = (volatile uint32_t *)(page + 0x800);
	uint32_t slot_mask = (1u << 1) | (1u << 17);

	build_write_value_job(page, gpu_va, magic);

	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_MMU_IRQ_CLEAR, 0xFFFFFFFFu);

	mmio_w32(g_mali.mali, MALI_JS1 + JS_HEAD_NEXT_LO, (uint32_t)gpu_va);
	mmio_w32(g_mali.mali, MALI_JS1 + JS_HEAD_NEXT_HI, (uint32_t)(gpu_va >> 32));
	mmio_w32(g_mali.mali, MALI_JS1 + JS_AFFINITY_NEXT_LO, g_mali.shader_present_lo);
	mmio_w32(g_mali.mali, MALI_JS1 + JS_AFFINITY_NEXT_HI, g_mali.shader_present_hi);
	mmio_w32(g_mali.mali, MALI_JS1 + JS_CONFIG_NEXT, JS_CONFIG_DEFAULT);
	if (g_mali.job_irq_ok) {
		mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, slot_mask);
	}
	mmio_w32(g_mali.mali, MALI_JS1 + JS_COMMAND_NEXT, JS_CMD_START);

	(void)wait_job(slot_mask, 200);

	FlushPoC_DcacheRegion(page, 4096);
	res->rawstat        = mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT);
	res->js_status      = mmio_r32(g_mali.mali, MALI_JS1 + JS_STATUS);
	res->job_hdr_status = job[0];
	res->mmu_fault      = mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTSTATUS);
	res->mmu_fault_addr = (uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_LO) |
	    ((uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_HI) << 32);
	res->word = *target;
	res->ok   = (res->word == magic) && ((res->rawstat & (1u << 1)) != 0u) && ((res->rawstat & (1u << 17)) == 0u);

	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, 0xFFFFFFFFu);

	printf("mtk_mali: %s: %s word=0x%08x raw=0x%x js=0x%x hdr=0x%x fault=0x%x@0x%llx\n",
	    label, res->ok ? "OK" : "FAIL",
	    res->word, res->rawstat, res->js_status, res->job_hdr_status,
	    res->mmu_fault, (unsigned long long)res->mmu_fault_addr);
	return res->ok;
}

/* =========================================================================
 * Section 7: Exported C API (ready for Stage 3 HakiosMali.cpp)
 * ========================================================================= */

bool mtk_mali_ready(void);
bool mtk_mali_parameter(uint32_t which, uint64_t *value);
uint64_t mtk_mali_address(uint64_t size);
bool mtk_mali_map(uint64_t gpu, uint64_t phys);
void mtk_mali_mapped(void);
void mtk_mali_unmap(uint64_t gpu, uint64_t size);
void mtk_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height,
    uint32_t row_bytes, uint32_t format, uint32_t offset);
uint32_t mtk_mali_submit(uint64_t chain, bool fragment);
uint64_t mtk_mali_enqueue(uint64_t chain, bool fragment);
bool mtk_mali_wait(uint64_t seq, uint64_t timeout_ns);

bool
mtk_mali_ready(void)
{
	return g_mali.stage >= 6;
}

/* Linux DRM_PANFROST_PARAM_* values directly from Mali-G68 hardware registers */
bool
mtk_mali_parameter(uint32_t which, uint64_t *value)
{
	static const uint16_t simple[] = {
		[5]  = MALI_STACK_PRESENT_LO,
		[6]  = MALI_AS_PRESENT,
		[7]  = MALI_JS_PRESENT,
		[8]  = MALI_L2_FEATURES,
		[9]  = MALI_CORE_FEATURES,
		[10] = MALI_TILER_FEATURES,
		[11] = MALI_MEM_FEATURES,
		[12] = MALI_MMU_FEATURES,
		[13] = MALI_THREAD_FEATURES,
		[14] = MALI_THREAD_MAX_THREADS,
		[15] = MALI_THREAD_MAX_WG,
		[16] = MALI_THREAD_MAX_BARRIER,
		[17] = MALI_COHERENCY_FEATURES,
		[18] = MALI_TEX_FEATURES_0,
		[19] = MALI_TEX_FEATURES_1,
		[20] = MALI_TEX_FEATURES_2,
		[21] = 0x0BCu, /* TEXTURE_FEATURES_3 */
	};

	if (g_mali.stage < 2 || g_mali.mali == NULL) {
		return false;
	}
	switch (which) {
	case 0:
		*value = g_mali.gpu_id >> 16;
		return true;
	case 1:
		*value = g_mali.gpu_id & 0xFFFFu;
		return true;
	case 2:
	case 3:
	case 4: {
		uint32_t base = 0x100u + (which - 2u) * 0x10u;
		*value = (uint64_t)mmio_r32(g_mali.mali, base) |
		    ((uint64_t)mmio_r32(g_mali.mali, base + 4u) << 32);
		return true;
	}
	case 38: /* NR_CORE_GROUPS */
		*value = 1u;
		return true;
	case 39: { /* THREAD_TLS_ALLOC */
		uint32_t tls = mmio_r32(g_mali.mali, MALI_THREAD_TLS_ALLOC);
		uint32_t maxt = mmio_r32(g_mali.mali, MALI_THREAD_MAX_THREADS);
		*value = (tls >= maxt) ? tls : maxt;
		return true;
	}
	case 40: /* AFBC_FEATURES */
		*value = mmio_r32(g_mali.mali, MALI_AFBC_FEATURES);
		return true;
	default:
		if (which >= 22u && which <= 37u) {
			*value = mmio_r32(g_mali.mali, MALI_JS_FEATURES_0 + (which - 22u) * 4u);
			return true;
		}
		if (which < (sizeof(simple) / sizeof(simple[0])) && simple[which] != 0u) {
			*value = mmio_r32(g_mali.mali, simple[which]);
			return true;
		}
		return false;
	}
}

uint64_t
mtk_mali_address(uint64_t size)
{
	uint64_t address = 0;
	uint64_t count = gpu_granules_of(size);

	lck_mtx_lock(&mali_lock);
	uint64_t first = (size == 0) ? GPU_GRANULES : gpu_granules_find(count);
	if (first < GPU_GRANULES) {
		gpu_granules_set(first, count, true);
		gpu_granule_next = first + count;
		address = GPU_ADDRESS_FIRST + (first << GPU_GRANULE_SHIFT);
		for (int i = 0; i < MALI_RECENT; i++) {
			if (mali_recent[i].address == address) {
				mali_recent[i].size = 0;
			}
		}
		uint32_t idx = mali_recent_next++ % MALI_RECENT;
		mali_recent[idx].address   = address;
		mali_recent[idx].size      = size;
		mali_recent[idx].freed     = 0;
		mali_recent[idx].kind      = 0;
		mali_recent[idx].made_pid  = proc_selfpid();
		proc_selfname(mali_recent[idx].made_name, sizeof(mali_recent[idx].made_name));
		mali_recent[idx].freed_pid = 0;
		mali_recent[idx].freed_name[0] = 0;
	}
	lck_mtx_unlock(&mali_lock);
	return address;
}

bool
mtk_mali_map(uint64_t gpu, uint64_t phys)
{
	lck_mtx_lock(&mali_lock);
	bool ok = map_page_locked(gpu, phys);
	lck_mtx_unlock(&mali_lock);
	return ok;
}

void
mtk_mali_mapped(void)
{
	if (g_mali.fastmap) {
		return;
	}
	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, mali_transcfg_aarch64());
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

void
mtk_mali_unmap(uint64_t gpu, uint64_t size)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == gpu && mali_recent[i].size != 0) {
			mali_recent[i].freed     = 1;
			mali_recent[i].freed_pid = proc_selfpid();
			mali_recent[i].freed_at  = mach_absolute_time();
			proc_selfname(mali_recent[i].freed_name, sizeof(mali_recent[i].freed_name));
		}
	}
	for (uint64_t at = gpu; at < gpu + size; at += 4096ull) {
		unmap_page_locked(at);
	}
	lck_mtx_unlock(&mali_lock);

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	as0_update(pt_top_phys, mali_transcfg_aarch64());
	if (gpu >= GPU_ADDRESS_FIRST && gpu < GPU_ADDRESS_END) {
		gpu_granules_set((gpu - GPU_ADDRESS_FIRST) >> GPU_GRANULE_SHIFT,
		    gpu_granules_of(size), false);
	}
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
}

void
mtk_mali_tag(uint64_t address, uint32_t kind, uint32_t width, uint32_t height,
    uint32_t row_bytes, uint32_t format, uint32_t offset)
{
	lck_mtx_lock(&mali_lock);
	for (int i = 0; i < MALI_RECENT; i++) {
		if (mali_recent[i].address == address && mali_recent[i].size != 0) {
			mali_recent[i].kind      = kind;
			mali_recent[i].width     = width;
			mali_recent[i].height    = height;
			mali_recent[i].row_bytes = row_bytes;
			mali_recent[i].format    = format;
			mali_recent[i].offset    = offset;
		}
	}
	lck_mtx_unlock(&mali_lock);
}

uint32_t
mtk_mali_submit(uint64_t chain, bool fragment)
{
	uint32_t slot = fragment ? 0u : 1u;
	uint32_t js   = fragment ? MALI_JS0 : MALI_JS1;
	uint32_t slot_mask = (1u << slot) | (1u << (slot + 16u));
	uint32_t status = 0;

	lck_mtx_lock(&mali_job_lock);
	lck_mtx_lock(&mali_lock);
	g_mali.submits_total++;

	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, slot_mask);
	mmio_w32(g_mali.mali, js + JS_HEAD_NEXT_LO, (uint32_t)chain);
	mmio_w32(g_mali.mali, js + JS_HEAD_NEXT_HI, (uint32_t)(chain >> 32));
	mmio_w32(g_mali.mali, js + JS_AFFINITY_NEXT_LO, g_mali.shader_present_lo);
	mmio_w32(g_mali.mali, js + JS_AFFINITY_NEXT_HI, g_mali.shader_present_hi);
	mmio_w32(g_mali.mali, js + JS_CONFIG_NEXT, JS_CONFIG_DEFAULT);
	if (g_mali.job_irq_ok) {
		mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, slot_mask);
	}
	mmio_w32(g_mali.mali, js + JS_COMMAND_NEXT, JS_CMD_START);
	lck_mtx_unlock(&mali_lock);

	bool waited = wait_job(slot_mask, 1000);

	lck_mtx_lock(&mali_lock);
	uint32_t raw = mmio_r32(g_mali.mali, MALI_JOB_IRQ_RAWSTAT);
	bool ok = waited && ((raw & (1u << slot)) != 0u) && ((raw & (1u << (slot + 16u))) == 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_JOB_IRQ_CLEAR, slot_mask);

	if (!ok) {
		g_mali.submits_failed++;
		status = (raw & (1u << (slot + 16u))) ? mmio_r32(g_mali.mali, js + JS_STATUS) : 0xFFu;
		g_mali.last_fail_slot   = slot;
		g_mali.last_fail_raw    = raw;
		g_mali.last_fail_status = mmio_r32(g_mali.mali, js + JS_STATUS);
		g_mali.last_fail_mmu    = mmio_r32(g_mali.mali, MALI_MMU_IRQ_RAWSTAT);
		g_mali.last_fail_fault  = mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTSTATUS);
		g_mali.last_fail_addr   = (uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_LO) |
		    ((uint64_t)mmio_r32(g_mali.mali, MALI_AS0 + AS_FAULTADDRESS_HI) << 32);
		g_mali.last_fail_chain  = chain;
		walk_tables_locked(g_mali.last_fail_addr, g_mali.last_fail_walk);

		printf("mtk_mali: job fail slot=%u raw=0x%x status=0x%x mmu=0x%x fault=0x%x@0x%llx chain=0x%llx\n",
		    slot, raw, g_mali.last_fail_status, g_mali.last_fail_mmu,
		    g_mali.last_fail_fault, (unsigned long long)g_mali.last_fail_addr,
		    (unsigned long long)chain);

		/* Soft-reset & restore AS0 so subsequent jobs keep working */
		(void)mali_hw_reset_and_pwron();
		as0_update(pt_top_phys, mali_transcfg_aarch64());
		if (g_mali.mmu_irq_ok) {
			mmio_w32(g_mali.mali, MALI_MMU_IRQ_MASK, 0x00010001u);
		}
	}
	lck_mtx_unlock(&mali_lock);
	lck_mtx_unlock(&mali_job_lock);
	return status;
}

static void
mali_worker(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		lck_mtx_lock(&mali_queue_lock);
		while (mali_queue_count == 0) {
			lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT,
			    (event_t)&mali_queue_count, THREAD_UNINT);
		}
		uint64_t chain = mali_queue[mali_queue_head].chain;
		uint64_t seq   = mali_queue[mali_queue_head].seq;
		bool fragment  = mali_queue[mali_queue_head].fragment;
		lck_mtx_unlock(&mali_queue_lock);

		(void)mtk_mali_submit(chain, fragment);

		lck_mtx_lock(&mali_queue_lock);
		mali_queue_head = (mali_queue_head + 1u) % MALI_QUEUE;
		mali_queue_count--;
		mali_done_seq = seq;
		lck_mtx_unlock(&mali_queue_lock);
		wakeup((caddr_t)&mali_done_seq);
		wakeup((caddr_t)&mali_queue_head);
	}
}

uint64_t
mtk_mali_enqueue(uint64_t chain, bool fragment)
{
	lck_mtx_lock(&mali_queue_lock);
	if (!mali_worker_started) {
		thread_t thread = THREAD_NULL;
		if (kernel_thread_start(mali_worker, NULL, &thread) == KERN_SUCCESS && thread != THREAD_NULL) {
			thread_set_thread_name(thread, "the Mali's jobs");
			thread_deallocate(thread);
			mali_worker_started = true;
		}
	}
	while (mali_queue_count == MALI_QUEUE) {
		lck_mtx_sleep(&mali_queue_lock, LCK_SLEEP_DEFAULT,
		    (event_t)&mali_queue_head, THREAD_UNINT);
	}
	uint64_t seq = ++mali_queued_seq;
	unsigned at = (mali_queue_head + mali_queue_count) % MALI_QUEUE;
	mali_queue[at].chain    = chain;
	mali_queue[at].fragment = fragment;
	mali_queue[at].seq      = seq;
	mali_queue[at].queued   = mach_absolute_time();
	mali_queue[at].pid      = proc_selfpid();
	proc_selfname(mali_queue[at].name, sizeof(mali_queue[at].name));
	mali_queue_count++;
	lck_mtx_unlock(&mali_queue_lock);
	wakeup((caddr_t)&mali_queue_count);
	return seq;
}

bool
mtk_mali_wait(uint64_t seq, uint64_t timeout_ns)
{
	if (timeout_ns > 10ull * NSEC_PER_SEC) {
		timeout_ns = 10ull * NSEC_PER_SEC;
	}
	uint64_t deadline = 0;
	nanoseconds_to_absolutetime(timeout_ns, &deadline);
	deadline += mach_absolute_time();

	lck_mtx_lock(&mali_queue_lock);
	while (mali_done_seq < seq && timeout_ns != 0 && mach_absolute_time() < deadline) {
		lck_mtx_sleep_deadline(&mali_queue_lock, LCK_SLEEP_DEFAULT,
		    (event_t)&mali_done_seq, THREAD_UNINT, deadline);
	}
	bool done = (mali_done_seq >= seq);
	lck_mtx_unlock(&mali_queue_lock);
	return done;
}

static bool
mt6877_mali_selftests(void)
{
	vm_offset_t page = 0;
	if (kmem_alloc(kernel_map, &page, PAGE_SIZE,
	    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		g_mali.state = "selftest_nomem";
		return false;
	}
	uint64_t phys = kvtophys(page);

	/* Self-test 1 (stage 4): Identity mode WRITE_VALUE 0xC0FFEE01 */
	as0_update(0ull, AS_TRANSCFG_ADRMODE_IDENTITY);
	if (!run_selftest_write_job(page, phys, 0xC0FFEE01u, &g_mali.st_id, "selftest_identity")) {
		g_mali.state = "selftest_identity_fail";
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}
	g_mali.stage = 4;

	/* Self-test 2 (stage 5): AArch64_4K page table mode WRITE_VALUE 0xC0FFEE02 */
	if (!map_page_locked(FIRST_JOB_GPU, phys)) {
		g_mali.state = "selftest_pt_nomem";
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}
	as0_update(pt_top_phys, mali_transcfg_aarch64());
	if (!run_selftest_write_job(page, FIRST_JOB_GPU, 0xC0FFEE02u, &g_mali.st_mmu, "selftest_mmu")) {
		g_mali.state = "selftest_mmu_fail";
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}
	unmap_page_locked(FIRST_JOB_GPU);
	as0_update(pt_top_phys, mali_transcfg_aarch64());
	g_mali.stage = 5;

	/* Enable MMU AS0 fault IRQ now that page tables are verified */
	if (g_mali.mmu_irq_ok) {
		mmio_w32(g_mali.mali, MALI_MMU_IRQ_CLEAR, 0xFFFFFFFFu);
		mmio_w32(g_mali.mali, MALI_MMU_IRQ_MASK, 0x00010001u);
	}

	/* Self-test 3 (stage 6): Full API (mtk_mali_address/map/enqueue/wait/unmap) WRITE_VALUE 0xC0FFEE03 */
	uint64_t va = mtk_mali_address(4096);
	g_mali.st_api_va = va;
	if (va == 0 || !mtk_mali_map(va, phys)) {
		g_mali.state = "selftest_api_map_fail";
		kmem_free(kernel_map, page, PAGE_SIZE);
		return false;
	}
	mtk_mali_mapped();
	build_write_value_job(page, va, 0xC0FFEE03u);

	uint64_t seq = mtk_mali_enqueue(va, false);
	g_mali.st_api_seq = seq;
	bool done = mtk_mali_wait(seq, 1000ull * NSEC_PER_MSEC);
	FlushPoC_DcacheRegion(page, 4096);
	volatile uint32_t *target = (volatile uint32_t *)(page + 0x800);
	g_mali.st_api_word = *target;
	g_mali.st_api_ok   = done && (g_mali.st_api_word == 0xC0FFEE03u) && (g_mali.submits_failed == 0);
	mtk_mali_unmap(va, 4096);
	kmem_free(kernel_map, page, PAGE_SIZE);

	printf("mtk_mali: selftest_api: %s va=0x%llx seq=%llu word=0x%08x\n",
	    g_mali.st_api_ok ? "OK" : "FAIL",
	    (unsigned long long)va, (unsigned long long)seq, g_mali.st_api_word);

	if (!g_mali.st_api_ok) {
		g_mali.state = "selftest_api_fail";
		return false;
	}
	g_mali.stage = 6;
	g_mali.state = "selftest_ok";
	return true;
}

/* =========================================================================
 * Persistent sysctl kern.hakios.mali & driver entry point
 * ========================================================================= */

static int
sysctl_hakios_mali(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char buf[1536];
	uint32_t vsram_uv = 500000u + (((g_mali.vsram_da_a >> 8) & 0x7Fu) * 6250u);
	uint32_t vgpu_uv  = 400000u + ((g_mali.vgpu_da_a & 0x7Fu) * 6250u);
	uint32_t arch_maj = (g_mali.gpu_id >> 28) & 0xFu;
	uint32_t prod     = (g_mali.gpu_id >> 16) & 0xFFFFu;
	uint32_t rev_maj  = (g_mali.gpu_id >> 12) & 0xFu;
	uint32_t rev_min  = (g_mali.gpu_id >> 4) & 0xFFu;

	int n = snprintf(buf, sizeof(buf),
	    "stage=%d state=%s freq_mhz=%u\n"
	    "pmif done=%u hwcid=0x%04x swcid=0x%04x vsram=%uuV vgpu=%uuV\n"
	    "spm xpu=0x%x/0x%x->0x%x/0x%x mfg_ok=0x%x prot=0x%x/0x%x clk20=0x%x cg=0x%x\n"
	    "mali id=0x%08x prod=0x%04x arch=v%u r%up%u reset_ok=%u\n"
	    "mali feat: l2=0x%x core=0x%x tiler=0x%x mem=0x%x mmu=0x%x as=0x%x js=0x%x\n"
	    "mali ext: afbc=0x%x coh=0x%x/0x%x tls=0x%x stack=0x%x va=%u pa=%u lvl=%u tcfg=0x%x\n"
	    "mali ready: l2=0x%x tiler=0x%x shader=0x%x (present=0x%x)\n"
	    "irq: gpu=%u(ok=%u cnt=%llu sp=%llu test=%u) mmu=%u(ok=%u cnt=%llu sp=%llu) job=%u(ok=%u cnt=%llu sp=%llu)\n"
	    "selftest: id=%s(0x%08x/raw=0x%x/js=0x%x/flt=0x%x) mmu=%s(0x%08x/raw=0x%x/js=0x%x/flt=0x%x) api=%s(0x%08x/va=0x%llx/seq=%llu)\n"
	    "queue: submits=%llu failed=%llu done=%llu granules=%llu pt_top=0x%llx",
	    g_mali.stage, g_mali.state, g_mali.target_mhz,
	    g_mali.pmif_done, g_mali.hwcid, g_mali.swcid, vsram_uv, vgpu_uv,
	    g_mali.xpu_b, g_mali.xpu2_b, g_mali.xpu_a, g_mali.xpu2_a,
	    g_mali.mfg_ok_mask, g_mali.prot1_a, g_mali.prot2_a, g_mali.clk20_a, g_mali.cg_a,
	    g_mali.gpu_id, prod, arch_maj, rev_maj, rev_min, g_mali.reset_ok,
	    g_mali.l2_feat, g_mali.core_feat, g_mali.tiler_feat, g_mali.mem_feat,
	    g_mali.mmu_feat, g_mali.as_present, g_mali.js_present,
	    g_mali.afbc_feat, g_mali.coh_feat, g_mali.coh_en, g_mali.tls_alloc, g_mali.stack_present,
	    g_mali.va_bits, g_mali.pa_bits, g_mali.pt_levels, g_mali.transcfg_active,
	    g_mali.l2_ready_lo, g_mali.tiler_ready_lo, g_mali.shader_ready_lo, g_mali.shader_present_lo,
	    MALI_IRQ_GPU, g_mali.gpu_irq_ok, (unsigned long long)g_mali.gpu_irq_cnt,
	    (unsigned long long)g_mali.gpu_irq_spurious, g_mali.gpu_irq_test_ok,
	    MALI_IRQ_MMU, g_mali.mmu_irq_ok, (unsigned long long)g_mali.mmu_irq_cnt,
	    (unsigned long long)g_mali.mmu_irq_spurious,
	    MALI_IRQ_JOB, g_mali.job_irq_ok, (unsigned long long)g_mali.job_irq_cnt,
	    (unsigned long long)g_mali.job_irq_spurious,
	    g_mali.st_id.ok ? "OK" : "FAIL", g_mali.st_id.word, g_mali.st_id.rawstat,
	    g_mali.st_id.js_status, g_mali.st_id.mmu_fault,
	    g_mali.st_mmu.ok ? "OK" : "FAIL", g_mali.st_mmu.word, g_mali.st_mmu.rawstat,
	    g_mali.st_mmu.js_status, g_mali.st_mmu.mmu_fault,
	    g_mali.st_api_ok ? "OK" : "FAIL", g_mali.st_api_word,
	    (unsigned long long)g_mali.st_api_va, (unsigned long long)g_mali.st_api_seq,
	    (unsigned long long)g_mali.submits_total, (unsigned long long)g_mali.submits_failed,
	    (unsigned long long)mali_done_seq, (unsigned long long)gpu_granules_taken,
	    (unsigned long long)pt_top_phys);
	if (n < 0) n = 0;
	if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
	return SYSCTL_OUT(req, buf, (size_t)n + 1);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, mali,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_hakios_mali, "A",
    "MediaTek MT6877 Mali-G68 MC4 bring-up & self-test status");

static void
mtk_mali_thread(__unused void *arg, __unused wait_result_t wr)
{
	delay(100000);

	if (!mt6877_pmic_power_on()) {
		printf("mtk_mali: PMIC bring-up failed (%s, done=%u swcid=0x%04x)\n",
		    g_mali.state, g_mali.pmif_done, g_mali.swcid);
		goto done;
	}
	if (!mt6877_mfg_power_and_clk()) {
		printf("mtk_mali: MFG power/clk failed (%s, mfg_ok=0x%x xpu=0x%x/0x%x)\n",
		    g_mali.state, g_mali.mfg_ok_mask, g_mali.xpu_a, g_mali.xpu2_a);
		goto done;
	}
	if (!mt6877_mali_probe_and_reset()) {
		printf("mtk_mali: Mali probe/reset failed (%s, gpu_id=0x%08x)\n",
		    g_mali.state, g_mali.gpu_id);
		goto done;
	}
	mt6877_mali_attach_irqs();
	if (!mt6877_mali_selftests()) {
		printf("mtk_mali: self-test stopped at stage %d (%s)\n", g_mali.stage, g_mali.state);
		goto done;
	}

	printf("mtk_mali: STAGE 6 OK! Mali-G68 0x%08x (v%u r%up%u) %uMHz VA=%u(%ulvl) irq(gpu=%llu mmu=%llu job=%llu)\n",
	    g_mali.gpu_id, (g_mali.gpu_id >> 28) & 0xFu,
	    (g_mali.gpu_id >> 12) & 0xFu, (g_mali.gpu_id >> 4) & 0xFFu,
	    g_mali.target_mhz, g_mali.va_bits, g_mali.pt_levels,
	    (unsigned long long)g_mali.gpu_irq_cnt,
	    (unsigned long long)g_mali.mmu_irq_cnt,
	    (unsigned long long)g_mali.job_irq_cnt);

done:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

void
hakios_mtk_mali_init(void)
{
	int mode = 0;
	if (g_mali.started) {
		return;
	}
	if (!PE_parse_boot_argn("mali", &mode, sizeof(mode)) || mode <= 0) {
		g_mali.state = "bootarg_off";
		return;
	}
	g_mali.started = true;

	int val = 0;
	if (PE_parse_boot_argn("mali_freq", &val, sizeof(val)) && val > 0) {
		g_mali.target_mhz = (uint32_t)val;
	}
	if (PE_parse_boot_argn("mali_irq", &val, sizeof(val))) {
		g_mali.want_irq = (val != 0) ? 1u : 0u;
	}
	if (PE_parse_boot_argn("mali_fastmap", &val, sizeof(val))) {
		g_mali.fastmap = (val != 0) ? 1u : 0u;
	}
	if (PE_parse_boot_argn("mali_walk_wb", &val, sizeof(val))) {
		g_mali.walk_wb = (val != 0) ? 1u : 0u;
	}

	vm_offset_t v_top  = ml_io_map(MT6877_TOPCKGEN_PA, 0x1000);
	vm_offset_t v_inf  = ml_io_map(MT6877_INFRACFG_PA, 0x1000);
	vm_offset_t v_spm  = ml_io_map(MT6877_SPM_PA,      0x1000);
	vm_offset_t v_pmif = ml_io_map(MT6877_PMIF_PA,     0x1000);
	vm_offset_t v_pll  = ml_io_map(MT6877_GPU_PLL_PA,  0x1000);
	vm_offset_t v_cfg  = ml_io_map(MT6877_MFGCFG_PA,   0x1000);
	vm_offset_t v_mali = ml_io_map(MT6877_MALI_PA,     0x4000);

	if (!v_top || !v_inf || !v_spm || !v_pmif || !v_pll || !v_cfg || !v_mali) {
		g_mali.state = "mmap_fail";
		printf("mtk_mali: ml_io_map failed\n");
		return;
	}

	g_mali.topckgen = (volatile uint32_t *)v_top;
	g_mali.infracfg = (volatile uint32_t *)v_inf;
	g_mali.spm      = (volatile uint32_t *)v_spm;
	g_mali.pmif     = (volatile uint32_t *)v_pmif;
	g_mali.gpu_pll  = (volatile uint32_t *)v_pll;
	g_mali.mfgcfg   = (volatile uint32_t *)v_cfg;
	g_mali.mali     = (volatile uint32_t *)v_mali;

	thread_t th = THREAD_NULL;
	if (kernel_thread_start(mtk_mali_thread, NULL, &th) == KERN_SUCCESS && th != THREAD_NULL) {
		thread_set_thread_name(th, "mtk-mali-init");
		thread_deallocate(th);
	} else {
		g_mali.state = "thread_fail";
	}
}
