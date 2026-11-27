/*
 * MediaTek MT6877 (Dimensity 1080 / ruby) Mali-G68 MC4 (Valhall v9) platform bring-up:
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
 *
 * Persistent telemetry: sysctl kern.hakios.mali
 * Boot args: mali=1 [mali_freq=450]
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);

SYSCTL_DECL(_kern_hakios);

/* Physical MMIO bases verified against live ruby FDT & MT6877 Linux kernel */
#define MT6877_TOPCKGEN_PA      0x10000000UL
#define MT6877_INFRACFG_PA      0x10001000UL
#define MT6877_SPM_PA           0x10006000UL
#define MT6877_PMIF_PA          0x10026000UL
#define MT6877_MALI_PA          0x13000000UL
#define MT6877_GPU_PLL_PA       0x13FA0000UL
#define MT6877_MFGCFG_PA        0x13FBF000UL

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

/* Mali-G68 (Valhall Job Manager) registers at 0x13000000 */
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
#define MALI_GPU_COMMAND        0x0030u
#define MALI_GPU_STATUS         0x0034u
#define MALI_THREAD_MAX_THREADS 0x00A0u
#define MALI_THREAD_MAX_WG      0x00A4u
#define MALI_THREAD_FEATURES    0x00ACu
#define MALI_TEX_FEATURES_0     0x00B0u
#define MALI_TEX_FEATURES_1     0x00B4u
#define MALI_TEX_FEATURES_2     0x00B8u
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

#define MALI_CMD_SOFT_RESET     1u
#define MALI_IRQ_RESET_DONE     (1u << 8)

static struct {
	bool started;
	int stage;
	const char *state;
	uint32_t target_mhz;

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

	/* Mali telemetry */
	uint32_t gpu_id;
	uint32_t l2_feat, core_feat, tiler_feat, mem_feat, mmu_feat;
	uint32_t as_present, js_present;
	uint32_t max_threads, max_wg, thread_feat;
	uint32_t tex0, tex1, tex2;
	uint32_t shader_present_lo, shader_present_hi;
	uint32_t tiler_present_lo, l2_present_lo;
	uint32_t shader_ready_lo, tiler_ready_lo, l2_ready_lo;
	uint32_t reset_ok;
} g_mali = {
	.state = "not_started",
	.target_mhz = 450,
};

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
pmif_wait_fsm(uint32_t want)
{
	for (uint32_t i = 0; i < 20000; i++) {
		uint32_t sta = mmio_r32(g_mali.pmif, PMIF_SWINF_2_STA);
		if (((sta >> 1) & 0x7u) == want) {
			return true;
		}
		mali_udelay(1);
	}
	return false;
}

static bool
pmif_read16(uint16_t addr, uint16_t *out)
{
	if (!pmif_wait_fsm(PMIF_FSM_IDLE)) {
		return false;
	}
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_ACC, (uint32_t)addr);
	if (!pmif_wait_fsm(PMIF_FSM_WFVLDCLR)) {
		return false;
	}
	*out = (uint16_t)(mmio_r32(g_mali.pmif, PMIF_SWINF_2_RDATA) & 0xFFFFu);
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_VLD_CLR, 1u);
	return true;
}

static bool
pmif_write16(uint16_t addr, uint16_t val)
{
	if (!pmif_wait_fsm(PMIF_FSM_IDLE)) {
		return false;
	}
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_WDATA, (uint32_t)val);
	mmio_w32(g_mali.pmif, PMIF_SWINF_2_ACC, (1u << 29) | (uint32_t)addr);
	return pmif_wait_fsm(PMIF_FSM_IDLE);
}

