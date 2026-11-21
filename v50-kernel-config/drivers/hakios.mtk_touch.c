/*
 * SPI touch screen driver for Redmi Note 12 Pro 5G (ruby, MediaTek MT6877).
 *
 * Hardware layout verified from ruby live DTB (m16-touch@0 on spi3@11013000),
 * pinctrl-mtk-mt6877.h, and clk-mt6877.c:
 *   - SPI3 controller:  0x11013000 (mediatek,mt6765-spi, pad-select = 0)
 *   - GPIO base:        0x10005000 (DIR=0x000, DOUT=0x100, DIN=0x200, MODE=0x300, step 0x10)
 *   - infracfg_ao:      0x10001000 (IFRAO2_CG_CLR=0xa8, STA=0xac, bit 10 = CLK_IFRAO_SPI3)
 *   - SPI3 pins (mode 2): GPIO 160=MO, 161=MI, 162=CSB, 163=CLK
 *   - Touch control pins (mode 0): GPIO 14=INT (active-low), GPIO 15=RST (active-low),
 *                                  GPIO 110=DVDD_EN (output-high)
 *
 * Supports both ruby panel controllers automatically (no flash writes):
 *   1. FocalTech FT3680 (FTS 5.x SPI protocol with CRC-16)
 *   2. Goodix GT9916R ("Berlin-D" SPI protocol 0xF1/0xF0)
 *
 * Transport: tries hardware MTK SPI3 FIFO first; automatically falls back to
 * GPIO bit-bang SPI Mode 0 on GPIO 160..163 if SPI3 clock/FIFO does not respond.
 * Activated by boot-arg touchscreen=2 (optional touchlog=1).
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

/* MT6877 physical addresses */
#define MTK_GPIO_PHYS       0x10005000UL
#define MTK_INFRACFG_PHYS   0x10001000UL
#define MTK_SPI3_PHYS       0x11013000UL
#define MTK_REG_SIZE        0x1000UL

/* MT6877 GPIO register layout */
#define GPIO_DIR_BASE       0x000u
#define GPIO_DOUT_BASE      0x100u
#define GPIO_DIN_BASE       0x200u
#define GPIO_MODE_BASE      0x300u
#define GPIO_SET_OFF        0x004u
#define GPIO_CLR_OFF        0x008u

/* ruby m16-touch@0 GPIOs */
#define PIN_TOUCH_IRQ       14u
#define PIN_TOUCH_RST       15u
#define PIN_TOUCH_DVDD      110u
#define PIN_SPI3_MO         160u
#define PIN_SPI3_MI         161u
#define PIN_SPI3_CS         162u
#define PIN_SPI3_CLK        163u

/* MT6877 infracfg_ao SPI3 clock gate */
#define IFRAO2_CG_CLR       0x0a8u
#define IFRAO2_CG_STA       0x0acu
#define IFRAO2_SPI3_BIT     (1u << 10)

/* MediaTek mt6765-spi registers */
#define SPI_CFG0_REG        0x00u
#define SPI_CFG1_REG        0x04u
#define SPI_TX_DATA_REG     0x10u
#define SPI_RX_DATA_REG     0x14u
#define SPI_CMD_REG         0x18u
#define SPI_STATUS0_REG     0x1cu
#define SPI_STATUS1_REG     0x20u
#define SPI_PAD_SEL_REG     0x24u
#define SPI_CFG2_REG        0x28u

#define SPI_CMD_ACT         (1u << 0)
#define SPI_CMD_RESUME      (1u << 1)
#define SPI_CMD_RST         (1u << 2)
#define SPI_CMD_PAUSE_EN    (1u << 4)
#define SPI_CMD_DEASSERT    (1u << 5)
#define SPI_CMD_RX_MLSB     (1u << 12)
#define SPI_CMD_TX_MLSB     (1u << 13)
#define SPI_CMD_FINISH_IE   (1u << 16)
#define SPI_CMD_PAUSE_IE    (1u << 17)

