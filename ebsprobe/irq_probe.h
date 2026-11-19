/* SPDX-License-Identifier: MIT
 * Own-handler EL1/NS diagnostic, NOT an XNU scheduler/timer implementation.
 * Compile exactly one C/C++ TU with IRQ_PROBE_IMPLEMENTATION, and irq_vectors.S.
 * All TUs: -ffreestanding -fno-builtin -mgeneral-regs-only -ffixed-x18.
 */
#ifndef IRQ_PROBE_H
#define IRQ_PROBE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

#define IRQ_PROBE_VERSION 1u
#define IRQ_PROBE_TIMER_ID 27u
#define IRQ_PROBE_SHOTS 3u
#define IRQ_PROBE_PPI_BIT (UINT32_C(1) << IRQ_PROBE_TIMER_ID)
#define IRQ_PROBE_RUBY_D UINT64_C(0x0c000000)
#define IRQ_PROBE_RUBY_D_BYTES UINT64_C(0x40000)
#define IRQ_PROBE_RUBY_R UINT64_C(0x0c040000)
#define IRQ_PROBE_RUBY_R_BYTES UINT64_C(0x200000)
#define IRQ_PROBE_R_STRIDE UINT64_C(0x20000)

enum irq_probe_status {
    IRQ_PROBE_OK = 0, IRQ_E_ARGUMENT = -1, IRQ_E_BUSY = -2,
    IRQ_E_ENTRY = -3, IRQ_E_PIDR = -4, IRQ_E_D_GATE = -5,
    IRQ_E_SRE = -6, IRQ_E_CPU_GATE = -7, IRQ_E_REDIST = -8,
    IRQ_E_R_SLEEP = -9, IRQ_E_TIMER_OWNED = -10, IRQ_E_PPI_BUSY = -11,
    IRQ_E_GROUP = -12, IRQ_E_CONFIG = -13, IRQ_E_FREQ = -14,
    IRQ_E_CPU_ACTIVE = -15, IRQ_E_RWP = -16, IRQ_E_READBACK = -17,
    IRQ_E_TIMEOUT = -18, IRQ_E_UNEXPECTED = -19, IRQ_E_SPURIOUS = -20,
    IRQ_E_EXCEPTION = -21, IRQ_E_RESTORE = -22, IRQ_E_WHITELIST = -23
};
enum irq_probe_stage {
    IRQ_STAGE_ENTRY = 1, IRQ_STAGE_APERTURES, IRQ_STAGE_D_PIDR,
    IRQ_STAGE_D_GATE, IRQ_STAGE_SRE, IRQ_STAGE_CPU_GATE,
    IRQ_STAGE_REDIST, IRQ_STAGE_R_STATE, IRQ_STAGE_TIMER_OWNER,
    IRQ_STAGE_PPI_OWNER, IRQ_STAGE_GROUP, IRQ_STAGE_CONFIG,
    IRQ_STAGE_COUNTER, IRQ_STAGE_CPU_IDLE, IRQ_STAGE_SETUP,
    IRQ_STAGE_ARM, IRQ_STAGE_WAIT, IRQ_STAGE_IRQ, IRQ_STAGE_DONE,
    IRQ_STAGE_EXCEPTION
};
enum irq_probe_modified {
    IRQ_MOD_VBAR = 1u, IRQ_MOD_TIMER = 2u, IRQ_MOD_PPI = 4u,
    IRQ_MOD_PRIORITY = 8u, IRQ_MOD_PMR = 16u,
    IRQ_MOD_D_GATE = 32u, IRQ_MOD_GROUP1 = 64u
};

