/*
 * Dual-chip SPI3 touch driver for Redmi Note 12 Pro 5G (ruby, MediaTek MT6877):
 *   1. FocalTech FT3680 (FTS 5.x SPI v2: 0xA0 read + 3 dummy + CRC-16/Kermit LE, 10x super-res)
 *   2. Goodix GT9916R (Berlin-D SPI: 0xF1 read / 0xF0 write, 0x10308 coord polling)
 *
 * Shared power-up sequence verified on live ruby:
 *   - Hold reset GPIO15 low before power rails
 *   - MT6360 PMIC on I2C5 (GPIO132 SCL / GPIO133 SDA, addr 0x64, SMBus CRC8):
 *       LDO3 (VMC/IOVDD) = 1.8 V (0x3a), LDO2 (VTP/AVDD) = 3.3 V (0xd0)
 *   - DVDD/IOVDD switch GPIO110 high, enable LDO3 then LDO2
 *   - SPI3 bit-bang Mode 0 on GPIO160 MO / 161 MI / 162 CS / 163 CLK
 *   - Release reset GPIO15 high + 200 ms settle, then auto-probe FT3680 -> GT9916R
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

/* Reset GPIO15 (bank 0), DVDD/IOVDD switch GPIO110 (bank 3) */
#define GT_RESET        (1u << 15)
#define GT_IOVDD        (1u << 14)
#define GT_RESET_DIR    GT_DIR(0)
#define GT_RESET_DO     GT_DO(0)
#define GT_IOVDD_DIR    GT_DIR(3)
#define GT_IOVDD_DO     GT_DO(3)

/* MT6360 LDO page (addr 0x64, SMBus CRC8) */
#define GT_PMIC_ADDR    0x64u
#define GT_LDO3_EN      0x05u
#define GT_LDO3_VOUT    0x09u   /* VMC/IOVDD: 0x3a = 1.8 V */
#define GT_LDO2_EN      0x11u
#define GT_LDO2_VOUT    0x15u   /* VTP/AVDD:  0xd0 = 3.3 V (MT6360 LDO2) */

/* FocalTech FT3680 SPI v2 constants (verified against ruby focaltech_3680) */
#define FTS_READ_CTRL       0xa0u   /* 0x80 | DATA_CRC_EN(0x20) */
#define FTS_WRITE_CTRL      0x00u
#define FTS_REG_TOUCH       0x01u
#define FTS_REG_CHIP_ID_H   0xa3u
#define FTS_REG_CHIP_ID_L   0x9fu
#define FTS_REG_FW_VER      0xa6u
#define FTS_CMD_START1      0x55u
#define FTS_CMD_START2      0xaau
#define FTS_CMD_READ_BOOTID 0x90u
#define FTS_CMD_START_APP   0x08u
#define FTS_TOUCH_LEN       (2u + 6u * GT_MAX)  /* 62 bytes */
#define FTS_BUF_MAX         80u

enum mtk_touch_chip {
	CHIP_NONE = 0,
	CHIP_FTS  = 1,
	CHIP_GOODIX = 2,
};

static struct {
	volatile uint32_t *gpio;
	uint32_t active;
	uint32_t x[GT_MAX], y[GT_MAX];
	uint32_t prev_x[GT_MAX], prev_y[GT_MAX];
	enum mtk_touch_chip chip;
	bool started;
	bool log;
	unsigned log_cnt;
} gt;

static uint8_t fts_tx[FTS_BUF_MAX];
static uint8_t fts_rx[FTS_BUF_MAX];

static inline uint32_t
gt_rd(uint32_t off)
{
	return gt.gpio[off >> 2];
}