static bool
mt6877_pmic_power_on(void)
{
	g_mali.pmif_done = mmio_r32(g_mali.pmif, PMIF_INIT_DONE);
	if ((g_mali.pmif_done & 1u) == 0) {
		g_mali.state = "pmif_not_ready";
		return false;
	}

	if (!pmif_read16(MT6359_HWCID, &g_mali.hwcid) ||
	    !pmif_read16(MT6359_SWCID, &g_mali.swcid) ||
	    g_mali.swcid == 0x0000u || g_mali.swcid == 0xFFFFu) {
		g_mali.state = "pmic_id_fail";
		return false;
	}

	if (!pmif_read16(MT6359_VSRAM_MD_CON0, &g_mali.vsram_con0_b) ||
	    !pmif_read16(MT6359_VSRAM_MD_MON, &g_mali.vsram_mon_b) ||
	    !pmif_read16(MT6359_VSRAM_MD_ELR, &g_mali.vsram_elr_b) ||
	    !pmif_read16(MT6359_VSRAM_MD_VOSEL1, &g_mali.vsram_da_b) ||
	    !pmif_read16(MT6359_VPROC2_CON0, &g_mali.vgpu_con0_b) ||
	    !pmif_read16(MT6359_VPROC2_DBG1, &g_mali.vgpu_dbg1_b) ||
	    !pmif_read16(MT6359_VPROC2_ELR0, &g_mali.vgpu_elr_b) ||
	    !pmif_read16(MT6359_VPROC2_DBG0, &g_mali.vgpu_da_b)) {
		g_mali.state = "pmic_read_fail";
		return false;
	}

	/* 1. VSRAM_GPU (ldo_vsram_md) first: >= 0.75 V (sel 0x28) */
	uint16_t vsram_sel = g_mali.vsram_elr_b & 0x7Fu;
	if (vsram_sel < VSRAM_TARGET_SEL) {
		uint16_t nv = (uint16_t)((g_mali.vsram_elr_b & ~0x7Fu) | VSRAM_TARGET_SEL);
		if (!pmif_write16(MT6359_VSRAM_MD_ELR, nv)) {
			g_mali.state = "pmic_vsram_vosel_fail";
			return false;
		}
	}
	if ((g_mali.vsram_mon_b & 1u) == 0) {
		if (!pmif_write16(MT6359_VSRAM_MD_CON0, (uint16_t)(g_mali.vsram_con0_b | 1u))) {
			g_mali.state = "pmic_vsram_en_fail";
			return false;
		}
		mali_udelay(500);
	}

	/* 2. VGPU (buck_vproc2) second: >= 0.70 V (sel 0x30), keeping VSRAM >= VGPU */
	uint16_t vgpu_sel = g_mali.vgpu_elr_b & 0x7Fu;
	if (vgpu_sel < VGPU_TARGET_SEL) {
		uint16_t nv = (uint16_t)((g_mali.vgpu_elr_b & ~0x7Fu) | VGPU_TARGET_SEL);
		if (!pmif_write16(MT6359_VPROC2_ELR0, nv)) {
			g_mali.state = "pmic_vgpu_vosel_fail";
			return false;
		}
	}
	if ((g_mali.vgpu_dbg1_b & 1u) == 0) {
		if (!pmif_write16(MT6359_VPROC2_CON0, (uint16_t)(g_mali.vgpu_con0_b | 1u))) {
			g_mali.state = "pmic_vgpu_en_fail";
			return false;
		}
		mali_udelay(500);
	}

	/* Read back active regulator state */
	(void)pmif_read16(MT6359_VSRAM_MD_MON, &g_mali.vsram_mon_a);
	(void)pmif_read16(MT6359_VSRAM_MD_VOSEL1, &g_mali.vsram_da_a);
	(void)pmif_read16(MT6359_VPROC2_DBG1, &g_mali.vgpu_dbg1_a);
	(void)pmif_read16(MT6359_VPROC2_DBG0, &g_mali.vgpu_da_a);

	if ((g_mali.vsram_mon_a & 1u) == 0 || (g_mali.vgpu_dbg1_a & 1u) == 0) {
		g_mali.state = "pmic_da_en_zero";
		return false;
	}
	return true;
}

