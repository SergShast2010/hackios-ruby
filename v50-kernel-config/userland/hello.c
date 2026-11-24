/*
 * PID 1 for HakiOS-on-ruby bring-up (v0.55f: live TCP debug console + USB QMU DMA benchmark).
 *
 * Static arm64 Mach-O with no libc and no dyld: talks to XNU with raw system calls.
 * When usb_hold=<sec> is set in boot-args, opens three TCP listeners on 0.0.0.0:
 *   - 2323: interactive / one-shot debug shell (info, mem, usb, ufs, dmesg, ls, sysctl, touch, bench, return)
 *   - 2324: TX benchmark (phone -> host, deterministic pattern for bit-exact verification)
 *   - 2325: RX benchmark (host -> phone, streaming IEEE CRC-32 verification)
 */
typedef unsigned long      u64;
typedef long               s64;
typedef unsigned int       u32;
typedef int                s32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* Provide freestanding memcpy/memset in case clang emits calls under -Os */
void *
memcpy(void *dst, const void *src, u64 n)
{
	u8 *d = (u8 *)dst;
	const u8 *s = (const u8 *)src;
	while (n--) *d++ = *s++;
	return dst;
}

void *
memset(void *dst, int c, u64 n)
{
	u8 *d = (u8 *)dst;
	u8 v = (u8)c;
	while (n--) *d++ = v;
	return dst;
}

/* XNU arm64 syscall: returns -errno when carry flag is set, otherwise >= 0 */
static long
syscall3_err(long num, long a, long b, long c)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	long err = 0;
	__asm__ volatile (
		"svc #0x80\n"
		"cset %1, cs\n"
		: "+r" (x0), "=r" (err)
		: "r" (x16), "r" (x1), "r" (x2)
		: "memory", "cc"
	);
	return err ? -x0 : x0;
}

static long
syscall6_err(long num, long a, long b, long c, long d, long e, long f)
{
	register long x16 __asm__("x16") = num;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	long err = 0;
	__asm__ volatile (
		"svc #0x80\n"
		"cset %1, cs\n"
		: "+r" (x0), "=r" (err)
		: "r" (x16), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5)
		: "memory", "cc"
	);
	return err ? -x0 : x0;
}

#define SYS_exit            1
#define SYS_read            3
#define SYS_write           4
#define SYS_open            5
#define SYS_close           6
#define SYS_getpid          20
#define SYS_getuid          24
#define SYS_accept          30
#define SYS_fcntl           92
#define SYS_select          93
#define SYS_socket          97
#define SYS_bind            104
#define SYS_setsockopt      105
#define SYS_listen          106
#define SYS_gettimeofday    116
#define SYS_shutdown        134
#define SYS_sysctlbyname    274
#define SYS_getdirentries64 344

#define O_RDONLY            0x0000
#define O_RDWR              0x0002
#define O_NONBLOCK          0x0004

#define F_GETFL             3
#define F_SETFL             4

#define EAGAIN              35
#define EINTR               4

#define AF_INET             2
#define SOCK_STREAM         1
#define IPPROTO_TCP         6
#define SOL_SOCKET          0xffff
#define SO_REUSEADDR        0x0004
#define SO_SNDBUF           0x1001
#define SO_RCVBUF           0x1002
#define SO_NOSIGPIPE        0x1022
#define TCP_NODELAY         0x0001
#define SHUT_WR             1

#define PORT_CONSOLE        2323
#define PORT_TXBENCH        2324
#define PORT_RXBENCH        2325

struct sockaddr_in {
	u8  sin_len;
	u8  sin_family;
	u16 sin_port;
	u32 sin_addr;
	u8  sin_zero[8];
};

struct tv64 {
	s64 tv_sec;
	s32 tv_usec;
	s32 _pad;
};

struct dirent64_hdr {
	u64 d_ino;
	u64 d_seekoff;
	u16 d_reclen;
	u16 d_namlen;
	u8  d_type;
	char d_name[1];
};

/* Mirror of bsd/sys/hakios.input.h */
struct hakios_touch {
	u64 time;
	u32 type;
	u32 finger;
	u32 x;
	u32 y;
};

#define HAKIOS_TOUCH_DOWN   1
#define HAKIOS_TOUCH_MOVE   2
#define HAKIOS_TOUCH_UP     3
#define HAKIOS_BUTTON_DOWN  4
#define HAKIOS_BUTTON_UP    5

#define HAKIOS_BTN_LOCK     1
#define HAKIOS_BTN_VOLUP    2
#define HAKIOS_BTN_VOLDOWN  3
#define HAKIOS_BTN_HOME     4

