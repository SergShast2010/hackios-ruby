/* ruby v0.50: clean UEFI -> physical XNU loader for our source-built hakios-kernel + hello.
 * - Dynamically validates live-FDT (GICv3 + mblock_info RAM window) on ruby (MT6877).
 * - Supports QEMU virt end-to-end verification using the exact same extracted BOOTAA64.EFI + PAYLOAD.XNP.
 * - Installs V50XnuReturn in XNU's loader_stub_slot so both normal completion and any panic
 *   render XNU's full loader_log directly onto the framebuffer.
 */
#include "efi.h"
#include "Font.h"
#include "IrqFdt.h"
#include "v50_ram_select.h"

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL
#define KERNEL_LINK_OFFSET  0x03000000ULL
#define PAGE_SIZE_MAX       0x4000ULL
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */
#define PAYLOAD_MAX_BYTES   (16ULL * 1024ULL * 1024ULL)
#define MH_MAGIC_64         0xFEEDFACFu
#define LC_SEGMENT_64       0x19u
#define LC_UNIXTHREAD       0x5u
#define BOOT_LINE_LENGTH    608u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_SIZE      0x80000u
#define PSCI_AFFINITY_INFO  0xC4000004ULL

#define COL_BLACK   0xFF000000u
#define COL_WHITE   0xFFFFFFFFu
#define COL_GREEN   0xFF70FF90u
#define COL_YELLOW  0xFFFFFF60u
#define COL_RED     0xFFFF6060u
#define COL_CYAN    0xFF70E8FFu

static const uint64_t g_font[] = GLYPH_FONT;

struct payload_header { uint32_t magic, size; };
struct payload_entry  { char name[16]; uint32_t size, reserved; };

struct mach_header_64 {
    uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved;
};
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
    uint32_t cmd, cmdsize;
    char segname[16];
    uint64_t vmaddr, vmsize, fileoff, filesize;
    uint32_t maxprot, initprot, nsects, flags;
};

struct boot_video {
    uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};
struct boot_args {
    uint16_t revision;
    uint16_t version;
    uint64_t virtBase;
    uint64_t physBase;
    uint64_t memSize;
    uint64_t topOfKernelData;
    struct boot_video video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char commandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags;
    uint64_t memSizeActual;
};
_Static_assert(offsetof(struct boot_args, virtBase) == 8, "boot_args virtBase");
_Static_assert(offsetof(struct boot_args, video) == 40, "boot_args video");
_Static_assert(offsetof(struct boot_args, deviceTreeP) == 96, "boot_args deviceTreeP");
_Static_assert(offsetof(struct boot_args, commandLine) == 108, "boot_args commandLine");
_Static_assert(offsetof(struct boot_args, bootFlags) == 720, "boot_args bootFlags");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args size");

typedef struct {
    uint32_t revision, pad;
    efi_handle parent;
    efi_system_table *system;
    efi_handle device;
} efi_loaded_image;

typedef struct efi_file efi_file;
struct efi_file {
    uint64_t revision;
    efi_status (EFIAPI *open)(efi_file *, efi_file **, const char16 *, uint64_t, uint64_t);
    efi_status (EFIAPI *close)(efi_file *);
    void *delete_file;
    efi_status (EFIAPI *read)(efi_file *, uint64_t *, void *);
};

typedef struct efi_simple_fs {
    uint64_t revision;
    efi_status (EFIAPI *open_volume)(struct efi_simple_fs *, efi_file **);
} efi_simple_fs;

static const efi_guid g_gop_guid = {0x9042a9de, 0x23dc, 0x4a38, {0x96,0xfb,0x7a,0xde,0xd0,0x80,0x51,0x6a}};
static const efi_guid g_li_guid  = {0x5b1b31a1, 0x9562, 0x11d2, {0x8e,0x3f,0x00,0xa0,0xc9,0x69,0x72,0x3b}};
static const efi_guid g_fs_guid  = {0x964e5b22, 0x6459, 0x11d2, {0x8e,0x39,0x00,0xa0,0xc9,0x69,0x72,0x3b}};

struct v50_ctx {
    uint64_t stack_top;         /* offset 0: required by v50_asm.S */
    uint64_t fb_base;
    uint64_t fb_pitch;          /* bytes per scanline */
    uint32_t fb_width;
    uint32_t fb_height;
    uint32_t fb_stride_px;
    uint32_t is_qemu;
    uint32_t psci_hvc;
    uint32_t physical;
    uint64_t cntfrq;
    uint64_t mpidr;
    uint32_t cpu_count;
    uint32_t cpu_skipped;
    uint32_t cpu_mpidr[V50_MAX_CPUS];
    uint64_t io_base, io_size;
    uint64_t gicd, gicd_size;
    uint64_t gicr, gicr_size;
    uint64_t xnu_phys_base;
    uint64_t xnu_mem_size;
    uint64_t fallback_phys_base;
    uint64_t fallback_mem_size;
    uint64_t ram_orig_size;
    uint64_t ram_probe_fail_pa;
    uint64_t ram_probe_esr;
    uint64_t pram_pa;
    uint64_t payload_pa;
    uint64_t payload_size;
    uint64_t kernel_off;
    uint64_t kernel_size;
    uint64_t ramdisk_off;
    uint64_t ramdisk_size;
    uint64_t vm_lo, vm_hi, entry_va;
    uint64_t isr_pre_jump;
    char cmdline[256];
    char kver[96];
};
_Static_assert(offsetof(struct v50_ctx, stack_top) == 0, "v50_asm stack_top offset");

extern unsigned char __ImageBase[];
extern void V50EnterPhysical(struct v50_ctx *ctx, uint64_t stack_top) __attribute__((noreturn));
extern void V50JumpXnu(uint64_t entry_pa, uint64_t boot_args_pa) __attribute__((noreturn));
extern void V50XnuReturn(void);
extern int64_t V50PsciCall(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3, uint32_t use_hvc);
extern uint64_t V50ProbeRam64(uint64_t pa, uint64_t *esr_out);

static efi_text_output *g_con;

/* Freestanding memory helpers: strict-align safe for MMU-off Device memory. */
static void v50_zero(void *dst, uint64_t n) {
    unsigned char *p = (unsigned char *)dst;
    while (n && ((uintptr_t)p & 7u)) { *p++ = 0; n--; }
    uint64_t *q = (uint64_t *)p;
    while (n >= 8) { *q++ = 0; n -= 8; }
    p = (unsigned char *)q;
    while (n--) *p++ = 0;
}

static void v50_copy(void *dst, const void *src, uint64_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if ((((uintptr_t)d | (uintptr_t)s) & 7u) == 0) {
        uint64_t *dq = (uint64_t *)d;
        const uint64_t *sq = (const uint64_t *)s;
        while (n >= 8) { *dq++ = *sq++; n -= 8; }
        d = (unsigned char *)dq;
        s = (const unsigned char *)sq;
    }
    while (n--) *d++ = *s++;
}

void *memset(void *p, int c, size_t n) {
    if (c == 0) { v50_zero(p, n); return p; }
    unsigned char *q = (unsigned char *)p;
    while (n--) *q++ = (unsigned char)c;
    return p;
}
void *memcpy(void *dst, const void *src, size_t n) {
    v50_copy(dst, src, n);
    return dst;
}

