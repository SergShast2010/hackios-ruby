/*
 * MediaTek MT6877 (ruby / Redmi Note 12 Pro 5G) USB2 MTU3 + QMU + CDC-ECM Ethernet driver.
 *
 * Brings up usb0@11201000 (MTU3 device mode), TPHY@11e40000, PS5169 mux (GPIO121),
 * GICv3 INTID 160 (SPI 128, level-high), EP0 control, EP1 Bulk OUT/IN (QMU 36-bit DMA),
 * and EP3 Interrupt IN (CDC-ECM link notifications), and attaches to HakiOS en0
 * (192.168.7.2/24; host side 192.168.7.1/24, MAC 02:00:5e:77:ee:01).
 *
 * Hardware register sequences and MT6877 errata adapted from lky005/ruby-ios-bringup.
 * Boot arg: mtk_usb_ecm=1 (off by default; safe on QEMU).
 */

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <vm/vm_kern.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
#include <netinet/in_var.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/hakios.net.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/param.h>
#include <sys/sockio.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge,
    void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);

/* ---- MT6877 physical addresses & IRQ ----------------------------------------------------------- */

#define MTU3_PA             0x11201000UL
#define MTU3_SIZE           0x00003000UL
#define IPPC_PA             0x11203e00UL
#define IPPC_SIZE           0x00000100UL
#define PHY_PA              0x11e40000UL
#define PHY_SIZE            0x00002000UL
#define GPIO_PA             0x10005000UL
#define GPIO_SIZE           0x00001000UL
#define IFRAO_PA            0x10001000UL
#define IFRAO_SIZE          0x00001000UL

#define MTU3_IRQ_INTID      160u            /* GICv3 SPI 128 (0x80) = INTID 160, level-high */

/* PS5169 USB-C mux enable: GPIO121 (bank 3, bit 25) */
#define GPIO_PINMUX_121     0x03f0u
#define GPIO_DIR3           0x0030u
#define GPIO_DO3            0x0130u

/* ---- IPPC registers (base 0x11203e00) ---------------------------------------------------------- */

#define IPPC_PW_CTRL0       0x0000u
#define IPPC_PW_CTRL2       0x0008u
#define IPPC_PW_STS1        0x0010u
#define IPPC_PW_STS2        0x0014u
#define IPPC_U3_CTRL_0P     0x0030u
#define IPPC_U2_CTRL_0P     0x0050u
#define IPPC_DEV_RST_CTRL   0x0098u

#define U2_OTG_SEL          (1u << 7)

/* ---- TPHY v2 registers (base 0x11e40000, verified MT6877 layout) ------------------------------- */

#define IPPC_OTG_STS        0x0018u
#define OTG_VBUS_VALID      (1u << 9)
#define U3_DUAL_MODE        (1u << 7)
#define U3_HOST_SEL         (1u << 2)

#define PHY_U2_ACR0         0x0300u
#define PHY_U2_ACR5         0x0314u
#define PHY_U2_ACR6         0x0318u
#define PHY_U2_ACR3         0x031cu
#define PHY_U2_ACR4         0x0320u
#define PHY_U2_DTM0         0x0368u
#define PHY_U2_DTM1         0x036cu
#define PHY_U3_SPLLC        0x0700u
#define PHY_U3_CHIP         0x0800u
#define PHY_U3_PHYA         0x0b00u
#define PHY_U3_PHYA_DA0     (PHY_U3_PHYA + 0x0100u)

#define DTM0_FORCE_UART_EN  (1u << 26)
#define DTM0_FORCE_DATAIN   (1u << 23)
#define DTM0_FORCE_DM_PD    (1u << 21)
#define DTM0_FORCE_DP_PD    (1u << 20)
#define DTM0_FORCE_XCVRSEL  (1u << 19)
#define DTM0_FORCE_SUSPENDM (1u << 18)
#define DTM0_FORCE_TERMSEL  (1u << 17)
#define DTM0_RG_DATAIN_MSK  (0xfu << 10)
#define DTM0_RG_XCVRSEL_MSK (0x3u << 4)
#define DTM0_RG_XCVRSEL_VAL(x) (((x) & 0x3u) << 4)
#define DTM0_RG_SUSPENDM    (1u << 3)
#define DTM0_RG_TERMSEL     (1u << 2)
#define DTM0_RG_DP_PD       (1u << 6)
#define DTM0_RG_DM_PD       (1u << 7)
#define DTM0_PART_MASK      (DTM0_FORCE_DATAIN | DTM0_FORCE_DM_PD | DTM0_FORCE_DP_PD | \
                             DTM0_FORCE_XCVRSEL | DTM0_FORCE_SUSPENDM | DTM0_FORCE_TERMSEL | \
                             DTM0_RG_DP_PD | DTM0_RG_DM_PD | DTM0_RG_TERMSEL)

#define DTM1_FORCE_IDDIG    (1u << 9)
#define DTM1_RG_VBUSVALID   (1u << 5)
#define DTM1_RG_SESSEND     (1u << 4)
#define DTM1_RG_AVALID      (1u << 2)
#define DTM1_RG_IDDIG       (1u << 1)
#define DTM1_UART_EN        (1u << 16)

#define ACR0_INTR_EN        (1u << 5)
#define ACR3_PUPD_BIST_EN   (1u << 12)
#define ACR3_EN_PU_DP       (1u << 9)
#define ACR4_GPIO_CTL       (1u << 9)
#define ACR4_GPIO_MODE      (1u << 8)
#define ACR5_HS_100U_U3_EN  (1u << 11)
#define ACR6_BC11_SW_EN     (1u << 23)
#define ACR6_OTG_VBUSCMP_EN (1u << 20)

/* ---- MTU3 MAC / EP / QMU registers (base 0x11201000) ------------------------------------------- */

#define LV1ISR_OFF          0x0000u
#define LV1IER_OFF          0x0004u
#define LV1IESR_OFF         0x0008u
#define LV1IECR_OFF         0x000cu
#define LV1_BMU_INTR        (1u << 0)
#define LV1_QMU_INTR        (1u << 1)
#define LV1_MAC3_INTR       (1u << 2)
#define LV1_MAC2_INTR       (1u << 3)
#define LV1_EP0_INTR        (1u << 4)

#define EPISR_OFF           0x0080u
#define EPIER_OFF           0x0084u
#define EPIESR_OFF          0x0088u
#define EPIECR_OFF          0x008cu
#define EPISR_EP0ISR        (1u << 0)
#define EPISR_SETUPEND      (1u << 16)

#define EP_RST_OFF          0x00c0u
#define EP_RST_EP0_BIT      (1u << 0)
#define EP_RST_RX1_BIT      (1u << 1)
#define EP_RST_TX1_BIT      (1u << 17)
#define EP_RST_TX3_BIT      (1u << 19)

#define EP0CSR_OFF          0x0100u
#define RXCOUNT0_OFF        0x0108u
#define EP0_MAXPKTSZ_MSK    0x03ffu
#define EP0_RXPKTRDY        (1u << 16)
#define EP0_SETUPPKTRDY     (1u << 17)
#define EP0_TXPKTRDY        (1u << 18)
#define EP0_DATAEND         (1u << 19)
#define EP0_DPHTX           (1u << 20)
#define EP0_SENTSTALL       (1u << 22)
#define EP0_FIFOFULL        (1u << 23)
#define EP0_SENDSTALL       (1u << 25)
#define EP0_W1C_BITS        (~(EP0_RXPKTRDY | EP0_SETUPPKTRDY | EP0_SENTSTALL))

#define TX1CSR0_OFF         0x0110u
#define TX1CSR1_OFF         0x0114u
#define TX1CSR2_OFF         0x0118u
#define TX3CSR0_OFF         0x0130u
#define TX3CSR1_OFF         0x0134u
#define TX3CSR2_OFF         0x0138u
#define TX_TXPKTRDY         (1u << 16)
#define TX_SENDSTALL        (1u << 20)
#define TX_SENTSTALL        (1u << 22)
#define TX_DMAREQEN         (1u << 29)
#define TX_SLOT(n)          (((uint32_t)(n) & 0x3fu) << 8)
#define TX_TYPE(n)          (((uint32_t)(n) & 0x3u) << 4)
#define TX_FIFOADDR(a)      ((uint32_t)(a) & 0x1fffu)
#define TX_FIFOSEGSIZE(s)   (((uint32_t)(s) & 0xfu) << 16)

#define RX1CSR0_OFF         0x0210u
#define RX1CSR1_OFF         0x0214u
#define RX1CSR2_OFF         0x0218u
#define RX_RXPKTRDY         (1u << 16)
#define RX_SENDSTALL        (1u << 20)
#define RX_SENTSTALL        (1u << 22)
#define RX_DMAREQEN         (1u << 29)
#define RX_SLOT(n)          (((uint32_t)(n) & 0x3fu) << 8)
#define RX_TYPE(n)          (((uint32_t)(n) & 0x3u) << 4)
#define RX_FIFOADDR(a)      ((uint32_t)(a) & 0x1fffu)
#define RX_FIFOSEGSIZE(s)   (((uint32_t)(s) & 0xfu) << 16)

#define FIFO0_OFF           0x0300u
#define FIFO3_OFF           0x0330u

/* QMU registers */
#define QCR0_OFF            0x0400u
#define QCR1_OFF            0x0404u
#define QCR2_OFF            0x0408u
#define QCR3_OFF            0x040cu
#define TQHIAR1_OFF         0x0484u
#define RQHIAR1_OFF         0x04c4u
#define TQCSR1_OFF          0x0510u
#define TQSAR1_OFF          0x0514u
#define TQCPR1_OFF          0x0518u
#define RQCSR1_OFF          0x0610u
#define RQSAR1_OFF          0x0614u
#define RQCPR1_OFF          0x0618u

#define QISAR0_OFF          0x0700u
#define QIER0_OFF           0x0704u
#define QIESR0_OFF          0x0708u
#define QISAR1_OFF          0x0710u
#define QIER1_OFF           0x0714u
#define QIESR1_OFF          0x0718u
#define TQERRIR0_OFF        0x0780u
#define TQERRIESR0_OFF      0x0788u
#define RQERRIR0_OFF        0x07c0u
#define RQERRIESR0_OFF      0x07c8u
#define RQERRIR1_OFF        0x07d0u
#define RQERRIESR1_OFF      0x07d8u

#define QMU_Q_START         (1u << 0)
#define QMU_Q_RESUME        (1u << 1)
#define QMU_Q_STOP          (1u << 2)
#define QMU_Q_ACTIVE        (1u << 15)