/* Default ruby display geometry */
#define RUBY_DEFAULT_W      1080u
#define RUBY_DEFAULT_H      2400u
#define MAX_FINGERS         10u
#define TOUCH_POLL_MS       8u

enum touch_chip {
	CHIP_NONE = 0,
	CHIP_FOCALTECH,
	CHIP_GOODIX,
};

struct finger_state {
	bool     down;
	uint32_t x;
	uint32_t y;
};

static volatile uint8_t *gpio_va;
static volatile uint8_t *infra_va;
static volatile uint8_t *spi3_va;

static bool              use_bitbang;
static enum touch_chip   active_chip;
static uint32_t          panel_w = RUBY_DEFAULT_W;
static uint32_t          panel_h = RUBY_DEFAULT_H;
static uint32_t          goodix_touch_addr;
static uint32_t          goodix_max_fingers = MAX_FINGERS;
static struct finger_state fingers[MAX_FINGERS];
static thread_call_t     touch_call;
static int               touch_log;
static uint32_t          move_log_div;

static inline uint32_t
mmio_rd32(volatile uint8_t *base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
mmio_wr32(volatile uint8_t *base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dsb st" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb st" ::: "memory");
}

static void
busy_us(uint32_t us)
{
	uint64_t deadline, now;
	clock_interval_to_deadline(us, NSEC_PER_USEC, &deadline);
	do {
		now = mach_absolute_time();
	} while (now < deadline);
}

static void
busy_ms(uint32_t ms)
{
	busy_us(ms * 1000u);
}

/* --- MT6877 GPIO helpers --- */
static void
gpio_set_mode(unsigned pin, unsigned mode)
{
	uint32_t reg = GPIO_MODE_BASE + (pin / 8u) * 0x10u;
	uint32_t sh  = (pin % 8u) * 4u;
	uint32_t v   = mmio_rd32(gpio_va, reg);
	v = (v & ~(0xfu << sh)) | ((mode & 0xfu) << sh);
	mmio_wr32(gpio_va, reg, v);
}

static void
gpio_set_dir(unsigned pin, bool out)
{
	uint32_t reg = GPIO_DIR_BASE + (pin / 32u) * 0x10u + (out ? GPIO_SET_OFF : GPIO_CLR_OFF);
	mmio_wr32(gpio_va, reg, 1u << (pin % 32u));
}

static void
gpio_write(unsigned pin, bool high)
{
	uint32_t reg = GPIO_DOUT_BASE + (pin / 32u) * 0x10u + (high ? GPIO_SET_OFF : GPIO_CLR_OFF);
	mmio_wr32(gpio_va, reg, 1u << (pin % 32u));
}

static bool
gpio_read(unsigned pin)
{
	uint32_t reg = GPIO_DIN_BASE + (pin / 32u) * 0x10u;
	return (mmio_rd32(gpio_va, reg) & (1u << (pin % 32u))) != 0;
}

/* --- SPI transport (Hardware MTK SPI3 FIFO + GPIO bit-bang Mode 0 fallback) --- */
static void
spi_select_bitbang(void)
{
	use_bitbang = true;
	gpio_set_mode(PIN_SPI3_MO, 0);
	gpio_set_mode(PIN_SPI3_MI, 0);
	gpio_set_mode(PIN_SPI3_CS, 0);
	gpio_set_mode(PIN_SPI3_CLK, 0);

	gpio_write(PIN_SPI3_CS, true);
	gpio_write(PIN_SPI3_CLK, false);
	gpio_write(PIN_SPI3_MO, false);

	gpio_set_dir(PIN_SPI3_CS, true);
	gpio_set_dir(PIN_SPI3_CLK, true);
	gpio_set_dir(PIN_SPI3_MO, true);
	gpio_set_dir(PIN_SPI3_MI, false);
	busy_us(20);
}

