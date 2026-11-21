#ifndef RUBY_STAGE_H
#define RUBY_STAGE_H

#include <stdint.h>
#include <stddef.h>

/* v23: a bounded staging operation, NOT a loader that executes a kernel.
 * No allocation, firmware calls, relocation, cache/MMU changes or entry calls.
 * All wire integers are little-endian; no unaligned typed input accesses.
 *
 * Caller contracts: p/payload names n readable, stable bytes; plan names a
 * valid, aligned RubyStagePlan object; dest names dest_bytes writable RAM owned
 * exclusively by the caller. Buffers must not change concurrently. A C routine
 * cannot establish ownership, accessibility or the physical mapping of a pointer.
 * dest is a caller-provided view of RAM whose declared physical base MUST be
 * 0x63000000 (it need not have that numerical virtual address on a native host).
 * The source's boot-time physical window is 0x60000000, at most 16 MiB.
 *
 * seg[].file_offset is relative to the Mach-O, NOT the XNUP header.
 * seg[].dest_offset is vmaddr - (0xfffffff004000000 + 0x03000000).
 * Only nonempty segments occur in seg[]; unused slots are zero. vm_lo/vm_hi
 * describe the actual nonempty VM extent; span_bytes = vm_hi - vm_lo.
 * file_bytes/zero_bytes are sums over the staged segments. Holes, including the
 * leading 16 KiB of the real kernel's window, are not touched.
 *
 * A deliberately restricted Mach-O command whitelist is used. Metadata tables
 * and section relocations are range checked, not applied/interpreted as code.
 * This is NOT authentication or proof that the input is safe to execute.
 *
 * Return codes (small positive integers):
 *   0 success; 1 invalid argument/pointer extent; 2 malformed/non-kernel XNUP;
 *   3 Mach-O header; 4 command layout/count/unsupported command;
 *   5 segment layout/window/protection; 6 section layout/reserved fields;
 *   7 metadata tables; 8 thread/entry; 9 changed/forged plan;
 *  10 destination base/capacity; 11 source/destination overlap;
 *  12 read-back failure (RAM MAY ALREADY HAVE BEEN WRITTEN; no rollback).
 * PlanPayload leaves *plan unchanged on error. Copy performs ALL structural,
 * plan, pointer and destination checks before its first destination store.
 */
enum {
    RUBY_STAGE_OK = 0,
    RUBY_STAGE_ARGUMENT = 1,
    RUBY_STAGE_XNUP = 2,
    RUBY_STAGE_MACH = 3,
    RUBY_STAGE_COMMAND = 4,
    RUBY_STAGE_SEGMENT = 5,
    RUBY_STAGE_SECTION = 6,
    RUBY_STAGE_METADATA = 7,
    RUBY_STAGE_ENTRY = 8,
    RUBY_STAGE_PLAN = 9,
    RUBY_STAGE_DESTINATION = 10,
    RUBY_STAGE_OVERLAP = 11,
    RUBY_STAGE_VERIFY = 12
};

#define RUBY_STAGE_SOURCE_BASE UINT64_C(0x60000000)
#define RUBY_STAGE_SOURCE_MAX  UINT64_C(0x01000000)
#define RUBY_STAGE_DEST_BASE   UINT64_C(0x63000000)
#define RUBY_STAGE_DEST_MAX    UINT64_C(0x02000000)
#define RUBY_STAGE_LINK_BASE   UINT64_C(0xfffffff007000000)

/* Limits apply to the wire commands, including empty segments/sections. */
#define RUBY_STAGE_MAX_COMMANDS 128u
#define RUBY_STAGE_MAX_COMMAND_BYTES 65536u
#define RUBY_STAGE_MAX_SECTIONS 1024u
#define RUBY_STAGE_MAX_SECTIONS_PER_SEGMENT 256u

typedef struct ruby_stage_plan {
    uint64_t kernel_offset, kernel_bytes, vm_lo, vm_hi, entry_va, entry_pa;
    uint64_t span_bytes, segments, file_bytes, zero_bytes;
    struct {
        uint64_t dest_offset, file_offset, file_bytes, mem_bytes;
    } seg[32];
} RubyStagePlan;