static uint32_t rd_u32(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}
static uint64_t rd_u64(const void *p) {
    const unsigned char *b = (const unsigned char *)p;
    return (uint64_t)rd_u32(b) | ((uint64_t)rd_u32(b + 4) << 32);
}
static uint32_t slen(const char *s) {
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}
static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static int sfind_n(const char *hay, uint64_t hlen, const char *needle) {
    uint32_t nlen = slen(needle);
    if (!nlen || nlen > hlen) return 0;
    for (uint64_t i = 0; i <= hlen - nlen; i++) {
        uint32_t j = 0;
        while (j < nlen && hay[i + j] == needle[j]) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

static void v50_flush_cache(uint64_t a, uint64_t n) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 15u);
    if (line < 16) line = 64;
    for (uint64_t p = a & ~(line - 1); p < a + n; p += line) {
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static void con_puts(const char *s) {
    if (!g_con || !g_con->output_string) return;
    char16 buf[160];
    uint32_t i = 0;
    while (s[i] && i + 3 < sizeof(buf) / sizeof(buf[0])) {
        buf[i] = (char16)(unsigned char)s[i];
        i++;
    }
    buf[i++] = '\r';
    buf[i++] = '\n';
    buf[i] = 0;
    g_con->output_string(g_con, buf);
}

static void fb_clear(const struct v50_ctx *ctx) {
    if (!ctx->fb_base || !ctx->fb_width || !ctx->fb_height) return;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)ctx->fb_base;
    for (uint32_t y = 0; y < ctx->fb_height; y++) {
        volatile uint32_t *row = fb + (uint64_t)y * ctx->fb_stride_px;
        for (uint32_t x = 0; x < ctx->fb_width; x++) row[x] = COL_BLACK;
    }
    if (!ctx->physical) v50_flush_cache(ctx->fb_base, (uint64_t)ctx->fb_height * ctx->fb_pitch);
    else __asm__ volatile("dsb sy" ::: "memory");
}

static void fb_draw_text(const struct v50_ctx *ctx, uint32_t x0, uint32_t y0, uint32_t scale,
                         const char *s, uint32_t color, uint32_t clear_row) {
    if (!ctx->physical) con_puts(s);
    if (!ctx->fb_base || !ctx->fb_width || !ctx->fb_height) return;
    uint32_t ch_h = 12u * scale;
    uint32_t row_h = ch_h + scale * 2u;
    if (y0 + row_h > ctx->fb_height) return;
    volatile uint32_t *fb = (volatile uint32_t *)(uintptr_t)ctx->fb_base;
    if (clear_row) {
        for (uint32_t y = y0; y < y0 + row_h; y++) {
            volatile uint32_t *row = fb + (uint64_t)y * ctx->fb_stride_px;
            for (uint32_t x = 0; x < ctx->fb_width; x++) row[x] = COL_BLACK;
        }
    }
    uint32_t step = 6u * scale;
    uint32_t max_c = (ctx->fb_width > x0) ? ((ctx->fb_width - x0) / step) : 0;
    for (uint32_t c = 0; s[c] && c < max_c; c++) {
        unsigned ch = (unsigned char)s[c];
        if (ch < 32 || ch >= 127) ch = '?';
        uint64_t g = g_font[ch - 32];
        for (uint32_t gy = 0; gy < 12; gy++) {
            uint32_t bits = (gy < 6) ? (uint32_t)(g >> 32) : (uint32_t)g;
            bits >>= (gy % 6) * 5;
            for (uint32_t gx = 0; gx < 5; gx++) {
                if (!(bits & (1u << gx))) continue;
                for (uint32_t sy = 0; sy < scale; sy++) {
                    volatile uint32_t *row = fb + (uint64_t)(y0 + gy * scale + sy) * ctx->fb_stride_px;
                    uint32_t px = x0 + c * step + gx * scale;
                    for (uint32_t sx = 0; sx < scale; sx++) row[px + sx] = color;
                }
            }
        }
    }
    if (!ctx->physical) v50_flush_cache(ctx->fb_base + (uint64_t)y0 * ctx->fb_pitch, (uint64_t)row_h * ctx->fb_pitch);
    else __asm__ volatile("dsb sy" ::: "memory");
}

static void ui_line(const struct v50_ctx *ctx, uint32_t row, const char *s, uint32_t color) {
    fb_draw_text(ctx, 20u, 80u + row * 44u, 3u, s, color, 1u);
}

static void ui_hex(const struct v50_ctx *ctx, uint32_t row, const char *label, uint64_t v, uint32_t color) {
    char buf[96];
    uint32_t n = 0;
    while (label[n] && n < 50) { buf[n] = label[n]; n++; }
    buf[n++] = '0'; buf[n++] = 'x';
    for (int sh = 60; sh >= 0; sh -= 4) buf[n++] = "0123456789ABCDEF"[(v >> sh) & 15u];
    buf[n] = 0;
    ui_line(ctx, row, buf, color);
}

__attribute__((noinline, noreturn)) static void ui_halt(const struct v50_ctx *ctx, uint32_t row, const char *msg, uint64_t code) {
    ui_line(ctx, row, msg, COL_RED);
    ui_hex(ctx, row + 1, "Code: ", code, COL_YELLOW);
    ui_line(ctx, row + 2, "Halted safely - photograph this screen", COL_WHITE);
    for (;;) __asm__ volatile("yield");
}

/* Safe check whether an address is mapped readable in current EL1 translation tables. */
static int v50_va_readable(uint64_t addr) {
    uint64_t par;
    __asm__ volatile("at s1e1r, %1\nisb\nmrs %0, par_el1" : "=r"(par) : "r"(addr) : "memory");
    return (par & 1ULL) == 0;
}

static int v50_overlaps(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) {
    if (!asz || !bsz) return 0;
    return a < b + bsz && b < a + asz;
}

static uint64_t own_image_bytes(void) {
    const unsigned char *p = __ImageBase;
    if (p[0] != 'M' || p[1] != 'Z') return 0;
    uint32_t off = rd_u32(p + 0x3c);
    if (off < 64 || off > 1024 || rd_u32(p + off) != 0x4550u) return 0;
    uint64_t n = rd_u32(p + off + 24 + 56);
    return (n >= 4096 && n <= 0x100000ULL) ? n : 0;
}

/* --- Apple Device Tree writer (strictly aligned, MMU-off safe) --- */
struct dt_writer {
    unsigned char *base, *cur, *end;
    int overflow;
    uint32_t scratch[2];
    uint32_t *counts[16];
    int depth;
};

static void dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len) {
    uint64_t padded = ((uint64_t)len + 3ULL) & ~3ULL;
    if (w->overflow || slen(name) > 32 || (uint64_t)(w->end - w->cur) < 36ULL + padded) {
        w->overflow = 1;
        return;
    }
    w->counts[w->depth - 1][0]++;
    v50_zero(w->cur, 32);
    v50_copy(w->cur, name, slen(name));
    w->cur += 32;
    v50_copy(w->cur, &len, 4);
    w->cur += 4;
    if (len) v50_copy(w->cur, data, len);
    if (padded > len) v50_zero(w->cur + len, padded - len);
    w->cur += padded;
}
static void dt_prop_u32(struct dt_writer *w, const char *name, uint32_t v) { dt_prop(w, name, &v, 4); }
static void dt_prop_u64(struct dt_writer *w, const char *name, uint64_t v) { dt_prop(w, name, &v, 8); }
static void dt_prop_str(struct dt_writer *w, const char *name, const char *v) { dt_prop(w, name, v, slen(v) + 1); }
static void dt_prop_padded(struct dt_writer *w, const char *name, const char *v, uint32_t sz) {
    unsigned char tmp[64];
    if (sz > sizeof(tmp)) { w->overflow = 1; return; }
    v50_zero(tmp, sz);
    uint32_t l = slen(v);
    if (l > sz) l = sz;
    v50_copy(tmp, v, l);
    dt_prop(w, name, tmp, sz);
}
static void dt_begin(struct dt_writer *w, const char *name) {
    if (w->depth > 0) w->counts[w->depth - 1][1]++;
    if (w->overflow || w->depth >= 16 || (uint64_t)(w->end - w->cur) < 8) {
        w->overflow = 1;
        w->counts[w->depth < 16 ? w->depth : 15] = w->scratch;
        if (w->depth < 16) w->depth++;
        return;
    }
    w->counts[w->depth++] = (uint32_t *)w->cur;
    v50_zero(w->cur, 8);
    w->cur += 8;
    dt_prop_str(w, "name", name);
}
static void dt_end(struct dt_writer *w) {
    if (w->depth > 0) w->depth--;
}

static void nvram_chrp_header(unsigned char *h, unsigned char sig, uint32_t bytes, const char *name) {
    uint16_t blocks = (uint16_t)(bytes / 16u);
    v50_zero(h, 16);
    h[0] = sig;
    h[2] = (unsigned char)(blocks & 0xffu);
    h[3] = (unsigned char)((blocks >> 8) & 0xffu);
    v50_copy(h + 4, name, slen(name));
    uint32_t sum = h[0];
    for (int i = 2; i < 16; i++) sum += h[i];
    while (sum > 0xffu) sum = (sum & 0xffu) + (sum >> 8);
    h[1] = (unsigned char)sum;
}

static void build_nvram_image(unsigned char *image) {
    uint32_t lo = 1, hi = 0, generation = 1;
    v50_zero(image, NVRAM_BANK_SIZE);
    nvram_chrp_header(image, 0x5au, 0x20u, "nvram");
    nvram_chrp_header(image + 0x20, 0x70u, NVRAM_BANK_SIZE - 0x20u, "common");
    v50_copy(image + 20, &generation, 4);
    for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
        lo = (lo + image[i]) % 65521u;
        hi = (hi + lo) % 65521u;
    }
    uint32_t adler = (hi << 16) | lo;
    v50_copy(image + 16, &adler, 4);
}