#define QMU_TX1_DONE        (1u << 1)
#define QMU_RX1_DONE        (1u << 17)
#define QISAR1_TXQ_EMPTY    (1u << 0)
#define QISAR1_TXQ_CSERR    (1u << 1)
#define QISAR1_TXQ_LENERR   (1u << 2)
#define QISAR1_RXQ_EMPTY    (1u << 16)
#define QISAR1_RXQ_CSERR    (1u << 17)
#define QISAR1_RXQ_LENERR   (1u << 18)
#define QISAR1_RXQ_ZLPERR   (1u << 20)
#define QMU_QIER1_MASK      (QISAR1_TXQ_EMPTY | QISAR1_TXQ_CSERR | QISAR1_TXQ_LENERR | \
                             QISAR1_RXQ_EMPTY | QISAR1_RXQ_CSERR | QISAR1_RXQ_LENERR | QISAR1_RXQ_ZLPERR)

#define QMU_RX1_CS_ERR      (1u << 1)
#define QMU_RX1_LEN_ERR     (1u << 17)
#define QMU_RX1_ZLP_ERR     (1u << 17)
#define QMU_TX1_CS_ERR      (1u << 1)
#define QMU_TX1_LEN_ERR     (1u << 17)

/* Device / MAC2 / MAC3 registers (verified MT6877 MTU3 offsets) */
#define DEV_CONF_OFF        0x0800u
#define MISC_CTRL_OFF       0x0c84u
#define USB3_CONFIG_OFF     0x141cu
#define MAC2_PM_CTRL_OFF    0x2404u
#define MAC2_INTR_EN_OFF    0x2418u
#define MAC2_INTR_STS_OFF   0x241cu
#define MAC2_INTR_SUSPEND   (1u << 0)
#define MAC2_INTR_RESUME    (1u << 1)
#define MAC2_INTR_RESET     (1u << 2)

/* ---- Ring & ECM constants ---------------------------------------------------------------------- */

#define ECM_MPS             512u
#define ECM_FRAME_MAX       1536u
#define ECM_TXQ_BYTES       (64u * 1024u)

#define TX_RING_N           64u
#define TX_BUF_STRIDE       2048u
#define RX_RING_N           16u
#define RX_BUF_STRIDE       2048u
#define GPD_SIZE            16u

/* ---- Low-level MMIO & cache helpers ------------------------------------------------------------ */

static inline uint32_t
rd32(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr32(vm_offset_t base, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile ("dsb sy\n" ::: "memory");
}

static inline uint8_t
rd8(vm_offset_t base, uint32_t off)
{
	return *(volatile uint8_t *)(base + off);
}

static inline void
wr8(vm_offset_t base, uint32_t off, uint8_t val)
{
	*(volatile uint8_t *)(base + off) = val;
	__asm__ volatile ("dsb sy\n" ::: "memory");
}

static inline void
wr16(vm_offset_t base, uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(base + off) = val;
	__asm__ volatile ("dsb sy\n" ::: "memory");
}

static inline void
dma_mb(void)
{
	__asm__ volatile ("dsb sy\n" ::: "memory");
}

static inline void
dc_civac(const void *va, size_t len)
{
	uintptr_t start = (uintptr_t)va & ~(uintptr_t)63u;
	uintptr_t end = ((uintptr_t)va + len + 63u) & ~(uintptr_t)63u;
	for (uintptr_t p = start; p < end; p += 64u) {
		__asm__ volatile ("dc civac, %0\n" :: "r" (p) : "memory");
	}
	__asm__ volatile ("dsb sy\n" ::: "memory");
}

static uint64_t
now_ms(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000ULL;
}

static uint64_t
dma_pa(const void *va)
{
	return (uint64_t)kvtophys((vm_offset_t)(uintptr_t)va);
}

/* ---- USB CDC-ECM descriptors (VID 0x1209 / PID 0x0002 / High-Speed 480M) ----------------------- */

static const uint8_t ecm_phone_mac[ETHER_ADDR_LEN] = {
	0x02, 0x00, 0x5e, 0x77, 0xee, 0x02
};

static const uint8_t usb_dev_desc[18] = {
	18, 0x01, 0x00, 0x02,               /* bLength, DEVICE, bcdUSB 2.00 */
	0x02, 0x00, 0x00, 64,               /* bDeviceClass=CDC (0x02), MPS0=64 */
	0x09, 0x12, 0x02, 0x00,             /* VID 0x1209, PID 0x0002 */
	0x12, 0x02,                         /* bcdDevice 2.12 */
	1, 2, 3, 1                          /* iMfg, iProd, iSerial, bNumConfigs */
};

static const uint8_t usb_dev_qual_desc[10] = {
	10, 0x06, 0x00, 0x02,               /* bLength, DEVICE_QUALIFIER, USB 2.00 */
	0x02, 0x00, 0x00, 64,
	1, 0
};

/* 9 (cfg) + 9 (if0) + 5 (hdr) + 5 (union) + 13 (eth) + 7 (ep83) + 9 (if1 alt0) + 9 (if1 alt1) + 7 (ep01) + 7 (ep81) = 80 */
static const uint8_t usb_cfg_desc_hs[80] = {
	/* Configuration Descriptor */
	9, 0x02, 80, 0x00, 2, 1, 0, 0xC0, 250,
	/* Interface 0: CDC Communication (ECM) */
	9, 0x04, 0, 0, 1, 0x02, 0x06, 0x00, 0,
	/* CDC Header Functional Descriptor */
	5, 0x24, 0x00, 0x10, 0x01,
	/* CDC Union Functional Descriptor (master=0, slave=1) */
	5, 0x24, 0x06, 0, 1,
	/* CDC Ethernet Networking Functional Descriptor (iMACAddress=4, MTU 1514) */
	13, 0x24, 0x0F, 4, 0, 0, 0, 0, 0xEA, 0x05, 0x00, 0x00, 0,
	/* Endpoint 0x83 Interrupt IN (MPS 16, bInterval 9 = 32ms) */
	7, 0x05, 0x83, 0x03, 16, 0x00, 9,
	/* Interface 1 Alt 0: CDC Data (idle, 0 endpoints) */
	9, 0x04, 1, 0, 0, 0x0A, 0x00, 0x00, 0,
	/* Interface 1 Alt 1: CDC Data (active, 2 bulk endpoints) */
	9, 0x04, 1, 1, 2, 0x0A, 0x00, 0x00, 0,
	/* Endpoint 0x01 Bulk OUT (MPS 512) */
	7, 0x05, 0x01, 0x02, 0x00, 0x02, 0,
	/* Endpoint 0x81 Bulk IN (MPS 512) */
	7, 0x05, 0x81, 0x02, 0x00, 0x02, 0,
};

static const uint8_t usb_cfg_desc_fs[80] = {
	9, 0x07, 80, 0x00, 2, 1, 0, 0xC0, 250,
	9, 0x04, 0, 0, 1, 0x02, 0x06, 0x00, 0,
	5, 0x24, 0x00, 0x10, 0x01,
	5, 0x24, 0x06, 0, 1,
	13, 0x24, 0x0F, 4, 0, 0, 0, 0, 0xEA, 0x05, 0x00, 0x00, 0,
	7, 0x05, 0x83, 0x03, 16, 0x00, 32,
	9, 0x04, 1, 0, 0, 0x0A, 0x00, 0x00, 0,
	9, 0x04, 1, 1, 2, 0x0A, 0x00, 0x00, 0,
	7, 0x05, 0x01, 0x02, 64, 0x00, 0,
	7, 0x05, 0x81, 0x02, 64, 0x00, 0,
};

static const uint8_t str_lang[4] = { 4, 0x03, 0x09, 0x04 };
static const uint8_t str_mfg[10] = { 10, 0x03, 'r', 0, 'u', 0, 'b', 0, 'y', 0 };
static const uint8_t str_prod[30] = {
	30, 0x03,
	'H', 0, 'a', 0, 'k', 0, 'i', 0, 'O', 0, 'S', 0, ' ', 0,
	'U', 0, 'S', 0, 'B', 0, ' ', 0, 'E', 0, 'C', 0, 'M', 0
};
static const uint8_t str_ser[10] = { 10, 0x03, '0', 0, '0', 0, '0', 0, '3', 0 };
/* Host-side MAC string (02:00:5e:77:ee:01) */
static const uint8_t str_mac[26] = {
	26, 0x03,
	'0', 0, '2', 0, '0', 0, '0', 0, '5', 0, 'E', 0,
	'7', 0, '7', 0, 'E', 0, 'E', 0, '0', 0, '1', 0
};

/* ---- Driver state ------------------------------------------------------------------------------ */

static LCK_GRP_DECLARE(usb_lck_grp, "mtk-usb-ecm");

static struct {
	vm_offset_t mac;
	vm_offset_t ippc;
	vm_offset_t phy;
	vm_offset_t gpio;
	vm_offset_t ifrao;

	int         initialized;
	volatile int quiesced;
	int         irq_attached;
	volatile int irq_masked;

	/* USB device / EP0 state */
	uint8_t     config;
	uint8_t     ecm_alt;
	uint8_t     dev_addr;
	uint32_t    cfg_seen;
	uint32_t    resets_seen;
	uint32_t    setups_seen;
	uint32_t    reattach_cnt;
	uint64_t    attach_ms;
	uint64_t    last_ev_ms;
	int         reset_phase;
	uint64_t    reset_seen_ms;

	uint8_t     ep0_tx_buf[128];
	uint32_t    ep0_tx_total;
	uint32_t    ep0_tx_sent;
	int         ep0_tx_pending;

	/* EP3 Interrupt IN link notifications */
	volatile uint32_t notify_step;      /* 0=idle, 1=NETWORK_CONNECTION, 2=SPEED_CHANGE */
	uint32_t    notify_sent;

	/* QMU TX EP1 (0x81) */
	void       *tx_ring_alloc;
	vm_offset_t tx_ring_uc;
	uint8_t    *tx_gpd_cpu;
	uint64_t    tx_ring_pa;
	uint64_t    tx_gpd_pa[TX_RING_N];
	void       *tx_buf_base;
	uint8_t    *tx_buf[TX_RING_N];
	uint64_t    tx_buf_pa[TX_RING_N];
	int         tx_ready;
	int         tx_started;
	uint32_t    tx_enq;
	uint32_t    tx_deq;
	uint32_t    tx_inflight;
	uint64_t    tx_progress_ms;
	uint64_t    tx_salvage_ms;
	uint32_t    tx_salvages;
	uint32_t    tx_frames;
	uint32_t    tx_drops;

	/* QMU RX EP1 (0x01) */
	void       *rx_ring_alloc;
	vm_offset_t rx_ring_uc;
	uint8_t    *rx_gpd_cpu;
	uint64_t    rx_ring_pa;
	uint64_t    rx_gpd_pa[RX_RING_N];
	void       *rx_buf_base;
	uint8_t    *rx_buf[RX_RING_N];
	uint64_t    rx_buf_pa[RX_RING_N];
	int         rx_ready;
	uint32_t    rx_deq;
	uint32_t    rx_packets;
	uint32_t    rx_errors;
	uint32_t    rx_frames;
	uint32_t    rx_drops;

	/* ECM software TX queue: [u16 len][bytes...] */
	lck_spin_t *txq_lock;
	uint8_t     txq[ECM_TXQ_BYTES];
	uint32_t    txq_head;
	uint32_t    txq_tail;
	uint32_t    tx_left;

	/* ECM RX frame reassembly */
	uint8_t     rx_frame[ECM_FRAME_MAX + ECM_MPS];
	uint32_t    rx_flen;
	int         rx_fbad;

	/* Worker wakeup channel */
	int         wake_chan;
	uint32_t    irq_count;
} u;

static void
usb_wake_worker(void)
{
	thread_wakeup((event_t)&u.wake_chan);
}

/* ---- Platform PHY / IPPC / GPIO121 init (verified MT6877 sequence) ----------------------------- */

static void
u3_phy_on(vm_offset_t phy)
{
	uint32_t v;

	v = rd32(phy, PHY_U3_PHYA + 0x04u);         /* RG_SSUSB_VA_ON */
	v |= (1u << 29);
	wr32(phy, PHY_U3_PHYA + 0x04u, v);

	v = rd32(phy, PHY_U3_SPLLC + 0x18u);        /* XTAL RX */
	v |= (1u << 9) | (1u << 8);
	wr32(phy, PHY_U3_SPLLC + 0x18u, v);

	v = rd32(phy, PHY_U3_PHYA_DA0);             /* XTAL_EXT_EN_U3 */
	v &= ~(0x3u << 10);
	v |= (2u << 10);
	wr32(phy, PHY_U3_PHYA_DA0, v);

	v = rd32(phy, PHY_U3_CHIP + 0x0cu);         /* release IP reset */
	v &= ~((1u << 31) | (1u << 29));
	wr32(phy, PHY_U3_CHIP + 0x0cu, v);

	v = rd32(phy, PHY_U3_CHIP + 0x10u);         /* release PHYD reset */
	v &= ~((1u << 25) | (1u << 24));
	wr32(phy, PHY_U3_CHIP + 0x10u, v);
}

static void
u2_phy_on(vm_offset_t phy)
{
	uint32_t v;

	/* u2_phy_instance_init (tphy v2, verified on ruby MT6877) */
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~(DTM0_FORCE_UART_EN | DTM0_FORCE_SUSPENDM);
	v |= (1u << 4);                             /* XCVRSEL = 1 */
	wr32(phy, PHY_U2_DTM0, v);

	v = rd32(phy, PHY_U2_DTM1);
	v &= ~DTM1_UART_EN;
	wr32(phy, PHY_U2_DTM1, v);

	v = rd32(phy, PHY_U2_ACR0);
	v |= ACR0_INTR_EN;
	wr32(phy, PHY_U2_ACR0, v);

	v = rd32(phy, PHY_U2_ACR5);
	v &= ~ACR5_HS_100U_U3_EN;
	wr32(phy, PHY_U2_ACR5, v);

	v = rd32(phy, PHY_U2_ACR4);
	v &= ~(ACR4_GPIO_CTL | ACR4_GPIO_MODE);
	wr32(phy, PHY_U2_ACR4, v);

	v = rd32(phy, PHY_U2_ACR6);
	v &= ~(ACR6_BC11_SW_EN | 0xfu);
	v |= 2u;                                    /* SQTH = 2 */
	wr32(phy, PHY_U2_ACR6, v);

	/* u2_phy_instance_power_on */
	v = rd32(phy, PHY_U2_DTM0);
	v |= DTM0_FORCE_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~DTM0_RG_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM0);
	v |= DTM0_RG_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	delay(30);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~DTM0_FORCE_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~DTM0_RG_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~DTM0_FORCE_UART_EN;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM1);
	v &= ~DTM1_UART_EN;
	wr32(phy, PHY_U2_DTM1, v);
	v = rd32(phy, PHY_U2_ACR4);
	v &= ~(ACR4_GPIO_CTL | ACR4_GPIO_MODE);
	wr32(phy, PHY_U2_ACR4, v);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~DTM0_FORCE_SUSPENDM;
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_DTM0);
	v &= ~(DTM0_RG_XCVRSEL_MSK | DTM0_RG_DATAIN_MSK | DTM0_PART_MASK);
	wr32(phy, PHY_U2_DTM0, v);
	v = rd32(phy, PHY_U2_ACR6);
	v &= ~ACR6_BC11_SW_EN;
	wr32(phy, PHY_U2_ACR6, v);
	v = rd32(phy, PHY_U2_ACR6);
	v |= ACR6_OTG_VBUSCMP_EN;
	wr32(phy, PHY_U2_ACR6, v);
	v = rd32(phy, PHY_U2_DTM1);
	v |= DTM1_RG_VBUSVALID | DTM1_RG_AVALID;
	v &= ~DTM1_RG_SESSEND;
	wr32(phy, PHY_U2_DTM1, v);
}

