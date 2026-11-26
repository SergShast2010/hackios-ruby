/*
 * Physical buttons on Redmi Note 12 Pro 5G (ruby, MediaTek MT6877).
 * Hardware layout verified on live ruby (adapted from lky005/ruby-ios-bringup):
 *   - Volume Down : SoC keypad controller KPD (0x10010000) KP_MEM1 bit 0 (active low)
 *   - Power / Lock: PMIC MT6359 TOPSTATUS (0x002a) bit 1 PWRKEY_DEB (active low)
 *   - Volume Up   : PMIC MT6359 TOPSTATUS (0x002a) bit 3 HOMEKEY_DEB (active low)
 *
 * Read-only on PMIC: TOPSTATUS is polled via pwrap SWINF (0x10026000) without writing
 * any PMIC register. Two consecutive matching 20 ms samples debounce contact bounce.
 * Holding Volume Up + Volume Down for 1.5 s invokes loader_return() cleanly.
 *
 * Boot args: buttons=2 [keylog=1].
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern void loader_return(void);
extern void hakios_mtk_usb_ecm_log_stats(void) __attribute__((weak));

#define RK_KPD_PA          0x10010000UL
#define RK_KPD_MEM1        0x0004u

#define RK_PWRAP_PA        0x10026000UL
#define RK_MMIO_SIZE       0x1000UL
#define RK_PWRAP_ACC       0x0880u
#define RK_PWRAP_RDATA     0x0894u
#define RK_PWRAP_VLDCLR    0x08a4u
#define RK_PWRAP_STA       0x08a8u

#define RK_SWINF_FSM(v)    (((v) >> 1) & 7u)
#define RK_SWINF_IDLE      0u
#define RK_SWINF_VLDCLR    6u
#define RK_SWINF_INIT      (1u << 15)

#define RK_MT6359_TOPSTATUS  0x002au
#define RK_TOP_PWRKEY_DEB    (1u << 1)
#define RK_TOP_HOMEKEY_DEB   (1u << 3)

#define RK_POLL_MS         20u
#define RK_COMBO_TICKS     (1500u / RK_POLL_MS)
#define RK_HOME_TICKS      (700u / RK_POLL_MS)

struct rk_key {
	const char *name;
	uint32_t code;
	bool down;
	bool seen;
};

static struct rk_key rk_keys[3] = {
	{ "power",   HAKIOS_BUTTON_LOCK,        false, false },
	{ "volup",   HAKIOS_BUTTON_VOLUME_UP,   false, false },
	{ "voldown", HAKIOS_BUTTON_VOLUME_DOWN, false, false },
};

static vm_offset_t rk_kpd, rk_pwrap;
static bool rk_started;
static int rk_log;
static int rk_sleep_chan;
static unsigned rk_errors;
static unsigned rk_combo_ticks;
static unsigned rk_home_ticks;
static bool rk_home_down;
static bool rk_home_sent;

static inline uint32_t
rk_rd32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline uint16_t
rk_rd16(vm_offset_t base, uint32_t off)
{
	uint16_t v = *(volatile uint16_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
rk_wr32(vm_offset_t base, uint32_t off, uint32_t v)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rk_pwrap_wait(uint32_t fsm)
{
	for (uint32_t i = 0; i < 10000u; i++) {
		if (RK_SWINF_FSM(rk_rd32(rk_pwrap, RK_PWRAP_STA)) == fsm) {
			return true;
		}
	}
	return false;
}

static bool
rk_pmic_read(uint16_t reg, uint16_t *val)
{
	if (!rk_pwrap) {
		return false;
	}
	uint32_t sta = rk_rd32(rk_pwrap, RK_PWRAP_STA);
	if (!(sta & RK_SWINF_INIT)) {
		return false;
	}
	if (!rk_pwrap_wait(RK_SWINF_IDLE)) {
		return false;
	}
	rk_wr32(rk_pwrap, RK_PWRAP_ACC, reg);
	if (!rk_pwrap_wait(RK_SWINF_VLDCLR)) {
		return false;
	}
	uint32_t rd = rk_rd32(rk_pwrap, RK_PWRAP_RDATA);
	*val = (uint16_t)(rd & 0xffffu);
	rk_wr32(rk_pwrap, RK_PWRAP_VLDCLR, 1u);
	return true;
}

static void
rk_thread(__unused void *arg, __unused wait_result_t result)
{
	for (;;) {
		uint16_t mem1 = 0xffffu, top = 0xffffu;
		bool pmic_ok = rk_pmic_read(RK_MT6359_TOPSTATUS, &top);
		bool now[3];

		if (rk_kpd) {
			mem1 = rk_rd16(rk_kpd, RK_KPD_MEM1);
		}
		now[0] = pmic_ok ? !(top & RK_TOP_PWRKEY_DEB) : rk_keys[0].down;
		now[1] = pmic_ok ? !(top & RK_TOP_HOMEKEY_DEB) : rk_keys[1].down;
		now[2] = rk_kpd && !(mem1 & 1u);

		if (pmic_ok) {
			rk_errors = 0;
		} else if ((rk_errors++ % 250u) == 0u) {
			printf("mtk_buttons: PMIC read failed\n");
		}

		for (unsigned i = 0; i < 3; i++) {
			if (now[i] == rk_keys[i].seen && now[i] != rk_keys[i].down) {
				rk_keys[i].down = now[i];
				if (rk_log) {
					printf("mtk_buttons: %s %s (mem1=%04x top=%04x)\n",
					    rk_keys[i].name, now[i] ? "down" : "up", mem1, top);
				}
				hakios_touch_post(now[i] ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP,
				    rk_keys[i].code, 0, 0);
			}
			rk_keys[i].seen = now[i];
		}

		if (rk_keys[1].down && rk_keys[2].down) {
			rk_home_ticks = 0;
			if (++rk_combo_ticks == RK_COMBO_TICKS) {
				printf("mtk_buttons: VolUp+VolDown hold -> loader_return\n");
				if (hakios_mtk_usb_ecm_log_stats) hakios_mtk_usb_ecm_log_stats();
				loader_return();
				return;
			}
		} else {
			rk_combo_ticks = 0;
		}

		/* Hold VolDown alone for 0.7 s -> synthetic HOME tap (unlocks SpringBoard without touch). */
		if (rk_keys[2].down && !rk_keys[1].down && !rk_keys[0].down) {
			if (!rk_home_sent && ++rk_home_ticks >= RK_HOME_TICKS) {
				rk_home_sent = true;
				rk_home_down = true;
				printf("mtk_buttons: VolDown hold -> HOME\n");
				hakios_touch_post(HAKIOS_BUTTON_DOWN, HAKIOS_BUTTON_HOME, 0, 0);
			} else if (rk_home_down && ++rk_home_ticks >= RK_HOME_TICKS + 4u) {
				rk_home_down = false;
				hakios_touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_HOME, 0, 0);
			}
		} else {
			if (rk_home_down) {
				rk_home_down = false;
				hakios_touch_post(HAKIOS_BUTTON_UP, HAKIOS_BUTTON_HOME, 0, 0);
			}
			rk_home_ticks = 0;
			rk_home_sent = false;
		}

		uint64_t deadline;
		clock_interval_to_deadline(RK_POLL_MS, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&rk_sleep_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

void
hakios_mtk_buttons_init(void)
{
	int mode = 0;
	if (rk_started) {
		return;
	}
	if (!PE_parse_boot_argn("buttons", &mode, sizeof(mode)) || mode != 2) {
		return;
	}
	rk_started = true;
	PE_parse_boot_argn("keylog", &rk_log, sizeof(rk_log));

	rk_kpd = ml_io_map(RK_KPD_PA, RK_MMIO_SIZE);
	rk_pwrap = ml_io_map(RK_PWRAP_PA, RK_MMIO_SIZE);
	if (!rk_kpd && !rk_pwrap) {
		printf("mtk_buttons: MMIO map failed\n");
		return;
	}

	uint16_t top = 0xffffu;
	bool pmic_ok = rk_pmic_read(RK_MT6359_TOPSTATUS, &top);
	uint16_t mem1 = rk_kpd ? rk_rd16(rk_kpd, RK_KPD_MEM1) : 0xffffu;

	/* Seed initial state so already-held keys don't emit a bogus edge. */
	rk_keys[0].down = rk_keys[0].seen = pmic_ok ? !(top & RK_TOP_PWRKEY_DEB) : false;
	rk_keys[1].down = rk_keys[1].seen = pmic_ok ? !(top & RK_TOP_HOMEKEY_DEB) : false;
	rk_keys[2].down = rk_keys[2].seen = rk_kpd && !(mem1 & 1u);

	printf("mtk_buttons: ready (kpd=%d pwrap=%d top_ok=%d top=0x%04x mem1=0x%04x)\n",
	    rk_kpd != 0, rk_pwrap != 0, pmic_ok ? 1 : 0, top, mem1);

	thread_t th;
	if (kernel_thread_start(rk_thread, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		printf("mtk_buttons: thread start failed\n");
	}
}