static uint32_t build_apple_dt(const struct v50_ctx *ctx, uint64_t dt_pa, uint64_t dt_max,
                               uint64_t rd_pa, uint64_t rd_size) {
    struct dt_writer w;
    w.base = (unsigned char *)(uintptr_t)dt_pa;
    w.cur = w.base;
    w.end = w.base + dt_max;
    w.overflow = 0;
    w.depth = 0;

    unsigned char seed[64];
    uint64_t cntv;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cntv));
    for (uint32_t i = 0; i < sizeof(seed); i++) {
        seed[i] = (unsigned char)((cntv >> (i & 7u)) ^ (i * 37u + 0x5Au));
    }

    static unsigned char nvram_image[NVRAM_BANK_SIZE];
    build_nvram_image(nvram_image);
    static const unsigned char wifi_mac[6] = { 0x02, 0x52, 0x55, 0x42, 0x59, 0x01 };
    static const unsigned char bt_mac[6]   = { 0x02, 0x52, 0x55, 0x42, 0x59, 0x02 };
    static const unsigned char manifest_hash[48] = { 'r','u','b','y','m','t','6','8','7','7' };
    static const unsigned char no_behavior[16] = { 0 };

    const char compat_root[] = "D221AP\0iPhone10,6\0ARM64";
    uint64_t ranges[3] = { 0, ctx->io_base, ctx->io_size };
    uint64_t gic_reg[4] = {
        ctx->gicd - ctx->io_base, ctx->gicd_size,
        ctx->gicr - ctx->io_base, ctx->gicr_size
    };

    dt_begin(&w, "device-tree");
        dt_prop_str(&w, "model", "iPhone10,6");
        dt_prop(&w, "compatible", compat_root, sizeof(compat_root));
        dt_prop_str(&w, "target-type", "D221");
        dt_prop_str(&w, "platform-name", "t8015");
        dt_prop_padded(&w, "serial-number", "RUBY00MT6877", 32);
        dt_prop_padded(&w, "mlb-serial-number", "RUBY00MT6877MLB00", 32);
        dt_prop_padded(&w, "model-number", "MQAC2", 32);
        dt_prop_padded(&w, "region-info", "LL/A", 32);
        dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
        dt_prop_u32(&w, "#address-cells", 2);
        dt_prop_u32(&w, "#size-cells", 2);

        dt_begin(&w, "chosen");
            dt_prop_str(&w, "darwin-board", ctx->is_qemu ? "qemu-virt" : "ruby");
            dt_prop(&w, "random-seed", seed, sizeof(seed));
            dt_prop_u64(&w, "dram-base", ctx->xnu_phys_base);
            dt_prop_u64(&w, "dram-size", ctx->xnu_mem_size);
            dt_prop_u32(&w, "debug-enabled", 1);
            dt_prop_u32(&w, "display-scale", 3);
            dt_prop_u32(&w, "display-rotation", 0);
            dt_prop_str(&w, "psci-method", ctx->psci_hvc ? "hvc" : "smc");
            dt_prop_u64(&w, "unique-chip-id", 0x0000525542590001ULL);
            dt_prop_u32(&w, "chip-id", 0x8015);
            dt_prop_u32(&w, "board-id", 0x06);
            dt_prop(&w, "mac-address-wifi0", wifi_mac, sizeof(wifi_mac));
            dt_prop(&w, "mac-address-bluetooth0", bt_mac, sizeof(bt_mac));
            dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
            dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
            dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
            dt_prop_u32(&w, "certificate-production-status", 1);
            dt_prop_u32(&w, "certificate-security-mode", 1);
            dt_prop_u32(&w, "effective-production-status-ap", 1);
            dt_prop_u32(&w, "effective-security-mode-ap", 1);
            dt_prop_u32(&w, "secure-boot", 1);
            dt_prop_u32(&w, "production-cert", 1);
            dt_prop_u32(&w, "development-cert", 0);
            dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
            dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
            dt_prop_u32(&w, "nvram-bank-count", 1);
            dt_prop_u32(&w, "nvram-current-bank", 0);
            dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
            dt_begin(&w, "memory-map");
                if (rd_size) {
                    uint64_t rd_prop[2] = { rd_pa, rd_size };
                    dt_prop(&w, "RAMDisk", rd_prop, sizeof(rd_prop));
                }
            dt_end(&w);
            if (ctx->is_qemu && ctx->cpu_count > 1u) {
                dt_begin(&w, "machine-timeouts");
                    dt_prop_u32(&w, "global-scale", 20u);
                dt_end(&w);
            }
        dt_end(&w);

        dt_begin(&w, "defaults");
            dt_prop_u32(&w, "kern.vm_compressor", 2);
        dt_end(&w);

        dt_begin(&w, "product");
            dt_prop_str(&w, "product-name", "iPhone");
        dt_end(&w);

        dt_begin(&w, "options");
        dt_end(&w);

        if (ctx->pram_pa) {
            uint64_t pram_reg[2] = { ctx->pram_pa, PANIC_LOG_SIZE };
            v50_zero((void *)(uintptr_t)ctx->pram_pa, PANIC_LOG_SIZE);
            dt_begin(&w, "pram");
                dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
            dt_end(&w);
        }

        dt_begin(&w, "cpus");
            uint32_t ncpu = ctx->cpu_count;
            if (ncpu < 1u) ncpu = 1u;
            if (ncpu > V50_MAX_CPUS) ncpu = V50_MAX_CPUS;
            for (uint32_t i = 0; i < ncpu; i++) {
                char cname[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };
                dt_begin(&w, cname);
                    dt_prop_str(&w, "device_type", "cpu");
                    dt_prop_str(&w, "state", (i == 0) ? "running" : "waiting");
                    dt_prop_u32(&w, "reg", ctx->cpu_mpidr[i]);
                    dt_prop_u32(&w, "cpu-id", i);
                    dt_prop_u32(&w, "cluster-id", 0);
                    dt_prop_u64(&w, "timebase-frequency", ctx->cntfrq);
                    dt_prop_u64(&w, "fixed-frequency", ctx->cntfrq);
                    dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
                    dt_prop_u64(&w, "bus-frequency", 100000000ULL);
                    dt_prop_u64(&w, "memory-frequency", 100000000ULL);
                    dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
                dt_end(&w);
            }
        dt_end(&w);

        dt_begin(&w, "arm-io");
            dt_prop_str(&w, "device_type", ctx->is_qemu ? "qemu-virt" : "ruby-mt6877");
            dt_prop(&w, "ranges", ranges, sizeof(ranges));
            dt_prop_u32(&w, "#address-cells", 2);
            dt_prop_u32(&w, "#size-cells", 2);
            dt_begin(&w, "gic");
                dt_prop_str(&w, "device_type", "interrupt-controller");
                dt_prop_str(&w, "compatible", "arm,gic-v3");
                dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
            dt_end(&w);
        dt_end(&w);
    dt_end(&w);

    if (w.overflow) return 0;
    return (uint32_t)(w.cur - w.base);
}

static void append_str(char *dst, uint32_t *n, uint32_t max, const char *s) {
    while (*s && *n + 1 < max) dst[(*n)++] = *s++;
    dst[*n] = 0;
}

static void gic_fix_eoimode(void) {
    uint64_t sre;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
    if (sre & 1ULL) {
        uint64_t ctlr;
        __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(ctlr));
        if (ctlr & 2ULL) {
            __asm__ volatile("msr icc_ctlr_el1, %0\nisb" : : "r"(ctlr & ~2ULL) : "memory");
        }
    }
}

