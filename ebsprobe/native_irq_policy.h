#ifndef NATIVE_IRQ_POLICY_H
#define NATIVE_IRQ_POLICY_H
#include <stdint.h>
/* v0.41a one-shot IRQ ownership contract. QEMU profile ONLY; the phone path is
 * refused before any write. native_snapshot_policy.h (quiet path) is unchanged.
 *
 * Owned resources, each recorded in NativeState.irq_mod BEFORE it is changed and
 * undone in reverse order before NtPreemptArm returns:
 *   virtual timer (CNTV_CTL/CVAL, XNU may already have armed it: saved, then restored), GICR_WAKER.ProcessorSleep, IGROUPR0 bit 27,
 *   IPRIORITYR byte 27, GICD_CTLR.EnableGrp1, ICC_PMR/ICC_IGRPEN1, PPI 27 enable.
 * Allowed MMIO writes are exactly the NT_GICD_*, NT_GICR_* and NT_SGI_* offsets below.
 * Exactly one ICC_IAR1 read and one ICC_EOIR1 write; EOImode must be 0, so no DIR.
 * IRQ is unmasked only inside NtIrqWindow; the handler returns with PSTATE.I set.
 * No scheduler, wakeup, AST or XNU interrupt code is called on this path.
 * The exit path still compares every inherited field with the snapshot. */
#define NT_IRQ_ID 27u
#define NT_IRQ_BIT (UINT32_C(1) << NT_IRQ_ID)
#define NT_IRQ_PRIORITY 0x80u
#define NT_IRQ_POLL_LIMIT 1000000u
#define NT_GICD_CTLR 0x0
#define NT_GICR_CTLR 0x0
#define NT_GICR_WAKER 0x14
#define NT_SGI_FRAME 0x10000
#define NT_SGI_IGROUPR0 0x80
#define NT_SGI_ISENABLER0 0x100
#define NT_SGI_ICENABLER0 0x180
#define NT_SGI_ISPENDR0 0x200
#define NT_SGI_ICPENDR0 0x280
#define NT_SGI_ISACTIVER0 0x300
#define NT_SGI_IPRIORITY27 (0x400 + NT_IRQ_ID)
#define NT_SGI_ICFGR1 0xc04
#define NT_SGI_IGRPMODR0 0xd00

enum {
 NT_IRQ_OWN_TIMER = 1, NT_IRQ_OWN_WAKER = 2, NT_IRQ_OWN_GROUP = 4, NT_IRQ_OWN_PRIORITY = 8,
 NT_IRQ_OWN_DGATE = 16, NT_IRQ_OWN_CPU = 32, NT_IRQ_OWN_ENABLE = 64
};
enum {
 NT_IRQ_E_REFUSED = 0x301, NT_IRQ_E_WAKER = 0x311, NT_IRQ_E_READBACK = 0x312,
 NT_IRQ_E_DGATE = 0x313, NT_IRQ_E_CPU = 0x314, NT_IRQ_E_ENABLE = 0x315, NT_IRQ_E_PENDING = 0x316,
 NT_IRQ_E_TIMEOUT = 0x320, NT_IRQ_E_UNEXPECTED = 0x321
};
/* Refusal reasons; nothing has been written when any bit is set. */
enum {
 NT_IRQ_P_PROFILE = 1, NT_IRQ_P_BUSY = 2, NT_IRQ_P_APR = 4, NT_IRQ_P_CPU = 8,
 NT_IRQ_P_DIST = 16, NT_IRQ_P_PPI = 32, NT_IRQ_P_CONFIG = 64, NT_IRQ_P_TIMER = 128,
 NT_IRQ_P_EOIMODE = 256, NT_IRQ_P_FREQ = 512, NT_IRQ_P_WAKER = 1024, NT_IRQ_P_DAIF = 2048
};
typedef struct {
 uint64_t qemu, daif, rpr, ap1r0, igrpen, iccctl, timer, frequency;
 uint32_t ctlr, waker, modifier, enable, pending, active, config;
} NtIrqView;
static inline uint64_t NtIrqPolicy(const NtIrqView *v) {
 uint64_t f = 0;
 if (!v->qemu) f |= NT_IRQ_P_PROFILE;
 if (v->rpr != 0xff) f |= NT_IRQ_P_BUSY;            /* inherited active priority */
 if (v->ap1r0) f |= NT_IRQ_P_APR;
 if (v->igrpen) f |= NT_IRQ_P_CPU;                   /* Group 1 owned elsewhere */
 /* Single security state (ARE+DS), no RWP, Group 1 still closed by firmware. */
 if ((v->ctlr & 0x50) != 0x50 || (v->ctlr & UINT32_C(0x80000002))) f |= NT_IRQ_P_DIST;
 /* XNU arms CNTV itself; once its deadline passes, the level line keeps PPI 27 pending.
  * That pending state is explained by the timer we take over (masking it drops the line);
  * any other PPI 27 state belongs to someone else. */
 int firing = (v->timer & 7) == 5;
 if (((v->enable | v->active | v->modifier) & NT_IRQ_BIT) || ((v->pending & NT_IRQ_BIT) && !firing)) f |= NT_IRQ_P_PPI;
 if ((v->config >> ((NT_IRQ_ID - 16) * 2)) & 3) f |= NT_IRQ_P_CONFIG; /* level only */
 /* An inherited armed or firing timer is saved, masked and restored, never refused:
  * whether its deadline passed is a race with TCG speed. NT_IRQ_P_TIMER is reserved. */
 if (v->iccctl & 2) f |= NT_IRQ_P_EOIMODE;
 if (v->frequency < 1000000 || v->frequency > 1000000000) f |= NT_IRQ_P_FREQ;
 if ((v->waker & 6) != 0 && (v->waker & 6) != 6) f |= NT_IRQ_P_WAKER;
 if (!(v->daif & 128)) f |= NT_IRQ_P_DAIF;
 return f;
}
#endif
