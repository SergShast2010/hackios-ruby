/*
 * Physical buttons on MediaTek MT6877 (Redmi Note 12 Pro 5G / ruby).
 *
 * The MediaTek keypad controller (kp@10010000) reflects hardware key state
 * directly in read-only memory registers KP_MEM1..KP_MEM5 (active-low bits).
 * From ruby's live DTB (kp@10010000):
 *   - KP_MEM1 bit 0  (key 0,  0x72 KEY_VOLUMEDOWN): Volume Down
 *   - KP_MEM1 bit 8  (key 8,  0x74 KEY_POWER):      Power / Lock
 *   - KP_MEM2 bit 1  (key 17, 0x73 KEY_VOLUMEUP):   Volume Up
 *
 * Activated by boot-arg buttons=2. Optional keylog=1 logs transitions and
 * any unexpected bit changes across KP_MEM1..KP_MEM3 for diagnostics.
 * Holding VolDown + VolUp for 1.5s triggers loader_return() cleanly.
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_return(void);

#define MTK_KPD_PHYS      0x10010000UL
#define MTK_KPD_SIZE      0x1000UL
#define KP_STA            0x0000
#define KP_MEM1           0x0004
#define KP_MEM2           0x0008
#define KP_MEM3           0x000c
#define KP_MEM4           0x0010
#define KP_MEM5           0x0014

#define KPD_POLL_MS       20
#define KPD_COMBO_TICKS   (1500 / KPD_POLL_MS)

/* ruby live-DTB key bits (active low in KP_MEM1..KP_MEM2) */
#define BIT_VOLDOWN       0u    /* KP_MEM1 bit 0  */
#define BIT_POWER         8u    /* KP_MEM1 bit 8  */
#define BIT_VOLUP         17u   /* KP_MEM2 bit 1  */

static volatile uint16_t *kpd_regs;
static thread_call_t      kpd_call;
static uint32_t           kpd_prev_bits;
static uint16_t           kpd_prev_mem3;
static int                kpd_log;
static unsigned           kpd_combo_ticks;

static inline uint16_t
kpd_rd16(unsigned off)
{
	uint16_t v = *(volatile uint16_t *)((uintptr_t)kpd_regs + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static void
kpd_schedule(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(KPD_POLL_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(kpd_call, deadline);
}

static void
kpd_report_mapped(unsigned bit, bool down)
{
	uint32_t btn = 0;
	const char *name = NULL;

	if (bit == BIT_VOLDOWN) {
		btn = HAKIOS_BUTTON_VOLUME_DOWN;
		name = "voldown";
	} else if (bit == BIT_POWER) {
		btn = HAKIOS_BUTTON_LOCK;
		name = "lock";
	} else if (bit == BIT_VOLUP) {
		btn = HAKIOS_BUTTON_VOLUME_UP;
		name = "volup";
	} else {
		if (kpd_log) {
			printf("mtk_buttons: raw bit %u %s\n", bit, down ? "down" : "up");
		}
		return;
	}

	if (kpd_log) {
		printf("mtk_buttons: %s %s\n", name, down ? "down" : "up");
	}
	hakios_touch_post(down ? HAKIOS_BUTTON_DOWN : HAKIOS_BUTTON_UP, btn, 0, 0);
}

static void
kpd_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint16_t m1 = kpd_rd16(KP_MEM1);
	uint16_t m2 = kpd_rd16(KP_MEM2);
	uint16_t m3 = kpd_rd16(KP_MEM3);
	/* Active low: invert so 1 = pressed */
	uint32_t now = (~((uint32_t)m1 | ((uint32_t)m2 << 16))) & 0xffffffffu;
	uint32_t changed = now ^ kpd_prev_bits;

	if (changed) {
		for (unsigned b = 0; b < 32; b++) {
			if (changed & (1u << b)) {
				kpd_report_mapped(b, (now & (1u << b)) != 0);
			}
		}
		kpd_prev_bits = now;
	}

	if (m3 != kpd_prev_mem3) {
		if (kpd_log) {
			printf("mtk_buttons: mem3 0x%04x -> 0x%04x\n", kpd_prev_mem3, m3);
		}
		kpd_prev_mem3 = m3;
	}

	/* Clean exit combo: VolDown + VolUp held for 1.5s */
	if ((now & (1u << BIT_VOLDOWN)) && (now & (1u << BIT_VOLUP))) {
		if (++kpd_combo_ticks == KPD_COMBO_TICKS) {
			printf("mtk_buttons: VolDown+VolUp hold -> loader_return\n");
			loader_return();
			return;
		}
	} else {
		kpd_combo_ticks = 0;
	}

	kpd_schedule();
}

void
hakios_mtk_buttons_init(void)
{
	int mode = 0;
	if (!PE_parse_boot_argn("buttons", &mode, sizeof(mode)) || mode != 2) {
		return;
	}
	PE_parse_boot_argn("keylog", &kpd_log, sizeof(kpd_log));

	vm_offset_t va = ml_io_map(MTK_KPD_PHYS, MTK_KPD_SIZE);
	if (!va) {
		printf("mtk_buttons: ml_io_map(0x%lx) failed\n", MTK_KPD_PHYS);
		return;
	}
	kpd_regs = (volatile uint16_t *)va;

	uint16_t sta = kpd_rd16(KP_STA);
	uint16_t m1  = kpd_rd16(KP_MEM1);
	uint16_t m2  = kpd_rd16(KP_MEM2);
	uint16_t m3  = kpd_rd16(KP_MEM3);
	kpd_prev_bits = (~((uint32_t)m1 | ((uint32_t)m2 << 16))) & 0xffffffffu;
	kpd_prev_mem3 = m3;

	printf("mtk_buttons: ready (KPD @ 0x%lx sta=0x%04x mem1=0x%04x mem2=0x%04x mem3=0x%04x)\n",
	    MTK_KPD_PHYS, sta, m1, m2, m3);

	kpd_call = thread_call_allocate(kpd_poll, NULL);
	kpd_schedule();
}