/* --- Physical execution after ExitBootServices + MMU OFF --- */
__attribute__((noinline, noreturn)) void V50BootPhysical(struct v50_ctx *ctx) {
    ctx->physical = 1;
    g_con = NULL;

    ui_line(ctx, 13, "ExitBootServices + MMU OFF: Success", COL_GREEN);

    /* Disable MT6877 watchdog before long boot/input runs on the real phone. */
    if (!ctx->is_qemu) {
        *(volatile uint32_t *)(uintptr_t)0x10007000ULL = 0x22000000U;
        __asm__ volatile("dsb sy" ::: "memory");
    }

    /* Probe every 1 MiB of the chosen XNU RAM window for hidden EMI/MPU SErrors. */
    ctx->ram_orig_size = ctx->xnu_mem_size;
    ctx->ram_probe_fail_pa = 0;
    ctx->ram_probe_esr = 0;
    if (!ctx->is_qemu && ctx->xnu_phys_base >= 0x100000000ULL) {
        uint64_t step = 0x100000ULL; /* 1 MiB */
        uint64_t good_end = ctx->xnu_phys_base;
        for (uint64_t a = ctx->xnu_phys_base; a < ctx->xnu_phys_base + ctx->xnu_mem_size; a += step) {
            uint64_t esr = 0;
            if (V50ProbeRam64(a, &esr) || V50ProbeRam64(a + step - 8ULL, &esr)) {
                ctx->ram_probe_fail_pa = a;
                ctx->ram_probe_esr = esr;
                break;
            }
            good_end = a + step;
        }
        uint64_t clean_sz = (good_end - ctx->xnu_phys_base) & ~(V50_XNU_ALIGN - 1ULL);
        if (clean_sz < ctx->xnu_mem_size) {
            if (clean_sz >= V50_XNU_MIN_WIN) {
                ctx->xnu_mem_size = clean_sz;
            } else if (ctx->fallback_phys_base && ctx->fallback_mem_size >= V50_XNU_MIN_WIN) {
                ctx->xnu_phys_base = ctx->fallback_phys_base;
                ctx->xnu_mem_size = ctx->fallback_mem_size;
            } else {
                ui_hex(ctx, 14, "RAM probe ESR: ", ctx->ram_probe_esr, COL_RED);
                ui_halt(ctx, 15, "ERROR: High RAM SError with no fallback", ctx->ram_probe_fail_pa);
            }
        }
    } else if (ctx->is_qemu) {
        uint64_t esr = 0;
        if (V50ProbeRam64(ctx->xnu_phys_base, &esr) ||
            V50ProbeRam64(ctx->xnu_phys_base + ctx->xnu_mem_size - 8ULL, &esr)) {
            ctx->ram_probe_fail_pa = ctx->xnu_phys_base;
            ctx->ram_probe_esr = esr;
        }
    }

    if (ctx->ram_probe_fail_pa) {
        ui_hex(ctx, 14, "RAM cut@PA: ", ctx->ram_probe_fail_pa, COL_YELLOW);
    } else {
        ui_line(ctx, 14, "RAM SError probe: PASS (full window)", COL_GREEN);
    }

    gic_fix_eoimode();

    uint64_t phys_base = ctx->xnu_phys_base;
    uint64_t slide = phys_base - KERNEL_VIRT_BASE;
    const unsigned char *kernel = (const unsigned char *)(uintptr_t)(ctx->payload_pa + ctx->kernel_off);
    const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
    const unsigned char *p = kernel + sizeof(*mh);

    for (uint32_t i = 0; i < mh->ncmds; i++) {
        uint32_t cmd = rd_u32(p);
        uint32_t cmdsize = rd_u32(p + 4);
        if (cmd == LC_SEGMENT_64 && cmdsize >= sizeof(struct segment_command_64)) {
            uint64_t vmaddr   = rd_u64(p + 24);
            uint64_t vmsize   = rd_u64(p + 32);
            uint64_t fileoff  = rd_u64(p + 40);
            uint64_t filesize = rd_u64(p + 48);
            if (vmsize) {
                uint64_t dst_pa = vmaddr + slide;
                if (filesize) v50_copy((void *)(uintptr_t)dst_pa, kernel + fileoff, filesize);
                if (vmsize > filesize) v50_zero((void *)(uintptr_t)(dst_pa + filesize), vmsize - filesize);
            }
        }
        p += cmdsize;
    }

    uint64_t kern_lo = ctx->vm_lo + slide;
    uint64_t kern_hi = (ctx->vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

    /* Patch XNU loader_stub_slot so panics/exits return cleanly to V50XnuReturn with loader_log. */
    uint64_t slot_pa = 0;
    for (uint64_t a = kern_lo & ~7ULL; a + 32 <= kern_hi; a += 8) {
        volatile uint64_t *q = (volatile uint64_t *)(uintptr_t)a;
        if (q[0] == 0x545352444C554E58ULL /* "XNULDRST" */) {
            q[1] = (uint64_t)(uintptr_t)&V50XnuReturn;
            q[2] = 0ULL; /* checkpoint 0 = full boot */
            q[3] = slide;
            slot_pa = a;
            break;
        }
    }

    uint64_t rd_pa = kern_hi;
    uint64_t rd_size = (ctx->ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
    if (ctx->ramdisk_size) {
        v50_copy((void *)(uintptr_t)rd_pa,
                 (const void *)(uintptr_t)(ctx->payload_pa + ctx->ramdisk_off),
                 ctx->ramdisk_size);
        if (rd_size > ctx->ramdisk_size) {
            v50_zero((void *)(uintptr_t)(rd_pa + ctx->ramdisk_size), rd_size - ctx->ramdisk_size);
        }
    }

    uint64_t dt_pa = rd_pa + rd_size;
    uint64_t dt_max = 0x20000ULL;
    uint32_t dt_len = build_apple_dt(ctx, dt_pa, dt_max, rd_pa, rd_size);
    if (!dt_len) ui_halt(ctx, 16, "ERROR: Apple DT overflow", dt_pa);

    uint64_t ba_pa = (dt_pa + dt_len + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
    uint64_t top = ba_pa + PAGE_SIZE_MAX;

    struct boot_args *ba = (struct boot_args *)(uintptr_t)ba_pa;
    v50_zero(ba, sizeof(*ba));
    ba->revision = 2;
    ba->version = 2;
    ba->virtBase = KERNEL_VIRT_BASE;
    ba->physBase = phys_base;
    ba->memSize = ctx->xnu_mem_size;
    ba->memSizeActual = ctx->xnu_mem_size;
    ba->topOfKernelData = top;
    ba->deviceTreeP = dt_pa - slide;
    ba->deviceTreeLength = dt_len;

    if (ctx->fb_base) {
        ba->video.v_baseAddr = ctx->fb_base;
        ba->video.v_display = 0; /* text console mode for -v */
        ba->video.v_rowBytes = ctx->fb_pitch;
        ba->video.v_width = ctx->fb_width;
        ba->video.v_height = ctx->fb_height;
        ba->video.v_depth = 32;
    }

    uint32_t cl = 0;
    append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "debug=0x100 ");
    if (!sfind_n(ctx->cmdline, slen(ctx->cmdline), "cpus=")) {
        uint32_t nc = ctx->cpu_count;
        if (nc < 1u) nc = 1u;
        if (nc > V50_MAX_CPUS) nc = V50_MAX_CPUS;
        char cbuf[8] = { 'c', 'p', 'u', 's', '=', (char)('0' + nc), ' ', 0 };
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, cbuf);
    }
    if (rd_size) append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "rd=md0 ");
    append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "-v ");
    if (ctx->kver[0]) {
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "hakios_kver=");
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, ctx->kver);
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, " ");
    }
    if (ctx->cmdline[0]) {
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, ctx->cmdline);
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, " ");
    }
    if (ctx->is_qemu) {
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "virtio_input=1 keylog=1 touchlog=1 hello_wait=6");
    } else {
        append_str(ba->commandLine, &cl, BOOT_LINE_LENGTH, "buttons=2 keylog=1 touchscreen=2 touchlog=1 mtk_ufs_ro=1 hello_wait=25");
    }

    uint64_t entry_pa = ctx->entry_va + slide;
    ui_hex(ctx, 15, "XNU entry PA: ", entry_pa, COL_WHITE);
    ui_hex(ctx, 16, "boot_args PA: ", ba_pa, COL_WHITE);
    ui_hex(ctx, 17, "return slot:  ", slot_pa, slot_pa ? COL_GREEN : COL_YELLOW);
    ui_line(ctx, 18, "Jumping to XNU kernel now...", COL_GREEN);

    /* Brief pause on phone so staging summary is visible before XNU clears the screen. */
    if (!ctx->is_qemu && ctx->cntfrq >= 1000000ULL) {
        uint64_t t0, now;
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(t0));
        do {
            __asm__ volatile("yield\nmrs %0, cntpct_el0" : "=r"(now));
        } while (now - t0 < ctx->cntfrq * 2ULL);
    }

    fb_clear(ctx);
    __asm__ volatile("msr cntv_ctl_el0, %0\nmsr cntp_ctl_el0, %0\nisb" : : "r"(2ULL) : "memory");
    __asm__ volatile("dsb sy\nisb\nmrs %0, isr_el1" : "=r"(ctx->isr_pre_jump));
    V50JumpXnu(entry_pa, ba_pa);
}