typedef struct ruby_stage_segment_internal {
    uint64_t va, mem, off, file, command;
    uint32_t protection, sections;
} RubyStageSegmentInternal;

typedef struct ruby_stage_range_internal {
    uint64_t off, bytes;
} RubyStageRangeInternal;

static inline uint32_t RubyStageU32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t RubyStageU64(const unsigned char *p)
{
    return (uint64_t)RubyStageU32(p) |
           ((uint64_t)RubyStageU32(p + 4) << 32);
}

static inline int RubyStageRange(uint64_t off, uint64_t bytes, uint64_t limit)
{
    return off <= limit && bytes <= limit - off;
}

/* Also works without computing either potentially wrapping end address. */
static inline int RubyStageOverlaps(uint64_t a, uint64_t an,
                                    uint64_t b, uint64_t bn)
{
    if (an == 0 || bn == 0) return 0;
    return a <= b ? b - a < an : a - b < bn;
}

static inline int RubyStagePointer(const void *p, uint64_t bytes)
{
    if (p == NULL || bytes > (uint64_t)SIZE_MAX) return 0;
    return bytes <= (uint64_t)(UINTPTR_MAX - (uintptr_t)p);
}

static inline int RubyStageAllZero(const unsigned char *p, uint64_t bytes)
{
    uint64_t i;
    for (i = 0; i < bytes; ++i) if (p[(size_t)i] != 0) return 0;
    return 1;
}

static inline int RubyStageEqual16(const unsigned char *a,
                                   const unsigned char *b)
{
    unsigned int i;
    for (i = 0; i < 16; ++i) if (a[i] != b[i]) return 0;
    return 1;
}

static inline int RubyStageName(const unsigned char *p)
{
    unsigned int i;
    int ended = 0;
    if (p[0] == 0) return 0;
    for (i = 0; i < 16; ++i) {
        if (p[i] == 0) ended = 1;
        else if (ended) return 0;
    }
    return 1;
}

/* Fixed names as integer immediates: no external/global storage to read. */
static inline int RubyStageKernelName(const unsigned char *p)
{
    return RubyStageU64(p) == UINT64_C(0x00006c656e72656b) &&
           RubyStageU64(p + 8) == 0;
}

static inline int RubyStageLinkeditName(const unsigned char *p)
{
    return RubyStageU32(p) == UINT32_C(0x494c5f5f) &&
           RubyStageU32(p + 4) == UINT32_C(0x44454b4e) &&
           RubyStageU32(p + 8) == UINT32_C(0x00005449) &&
           RubyStageU32(p + 12) == 0;
}

/* Field-wise operations avoid padding-dependent comparisons and libc calls. */
static inline void RubyStagePlanClear(RubyStagePlan *p)
{
    unsigned int i;
    p->kernel_offset = 0; p->kernel_bytes = 0;
    p->vm_lo = 0; p->vm_hi = 0; p->entry_va = 0; p->entry_pa = 0;
    p->span_bytes = 0; p->segments = 0; p->file_bytes = 0; p->zero_bytes = 0;
    for (i = 0; i < 32; ++i) {
        p->seg[i].dest_offset = 0; p->seg[i].file_offset = 0;
        p->seg[i].file_bytes = 0; p->seg[i].mem_bytes = 0;
    }
}

static inline void RubyStagePlanAssign(RubyStagePlan *a, const RubyStagePlan *b)
{
    unsigned int i;
    a->kernel_offset = b->kernel_offset; a->kernel_bytes = b->kernel_bytes;
    a->vm_lo = b->vm_lo; a->vm_hi = b->vm_hi;
    a->entry_va = b->entry_va; a->entry_pa = b->entry_pa;
    a->span_bytes = b->span_bytes; a->segments = b->segments;
    a->file_bytes = b->file_bytes; a->zero_bytes = b->zero_bytes;
    for (i = 0; i < 32; ++i) {
        a->seg[i].dest_offset = b->seg[i].dest_offset;
        a->seg[i].file_offset = b->seg[i].file_offset;
        a->seg[i].file_bytes = b->seg[i].file_bytes;
        a->seg[i].mem_bytes = b->seg[i].mem_bytes;
    }
}