static inline void
gt_wr(uint32_t off, uint32_t v)
{
	gt.gpio[off >> 2] = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

static void
gt_udelay(uint32_t us)
{
	uint64_t deadline;
	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	while (mach_absolute_time() < deadline) {
		__asm__ volatile("yield" ::: "memory");
	}
}

static void
gt_wait(uint32_t ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	assert_wait_deadline((event_t)&gt, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
gt_set_dir(uint32_t off, uint32_t mask, bool out)
{
	uint32_t v = gt_rd(off);
	gt_wr(off, out ? (v | mask) : (v & ~mask));
}

static void
gt_set_do(uint32_t off, uint32_t mask, bool high)
{
	uint32_t v = gt_rd(off);
	gt_wr(off, high ? (v | mask) : (v & ~mask));
}

/* ---- I2C5 GPIO bit-bang for MT6360 PMIC ---- */

static void
i2c_scl(bool high)
{
	gt_set_dir(GT_I2C_DIR, GT_SCL, !high);
	gt_udelay(5);
}

static void
i2c_sda(bool high)
{
	gt_set_dir(GT_I2C_DIR, GT_SDA, !high);
	gt_udelay(5);
}

static bool
i2c_sda_in(void)
{
	return (gt_rd(GT_I2C_DI) & GT_SDA) != 0;
}

static void
i2c_start(void)
{
	i2c_sda(true);
	i2c_scl(true);
	i2c_sda(false);
	i2c_scl(false);
}

static void
i2c_stop(void)
{
	i2c_sda(false);
	i2c_scl(true);
	i2c_sda(true);
}

static bool
i2c_write_byte(uint8_t v)
{
	bool ack;
	for (int b = 7; b >= 0; b--) {
		i2c_sda(((v >> b) & 1) != 0);
		i2c_scl(true);
		i2c_scl(false);
	}
	i2c_sda(true);
	i2c_scl(true);
	ack = !i2c_sda_in();
	i2c_scl(false);
	return ack;
}

static uint8_t
i2c_read_byte(bool ack)
{
	uint8_t v = 0;
	i2c_sda(true);
	for (int b = 7; b >= 0; b--) {
		i2c_scl(true);
		if (i2c_sda_in()) {
			v |= (uint8_t)(1u << b);
		}
		i2c_scl(false);
	}
	i2c_sda(!ack);
	i2c_scl(true);
	i2c_scl(false);
	i2c_sda(true);
	return v;
}

static uint8_t
gt_crc8(const uint8_t *p, unsigned n)
{
	uint8_t crc = 0;
	for (unsigned i = 0; i < n; i++) {
		crc ^= p[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
		}
	}
	return crc;
}

static bool
pmic_read(uint8_t reg, uint8_t *val)
{
	uint8_t c[3], d[2];

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

/* ---- SPI3 GPIO bit-bang, Mode 0 (CPOL=0, CPHA=0) ---- */

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

/* ---- Shared finger state publisher ---- */

static void
touch_release_all(void)
{
	for (uint32_t i = 0; i < GT_MAX; i++) {
		if (gt.active & (1u << i)) {
			hakios_touch_post(HAKIOS_TOUCH_UP, i, gt.x[i], gt.y[i]);
		}
	}
	gt.active = 0;
}

static void
touch_commit_mask(uint32_t mask)
{
	for (uint32_t i = 0; i < GT_MAX; i++) {
		bool now = (mask & (1u << i)) != 0;
		bool was = (gt.active & (1u << i)) != 0;
		if (now && !was) {
			gt.prev_x[i] = gt.x[i];
			gt.prev_y[i] = gt.y[i];
			if (gt.log && gt.log_cnt < 8u) {
				gt.log_cnt++;
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
			if (gt.log && gt.log_cnt < 8u) {
				gt.log_cnt++;
				printf("mtk_touch: up f=%u x=%u y=%u\n", i, gt.x[i], gt.y[i]);
			}
			hakios_touch_post(HAKIOS_TOUCH_UP, i, gt.x[i], gt.y[i]);
		}
	}
	gt.active = mask;
}

/* =========================================================================
 * Protocol 1: FocalTech FT3680 (FTS 5.x SPI v2, verified from ruby kernel)
 * ========================================================================= */

static uint16_t
fts_crc16(const uint8_t *data, unsigned len)
{
	uint16_t crc = 0xffffu;
	for (unsigned i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i];
		for (int b = 0; b < 8; b++) {
			crc = (crc & 1u) ? (uint16_t)((crc >> 1) ^ 0x8408u) : (uint16_t)(crc >> 1);
		}
	}
	return crc;
}

static bool
fts_read(uint8_t addr, uint8_t *out, unsigned len)
{
	const unsigned dp = 7u;
	unsigned total = dp + len + 2u;
	if (len == 0 || total > FTS_BUF_MAX) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		for (unsigned i = 0; i < total; i++) {
			fts_tx[i] = 0;
			fts_rx[i] = 0;
		}
		fts_tx[0] = addr;
		fts_tx[1] = FTS_READ_CTRL;
		fts_tx[2] = (uint8_t)((len >> 8) & 0xffu);
		fts_tx[3] = (uint8_t)(len & 0xffu);
		spi_xfer(fts_tx, fts_rx, total);
		gt_udelay(200); /* CS_HIGH_DELAY >= 150 us */
		if ((fts_rx[3] & 0xa0u) == 0u) {
			uint16_t calc = fts_crc16(fts_rx + dp, len);
			uint16_t got = (uint16_t)((uint16_t)fts_rx[dp + len] |
			    ((uint16_t)fts_rx[dp + len + 1] << 8));
			if (calc == got) {
				for (unsigned i = 0; i < len; i++) {
					out[i] = fts_rx[dp + i];
				}
				return true;
			}
		}
	}
	return false;
}

static bool
fts_write(uint8_t addr, const uint8_t *data, unsigned len)
{
	unsigned total = (len == 0u) ? 4u : (7u + len);
	if (total > FTS_BUF_MAX) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		for (unsigned i = 0; i < total; i++) {
			fts_tx[i] = 0;
			fts_rx[i] = 0;
		}
		fts_tx[0] = addr;
		fts_tx[1] = FTS_WRITE_CTRL;
		fts_tx[2] = (uint8_t)((len >> 8) & 0xffu);
		fts_tx[3] = (uint8_t)(len & 0xffu);
		for (unsigned i = 0; i < len; i++) {
			fts_tx[7u + i] = data[i];
		}
		spi_xfer(fts_tx, fts_rx, total);
		gt_udelay(200);
		if ((fts_rx[3] & 0xa0u) == 0u) {
			return true;
		}
	}
	return false;
}

static void
touch_reset_pulse(uint32_t low_ms, uint32_t high_ms)
{
	gt_wr(GT_RESET_DIR, gt_rd(GT_RESET_DIR) | GT_RESET);
	gt_wr(GT_RESET_DO, gt_rd(GT_RESET_DO) & ~GT_RESET);
	gt_wait(low_ms);
	gt_wr(GT_RESET_DO, gt_rd(GT_RESET_DO) | GT_RESET);
	if (high_ms) {
		gt_wait(high_ms);
	}
}

static bool
fts_probe(uint8_t en3, uint8_t en2)
{
	uint8_t id_h = 0, id_l = 0, fw = 0;

	/* Firmware in flash boots automatically within ~200 ms after reset. */
	for (int attempt = 0; attempt < 5; attempt++) {
		bool ok_h = fts_read(FTS_REG_CHIP_ID_H, &id_h, 1);
		bool ok_l = fts_read(FTS_REG_CHIP_ID_L, &id_l, 1);
		if (ok_h && ok_l && !(id_h == 0x00u && id_l == 0x00u) && !(id_h == 0xffu && id_l == 0xffu)) {
			(void)fts_read(FTS_REG_FW_VER, &fw, 1);
			printf("mtk_touch: detected FocalTech FT3680 (id=0x%02x%02x fw=0x%02x) 1080x2400 rails=%02x/%02x\n",
			    id_h, id_l, fw, en3, en2);
			gt.chip = CHIP_FTS;
			return true;
		}
		gt_wait(80);
	}

	/* Fallback: check if chip is sitting in Boot ROM and issue START_APP (no flash/PRAM writes). */
	uint8_t boot_id[2] = { 0, 0 };
	uint8_t aa = FTS_CMD_START2;
	for (int cnt = 0; cnt < 2; cnt++) {
		touch_reset_pulse(2, 12u + (uint32_t)cnt * 8u);
		if (fts_write(FTS_CMD_START1, &aa, 1)) {
			gt_wait(12);
			if (fts_read(FTS_CMD_READ_BOOTID, boot_id, 2) &&
			    boot_id[0] == 0x56u && boot_id[1] == 0x62u) {
				printf("mtk_touch: FT3680 in boot ROM (5662), starting flash app\n");
				(void)fts_write(FTS_CMD_START_APP, NULL, 0);
				gt_wait(250);
				if (fts_read(FTS_REG_CHIP_ID_H, &id_h, 1) &&
				    fts_read(FTS_REG_CHIP_ID_L, &id_l, 1) &&
				    !(id_h == 0x00u && id_l == 0x00u) &&
				    !(id_h == 0xffu && id_l == 0xffu)) {
					(void)fts_read(FTS_REG_FW_VER, &fw, 1);
					printf("mtk_touch: detected FocalTech FT3680 (id=0x%02x%02x fw=0x%02x bootrom) rails=%02x/%02x\n",
					    id_h, id_l, fw, en3, en2);
					gt.chip = CHIP_FTS;
					return true;
				}
			}
		}
	}

	/* Print raw SPI RX header from one 0xA3 read for hardware diagnostics. */
	for (unsigned i = 0; i < 10u; i++) { fts_tx[i] = 0; fts_rx[i] = 0; }
	fts_tx[0] = FTS_REG_CHIP_ID_H;
	fts_tx[1] = FTS_READ_CTRL;
	fts_tx[2] = 0;
	fts_tx[3] = 1;
	spi_xfer(fts_tx, fts_rx, 10u);
	gt_udelay(200);
	printf("mtk_touch: FTS miss id=%02x%02x boot=%02x%02x raw=%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
	    id_h, id_l, boot_id[0], boot_id[1],
	    fts_rx[0], fts_rx[1], fts_rx[2], fts_rx[3], fts_rx[4],
	    fts_rx[5], fts_rx[6], fts_rx[7], fts_rx[8], fts_rx[9]);
	return false;
}

static int
fts_frame(void)
{
	uint8_t buf[FTS_TOUCH_LEN];
	if (!fts_read(FTS_REG_TOUCH, buf, sizeof(buf))) {
		return EIO;
	}
	/* Idle / uninitialized report markers used by vendor driver */
	if ((buf[1] == 0xffu && buf[2] == 0xffu && buf[3] == 0xffu) ||
	    (buf[1] == 0xefu && buf[2] == 0xefu && buf[3] == 0xefu)) {
		return EAGAIN;
	}

	uint8_t etype = (buf[1] >> 4) & 0x0fu;
	if (etype != 0x00u && etype != 0x02u) {
		return 0;
	}

	uint32_t mask = 0;
	for (uint32_t i = 0; i < GT_MAX; i++) {
		const uint8_t *p = buf + 2u + i * 6u;
		uint32_t id = (uint32_t)((p[2] >> 4) & 0x0fu);
		if (id >= GT_MAX) {
			break;
		}
		uint32_t flag = (uint32_t)((p[0] >> 6) & 0x03u); /* 0=down, 1=up, 2=contact */
		if (flag == 0u || flag == 2u) {
			/* ruby focaltech_3680 super_resolution_factors == 10 (10800 x 24000) */
			uint32_t rx = (((uint32_t)(p[0] & 0x0fu)) << 11)
			            | (((uint32_t)p[1]) << 3)
			            | ((((uint32_t)(p[4] & 0xc0u)) >> 6) << 1)
			            | (((uint32_t)(p[0] & 0x20u)) >> 5);
			uint32_t ry = (((uint32_t)(p[2] & 0x0fu)) << 11)
			            | (((uint32_t)p[3]) << 3)
			            | ((((uint32_t)(p[4] & 0x30u)) >> 4) << 1)
			            | (((uint32_t)(p[2] & 0x10u)) >> 4);
			if (rx > 10799u) rx = 10799u;
			if (ry > 23999u) ry = 23999u;
			/* hakios_touch_post() expects 0..65535 normalized display coordinates */
			uint32_t x = (uint32_t)(((uint64_t)rx * 65535u) / 10799u);
			uint32_t y = (uint32_t)(((uint64_t)ry * 65535u) / 23999u);
			mask |= (1u << id);
			gt.x[id] = x;
			gt.y[id] = y;
		}
	}
	touch_commit_mask(mask);
	return 0;
}

/* =========================================================================
 * Protocol 2: Goodix GT9916R ("Berlin-D" SPI 0xF1 read / 0xF0 write)
 * ========================================================================= */

static void
gt_spi_read_req(uint8_t *tx, unsigned len, uint32_t addr)
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
		gt_wait(2);

		gt_spi_read_req(tx, sizeof(tx), 0x10000);
		spi_xfer(tx, rx, 16);
		bool ok = true;
		for (int i = 0; i < 8; i++) {
			if (rx[8 + i] != 0xaa) {
				ok = false;
				break;
			}
		}
		if (ok) {
			return true;
		}
		gt_wait(10);
	}
	return false;
}