/* Static buffers in .bss to keep PID 1 stack tiny */
static u8   io_buf[65536];
static char klog_buf[65536];
static char dir_buf[8192];
static char ufs_buf[4096];
static u32  crc_tab[256];
static int  crc_ready;
static int  pattern_ready;
static char last_tx_bench[160] = "TXBENCH: (not run yet)\n";
static char last_rx_bench[160] = "RXBENCH: (not run yet)\n";

/* ---- Tiny string / formatting helpers ---- */

static u64
slen(const char *s)
{
	u64 n = 0;
	while (s[n]) n++;
	return n;
}

static int
streq(const char *a, const char *b)
{
	while (*a && *b) {
		if (*a != *b) return 0;
		a++; b++;
	}
	return *a == *b;
}

static int
strstarts(const char *s, const char *pfx)
{
	while (*pfx) {
		if (*s++ != *pfx++) return 0;
	}
	return 1;
}

static u64
parse_u64(const char *s)
{
	u64 v = 0;
	while (*s == ' ' || *s == '\t') s++;
	while (*s >= '0' && *s <= '9') {
		v = v * 10ULL + (u64)(*s - '0');
		s++;
	}
	return v;
}

static u16
htons16(u16 x)
{
	return (u16)((x >> 8) | (x << 8));
}

static long
write_all(int fd, const void *buf, u64 len)
{
	const u8 *p = (const u8 *)buf;
	u64 off = 0;
	int spins = 0;
	while (off < len) {
		long n = syscall3_err(SYS_write, fd, (long)(p + off), (long)(len - off));
		if (n > 0) {
			off += (u64)n;
			spins = 0;
		} else if (n == -EAGAIN || n == -EINTR) {
			if (++spins > 2000) return -1;
			struct tv64 tv = { 0, 1000, 0 };
			(void)syscall6_err(SYS_select, 0, 0, 0, 0, (long)&tv, 0);
		} else {
			return -1;
		}
	}
	return (long)off;
}

static void
put(int fd, const char *s)
{
	if (fd < 0 || !s) return;
	(void)write_all(fd, s, slen(s));
}

static void
put_u64(int fd, u64 v)
{
	char buf[24];
	int i = (int)sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + (v % 10ULL));
		v /= 10ULL;
	} while (v);
	put(fd, buf + i);
}

static void
put_hex32(int fd, u32 v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[9];
	for (int i = 7; i >= 0; i--) {
		buf[i] = hex[v & 0xfu];
		v >>= 4;
	}
	buf[8] = 0;
	put(fd, buf);
}

static void
put_hex64(int fd, u64 v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[17];
	for (int i = 15; i >= 0; i--) {
		buf[i] = hex[v & 0xfu];
		v >>= 4;
	}
	buf[16] = 0;
	put(fd, buf);
}

static char *
app_str(char *dst, char *end, const char *s)
{
	while (*s && dst + 1 < end) *dst++ = *s++;
	*dst = 0;
	return dst;
}

static char *
app_u64(char *dst, char *end, u64 v)
{
	char buf[24];
	int i = (int)sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + (v % 10ULL));
		v /= 10ULL;
	} while (v);
	return app_str(dst, end, buf + i);
}

static char *
app_hex32(char *dst, char *end, u32 v)
{
	static const char hex[] = "0123456789abcdef";
	char buf[9];
	for (int i = 7; i >= 0; i--) {
		buf[i] = hex[v & 0xfu];
		v >>= 4;
	}
	buf[8] = 0;
	return app_str(dst, end, buf);
}

static char *
app_mbps(char *dst, char *end, u64 bytes, u64 ms)
{
	if (ms == 0) ms = 1;
	/* MB/s * 100 = (bytes * 100 * 1000) / (ms * 1048576) */
	u64 hundredths = (bytes / 1024ULL) * 100000ULL / (ms * 1024ULL);
	u64 whole = hundredths / 100ULL;
	u64 frac = hundredths % 100ULL;
	dst = app_u64(dst, end, whole);
	dst = app_str(dst, end, ".");
	if (frac < 10ULL) dst = app_str(dst, end, "0");
	dst = app_u64(dst, end, frac);
	return dst;
}

static u64
now_ms(void)
{
	struct tv64 tv = { 0, 0, 0 };
	(void)syscall3_err(SYS_gettimeofday, (long)&tv, 0, 0);
	return (u64)tv.tv_sec * 1000ULL + (u64)((u32)tv.tv_usec / 1000u);
}

static void
sleep_ms(u32 ms)
{
	struct tv64 tv;
	tv.tv_sec = (s64)(ms / 1000u);
	tv.tv_usec = (s32)((ms % 1000u) * 1000u);
	tv._pad = 0;
	(void)syscall6_err(SYS_select, 0, 0, 0, 0, (long)&tv, 0);
}