static inline int RubyStagePlanEqual(const RubyStagePlan *a,
                                     const RubyStagePlan *b)
{
    unsigned int i;
    if (a->kernel_offset != b->kernel_offset || a->kernel_bytes != b->kernel_bytes ||
        a->vm_lo != b->vm_lo || a->vm_hi != b->vm_hi ||
        a->entry_va != b->entry_va || a->entry_pa != b->entry_pa ||
        a->span_bytes != b->span_bytes || a->segments != b->segments ||
        a->file_bytes != b->file_bytes || a->zero_bytes != b->zero_bytes) return 0;
    for (i = 0; i < 32; ++i) {
        if (a->seg[i].dest_offset != b->seg[i].dest_offset ||
            a->seg[i].file_offset != b->seg[i].file_offset ||
            a->seg[i].file_bytes != b->seg[i].file_bytes ||
            a->seg[i].mem_bytes != b->seg[i].mem_bytes) return 0;
    }
    return 1;
}

static inline int RubyStageSections(const unsigned char *k, uint64_t bytes,
                                    const RubyStageSegmentInternal *g)
{
    uint32_t i, j;
    for (i = 0; i < g->sections; ++i) {
        const unsigned char *s = k + (size_t)(g->command + 72 + (uint64_t)i * 80);
        uint64_t va = RubyStageU64(s + 32), sz = RubyStageU64(s + 40);
        uint64_t off = RubyStageU32(s + 48), ro = RubyStageU32(s + 56);
        uint32_t al = RubyStageU32(s + 52), nr = RubyStageU32(s + 60);
        uint32_t flags = RubyStageU32(s + 64), type = flags & 255u;
        uint32_t r1 = RubyStageU32(s + 68), r2 = RubyStageU32(s + 72);
        int indirect = type == 6 || type == 7 || type == 8 || type == 16 || type == 20;
        int zero = type == 1 || type == 12 || type == 18;
        if (!RubyStageName(s) || !RubyStageEqual16(s + 16, k + (size_t)g->command + 8) ||
            va < g->va || !RubyStageRange(va - g->va, sz, g->mem) ||
            al > 31 || (va & ((UINT64_C(1) << al) - 1)) != 0 ||
            type > 21 || (flags & ~UINT32_C(0xfe0007ff)) != 0 ||
            RubyStageU32(s + 76) != 0 || (!indirect && r1 != 0) ||
            (type != 8 && r2 != 0) || (type == 8 && (r2 == 0 || sz % r2 != 0)))
            return RUBY_STAGE_SECTION;
        if (zero) {
            if (off != 0) return RUBY_STAGE_SECTION;
        } else {
            if (off < g->off || !RubyStageRange(off - g->off, sz, g->file) ||
                off - g->off != va - g->va || !RubyStageRange(off, sz, bytes) ||
                (sz != 0 && (off & ((UINT64_C(1) << al) - 1)) != 0))
                return RUBY_STAGE_SECTION;
        }
        if (!RubyStageRange(ro, (uint64_t)nr * 8, bytes) ||
            (nr != 0 && (ro & 3u) != 0)) return RUBY_STAGE_SECTION;
        for (j = 0; j < i; ++j) {
            const unsigned char *t = k + (size_t)(g->command + 72 + (uint64_t)j * 80);
            if (RubyStageEqual16(s, t) ||
                RubyStageOverlaps(va, sz, RubyStageU64(t + 32), RubyStageU64(t + 40)))
                return RUBY_STAGE_SECTION;
        }
    }
    return RUBY_STAGE_OK;
}

/* Nine nonempty ranges suffice: symbols, strings, six dysym tables, functions. */
static inline int RubyStageMetadataRange(uint64_t bytes, uint64_t lo, uint64_t len,
                                         uint64_t off, uint64_t count, uint64_t width,
                                         RubyStageRangeInternal *ranges,
                                         unsigned int *used)
{
    uint64_t size;
    unsigned int i;
    if (width == 0 || count > UINT64_MAX / width) return 0;
    size = count * width;
    if (!RubyStageRange(off, size, bytes)) return 0;
    if (size == 0) return 1;
    if (off < lo || !RubyStageRange(off - lo, size, len) || *used >= 9) return 0;
    for (i = 0; i < *used; ++i)
        if (RubyStageOverlaps(off, size, ranges[i].off, ranges[i].bytes)) return 0;
    ranges[*used].off = off; ranges[*used].bytes = size; ++*used;
    return 1;
}