struct irq_report {
    uint32_t version, bytes;
    volatile int32_t status;
    volatile uint32_t stage;
    volatile uint32_t irq_count, unexpected_count, spurious_count, exceptions;
    uint32_t timer_id, shots_requested, last_iar, last_id;
    uint32_t d_pidr2, r_pidr2, d_ctlr, d_ctlr_after, r_ctlr, r_waker;
    uint32_t group, config, enabled_before, pending_before, active_before;
    uint32_t priority_before, priority_used, ownership_proof, timeout_loop_bound;
    uint32_t pending_cleanup, active_cleanup, enabled_after, priority_after;
    uint32_t modified, restored, restore_error, fault_stage, failure_stage;
    uint32_t mmio_reads, mmio_writes, wait_loops, timeout_by_loops;
    uint64_t gicd, gicr_region, gicr_bytes, redist, mpidr;
    uint64_t entry_el, entry_sctlr, entry_daif, entry_spsel, vbar_before, vbar_after;
    uint64_t sre, icc_ctlr, igrpen1, igrpen1_before, pmr_before, pmr_used, pmr_after, bpr1, rpr;
    uint64_t cntfrq, cntpct_begin, cntpct_end, cntvct_begin, counter_deadline;
    uint64_t cntv_ctl_before, cntv_cval_before, cntp_ctl_observed;
    uint64_t cntv_ctl_after, cntv_cval_after;
    uint64_t programmed_cval[IRQ_PROBE_SHOTS];
    uint64_t irq_cntpct[IRQ_PROBE_SHOTS], irq_cntvct[IRQ_PROBE_SHOTS];
    uint64_t irq_timer_ctl[IRQ_PROBE_SHOTS];
    uint64_t esr, far, elr, spsr;
};

/* All GPRs, including x18, are saved by our vector. No FP/SIMD is used. */
struct irq_probe_frame { uint64_t x[31], elr, spsr, reserved; };
#ifdef __cplusplus
static_assert(sizeof(struct irq_probe_frame) == 272 && offsetof(struct irq_probe_frame, elr) == 248 &&
              offsetof(struct irq_probe_frame, spsr) == 256, "vector frame ABI");
#else
_Static_assert(sizeof(struct irq_probe_frame) == 272 && offsetof(struct irq_probe_frame, elr) == 248 &&
               offsetof(struct irq_probe_frame, spsr) == 256, "vector frame ABI");
#endif
int IrqProbeRun(struct irq_report *, uint64_t gicd, uint64_t gicr, uint64_t bytes);
extern const unsigned char IrqProbeVectors[];

#ifdef IRQ_PROBE_IMPLEMENTATION
#if !defined(__aarch64__)
#error AArch64 only
#endif
#define IRQ_READ(reg) ({ uint64_t _x; __asm__ volatile("mrs %0, " #reg : "=r"(_x) :: "memory"); _x; })
#define IRQ_WRITE(reg, val) do { uint64_t _x = (uint64_t)(val); __asm__ volatile("msr " #reg ", %0" :: "r"(_x) : "memory"); } while (0)
#define IRQ_SYNC() __asm__ volatile("dsb sy; isb" ::: "memory")
#define IRQ_MASK() __asm__ volatile("msr daifset, #15; isb" ::: "memory")
#define IRQ_UNMASK_I() __asm__ volatile("msr daifclr, #2; isb" ::: "memory")
#define IRQ_MAX_LOOPS UINT32_C(8000000)
#define IRQ_MIN_FREQ UINT64_C(1000000)
#define IRQ_MAX_FREQ UINT64_C(1000000000)

/* Test seams cannot be enabled accidentally in a phone build. */
#if defined(IRQ_PROBE_TEST_HOOKS) && !defined(IRQ_PROBE_QEMU)
#error Test hooks are forbidden in the phone build
#endif
#ifdef IRQ_PROBE_TEST_HOOKS
extern uint64_t IrqProbeTestObservation(unsigned which, uint64_t actual);
extern void IrqProbeTestArmHook(void);
#define IRQ_OBSERVE(which, actual) IrqProbeTestObservation(which, actual)
#define IRQ_ARM_HOOK() IrqProbeTestArmHook()
#else
#define IRQ_OBSERVE(which, actual) (actual)
#define IRQ_ARM_HOOK() ((void)0)
#endif

struct irq_probe_live {
    volatile struct irq_report *r;
    uint64_t recovery_sp, recovery_pc, sgibase;
    uint32_t flags, cleanup_step;
    volatile uint32_t armed, stop;
};
static struct irq_probe_live irq_live;