static void
spi_select_hw(void)
{
	use_bitbang = false;
	/* Ungate CLK_IFRAO_SPI3 */
	mmio_wr32(infra_va, IFRAO2_CG_CLR, IFRAO2_SPI3_BIT);
	busy_us(10);

	gpio_set_mode(PIN_SPI3_MO, 2);
	gpio_set_mode(PIN_SPI3_MI, 2);
	gpio_set_mode(PIN_SPI3_CS, 2);
	gpio_set_mode(PIN_SPI3_CLK, 2);

	/* Reset SPI3 controller */
	mmio_wr32(spi3_va, SPI_CMD_REG, SPI_CMD_RST);
	busy_us(10);
	mmio_wr32(spi3_va, SPI_CMD_REG, 0);
	mmio_wr32(spi3_va, SPI_PAD_SEL_REG, 0); /* pad-select = 0 from ruby DTB */
	/* Conservative ~2-4 MHz clock timing (CFG2 high/low counts = 20) */
	mmio_wr32(spi3_va, SPI_CFG2_REG, (20u << 16) | 20u);
	mmio_wr32(spi3_va, SPI_CFG0_REG, (15u << 24) | (15u << 16) | (15u << 8) | 15u);
}

static bool
spi_xfer_bitbang(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	gpio_write(PIN_SPI3_CLK, false);
	gpio_write(PIN_SPI3_CS, false);
	busy_us(2);

	for (uint32_t i = 0; i < len; i++) {
		uint8_t out = tx ? tx[i] : 0xffu;
		uint8_t in  = 0;
		for (int b = 7; b >= 0; b--) {
			gpio_write(PIN_SPI3_MO, (out & (1u << b)) != 0);
			busy_us(1);
			gpio_write(PIN_SPI3_CLK, true);
			busy_us(1);
			if (gpio_read(PIN_SPI3_MI)) {
				in |= (uint8_t)(1u << b);
			}
			gpio_write(PIN_SPI3_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}

	busy_us(2);
	gpio_write(PIN_SPI3_CS, true);
	busy_us(5);
	return true;
}

static bool
spi_xfer_hw(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (len == 0 || len > 1024u) {
		return false;
	}
	/*
	 * Hardware FIFO is 32 bytes deep; use PAUSE mode for transfers > 32 bytes,
	 * keeping CS asserted across chunks.
	 */
	uint32_t off = 0;
	bool first = true;
	while (off < len) {
		uint32_t chunk = len - off;
		if (chunk > 32u) {
			chunk = 32u;
		}
		bool last = (off + chunk >= len);

		if (first) {
			/* Clear stale status */
			(void)mmio_rd32(spi3_va, SPI_STATUS0_REG);
			(void)mmio_rd32(spi3_va, SPI_STATUS1_REG);
			uint32_t cfg1 = (15u << 0) | ((chunk - 1u) << 16) | (0u << 8);
			mmio_wr32(spi3_va, SPI_CFG1_REG, cfg1);
		} else {
			uint32_t cfg1 = (15u << 0) | ((chunk - 1u) << 16) | (0u << 8);
			mmio_wr32(spi3_va, SPI_CFG1_REG, cfg1);
		}

		/* Pack TX FIFO in little-endian 32-bit words (MSB-first per byte via TX_MLSB) */
		uint32_t words = (chunk + 3u) / 4u;
		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = 0;
			for (uint32_t b = 0; b < 4u; b++) {
				uint32_t idx = off + w * 4u + b;
				uint8_t byte = (idx < len && tx) ? tx[idx] : 0xffu;
				word |= ((uint32_t)byte) << (b * 8u);
			}
			mmio_wr32(spi3_va, SPI_TX_DATA_REG, word);
		}

		uint32_t cmd = SPI_CMD_TX_MLSB | SPI_CMD_RX_MLSB | SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE;
		if (!last) {
			cmd |= SPI_CMD_PAUSE_EN;
		}
		if (first) {
			mmio_wr32(spi3_va, SPI_CMD_REG, cmd);
			mmio_wr32(spi3_va, SPI_CMD_REG, cmd | SPI_CMD_ACT);
		} else {
			mmio_wr32(spi3_va, SPI_CMD_REG, cmd | SPI_CMD_RESUME);
		}

		/* Wait for finish or pause flag in STATUS0 (bits 0..1) */
		uint64_t deadline;
		clock_interval_to_deadline(5, NSEC_PER_MSEC, &deadline);
		uint32_t st = 0;
		while ((st = mmio_rd32(spi3_va, SPI_STATUS0_REG) & 3u) == 0) {
			if (mach_absolute_time() >= deadline) {
				mmio_wr32(spi3_va, SPI_CMD_REG, SPI_CMD_RST);
				busy_us(5);
				mmio_wr32(spi3_va, SPI_CMD_REG, 0);
				return false;
			}
		}

		for (uint32_t w = 0; w < words; w++) {
			uint32_t word = mmio_rd32(spi3_va, SPI_RX_DATA_REG);
			if (rx) {
				for (uint32_t b = 0; b < 4u; b++) {
					uint32_t idx = off + w * 4u + b;
					if (idx < off + chunk) {
						rx[idx] = (uint8_t)((word >> (b * 8u)) & 0xffu);
					}
				}
			}
		}

		off += chunk;
		first = false;
	}
	return true;
}

static bool
spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	if (use_bitbang) {
		return spi_xfer_bitbang(tx, rx, len);
	}
	return spi_xfer_hw(tx, rx, len);
}