static void
ps5169_enable(vm_offset_t gpio)
{
	uint32_t v;

	v = rd32(gpio, GPIO_PINMUX_121) & ~(0xfu << 4);
	wr32(gpio, GPIO_PINMUX_121, v);

	v = rd32(gpio, GPIO_DIR3) | (1u << 25);
	wr32(gpio, GPIO_DIR3, v);

	v = rd32(gpio, GPIO_DO3) | (1u << 25);
	wr32(gpio, GPIO_DO3, v);
}

static void
ippc_init(vm_offset_t ippc)
{
	uint32_t c0 = rd32(ippc, IPPC_PW_CTRL0);
	uint32_t c2 = rd32(ippc, IPPC_PW_CTRL2);
	uint32_t u3 = rd32(ippc, IPPC_U3_CTRL_0P);
	uint32_t u2 = rd32(ippc, IPPC_U2_CTRL_0P);
	uint32_t rst = rd32(ippc, IPPC_DEV_RST_CTRL);

	/* Global SSUSB SW reset */
	wr32(ippc, IPPC_PW_CTRL0, c0 | 1u);
	delay(200);
	wr32(ippc, IPPC_PW_CTRL0, c0 & ~1u);
	delay(200);

	/* Enable Device IP power; U3/U2 ports enabled in dual/OTG mode */
	wr32(ippc, IPPC_PW_CTRL2, c2 & ~1u);
	u3 = (u3 & ~((1u << 0) | (1u << 1) | U3_HOST_SEL)) | U3_DUAL_MODE;
	u2 = (u2 & ~((1u << 0) | (1u << 1) | (1u << 2))) | U2_OTG_SEL;
	wr32(ippc, IPPC_U3_CTRL_0P, u3);
	wr32(ippc, IPPC_U2_CTRL_0P, u2);

	/* Device SW reset */
	wr32(ippc, IPPC_DEV_RST_CTRL, rst | 1u);
	delay(200);
	wr32(ippc, IPPC_DEV_RST_CTRL, rst & ~1u);

	/* Wait for IPPC clocks ready (SYSPLL + REF + SYS125 + U3_RST + U2_RST) */
	uint32_t want = (1u << 0) | (1u << 8) | (1u << 10) | (1u << 16);
	for (int i = 0; i < 2000; i++) {
		uint32_t s1 = rd32(ippc, IPPC_PW_STS1);
		uint32_t s2 = rd32(ippc, IPPC_PW_STS2);
		if ((s1 & want) == want && (s2 & 0x01u)) {
			break;
		}
		delay(100);
	}
}

static void
ep0_quiet_reset(vm_offset_t mac)
{
	uint32_t count, csr;

	wr32(mac, EP_RST_OFF, EP_RST_EP0_BIT);
	wr32(mac, EP_RST_OFF, 0);

	count = rd32(mac, RXCOUNT0_OFF);
	if (count > 512u) {
		count = 512u;
	}
	for (uint32_t i = 0; i < count; i++) {
		(void)rd8(mac, FIFO0_OFF);
	}

	csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
	csr |= EP0_RXPKTRDY | EP0_SETUPPKTRDY | EP0_SENTSTALL;
	wr32(mac, EP0CSR_OFF, csr);
}

static void
mac_soft_connect(vm_offset_t mac, vm_offset_t phy, int on)
{
	uint32_t v;

	if (on) {
		v = rd32(phy, PHY_U2_ACR3) | ACR3_PUPD_BIST_EN | ACR3_EN_PU_DP;
		wr32(phy, PHY_U2_ACR3, v);
		delay(1000);
		v = rd32(mac, MISC_CTRL_OFF) | 0x3u;
		wr32(mac, MISC_CTRL_OFF, v);
		v = rd32(mac, MAC2_PM_CTRL_OFF) | (1u << 6) | (1u << 0);
		wr32(mac, MAC2_PM_CTRL_OFF, v);
		delay(2000);
	} else {
		v = rd32(mac, MAC2_PM_CTRL_OFF) & ~((1u << 6) | (1u << 0));
		wr32(mac, MAC2_PM_CTRL_OFF, v);
		v = rd32(phy, PHY_U2_ACR3) & ~(ACR3_PUPD_BIST_EN | ACR3_EN_PU_DP);
		wr32(phy, PHY_U2_ACR3, v);
	}
}

/* ---- QMU GPD helpers (aligned volatile writes safe for Device-nGnRnE memory) ------------------- */

static uint8_t
gpd_chksum(const uint8_t *g)
{
	uint32_t sum = 0;
	for (uint32_t i = 0; i < 16u; i++) {
		if (i == 1u) {
			continue;
		}
		sum += g[i];
	}
	sum += 1u;
	return (uint8_t)(0xffu - (sum & 0xffu));
}

static inline void
gpd_zero_aligned(uint8_t *g)
{
	volatile uint32_t *w = (volatile uint32_t *)(void *)g;
	w[0] = 0;
	w[1] = 0;
	w[2] = 0;
	w[3] = 0;
}

