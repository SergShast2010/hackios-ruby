#ifndef V50_RAM_SELECT_H
#define V50_RAM_SELECT_H
/* Allocation-free dynamic XNU RAM window selector from MediaTek ruby live-FDT.
 * Parses /memory/mblock_info (v2), /reserved-memory children, and FDT memreserve table,
 * subtracts all reserved ranges plus [0, V50_RUBY_MIN_PHYS), and selects a clean
 * 32 MiB-aligned window of [V50_XNU_MIN_WIN .. V50_XNU_MAX_WIN] bytes.
 */
#include <stdint.h>
#include <stddef.h>

#define RUBY_LIVE_FDT_BASE  0x47C80000ULL
#define RUBY_LIVE_FDT_LIMIT 0x00080000ULL
#define V50_RUBY_MIN_PHYS   0x80000000ULL
#define V50_XNU_ALIGN       0x02000000ULL /* 32 MiB */
#define V50_XNU_MIN_WIN     0x10000000ULL /* 256 MiB */
#define V50_XNU_MAX_WIN     0x20000000ULL /* 512 MiB */
#define V50_MAX_RANGES      256u

struct v50_range {
    uint64_t start;
    uint64_t end; /* exclusive */
};

struct v50_ram_window {
    uint64_t base;
    uint64_t size;
    uint64_t mblock_count;
    uint64_t reserved_count;
    uint64_t fdt_bytes;
    uint32_t error;
};

static uint32_t v50_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t v50_be64(const unsigned char *p) {
    return ((uint64_t)v50_be32(p) << 32) | v50_be32(p + 4);
}
static uint32_t v50_le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t v50_le64(const unsigned char *p) {
    return (uint64_t)v50_le32(p) | ((uint64_t)v50_le32(p + 4) << 32);
}
static int v50_streq(const unsigned char *p, uint64_t n, const char *s) {
    uint64_t i = 0;
    while (s[i]) {
        if (i >= n || p[i] != (unsigned char)s[i]) return 0;
        i++;
    }
    return i == n;
}
static int v50_is_mem_node(const unsigned char *p, uint64_t n) {
    return v50_streq(p, n, "memory") ||
           (n > 7 && p[0] == 'm' && p[1] == 'e' && p[2] == 'm' && p[3] == 'o' &&
            p[4] == 'r' && p[5] == 'y' && p[6] == '@');
}

static int v50_add_range(struct v50_range *arr, uint32_t *cnt, uint64_t a, uint64_t sz) {
    if (!sz) return 1;
    if (a > UINT64_MAX - sz) return 0;
    if (*cnt >= V50_MAX_RANGES) return 0;
    arr[*cnt].start = a;
    arr[*cnt].end = a + sz;
    (*cnt)++;
    return 1;
}

static void v50_subtract_one(struct v50_range *free_r, uint32_t *fcnt, uint64_t rs, uint64_t re) {
    uint32_t n = *fcnt;
    for (uint32_t i = 0; i < n; ) {
        uint64_t fs = free_r[i].start, fe = free_r[i].end;
        if (re <= fs || rs >= fe) {
            i++;
            continue;
        }
        if (rs <= fs && re >= fe) {
            free_r[i] = free_r[n - 1];
            n--;
            continue;
        }
        if (rs <= fs) {
            free_r[i].start = re;
            i++;
        } else if (re >= fe) {
            free_r[i].end = rs;
            i++;
        } else {
            free_r[i].end = rs;
            if (n < V50_MAX_RANGES) {
                free_r[n].start = re;
                free_r[n].end = fe;
                n++;
            }
            i++;
        }
    }
    *fcnt = n;
}