/* --- Coordinate scaling & event emission --- */
static void
emit_fingers(const bool seen[MAX_FINGERS], const uint32_t px[MAX_FINGERS], const uint32_t py[MAX_FINGERS])
{
	for (uint32_t f = 0; f < MAX_FINGERS; f++) {
		if (seen[f]) {
			uint32_t nx = (panel_w > 1) ? (uint32_t)(((uint64_t)px[f] * 65535u) / (panel_w - 1u)) : 0;
			uint32_t ny = (panel_h > 1) ? (uint32_t)(((uint64_t)py[f] * 65535u) / (panel_h - 1u)) : 0;
			if (nx > 65535u) nx = 65535u;
			if (ny > 65535u) ny = 65535u;

			if (!fingers[f].down) {
				fingers[f].down = true;
				fingers[f].x = nx;
				fingers[f].y = ny;
				if (touch_log) {
					printf("mtk_touch: down f=%u x=%u y=%u\n", f, px[f], py[f]);
				}
				hakios_touch_post(HAKIOS_TOUCH_DOWN, f, nx, ny);
			} else if (fingers[f].x != nx || fingers[f].y != ny) {
				fingers[f].x = nx;
				fingers[f].y = ny;
				if (touch_log && ((++move_log_div) & 7u) == 0) {
					printf("mtk_touch: move f=%u x=%u y=%u\n", f, px[f], py[f]);
				}
				hakios_touch_post(HAKIOS_TOUCH_MOVE, f, nx, ny);
			}
		} else if (fingers[f].down) {
			fingers[f].down = false;
			if (touch_log) {
				printf("mtk_touch: up f=%u\n", f);
			}
			hakios_touch_post(HAKIOS_TOUCH_UP, f, fingers[f].x, fingers[f].y);
		}
	}
}

/* =========================================================================
 * Protocol 1: FocalTech FT3680 (FTS 5.x SPI protocol, same as hakios.goodix.c)
 * ========================================================================= */
static uint16_t
fts_crc16(const uint8_t *buf, uint32_t len)
{
	uint16_t crc = 0xffffu;
	for (uint32_t i = 0; i < len; i++) {
		crc ^= (uint16_t)buf[i];
		for (int b = 0; b < 8; b++) {
			if (crc & 1u) {
				crc = (uint16_t)((crc >> 1) ^ 0x8408u);
			} else {
				crc >>= 1;
			}
		}
	}
	return crc;
}