/* ---- IEEE 802.3 CRC-32 & deterministic TX pattern ---- */

static void
crc32_init(void)
{
	if (crc_ready) return;
	for (u32 i = 0; i < 256u; i++) {
		u32 c = i;
		for (int k = 0; k < 8; k++) {
			c = (c & 1u) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
		}
		crc_tab[i] = c;
	}
	crc_ready = 1;
}

static u32
crc32_update(u32 crc, const u8 *buf, u64 len)
{
	for (u64 i = 0; i < len; i++) {
		crc = crc_tab[(crc ^ buf[i]) & 0xffu] ^ (crc >> 8);
	}
	return crc;
}

static void
fill_tx_pattern(void)
{
	if (pattern_ready) return;
	for (u32 i = 0; i < 65536u; i++) {
		io_buf[i] = (u8)((((i * 131u) ^ ((i >> 8) * 17u)) + 0x5au) & 0xffu);
	}
	pattern_ready = 1;
}

/* ---- Sysctl & boot-arg helpers ---- */

static void
show_int(int fd, const char *name)
{
	int value = 0;
	u64 size = sizeof(value);
	if (syscall6_err(SYS_sysctlbyname, (long)name, (long)slen(name), (long)&value, (long)&size, 0, 0) == 0) {
		put(fd, name);
		put(fd, "=");
		if (value < 0) { put(fd, "-"); value = -value; }
		put_u64(fd, (u64)value);
		put(fd, "\n");
	}
}

static void
show_u64_mb(int fd, const char *name)
{
	u64 value = 0;
	u64 size = sizeof(value);
	if (syscall6_err(SYS_sysctlbyname, (long)name, (long)slen(name), (long)&value, (long)&size, 0, 0) == 0) {
		put(fd, name);
		put(fd, "=");
		put_u64(fd, value);
		put(fd, " (");
		put_u64(fd, value >> 20);
		put(fd, " MB)\n");
	}
}

static void
show_str_sysctl(int fd, const char *label, const char *name, char *buf, u64 bufsz)
{
	u64 sz = bufsz - 1;
	long rc = syscall6_err(SYS_sysctlbyname, (long)name, (long)slen(name), (long)buf, (long)&sz, 0, 0);
	if (label) {
		put(fd, label);
		put(fd, "=");
	}
	if (rc != 0) {
		put(fd, "(err ");
		put_u64(fd, (u64)(-rc));
		put(fd, ")\n");
		return;
	}
	if (sz > bufsz - 1) sz = bufsz - 1;
	buf[sz] = 0;
	/* Trim trailing NULs from string sysctls */
	while (sz > 0 && buf[sz - 1] == 0) sz--;
	if (sz > 0) (void)write_all(fd, buf, sz);
	if (sz == 0 || buf[sz - 1] != '\n') put(fd, "\n");
}

static long
bootarg_u64(const char *key)
{
	char args[700];
	u64 size = sizeof(args) - 1;
	u64 klen = slen(key);
	if (syscall6_err(SYS_sysctlbyname, (long)"kern.bootargs", 13, (long)args, (long)&size, 0, 0) != 0) {
		return 0;
	}
	args[size] = 0;
	for (char *c = args; *c; c++) {
		if (c > args && c[-1] != ' ') continue;
		u64 i = 0;
		while (i < klen && c[i] == key[i]) i++;
		if (i == klen && c[i] == '=') {
			return (long)parse_u64(c + i + 1);
		}
	}
	return 0;
}

static const char *
btn_name(u32 b)
{
	switch (b) {
	case HAKIOS_BTN_HOME:    return "HOME";
	case HAKIOS_BTN_LOCK:    return "POWER";
	case HAKIOS_BTN_VOLUP:   return "VOLUP";
	case HAKIOS_BTN_VOLDOWN: return "VOLDOWN";
	default:                 return "UNKNOWN";
	}
}

/* ---- Socket helpers ---- */

static void
set_sockopt_int(int fd, int level, int name, int val)
{
	(void)syscall6_err(SYS_setsockopt, fd, level, name, (long)&val, (long)sizeof(val), 0);
}

static void
set_nonblock(int fd, int nb)
{
	long fl = syscall3_err(SYS_fcntl, fd, F_GETFL, 0);
	if (fl < 0) fl = 0;
	if (nb) fl |= O_NONBLOCK;
	else fl &= ~O_NONBLOCK;
	(void)syscall3_err(SYS_fcntl, fd, F_SETFL, fl);
}