static inline int RubyStageMach(const unsigned char *k, uint64_t bytes,
                                RubyStagePlan *plan)
{
    RubyStageSegmentInternal g[32];
    RubyStageRangeInternal ranges[9];
    uint64_t cursor, end, sym = 0, dys = 0, fun = 0, leoff = 0, lebytes = 0;
    uint32_t ncmd, cmds, i, j, ng = 0, total_sections = 0, seen = 0;
    uint32_t nsyms = 0, nindirect = 0;
    unsigned int used = 0;
    int header_mapped = 0, entry_found = 0;
    if (bytes < 32 || RubyStageU32(k) != UINT32_C(0xfeedfacf) ||
        RubyStageU32(k + 4) != UINT32_C(0x0100000c) || RubyStageU32(k + 8) != 0 ||
        RubyStageU32(k + 12) != 2 || RubyStageU32(k + 28) != 0 ||
        (RubyStageU32(k + 24) & ~UINT32_C(0x00200001)) != 0)
        return RUBY_STAGE_MACH;
    ncmd = RubyStageU32(k + 16); cmds = RubyStageU32(k + 20);
    if (ncmd == 0 || ncmd > RUBY_STAGE_MAX_COMMANDS ||
        cmds > RUBY_STAGE_MAX_COMMAND_BYTES || (cmds & 7u) != 0 ||
        (uint64_t)ncmd * 8 > cmds || !RubyStageRange(32, cmds, bytes))
        return RUBY_STAGE_COMMAND;
    end = 32 + (uint64_t)cmds; cursor = 32;
    for (i = 0; i < ncmd; ++i) {
        const unsigned char *c;
        uint32_t cmd, size, bit = 0;
        if (!RubyStageRange(cursor, 8, end)) return RUBY_STAGE_COMMAND;
        c = k + (size_t)cursor; cmd = RubyStageU32(c); size = RubyStageU32(c + 4);
        if (size < 8 || (size & 7u) != 0 || !RubyStageRange(cursor, size, end))
            return RUBY_STAGE_COMMAND;
        if (cmd == 0x19) { /* LC_SEGMENT_64 */
            uint64_t va, mem, off, file;
            uint32_t maxp, initp, ns, flags;
            int rc;
            if (size < 72 || ng == 32) return RUBY_STAGE_SEGMENT;
            va = RubyStageU64(c + 24); mem = RubyStageU64(c + 32);
            off = RubyStageU64(c + 40); file = RubyStageU64(c + 48);
            maxp = RubyStageU32(c + 56); initp = RubyStageU32(c + 60);
            ns = RubyStageU32(c + 64); flags = RubyStageU32(c + 68);
            if (ns > RUBY_STAGE_MAX_SECTIONS_PER_SEGMENT ||
                total_sections > RUBY_STAGE_MAX_SECTIONS - ns ||
                (uint64_t)size != 72 + (uint64_t)ns * 80)
                return RUBY_STAGE_COMMAND;
            total_sections += ns;
            if (!RubyStageName(c + 8) || file > mem || !RubyStageRange(off, file, bytes) ||
                (maxp & ~7u) != 0 || (initp & ~maxp) != 0 ||
                (maxp & 6u) == 6u || (flags & ~4u) != 0 ||
                (mem != 0 && ((initp & 1u) == 0 || va < RUBY_STAGE_LINK_BASE ||
                 !RubyStageRange(va - RUBY_STAGE_LINK_BASE, mem, RUBY_STAGE_DEST_MAX))))
                return RUBY_STAGE_SEGMENT;
            for (j = 0; j < ng; ++j) {
                if (RubyStageEqual16(c + 8, k + (size_t)g[j].command + 8) ||
                    RubyStageOverlaps(va, mem, g[j].va, g[j].mem) ||
                    RubyStageOverlaps(off, file, g[j].off, g[j].file))
                    return RUBY_STAGE_SEGMENT;
            }
            g[ng].va = va; g[ng].mem = mem; g[ng].off = off; g[ng].file = file;
            g[ng].command = cursor; g[ng].protection = initp; g[ng].sections = ns;
            rc = RubyStageSections(k, bytes, &g[ng]);
            if (rc != RUBY_STAGE_OK) return rc;
            ++ng;
            if (RubyStageLinkeditName(c + 8)) {
                leoff = off; lebytes = file;
            }
            if (mem != 0) {
                uint64_t ix = plan->segments;
                if (off == 0 && file >= end) header_mapped = 1;
                if (ix == 0 || va < plan->vm_lo) plan->vm_lo = va;
                if (ix == 0 || va + mem > plan->vm_hi) plan->vm_hi = va + mem;
                plan->seg[(size_t)ix].dest_offset = va - RUBY_STAGE_LINK_BASE;
                plan->seg[(size_t)ix].file_offset = off;
                plan->seg[(size_t)ix].file_bytes = file;
                plan->seg[(size_t)ix].mem_bytes = mem;
                plan->segments = ix + 1;
                /* Disjoint segments bounded by one 32 MiB window: sums cannot wrap. */
                plan->file_bytes += file; plan->zero_bytes += mem - file;
            }
        } else if (cmd == 2) { /* LC_SYMTAB */
            if (size != 24) return RUBY_STAGE_COMMAND;
            bit = 1u; sym = cursor;
        } else if (cmd == 0x0b) { /* LC_DYSYMTAB (64-bit module records) */
            if (size != 80) return RUBY_STAGE_COMMAND;
            bit = 2u; dys = cursor;
        } else if (cmd == 0x1b) { /* LC_UUID */
            if (size != 24) return RUBY_STAGE_COMMAND;
            bit = 4u;
        } else if (cmd == 0x32) { /* LC_BUILD_VERSION */
            uint32_t nt, t, u;
            if (size < 24) return RUBY_STAGE_COMMAND;
            nt = RubyStageU32(c + 20);
            if (nt > 16 || (uint64_t)size != 24 + (uint64_t)nt * 8 ||
                RubyStageU32(c + 8) == 0) return RUBY_STAGE_METADATA;
            for (t = 0; t < nt; ++t) {
                uint32_t tool = RubyStageU32(c + 24 + (size_t)t * 8);
                if (tool == 0) return RUBY_STAGE_METADATA;
                for (u = 0; u < t; ++u)
                    if (tool == RubyStageU32(c + 24 + (size_t)u * 8))
                        return RUBY_STAGE_METADATA;
            }
            bit = 8u;
        } else if (cmd == 0x2a) { /* LC_SOURCE_VERSION */
            if (size != 16) return RUBY_STAGE_COMMAND;
            bit = 16u;
        } else if (cmd == 5) { /* One exact ARM_THREAD_STATE64 in LC_UNIXTHREAD. */
            if (size != 288 || RubyStageU32(c + 8) != 6 ||
                RubyStageU32(c + 12) != 68 || RubyStageU32(c + 284) != 0)
                return RUBY_STAGE_ENTRY;
            bit = 32u; plan->entry_va = RubyStageU64(c + 272);
        } else if (cmd == 0x26) { /* LC_FUNCTION_STARTS */
            if (size != 16) return RUBY_STAGE_COMMAND;
            bit = 64u; fun = cursor;
        } else {
            return RUBY_STAGE_COMMAND;
        }
        if (bit != 0) {
            if ((seen & bit) != 0) return RUBY_STAGE_COMMAND;
            seen |= bit;
        }
        cursor += size;
    }
    if (cursor != end) return RUBY_STAGE_COMMAND;
    if (plan->segments == 0 || !header_mapped) return RUBY_STAGE_SEGMENT;
    if ((seen & 32u) == 0 || (plan->entry_va & 3u) != 0) return RUBY_STAGE_ENTRY;
    for (i = 0; i < ng; ++i) {
        if ((g[i].protection & 4u) != 0 && plan->entry_va >= g[i].va &&
            RubyStageRange(plan->entry_va - g[i].va, 4, g[i].file)) entry_found = 1;
    }
    if (!entry_found) return RUBY_STAGE_ENTRY;
    plan->span_bytes = plan->vm_hi - plan->vm_lo;
    plan->entry_pa = RUBY_STAGE_DEST_BASE + (plan->entry_va - RUBY_STAGE_LINK_BASE);
    if (sym != 0) {
        const unsigned char *s = k + (size_t)sym;
        uint64_t so = RubyStageU32(s + 8), st = RubyStageU32(s + 16);
        uint32_t strings = RubyStageU32(s + 20);
        nsyms = RubyStageU32(s + 12);
        if ((nsyms != 0 && (strings == 0 || (so & 7u) != 0)) ||
            !RubyStageMetadataRange(bytes, leoff, lebytes, so, nsyms, 16, ranges, &used) ||
            !RubyStageMetadataRange(bytes, leoff, lebytes, st, strings, 1, ranges, &used))
            return RUBY_STAGE_METADATA;
    }
    if (dys != 0) {
        const unsigned char *d = k + (size_t)dys;
        uint64_t nl = RubyStageU32(d + 12), ie = RubyStageU32(d + 16);
        uint64_t ne = RubyStageU32(d + 20), iu = RubyStageU32(d + 24);
        uint64_t nu = RubyStageU32(d + 28);
        if (sym == 0 || RubyStageU32(d + 8) != 0 || ie != nl || iu != ie + ne ||
            iu + nu != nsyms) return RUBY_STAGE_METADATA;
        for (i = 0; i < 6; ++i) {
            uint64_t off = RubyStageU32(d + 32 + (size_t)i * 8);
            uint64_t count = RubyStageU32(d + 36 + (size_t)i * 8);
            uint64_t width = i == 0 ? 8 : (i == 1 ? 56 : (i <= 3 ? 4 : 8));
            if ((count != 0 && (off & 3u) != 0) ||
                !RubyStageMetadataRange(bytes, leoff, lebytes, off, count, width, ranges, &used))
                return RUBY_STAGE_METADATA;
        }
        nindirect = RubyStageU32(d + 60);
    }
    if (fun != 0) {
        const unsigned char *f = k + (size_t)fun;
        if (!RubyStageMetadataRange(bytes, leoff, lebytes, RubyStageU32(f + 8),
                                    RubyStageU32(f + 12), 1, ranges, &used))
            return RUBY_STAGE_METADATA;
    }
    /* Cross-check section relocation ranges and indirect-symbol indices only
     * after all commands (including a possible late __LINKEDIT) are known. */
    for (i = 0; i < ng; ++i) {
        for (j = 0; j < g[i].sections; ++j) {
            const unsigned char *s = k + (size_t)(g[i].command + 72 + (uint64_t)j * 80);
            uint64_t ro = RubyStageU32(s + 56), nr = RubyStageU32(s + 60);
            uint32_t type = RubyStageU32(s + 64) & 255u;
            if (nr != 0 && (ro < leoff || !RubyStageRange(ro - leoff, nr * 8, lebytes)))
                return RUBY_STAGE_METADATA;
            if (type == 6 || type == 7 || type == 8 || type == 16 || type == 20) {
                uint64_t sz = RubyStageU64(s + 40);
                uint64_t stride = type == 8 ? RubyStageU32(s + 72) : 8;
                if (dys == 0 || sz % stride != 0 ||
                    !RubyStageRange(RubyStageU32(s + 68), sz / stride, nindirect))
                    return RUBY_STAGE_METADATA;
            }
        }
    }
    return RUBY_STAGE_OK;
}