static bool
spm_mfg_domain_on(uint32_t idx, bool is_mfg1)
{
	uint32_t ofs = SPM_MFG0_PWR_CON + idx * 4u;
	uint32_t sta_bit = (1u << idx);

	uint32_t con = mmio_r32(g_mali.spm, ofs);
	uint32_t x1 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	uint32_t x2 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);

	if ((x1 & sta_bit) == 0 || (x2 & sta_bit) == 0 ||
	    (con & PWR_ISO) != 0 || (con & PWR_RST_B) == 0) {
		con |= PWR_ON;
		mmio_w32(g_mali.spm, ofs, con);
		con |= PWR_ON_2ND;
		mmio_w32(g_mali.spm, ofs, con);

		bool ack = false;
		for (uint32_t i = 0; i < 50000; i++) {
			x1 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
			x2 = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
			if ((x1 & sta_bit) && (x2 & sta_bit)) {
				ack = true;
				break;
			}
			mali_udelay(1);
		}
		if (!ack) {
			return false;
		}

		con = mmio_r32(g_mali.spm, ofs);
		con &= ~PWR_CLK_DIS;
		mmio_w32(g_mali.spm, ofs, con);
		con &= ~PWR_ISO;
		mmio_w32(g_mali.spm, ofs, con);
		con |= PWR_RST_B;
		mmio_w32(g_mali.spm, ofs, con);
	}

	/* Release SRAM_PDN and wait SRAM_PDN_ACK == 0 */
	con = mmio_r32(g_mali.spm, ofs);
	con &= ~MFG_SRAM_PDN;
	mmio_w32(g_mali.spm, ofs, con);

	bool sram_ok = false;
	for (uint32_t i = 0; i < 50000; i++) {
		if ((mmio_r32(g_mali.spm, ofs) & MFG_SRAM_PDN_ACK) == 0) {
			sram_ok = true;
			break;
		}
		mali_udelay(1);
	}
	if (!sram_ok) {
		return false;
	}

	/* MFG1 Bus Protect release: Step 2 then Step 1 (exact MT6877 PG order) */
	if (is_mfg1) {
		mmio_w32(g_mali.infracfg, INFRA_PROT_CLR, MFG1_PROT_STEP2_MASK);
		mmio_w32(g_mali.infracfg, INFRA_PROT2_CLR, MFG1_PROT_STEP1_MASK);
		mali_udelay(10);
	}

	return true;
}