static bool
gt_version_ok(void)
{
	uint8_t tx[40], rx[40];
	uint32_t sum = 0;

	gt_spi_read_req(tx, sizeof(tx), 0x10014);
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
gt_head_ok(const uint8_t *d)
{
	uint32_t sum = 0;
	for (int i = 0; i < 6; i++) {
		sum += d[i];
	}
	return (sum & 0xffffu) == (uint32_t)(d[6] | ((uint32_t)d[7] << 8));
}

static int
gt_frame(void)
{
	uint8_t tx[40], rx[40];
	const uint8_t *d = &rx[8];
	uint32_t nz = 0, n, mask = 0, sum = 0;

	gt_spi_read_req(tx, sizeof(tx), 0x10308);
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
			uint32_t rx = (uint32_t)p[2] | ((uint32_t)p[3] << 8);
			uint32_t ry = (uint32_t)p[4] | ((uint32_t)p[5] << 8);
			if (rx > 10799u) rx = 10799u;
			if (ry > 23999u) ry = 23999u;
			/* hakios_touch_post() expects 0..65535 normalized display coordinates */
			uint32_t x = (uint32_t)(((uint64_t)rx * 65535u) / 10799u);
			uint32_t y = (uint32_t)(((uint64_t)ry * 65535u) / 23999u);
			mask |= (1u << id);
			gt.x[id] = x;
			gt.y[id] = y;
		}
	}
	gt_clear_sync();
	touch_commit_mask(mask);
	return 0;
}