/* --- Return from XNU (after hello exits or on any panic/early exception) --- */
__attribute__((noinline, noreturn)) void V50OnXnuReturn(
    uint64_t log_pa, uint64_t log_len, uint64_t reason, uint64_t code,
    uint64_t elr, uint64_t far, struct v50_ctx *ctx)
{
    ctx->physical = 1;
    g_con = NULL;
    fb_clear(ctx);

    const char *log = (const char *)(uintptr_t)log_pa;
    int valid_log = (log_pa >= 0x40000000ULL && log_len > 0 && log_len <= 2ULL * 1024ULL * 1024ULL);
    int hello_ok = valid_log && sfind_n(log, log_len, "Hello from user space on Darwin!");
    int gic_ok   = valid_log && sfind_n(log, log_len, "gic: v3");
    int bsd_ok   = valid_log && sfind_n(log, log_len, "bsd_init: done");
    int smp_ok   = (ctx->cpu_count <= 1u) || (valid_log && sfind_n(log, log_len, "processors are running"));
    int btn_ok   = valid_log && (sfind_n(log, log_len, "mtk_buttons: ready") || sfind_n(log, log_len, "virtio-input:"));
    int touch_ok = valid_log && (sfind_n(log, log_len, "mtk_touch: detected") || sfind_n(log, log_len, "virtio-input: tablet"));
    int ufs_ok   = valid_log && sfind_n(log, log_len, "mtk_ufs_ro: LUN2 GPT OK");

    if (hello_ok) {
        ui_line(ctx, 0, "RUBY v0.53b: XNU SMP+INPUT+UFS PASS!", COL_GREEN);
    } else {
        ui_line(ctx, 0, "RUBY v0.53b: XNU RETURNED (SEE LOG)", COL_YELLOW);
    }
    ui_hex(ctx, 1, "RAMbase: ", ctx->xnu_phys_base, COL_WHITE);
    ui_hex(ctx, 2, "RAMsize: ", ctx->xnu_mem_size, ctx->ram_probe_fail_pa ? COL_YELLOW : COL_GREEN);
    ui_hex(ctx, 3, "code/ESR:", code, reason ? COL_RED : COL_WHITE);
    if (reason == 1) {
        ui_hex(ctx, 4, "ELR:     ", elr, COL_YELLOW);
        ui_hex(ctx, 5, "FAR:     ", far, COL_YELLOW);
    } else {
        char st[80];
        uint32_t n = 0;
        append_str(st, &n, sizeof(st), "GIC:");
        append_str(st, &n, sizeof(st), gic_ok ? "OK " : "NO ");
        append_str(st, &n, sizeof(st), "SMP:");
        append_str(st, &n, sizeof(st), (ctx->cpu_count <= 1u) ? "1C " : (smp_ok ? "OK " : "NO "));
        append_str(st, &n, sizeof(st), "BTN:");
        append_str(st, &n, sizeof(st), btn_ok ? "OK " : "NO ");
        append_str(st, &n, sizeof(st), "TCH:");
        append_str(st, &n, sizeof(st), touch_ok ? "OK " : "NO ");
        append_str(st, &n, sizeof(st), "UFS:");
        append_str(st, &n, sizeof(st), ctx->is_qemu ? "-- " : (ufs_ok ? "OK " : "NO "));
        append_str(st, &n, sizeof(st), "P1:");
        append_str(st, &n, sizeof(st), hello_ok ? "OK" : "NO");
        ui_line(ctx, 4, st, (gic_ok && bsd_ok && smp_ok && hello_ok) ? COL_GREEN : COL_YELLOW);
        if (ctx->ram_probe_fail_pa) {
            ui_hex(ctx, 5, "RAMcut@: ", ctx->ram_probe_fail_pa, COL_YELLOW);
        } else if (!hello_ok) {
            ui_hex(ctx, 5, "ISR_pre: ", ctx->isr_pre_jump, ctx->isr_pre_jump ? COL_RED : COL_WHITE);
        }
    }

    /* Render tail of XNU's loader_log with line wrapping so nothing clips off the right edge. */
    if (valid_log && ctx->fb_base) {
        uint32_t y_start = (reason == 1 || ctx->ram_probe_fail_pa || !hello_ok) ? 376u : 320u;
        /* Show mtk_buttons / mtk_touch / mtk_ufs_ro / virtio-input diag lines at top of log pane */
        if (reason != 1) {
            uint32_t shown = 0;
            uint64_t scan = 0;
            while (scan < log_len && shown < 20u && y_start + 56u < ctx->fb_height) {
                uint64_t ls = scan;
                while (scan < log_len && log[scan] != '\n') scan++;
                uint64_t ll = scan - ls;
                if (scan < log_len && log[scan] == '\n') scan++;
                if (ll > 0 && (sfind_n(log + ls, ll, "mtk_buttons:") ||
                               sfind_n(log + ls, ll, "mtk_touch:") ||
                               sfind_n(log + ls, ll, "mtk_ufs_ro:") ||
                               sfind_n(log + ls, ll, "mtk_ram_diag:") ||
                               sfind_n(log + ls, ll, "virtio-input:"))) {
                    char lbuf[96];
                    uint32_t ln = 0;
                    uint64_t copy = ll < 88u ? ll : 88u;
                    for (uint64_t k = 0; k < copy; k++) {
                        char c = log[ls + k];
                        if (c >= 32 && c < 127) lbuf[ln++] = c;
                    }
                    lbuf[ln] = 0;
                    uint32_t col = (sfind_n(lbuf, ln, "detected") || sfind_n(lbuf, ln, "ready") || sfind_n(lbuf, ln, "GPT OK") || sfind_n(lbuf, ln, "CLEAN")) ? COL_GREEN : (sfind_n(lbuf, ln, "SERROR") ? COL_RED : COL_CYAN);
                    fb_draw_text(ctx, 16u, y_start, 2u, lbuf, col, 0u);
                    y_start += 26u;
                    shown++;
                }
            }
            if (shown) y_start += 8u;
        }
        if (!hello_ok) {
            for (uint64_t p = 0; p + 7 <= log_len; p++) {
                if (log[p]=='g' && log[p+1]=='i' && log[p+2]=='c' && log[p+3]==':' &&
                    log[p+4]==' ' && log[p+5]=='v' && log[p+6]=='3') {
                    char gbuf[128];
                    uint32_t gn = 0;
                    while (p + gn < log_len && gn + 1 < sizeof(gbuf) && log[p + gn] != '\n' && log[p + gn] != '\r') {
                        gbuf[gn] = log[p + gn];
                        gn++;
                    }
                    gbuf[gn] = 0;
                    fb_draw_text(ctx, 16u, y_start, 2u, gbuf, COL_CYAN, 0u);
                    y_start += 28u;
                    break;
                }
            }
        }
        uint32_t row_h = 28u;
        uint32_t max_rows = (ctx->fb_height > y_start + 56u) ? ((ctx->fb_height - y_start - 56u) / row_h) : 0;
        if (max_rows > 72u) max_rows = 72u;
        uint32_t max_cols = (ctx->fb_width > 32u) ? ((ctx->fb_width - 32u) / 12u) : 40u;
        if (max_cols > 92u) max_cols = 92u;
        if (max_cols < 20u) max_cols = 20u;

        /* Skip binary compressed panic trailer if present (PMOC). */
        uint64_t text_end = log_len;
        for (uint64_t i = 0; i + 4 <= log_len; i++) {
            if (log[i] == 'P' && log[i + 1] == 'M' && log[i + 2] == 'O' && log[i + 3] == 'C') {
                text_end = i;
                break;
            }
        }
        /* When PID 1 succeeded and exited intentionally, trim expected "init died" panic dump
         * so the live Hello + kernel boot lines stay visible on screen. */
        if (hello_ok) {
            const char *marker = "--- panic buffer ---";
            uint32_t mlen = slen(marker);
            for (uint64_t i = 0; i + mlen <= text_end; i++) {
                uint32_t m = 0;
                while (m < mlen && log[i + m] == marker[m]) m++;
                if (m == mlen) {
                    text_end = i;
                    break;
                }
            }
            const char *msgm = "--- kernel message buffer ---";
            uint32_t msglen = slen(msgm);
            for (uint64_t i = 0; i + msglen <= text_end; i++) {
                uint32_t m = 0;
                while (m < msglen && log[i + m] == msgm[m]) m++;
                if (m == msglen) {
                    if (sfind_n(log, i, "Hello from user space on Darwin!")) {
                        text_end = i;
                    }
                    break;
                }
            }
        }

        /* Find last lines that fit into max_rows (accounting for wrapped lines). */
        uint32_t rows_need = 0;
        uint64_t pos = text_end;
        while (pos > 0 && rows_need < max_rows) {
            uint64_t line_end = pos;
            if (line_end > 0 && log[line_end - 1] == '\n') line_end--;
            uint64_t line_start = line_end;
            while (line_start > 0 && log[line_start - 1] != '\n') line_start--;
            uint64_t llen = line_end - line_start;
            uint32_t wrapped = (llen == 0) ? 0u : (uint32_t)((llen + max_cols - 1u) / max_cols);
            if (rows_need + wrapped > max_rows && rows_need > 0) break;
            rows_need += wrapped;
            pos = line_start;
        }

        uint32_t r = 0;
        while (pos < text_end && r < max_rows) {
            uint64_t lstart = pos;
            while (pos < text_end && log[pos] != '\n') pos++;
            uint64_t lend = pos;
            if (pos < text_end && log[pos] == '\n') pos++;

            /* Determine line color across the full logical line. */
            uint32_t col = COL_WHITE;
            if (sfind_n(log + lstart, lend - lstart, "Hello from user space") ||
                sfind_n(log + lstart, lend - lstart, "I am process 1") ||
                sfind_n(log + lstart, lend - lstart, "kernel.ver=")) col = COL_GREEN;
            else if (sfind_n(log + lstart, lend - lstart, "panic(") ||
                     sfind_n(log + lstart, lend - lstart, "FAILED") ||
                     sfind_n(log + lstart, lend - lstart, "Unable to")) col = COL_RED;
            else if (sfind_n(log + lstart, lend - lstart, "Darwin Kernel") ||
                     sfind_n(log + lstart, lend - lstart, "gic: v3") ||
                     sfind_n(log + lstart, lend - lstart, "bsd_init: done")) col = COL_YELLOW;

            uint64_t cur = lstart;
            while (cur < lend && r < max_rows) {
                char seg[96];
                uint32_t k = 0;
                while (cur < lend && k < max_cols) {
                    unsigned char ch = (unsigned char)log[cur++];
                    if (ch == '\r') continue;
                    if (ch == '\t') ch = ' ';
                    if (ch < 32 || ch >= 127) ch = '.';
                    seg[k++] = (char)ch;
                }
                seg[k] = 0;
                if (k == 0) break;
                fb_draw_text(ctx, 16u, y_start + r * row_h, 2u, seg, col, 0u);
                r++;
            }
        }
    }

    fb_draw_text(ctx, 20u, ctx->fb_height > 48u ? ctx->fb_height - 44u : 0u, 2u,
                 "Halted safely - photograph this screen", COL_CYAN, 1u);
    for (;;) __asm__ volatile("yield");
}