static void irq_error(int code) {
    if (irq_live.r && irq_live.r->status == 0) {
        irq_live.r->failure_stage = irq_live.r->stage;
        irq_live.r->status = code;
    }
}
static uint32_t irq_rd32(uint64_t a) {
    irq_live.r->mmio_reads++;
    return *(volatile uint32_t *)(uintptr_t)a;
}
static uint64_t irq_rd64(uint64_t a) {
    irq_live.r->mmio_reads++;
    return *(volatile uint64_t *)(uintptr_t)a;
}
static uint8_t irq_rd8(uint64_t a) {
    irq_live.r->mmio_reads++;
    return *(volatile uint8_t *)(uintptr_t)a;
}
/* MMIO writes are narrowly bounded: one GICD Group1 gate bit, the ICC
 * Group1 gate, W1S/W1C of owned PPI bit 27, or its priority byte.
 * No DS/ARE/config/group/pending/active writes and no other source RMW. */
static void irq_ppi_write(uint32_t offset) {
    if (!irq_live.sgibase || (offset != 0x100 && offset != 0x180 && offset != 0x280)) {
        irq_error(IRQ_E_WHITELIST); return;
    }
    irq_live.r->mmio_writes++;
    *(volatile uint32_t *)(uintptr_t)(irq_live.sgibase + offset) = IRQ_PROBE_PPI_BIT;
}
static void irq_priority_write(uint8_t value) {
    if (!irq_live.sgibase) { irq_error(IRQ_E_WHITELIST); return; }
    irq_live.r->mmio_writes++;
    *(volatile uint8_t *)(uintptr_t)(irq_live.sgibase + 0x400 + IRQ_PROBE_TIMER_ID) = value;
}
static int irq_apertures(uint64_t d, uint64_t r, uint64_t n) {
    /* Reusable primitive supports two explicitly bounded register maps.
     * Production caller is RubyRunIrq: it accepts ONLY the live ruby FDT map.
     * The second map lets a separate QEMU harness execute these exact PE bytes,
     * without recompiling the device implementation or patching its instructions. */
    return (d == IRQ_PROBE_RUBY_D && r == IRQ_PROBE_RUBY_R && n == IRQ_PROBE_RUBY_R_BYTES) ||
           (d == UINT64_C(0x08000000) && r == UINT64_C(0x080a0000) && n == UINT64_C(0x20000));
}
static int irq_arch(uint32_t pidr) { unsigned a = (pidr >> 4) & 15; return a == 3 || a == 4; }
static int irq_rwp_clear(void) {
    uint64_t start = IRQ_READ(cntpct_el0);
    for (uint32_t i = 0; i < IRQ_MAX_LOOPS; ++i) {
        if (!(irq_rd32(irq_live.r->redist) & 8u)) return 1;
        if (IRQ_READ(cntpct_el0) - start > irq_live.r->cntfrq / 5) break;
    }
    return 0;
}
static int irq_d_rwp_clear(void) {
    uint64_t start = IRQ_READ(cntpct_el0);
    for (uint32_t i = 0; i < IRQ_MAX_LOOPS; ++i) {
        if (!(irq_rd32(irq_live.r->gicd) & UINT32_C(0x80000000))) return 1;
        if (IRQ_READ(cntpct_el0) - start > irq_live.r->cntfrq / 5) break;
    }
    return 0;
}

/* Called with DAIF masked. Flag is cleared BEFORE each restore step, so a
 * synchronous trap during restoration cannot create an unbounded retry. */