static int
make_listener(u16 port)
{
	int s = (int)syscall3_err(SYS_socket, AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -1;
	set_sockopt_int(s, SOL_SOCKET, SO_REUSEADDR, 1);
	set_sockopt_int(s, SOL_SOCKET, SO_NOSIGPIPE, 1);

	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_len = (u8)sizeof(sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons16(port);
	sa.sin_addr = 0; /* INADDR_ANY */

	if (syscall3_err(SYS_bind, s, (long)&sa, (long)sizeof(sa)) < 0) {
		(void)syscall3_err(SYS_close, s, 0, 0);
		return -1;
	}
	if (syscall3_err(SYS_listen, s, 4, 0) < 0) {
		(void)syscall3_err(SYS_close, s, 0, 0);
		return -1;
	}
	set_nonblock(s, 1);
	return s;
}

/* ---- TCP benchmark handlers ---- */

static void
handle_txbench(int con_out, int cfd)
{
	set_nonblock(cfd, 0);
	set_sockopt_int(cfd, SOL_SOCKET, SO_NOSIGPIPE, 1);
	set_sockopt_int(cfd, SOL_SOCKET, SO_SNDBUF, 262144);

	/* Optional 150 ms window for client to send MB count (e.g. "16\n"); default 16 MB */
	u64 mb = 16ULL;
	u32 rfds[32];
	memset(rfds, 0, sizeof(rfds));
	rfds[cfd >> 5] |= (1u << (cfd & 31));
	struct tv64 tv = { 0, 150000, 0 };
	if (syscall6_err(SYS_select, cfd + 1, (long)rfds, 0, 0, (long)&tv, 0) > 0) {
		char req[32];
		long rn = syscall3_err(SYS_read, cfd, (long)req, (long)(sizeof(req) - 1));
		if (rn > 0) {
			req[rn] = 0;
			u64 v = parse_u64(req);
			if (v >= 1ULL && v <= 256ULL) mb = v;
		}
	}

	fill_tx_pattern();
	u64 total = mb * 1048576ULL;
	u64 sent = 0;
	u64 t0 = now_ms();
	while (sent < total) {
		u64 chunk = total - sent;
		if (chunk > sizeof(io_buf)) chunk = sizeof(io_buf);
		long w = write_all(cfd, io_buf, chunk);
		if (w < 0) break;
		sent += (u64)w;
	}
	(void)syscall3_err(SYS_shutdown, cfd, SHUT_WR, 0);
	u64 dt = now_ms() - t0;
	(void)syscall3_err(SYS_close, cfd, 0, 0);

	char *p = last_tx_bench;
	char *end = last_tx_bench + sizeof(last_tx_bench);
	p = app_str(p, end, "TXBENCH ");
	p = app_str(p, end, sent == total ? "OK" : "PARTIAL");
	p = app_str(p, end, " bytes=");
	p = app_u64(p, end, sent);
	p = app_str(p, end, " ms=");
	p = app_u64(p, end, dt);
	p = app_str(p, end, " speed=");
	p = app_mbps(p, end, sent, dt);
	p = app_str(p, end, " MB/s\n");
	put(con_out, last_tx_bench);
}

static void
handle_rxbench(int con_out, int cfd)
{
	set_nonblock(cfd, 0);
	set_sockopt_int(cfd, SOL_SOCKET, SO_NOSIGPIPE, 1);
	set_sockopt_int(cfd, SOL_SOCKET, SO_RCVBUF, 262144);
	crc32_init();

	u64 total = 0;
	u32 crc = 0xffffffffu;
	u64 t0 = 0;
	int started = 0;
	int ok = 1;

	for (;;) {
		long n = syscall3_err(SYS_read, cfd, (long)io_buf, (long)sizeof(io_buf));
		if (n > 0) {
			if (!started) {
				t0 = now_ms();
				started = 1;
			}
			crc = crc32_update(crc, io_buf, (u64)n);
			total += (u64)n;
		} else if (n == 0) {
			break;
		} else if (n == -EINTR) {
			continue;
		} else {
			ok = 0;
			break;
		}
	}
	u64 dt = started ? (now_ms() - t0) : 0;
	crc ^= 0xffffffffu;

	char *p = last_rx_bench;
	char *end = last_rx_bench + sizeof(last_rx_bench);
	p = app_str(p, end, "RXBENCH ");
	p = app_str(p, end, ok ? "OK" : "ERR");
	p = app_str(p, end, " bytes=");
	p = app_u64(p, end, total);
	p = app_str(p, end, " ms=");
	p = app_u64(p, end, dt);
	p = app_str(p, end, " speed=");
	p = app_mbps(p, end, total, dt);
	p = app_str(p, end, " MB/s crc32=0x");
	p = app_hex32(p, end, crc);
	p = app_str(p, end, "\n");

	(void)write_all(cfd, last_rx_bench, slen(last_rx_bench));
	(void)syscall3_err(SYS_shutdown, cfd, SHUT_WR, 0);
	(void)syscall3_err(SYS_close, cfd, 0, 0);
	put(con_out, last_rx_bench);
}

/* ---- Interactive TCP console commands ---- */

static void
cmd_ls(int out, const char *path)
{
	if (!path || !*path) path = "/dev";
	int dfd = (int)syscall3_err(SYS_open, (long)path, O_RDONLY, 0);
	if (dfd < 0) {
		put(out, "ls: open failed err=");
		put_u64(out, (u64)(-dfd));
		put(out, "\n");
		return;
	}
	s64 pos = 0;
	u64 count = 0;
	for (;;) {
		long n = syscall6_err(SYS_getdirentries64, dfd, (long)dir_buf, (long)sizeof(dir_buf), (long)&pos, 0, 0);
		if (n <= 0) break;
		u64 off = 0;
		while (off + 21ULL <= (u64)n) {
			const struct dirent64_hdr *de = (const struct dirent64_hdr *)(dir_buf + off);
			if (de->d_reclen == 0 || off + de->d_reclen > (u64)n) break;
			char t = '?';
			if (de->d_type == 4) t = 'd';
			else if (de->d_type == 8) t = 'f';
			else if (de->d_type == 2) t = 'c';
			else if (de->d_type == 6) t = 'b';
			else if (de->d_type == 10) t = 'l';
			char pfx[3] = { t, ' ', 0 };
			put(out, pfx);
			if (de->d_namlen > 0 && off + 21ULL + de->d_namlen <= (u64)n) {
				(void)write_all(out, de->d_name, de->d_namlen);
			}
			put(out, "\n");
			count++;
			off += de->d_reclen;
		}
	}
	(void)syscall3_err(SYS_close, dfd, 0, 0);
	put(out, "total=");
	put_u64(out, count);
	put(out, "\n");
}

static void
cmd_sysctl(int out, const char *name)
{
	while (*name == ' ') name++;
	if (!*name) {
		put(out, "usage: sysctl <name>\n");
		return;
	}
	u64 sz = sizeof(ufs_buf) - 1;
	long rc = syscall6_err(SYS_sysctlbyname, (long)name, (long)slen(name), (long)ufs_buf, (long)&sz, 0, 0);
	if (rc != 0) {
		put(out, "sysctl error=");
		put_u64(out, (u64)(-rc));
		put(out, "\n");
		return;
	}
	if (sz == 4 && (ufs_buf[0] < 32 || ufs_buf[0] > 126)) {
		s32 v = 0;
		memcpy(&v, ufs_buf, 4);
		if (v < 0) { put(out, "-"); put_u64(out, (u64)(-(s64)v)); }
		else put_u64(out, (u64)v);
		put(out, " (0x"); put_hex32(out, (u32)v); put(out, ")\n");
		return;
	}
	if (sz == 8 && (ufs_buf[0] < 32 || ufs_buf[0] > 126)) {
		u64 v = 0;
		memcpy(&v, ufs_buf, 8);
		put_u64(out, v);
		put(out, " (0x"); put_hex64(out, v); put(out, ")\n");
		return;
	}
	ufs_buf[sz] = 0;
	while (sz > 0 && ufs_buf[sz - 1] == 0) sz--;
	if (sz > 0) (void)write_all(out, ufs_buf, sz);
	if (sz == 0 || ufs_buf[sz - 1] != '\n') put(out, "\n");
}

static void
cmd_touch(int out, const char *arg)
{
	while (*arg == ' ') arg++;
	u64 x = parse_u64(arg);
	while (*arg >= '0' && *arg <= '9') arg++;
	while (*arg == ' ') arg++;
	u64 y = parse_u64(arg);
	int tfd = (int)syscall3_err(SYS_open, (long)"/dev/hakios-touch", O_RDWR, 0);
	if (tfd < 0) {
		put(out, "touch: cannot open /dev/hakios-touch\n");
		return;
	}
	struct hakios_touch ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = HAKIOS_TOUCH_DOWN;
	ev.finger = 0;
	ev.x = (u32)x;
	ev.y = (u32)y;
	long w1 = syscall3_err(SYS_write, tfd, (long)&ev, (long)sizeof(ev));
	sleep_ms(20);
	ev.type = HAKIOS_TOUCH_UP;
	long w2 = syscall3_err(SYS_write, tfd, (long)&ev, (long)sizeof(ev));
	(void)syscall3_err(SYS_close, tfd, 0, 0);
	put(out, "touch posted x="); put_u64(out, x);
	put(out, " y="); put_u64(out, y);
	put(out, " rc="); put_u64(out, (u64)(w1 > 0 && w2 > 0 ? 0 : 1));
	put(out, "\n");
}

/* Returns: 0 = continue, 1 = close client, 2 = return to UEFI */
static int
exec_console_cmd(int out, char *line)
{
	/* Strip leading/trailing whitespace & CR/LF */
	while (*line == ' ' || *line == '\t') line++;
	u64 len = slen(line);
	while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
	       line[len - 1] == ' ' || line[len - 1] == '\t')) {
		line[--len] = 0;
	}
	if (len == 0) return 0;

	if (streq(line, "help") || streq(line, "?")) {
		put(out,
		    "HakiOS ruby TCP console commands:\n"
		    "  info            - kernel version, CPU, RAM, drivers, boot-args, uptime\n"
		    "  mem             - live XNU memory breakdown (kern.hakios_memory)\n"
		    "  usb             - live MT6877 USB MTU3/QMU/ECM counters\n"
		    "  ufs             - read-only LUN2 GPT partition table\n"
		    "  dmesg [bytes]   - tail of kernel log (default 8192, max 65535)\n"
		    "  ls [path]       - list directory (default /dev)\n"
		    "  sysctl <name>   - read any sysctl value\n"
		    "  touch <x> <y>   - inject virtual tap into /dev/hakios-touch\n"
		    "  bench           - show last TX/RX USB DMA benchmark results\n"
		    "  ping            - reply pong\n"
		    "  quit            - close this TCP session\n"
		    "  return          - cleanly exit PID 1 back to UEFI summary\n"
		    "Benchmark ports: :2324 = TX (phone->host), :2325 = RX (host->phone + CRC32)\n");
	} else if (streq(line, "ping")) {
		put(out, "pong\n");
	} else if (streq(line, "info")) {
		show_str_sysctl(out, "kernel.ver", "kern.hakios.version", ufs_buf, sizeof(ufs_buf));
		show_str_sysctl(out, "drivers", "kern.hakios.drivers", ufs_buf, sizeof(ufs_buf));
		show_int(out, "hw.ncpu");
		show_int(out, "hw.activecpu");
		show_int(out, "hw.physicalcpu");
		show_u64_mb(out, "hw.memsize");
		put(out, "uptime_ms="); put_u64(out, now_ms()); put(out, "\n");
		show_str_sysctl(out, "bootargs", "kern.bootargs", ufs_buf, sizeof(ufs_buf));
	} else if (streq(line, "mem")) {
		show_str_sysctl(out, 0, "kern.hakios_memory", ufs_buf, sizeof(ufs_buf));
	} else if (streq(line, "usb")) {
		show_str_sysctl(out, 0, "kern.hakios.usb_stats", ufs_buf, sizeof(ufs_buf));
	} else if (streq(line, "ufs")) {
		show_str_sysctl(out, 0, "kern.hakios.ufs_parts", ufs_buf, sizeof(ufs_buf));
	} else if (streq(line, "dmesg") || strstarts(line, "dmesg ")) {
		u64 want = 8192ULL;
		if (line[5] == ' ') {
			u64 v = parse_u64(line + 6);
			if (v >= 256ULL && v < sizeof(klog_buf)) want = v;
		}
		show_str_sysctl(out, 0, "kern.hakios.klog", klog_buf, want + 1ULL);
	} else if (streq(line, "ls") || strstarts(line, "ls ")) {
		cmd_ls(out, line[2] == ' ' ? line + 3 : "/dev");
	} else if (strstarts(line, "sysctl ")) {
		cmd_sysctl(out, line + 7);
	} else if (strstarts(line, "touch ")) {
		cmd_touch(out, line + 6);
	} else if (streq(line, "bench")) {
		put(out, last_tx_bench);
		put(out, last_rx_bench);
	} else if (streq(line, "quit") || streq(line, "exit") || streq(line, "bye")) {
		put(out, "bye\n");
		return 1;
	} else if (streq(line, "return")) {
		put(out, "Returning to UEFI summary...\n");
		return 2;
	} else {
		put(out, "unknown command: ");
		put(out, line);
		put(out, " (type 'help')\n");
	}
	return 0;
}