static bool
fts_read_reg(uint8_t reg, uint8_t *out, uint32_t len)
{
	/* [reg, 0x80, len_hi, len_lo, 3 dummy, data(len), crc_hi, crc_lo] */
	uint8_t tx[128];
	uint8_t rx[128];
	uint32_t total = 7u + len + 2u;
	if (total > sizeof(tx)) {
		return false;
	}
	memset(tx, 0xff, total);
	tx[0] = reg;
	tx[1] = 0x80u;
	tx[2] = (uint8_t)((len >> 8) & 0xffu);
	tx[3] = (uint8_t)(len & 0xffu);
	tx[4] = 0x00u;
	tx[5] = 0x00u;
	tx[6] = 0x00u;

	if (!spi_xfer(tx, rx, total)) {
		return false;
	}
	/* Check CRC over data (some FTS firmwares use 0x8408 over rx[7..7+len-1]) */
	uint16_t calc = fts_crc16(rx + 7, len);
	uint16_t got  = (uint16_t)(((uint16_t)rx[7 + len] << 8) | rx[7 + len + 1]);
	uint16_t got_le = (uint16_t)(((uint16_t)rx[7 + len + 1] << 8) | rx[7 + len]);
	memcpy(out, rx + 7, len);
	return (calc == got || calc == got_le);
}

static bool
fts_read_reg_raw(uint8_t reg, uint8_t *out, uint32_t len, uint8_t *raw_hdr)
{
	uint8_t tx[128];
	uint8_t rx[128];
	uint32_t total = 7u + len + 2u;
	if (total > sizeof(tx)) {
		return false;
	}
	memset(tx, 0xff, total);
	tx[0] = reg;
	tx[1] = 0x80u;
	tx[2] = (uint8_t)((len >> 8) & 0xffu);
	tx[3] = (uint8_t)(len & 0xffu);
	tx[4] = 0x00u;
	tx[5] = 0x00u;
	tx[6] = 0x00u;
	if (!spi_xfer(tx, rx, total)) {
		return false;
	}
	if (raw_hdr) {
		memcpy(raw_hdr, rx, total < 12u ? total : 12u);
	}
	memcpy(out, rx + 7, len);
	return true;
}

static bool
fts_probe(void)
{
	uint8_t id_h = 0, id_l = 0, fw = 0;
	bool ok_h = fts_read_reg(0xa3u, &id_h, 1);
	bool ok_l = fts_read_reg(0x9fu, &id_l, 1);
	(void)fts_read_reg(0xa6u, &fw, 1);

	if ((ok_h || ok_l) && !(id_h == 0x00 && id_l == 0x00) && !(id_h == 0xff && id_l == 0xff)) {
		printf("mtk_touch: detected FocalTech FT3680 (id=0x%02x%02x fw=0x%02x transport=%s)\n",
		    id_h, id_l, fw, use_bitbang ? "bitbang" : "hw-spi3");
		active_chip = CHIP_FOCALTECH;
		panel_w = RUBY_DEFAULT_W;
		panel_h = RUBY_DEFAULT_H;
		return true;
	}
	/* Also check raw without strict CRC in case CRC polynomial differs slightly */
	uint8_t r_h = 0, r_l = 0;
	if (fts_read_reg_raw(0xa3u, &r_h, 1, NULL) && fts_read_reg_raw(0x9fu, &r_l, 1, NULL)) {
		if ((r_h == 0x56u && (r_l == 0x62u || r_l == 0x52u)) ||
		    (r_h == 0x36u && r_l == 0x80u) ||
		    (r_h == 0x56u && r_l == 0x80u)) {
			printf("mtk_touch: detected FocalTech (raw id=0x%02x%02x transport=%s)\n",
			    r_h, r_l, use_bitbang ? "bitbang" : "hw-spi3");
			active_chip = CHIP_FOCALTECH;
			panel_w = RUBY_DEFAULT_W;
			panel_h = RUBY_DEFAULT_H;
			return true;
		}
	}
	return false;
}