static int irq_cleanup(void) {
    volatile struct irq_report *r = irq_live.r;
    irq_live.cleanup_step = 1;
    IRQ_MASK();
    irq_live.armed = 0;
    if (irq_live.flags & IRQ_MOD_TIMER) {
        irq_live.flags &= ~IRQ_MOD_TIMER;
        IRQ_WRITE(cntv_ctl_el0, 2); IRQ_SYNC();
        IRQ_WRITE(cntv_cval_el0, r->cntv_cval_before);
        IRQ_WRITE(cntv_ctl_el0, r->cntv_ctl_before & 3); IRQ_SYNC();
        r->cntv_ctl_after = IRQ_READ(cntv_ctl_el0);
        r->cntv_cval_after = IRQ_READ(cntv_cval_el0);
        if ((r->cntv_ctl_after & 3) != (r->cntv_ctl_before & 3) || r->cntv_cval_after != r->cntv_cval_before)
            r->restore_error |= IRQ_MOD_TIMER;
        else r->restored |= IRQ_MOD_TIMER;
    }
    if (irq_live.flags & IRQ_MOD_PPI) {
        irq_live.flags &= ~IRQ_MOD_PPI;
        irq_ppi_write(0x180); IRQ_SYNC();
        if (!irq_rwp_clear()) r->restore_error |= IRQ_MOD_PPI;
        r->pending_cleanup = irq_rd32(irq_live.sgibase + 0x200) & IRQ_PROBE_PPI_BIT;
        r->active_cleanup = irq_rd32(irq_live.sgibase + 0x300) & IRQ_PROBE_PPI_BIT;
        /* Initially not pending/active, owned timer is now deasserted. Clear
         * ONLY our pending bit, never another source and never an active bit. */
        irq_ppi_write(0x280); IRQ_SYNC();
        if (r->active_cleanup || (irq_rd32(irq_live.sgibase + 0x200) & IRQ_PROBE_PPI_BIT))
            r->restore_error |= IRQ_MOD_PPI;
        if (irq_live.flags & IRQ_MOD_PRIORITY) {
            irq_live.flags &= ~IRQ_MOD_PRIORITY;
            irq_priority_write((uint8_t)r->priority_before); IRQ_SYNC();
            r->priority_after = irq_rd8(irq_live.sgibase + 0x400 + IRQ_PROBE_TIMER_ID);
            if (r->priority_after != r->priority_before) r->restore_error |= IRQ_MOD_PRIORITY;
            else r->restored |= IRQ_MOD_PRIORITY;
        }
        if (r->enabled_before & IRQ_PROBE_PPI_BIT) irq_ppi_write(0x100);
        IRQ_SYNC();
        if (!irq_rwp_clear()) r->restore_error |= IRQ_MOD_PPI;
        r->enabled_after = irq_rd32(irq_live.sgibase + 0x100) & IRQ_PROBE_PPI_BIT;
        if (r->enabled_after != (r->enabled_before & IRQ_PROBE_PPI_BIT)) r->restore_error |= IRQ_MOD_PPI;
        if (!(r->restore_error & IRQ_MOD_PPI)) r->restored |= IRQ_MOD_PPI;
    }
    /* A fault partway through the PPI step can still restore its priority. */
    if (irq_live.flags & IRQ_MOD_PRIORITY) {
        irq_live.flags &= ~IRQ_MOD_PRIORITY;
        irq_priority_write((uint8_t)r->priority_before); IRQ_SYNC();
        r->priority_after = irq_rd8(irq_live.sgibase + 0x400 + IRQ_PROBE_TIMER_ID);
        if (r->priority_after != r->priority_before) r->restore_error |= IRQ_MOD_PRIORITY;
        else r->restored |= IRQ_MOD_PRIORITY;
    }
    if (irq_live.flags & IRQ_MOD_PMR) {
        irq_live.flags &= ~IRQ_MOD_PMR;
        IRQ_WRITE(icc_pmr_el1, r->pmr_before); IRQ_SYNC();
        r->pmr_after = IRQ_READ(icc_pmr_el1);
        if (r->pmr_after != r->pmr_before) r->restore_error |= IRQ_MOD_PMR;
        else r->restored |= IRQ_MOD_PMR;
    }
    if (irq_live.flags & IRQ_MOD_GROUP1) {
        irq_live.flags &= ~IRQ_MOD_GROUP1;
        IRQ_WRITE(icc_igrpen1_el1, r->igrpen1_before & 1u); IRQ_SYNC();
        uint64_t after = IRQ_READ(icc_igrpen1_el1);
        if ((after & 1u) != (r->igrpen1_before & 1u)) r->restore_error |= IRQ_MOD_GROUP1;
        else r->restored |= IRQ_MOD_GROUP1;
    }
    if (irq_live.flags & IRQ_MOD_D_GATE) {
        irq_live.flags &= ~IRQ_MOD_D_GATE;
        *(volatile uint32_t *)(uintptr_t)r->gicd = r->d_ctlr; IRQ_SYNC();
        if (!irq_d_rwp_clear()) r->restore_error |= IRQ_MOD_D_GATE;
        r->d_ctlr_after = irq_rd32(r->gicd);
        if ((r->d_ctlr_after & 0x12u) != (r->d_ctlr & 0x12u)) r->restore_error |= IRQ_MOD_D_GATE;
        else r->restored |= IRQ_MOD_D_GATE;
    }
    if (irq_live.flags & IRQ_MOD_VBAR) {
        irq_live.flags &= ~IRQ_MOD_VBAR;
        IRQ_WRITE(vbar_el1, r->vbar_before); IRQ_SYNC();
        r->vbar_after = IRQ_READ(vbar_el1);
        if (r->vbar_after != r->vbar_before) r->restore_error |= IRQ_MOD_VBAR;
        else r->restored |= IRQ_MOD_VBAR;
    }
    if (r->restore_error) irq_error(IRQ_E_RESTORE);
    int result = r->status;
    uint64_t daif = r->entry_daif;
    irq_live.r = 0;
    IRQ_WRITE(daif, daif); __asm__ volatile("isb" ::: "memory");
    return result;
}