static void
run_tcp_services(int con_out, long hold_s)
{
	int l_con = make_listener(PORT_CONSOLE);
	int l_tx  = make_listener(PORT_TXBENCH);
	int l_rx  = make_listener(PORT_RXBENCH);

	put(con_out, "TCP READY (hold=");
	put_u64(con_out, (u64)hold_s);
	put(con_out, "s): shell=:2323 tx=:2324 rx=:2325 (fd=");
	put_u64(con_out, (u64)(l_con >= 0 ? l_con : 0));
	put(con_out, "/");
	put_u64(con_out, (u64)(l_tx >= 0 ? l_tx : 0));
	put(con_out, "/");
	put_u64(con_out, (u64)(l_rx >= 0 ? l_rx : 0));
	put(con_out, "; VolUp+VolDown or 'return')\n");

	int cfd = -1;
	char line_buf[512];
	u64 line_len = 0;

	long max_idle_ticks = hold_s * 20L; /* 50ms per tick */
	long idle_ticks = 0;
	int want_return = 0;

	while (!want_return && idle_ticks < max_idle_ticks) {
		u32 rfds[32];
		memset(rfds, 0, sizeof(rfds));
		int maxfd = -1;
		if (l_con >= 0) { rfds[l_con >> 5] |= (1u << (l_con & 31)); if (l_con > maxfd) maxfd = l_con; }
		if (l_tx  >= 0) { rfds[l_tx  >> 5] |= (1u << (l_tx  & 31)); if (l_tx  > maxfd) maxfd = l_tx;  }
		if (l_rx  >= 0) { rfds[l_rx  >> 5] |= (1u << (l_rx  & 31)); if (l_rx  > maxfd) maxfd = l_rx;  }
		if (cfd   >= 0) { rfds[cfd   >> 5] |= (1u << (cfd   & 31)); if (cfd   > maxfd) maxfd = cfd;   }

		struct tv64 tv = { 0, 50000, 0 }; /* 50 ms */
		long ready = syscall6_err(SYS_select, maxfd + 1, (long)rfds, 0, 0, (long)&tv, 0);
		if (ready <= 0) {
			idle_ticks++;
			continue;
		}
		idle_ticks = 0;

		/* Accept new console client */
		if (l_con >= 0 && (rfds[l_con >> 5] & (1u << (l_con & 31)))) {
			int nfd = (int)syscall3_err(SYS_accept, l_con, 0, 0);
			if (nfd >= 0) {
				if (cfd >= 0) {
					(void)syscall3_err(SYS_close, cfd, 0, 0);
				}
				cfd = nfd;
				line_len = 0;
				set_nonblock(cfd, 1);
				set_sockopt_int(cfd, SOL_SOCKET, SO_NOSIGPIPE, 1);
				set_sockopt_int(cfd, IPPROTO_TCP, TCP_NODELAY, 1);
				put(con_out, "TCP console: client connected\n");
				put(cfd, "HakiOS ruby TCP console (type 'help')\nhakios> ");
			}
		}

		/* Handle active console client input */
		if (cfd >= 0 && (rfds[cfd >> 5] & (1u << (cfd & 31)))) {
			char rx[256];
			long n = syscall3_err(SYS_read, cfd, (long)rx, (long)sizeof(rx));
			if (n > 0) {
				int close_client = 0;
				for (long i = 0; i < n; i++) {
					char ch = rx[i];
					if (ch == '\n') {
						line_buf[line_len] = 0;
						int act = exec_console_cmd(cfd, line_buf);
						line_len = 0;
						if (act == 2) {
							want_return = 1;
							close_client = 1;
							break;
						} else if (act == 1) {
							close_client = 1;
							break;
						} else {
							put(cfd, "hakios> ");
						}
					} else if (ch != '\r') {
						if (line_len + 1 < sizeof(line_buf)) {
							line_buf[line_len++] = ch;
						}
					}
				}
				if (close_client) {
					(void)syscall3_err(SYS_shutdown, cfd, SHUT_WR, 0);
					(void)syscall3_err(SYS_close, cfd, 0, 0);
					cfd = -1;
					line_len = 0;
				}
			} else if (n == 0) {
				if (line_len > 0) {
					line_buf[line_len] = 0;
					int act = exec_console_cmd(cfd, line_buf);
					if (act == 2) want_return = 1;
				}
				(void)syscall3_err(SYS_close, cfd, 0, 0);
				cfd = -1;
				line_len = 0;
			} else if (n != -EAGAIN && n != -EINTR) {
				(void)syscall3_err(SYS_close, cfd, 0, 0);
				cfd = -1;
				line_len = 0;
			}
		}

		/* Accept & run TX benchmark (phone -> host) */
		if (!want_return && l_tx >= 0 && (rfds[l_tx >> 5] & (1u << (l_tx & 31)))) {
			int bfd = (int)syscall3_err(SYS_accept, l_tx, 0, 0);
			if (bfd >= 0) {
				handle_txbench(con_out, bfd);
			}
		}

		/* Accept & run RX benchmark (host -> phone + CRC32) */
		if (!want_return && l_rx >= 0 && (rfds[l_rx >> 5] & (1u << (l_rx & 31)))) {
			int bfd = (int)syscall3_err(SYS_accept, l_rx, 0, 0);
			if (bfd >= 0) {
				handle_rxbench(con_out, bfd);
			}
		}
	}

	if (cfd   >= 0) (void)syscall3_err(SYS_close, cfd, 0, 0);
	if (l_con >= 0) (void)syscall3_err(SYS_close, l_con, 0, 0);
	if (l_tx  >= 0) (void)syscall3_err(SYS_close, l_tx, 0, 0);
	if (l_rx  >= 0) (void)syscall3_err(SYS_close, l_rx, 0, 0);

	/* Emit final USB stats into console log before exiting to UEFI summary */
	show_str_sysctl(con_out, 0, "kern.hakios.usb_stats", ufs_buf, sizeof(ufs_buf));
}

