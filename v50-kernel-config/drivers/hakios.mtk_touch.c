/*
 * Goodix GT9916R (BerlinD) SPI3 touch driver for Redmi Note 12 Pro 5G (ruby, MT6877).
 * Hardware sequence verified on live ruby (adapted from lky005/ruby-ios-bringup):
 *   - MT6360 PMIC on I2C5 (GPIO132 SCL / GPIO133 SDA, addr 0x64, SMBus CRC8):
 *       LDO3 (VMC/IOVDD) = 1.8 V (0x3a), LDO2 (VTP/AVDD) = 3.3 V (0xd0)
 *   - IOVDD switch GPIO110 high, reset GPIO15 low 10 ms -> high 100 ms
 *   - SPI3 bit-bang Mode 0 on GPIO160 MO / 161 MI / 162 CS / 163 CLK
 *   - Confirm via 0x10000 loopback + 0x10014 "BERLIN" version checksum
 *   - Poll 0x10308 every 8 ms, scale 10800x24000 -> 1080x2400, post to /dev/hakios-touch
 *
 * No firmware or config upload is performed.
 * Boot args: touchscreen=2 [touchlog=1].
 */
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t thread_terminate(thread_t);

#define GT_MAX          10u

#define GT_GPIO_PA      0x10005000UL
#define GT_DIR(n)       (0x000u + ((n) * 0x10u))
#define GT_DO(n)        (0x100u + ((n) * 0x10u))
#define GT_DI(n)        (0x200u + ((n) * 0x10u))
#define GT_MODE(n)      (0x300u + ((n) * 0x10u))

/* I2C5: GPIO132 SCL / GPIO133 SDA, bank 4 */
#define GT_SCL          (1u << 4)
#define GT_SDA          (1u << 5)
#define GT_I2C_MODE     0x400u  /* pins 128-135 = group 16 */
#define GT_I2C_DIR      GT_DIR(4)
#define GT_I2C_DO       GT_DO(4)
#define GT_I2C_DI       GT_DI(4)

/* SPI3: GPIO160 MO / 161 MI / 162 CS / 163 CLK, bank 5 */
#define GT_MO           (1u << 0)
#define GT_MI           (1u << 1)
#define GT_CS           (1u << 2)
#define GT_CLK          (1u << 3)
#define GT_SPI_MODE     0x440u  /* pins 160-167 = group 20 */
#define GT_SPI_DIR      GT_DIR(5)
#define GT_SPI_DO       GT_DO(5)
#define GT_SPI_DI       GT_DI(5)

/* Reset GPIO15 (bank 0), IOVDD switch GPIO110 (bank 3) */
#define GT_RESET        (1u << 15)
#define GT_IOVDD        (1u << 14)
#define GT_RESET_DIR    GT_DIR(0)
#define GT_RESET_DO     GT_DO(0)
#define GT_IOVDD_DIR    GT_DIR(3)
#define GT_IOVDD_DO     GT_DO(3)

/* MT6360 on I2C5 */
#define GT_PMIC_ADDR    0x64u
#define GT_LDO3_EN      0x05u
#define GT_LDO3_VOUT    0x09u
#define GT_LDO2_EN      0x11u
#define GT_LDO2_VOUT    0x15u

static struct {
	vm_offset_t gpio;
	bool started;
	int log;
	uint32_t active;
	uint32_t x[GT_MAX], y[GT_MAX];
	uint32_t prev_x[GT_MAX], prev_y[GT_MAX];
} gt;

static int gt_sleep_chan;