/* C helpers called by irq_vectors.S, same names/ABI on ELF and ARM64 COFF. */
int IrqProbeFaultCleanup(void) { return irq_cleanup(); }
uint64_t IrqProbeHandleFault(struct irq_probe_frame *f) {
    volatile struct irq_report *r = irq_live.r;
    if (!r) return 0; /* unreachable while our VBAR is installed */
    r->exceptions++;
    if (!r->fault_stage) r->fault_stage = r->stage;
    r->stage = IRQ_STAGE_EXCEPTION;
    r->status = IRQ_E_EXCEPTION;
    r->esr = IRQ_READ(esr_el1); r->far = IRQ_READ(far_el1);
    r->elr = f->elr; r->spsr = f->spsr;
    /* Never acknowledge an interrupt from a synchronous/FIQ/SError vector. */
    irq_live.stop = 1;
    if (irq_live.cleanup_step) r->restore_error |= r->modified & ~r->restored;
    f->elr = irq_live.recovery_pc;
    f->spsr |= UINT64_C(0x3c0);
    return irq_live.recovery_sp;
}
void IrqProbeHandleIrq(struct irq_probe_frame *f) {
    volatile struct irq_report *r = irq_live.r;
    if (!r) { f->spsr |= 0x80; return; }
    uint64_t iar = IRQ_READ(icc_iar1_el1);
    uint32_t id = (uint32_t)iar & 0xffffffu;
    r->last_iar = (uint32_t)iar; r->last_id = id;
    if (id >= 1020 && id <= 1023) {
        /* Special/spurious IDs MUST NOT be EOI'd or deactivated. */
        r->spurious_count++;
        if (r->spurious_count > 8) {
            irq_error(IRQ_E_SPURIOUS); irq_live.stop = 1; f->spsr |= 0x80;
        }
        return;
    }
    uint64_t ctl = IRQ_READ(cntv_ctl_el0);
    uint64_t p = IRQ_READ(cntpct_el0), v = IRQ_READ(cntvct_el0);
    /* Mask and disable ONLY the owned timer, deassert before EOI. */
    IRQ_WRITE(cntv_ctl_el0, 2); IRQ_SYNC();
    unsigned shot = r->irq_count;
    if (id == IRQ_PROBE_TIMER_ID && irq_live.armed && (ctl & 5) == 5 && shot < IRQ_PROBE_SHOTS) {
        r->irq_cntpct[shot] = p; r->irq_cntvct[shot] = v; r->irq_timer_ctl[shot] = ctl;
        irq_live.armed = 0;
        r->irq_count = shot + 1;
    } else {
        r->unexpected_count++; irq_error(IRQ_E_UNEXPECTED);
        irq_live.armed = 0; irq_live.stop = 1;
        /* Stop a possible storm on return, without masking/disabling another
         * device, PPI, SPI, distributor, or the Group1 gate. */
        f->spsr |= 0x80;
    }
    IRQ_WRITE(icc_eoir1_el1, iar); __asm__ volatile("isb" ::: "memory");
    if (r->icc_ctlr & 2) { IRQ_WRITE(icc_dir_el1, iar); __asm__ volatile("isb" ::: "memory"); }
}