void
start(void)
{
	int fd = (int)syscall3_err(SYS_open, (long)"/dev/console", O_RDWR, 0);

	put(fd, "\nHello from user space on Darwin!\n");
	put(fd, "I am process ");
	put_u64(fd, (u64)syscall3_err(SYS_getpid, 0, 0, 0));
	put(fd, ", uid ");
	put_u64(fd, (u64)syscall3_err(SYS_getuid, 0, 0, 0));
	put(fd, ", console fd ");
	put_u64(fd, (u64)fd);
	put(fd, ".\n");

	show_str_sysctl(fd, "kernel.ver", "kern.hakios.version", ufs_buf, sizeof(ufs_buf));
	show_int(fd, "hw.ncpu");
	show_int(fd, "hw.activecpu");
	show_int(fd, "hw.physicalcpu");
	show_u64_mb(fd, "hw.memsize");

	{
		long rdfd = syscall3_err(SYS_open, (long)"/dev/hakios-ramdiag", 0, 0);
		if (rdfd >= 0) (void)syscall3_err(SYS_close, rdfd, 0, 0);
	}

	long wait_s = bootarg_u64("hello_wait");
	if (wait_s > 0) {
		int tfd = (int)syscall3_err(SYS_open, (long)"/dev/hakios-touch", O_RDWR | O_NONBLOCK, 0);
		if (tfd >= 0) {
			put(fd, "INPUT READY (/dev/hakios-touch fd=");
			put_u64(fd, (u64)tfd);
			put(fd, "): press Vol+/Vol-/Power or touch screen (");
			put_u64(fd, (u64)wait_s);
			put(fd, "s; VolUp+VolDown=exit)\n");
		} else {
			put(fd, "INPUT ERROR: cannot open /dev/hakios-touch\n");
		}

		u64 btn_cnt = 0, touch_cnt = 0, move_div = 0;
		long ticks = wait_s * 20;
		for (long tick = 0; tick < ticks; tick++) {
			if (tfd >= 0) {
				struct hakios_touch ev[16];
				long n = syscall3_err(SYS_read, tfd, (long)ev, (long)sizeof(ev));
				if (n > 0) {
					long cnt = n / (long)sizeof(struct hakios_touch);
					for (long i = 0; i < cnt; i++) {
						if (ev[i].type == HAKIOS_BUTTON_DOWN || ev[i].type == HAKIOS_BUTTON_UP) {
							btn_cnt++;
							put(fd, "INPUT BTN: ");
							put(fd, btn_name(ev[i].finger));
							put(fd, ev[i].type == HAKIOS_BUTTON_DOWN ? " DOWN\n" : " UP\n");
						} else if (ev[i].type == HAKIOS_TOUCH_DOWN || ev[i].type == HAKIOS_TOUCH_UP ||
						           ev[i].type == HAKIOS_TOUCH_MOVE) {
							touch_cnt++;
							if (ev[i].type != HAKIOS_TOUCH_MOVE || ((move_div++) % 6 == 0)) {
								put(fd, "INPUT TOUCH: ");
								put(fd, ev[i].type == HAKIOS_TOUCH_DOWN ? "DOWN" :
								        ev[i].type == HAKIOS_TOUCH_UP   ? "UP"   : "MOVE");
								put(fd, " f="); put_u64(fd, ev[i].finger);
								put(fd, " x="); put_u64(fd, ev[i].x);
								put(fd, " y="); put_u64(fd, ev[i].y);
								put(fd, "\n");
							}
						}
					}
				}
			}
			sleep_ms(50);
		}
		put(fd, "INPUT SUMMARY: dev=");
		put(fd, tfd >= 0 ? "ok" : "err");
		put(fd, " buttons="); put_u64(fd, btn_cnt);
		put(fd, " touches="); put_u64(fd, touch_cnt);
		put(fd, "\n");
	}

	long usb_s = bootarg_u64("usb_hold");
	if (usb_s > 0) {
		run_tcp_services(fd, usb_s);
	}

	put(fd, "PID 1 DONE\n");
	(void)syscall3_err(SYS_exit, 0, 0, 0);
	for (;;) {}
}