static void
fts_poll(void)
{
	/* Read 64 bytes starting at reg 0x01: [0]=gesture/mode, [1]=td_status, [2..]=points (6B each) */
	uint8_t buf[64];
	if (!fts_read_reg_raw(0x01u, buf, sizeof(buf), NULL)) {
		return;
	}
	uint8_t count = buf[1] & 0x0fu;
	if (count > MAX_FINGERS || buf[1] == 0xffu) {
		return;
	}

	bool seen[MAX_FINGERS];
	uint32_t px[MAX_FINGERS], py[MAX_FINGERS];
	memset(seen, 0, sizeof(seen));
	memset(px, 0, sizeof(px));
	memset(py, 0, sizeof(py));

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *p = &buf[2u + i * 6u];
		uint8_t ev = (p[0] >> 6) & 3u; /* 0=down, 1=up, 2=contact */
		uint8_t id = (p[2] >> 4) & 0x0fu;
		uint32_t x = ((uint32_t)(p[0] & 0x0fu) << 8) | p[1];
		uint32_t y = ((uint32_t)(p[2] & 0x0fu) << 8) | p[3];
		if (id < MAX_FINGERS && (ev == 0u || ev == 2u)) {
			seen[id] = true;
			px[id] = x;
			py[id] = y;
		}
	}
	emit_fingers(seen, px, py);
}

/* =========================================================================
 * Protocol 2: Goodix GT9916R ("Berlin-D" SPI 0xF1 read / 0xF0 write)
 * ========================================================================= */
static bool
goodix_read(uint32_t addr, uint8_t *out, uint32_t len)
{
	uint8_t tx[256];
	uint8_t rx[256];
	uint32_t total = 8u + len;
	if (total > sizeof(tx)) {
		return false;
	}
	memset(tx, 0, total);
	tx[0] = 0xf1u;
	tx[1] = (uint8_t)(addr >> 24);
	tx[2] = (uint8_t)(addr >> 16);
	tx[3] = (uint8_t)(addr >> 8);
	tx[4] = (uint8_t)(addr & 0xffu);
	/* 3 dummy bytes: tx[5..7] */
	if (!spi_xfer(tx, rx, total)) {
		return false;
	}
	memcpy(out, rx + 8, len);
	return true;
}

static bool
goodix_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
	uint8_t tx[64];
	uint32_t total = 5u + len;
	if (total > sizeof(tx)) {
		return false;
	}
	tx[0] = 0xf0u;
	tx[1] = (uint8_t)(addr >> 24);
	tx[2] = (uint8_t)(addr >> 16);
	tx[3] = (uint8_t)(addr >> 8);
	tx[4] = (uint8_t)(addr & 0xffu);
	memcpy(tx + 5, data, len);
	return spi_xfer(tx, NULL, total);
}