static bool
mt6877_mfg_power_and_clk(void)
{
	/* Park MFG mux on 26 MHz first so f_fmfg_core_ck is alive for SRAM PDN delay IP */
	g_mali.clk20_b = mmio_r32(g_mali.topckgen, TOP_CLK_CFG_20);
	mmio_w32(g_mali.topckgen, TOP_CLK_CFG_20_CLR, TOP_MFG_INT2_SEL | TOP_MFG_INT1_SEL);
	mali_udelay(10);

	/* Unlock SPM register writes if needed */
	g_mali.spm_cfg_b = mmio_r32(g_mali.spm, SPM_POWERON_CONFIG_EN);
	g_mali.xpu_b = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	g_mali.xpu2_b = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);

	if ((g_mali.spm_cfg_b & 1u) == 0) {
		mmio_w32(g_mali.spm, SPM_POWERON_CONFIG_EN, SPM_PROJECT_CODE | 1u);
	}

	/* Power on MFG0 -> MFG1 -> MFG2..MFG5 */
	for (uint32_t i = 0; i < 6; i++) {
		if (spm_mfg_domain_on(i, i == 1)) {
			g_mali.mfg_ok_mask |= (1u << i);
		} else if (i == 0) {
			g_mali.xpu_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
			g_mali.xpu2_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
			g_mali.state = "spm_mfg0_fail";
			return false;
		} else if (i == 1) {
			g_mali.xpu_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
			g_mali.xpu2_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
			g_mali.state = "spm_mfg1_fail";
			return false;
		}
	}

	g_mali.xpu_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS);
	g_mali.xpu2_a = mmio_r32(g_mali.spm, SPM_XPU_PWR_STATUS_2ND);
	g_mali.prot1_a = mmio_r32(g_mali.infracfg, INFRA_PROT2_STA1);
	g_mali.prot2_a = mmio_r32(g_mali.infracfg, INFRA_PROT_STA1);
	g_mali.stage = 1;

	/* Configure MFGPLL1 at GPU_PLL_CTRL (0x13FA0000) unless user asked for 26 MHz */
	g_mali.pll_con0_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON0);
	g_mali.pll_con1_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON1);
	g_mali.pll_con3_b = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON3);

	if (g_mali.target_mhz != 26u) {
		uint32_t mhz = g_mali.target_mhz;
		if (mhz < 200u) mhz = 200u;
		if (mhz > 850u) mhz = 850u;
		g_mali.target_mhz = mhz;

		uint32_t posdiv_pow = (mhz > 750u) ? 1u : 2u; /* /2 or /4 */
		uint32_t pcw = (uint32_t)((((uint64_t)mhz * (1ULL << posdiv_pow) * 16384ULL) + 13ULL) / 26ULL);

		uint32_t con3 = g_mali.pll_con3_b | PLL_PWR_ON;
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON3, con3);
		mali_udelay(5);
		con3 &= ~PLL_ISO_EN;
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON3, con3);
		mali_udelay(5);

		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON1,
		    PLL_PCW_CHG | (posdiv_pow << 24) | (pcw & 0x3FFFFFu));
		mmio_w32(g_mali.gpu_pll, MFGPLL1_CON0, g_mali.pll_con0_b | PLL_BASE_EN);
		mali_udelay(50);

		/* Switch mfg_internal2_sel -> mfgpll1_ck */
		mmio_w32(g_mali.topckgen, TOP_CLK_CFG_20_SET, TOP_MFG_INT2_SEL);
		mali_udelay(10);
	}

	g_mali.clk20_a = mmio_r32(g_mali.topckgen, TOP_CLK_CFG_20);
	g_mali.pll_con0_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON0);
	g_mali.pll_con1_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON1);
	g_mali.pll_con3_a = mmio_r32(g_mali.gpu_pll, MFGPLL1_CON3);

	/* Ungate BG3D in MFGCFG (0x13FBF000) */
	g_mali.cg_b = mmio_r32(g_mali.mfgcfg, MFG_CG_CON);
	mmio_w32(g_mali.mfgcfg, MFG_CG_CLR, 1u);
	mali_udelay(10);
	g_mali.cg_a = mmio_r32(g_mali.mfgcfg, MFG_CG_CON);

	return true;
}