__attribute__((noinline, noreturn)) void V50OnException(
    uint64_t vec, uint64_t esr, uint64_t elr, uint64_t far, struct v50_ctx *ctx)
{
    ctx->physical = 1;
    g_con = NULL;
    fb_clear(ctx);
    ui_line(ctx, 0, "RUBY v0.53: EARLY LOADER EXCEPTION", COL_RED);
    ui_hex(ctx, 2, "Vector: ", vec, COL_YELLOW);
    ui_hex(ctx, 3, "ESR:    ", esr, COL_RED);
    ui_hex(ctx, 4, "ELR:    ", elr, COL_WHITE);
    ui_hex(ctx, 5, "FAR:    ", far, COL_WHITE);
    ui_line(ctx, 7, "Halted safely - photograph this screen", COL_CYAN);
    for (;;) __asm__ volatile("yield");
}

/* Pick a clean 32 MiB-aligned window in QEMU from UEFI MemoryMap. */
static int find_qemu_window(efi_boot_services *bs, uint64_t *out_base, uint64_t *out_size) {
    static unsigned char map_buf[32768];
    uint64_t map_sz = sizeof(map_buf), key = 0, desc_sz = 0;
    uint32_t desc_ver = 0;
    efi_status s = bs->get_memory_map(&map_sz, map_buf, &key, &desc_sz, &desc_ver);
    if (EFI_ERROR(s) || desc_sz < sizeof(efi_memory_descriptor)) return 0;

    uint64_t best_base = 0, best_size = 0;
    uint64_t high_base = 0, high_size = 0;
    for (uint64_t off = 0; off + desc_sz <= map_sz; off += desc_sz) {
        const efi_memory_descriptor *d = (const efi_memory_descriptor *)(map_buf + off);
        if (d->type != EfiConventionalMemory) continue;
        uint64_t start = d->phys_start;
        uint64_t end = start + d->pages * 4096ULL;
        uint64_t a = (start + V50_XNU_ALIGN - 1) & ~(V50_XNU_ALIGN - 1);
        uint64_t e = end & ~(V50_XNU_ALIGN - 1);
        if (e <= a) continue;
        /* Leave 32 MiB margin for UEFI FAT/GetMemoryMap pool allocations. */
        if (e - a > V50_XNU_MIN_WIN + V50_XNU_ALIGN) a += V50_XNU_ALIGN;
        if (e > 0x100000000ULL) {
            uint64_t ha = a > 0x100000000ULL ? a : 0x100000000ULL;
            if (e > ha && e - ha >= 0x40000000ULL) {
                uint64_t hsz = e - ha;
                if (hsz > V50_XNU_MAX_WIN) hsz = V50_XNU_MAX_WIN;
                if (hsz > high_size) {
                    high_base = ha;
                    high_size = hsz;
                }
            }
        }
        uint64_t sz = e - a;
        if (sz > V50_XNU_MAX_WIN) sz = V50_XNU_MAX_WIN;
        if (sz >= V50_XNU_MIN_WIN && sz > best_size) {
            best_base = a;
            best_size = sz;
        }
    }
    if (high_size >= 0x40000000ULL) {
        best_base = high_base;
        best_size = high_size;
    }
    if (best_size < V50_XNU_MIN_WIN) return 0;
    uint64_t alloc = best_base;
    s = bs->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &alloc);
    if (EFI_ERROR(s)) return 0;
    *out_base = best_base;
    *out_size = best_size;
    return 1;
}

static int parse_xnup(struct v50_ctx *ctx) {
    const unsigned char *buf = (const unsigned char *)(uintptr_t)ctx->payload_pa;
    uint64_t total = ctx->payload_size;
    if (total < 8 || rd_u32(buf) != PAYLOAD_MAGIC) return -1;
    uint64_t body_len = rd_u32(buf + 4);
    if (body_len < 24 || body_len > total - 8) return -2;
    uint64_t end = 8 + body_len;

    uint64_t off = 8;
    while (off + 24 <= end) {
        char name[17];
        v50_copy(name, buf + off, 16);
        name[16] = 0;
        if (name[0] == 0) break;
        uint64_t sz = rd_u32(buf + off + 16);
        uint64_t padded = (sz + 15ULL) & ~15ULL;
        off += 24;
        if (padded > end - off) return -3;
        if (seq(name, "kernel")) {
            ctx->kernel_off = off;
            ctx->kernel_size = sz;
        } else if (seq(name, "ramdisk")) {
            ctx->ramdisk_off = off;
            ctx->ramdisk_size = sz;
        } else if (seq(name, "cmdline")) {
            uint32_t n = sz < sizeof(ctx->cmdline) - 1 ? sz : sizeof(ctx->cmdline) - 1;
            v50_copy(ctx->cmdline, buf + off, n);
            ctx->cmdline[n] = 0;
            while (n && (ctx->cmdline[n - 1] == '\n' || ctx->cmdline[n - 1] == '\r' || ctx->cmdline[n - 1] == ' '))
                ctx->cmdline[--n] = 0;
        } else if (seq(name, "kernel.ver")) {
            uint32_t n = sz < sizeof(ctx->kver) - 1 ? sz : sizeof(ctx->kver) - 1;
            v50_copy(ctx->kver, buf + off, n);
            ctx->kver[n] = 0;
            while (n && (ctx->kver[n - 1] == '\n' || ctx->kver[n - 1] == '\r' || ctx->kver[n - 1] == ' '))
                ctx->kver[--n] = 0;
        }
        off += padded;
    }
    return ctx->kernel_size ? 0 : -4;
}