static inline int RubyStagePlanPayload(const unsigned char *p, uint64_t n,
                                       RubyStagePlan *plan)
{
    RubyStagePlan q;
    uint64_t cursor = 8;
    unsigned int entries = 0, kernels = 0;
    int rc, ended = 0;
    if (!RubyStagePointer(p, n) || !RubyStagePointer(plan, sizeof(*plan)) ||
        RubyStageOverlaps((uint64_t)(uintptr_t)p, n,
                          (uint64_t)(uintptr_t)plan, sizeof(*plan)))
        return RUBY_STAGE_ARGUMENT;
    if (n < 8 || n > RUBY_STAGE_SOURCE_MAX || RubyStageU32(p) != UINT32_C(0x50554e58) ||
        (uint64_t)RubyStageU32(p + 4) != n - 8) return RUBY_STAGE_XNUP;
    RubyStagePlanClear(&q);
    while (cursor < n) {
        const unsigned char *e;
        uint64_t size, padded, data;
        if (++entries > 2 || !RubyStageRange(cursor, 24, n)) return RUBY_STAGE_XNUP;
        e = p + (size_t)cursor;
        if (e[0] == 0) {
            if (!RubyStageAllZero(e, 24) || cursor + 24 != n || kernels != 1)
                return RUBY_STAGE_XNUP;
            ended = 1; break;
        }
        if (!RubyStageKernelName(e) || ++kernels != 1 ||
            RubyStageU32(e + 20) != 0) return RUBY_STAGE_XNUP;
        size = RubyStageU32(e + 16); data = cursor + 24;
        /* u32 size promoted BEFORE rounding; never round in 32-bit arithmetic. */
        padded = (size + UINT64_C(15)) & ~UINT64_C(15);
        if (size == 0 || !RubyStageRange(data, padded, n) ||
            !RubyStageAllZero(p + (size_t)(data + size), padded - size))
            return RUBY_STAGE_XNUP;
        q.kernel_offset = data; q.kernel_bytes = size;
        cursor = data + padded;
    }
    if (!ended) return RUBY_STAGE_XNUP;
    rc = RubyStageMach(p + (size_t)q.kernel_offset, q.kernel_bytes, &q);
    if (rc != RUBY_STAGE_OK) return rc;
    RubyStagePlanAssign(plan, &q);
    return RUBY_STAGE_OK;
}