static uint16_t
rd_le16(const uint8_t *p)
{
	return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t
rd_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool
goodix_probe(void)
{
	/* Berlin-D ic_info lives at 0x10070 (GT9916R) or 0x10010 (GT9916/Berlin-A) */
	static const uint32_t ic_addrs[] = { 0x10070u, 0x10010u, 0x10000u };
	uint8_t hdr[16];

	for (unsigned a = 0; a < sizeof(ic_addrs) / sizeof(ic_addrs[0]); a++) {
		memset(hdr, 0, sizeof(hdr));
		if (!goodix_read(ic_addrs[a], hdr, sizeof(hdr))) {
			continue;
		}
		uint16_t len = rd_le16(hdr);
		if (len < 64u || len > 512u || (hdr[0] == 0xff && hdr[1] == 0xff)) {
			continue;
		}
		/* Read full ic_info (capped to 240 bytes) */
		uint8_t info[240];
		uint32_t rlen = len < sizeof(info) ? len : (uint32_t)sizeof(info);
		if (!goodix_read(ic_addrs[a], info, rlen)) {
			continue;
		}
		/*
		 * Berlin-D ic_info layout:
		 *   [0..1]   length
		 *   [2..91]  ic_info_version (90 bytes: [2..3]=info_customer_id, [4]=info_version_id, ...)
		 *   [92..111] ic_info_feature (20 bytes)
		 *   [112..135] ic_info_param (24 bytes: [112..115]=drv_num.., [116..119]=max_x/max_y etc.)
		 *   [136..191] ic_info_misc (touch_data_addr at offset ~140 or 144)
		 * Check standard Berlin-D touch_data_addr fallback 0x10308 if parsed addr looks invalid.
		 */
		uint32_t td_addr = 0x10308u;
		for (uint32_t off = 120u; off + 4u <= rlen; off += 2u) {
			uint32_t cand = rd_le32(info + off);
			if (cand >= 0x10200u && cand <= 0x10500u && (cand & 3u) == 0) {
				td_addr = cand;
				break;
			}
		}
		goodix_touch_addr = td_addr;
		panel_w = RUBY_DEFAULT_W;
		panel_h = RUBY_DEFAULT_H;
		goodix_max_fingers = MAX_FINGERS;
		active_chip = CHIP_GOODIX;
		printf("mtk_touch: detected Goodix GT9916R (ic_info@0x%x len=%u td=0x%x transport=%s)\n",
		    ic_addrs[a], len, goodix_touch_addr, use_bitbang ? "bitbang" : "hw-spi3");
		return true;
	}
	return false;
}

static void
goodix_poll(void)
{
	/* Berlin-D touch packet: 8-byte header + N*8-byte finger records */
	uint8_t pkt[8 + MAX_FINGERS * 8];
	if (!goodix_read(goodix_touch_addr, pkt, sizeof(pkt))) {
		return;
	}
	/* Bit 7 of byte 0 = data ready; byte 2 low nibble = touch count */
	if ((pkt[0] & 0x80u) == 0) {
		return;
	}
	uint8_t ev_type = pkt[0] & 0x7fu;
	uint8_t count   = pkt[2] & 0x0fu;

	if (ev_type == 0x01u || ev_type == 0x00u || count <= MAX_FINGERS) {
		if (count <= MAX_FINGERS) {
			bool seen[MAX_FINGERS];
			uint32_t px[MAX_FINGERS], py[MAX_FINGERS];
			memset(seen, 0, sizeof(seen));
			memset(px, 0, sizeof(px));
			memset(py, 0, sizeof(py));

			for (uint32_t i = 0; i < count; i++) {
				const uint8_t *p = &pkt[8u + i * 8u];
				uint8_t id = (p[0] >> 4) & 0x0fu;
				uint32_t x = rd_le16(p + 2);
				uint32_t y = rd_le16(p + 4);
				if (id < MAX_FINGERS) {
					seen[id] = true;
					px[id] = x;
					py[id] = y;
				}
			}
			emit_fingers(seen, px, py);
		}
	}

	/* Clear data-ready flag in touch RAM buffer */
	uint8_t zero = 0;
	(void)goodix_write(goodix_touch_addr, &zero, 1);
}

static void
touch_schedule(void)
{
	uint64_t deadline;
	clock_interval_to_deadline(TOUCH_POLL_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(touch_call, deadline);
}

static void
touch_poll(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	if (active_chip == CHIP_FOCALTECH) {
		fts_poll();
	} else if (active_chip == CHIP_GOODIX) {
		goodix_poll();
	}
	touch_schedule();
}

static bool
probe_both_chips(void)
{
	if (goodix_probe()) {
		return true;
	}
	if (fts_probe()) {
		return true;
	}
	return false;
}

static void
dump_spi_diag(const char *tag)
{
	uint8_t gd[8] = {0}, ft[8] = {0};
	bool gd_ok = goodix_read(0x10070u, gd, 8);
	bool ft_ok = fts_read_reg_raw(0xa3u, ft, 4, NULL);
	printf("mtk_touch: diag[%s] int=%d rst=%d dvdd=%d gd(%d)=%02x %02x %02x %02x ft(%d)=%02x %02x\n",
	    tag,
	    gpio_read(PIN_TOUCH_IRQ) ? 1 : 0,
	    gpio_read(PIN_TOUCH_RST) ? 1 : 0,
	    gpio_read(PIN_TOUCH_DVDD) ? 1 : 0,
	    gd_ok ? 1 : 0, gd[0], gd[1], gd[2], gd[3],
	    ft_ok ? 1 : 0, ft[0], ft[1]);
}

void
hakios_mtk_touch_init(void)
{
	int mode = 0;
	if (!PE_parse_boot_argn("touchscreen", &mode, sizeof(mode)) || mode != 2) {
		return;
	}
	PE_parse_boot_argn("touchlog", &touch_log, sizeof(touch_log));

	gpio_va  = (volatile uint8_t *)ml_io_map(MTK_GPIO_PHYS, MTK_REG_SIZE);
	infra_va = (volatile uint8_t *)ml_io_map(MTK_INFRACFG_PHYS, MTK_REG_SIZE);
	spi3_va  = (volatile uint8_t *)ml_io_map(MTK_SPI3_PHYS, MTK_REG_SIZE);
	if (!gpio_va || !infra_va || !spi3_va) {
		printf("mtk_touch: ml_io_map failed\n");
		return;
	}

	/* Configure control pins (from ruby live-DTB m16-touch@0) */
	gpio_set_mode(PIN_TOUCH_IRQ, 0);
	gpio_set_dir(PIN_TOUCH_IRQ, false);

	gpio_set_mode(PIN_TOUCH_DVDD, 0);
	gpio_write(PIN_TOUCH_DVDD, true);
	gpio_set_dir(PIN_TOUCH_DVDD, true);

	gpio_set_mode(PIN_TOUCH_RST, 0);
	gpio_write(PIN_TOUCH_RST, true);
	gpio_set_dir(PIN_TOUCH_RST, true);
	busy_ms(10);

	uint32_t cg_before = mmio_rd32(infra_va, IFRAO2_CG_STA);
	/* Pass 1: try hardware SPI3 */
	spi_select_hw();
	uint32_t cg_after = mmio_rd32(infra_va, IFRAO2_CG_STA);
	printf("mtk_touch: init SPI3@0x%lx cg=0x%x->0x%x int=%d\n",
	    MTK_SPI3_PHYS, cg_before, cg_after, gpio_read(PIN_TOUCH_IRQ) ? 1 : 0);

	if (!probe_both_chips()) {
		dump_spi_diag("hw-pre-rst");
		/* Pass 2: fallback to GPIO bit-bang SPI Mode 0 */
		spi_select_bitbang();
		if (!probe_both_chips()) {
			dump_spi_diag("bb-pre-rst");
			/* Pass 3: clean hardware reset pulse on GPIO 15 (10ms low -> 150ms high) */
			gpio_write(PIN_TOUCH_RST, false);
			busy_ms(10);
			gpio_write(PIN_TOUCH_RST, true);
			busy_ms(150);

			if (!probe_both_chips()) {
				dump_spi_diag("bb-post-rst");
				/* Final check on hw-spi3 after reset */
				spi_select_hw();
				if (!probe_both_chips()) {
					dump_spi_diag("hw-post-rst");
					printf("mtk_touch: no controller responded (AVDD/IOVDD rail may be off in LK)\n");
					return;
				}
			}
		}
	}

	touch_call = thread_call_allocate(touch_poll, NULL);
	touch_schedule();
}