static bool
mt6877_mali_probe_and_reset(void)
{
	g_mali.gpu_id = mmio_r32(g_mali.mali, MALI_GPU_ID);
	g_mali.l2_feat = mmio_r32(g_mali.mali, MALI_L2_FEATURES);
	g_mali.core_feat = mmio_r32(g_mali.mali, MALI_CORE_FEATURES);
	g_mali.tiler_feat = mmio_r32(g_mali.mali, MALI_TILER_FEATURES);
	g_mali.mem_feat = mmio_r32(g_mali.mali, MALI_MEM_FEATURES);
	g_mali.mmu_feat = mmio_r32(g_mali.mali, MALI_MMU_FEATURES);
	g_mali.as_present = mmio_r32(g_mali.mali, MALI_AS_PRESENT);
	g_mali.js_present = mmio_r32(g_mali.mali, MALI_JS_PRESENT);
	g_mali.max_threads = mmio_r32(g_mali.mali, MALI_THREAD_MAX_THREADS);
	g_mali.max_wg = mmio_r32(g_mali.mali, MALI_THREAD_MAX_WG);
	g_mali.thread_feat = mmio_r32(g_mali.mali, MALI_THREAD_FEATURES);
	g_mali.tex0 = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_0);
	g_mali.tex1 = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_1);
	g_mali.tex2 = mmio_r32(g_mali.mali, MALI_TEX_FEATURES_2);
	g_mali.shader_present_lo = mmio_r32(g_mali.mali, MALI_SHADER_PRESENT_LO);
	g_mali.shader_present_hi = mmio_r32(g_mali.mali, MALI_SHADER_PRESENT_HI);
	g_mali.tiler_present_lo = mmio_r32(g_mali.mali, MALI_TILER_PRESENT_LO);
	g_mali.l2_present_lo = mmio_r32(g_mali.mali, MALI_L2_PRESENT_LO);

	if (g_mali.gpu_id == 0u || g_mali.gpu_id == 0xFFFFFFFFu) {
		g_mali.state = "bad_gpu_id";
		return false;
	}

	/* Soft reset Mali */
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_MASK, 0u);
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);
	mmio_w32(g_mali.mali, MALI_GPU_COMMAND, MALI_CMD_SOFT_RESET);

	for (uint32_t i = 0; i < 50000; i++) {
		if (mmio_r32(g_mali.mali, MALI_GPU_IRQ_RAWSTAT) & MALI_IRQ_RESET_DONE) {
			g_mali.reset_ok = 1;
			break;
		}
		mali_udelay(1);
	}
	mmio_w32(g_mali.mali, MALI_GPU_IRQ_CLEAR, 0xFFFFFFFFu);

	if (!g_mali.reset_ok) {
		g_mali.state = "soft_reset_timeout";
		return false;
	}

	/* Power on L2, Tiler and all present Shader cores inside Mali */
	mmio_w32(g_mali.mali, MALI_L2_PWRON_LO, g_mali.l2_present_lo);
	mmio_w32(g_mali.mali, MALI_TILER_PWRON_LO, g_mali.tiler_present_lo);
	mmio_w32(g_mali.mali, MALI_SHADER_PWRON_LO, g_mali.shader_present_lo);

	for (uint32_t i = 0; i < 50000; i++) {
		g_mali.l2_ready_lo = mmio_r32(g_mali.mali, MALI_L2_READY_LO);
		g_mali.tiler_ready_lo = mmio_r32(g_mali.mali, MALI_TILER_READY_LO);
		g_mali.shader_ready_lo = mmio_r32(g_mali.mali, MALI_SHADER_READY_LO);
		if (g_mali.l2_ready_lo == g_mali.l2_present_lo &&
		    g_mali.tiler_ready_lo == g_mali.tiler_present_lo &&
		    g_mali.shader_ready_lo == g_mali.shader_present_lo) {
			break;
		}
		mali_udelay(1);
	}

	g_mali.stage = 2;
	if (g_mali.l2_ready_lo == g_mali.l2_present_lo &&
	    g_mali.tiler_ready_lo == g_mali.tiler_present_lo &&
	    g_mali.shader_ready_lo == g_mali.shader_present_lo) {
		g_mali.state = "mali_ready";
	} else {
		g_mali.state = "mali_pwron_partial";
	}
	return true;
}