static inline void
gt_udelay(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static uint32_t
gt_rd(uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(gt.gpio + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
gt_wr(uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(gt.gpio + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
gt_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&gt_sleep_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
gt_set_do(uint32_t off, uint32_t mask, bool high)
{
	uint32_t v = gt_rd(off);
	if (high) {
		v |= mask;
	} else {
		v &= ~mask;
	}
	gt_wr(off, v);
}

/* ---- I2C5 bit-bang (open-drain with external pull-ups) ---- */

static void i2c_scl_release(void) { gt_wr(GT_I2C_DIR, gt_rd(GT_I2C_DIR) & ~GT_SCL); }
static void i2c_scl_low(void)     { gt_set_do(GT_I2C_DO, GT_SCL, false); gt_wr(GT_I2C_DIR, gt_rd(GT_I2C_DIR) | GT_SCL); }
static void i2c_sda_release(void) { gt_wr(GT_I2C_DIR, gt_rd(GT_I2C_DIR) & ~GT_SDA); }
static void i2c_sda_low(void)     { gt_set_do(GT_I2C_DO, GT_SDA, false); gt_wr(GT_I2C_DIR, gt_rd(GT_I2C_DIR) | GT_SDA); }

static bool
i2c_sda_read(void)
{
	i2c_sda_release();
	gt_udelay(2);
	return (gt_rd(GT_I2C_DI) & GT_SDA) != 0;
}

static void
i2c_start(void)
{
	i2c_sda_release();
	i2c_scl_release();
	gt_udelay(5);
	i2c_sda_low();
	gt_udelay(5);
	i2c_scl_low();
	gt_udelay(5);
}

static void
i2c_stop(void)
{
	i2c_sda_low();
	gt_udelay(5);
	i2c_scl_release();
	gt_udelay(5);
	i2c_sda_release();
	gt_udelay(5);
}

static void
i2c_write_bit(bool bit)
{
	if (bit) i2c_sda_release();
	else i2c_sda_low();
	gt_udelay(5);
	i2c_scl_release();
	gt_udelay(5);
	i2c_scl_low();
	gt_udelay(5);
}

static bool
i2c_read_bit(void)
{
	i2c_sda_release();
	gt_udelay(5);
	i2c_scl_release();
	gt_udelay(5);
	bool bit = i2c_sda_read();
	i2c_scl_low();
	gt_udelay(5);
	return bit;
}

static bool
i2c_write_byte(uint8_t b)
{
	for (int i = 7; i >= 0; i--) {
		i2c_write_bit(((b >> i) & 1) != 0);
	}
	i2c_sda_release();
	gt_udelay(5);
	i2c_scl_release();
	gt_udelay(5);
	bool ack = !i2c_sda_read();
	i2c_scl_low();
	gt_udelay(5);
	return ack;
}

static uint8_t
i2c_read_byte(bool ack)
{
	uint8_t b = 0;
	for (int i = 7; i >= 0; i--) {
		if (i2c_read_bit()) {
			b |= (uint8_t)(1u << i);
		}
	}
	i2c_sda_release();
	gt_udelay(5);
	if (ack) {
		i2c_sda_low();
	}
	gt_udelay(5);
	i2c_scl_release();
	gt_udelay(5);
	i2c_scl_low();
	gt_udelay(5);
	i2c_sda_release();
	return b;
}

/* ---- MT6360 CRC8 SMBus access ---- */

static uint8_t
gt_crc8(const uint8_t *p, int n)
{
	uint8_t crc = 0;
	for (int i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static bool
pmic_read(uint8_t reg, uint8_t *val)
{
	uint8_t d[2], c[3];

	i2c_start();
	if (!i2c_write_byte((uint8_t)(GT_PMIC_ADDR << 1))) goto fail;
	if (!i2c_write_byte((uint8_t)(reg & 0x3fu))) goto fail;
	i2c_start();
	if (!i2c_write_byte((uint8_t)((GT_PMIC_ADDR << 1) | 1u))) goto fail;
	d[0] = i2c_read_byte(true);
	d[1] = i2c_read_byte(false);
	i2c_stop();

	c[0] = (uint8_t)((GT_PMIC_ADDR << 1) | 1u);
	c[1] = (uint8_t)(reg & 0x3fu);
	c[2] = d[0];
	if (gt_crc8(c, 3) != d[1]) {
		return false;
	}
	*val = d[0];
	return true;
fail:
	i2c_stop();
	return false;
}

static bool
pmic_write(uint8_t reg, uint8_t val)
{
	uint8_t c[3];
	c[0] = (uint8_t)(GT_PMIC_ADDR << 1);
	c[1] = (uint8_t)(reg & 0x3fu);
	c[2] = val;

	i2c_start();
	if (!i2c_write_byte(c[0])) goto fail;
	if (!i2c_write_byte(c[1])) goto fail;
	if (!i2c_write_byte(val)) goto fail;
	if (!i2c_write_byte(gt_crc8(c, 3))) goto fail;
	if (!i2c_write_byte(0)) goto fail;
	i2c_stop();
	return true;
fail:
	i2c_stop();
	return false;
}

/* ---- SPI3 GPIO bit-bang, Mode 0 ---- */

static void
spi_pin(uint32_t mask, bool high)
{
	gt_set_do(GT_SPI_DO, mask, high);
	gt_udelay(2);
}

static bool
spi_mi(void)
{
	return (gt_rd(GT_SPI_DI) & GT_MI) != 0;
}

static void
spi_xfer(const uint8_t *tx, uint8_t *rx, unsigned len)
{
	spi_pin(GT_CS, false);
	for (unsigned i = 0; i < len; i++) {
		uint8_t r = 0;
		for (int b = 7; b >= 0; b--) {
			spi_pin(GT_MO, ((tx[i] >> b) & 1) != 0);
			spi_pin(GT_CLK, true);
			if (rx) {
				r |= (uint8_t)((spi_mi() ? 1u : 0u) << b);
			}
			spi_pin(GT_CLK, false);
		}
		if (rx) {
			rx[i] = r;
		}
	}
	spi_pin(GT_CS, true);
}

static void
spi_read_req(uint8_t *tx, unsigned len, uint32_t addr)
{
	for (unsigned i = 0; i < len; i++) {
		tx[i] = 0;
	}
	tx[0] = 0xf1;
	tx[1] = (uint8_t)((addr >> 24) & 0xff);
	tx[2] = (uint8_t)((addr >> 16) & 0xff);
	tx[3] = (uint8_t)((addr >> 8) & 0xff);
	tx[4] = (uint8_t)(addr & 0xff);
	tx[5] = 0xff;
	tx[6] = 0xff;
	tx[7] = 0xff;
}

static void
gt_clear_sync(void)
{
	uint8_t tx[6] = { 0xf0, 0x00, 0x01, 0x03, 0x08, 0x00 };
	spi_xfer(tx, NULL, sizeof(tx));
}

static bool
gt_dev_confirm(void)
{
	uint8_t tx[40], rx[40];
	for (int attempt = 0; attempt < 3; attempt++) {
		tx[0] = 0xf0;
		tx[1] = 0x00;
		tx[2] = 0x01;
		tx[3] = 0x00;
		tx[4] = 0x00;
		for (int i = 0; i < 8; i++) {
			tx[5 + i] = 0xaa;
		}
		spi_xfer(tx, NULL, 13);
		gt_wait(5);

		spi_read_req(tx, sizeof(tx), 0x10000);
		spi_xfer(tx, rx, 16);
		int ok = 1;
		for (int i = 0; i < 8; i++) {
			if (rx[8 + i] != 0xaa) {
				ok = 0;
				break;
			}
		}
		if (ok) {
			return true;
		}
		gt_wait(5);
	}
	return false;
}

static bool
gt_version_ok(void)
{
	uint8_t tx[40], rx[40];
	uint32_t sum = 0;

	spi_read_req(tx, sizeof(tx), 0x10014);
	spi_xfer(tx, rx, 36);
	for (int i = 0; i < 26; i++) {
		sum += rx[8 + i];
	}
	if ((sum & 0xffffu) != (uint32_t)(rx[34] | ((uint32_t)rx[35] << 8))) {
		return false;
	}
	return rx[8] == 'B' && rx[9] == 'E' && rx[10] == 'R' &&
	       rx[11] == 'L' && rx[12] == 'I' && rx[13] == 'N';
}

static bool
gt_hw_init(void)
{
	uint8_t en3 = 0, en2 = 0, v3 = 0, v2 = 0;
	uint32_t v;

	/* I2C5 pins 132/133 -> GPIO open-drain */
	v = gt_rd(GT_I2C_MODE);
	gt_wr(GT_I2C_MODE, v & ~(0xffu << 16));
	gt_wr(GT_I2C_DIR, gt_rd(GT_I2C_DIR) & ~(GT_SCL | GT_SDA));
	gt_wr(GT_I2C_DO, gt_rd(GT_I2C_DO) & ~(GT_SCL | GT_SDA));
	gt_udelay(20);
	if ((gt_rd(GT_I2C_DI) & (GT_SCL | GT_SDA)) != (GT_SCL | GT_SDA)) {
		printf("mtk_touch: I2C5 idle low; abort\n");
		return false;
	}

	if (!pmic_read(GT_LDO3_EN, &en3) || !pmic_read(GT_LDO2_EN, &en2) ||
	    !pmic_read(GT_LDO3_VOUT, &v3) || !pmic_read(GT_LDO2_VOUT, &v2)) {
		printf("mtk_touch: MT6360 PMIC read failed; abort\n");
		return false;
	}
	printf("mtk_touch: MT6360 init LDO3 en=%02x vout=%02x LDO2 en=%02x vout=%02x\n",
	    en3, v3, en2, v2);

	/* Power-cycle both rails before adjusting VOUT. */
	if (!pmic_write(GT_LDO3_EN, (uint8_t)(en3 & ~0x40u)) ||
	    !pmic_write(GT_LDO2_EN, (uint8_t)(en2 & ~0x40u))) {
		printf("mtk_touch: MT6360 disable failed; abort\n");
		return false;
	}
	gt_wait(20);

	if (v3 < 0x3a || v3 > 0x40) {
		if (!pmic_write(GT_LDO3_VOUT, 0x3a)) {
			printf("mtk_touch: LDO3 VOUT write failed; abort\n");
			return false;
		}
	}
	if (v2 < 0xd0 || v2 > 0xd9) {
		if (!pmic_write(GT_LDO2_VOUT, 0xd0)) {
			printf("mtk_touch: LDO2 VOUT write failed; abort\n");
			return false;
		}
	}
	if (!pmic_read(GT_LDO3_VOUT, &v3) || !pmic_read(GT_LDO2_VOUT, &v2) ||
	    v3 < 0x3a || v3 > 0x40 || v2 < 0xd0 || v2 > 0xd9) {
		printf("mtk_touch: VOUT readback bad (%02x/%02x); abort\n", v3, v2);
		return false;
	}

	/* IOVDD switch GPIO110 high, then VMC 1.8 V (LDO3) and VTP 3.3 V (LDO2). */
	v = gt_rd(GT_MODE(13));
	gt_wr(GT_MODE(13), v & ~(0xfu << 24));
	gt_wr(GT_IOVDD_DIR, gt_rd(GT_IOVDD_DIR) | GT_IOVDD);
	gt_wr(GT_IOVDD_DO, gt_rd(GT_IOVDD_DO) | GT_IOVDD);
	gt_wait(3);

	if (!pmic_read(GT_LDO3_EN, &en3) ||
	    !pmic_write(GT_LDO3_EN, (uint8_t)(en3 | 0x40u)) ||
	    !pmic_read(GT_LDO3_EN, &en3) || !(en3 & 0x40u)) {
		printf("mtk_touch: LDO3 enable failed; abort\n");
		return false;
	}
	gt_wait(30);
	if (!pmic_read(GT_LDO2_EN, &en2) ||
	    !pmic_write(GT_LDO2_EN, (uint8_t)(en2 | 0x40u)) ||
	    !pmic_read(GT_LDO2_EN, &en2) || !(en2 & 0x40u)) {
		printf("mtk_touch: LDO2 enable failed; abort\n");
		return false;
	}
	gt_wait(15);

	/* Reset GPIO15 pinmux -> GPIO; SPI3 pins 160-163 -> GPIO (CS high, CLK/MO low). */
	v = gt_rd(GT_MODE(1));
	gt_wr(GT_MODE(1), v & ~(0xfu << 28));
	v = gt_rd(GT_SPI_MODE);
	gt_wr(GT_SPI_MODE, v & ~0xffffu);
	v = gt_rd(GT_SPI_DIR);
	gt_wr(GT_SPI_DIR, (v & ~0x0fu) | (GT_MO | GT_CS | GT_CLK));
	v = gt_rd(GT_SPI_DO);
	gt_wr(GT_SPI_DO, (v & ~0x0fu) | GT_CS);
	gt_udelay(20);

	/* Reset pulse: low 10 ms, high 100 ms. */
	gt_wr(GT_RESET_DIR, gt_rd(GT_RESET_DIR) | GT_RESET);
	gt_wr(GT_RESET_DO, gt_rd(GT_RESET_DO) & ~GT_RESET);
	gt_wait(10);
	gt_wr(GT_RESET_DO, gt_rd(GT_RESET_DO) | GT_RESET);
	gt_wait(100);

	if (!gt_dev_confirm()) {
		printf("mtk_touch: Goodix dev_confirm failed\n");
		return false;
	}
	if (!gt_version_ok()) {
		printf("mtk_touch: Goodix BERLIN version/checksum failed\n");
		return false;
	}
	printf("mtk_touch: detected Goodix GT9916R (BERLIN) 1080x2400 rails=%02x/%02x\n", en3, en2);
	return true;
}

static bool
gt_head_ok(const uint8_t *d)
{
	uint32_t sum = 0;
	for (int i = 0; i < 6; i++) {
		sum += d[i];
	}
	return (sum & 0xffffu) == (uint32_t)(d[6] | ((uint32_t)d[7] << 8));
}

static void
gt_release_all(void)
{
	for (uint32_t i = 0; i < GT_MAX; i++) {
		if (gt.active & (1u << i)) {
			hakios_touch_post(HAKIOS_TOUCH_UP, i, gt.x[i], gt.y[i]);
		}
	}
	gt.active = 0;
}

static int
gt_frame(void)
{
	uint8_t tx[40], rx[40];
	const uint8_t *d = &rx[8];
	uint32_t nz = 0, n, mask = 0, sum = 0;

	spi_read_req(tx, sizeof(tx), 0x10308);
	spi_xfer(tx, rx, 34);

	for (int i = 0; i < 8; i++) {
		nz |= d[i];
	}
	if (!nz || !gt_head_ok(d)) {
		return EAGAIN;
	}
	if (!(d[0] & 0x80u)) {
		gt_clear_sync();
		return EAGAIN;
	}
	n = d[2] & 0x0fu;
	if (n > 2u) {
		gt_clear_sync();
		return EIO;
	}

	if (n) {
		for (int i = 8; i < 8 + (int)n * 8; i++) {
			sum += d[i];
		}
		if ((sum & 0xffffu) != (uint32_t)(d[8 + n * 8] | ((uint32_t)d[8 + n * 8 + 1] << 8))) {
			gt_clear_sync();
			return EIO;
		}
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *p = d + 8 + i * 8;
			uint32_t id = (uint32_t)(p[0] >> 4);
			uint32_t type = (uint32_t)(p[0] & 0x0fu);

			if (id >= GT_MAX || (mask & (1u << id)) || type == 1u || type == 3u) {
				gt_clear_sync();
				return EIO;
			}
			uint32_t x = ((uint32_t)p[2] | ((uint32_t)p[3] << 8)) / 10u;
			uint32_t y = ((uint32_t)p[4] | ((uint32_t)p[5] << 8)) / 10u;
			if (x > 1079u) x = 1079u;
			if (y > 2399u) y = 2399u;
			mask |= (1u << id);
			gt.x[id] = x;
			gt.y[id] = y;
		}
	}
	gt_clear_sync();

	for (uint32_t i = 0; i < GT_MAX; i++) {
		bool now = (mask & (1u << i)) != 0;
		bool was = (gt.active & (1u << i)) != 0;
		if (now && !was) {
			gt.prev_x[i] = gt.x[i];
			gt.prev_y[i] = gt.y[i];
			if (gt.log) {
				printf("mtk_touch: down f=%u x=%u y=%u\n", i, gt.x[i], gt.y[i]);
			}
			hakios_touch_post(HAKIOS_TOUCH_DOWN, i, gt.x[i], gt.y[i]);
		} else if (now && was) {
			if (gt.x[i] != gt.prev_x[i] || gt.y[i] != gt.prev_y[i]) {
				gt.prev_x[i] = gt.x[i];
				gt.prev_y[i] = gt.y[i];
				hakios_touch_post(HAKIOS_TOUCH_MOVE, i, gt.x[i], gt.y[i]);
			}
		} else if (!now && was) {
			if (gt.log) {
				printf("mtk_touch: up f=%u x=%u y=%u\n", i, gt.x[i], gt.y[i]);
			}
			hakios_touch_post(HAKIOS_TOUCH_UP, i, gt.x[i], gt.y[i]);
		}
	}
	gt.active = mask;
	return 0;
}

static void
gt_thread(__unused void *arg, __unused wait_result_t wr)
{
	int bad = 0;

	if (!gt_hw_init()) {
		printf("mtk_touch: stopped (init failed)\n");
		(void)thread_terminate(current_thread());
		thread_block(THREAD_CONTINUE_NULL);
		return;
	}

	for (;;) {
		if (gt_frame() != 0) {
			if (++bad >= 50) {
				gt_release_all();
				bad = 0;
			}
		} else {
			bad = 0;
		}
		gt_wait(8);
	}
}

void
hakios_mtk_touch_init(void)
{
	int mode = 0;
	if (gt.started) {
		return;
	}
	if (!PE_parse_boot_argn("touchscreen", &mode, sizeof(mode)) || mode != 2) {
		return;
	}
	gt.started = true;
	PE_parse_boot_argn("touchlog", &gt.log, sizeof(gt.log));

	gt.gpio = ml_io_map(GT_GPIO_PA, 0x1000UL);
	if (!gt.gpio) {
		printf("mtk_touch: GPIO map failed\n");
		return;
	}

	thread_t th;
	kern_return_t kr = kernel_thread_start(gt_thread, NULL, &th);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		printf("mtk_touch: thread start failed (%d)\n", kr);
	}
}