/* =========================================================================
 * Shared hardware power-up and chip auto-detection
 * ========================================================================= */

static bool
gt_hw_init(void)
{
	uint8_t en3 = 0, en2 = 0, v3 = 0, v2 = 0;
	uint32_t v;

	/* Hold reset GPIO15 low before toggling rails (matches fts_power_source_ctrl). */
	v = gt_rd(GT_MODE(1));
	gt_wr(GT_MODE(1), v & ~(0xfu << 28));
	gt_wr(GT_RESET_DIR, gt_rd(GT_RESET_DIR) | GT_RESET);
	gt_wr(GT_RESET_DO, gt_rd(GT_RESET_DO) & ~GT_RESET);
	gt_wait(2);

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

	/* DVDD/IOVDD switch GPIO110 high, then VMC 1.8 V (LDO3) and VTP 3.3 V (LDO2). */
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

	/* SPI3 pins 160-163 -> GPIO (CS high, CLK/MO low, MI input). */
	v = gt_rd(GT_SPI_MODE);
	gt_wr(GT_SPI_MODE, v & ~0xffffu);
	v = gt_rd(GT_SPI_DIR);
	gt_wr(GT_SPI_DIR, (v & ~0x0fu) | (GT_MO | GT_CS | GT_CLK));
	v = gt_rd(GT_SPI_DO);
	gt_wr(GT_SPI_DO, (v & ~0x0fu) | GT_CS);
	gt_udelay(50);

	/* Release reset and wait 200 ms for FT3680 / GT9916R flash firmware startup. */
	touch_reset_pulse(10, 200);

	if (fts_probe(en3, en2)) {
		return true;
	}

	/* Fallback: Goodix GT9916R */
	touch_reset_pulse(10, 100);
	if (gt_dev_confirm() && gt_version_ok()) {
		printf("mtk_touch: detected Goodix GT9916R (BERLIN) 1080x2400 rails=%02x/%02x\n", en3, en2);
		gt.chip = CHIP_GOODIX;
		return true;
	}

	printf("mtk_touch: neither FocalTech FT3680 nor Goodix GT9916R responded\n");
	return false;
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
		int rc = (gt.chip == CHIP_FTS) ? fts_frame() : gt_frame();
		if (rc != 0) {
			if (rc == EIO && ++bad >= 50) {
				touch_release_all();
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

	int log_arg = 0;
	if (PE_parse_boot_argn("touchlog", &log_arg, sizeof(log_arg)) && log_arg != 0) {
		gt.log = true;
	}

	vm_offset_t va = ml_io_map(GT_GPIO_PA, 0x1000);
	if (!va) {
		printf("mtk_touch: ml_io_map(GPIO) failed\n");
		return;
	}
	gt.gpio = (volatile uint32_t *)va;

	thread_t th = THREAD_NULL;
	if (kernel_thread_start(gt_thread, NULL, &th) == KERN_SUCCESS && th != THREAD_NULL) {
		thread_deallocate(th);
	}
}