static int
sysctl_hakios_mali(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char buf[768];
	uint32_t vsram_uv = 500000u + (((g_mali.vsram_da_a >> 8) & 0x7Fu) * 6250u);
	uint32_t vgpu_uv  = 400000u + ((g_mali.vgpu_da_a & 0x7Fu) * 6250u);
	uint32_t arch_maj = (g_mali.gpu_id >> 28) & 0xFu;
	uint32_t prod     = (g_mali.gpu_id >> 16) & 0xFFFFu;
	uint32_t rev_maj  = (g_mali.gpu_id >> 12) & 0xFu;
	uint32_t rev_min  = (g_mali.gpu_id >> 4) & 0xFFu;

	int n = snprintf(buf, sizeof(buf),
	    "stage=%d state=%s freq_mhz=%u\n"
	    "pmif done=%u hwcid=0x%04x swcid=0x%04x\n"
	    "pmic vsram: before(con0=0x%x mon=0x%x elr=0x%x da=0x%x) after(mon=0x%x da=0x%x uV=%u)\n"
	    "pmic vgpu:  before(con0=0x%x dbg1=0x%x elr=0x%x da=0x%x) after(dbg1=0x%x da=0x%x uV=%u)\n"
	    "spm cfg=0x%x xpu=0x%x/0x%x->0x%x/0x%x mfg_ok=0x%x prot=0x%x/0x%x\n"
	    "clk cfg20=0x%x->0x%x pll1=0x%x/%x/%x->0x%x/%x/%x cg=0x%x->0x%x\n"
	    "mali id=0x%08x prod=0x%04x arch=v%u r%up%u reset_ok=%u\n"
	    "mali feat: l2=0x%x core=0x%x tiler=0x%x mem=0x%x mmu=0x%x as=0x%x js=0x%x\n"
	    "mali thr: max=%u wg=%u feat=0x%x tex=0x%x/0x%x/0x%x\n"
	    "mali present: l2=0x%x tiler=0x%x shader=0x%x_%08x ready: l2=0x%x tiler=0x%x shader=0x%x",
	    g_mali.stage, g_mali.state, g_mali.target_mhz,
	    g_mali.pmif_done, g_mali.hwcid, g_mali.swcid,
	    g_mali.vsram_con0_b, g_mali.vsram_mon_b, g_mali.vsram_elr_b, g_mali.vsram_da_b,
	    g_mali.vsram_mon_a, g_mali.vsram_da_a, vsram_uv,
	    g_mali.vgpu_con0_b, g_mali.vgpu_dbg1_b, g_mali.vgpu_elr_b, g_mali.vgpu_da_b,
	    g_mali.vgpu_dbg1_a, g_mali.vgpu_da_a, vgpu_uv,
	    g_mali.spm_cfg_b, g_mali.xpu_b, g_mali.xpu2_b, g_mali.xpu_a, g_mali.xpu2_a,
	    g_mali.mfg_ok_mask, g_mali.prot1_a, g_mali.prot2_a,
	    g_mali.clk20_b, g_mali.clk20_a,
	    g_mali.pll_con0_b, g_mali.pll_con1_b, g_mali.pll_con3_b,
	    g_mali.pll_con0_a, g_mali.pll_con1_a, g_mali.pll_con3_a,
	    g_mali.cg_b, g_mali.cg_a,
	    g_mali.gpu_id, prod, arch_maj, rev_maj, rev_min, g_mali.reset_ok,
	    g_mali.l2_feat, g_mali.core_feat, g_mali.tiler_feat, g_mali.mem_feat,
	    g_mali.mmu_feat, g_mali.as_present, g_mali.js_present,
	    g_mali.max_threads, g_mali.max_wg, g_mali.thread_feat,
	    g_mali.tex0, g_mali.tex1, g_mali.tex2,
	    g_mali.l2_present_lo, g_mali.tiler_present_lo,
	    g_mali.shader_present_hi, g_mali.shader_present_lo,
	    g_mali.l2_ready_lo, g_mali.tiler_ready_lo, g_mali.shader_ready_lo);
	if (n < 0) {
		n = 0;
	}
	if ((size_t)n >= sizeof(buf)) {
		n = (int)sizeof(buf) - 1;
	}
	return SYSCTL_OUT(req, buf, (size_t)n + 1);
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, mali,
    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    0, 0, sysctl_hakios_mali, "A",
    "MediaTek MT6877 Mali-G68 MC4 bring-up status");

static void
mtk_mali_thread(__unused void *arg, __unused wait_result_t wr)
{
	/* Small settle delay so early console / USB init runs unhindered */
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

	printf("mtk_mali: READY! Mali GPU_ID=0x%08x (v%u r%up%u) L2=0x%x TILER=0x%x SHADER=0x%x @ %u MHz\n",
	    g_mali.gpu_id, (g_mali.gpu_id >> 28) & 0xFu,
	    (g_mali.gpu_id >> 12) & 0xFu, (g_mali.gpu_id >> 4) & 0xFFu,
	    g_mali.l2_ready_lo, g_mali.tiler_ready_lo, g_mali.shader_ready_lo,
	    g_mali.target_mhz);

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

	int freq = 0;
	if (PE_parse_boot_argn("mali_freq", &freq, sizeof(freq)) && freq > 0) {
		g_mali.target_mhz = (uint32_t)freq;
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
		thread_deallocate(th);
	} else {
		g_mali.state = "thread_fail";
	}
}