static uint32_t V50FindRubyRamWindow(const unsigned char *f, uint64_t cap, struct v50_ram_window *out) {
    static struct v50_range free_r[V50_MAX_RANGES];
    static struct v50_range res_r[V50_MAX_RANGES];
    uint32_t fcnt = 0, rcnt = 0;
    uint64_t total, so, ss, to, ts, ro, o, end;
    uint32_t depth = 0, kinds[32] = {0}, ac[32] = {0}, sc[32] = {0};
    uint32_t mb_seen = 0;

    if (!out) return 1;
    out->base = out->size = out->mblock_count = out->reserved_count = out->fdt_bytes = 0;
    out->error = 1;
    if (!f || cap < 40 || v50_be32(f) != 0xD00DFEEDU) return 1;

    total = v50_be32(f + 4);
    so = v50_be32(f + 8);
    to = v50_be32(f + 12);
    ro = v50_be32(f + 16);
    ts = v50_be32(f + 32);
    ss = v50_be32(f + 36);
    out->fdt_bytes = total;

    if (total < 40 || total > cap || total > 0x80000ULL || v50_be32(f + 20) != 17 ||
        v50_be32(f + 24) > 17 || so < 40 || to < 40 || ro < 40 || (so & 3) || (ro & 7) ||
        ss < 4 || !ts || so > total || ss > total - so || to > total || ts > total - to ||
        (so < to + ts && to < so + ss) || ro >= so) {
        out->error = 2;
        return 2;
    }

    /* Always reserve [0, V50_RUBY_MIN_PHYS) so LK/UEFI/FAT/FDT/TEE/FB/GZ/SSPM stay untouched. */
    if (!v50_add_range(res_r, &rcnt, 0, V50_RUBY_MIN_PHYS)) {
        out->error = 3;
        return 3;
    }

    /* FDT memory reservation block. */
    o = ro;
    for (;;) {
        if (o > so || so - o < 16) { out->error = 3; return 3; }
        uint64_t a = v50_be64(f + o), z = v50_be64(f + o + 8);
        o += 16;
        if (!a && !z) break;
        if (!v50_add_range(res_r, &rcnt, a, z)) { out->error = 3; return 3; }
    }

    o = so;
    end = so + ss;
    while (o < end) {
        if (end - o < 4) { out->error = 4; return 4; }
        uint32_t tok = v50_be32(f + o);
        o += 4;
        if (tok == 1) {
            if (depth >= 32) { out->error = 4; return 4; }
            uint64_t name = o;
            while (o < end && f[o]) o++;
            if (o == end) { out->error = 4; return 4; }
            uint64_t n = o - name;
            o = (o + 4) & ~3ULL;
            if (o > end) { out->error = 4; return 4; }
            kinds[depth] = 0;
            ac[depth] = 2;
            sc[depth] = 2;
            if (depth == 0 && !n) kinds[depth] = 1;
            else if (depth == 1 && v50_is_mem_node(f + name, n)) kinds[depth] = 2;
            else if (depth == 1 && v50_streq(f + name, n, "reserved-memory")) kinds[depth] = 3;
            else if (depth == 2 && kinds[depth - 1] == 3) kinds[depth] = 4;
            depth++;
        } else if (tok == 2) {
            if (!depth) { out->error = 4; return 4; }
            depth--;
        } else if (tok == 3) {
            if (!depth || end - o < 8) { out->error = 4; return 4; }
            uint64_t len = v50_be32(f + o), no = v50_be32(f + o + 4);
            o += 8;
            if (len > end - o || no >= ts) { out->error = 4; return 4; }
            uint64_t pn = to + no, pl = 0;
            while (pl < ts - no && f[pn + pl]) pl++;
            if (pl == ts - no) { out->error = 4; return 4; }
            uint32_t k = kinds[depth - 1];
            if ((k == 1 || k == 3) && len == 4) {
                if (v50_streq(f + pn, pl, "#address-cells")) ac[depth - 1] = v50_be32(f + o);
                else if (v50_streq(f + pn, pl, "#size-cells")) sc[depth - 1] = v50_be32(f + o);
            }
            if (k == 4 && v50_streq(f + pn, pl, "reg") && depth >= 2 && ac[depth - 2] == 2 && sc[depth - 2] == 2) {
                if (!len || (len % 16)) { out->error = 5; return 5; }
                for (uint64_t i = 0; i < len; i += 16) {
                    if (!v50_add_range(res_r, &rcnt, v50_be64(f + o + i), v50_be64(f + o + i + 8))) {
                        out->error = 5;
                        return 5;
                    }
                }
            }
            if (k == 2 && v50_streq(f + pn, pl, "mblock_info")) {
                uint64_t b = 8 + 128 * 24, entries = b + 16;
                if (mb_seen++ || len != 22552) { out->error = 6; return 6; }
                uint64_t count = v50_le32(f + o);
                if (!count || count > 128 || v50_le32(f + o + 4)) { out->error = 6; return 6; }
                if (v50_le32(f + o + b) != 0x99999999U || v50_le32(f + o + b + 4) != 2 || v50_le32(f + o + b + 12)) {
                    out->error = 6;
                    return 6;
                }
                uint64_t rc = v50_le32(f + o + b + 8);
                if (!rc || rc > 128) { out->error = 6; return 6; }
                out->mblock_count = count;
                out->reserved_count = rc;
                for (uint64_t i = 0; i < count; i++) {
                    const unsigned char *p = f + o + 8 + i * 24;
                    uint64_t a = v50_le64(p), z = v50_le64(p + 8);
                    if (!z) continue;
                    if (!v50_add_range(free_r, &fcnt, a, z)) { out->error = 7; return 7; }
                }
                for (uint64_t i = 0; i < rc; i++) {
                    const unsigned char *p = f + o + entries + i * 152;
                    uint64_t a = v50_le64(p), z = v50_le64(p + 8);
                    if (!z) continue;
                    if (!v50_add_range(res_r, &rcnt, a, z)) { out->error = 8; return 8; }
                }
            }
            o = (o + len + 3) & ~3ULL;
            if (o > end) { out->error = 4; return 4; }
        } else if (tok == 4) {
        } else if (tok == 9) {
            break;
        } else {
            out->error = 4;
            return 4;
        }
    }

    if (!mb_seen || !fcnt) { out->error = 9; return 9; }

    for (uint32_t i = 0; i < rcnt; i++) {
        v50_subtract_one(free_r, &fcnt, res_r[i].start, res_r[i].end);
    }

    /* Pick largest 32 MiB-aligned clean window, preferring < 4 GiB first. */
    uint64_t best_base = 0, best_size = 0;
    for (int pass = 0; pass < 2 && best_size < V50_XNU_MIN_WIN; pass++) {
        for (uint32_t i = 0; i < fcnt; i++) {
            uint64_t s = (free_r[i].start + V50_XNU_ALIGN - 1) & ~(V50_XNU_ALIGN - 1);
            uint64_t e = free_r[i].end & ~(V50_XNU_ALIGN - 1);
            if (pass == 0 && e > 0x100000000ULL) e = 0x100000000ULL;
            if (e <= s) continue;
            uint64_t sz = e - s;
            if (sz > V50_XNU_MAX_WIN) sz = V50_XNU_MAX_WIN;
            if (sz >= V50_XNU_MIN_WIN && sz > best_size) {
                best_base = s;
                best_size = sz;
            }
        }
    }

    if (best_size < V50_XNU_MIN_WIN) {
        out->error = 10;
        return 10;
    }

    /* Final defensive overlap check against every reserved range. */
    for (uint32_t i = 0; i < rcnt; i++) {
        if (res_r[i].start < best_base + best_size && best_base < res_r[i].end) {
            out->error = 11;
            return 11;
        }
    }

    out->base = best_base;
    out->size = best_size;
    out->error = 0;
    return 0;
}

#endif