static int parse_macho(struct v50_ctx *ctx) {
    const unsigned char *k = (const unsigned char *)(uintptr_t)(ctx->payload_pa + ctx->kernel_off);
    uint64_t ksz = ctx->kernel_size;
    if (ksz < sizeof(struct mach_header_64) || rd_u32(k) != MH_MAGIC_64) return -1;
    uint32_t ncmds = rd_u32(k + 16);
    uint32_t sizeofcmds = rd_u32(k + 20);
    if (sizeofcmds > ksz - sizeof(struct mach_header_64)) return -2;

    uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
    const unsigned char *p = k + sizeof(struct mach_header_64);
    uint32_t consumed = 0;
    for (uint32_t i = 0; i < ncmds; i++) {
        if (consumed + 8 > sizeofcmds) return -3;
        uint32_t cmd = rd_u32(p);
        uint32_t cmdsize = rd_u32(p + 4);
        if (cmdsize < 8 || cmdsize > sizeofcmds - consumed) return -4;
        if (cmd == LC_SEGMENT_64 && cmdsize >= sizeof(struct segment_command_64)) {
            uint64_t vmaddr   = rd_u64(p + 24);
            uint64_t vmsize   = rd_u64(p + 32);
            uint64_t fileoff  = rd_u64(p + 40);
            uint64_t filesize = rd_u64(p + 48);
            if (vmsize) {
                if (vmaddr + vmsize < vmaddr || filesize > vmsize || fileoff > ksz || filesize > ksz - fileoff)
                    return -5;
                if (vmaddr < vm_lo) vm_lo = vmaddr;
                if (vmaddr + vmsize > vm_hi) vm_hi = vmaddr + vmsize;
            }
        } else if (cmd == LC_UNIXTHREAD && cmdsize >= 16 + 33 * 8) {
            entry_va = rd_u64(p + 16 + 32 * 8);
        }
        p += cmdsize;
        consumed += cmdsize;
    }
    if (!entry_va || vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || vm_hi <= vm_lo) return -6;
    uint64_t need = (vm_hi - KERNEL_VIRT_BASE) + ctx->ramdisk_size + 0x100000ULL;
    if (need > ctx->xnu_mem_size) return -7;
    ctx->vm_lo = vm_lo;
    ctx->vm_hi = vm_hi;
    ctx->entry_va = entry_va;
    return 0;
}

