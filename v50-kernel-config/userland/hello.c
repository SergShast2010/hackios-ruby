/*
 * First user-space program for Darwin-on-ruby bring-up (v0.52: interactive input + touch test).
 *
 * Static arm64 Mach-O with no libc and no dyld: talks to XNU with raw system calls.
 * Reads /dev/hakios-touch in non-blocking mode and prints button and touch events live.
 */
typedef unsigned long u64;
typedef unsigned int  u32;

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

#define SYS_exit         1
#define SYS_read         3
#define SYS_write        4
#define SYS_open         5
#define SYS_getpid       20
#define SYS_getuid       24
#define SYS_select       93
#define SYS_sysctlbyname 274

#define O_RDWR           0x0002
#define O_NONBLOCK       0x0004

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

static unsigned
length(const char *s)
{
	unsigned n = 0;
	while (s[n]) n++;
	return n;
}

static void
put(int fd, const char *s)
{
	(void)syscall3_err(SYS_write, fd, (long)s, length(s));
}

static void
put_u64(int fd, u64 v)
{
	char buf[24];
	int i = (int)sizeof(buf);
	buf[--i] = 0;
	do {
		buf[--i] = (char)('0' + (v % 10));
		v /= 10;
	} while (v);
	put(fd, buf + i);
}

static void
show_int(int fd, const char *name)
{
	int value = 0;
	u64 size = sizeof(value);
	if (syscall6_err(SYS_sysctlbyname, (long)name, length(name), (long)&value, (long)&size, 0, 0) == 0) {
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
	if (syscall6_err(SYS_sysctlbyname, (long)name, length(name), (long)&value, (long)&size, 0, 0) == 0) {
		put(fd, name);
		put(fd, "=");
		put_u64(fd, value);
		put(fd, " (");
		put_u64(fd, value >> 20);
		put(fd, " MB)\n");
	}
}

static long
wait_seconds(void)
{
	char args[700];
	u64 size = sizeof(args);
	long n = 0;
	if (syscall6_err(SYS_sysctlbyname, (long)"kern.bootargs", 13, (long)args, (long)&size, 0, 0) != 0) {
		return 0;
	}
	for (char *c = args; *c; c++) {
		if (c[0]=='h'&&c[1]=='e'&&c[2]=='l'&&c[3]=='l'&&c[4]=='o'&&c[5]=='_'&&
		    c[6]=='w'&&c[7]=='a'&&c[8]=='i'&&c[9]=='t'&&c[10]=='=') {
			for (c += 11; *c >= '0' && *c <= '9'; c++) {
				n = n * 10 + (*c - '0');
			}
			return n;
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

	{
		char version[96];
		u64 size = sizeof(version);
		long failed = syscall6_err(SYS_sysctlbyname, (long)"kern.hakios.version", 19,
		    (long)version, (long)&size, 0, 0);
		put(fd, "kernel.ver=");
		put(fd, failed ? "(unavailable)" : version);
		put(fd, "\n");
	}

	show_int(fd, "hw.ncpu");
	show_int(fd, "hw.activecpu");
	show_int(fd, "hw.physicalcpu");
	show_u64_mb(fd, "hw.memsize");

	int tfd = (int)syscall3_err(SYS_open, (long)"/dev/hakios-touch", O_RDWR | O_NONBLOCK, 0);
	long wait_s = wait_seconds();
	if (wait_s <= 0) wait_s = 10;

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
	long ticks = wait_s * 20; /* 50ms per tick */
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
		long tv[2] = { 0, 50000 }; /* 50 ms */
		(void)syscall6_err(SYS_select, 0, 0, 0, 0, (long)tv, 0);
	}

	put(fd, "INPUT SUMMARY: dev=");
	put(fd, tfd >= 0 ? "ok" : "err");
	put(fd, " buttons=");
	put_u64(fd, btn_cnt);
	put(fd, " touches=");
	put_u64(fd, touch_cnt);
	put(fd, "\n");

	(void)syscall3_err(SYS_exit, 0, 0, 0);
	for (;;) {}
}