static inline int RubyStageCopy(const unsigned char *payload, uint64_t n,
                                unsigned char *dest, uint64_t dest_base,
                                uint64_t dest_bytes, const RubyStagePlan *plan)
{
    RubyStagePlan q;
    const volatile unsigned char *s = payload;
    volatile unsigned char *d = dest;
    uint64_t i, j, required;
    int rc;
    if (!RubyStagePointer(plan, sizeof(*plan))) return RUBY_STAGE_ARGUMENT;
    /* Use only this freshly derived local plan after comparison, even if the
     * caller's plan is in RAM that will subsequently be overwritten. */
    rc = RubyStagePlanPayload(payload, n, &q);
    if (rc != RUBY_STAGE_OK) return rc;
    if (!RubyStagePlanEqual(plan, &q)) return RUBY_STAGE_PLAN;
    required = q.vm_hi - RUBY_STAGE_LINK_BASE;
    if (dest_base != RUBY_STAGE_DEST_BASE || dest_bytes < required ||
        !RubyStagePointer(dest, dest_bytes)) return RUBY_STAGE_DESTINATION;
    if (RubyStageOverlaps((uint64_t)(uintptr_t)payload, n,
                          (uint64_t)(uintptr_t)dest, dest_bytes) ||
        RubyStageOverlaps(RUBY_STAGE_SOURCE_BASE, n, dest_base, required))
        return RUBY_STAGE_OVERLAP;
    /* Every check that can reject malformed input is above this line.
     * Volatile byte stores/loads require actual RAM writes and read-back. */
    for (i = 0; i < q.segments; ++i) {
        uint64_t at = q.seg[(size_t)i].dest_offset;
        uint64_t from = q.kernel_offset + q.seg[(size_t)i].file_offset;
        uint64_t file = q.seg[(size_t)i].file_bytes;
        for (j = 0; j < file; ++j) d[(size_t)(at + j)] = s[(size_t)(from + j)];
        for (j = file; j < q.seg[(size_t)i].mem_bytes; ++j) d[(size_t)(at + j)] = 0;
    }
    for (i = 0; i < q.segments; ++i) {
        uint64_t at = q.seg[(size_t)i].dest_offset;
        uint64_t from = q.kernel_offset + q.seg[(size_t)i].file_offset;
        uint64_t file = q.seg[(size_t)i].file_bytes;
        for (j = 0; j < file; ++j)
            if (d[(size_t)(at + j)] != s[(size_t)(from + j)]) return RUBY_STAGE_VERIFY;
        for (j = file; j < q.seg[(size_t)i].mem_bytes; ++j)
            if (d[(size_t)(at + j)] != 0) return RUBY_STAGE_VERIFY;
    }
    return RUBY_STAGE_OK;
}

#endif /* RUBY_STAGE_H */