static int
qmu_stop_queue(vm_offset_t mac, uint32_t csr_off)
{
	if (!(rd32(mac, csr_off) & QMU_Q_ACTIVE)) {
		return 0;
	}
	wr32(mac, csr_off, QMU_Q_STOP);
	for (int i = 0; i < 2000; i++) {
		if (!(rd32(mac, csr_off) & QMU_Q_ACTIVE)) {
			return 0;
		}
		delay(50);
	}
	return -1;
}

/* ---- ECM TX software ring (length-prefixed frames) --------------------------------------------- */

static int
ecm_txq_pending(void)
{
	return u.tx_left != 0 || u.txq_head != u.txq_tail;
}

static void
ecm_txq_put_bytes(const uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		u.txq[(u.txq_head + i) % ECM_TXQ_BYTES] = p[i];
	}
	u.txq_head += n;
}

static void
ecm_txq_get_bytes(uint8_t *p, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		p[i] = u.txq[(u.txq_tail + i) % ECM_TXQ_BYTES];
	}
	u.txq_tail += n;
}

static bool
ecm_tx_enqueue_mbuf(mbuf_t packet, size_t length)
{
	uint8_t frame[ECM_FRAME_MAX + 2];
	uint32_t wire;
	uint8_t hdr[2];

	if (!u.initialized || u.quiesced || !u.tx_ready || u.ecm_alt == 0) {
		u.tx_drops++;
		return false;
	}
	if (length < ETHER_HDR_LEN || length > ECM_FRAME_MAX) {
		u.tx_drops++;
		return false;
	}
	if (mbuf_copydata(packet, 0, length, frame) != 0) {
		u.tx_drops++;
		return false;
	}

	/* Avoid ZLP on exact 512-byte multiples by appending 1 pad byte (IP/Ethernet ignores it) */
	wire = (uint32_t)length;
	if ((wire % ECM_MPS) == 0) {
		frame[wire++] = 0;
	}

	lck_spin_lock(u.txq_lock);
	uint32_t used = u.txq_head - u.txq_tail;
	if (used + 2u + wire > ECM_TXQ_BYTES) {
		lck_spin_unlock(u.txq_lock);
		u.tx_drops++;
		return false;
	}
	hdr[0] = (uint8_t)(wire & 0xffu);
	hdr[1] = (uint8_t)((wire >> 8) & 0xffu);
	ecm_txq_put_bytes(hdr, 2u);
	ecm_txq_put_bytes(frame, wire);
	u.tx_frames++;
	lck_spin_unlock(u.txq_lock);

	usb_wake_worker();
	return true;
}

static uint32_t
ecm_tx_dequeue_chunk(uint8_t *buf, uint32_t max_len)
{
	uint32_t n = 0;

	lck_spin_lock(u.txq_lock);
	if (u.tx_left == 0 && u.txq_head != u.txq_tail) {
		uint8_t h[2];
		ecm_txq_get_bytes(h, 2u);
		u.tx_left = (uint32_t)h[0] | ((uint32_t)h[1] << 8);
	}
	if (u.tx_left != 0) {
		n = u.tx_left < max_len ? u.tx_left : max_len;
		ecm_txq_get_bytes(buf, n);
		u.tx_left -= n;
	}
	lck_spin_unlock(u.txq_lock);
	return n;
}

static void
ecm_rx_chunk(const uint8_t *data, uint32_t len)
{
	if (u.rx_flen + len > sizeof(u.rx_frame)) {
		u.rx_fbad = 1;
	} else if (len > 0) {
		memcpy(u.rx_frame + u.rx_flen, data, len);
		u.rx_flen += len;
	}

	/* With RX_BUF_STRIDE=2048, a single GPD receives a complete Ethernet frame up to the short packet */
	if (len != 0 && len == RX_BUF_STRIDE) {
		return;
	}

	uint32_t flen = u.rx_flen;
	int bad = u.rx_fbad;
	u.rx_flen = 0;
	u.rx_fbad = 0;

	if (bad || flen < ETHER_HDR_LEN || flen > ECM_FRAME_MAX) {
		if (flen > 0) {
			u.rx_drops++;
		}
		return;
	}

	u.rx_frames++;
	if (u.rx_frames <= 4) {
		printf("mtk_usb_ecm: RX frame #%u len=%u ethertype=0x%02x%02x\n",
		    u.rx_frames, flen, u.rx_frame[12], u.rx_frame[13]);
	}
	hakios_net_input(u.rx_frame, flen);
}

/* ---- QMU TX EP1 (0x81) ------------------------------------------------------------------------- */