/* Wrapper passes its own preserved-register SP and an exception-return label.
 * Do NOT call this internal entry directly. */
int IrqProbeRunCore(struct irq_report *out, uint64_t d, uint64_t rr, uint64_t bytes,
                    uint64_t recovery_sp, uint64_t recovery_pc) {
    if (!out || ((uintptr_t)out & 7)) return IRQ_E_ARGUMENT;
    if (irq_live.r) return IRQ_E_BUSY;
    volatile unsigned char *z = (volatile unsigned char *)out;
    for (size_t i = 0; i < sizeof(*out); ++i) z[i] = 0;
    volatile struct irq_report *r = out;
    r->version = IRQ_PROBE_VERSION; r->bytes = sizeof(*out);
    r->stage = IRQ_STAGE_ENTRY; r->timer_id = IRQ_PROBE_TIMER_ID;
    r->shots_requested = IRQ_PROBE_SHOTS; r->timeout_loop_bound = IRQ_MAX_LOOPS;
    r->gicd = d; r->gicr_region = rr; r->gicr_bytes = bytes;
    r->entry_el = IRQ_READ(CurrentEL);
    if (r->entry_el != 4) { r->status = IRQ_E_ENTRY; return IRQ_E_ENTRY; }
    r->entry_daif = IRQ_READ(daif); r->entry_sctlr = IRQ_READ(sctlr_el1); r->entry_spsel = IRQ_READ(spsel);
    if ((r->entry_daif & 0x3c0) != 0x3c0 || (r->entry_sctlr & 0x1005) || r->entry_spsel != 1) {
        r->status = IRQ_E_ENTRY; return IRQ_E_ENTRY;
    }
    r->stage = IRQ_STAGE_APERTURES;
    if (!irq_apertures(d, rr, bytes)) { r->status = IRQ_E_ARGUMENT; return IRQ_E_ARGUMENT; }
    irq_live.r = r; irq_live.recovery_sp = recovery_sp; irq_live.recovery_pc = recovery_pc;
    irq_live.sgibase = 0; irq_live.flags = 0; irq_live.cleanup_step = 0; irq_live.armed = 0; irq_live.stop = 0;
    r->vbar_before = IRQ_READ(vbar_el1);
    irq_live.flags |= IRQ_MOD_VBAR; r->modified |= IRQ_MOD_VBAR;
    IRQ_WRITE(vbar_el1, (uintptr_t)IrqProbeVectors); IRQ_SYNC();
#define IRQ_GUARD(test, code) do { if (!(test)) { irq_error(code); goto finished; } } while (0)
    r->stage = IRQ_STAGE_D_PIDR;
    r->d_pidr2 = (uint32_t)IRQ_OBSERVE(1, irq_rd32(d + 0xffe8));
    IRQ_GUARD(irq_arch(r->d_pidr2), IRQ_E_PIDR);
    r->stage = IRQ_STAGE_D_GATE;
    r->d_ctlr = irq_rd32(d);
    /* The live phone showed ARE_NS=1 with Group 1 closed (0x10). Open
     * only EnableGrp1A/Group1 (bit 1), never ARE, DS, or reserved bits. */
    IRQ_GUARD(!(r->d_ctlr & 0x80000000u) && (r->d_ctlr & 0x10u), IRQ_E_D_GATE);
    if (!(r->d_ctlr & 0x2u)) {
         irq_live.flags |= IRQ_MOD_D_GATE; r->modified |= IRQ_MOD_D_GATE;
    *(volatile uint32_t *)(uintptr_t)d = r->d_ctlr | 0x2u; IRQ_SYNC();
    IRQ_GUARD(irq_d_rwp_clear(), IRQ_E_RWP);
    r->d_ctlr_after = irq_rd32(d);
    IRQ_GUARD((r->d_ctlr_after & 0x12u) == 0x12u, IRQ_E_READBACK);
     } else {
         r->d_ctlr_after = r->d_ctlr;
     }
    r->stage = IRQ_STAGE_SRE;
    r->sre = IRQ_OBSERVE(2, IRQ_READ(icc_sre_el1));
    IRQ_GUARD(r->sre & 1, IRQ_E_SRE);
    r->stage = IRQ_STAGE_CPU_GATE;
    r->igrpen1_before = IRQ_READ(icc_igrpen1_el1);
    r->igrpen1 = r->igrpen1_before;
    if (!(r->igrpen1_before & 1u)) {
        irq_live.flags |= IRQ_MOD_GROUP1; r->modified |= IRQ_MOD_GROUP1;
        IRQ_WRITE(icc_igrpen1_el1, 1); IRQ_SYNC();
        r->igrpen1 = IRQ_READ(icc_igrpen1_el1);
    }
    IRQ_GUARD(r->igrpen1 & 1u, IRQ_E_CPU_GATE);
    r->icc_ctlr = IRQ_READ(icc_ctlr_el1); r->pmr_before = IRQ_READ(icc_pmr_el1);
    r->bpr1 = IRQ_READ(icc_bpr1_el1);
    r->stage = IRQ_STAGE_REDIST;
    r->mpidr = IRQ_READ(mpidr_el1);
    uint32_t affinity = (uint32_t)(r->mpidr & 0xffffff) | (uint32_t)((r->mpidr >> 8) & 0xff000000);
    for (uint64_t off = 0; off + IRQ_PROBE_R_STRIDE <= bytes; off += IRQ_PROBE_R_STRIDE) {
        uint64_t typer = irq_rd64(rr + off + 8);
        IRQ_GUARD(!(typer & 2), IRQ_E_REDIST); /* no guessed VLPIS stride */
        if ((uint32_t)(typer >> 32) == affinity) { r->redist = rr + off; break; }
        if (typer & 16) break;
    }
    IRQ_GUARD(r->redist, IRQ_E_REDIST);
    irq_live.sgibase = r->redist + 0x10000;
    r->r_pidr2 = irq_rd32(r->redist + 0xffe8);
    IRQ_GUARD(irq_arch(r->r_pidr2), IRQ_E_PIDR);
    r->stage = IRQ_STAGE_R_STATE;
    r->r_ctlr = irq_rd32(r->redist); r->r_waker = irq_rd32(r->redist + 0x14);
    IRQ_GUARD(!(r->r_waker & 6), IRQ_E_R_SLEEP);
    IRQ_GUARD(!(r->r_ctlr & 8), IRQ_E_RWP);
    r->stage = IRQ_STAGE_TIMER_OWNER;
    r->cntv_ctl_before = IRQ_READ(cntv_ctl_el0); r->cntv_cval_before = IRQ_READ(cntv_cval_el0);
    r->cntp_ctl_observed = IRQ_READ(cntp_ctl_el0);
    IRQ_GUARD(!(r->cntv_ctl_before & 1), IRQ_E_TIMER_OWNED); /* masked+enabled is owned too */
    r->stage = IRQ_STAGE_PPI_OWNER;
    r->enabled_before = irq_rd32(irq_live.sgibase + 0x100);
    r->pending_before = irq_rd32(irq_live.sgibase + 0x200);
    r->active_before = irq_rd32(irq_live.sgibase + 0x300);
    IRQ_GUARD(!((r->pending_before | r->active_before) & IRQ_PROBE_PPI_BIT), IRQ_E_PPI_BUSY);
    r->stage = IRQ_STAGE_GROUP;
    r->group = irq_rd32(irq_live.sgibase + 0x80);
    r->priority_before = irq_rd8(irq_live.sgibase + 0x400 + IRQ_PROBE_TIMER_ID);
    if (r->d_ctlr & 0x40) {
        IRQ_GUARD(r->group & IRQ_PROBE_PPI_BIT, IRQ_E_GROUP);
        r->ownership_proof = 1; /* DS=1, readable Group1 */
    } else {
        /* IGROUPR is RAZ/WI in NS with DS=0; do not mistake RAZ for Group0.
         * Secure priority fields are ALSO RAZ/WI. A NONZERO priority read is
         * a read-only proof of NS Group1 access. Zero is ambiguous: fail closed.
         * We never change group, including when its NS value is unreadable. */
        IRQ_GUARD(r->priority_before != 0, IRQ_E_GROUP);
        r->ownership_proof = 2;
    }
    r->stage = IRQ_STAGE_CONFIG;
    r->config = irq_rd32(irq_live.sgibase + 0xc04);
    IRQ_GUARD(((r->config >> ((IRQ_PROBE_TIMER_ID - 16) * 2)) & 3) == 0, IRQ_E_CONFIG);
    r->stage = IRQ_STAGE_COUNTER;
    r->cntfrq = IRQ_READ(cntfrq_el0);
    IRQ_GUARD(r->cntfrq >= IRQ_MIN_FREQ && r->cntfrq <= IRQ_MAX_FREQ, IRQ_E_FREQ);
    r->cntpct_begin = IRQ_READ(cntpct_el0); r->cntvct_begin = IRQ_READ(cntvct_el0);
    r->stage = IRQ_STAGE_CPU_IDLE;
    r->rpr = IRQ_READ(icc_rpr_el1);
    IRQ_GUARD(r->rpr == 0xff, IRQ_E_CPU_ACTIVE);
    r->stage = IRQ_STAGE_SETUP;
    irq_live.flags |= IRQ_MOD_TIMER; r->modified |= IRQ_MOD_TIMER;
    IRQ_WRITE(cntv_ctl_el0, 2); IRQ_SYNC();
    irq_live.flags |= IRQ_MOD_PPI; r->modified |= IRQ_MOD_PPI;
    irq_ppi_write(0x180); IRQ_SYNC();
    IRQ_GUARD(irq_rwp_clear(), IRQ_E_RWP);
    irq_live.flags |= IRQ_MOD_PRIORITY; r->modified |= IRQ_MOD_PRIORITY;
    irq_priority_write(0x80); IRQ_SYNC();
    r->priority_used = irq_rd8(irq_live.sgibase + 0x400 + IRQ_PROBE_TIMER_ID);
    IRQ_GUARD(r->priority_used == 0x80, IRQ_E_READBACK);
    irq_live.flags |= IRQ_MOD_PMR; r->modified |= IRQ_MOD_PMR;
    IRQ_WRITE(icc_pmr_el1, 0xff); IRQ_SYNC(); r->pmr_used = IRQ_READ(icc_pmr_el1);
    IRQ_GUARD(r->pmr_used > r->priority_used, IRQ_E_READBACK);
    irq_ppi_write(0x100); IRQ_SYNC();
    IRQ_GUARD(irq_rwp_clear(), IRQ_E_RWP);
    IRQ_GUARD(irq_rd32(irq_live.sgibase + 0x100) & IRQ_PROBE_PPI_BIT, IRQ_E_READBACK);
    for (unsigned shot = 0; shot < IRQ_PROBE_SHOTS && !irq_live.stop; ++shot) {
        r->stage = IRQ_STAGE_ARM;
        r->programmed_cval[shot] = IRQ_READ(cntvct_el0) + r->cntfrq / 1000; /* 1ms, not hardcoded Hz */
        IRQ_WRITE(cntv_cval_el0, r->programmed_cval[shot]);
        irq_live.armed = 1;
        r->counter_deadline = IRQ_READ(cntpct_el0) + r->cntfrq / 5;
        IRQ_WRITE(cntv_ctl_el0, 1); IRQ_SYNC();
        IRQ_ARM_HOOK();
        r->stage = IRQ_STAGE_WAIT;
        IRQ_UNMASK_I();
        uint32_t loops = 0;
        while (r->irq_count == shot && !irq_live.stop && loops < IRQ_MAX_LOOPS) {
            ++loops;
            if ((int64_t)(IRQ_READ(cntpct_el0) - r->counter_deadline) >= 0) break;
            __asm__ volatile("yield" ::: "memory");
        }
        __asm__ volatile("msr daifset, #2; isb" ::: "memory");
        r->wait_loops += loops;
        if (r->irq_count == shot && !irq_live.stop) {
            r->timeout_by_loops = loops >= IRQ_MAX_LOOPS;
            irq_error(IRQ_E_TIMEOUT); break;
        }
        if (r->status) break;
    }
    r->cntpct_end = IRQ_READ(cntpct_el0);
    if (!r->status && r->irq_count == IRQ_PROBE_SHOTS) r->stage = IRQ_STAGE_DONE;
finished:
    return irq_cleanup();
#undef IRQ_GUARD
}
#endif /* implementation */
#ifdef __cplusplus
}
#endif
#endif