efi_status EFIAPI efi_main(efi_handle image, efi_system_table *st) {
    static struct v50_ctx ctx;
    v50_zero(&ctx, sizeof(ctx));

    if (!st || !st->bs) for (;;) __asm__ volatile("yield");
    efi_boot_services *bs = st->bs;
    g_con = st->con_out;

    if (bs->set_watchdog_timer) bs->set_watchdog_timer(0, 0, 0, NULL);

    efi_gop *gop = NULL;
    if (!EFI_ERROR(bs->locate_protocol(&g_gop_guid, NULL, (void **)&gop)) &&
        gop && gop->mode && gop->mode->info && gop->mode->fb_base) {
        ctx.fb_base = gop->mode->fb_base;
        ctx.fb_width = gop->mode->info->width;
        ctx.fb_height = gop->mode->info->height;
        ctx.fb_stride_px = gop->mode->info->pixels_per_scanline;
        if (ctx.fb_stride_px < ctx.fb_width) ctx.fb_stride_px = ctx.fb_width;
        ctx.fb_pitch = (uint64_t)ctx.fb_stride_px * 4ULL;
    }
    fb_clear(&ctx);
    ui_line(&ctx, 0, "ruby v0.53b - HakiOS SMP + Input + RO-UFS + RAMdiag", COL_GREEN);

    uint64_t el = 0;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(ctx.cntfrq));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(ctx.mpidr));
    if (el != 4ULL) ui_halt(&ctx, 2, "ERROR: Expected EL1", el);

    /* Platform detection: ruby live-FDT at 0x47C80000 first; else QEMU virt (62.5 MHz). */
    int has_live_fdt = 0;
    if (v50_va_readable(RUBY_LIVE_FDT_BASE) &&
        v50_be32((const unsigned char *)(uintptr_t)RUBY_LIVE_FDT_BASE) == 0xD00DFEEDU) {
        has_live_fdt = 1;
    }

    if (has_live_fdt) {
        const unsigned char *fdt = (const unsigned char *)(uintptr_t)RUBY_LIVE_FDT_BASE;
        RubyIrqHardware irq;
        struct v50_ram_window ram;
        if (!RubyIrqFdt(fdt, RUBY_LIVE_FDT_LIMIT, &irq)) {
            ui_halt(&ctx, 2, "ERROR: ruby live-FDT GIC/timer invalid", irq.code);
        }
        if (irq.dt_frequency != ctx.cntfrq) {
            ui_halt(&ctx, 2, "ERROR: CNTFRQ mismatch vs live-FDT", ctx.cntfrq);
        }
        uint32_t rc = V50FindRubyRamWindow(fdt, RUBY_LIVE_FDT_LIMIT, &ram);
        if (rc != 0) {
            ui_hex(&ctx, 2, "mblocks:  ", ram.mblock_count, COL_WHITE);
            ui_hex(&ctx, 3, "reserved: ", ram.reserved_count, COL_WHITE);
            ui_halt(&ctx, 4, "ERROR: No clean >=256MB RAM in live-FDT", rc);
        }
        struct v50_cpu_topology topo;
        uint32_t boot_id = (uint32_t)(ctx.mpidr & 0xFFFFu);
        uint32_t crc = V50ParseRubyCpus(fdt, RUBY_LIVE_FDT_LIMIT, boot_id, &topo);
        ctx.is_qemu = 0;
        ctx.psci_hvc = (crc == 0) ? topo.psci_hvc : 0u;
        ctx.gicd = irq.gicd;
        ctx.gicd_size = 0x40000ULL;
        ctx.gicr = irq.gicr;
        ctx.gicr_size = irq.gicr_bytes;
        ctx.io_base = irq.gicd;
        ctx.io_size = 0x10000000ULL;
        ctx.xnu_phys_base = ram.base;
        ctx.xnu_mem_size = ram.size;
        ctx.fallback_phys_base = ram.fallback_base;
        ctx.fallback_mem_size = ram.fallback_size;
        ctx.cpu_mpidr[0] = boot_id;
        ctx.cpu_count = 1u;
        ctx.cpu_skipped = 0u;
        if (crc == 0 && topo.count > 1u) {
            for (uint32_t i = 1u; i < topo.count && ctx.cpu_count < V50_MAX_CPUS; i++) {
                int64_t pst = V50PsciCall(PSCI_AFFINITY_INFO, topo.mpidr[i], 0, 0, ctx.psci_hvc);
                if (pst == 1 || pst == -1) {
                    ctx.cpu_mpidr[ctx.cpu_count++] = topo.mpidr[i];
                } else {
                    ctx.cpu_skipped++;
                }
            }
        }
        ui_line(&ctx, 1, "Platform: Redmi Note 12 Pro 5G (MT6877)", COL_CYAN);
    } else if (ctx.cntfrq == 62500000ULL) {
        uint32_t boot_id = (uint32_t)(ctx.mpidr & 0xFFFFu);
        ctx.is_qemu = 1;
        ctx.psci_hvc = 1;
        ctx.gicd = 0x08000000ULL;
        ctx.gicd_size = 0x10000ULL;
        ctx.gicr = 0x080A0000ULL;
        ctx.gicr_size = 0x200000ULL;
        ctx.io_base = 0x08000000ULL;
        ctx.io_size = 0x08000000ULL;
        ctx.cpu_mpidr[0] = boot_id;
        ctx.cpu_count = 1u;
        ctx.cpu_skipped = 0u;
        for (uint32_t id = 0; id < V50_MAX_CPUS && ctx.cpu_count < V50_MAX_CPUS; id++) {
            if (id == boot_id) continue;
            if (V50PsciCall(PSCI_AFFINITY_INFO, id, 0, 0, 1u) == 1) {
                ctx.cpu_mpidr[ctx.cpu_count++] = id;
            }
        }
        if (ctx.cpu_count == 1u) {
            for (uint32_t id = 0; id < V50_MAX_CPUS && ctx.cpu_count < V50_MAX_CPUS; id++) {
                uint32_t aff1 = id << 8;
                if (aff1 == boot_id) continue;
                if (V50PsciCall(PSCI_AFFINITY_INFO, aff1, 0, 0, 1u) == 1) {
                    ctx.cpu_mpidr[ctx.cpu_count++] = aff1;
                }
            }
        }
        ui_line(&ctx, 1, "Platform: QEMU virt (test mode)", COL_YELLOW);
    } else {
        ui_halt(&ctx, 2, "ERROR: Unknown hardware (no ruby FDT)", ctx.cntfrq);
    }

    /* Allocate physical stack (64 KiB), pram panic buffer (512 KiB), and payload buffer (16 MiB)
     * before claiming the QEMU XNU RAM window so AllocateAnyPages never competes with it. */
    uint64_t stack_pa = 0, pram_pa = 0, payload_pa = 0;
    efi_status s = bs->allocate_pages(AllocateAnyPages, EfiLoaderData, 16, &stack_pa);
    if (EFI_ERROR(s)) ui_halt(&ctx, 9, "ERROR: Alloc stack failed", s);
    ctx.stack_top = stack_pa + 0x10000ULL;

    s = bs->allocate_pages(AllocateAnyPages, EfiLoaderData, PANIC_LOG_SIZE >> 12, &pram_pa);
    if (EFI_ERROR(s)) ui_halt(&ctx, 9, "ERROR: Alloc pram failed", s);
    ctx.pram_pa = pram_pa;

    s = bs->allocate_pages(AllocateAnyPages, EfiLoaderData, PAYLOAD_MAX_BYTES >> 12, &payload_pa);
    if (EFI_ERROR(s)) ui_halt(&ctx, 9, "ERROR: Alloc 16MiB payload buf failed", s);
    ctx.payload_pa = payload_pa;

    if (ctx.is_qemu) {
        if (!find_qemu_window(bs, &ctx.xnu_phys_base, &ctx.xnu_mem_size)) {
            ui_halt(&ctx, 2, "ERROR: No >=256MB window in QEMU MemoryMap", 0);
        }
    }

    ui_hex(&ctx, 2, "CNTFRQ:   ", ctx.cntfrq, COL_WHITE);
    {
        char cpuline[64];
        uint32_t n = 0;
        append_str(cpuline, &n, sizeof(cpuline), "CPUs:     ");
        if (n + 1 < sizeof(cpuline)) cpuline[n++] = (char)('0' + ctx.cpu_count);
        cpuline[n] = 0;
        append_str(cpuline, &n, sizeof(cpuline), ctx.psci_hvc ? " (PSCI HVC)" : " (PSCI SMC)");
        if (ctx.cpu_skipped) {
            append_str(cpuline, &n, sizeof(cpuline), " skip=");
            if (n + 1 < sizeof(cpuline)) cpuline[n++] = (char)('0' + (ctx.cpu_skipped > 9 ? 9 : ctx.cpu_skipped));
            cpuline[n] = 0;
        }
        ui_line(&ctx, 3, cpuline, (ctx.cpu_count > 1u) ? COL_GREEN : COL_YELLOW);
    }
    ui_hex(&ctx, 4, "GICD:     ", ctx.gicd, COL_WHITE);
    ui_hex(&ctx, 5, "GICR:     ", ctx.gicr, COL_WHITE);
    ui_hex(&ctx, 6, "XNU base: ", ctx.xnu_phys_base, COL_GREEN);
    ui_hex(&ctx, 7, "XNU size: ", ctx.xnu_mem_size, COL_GREEN);

    /* Verify no overlap with XNU target window. */
    uint64_t img_pa = (uint64_t)(uintptr_t)__ImageBase;
    uint64_t img_sz = own_image_bytes();
    if (!img_sz ||
        v50_overlaps(img_pa, img_sz, ctx.xnu_phys_base, ctx.xnu_mem_size) ||
        v50_overlaps(stack_pa, 0x10000ULL, ctx.xnu_phys_base, ctx.xnu_mem_size) ||
        v50_overlaps(pram_pa, PANIC_LOG_SIZE, ctx.xnu_phys_base, ctx.xnu_mem_size) ||
        v50_overlaps(payload_pa, PAYLOAD_MAX_BYTES, ctx.xnu_phys_base, ctx.xnu_mem_size) ||
        v50_overlaps(ctx.fb_base, (uint64_t)ctx.fb_height * ctx.fb_pitch, ctx.xnu_phys_base, ctx.xnu_mem_size)) {
        ui_halt(&ctx, 9, "ERROR: Memory overlap with XNU window", img_pa);
    }
    if (ctx.fallback_phys_base && (
        v50_overlaps(img_pa, img_sz, ctx.fallback_phys_base, ctx.fallback_mem_size) ||
        v50_overlaps(stack_pa, 0x10000ULL, ctx.fallback_phys_base, ctx.fallback_mem_size) ||
        v50_overlaps(pram_pa, PANIC_LOG_SIZE, ctx.fallback_phys_base, ctx.fallback_mem_size) ||
        v50_overlaps(payload_pa, PAYLOAD_MAX_BYTES, ctx.fallback_phys_base, ctx.fallback_mem_size) ||
        v50_overlaps(ctx.fb_base, (uint64_t)ctx.fb_height * ctx.fb_pitch, ctx.fallback_phys_base, ctx.fallback_mem_size))) {
        ctx.fallback_phys_base = 0;
        ctx.fallback_mem_size = 0;
    }

    /* Read \PAYLOAD.XNP from the same FAT16 volume we were launched from. */
    efi_loaded_image *li = NULL;
    efi_simple_fs *fs = NULL;
    efi_file *root = NULL, *file = NULL;
    s = bs->handle_protocol(image, &g_li_guid, (void **)&li);
    if (EFI_ERROR(s) || !li || !li->device) ui_halt(&ctx, 9, "ERROR: LoadedImage protocol", s);
    s = bs->handle_protocol(li->device, &g_fs_guid, (void **)&fs);
    if (EFI_ERROR(s) || !fs || !fs->open_volume) ui_halt(&ctx, 9, "ERROR: SimpleFileSystem protocol", s);
    s = fs->open_volume(fs, &root);
    if (EFI_ERROR(s) || !root) ui_halt(&ctx, 9, "ERROR: Open FAT volume failed", s);

    static const char16 path[] = {'\\','P','A','Y','L','O','A','D','.','X','N','P',0};
    s = root->open(root, &file, path, 1, 0);
    if (EFI_ERROR(s) || !file) ui_halt(&ctx, 9, "ERROR: Open \\PAYLOAD.XNP failed", s);

    uint64_t got = 0;
    while (got < PAYLOAD_MAX_BYTES) {
        uint64_t chunk = PAYLOAD_MAX_BYTES - got;
        s = file->read(file, &chunk, (unsigned char *)(uintptr_t)payload_pa + got);
        if (EFI_ERROR(s)) ui_halt(&ctx, 9, "ERROR: Read \\PAYLOAD.XNP failed", s);
        if (chunk == 0) break;
        got += chunk;
    }
    file->close(file);
    root->close(root);
    ctx.payload_size = got;
    ui_hex(&ctx, 8, "PAYLOAD:  ", got, COL_WHITE);

    int pe = parse_xnup(&ctx);
    if (pe != 0) ui_halt(&ctx, 9, "ERROR: Invalid XNUP container", (uint64_t)(int64_t)pe);
    int me = parse_macho(&ctx);
    if (me != 0) ui_halt(&ctx, 9, "ERROR: Invalid XNU Mach-O", (uint64_t)(int64_t)me);

    ui_hex(&ctx, 9,  "Kernel:   ", ctx.kernel_size, COL_GREEN);
    ui_hex(&ctx, 10, "Ramdisk:  ", ctx.ramdisk_size, COL_GREEN);
    ui_hex(&ctx, 11, "FB base:  ", ctx.fb_base, COL_WHITE);
    ui_line(&ctx, 12, "Calling ExitBootServices...", COL_YELLOW);

    /* Clean all loader buffers to DRAM before ExitBootServices + cache disable. */
    v50_flush_cache(img_pa, img_sz);
    v50_flush_cache(payload_pa, got);
    v50_flush_cache((uint64_t)(uintptr_t)&ctx, sizeof(ctx));

    /* ExitBootServices (up to 3 retries per UEFI spec). */
    static unsigned char ebs_map[32768];
    for (int attempt = 0; attempt < 3; attempt++) {
        uint64_t msz = sizeof(ebs_map), key = 0, dsz = 0;
        uint32_t dver = 0;
        s = bs->get_memory_map(&msz, ebs_map, &key, &dsz, &dver);
        if (EFI_ERROR(s)) ui_halt(&ctx, 13, "ERROR: GetMemoryMap failed", s);
        s = bs->exit_boot_services(image, key);
        if (!EFI_ERROR(s)) {
            v50_flush_cache((uint64_t)(uintptr_t)&ctx, sizeof(ctx));
            V50EnterPhysical(&ctx, ctx.stack_top);
        }
    }
    ui_halt(&ctx, 13, "ERROR: ExitBootServices failed", s);
}