static int
tx_ring_alloc_once(void)
{
	if (u.tx_ring_pa && u.tx_buf_base) {
		return 0;
	}

	if (!u.tx_ring_alloc) {
		vm_offset_t va = 0;
		if (kmem_alloc(kernel_map, &va, PAGE_SIZE,
		    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mtk_usb_ecm: TX ring alloc failed\n");
			return -1;
		}
		u.tx_ring_alloc = (void *)va;
		u.tx_ring_pa = dma_pa(u.tx_ring_alloc);
		if (!u.tx_ring_pa || (u.tx_ring_pa & (PAGE_SIZE - 1u)) || (u.tx_ring_pa >> 36)) {
			printf("mtk_usb_ecm: TX ring PA invalid 0x%llx\n", (unsigned long long)u.tx_ring_pa);
			return -1;
		}
		dc_civac(u.tx_ring_alloc, PAGE_SIZE);
		u.tx_ring_uc = ml_io_map((vm_offset_t)u.tx_ring_pa, PAGE_SIZE);
		if (!u.tx_ring_uc) {
			printf("mtk_usb_ecm: TX ring uncached map failed\n");
			return -1;
		}
		u.tx_gpd_cpu = (uint8_t *)u.tx_ring_uc;
		for (uint32_t i = 0; i < TX_RING_N; i++) {
			u.tx_gpd_pa[i] = u.tx_ring_pa + (uint64_t)i * GPD_SIZE;
		}
	}

	if (!u.tx_buf_base) {
		vm_offset_t va = 0;
		size_t total = (size_t)TX_RING_N * TX_BUF_STRIDE;
		if (kmem_alloc(kernel_map, &va, total,
		    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mtk_usb_ecm: TX buf alloc failed\n");
			return -1;
		}
		u.tx_buf_base = (void *)va;
		for (uint32_t i = 0; i < TX_RING_N; i++) {
			u.tx_buf[i] = (uint8_t *)u.tx_buf_base + (size_t)i * TX_BUF_STRIDE;
			u.tx_buf_pa[i] = dma_pa(u.tx_buf[i]);
			if (!u.tx_buf_pa[i] || (u.tx_buf_pa[i] & 63u) || (u.tx_buf_pa[i] >> 36) ||
			    ((u.tx_buf_pa[i] & (PAGE_SIZE - 1u)) + TX_BUF_STRIDE > PAGE_SIZE)) {
				printf("mtk_usb_ecm: TX buf[%u] PA bad 0x%llx\n",
				    i, (unsigned long long)u.tx_buf_pa[i]);
				return -1;
			}
		}
	}
	return 0;
}

static void
tx_ep_program_regs(vm_offset_t mac, uint64_t sar_pa)
{
	wr32(mac, TX1CSR0_OFF, 512u | TX_DMAREQEN);
	wr32(mac, TX1CSR1_OFF, TX_SLOT(2) | TX_TYPE(0));
	wr32(mac, TX1CSR2_OFF, TX_FIFOADDR(0) | TX_FIFOSEGSIZE(9));
	wr32(mac, QCR0_OFF, rd32(mac, QCR0_OFF) | (1u << 1));    /* TX1 checksum enable */
	wr32(mac, QCR1_OFF, rd32(mac, QCR1_OFF) & ~(1u << 1));   /* TX1 ZLP off (we pad +1B on 512-multiples) */
	wr32(mac, TQERRIESR0_OFF, rd32(mac, TQERRIESR0_OFF) | QMU_TX1_CS_ERR | QMU_TX1_LEN_ERR);
	wr32(mac, TQSAR1_OFF, (uint32_t)sar_pa);
	wr32(mac, TQHIAR1_OFF, (uint32_t)((sar_pa >> 32) & 0xfu));
}

static void
tx_config(vm_offset_t mac)
{
	if (u.tx_ready) {
		return;
	}
	if (qmu_stop_queue(mac, TQCSR1_OFF) != 0) {
		printf("mtk_usb_ecm: TX stop timeout in config\n");
		return;
	}
	if (tx_ring_alloc_once() != 0) {
		return;
	}
	for (uint32_t i = 0; i < TX_RING_N; i++) {
		gpd_zero_aligned(u.tx_gpd_cpu + i * GPD_SIZE);
	}
	dma_mb();

	u.tx_enq = 0;
	u.tx_deq = 0;
	u.tx_inflight = 0;
	u.tx_started = 0;
	u.tx_progress_ms = now_ms();

	tx_ep_program_regs(mac, u.tx_ring_pa);
	wr32(mac, QIESR0_OFF, QMU_TX1_DONE | QMU_RX1_DONE);
	wr32(mac, QIESR1_OFF, QMU_QIER1_MASK);
	u.tx_ready = 1;
	printf("mtk_usb_ecm: TX1 QMU ready ring_pa=0x%llx\n", (unsigned long long)u.tx_ring_pa);
}

static void
tx_drain(vm_offset_t mac)
{
	uint32_t freed = 0;
	uint32_t q0;

	if (!u.tx_ready) {
		return;
	}
	q0 = rd32(mac, QISAR0_OFF);
	if (q0 & QMU_TX1_DONE) {
		wr32(mac, QISAR0_OFF, QMU_TX1_DONE);
	}
	uint32_t q1 = rd32(mac, QISAR1_OFF);
	uint32_t txq_err = q1 & (QISAR1_TXQ_EMPTY | QISAR1_TXQ_CSERR | QISAR1_TXQ_LENERR);
	if (txq_err) {
		wr32(mac, QISAR1_OFF, txq_err);
		uint32_t e = rd32(mac, TQERRIR0_OFF);
		if (e) {
			wr32(mac, TQERRIR0_OFF, e);
		}
	}

	dma_mb();
	while (u.tx_inflight > 0) {
		uint8_t *g = u.tx_gpd_cpu + u.tx_deq * GPD_SIZE;
		uint8_t flags = *(volatile uint8_t *)g;
		if (flags & 1u) {
			break;                          /* HWO still 1: owned by QMU */
		}
		gpd_zero_aligned(g);
		u.tx_deq = (u.tx_deq + 1u) % TX_RING_N;
		u.tx_inflight--;
		freed++;
	}
	if (freed) {
		u.tx_progress_ms = now_ms();
	}
}

static void
tx_push(vm_offset_t mac)
{
	uint32_t queued = 0;

	if (!u.tx_ready || !ecm_txq_pending()) {
		return;
	}
	tx_drain(mac);

	/* Keep at least 2 slots free so enq+1 sentinel never touches deq */
	while (u.tx_inflight < TX_RING_N - 2u) {
		uint32_t enq = u.tx_enq;
		uint32_t next_idx = (enq + 1u) % TX_RING_N;
		uint8_t *buf = u.tx_buf[enq];
		uint32_t n = ecm_tx_dequeue_chunk(buf, ECM_MPS);
		if (n == 0) {
			break;
		}
		uint64_t bpa = u.tx_buf_pa[enq];
		uint64_t npa = u.tx_gpd_pa[next_idx];
		uint8_t *g = u.tx_gpd_cpu + enq * GPD_SIZE;
		uint8_t *g_next = u.tx_gpd_cpu + next_idx * GPD_SIZE;

		/* Clear sentinel next GPD first, then build current GPD (32-bit aligned writes) */
		gpd_zero_aligned(g_next);
		uint32_t ext = ((uint32_t)((bpa >> 32) & 0xfu) | (((uint32_t)((npa >> 32) & 0xfu)) << 4));
		volatile uint32_t *gw = (volatile uint32_t *)(void *)g;
		gw[0] = (1u << 7) | (ext << 16);    /* IOC=1, chksum=0, ext_addr in byte 2 */
		gw[1] = (uint32_t)npa;
		gw[2] = (uint32_t)bpa;
		gw[3] = n;                          /* length, ZLP=0 */

		dc_civac(buf, (n + 63u) & ~63u);
		dma_mb();
		*(volatile uint8_t *)g = (1u << 0) | (1u << 7);  /* publish HWO | IOC */
		dma_mb();

		u.tx_enq = next_idx;
		u.tx_inflight++;
		if (u.tx_inflight == 1) {
			u.tx_progress_ms = now_ms();
		}
		queued++;
	}

	if (!queued) {
		return;
	}

	uint32_t tq = rd32(mac, TQCSR1_OFF);
	if (!u.tx_started || !(tq & QMU_Q_ACTIVE)) {
		uint64_t kpa = u.tx_gpd_pa[u.tx_deq];
		wr32(mac, TQSAR1_OFF, (uint32_t)kpa);
		wr32(mac, TQHIAR1_OFF, (uint32_t)((kpa >> 32) & 0xfu));
		wr32(mac, TQCSR1_OFF, QMU_Q_START);
		u.tx_started = 1;
	} else {
		wr32(mac, TQCSR1_OFF, QMU_Q_RESUME);
	}
}

static void
tx_watchdog(vm_offset_t mac)
{
	if (!u.tx_ready || u.tx_inflight == 0) {
		return;
	}
	uint64_t t = now_ms();
	if (t < u.tx_progress_ms + 500ULL) {
		return;
	}

	/* First try a soft doorbell kick if QMU went idle with pending GPDs */
	uint32_t tq = rd32(mac, TQCSR1_OFF);
	if (!(tq & QMU_Q_ACTIVE)) {
		uint64_t kpa = u.tx_gpd_pa[u.tx_deq];
		wr32(mac, TQSAR1_OFF, (uint32_t)kpa);
		wr32(mac, TQHIAR1_OFF, (uint32_t)((kpa >> 32) & 0xfu));
		wr32(mac, TQCSR1_OFF, QMU_Q_START);
		u.tx_started = 1;
		u.tx_progress_ms = t;
		return;
	}

	/* Stalled >= 1500 ms: salvage by stopping QMU, resetting TX1 FIFO, and restarting at deq */
	if (t >= u.tx_progress_ms + 1500ULL && t >= u.tx_salvage_ms + 1500ULL) {
		u.tx_salvage_ms = t;
		u.tx_salvages++;
		(void)qmu_stop_queue(mac, TQCSR1_OFF);
		wr32(mac, EP_RST_OFF, EP_RST_TX1_BIT);
		wr32(mac, EP_RST_OFF, 0);
		uint64_t kpa = u.tx_gpd_pa[u.tx_deq];
		tx_ep_program_regs(mac, kpa);
		wr32(mac, TQCSR1_OFF, QMU_Q_START);
		u.tx_started = 1;
		u.tx_progress_ms = t;
		if (u.tx_salvages <= 5) {
			printf("mtk_usb_ecm: TX salvage #%u inflight=%u deq=%u\n",
			    u.tx_salvages, u.tx_inflight, u.tx_deq);
		}
	}
}

/* ---- QMU RX EP1 (0x01) ------------------------------------------------------------------------- */

static int
rx_ring_alloc_once(void)
{
	if (u.rx_ring_pa && u.rx_buf_base) {
		return 0;
	}

	if (!u.rx_ring_alloc) {
		vm_offset_t va = 0;
		if (kmem_alloc(kernel_map, &va, PAGE_SIZE,
		    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mtk_usb_ecm: RX ring alloc failed\n");
			return -1;
		}
		u.rx_ring_alloc = (void *)va;
		u.rx_ring_pa = dma_pa(u.rx_ring_alloc);
		if (!u.rx_ring_pa || (u.rx_ring_pa & (PAGE_SIZE - 1u)) || (u.rx_ring_pa >> 36)) {
			printf("mtk_usb_ecm: RX ring PA invalid 0x%llx\n", (unsigned long long)u.rx_ring_pa);
			return -1;
		}
		dc_civac(u.rx_ring_alloc, PAGE_SIZE);
		u.rx_ring_uc = ml_io_map((vm_offset_t)u.rx_ring_pa, PAGE_SIZE);
		if (!u.rx_ring_uc) {
			printf("mtk_usb_ecm: RX ring uncached map failed\n");
			return -1;
		}
		u.rx_gpd_cpu = (uint8_t *)u.rx_ring_uc;
		for (uint32_t i = 0; i < RX_RING_N; i++) {
			u.rx_gpd_pa[i] = u.rx_ring_pa + (uint64_t)i * GPD_SIZE;
		}
	}

	if (!u.rx_buf_base) {
		vm_offset_t va = 0;
		size_t total = (size_t)RX_RING_N * RX_BUF_STRIDE;
		if (kmem_alloc(kernel_map, &va, total,
		    KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mtk_usb_ecm: RX buf alloc failed\n");
			return -1;
		}
		u.rx_buf_base = (void *)va;
		for (uint32_t i = 0; i < RX_RING_N; i++) {
			u.rx_buf[i] = (uint8_t *)u.rx_buf_base + (size_t)i * RX_BUF_STRIDE;
			u.rx_buf_pa[i] = dma_pa(u.rx_buf[i]);
			if (!u.rx_buf_pa[i] || (u.rx_buf_pa[i] & 63u) || (u.rx_buf_pa[i] >> 36) ||
			    ((u.rx_buf_pa[i] & (PAGE_SIZE - 1u)) + RX_BUF_STRIDE > PAGE_SIZE)) {
				printf("mtk_usb_ecm: RX buf[%u] PA bad 0x%llx\n",
				    i, (unsigned long long)u.rx_buf_pa[i]);
				return -1;
			}
		}
	}
	return 0;
}

static void
rx_arm_slot(uint32_t i)
{
	uint8_t local[16];
	uint64_t next = u.rx_gpd_pa[(i + 1u) % RX_RING_N];
	uint64_t buf = u.rx_buf_pa[i];

	/* Assemble in stack memory first so checksum calculation has zero uncached reads */
	bzero(local, sizeof(local));
	local[0] = 1u << 7;                             /* IOC; HWO added after checksum */
	local[2] = (uint8_t)(RX_BUF_STRIDE & 0xffu);
	local[3] = (uint8_t)((RX_BUF_STRIDE >> 8) & 0xffu);
	local[4] = (uint8_t)(next & 0xffu);
	local[5] = (uint8_t)((next >> 8) & 0xffu);
	local[6] = (uint8_t)((next >> 16) & 0xffu);
	local[7] = (uint8_t)((next >> 24) & 0xffu);
	local[8] = (uint8_t)(buf & 0xffu);
	local[9] = (uint8_t)((buf >> 8) & 0xffu);
	local[10] = (uint8_t)((buf >> 16) & 0xffu);
	local[11] = (uint8_t)((buf >> 24) & 0xffu);
	local[14] = (uint8_t)(((buf >> 32) & 0xfu) | (((next >> 32) & 0xfu) << 4));
	local[1] = gpd_chksum(local);

	dc_civac(u.rx_buf[i], RX_BUF_STRIDE);

	volatile uint32_t *gw = (volatile uint32_t *)(void *)(u.rx_gpd_cpu + i * GPD_SIZE);
	uint32_t w0, w1, w2, w3;
	memcpy(&w0, local + 0, 4);
	memcpy(&w1, local + 4, 4);
	memcpy(&w2, local + 8, 4);
	memcpy(&w3, local + 12, 4);
	gw[0] = w0;
	gw[1] = w1;
	gw[2] = w2;
	gw[3] = w3;
	dma_mb();
	*(volatile uint8_t *)(u.rx_gpd_cpu + i * GPD_SIZE) = (1u << 0) | (1u << 7); /* HWO | IOC */
	dma_mb();
}

static void
rx_ep_program_regs(vm_offset_t mac)
{
	wr32(mac, RX1CSR0_OFF, 512u | RX_DMAREQEN);
	wr32(mac, RX1CSR1_OFF, RX_SLOT(2) | RX_TYPE(0));
	wr32(mac, RX1CSR2_OFF, RX_FIFOADDR(0) | RX_FIFOSEGSIZE(9));
	wr32(mac, QCR0_OFF, rd32(mac, QCR0_OFF) | (1u << 17));   /* RX1 checksum enable */
	wr32(mac, QCR3_OFF, (rd32(mac, QCR3_OFF) & ~(1u << 1)) | (1u << 17)); /* RX1 ZLP off, COZ on */
	wr32(mac, RQERRIESR0_OFF, rd32(mac, RQERRIESR0_OFF) | QMU_RX1_LEN_ERR | QMU_RX1_CS_ERR);
	wr32(mac, RQERRIESR1_OFF, rd32(mac, RQERRIESR1_OFF) | QMU_RX1_ZLP_ERR);
	wr32(mac, RQSAR1_OFF, (uint32_t)u.rx_ring_pa);
	wr32(mac, RQHIAR1_OFF, (uint32_t)((u.rx_ring_pa >> 32) & 0xfu));
}

static void
rx_config(vm_offset_t mac)
{
	if (u.rx_ready) {
		return;
	}
	if (qmu_stop_queue(mac, RQCSR1_OFF) != 0) {
		printf("mtk_usb_ecm: RX stop timeout in config\n");
		return;
	}
	if (rx_ring_alloc_once() != 0) {
		return;
	}
	u.rx_deq = 0;
	u.rx_flen = 0;
	u.rx_fbad = 0;

	rx_ep_program_regs(mac);
	for (uint32_t i = 0; i < RX_RING_N; i++) {
		rx_arm_slot(i);
	}
	wr32(mac, RQCSR1_OFF, QMU_Q_START);
	u.rx_ready = 1;
	printf("mtk_usb_ecm: RX1 QMU ready ring_pa=0x%llx\n", (unsigned long long)u.rx_ring_pa);
}

static void
rx_recover(vm_offset_t mac)
{
	if (!u.rx_ring_pa) {
		return;
	}
	(void)qmu_stop_queue(mac, RQCSR1_OFF);
	wr32(mac, EP_RST_OFF, EP_RST_RX1_BIT);
	wr32(mac, EP_RST_OFF, 0);
	rx_ep_program_regs(mac);
	u.rx_deq = 0;
	u.rx_flen = 0;
	u.rx_fbad = 0;
	for (uint32_t i = 0; i < RX_RING_N; i++) {
		rx_arm_slot(i);
	}
	wr32(mac, RQCSR1_OFF, QMU_Q_START);
	u.rx_ready = 1;
}

static uint32_t
rx_cur_slot(vm_offset_t mac)
{
	uint32_t cpr = rd32(mac, RQCPR1_OFF);
	uint32_t base = (uint32_t)u.rx_ring_pa;
	if (cpr < base || cpr >= base + RX_RING_N * GPD_SIZE) {
		return 0xffu;
	}
	return (cpr - base) / GPD_SIZE;
}

static void
rx_poll_and_drain(vm_offset_t mac)
{
	if (!u.rx_ready) {
		return;
	}

	uint32_t q1 = rd32(mac, QISAR1_OFF);
	uint32_t rxq = q1 & (QISAR1_RXQ_EMPTY | QISAR1_RXQ_CSERR | QISAR1_RXQ_LENERR | QISAR1_RXQ_ZLPERR);
	if (rxq) {
		wr32(mac, QISAR1_OFF, rxq);
		if (rxq & (QISAR1_RXQ_CSERR | QISAR1_RXQ_LENERR)) {
			uint32_t e = rd32(mac, RQERRIR0_OFF);
			if (e) {
				wr32(mac, RQERRIR0_OFF, e);
			}
			u.rx_errors++;
			rx_recover(mac);
			return;
		}
		if (rxq & QISAR1_RXQ_ZLPERR) {
			uint32_t e = rd32(mac, RQERRIR1_OFF);
			if (e) {
				wr32(mac, RQERRIR1_OFF, e);
			}
		}
	}

	uint32_t q0 = rd32(mac, QISAR0_OFF);
	if (q0 & QMU_RX1_DONE) {
		wr32(mac, QISAR0_OFF, QMU_RX1_DONE);
	}

	dma_mb();
	uint32_t cur = rx_cur_slot(mac);
	if (cur == 0xffu) {
		cur = u.rx_deq;
	}

	/* Frame mode: walk whole ring from cur and drain every completed GPD (HWO == 0) */
	uint32_t done = 0;
	for (uint32_t n = 0; n < RX_RING_N; n++) {
		uint32_t i = (cur + n) % RX_RING_N;
		uint8_t *g = u.rx_gpd_cpu + i * GPD_SIZE;
		if (*(volatile uint8_t *)g & 1u) {
			continue;                               /* still owned by QMU */
		}
		dc_civac(u.rx_buf[i], RX_BUF_STRIDE);
		uint32_t dw3 = *(volatile uint32_t *)(void *)(g + 12);
		uint32_t len = dw3 & 0xffffu;
		if (len > RX_BUF_STRIDE) {
			len = RX_BUF_STRIDE;
		}
		u.rx_packets++;
		ecm_rx_chunk(u.rx_buf[i], len);
		rx_arm_slot(i);
		u.rx_deq = (i + 1u) % RX_RING_N;
		done++;
	}

	if (done || !(rd32(mac, RQCSR1_OFF) & QMU_Q_ACTIVE)) {
		if (!(rd32(mac, RQCSR1_OFF) & QMU_Q_ACTIVE)) {
			wr32(mac, RQCSR1_OFF, QMU_Q_START);
		} else {
			wr32(mac, RQCSR1_OFF, QMU_Q_RESUME);
		}
	}
}

/* ---- EP3 Interrupt IN (0x83) CDC-ECM link notifications ---------------------------------------- */

static void
ep3_config(vm_offset_t mac)
{
	wr32(mac, TX3CSR0_OFF, 16u);
	wr32(mac, TX3CSR1_OFF, 2u << 8);
	wr32(mac, TX3CSR2_OFF, (0x60u >> 4) | (4u << 16));
}

static void
ep3_link_poll(vm_offset_t mac)
{
	uint32_t step = u.notify_step;
	if (step == 0 || !u.cfg_seen) {
		return;
	}
	if (rd32(mac, TX3CSR0_OFF) & TX_TXPKTRDY) {
		return;                                     /* host hasn't polled previous packet yet */
	}
	if (step == 1) {
		static const uint8_t conn[8] = { 0xA1, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00 };
		for (uint32_t i = 0; i < 8; i += 4) {
			uint32_t w = (uint32_t)conn[i] | ((uint32_t)conn[i + 1] << 8) |
			    ((uint32_t)conn[i + 2] << 16) | ((uint32_t)conn[i + 3] << 24);
			wr32(mac, FIFO3_OFF, w);
		}
		wr32(mac, TX3CSR0_OFF, 16u | TX_TXPKTRDY);
		u.notify_step = 2;
	} else {
		static const uint8_t speed[16] = {
			0xA1, 0x2A, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
			0x00, 0x38, 0x9C, 0x1C, 0x00, 0x38, 0x9C, 0x1C  /* 480 Mbps up & down */
		};
		for (uint32_t i = 0; i < 16; i += 4) {
			uint32_t w = (uint32_t)speed[i] | ((uint32_t)speed[i + 1] << 8) |
			    ((uint32_t)speed[i + 2] << 16) | ((uint32_t)speed[i + 3] << 24);
			wr32(mac, FIFO3_OFF, w);
		}
		wr32(mac, TX3CSR0_OFF, 16u | TX_TXPKTRDY);
		u.notify_step = 0;
	}
	u.notify_sent++;
	if (u.notify_sent <= 4) {
		printf("mtk_usb_ecm: EP3 link notify #%u sent\n", u.notify_sent);
	}
}

static void
configure_data_endpoints(vm_offset_t mac)
{
	tx_config(mac);
	rx_config(mac);
	ep3_config(mac);
}

/* ---- EP0 control state machine ----------------------------------------------------------------- */

static void
ep0_write_fifo(vm_offset_t mac, const uint8_t *src, uint32_t len)
{
	uint32_t i = 0;
	while (i + 4u <= len) {
		uint32_t w = (uint32_t)src[i] | ((uint32_t)src[i + 1] << 8) |
		    ((uint32_t)src[i + 2] << 16) | ((uint32_t)src[i + 3] << 24);
		wr32(mac, FIFO0_OFF, w);
		i += 4u;
	}
	if (i + 2u <= len) {
		uint16_t w = (uint16_t)src[i] | ((uint16_t)src[i + 1] << 8);
		wr16(mac, FIFO0_OFF, w);
		i += 2u;
	}
	while (i < len) {
		wr8(mac, FIFO0_OFF, src[i++]);
	}
}

static void
ep0_ack(vm_offset_t mac)
{
	uint32_t csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
	csr |= EP0_SETUPPKTRDY | EP0_DATAEND;
	wr32(mac, EP0CSR_OFF, csr);
}

static void
ep0_stall(vm_offset_t mac)
{
	uint32_t csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
	csr |= EP0_SETUPPKTRDY | EP0_SENDSTALL;
	wr32(mac, EP0CSR_OFF, csr);
}

static void
ep0_send(vm_offset_t mac, const uint8_t *src, uint32_t desc_len, uint32_t wlen)
{
	uint32_t total = desc_len < wlen ? desc_len : wlen;
	if (wlen == 0) {
		ep0_ack(mac);
		return;
	}
	if (total > sizeof(u.ep0_tx_buf)) {
		total = sizeof(u.ep0_tx_buf);
	}
	memcpy(u.ep0_tx_buf, src, total);

	/* Clear SETUPPKTRDY + assert DPHTX (data-phase TX) before loading FIFO0 */
	uint32_t csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
	wr32(mac, EP0CSR_OFF, csr | EP0_SETUPPKTRDY | EP0_DPHTX);

	uint32_t sent = 0;
	while (sent < total) {
		uint32_t chunk = total - sent;
		if (chunk > 64u) {
			chunk = 64u;
		}
		ep0_write_fifo(mac, u.ep0_tx_buf + sent, chunk);
		csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
		wr32(mac, EP0CSR_OFF, csr | EP0_TXPKTRDY);
		for (uint32_t spin = 0; spin < 1000u; spin++) {
			if (!(rd32(mac, EP0CSR_OFF) & EP0_TXPKTRDY)) {
				break;
			}
			delay(10);
		}
		sent += chunk;
	}

	/* Wait briefly (~2 ms max) for FIFO drain, then always write DATAEND (0107 semantics) */
	delay(100);
	for (uint32_t spin = 0; spin < 20u && (rd32(mac, EP0CSR_OFF) & EP0_FIFOFULL); spin++) {
		delay(100);
	}
	csr = rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS;
	wr32(mac, EP0CSR_OFF, csr | EP0_DATAEND);
	u.ep0_tx_pending = 0;
}

static const uint8_t *
lookup_descriptor(uint32_t value, uint32_t *out_len)
{
	uint8_t dtype = (uint8_t)(value >> 8);
	uint8_t idx = (uint8_t)(value & 0xffu);

	switch (dtype) {
	case 0x01:
		*out_len = sizeof(usb_dev_desc);
		return usb_dev_desc;
	case 0x02:
		*out_len = sizeof(usb_cfg_desc_hs);
		return usb_cfg_desc_hs;
	case 0x03:
		switch (idx) {
		case 0: *out_len = sizeof(str_lang); return str_lang;
		case 1: *out_len = sizeof(str_mfg);  return str_mfg;
		case 2: *out_len = sizeof(str_prod); return str_prod;
		case 3: *out_len = sizeof(str_ser);  return str_ser;
		case 4: *out_len = sizeof(str_mac);  return str_mac;
		default: return NULL;
		}
	case 0x06:
		*out_len = sizeof(usb_dev_qual_desc);
		return usb_dev_qual_desc;
	case 0x07:
		*out_len = sizeof(usb_cfg_desc_fs);
		return usb_cfg_desc_fs;
	default:
		return NULL;
	}
}

static void
ep0_handle_setup(vm_offset_t mac)
{
	uint32_t count = rd32(mac, RXCOUNT0_OFF);
	if (count < 8u) {
		return;
	}
	if (count > 8u) {
		for (uint32_t i = 0; i < count; i++) {
			(void)rd8(mac, FIFO0_OFF);
		}
		return;
	}

	uint32_t w0 = rd32(mac, FIFO0_OFF);
	uint32_t w1 = rd32(mac, FIFO0_OFF);
	uint8_t type  = (uint8_t)(w0 & 0xffu);
	uint8_t req   = (uint8_t)((w0 >> 8) & 0xffu);
	uint32_t val  = (w0 >> 16) & 0xffffu;
	uint32_t idx  = w1 & 0xffffu;
	uint32_t wlen = (w1 >> 16) & 0xffffu;

	u.setups_seen++;
	u.last_ev_ms = now_ms();
	u.ep0_tx_pending = 0;

	/* CDC-ECM class-specific requests (SET_ETHERNET_PACKET_FILTER 0x43, multicast, etc.) */
	if ((type & 0x60u) == 0x20u) {
		if ((type & 0x80u) == 0 && wlen == 0) {
			ep0_ack(mac);
		} else {
			ep0_stall(mac);
		}
		return;
	}

	/* Standard requests */
	if ((type & 0x60u) != 0) {
		ep0_stall(mac);
		return;
	}

	switch (req) {
	case 6: {                                   /* GET_DESCRIPTOR */
		uint32_t dlen = 0;
		const uint8_t *desc = lookup_descriptor(val, &dlen);
		if (!desc) {
			ep0_stall(mac);
		} else if (wlen == 0) {
			ep0_ack(mac);
		} else {
			ep0_send(mac, desc, dlen, wlen);
		}
		return;
	}
	case 5: {                                   /* SET_ADDRESS */
		if (wlen != 0) {
			ep0_stall(mac);
			return;
		}
		uint32_t dc = rd32(mac, DEV_CONF_OFF) & ~0x7f000000u;
		dc |= (val & 0x7fu) << 24;
		wr32(mac, DEV_CONF_OFF, dc);
		u.dev_addr = (uint8_t)(val & 0x7fu);
		ep0_ack(mac);
		printf("mtk_usb_ecm: SET_ADDRESS %u\n", u.dev_addr);
		return;
	}
	case 9: {                                   /* SET_CONFIGURATION */
		u.config = (uint8_t)(val & 0xffu);
		u.cfg_seen = 1;
		ep0_ack(mac);
		if (u.config != 0) {
			configure_data_endpoints(mac);
		}
		printf("mtk_usb_ecm: SET_CONFIGURATION %u (addr=%u)\n", u.config, u.dev_addr);
		return;
	}
	case 8: {                                   /* GET_CONFIGURATION */
		uint8_t b = u.config;
		ep0_send(mac, &b, 1u, wlen);
		return;
	}
	case 0: {                                   /* GET_STATUS */
		uint8_t st[2] = { 0, 0 };
		ep0_send(mac, st, 2u, wlen);
		return;
	}
	case 10: {                                  /* GET_INTERFACE */
		uint8_t b = (idx == 1) ? u.ecm_alt : 0;
		ep0_send(mac, &b, 1u, wlen);
		return;
	}
	case 11: {                                  /* SET_INTERFACE */
		if (idx == 1) {
			u.ecm_alt = (uint8_t)(val & 1u);
			printf("mtk_usb_ecm: ECM data alt=%u -> link %s\n",
			    u.ecm_alt, u.ecm_alt ? "UP" : "DOWN");
			if (u.ecm_alt == 1) {
				if (!u.tx_ready || !u.rx_ready) {
					configure_data_endpoints(mac);
				}
				u.notify_step = 1;
				hakios_net_link_state(true);
			} else {
				hakios_net_link_state(false);
			}
		}
		ep0_ack(mac);
		return;
	}
	case 1: {                                   /* CLEAR_FEATURE (ENDPOINT_HALT) */
		if (wlen == 0 && val == 0) {
			if (idx == 0x81 && u.tx_ready) {
				uint32_t csr = (rd32(mac, TX1CSR0_OFF) & ~(TX_SENTSTALL | TX_TXPKTRDY)) & ~TX_SENDSTALL;
				wr32(mac, TX1CSR0_OFF, csr | TX_SENTSTALL);
			} else if (idx == 0x01 && u.rx_ready) {
				uint32_t csr = (rd32(mac, RX1CSR0_OFF) & ~(RX_SENTSTALL | RX_RXPKTRDY)) & ~RX_SENDSTALL;
				wr32(mac, RX1CSR0_OFF, csr | RX_SENTSTALL);
			}
			ep0_ack(mac);
		} else {
			ep0_stall(mac);
		}
		return;
	}
	case 3:                                     /* SET_FEATURE */
		if (wlen == 0) {
			ep0_ack(mac);
		} else {
			ep0_stall(mac);
		}
		return;
	default:
		ep0_stall(mac);
		return;
	}
}

static void
ep0_service(vm_offset_t mac)
{
	uint32_t episr = rd32(mac, EPISR_OFF);
	if (episr & (EPISR_EP0ISR | EPISR_SETUPEND)) {
		wr32(mac, EPISR_OFF, episr & (EPISR_EP0ISR | EPISR_SETUPEND));
	}

	uint32_t csr = rd32(mac, EP0CSR_OFF);
	if (!(csr & EP0_SETUPPKTRDY) && (csr & EP0_MAXPKTSZ_MSK) != 64u) {
		uint32_t v = (csr & EP0_W1C_BITS & ~EP0_MAXPKTSZ_MSK) | 64u;
		wr32(mac, EP0CSR_OFF, v);
		csr = rd32(mac, EP0CSR_OFF);
	}
	if (csr & EP0_SENTSTALL) {
		wr32(mac, EP0CSR_OFF, (csr & EP0_W1C_BITS) | EP0_SENTSTALL);
		csr = rd32(mac, EP0CSR_OFF);
	}
	if (csr & EP0_SETUPPKTRDY) {
		ep0_handle_setup(mac);
	}

	csr = rd32(mac, EP0CSR_OFF);
	if (csr & EP0_RXPKTRDY) {
		uint32_t count = rd32(mac, RXCOUNT0_OFF);
		if (count > 512u) {
			count = 512u;
		}
		for (uint32_t i = 0; i < count; i++) {
			(void)rd8(mac, FIFO0_OFF);
		}
		wr32(mac, EP0CSR_OFF, (csr & EP0_W1C_BITS) | EP0_RXPKTRDY | EP0_DATAEND);
	}
}

/* ---- Bus reset & rearm ------------------------------------------------------------------------- */

static void
usb_rearm(vm_offset_t mac)
{
	if (u.tx_ready) {
		(void)qmu_stop_queue(mac, TQCSR1_OFF);
		wr32(mac, EP_RST_OFF, EP_RST_TX1_BIT);
		wr32(mac, EP_RST_OFF, 0);
	}
	if (u.rx_ready) {
		(void)qmu_stop_queue(mac, RQCSR1_OFF);
		wr32(mac, EP_RST_OFF, EP_RST_RX1_BIT);
		wr32(mac, EP_RST_OFF, 0);
	}

	ep0_quiet_reset(mac);
	uint32_t csr = (rd32(mac, EP0CSR_OFF) & EP0_W1C_BITS & ~EP0_MAXPKTSZ_MSK) | 64u;
	wr32(mac, EP0CSR_OFF, csr);

	wr32(mac, EPISR_OFF, rd32(mac, EPISR_OFF));
	wr32(mac, DEV_CONF_OFF, rd32(mac, DEV_CONF_OFF) & ~0x7f000000u);

	u.config = 0;
	u.ecm_alt = 0;
	u.dev_addr = 0;
	u.cfg_seen = 0;
	u.ep0_tx_pending = 0;
	u.notify_step = 0;
	u.tx_ready = 0;
	u.tx_started = 0;
	u.tx_enq = 0;
	u.tx_deq = 0;
	u.tx_inflight = 0;
	u.rx_ready = 0;
	u.rx_deq = 0;
	u.rx_flen = 0;
	u.rx_fbad = 0;
	hakios_net_link_state(false);
}

static void
usb_check_bus_reset(vm_offset_t mac)
{
	uint32_t m2 = rd32(mac, MAC2_INTR_STS_OFF);
	uint64_t t = now_ms();

	if (m2 & MAC2_INTR_RESET) {
		wr32(mac, MAC2_INTR_STS_OFF, m2 & (MAC2_INTR_RESET | MAC2_INTR_RESUME | MAC2_INTR_SUSPEND));
		u.resets_seen++;
		u.last_ev_ms = t;
		if (u.reset_phase == 0) {
			u.reset_phase = 1;
			u.reset_seen_ms = t;
			usb_rearm(mac);
			if (u.resets_seen <= 6) {
				printf("mtk_usb_ecm: USB bus reset #%u\n", u.resets_seen);
			}
		}
		return;
	}
	if (m2 & (MAC2_INTR_RESUME | MAC2_INTR_SUSPEND)) {
		wr32(mac, MAC2_INTR_STS_OFF, m2 & (MAC2_INTR_RESUME | MAC2_INTR_SUSPEND));
	}
	if (u.reset_phase == 1 && (t >= u.reset_seen_ms + 50ULL || (rd32(mac, EP0CSR_OFF) & EP0_SETUPPKTRDY))) {
		u.reset_phase = 0;
	}
}

/* ---- IRQ handler & worker thread --------------------------------------------------------------- */

static void
mtk_usb_irq_handler(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	u.irq_masked = 1;
	u.irq_count++;
	usb_wake_worker();
}

static void
usb_service_once(void)
{
	vm_offset_t mac = u.mac;

	usb_check_bus_reset(mac);
	ep0_service(mac);
	rx_poll_and_drain(mac);
	tx_drain(mac);
	tx_push(mac);
	ep3_link_poll(mac);
	tx_watchdog(mac);
}

static void
mtk_usb_worker(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		if (u.quiesced) {
			delay_for_interval(50, NSEC_PER_MSEC);
			continue;
		}

		/* Drain up to 8 quick passes if events keep arriving */
		for (int pass = 0; pass < 8 && !u.quiesced; pass++) {
			usb_service_once();
			uint32_t lv1 = rd32(u.mac, LV1ISR_OFF);
			if (!(lv1 & (LV1_BMU_INTR | LV1_QMU_INTR | LV1_MAC2_INTR | LV1_EP0_INTR)) &&
			    !ecm_txq_pending() && u.notify_step == 0) {
				break;
			}
		}

		/* If never configured after 12s (and 2s quiet), detach/reattach once so late cables work */
		uint64_t t = now_ms();
		if (!u.cfg_seen && u.reattach_cnt < 6 &&
		    t >= u.attach_ms + 12000ULL && t >= u.last_ev_ms + 2000ULL) {
			u.reattach_cnt++;
			printf("mtk_usb_ecm: no host config yet, reattach #%u (setups=%u rst=%u irq=%u ep0=0x%x otg=0x%x)\n",
			    u.reattach_cnt, u.setups_seen, u.resets_seen, u.irq_count,
			    rd32(u.mac, EP0CSR_OFF), rd32(u.ippc, IPPC_OTG_STS));
			mac_soft_connect(u.mac, u.phy, 0);
			usb_rearm(u.mac);
			delay_for_interval(150, NSEC_PER_MSEC);
			mac_soft_connect(u.mac, u.phy, 1);
			u.attach_ms = now_ms();
			u.last_ev_ms = u.attach_ms;
		}

		if (u.irq_attached && u.irq_masked && !u.quiesced) {
			u.irq_masked = 0;
			gic_v3_spi_unmask(MTU3_IRQ_INTID);
		}

		/* Sleep up to 2 ms (or wake immediately on IRQ / TX enqueue) */
		uint64_t deadline = 0;
		clock_interval_to_deadline(1, NSEC_PER_MSEC, &deadline);
		assert_wait_deadline((event_t)&u.wake_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

/* ---- HakiOS en0 link & static IPv4 192.168.7.2/24 configuration -------------------------------- */

static struct hakios_net_link usb_ecm_link = {
	.name     = "MT6877 USB CDC-ECM",
	.unit     = 0,
	.wireless = false,
	.up       = false,
	.address  = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 },
	.transmit = ecm_tx_enqueue_mbuf,
	.poll     = NULL,
};

static void
configure_en0_ipv4(void)
{
	ifnet_t ifp = NULL;
	errno_t err = ifnet_find_by_name("en0", &ifp);
	if (err != 0 || ifp == NULL) {
		printf("mtk_usb_ecm: ifnet_find_by_name(en0) failed err=%d\n", err);
		return;
	}

	errno_t pe = proto_plumb(PF_INET, ifp);
	ifnet_set_flags(ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);

	socket_t so = NULL;
	errno_t se = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	errno_t ie = -1;
	if (se == 0 && so != NULL) {
		struct ifaliasreq ifra;
		bzero(&ifra, sizeof(ifra));
		strlcpy(ifra.ifra_name, "en0", sizeof(ifra.ifra_name));

		struct sockaddr_in *sa = (struct sockaddr_in *)&ifra.ifra_addr;
		sa->sin_len = sizeof(*sa);
		sa->sin_family = AF_INET;
		sa->sin_addr.s_addr = htonl((192u << 24) | (168u << 16) | (7u << 8) | 2u);

		struct sockaddr_in *sm = (struct sockaddr_in *)&ifra.ifra_mask;
		sm->sin_len = sizeof(*sm);
		sm->sin_family = AF_INET;
		sm->sin_addr.s_addr = htonl(0xffffff00u);

		struct sockaddr_in *sb = (struct sockaddr_in *)&ifra.ifra_broadaddr;
		sb->sin_len = sizeof(*sb);
		sb->sin_family = AF_INET;
		sb->sin_addr.s_addr = htonl((192u << 24) | (168u << 16) | (7u << 8) | 255u);

		ie = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	ifnet_release(ifp);
	printf("mtk_usb_ecm: en0 IPv4 192.168.7.2/24 ready (plumb=%d sock=%d ioctl=%d)\n", pe, se, ie);
}

/* ---- Entry points ------------------------------------------------------------------------------ */

void
hakios_mtk_usb_ecm_quiesce(void)
{
	if (!u.initialized || u.quiesced) {
		return;
	}
	u.quiesced = 1;
	if (u.irq_attached) {
		gic_v3_spi_detach(MTU3_IRQ_INTID);
		u.irq_attached = 0;
	}
	if (u.mac) {
		wr32(u.mac, LV1IECR_OFF, ~0u);
		if (u.tx_ready) {
			(void)qmu_stop_queue(u.mac, TQCSR1_OFF);
		}
		if (u.rx_ready) {
			(void)qmu_stop_queue(u.mac, RQCSR1_OFF);
		}
		if (u.phy) {
			mac_soft_connect(u.mac, u.phy, 0);
		}
	}
}

void
hakios_mtk_usb_ecm_init(void)
{
	uint32_t enabled = 0;
	if (!PE_parse_boot_argn("mtk_usb_ecm", &enabled, sizeof(enabled)) || enabled != 1) {
		return;
	}

	u.ifrao = ml_io_map(IFRAO_PA, IFRAO_SIZE);
	u.ippc  = ml_io_map(IPPC_PA,  IPPC_SIZE);
	u.mac   = ml_io_map(MTU3_PA,  MTU3_SIZE);
	u.phy   = ml_io_map(PHY_PA,   PHY_SIZE);
	u.gpio  = ml_io_map(GPIO_PA,  GPIO_SIZE);
	if (!u.ifrao || !u.ippc || !u.mac || !u.phy || !u.gpio) {
		printf("mtk_usb_ecm: ml_io_map failed\n");
		return;
	}

	uint32_t cg = rd32(u.ifrao, 0x00ac);
	if (cg & (1u << 1)) {
		printf("mtk_usb_ecm: SSUSB clock gated (ifrao+0xac=0x%08x); skip\n", cg);
		return;
	}

	u.txq_lock = lck_spin_alloc_init(&usb_lck_grp, LCK_ATTR_NULL);

	/* 1. IPPC -> U3/U2 PHY -> IDDIG -> PS5169 -> MAC2 (exact verified MT6877 order) */
	ippc_init(u.ippc);
	u3_phy_on(u.phy);
	u2_phy_on(u.phy);
	delay(1000);

	/* Force IDDIG=1 (device mode) and drop pull-up briefly for clean re-enumeration */
	wr32(u.phy, PHY_U2_DTM1, rd32(u.phy, PHY_U2_DTM1) | DTM1_FORCE_IDDIG | DTM1_RG_IDDIG);
	wr32(u.phy, PHY_U2_ACR3, rd32(u.phy, PHY_U2_ACR3) & ~(ACR3_PUPD_BIST_EN | ACR3_EN_PU_DP));

	ps5169_enable(u.gpio);

	/* MAC2 HS device mode configuration (0086 + 0091 + 0103) */
	wr32(u.mac, LV1IECR_OFF, ~0u);
	wr32(u.mac, USB3_CONFIG_OFF, rd32(u.mac, USB3_CONFIG_OFF) & ~1u);
	wr32(u.mac, MAC2_PM_CTRL_OFF,
	    (rd32(u.mac, MAC2_PM_CTRL_OFF) | (1u << 5)) & ~((1u << 6) | (1u << 0)));
	wr32(u.mac, DEV_CONF_OFF, 0);
	wr32(u.ippc, IPPC_U2_CTRL_0P, rd32(u.ippc, IPPC_U2_CTRL_0P) & ~U2_OTG_SEL);

	/* Clean detach window so host drops any bootloader USB state */
	ep0_quiet_reset(u.mac);
	delay(150000);

	/* Arm EP0 & enable MTU3 interrupts before soft-connect */
	uint32_t csr = (rd32(u.mac, EP0CSR_OFF) & EP0_W1C_BITS & ~EP0_MAXPKTSZ_MSK) | 64u;
	wr32(u.mac, EP0CSR_OFF, csr);
	wr32(u.mac, MAC2_INTR_STS_OFF, rd32(u.mac, MAC2_INTR_STS_OFF));
	wr32(u.mac, MAC2_INTR_EN_OFF, rd32(u.mac, MAC2_INTR_EN_OFF) | MAC2_INTR_RESET | MAC2_INTR_RESUME | MAC2_INTR_SUSPEND);
	wr32(u.mac, EPIESR_OFF, EPISR_EP0ISR | EPISR_SETUPEND);
	wr32(u.mac, LV1IESR_OFF, LV1_BMU_INTR | LV1_QMU_INTR | LV1_MAC2_INTR | LV1_EP0_INTR);

	/* 2. Attach BSD en0 + configure static 192.168.7.2/24 */
	memcpy(usb_ecm_link.address, ecm_phone_mac, ETHER_ADDR_LEN);
	if (hakios_net_attach(&usb_ecm_link)) {
		configure_en0_ipv4();
	}

	/* 3. Attach GICv3 INTID 160 & start worker thread, then soft-connect D+ */
	u.initialized = 1;
	u.attach_ms = now_ms();
	u.last_ev_ms = u.attach_ms;

	kern_return_t kr = gic_v3_spi_attach(MTU3_IRQ_INTID, false, mtk_usb_irq_handler, NULL);
	if (kr == KERN_SUCCESS) {
		u.irq_attached = 1;
	} else {
		printf("mtk_usb_ecm: gic_v3_spi_attach(%u) failed kr=%d (polling fallback)\n",
		    MTU3_IRQ_INTID, kr);
	}

	thread_t th = NULL;
	if (kernel_thread_start(mtk_usb_worker, NULL, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	}

	mac_soft_connect(u.mac, u.phy, 1);

	printf("mtk_usb_ecm: ready (mtu3=0x%lx ippc_sts=0x%x/0x%x irq=%u:%s VID=1209 PID=0002)\n",
	    MTU3_PA, rd32(u.ippc, IPPC_PW_STS1), rd32(u.ippc, IPPC_PW_STS2),
	    MTU3_IRQ_INTID, u.irq_attached ? "ok" : "poll");
}
