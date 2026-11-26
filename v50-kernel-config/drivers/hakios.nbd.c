/*
 * HakiOS Network Block Device (HNBD v1) over kernel TCP sockets.
 *
 * Exposes host-served disk images (e.g. APFS containers from tools/ruby_nbd.py)
 * as /dev/nbd0, /dev/rnbd0, /dev/nbd1, /dev/rnbd1 so the phone can mount and
 * execute from large APFS volumes over the ~38 MB/s USB CDC-ECM link with ZERO
 * writes to the phone's UFS flash.
 *
 * Boot arguments:
 *   nbd=1 | nbd=auto          enable /dev/nbd0..1, auto-connect nbd0 to
 *                             10.0.2.2:10809 (if virtio_net=1) or 192.168.7.1:10809
 *   nbd=<ip>[:<port>]         enable /dev/nbd0..1, auto-connect nbd0 to <ip>:<port>
 *
 * Sysctls:
 *   kern.hakios.nbd           (RO) status and I/O counters for nbd0..1
 *   kern.hakios.nbd_ctrl      (RW) "connect [unit] [ip] [port]",
 *                                  "disconnect [unit]",
 *                                  "auto [unit] [ip] [port]"
 */

#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/hakios.nbd.h>
#include <sys/hakios.net.h>
#include <sys/kpi_socket.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/uio.h>

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);

#define NBD_MAX_DEVS        4
#define NBD_SECTOR_SIZE     512u
#define NBD_MAX_XFER        (256u * 1024u)
#define NBD_DEFAULT_PORT    10809u

/* HNBD v1 wire protocol (all fields little-endian) */
#define HNBD_HELLO_MAGIC    0x44424e48u  /* "HNBD" */
#define HNBD_REQ_MAGIC      0x31514552u  /* "REQ1" */
#define HNBD_RSP_MAGIC      0x314c5052u  /* "RPL1" */
#define HNBD_VERSION        1u

#define HNBD_FLAG_RO        (1u << 0)

#define HNBD_OP_READ        1u
#define HNBD_OP_WRITE       2u
#define HNBD_OP_FLUSH       3u

struct hnbd_hello_req {
	uint32_t magic;
	uint16_t version;
	uint16_t unit;
	uint32_t flags;
	uint32_t reserved;
} __attribute__((packed));

struct hnbd_hello_rsp {
	uint32_t magic;
	uint32_t status;
	uint32_t flags;
	uint32_t block_size;
	uint64_t size_bytes;
} __attribute__((packed));

struct hnbd_req {
	uint32_t magic;
	uint32_t op;
	uint64_t offset;
	uint32_t length;
	uint32_t tag;
} __attribute__((packed));

struct hnbd_rsp {
	uint32_t magic;
	uint32_t status;
	uint32_t tag;
	uint32_t length;
} __attribute__((packed));

enum nbd_ctrl_op {
	NBD_CTRL_NONE = 0,
	NBD_CTRL_CONNECT,
	NBD_CTRL_DISCONNECT,
};

struct nbd_job {
	uint32_t        op;
	uint64_t        offset;
	void           *buf;
	uint32_t        len;
	int             error;
	bool            done;
	struct nbd_job *next;
};

struct nbd_unit {
	int             index;
	lck_mtx_t       lock;
	socket_t        so;             /* owned exclusively by nbd_worker */
	uint32_t        host_ip;        /* host byte order, 0 if unset */
	uint16_t        host_port;
	bool            auto_connect;
	bool            connected;
	bool            readonly;
	uint64_t        size_bytes;
	uint64_t        sectors;
	uint32_t        next_tag;

	/* statistics */
	uint64_t        reads;
	uint64_t        writes;
	uint64_t        flushes;
	uint64_t        read_bytes;
	uint64_t        write_bytes;
	uint64_t        errors;
	uint64_t        reconnects;
	int             last_err;

	/* worker queues */
	struct nbd_job *q_head;
	struct nbd_job *q_tail;

	enum nbd_ctrl_op ctrl_op;
	uint32_t        ctrl_ip;
	uint16_t        ctrl_port;
	bool            ctrl_auto;
	bool            ctrl_pending;
	int             ctrl_result;
	bool            ctrl_done;
};

static LCK_GRP_DECLARE(nbd_lck_grp, "hakios-nbd");
static struct nbd_unit nbd_units[NBD_MAX_DEVS];
static int nbd_bmajor = -1, nbd_cmajor = -1;
static bool nbd_enabled;
static volatile int nbd_quiesced;
static uint32_t nbd_default_ip;
static uint16_t nbd_default_port = NBD_DEFAULT_PORT;

SYSCTL_DECL(_kern_hakios);

bool
hakios_nbd_dev(dev_t dev)
{
	return (nbd_bmajor >= 0 && major(dev) == nbd_bmajor) ||
	       (nbd_cmajor >= 0 && major(dev) == nbd_cmajor);
}

void
hakios_nbd_quiesce(void)
{
	nbd_quiesced = 1;
}

static bool
parse_ipv4(const char *s, uint32_t *ip_out, uint16_t *port_out)
{
	uint32_t parts[4] = { 0, 0, 0, 0 };
	int part = 0;
	bool have_digit = false;
	const char *p = s;

	while (*p && *p != ':' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
		if (*p >= '0' && *p <= '9') {
			parts[part] = parts[part] * 10u + (uint32_t)(*p - '0');
			if (parts[part] > 255u) {
				return false;
			}
			have_digit = true;
		} else if (*p == '.') {
			if (!have_digit || part >= 3) {
				return false;
			}
			part++;
			have_digit = false;
		} else {
			return false;
		}
		p++;
	}
	if (part != 3 || !have_digit) {
		return false;
	}
	*ip_out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
	if (*p == ':' && port_out != NULL) {
		uint32_t port = 0;
		p++;
		if (*p < '0' || *p > '9') {
			return false;
		}
		while (*p >= '0' && *p <= '9') {
			port = port * 10u + (uint32_t)(*p - '0');
			if (port > 65535u) {
				return false;
			}
			p++;
		}
		if (port == 0) {
			return false;
		}
		*port_out = (uint16_t)port;
	}
	return true;
}

static void
format_ipv4(uint32_t ip, char *buf, size_t len)
{
	snprintf(buf, len, "%u.%u.%u.%u",
	    (ip >> 24) & 0xffu, (ip >> 16) & 0xffu, (ip >> 8) & 0xffu, ip & 0xffu);
}

/* ---- low-level socket I/O (called only from nbd_worker) ---- */

static int
nbd_send_all(socket_t so, const void *buf, size_t len)
{
	const uint8_t *p = (const uint8_t *)buf;
	size_t done = 0;

	while (done < len) {
		struct iovec iov;
		struct msghdr msg;
		size_t sent = 0;
		errno_t err;

		if (nbd_quiesced) {
			return ENXIO;
		}
		bzero(&msg, sizeof(msg));
		iov.iov_base = __DECONST(void *, p + done);
		iov.iov_len = len - done;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		err = sock_send(so, &msg, 0, &sent);
		if (err != 0) {
			return (int)err;
		}
		if (sent == 0) {
			return EPIPE;
		}
		done += sent;
	}
	return 0;
}

static int
nbd_recv_all(socket_t so, void *buf, size_t len)
{
	uint8_t *p = (uint8_t *)buf;
	size_t done = 0;

	while (done < len) {
		struct iovec iov;
		struct msghdr msg;
		size_t got = 0;
		errno_t err;

		if (nbd_quiesced) {
			return ENXIO;
		}
		bzero(&msg, sizeof(msg));
		iov.iov_base = p + done;
		iov.iov_len = len - done;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		err = sock_receive(so, &msg, MSG_WAITALL, &got);
		if (err != 0) {
			return (int)err;
		}
		if (got == 0) {
			return ECONNRESET;
		}
		done += got;
	}
	return 0;
}

static void
nbd_close_socket(struct nbd_unit *d)
{
	socket_t so = d->so;
	d->so = NULL;
	if (so != NULL) {
		(void)sock_shutdown(so, SHUT_RDWR);
		sock_close(so);
	}
}

static int
nbd_connect_worker(struct nbd_unit *d, bool verbose)
{
	socket_t so = NULL;
	struct sockaddr_in sin;
	struct timeval tv;
	struct hnbd_hello_req hreq;
	struct hnbd_hello_rsp hrsp;
	int one = 1;
	int bufsize = 256 * 1024;
	int err;
	char ipstr[24];
	uint32_t ip;
	uint16_t port;

	lck_mtx_lock(&d->lock);
	ip = d->host_ip;
	port = d->host_port ? d->host_port : NBD_DEFAULT_PORT;
	lck_mtx_unlock(&d->lock);

	if (ip == 0 || nbd_quiesced) {
		return ENXIO;
	}
	if (!hakios_net_is_up()) {
		lck_mtx_lock(&d->lock);
		d->connected = false;
		d->last_err = ENETDOWN;
		lck_mtx_unlock(&d->lock);
		return ENETDOWN;
	}

	nbd_close_socket(d);

	err = (int)sock_socket(PF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, NULL, &so);
	if (err != 0 || so == NULL) {
		goto fail;
	}

	(void)sock_nointerrupt(so, 1);
	(void)sock_setsockopt(so, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
	(void)sock_setsockopt(so, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));
	tv.tv_sec = 10;
	tv.tv_usec = 0;
	(void)sock_setsockopt(so, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)sock_setsockopt(so, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	bzero(&sin, sizeof(sin));
	sin.sin_len = sizeof(sin);
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = htonl(ip);

	err = (int)sock_connect(so, (const struct sockaddr *)&sin, MSG_DONTWAIT);
	if (err == EINPROGRESS) {
		struct timeval ctv = { .tv_sec = 2, .tv_usec = 0 };
		err = (int)sock_connectwait(so, &ctv);
	}
	if (err == 0) {
		int soerr = 0;
		int soerr_len = (int)sizeof(soerr);
		if (sock_getsockopt(so, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) == 0 && soerr != 0) {
			err = soerr;
		}
	}
	if (err != 0) {
		goto fail;
	}

	/* HNBD v1 handshake */
	bzero(&hreq, sizeof(hreq));
	hreq.magic = HNBD_HELLO_MAGIC;
	hreq.version = HNBD_VERSION;
	hreq.unit = (uint16_t)d->index;
	err = nbd_send_all(so, &hreq, sizeof(hreq));
	if (err != 0) {
		goto fail;
	}
	err = nbd_recv_all(so, &hrsp, sizeof(hrsp));
	if (err != 0) {
		goto fail;
	}
	if (hrsp.magic != HNBD_HELLO_MAGIC || hrsp.status != 0 ||
	    hrsp.block_size != NBD_SECTOR_SIZE || hrsp.size_bytes < NBD_SECTOR_SIZE ||
	    (hrsp.size_bytes % NBD_SECTOR_SIZE) != 0) {
		err = (hrsp.status != 0) ? (int)hrsp.status : EPROTO;
		goto fail;
	}

	d->so = so;
	lck_mtx_lock(&d->lock);
	d->connected = true;
	d->readonly = (hrsp.flags & HNBD_FLAG_RO) != 0;
	d->size_bytes = hrsp.size_bytes;
	d->sectors = hrsp.size_bytes / NBD_SECTOR_SIZE;
	d->last_err = 0;
	lck_mtx_unlock(&d->lock);

	format_ipv4(ip, ipstr, sizeof(ipstr));
	printf("nbd%d: connected %s:%u (%llu MiB, %llu sectors, %s)\n",
	    d->index, ipstr, (unsigned)port,
	    (unsigned long long)(hrsp.size_bytes >> 20),
	    (unsigned long long)(hrsp.size_bytes / NBD_SECTOR_SIZE),
	    (hrsp.flags & HNBD_FLAG_RO) ? "ro" : "rw");
	return 0;

fail:
	if (so != NULL) {
		sock_close(so);
	}
	lck_mtx_lock(&d->lock);
	int prev_err = d->last_err;
	d->connected = false;
	d->last_err = err;
	lck_mtx_unlock(&d->lock);
	if (verbose && err != prev_err) {
		format_ipv4(ip, ipstr, sizeof(ipstr));
		printf("nbd%d: connect %s:%u failed (err=%d)\n",
		    d->index, ipstr, (unsigned)port, err);
	}
	return err;
}

static int
nbd_exec_once(struct nbd_unit *d, struct nbd_job *job)
{
	struct hnbd_req req;
	struct hnbd_rsp rsp;
	uint32_t tag;
	int err;

	if (d->so == NULL) {
		return ENOTCONN;
	}
	tag = ++d->next_tag;
	req.magic = HNBD_REQ_MAGIC;
	req.op = job->op;
	req.offset = job->offset;
	req.length = job->len;
	req.tag = tag;

	err = nbd_send_all(d->so, &req, sizeof(req));
	if (err != 0) {
		return err;
	}
	if (job->op == HNBD_OP_WRITE && job->len > 0) {
		err = nbd_send_all(d->so, job->buf, job->len);
		if (err != 0) {
			return err;
		}
	}
	err = nbd_recv_all(d->so, &rsp, sizeof(rsp));
	if (err != 0) {
		return err;
	}
	if (rsp.magic != HNBD_RSP_MAGIC || rsp.tag != tag) {
		return EPROTO;
	}
	if (rsp.status != 0) {
		/* Protocol-level error from server (e.g. EROFS/EINVAL), connection still healthy */
		return (int)rsp.status;
	}
	if (job->op == HNBD_OP_READ) {
		if (rsp.length != job->len) {
			return EPROTO;
		}
		err = nbd_recv_all(d->so, job->buf, job->len);
		if (err != 0) {
			return err;
		}
	} else if (rsp.length != 0) {
		return EPROTO;
	}
	return 0;
}

static bool
nbd_is_transport_err(int err)
{
	return err == ENOTCONN || err == EPIPE || err == ECONNRESET ||
	       err == ETIMEDOUT || err == EAGAIN || err == EWOULDBLOCK ||
	       err == EPROTO || err == EHOSTUNREACH || err == ENETUNREACH;
}

static void
nbd_worker(void *arg, __unused wait_result_t wr)
{
	struct nbd_unit *d = (struct nbd_unit *)arg;

	while (!nbd_quiesced) {
		struct nbd_job *job = NULL;
		bool do_ctrl = false;
		enum nbd_ctrl_op cop = NBD_CTRL_NONE;
		uint32_t cip = 0;
		uint16_t cport = 0;
		bool cauto = false;
		bool try_auto = false;

		lck_mtx_lock(&d->lock);
		if (d->ctrl_pending) {
			do_ctrl = true;
			cop = d->ctrl_op;
			cip = d->ctrl_ip;
			cport = d->ctrl_port;
			cauto = d->ctrl_auto;
			d->ctrl_pending = false;
		} else if (d->q_head != NULL) {
			job = d->q_head;
			d->q_head = job->next;
			if (d->q_head == NULL) {
				d->q_tail = NULL;
			}
			job->next = NULL;
		} else if (d->auto_connect && !d->connected && d->host_ip != 0) {
			try_auto = true;
		} else {
			(void)lck_mtx_sleep(&d->lock, LCK_SLEEP_DEFAULT, (event_t)d, THREAD_UNINT);
		}
		lck_mtx_unlock(&d->lock);

		if (nbd_quiesced) {
			break;
		}

		if (do_ctrl) {
			int res = 0;
			if (cop == NBD_CTRL_DISCONNECT) {
				lck_mtx_lock(&d->lock);
				d->auto_connect = false;
				d->connected = false;
				d->size_bytes = 0;
				d->sectors = 0;
				lck_mtx_unlock(&d->lock);
				nbd_close_socket(d);
				printf("nbd%d: disconnected\n", d->index);
			} else if (cop == NBD_CTRL_CONNECT) {
				lck_mtx_lock(&d->lock);
				if (cip != 0) {
					d->host_ip = cip;
				}
				if (cport != 0) {
					d->host_port = cport;
				}
				d->auto_connect = cauto;
				lck_mtx_unlock(&d->lock);
				res = nbd_connect_worker(d, true);
			}
			lck_mtx_lock(&d->lock);
			d->ctrl_result = res;
			d->ctrl_done = true;
			thread_wakeup(&d->ctrl_done);
			lck_mtx_unlock(&d->lock);
			continue;
		}

		if (job != NULL) {
			int err = 0;
			if (d->so == NULL) {
				err = nbd_connect_worker(d, false);
			}
			if (err == 0) {
				err = nbd_exec_once(d, job);
				if (err != 0 && nbd_is_transport_err(err) && !nbd_quiesced) {
					nbd_close_socket(d);
					lck_mtx_lock(&d->lock);
					d->connected = false;
					d->reconnects++;
					lck_mtx_unlock(&d->lock);
					if (nbd_connect_worker(d, false) == 0) {
						err = nbd_exec_once(d, job);
					}
				}
			}
			lck_mtx_lock(&d->lock);
			if (err == 0) {
				if (job->op == HNBD_OP_READ) {
					d->reads++;
					d->read_bytes += job->len;
				} else if (job->op == HNBD_OP_WRITE) {
					d->writes++;
					d->write_bytes += job->len;
				} else if (job->op == HNBD_OP_FLUSH) {
					d->flushes++;
				}
			} else {
				d->errors++;
				d->last_err = err;
				if (nbd_is_transport_err(err)) {
					d->connected = false;
				}
			}
			job->error = err;
			job->done = true;
			thread_wakeup(job);
			lck_mtx_unlock(&d->lock);
			continue;
		}

		if (try_auto) {
			uint32_t wait_ms = 1000u;
			int cerr = ENETDOWN;
			if (!hakios_net_is_up()) {
				wait_ms = 250u;
			} else {
				cerr = nbd_connect_worker(d, false);
			}
			if (cerr != 0 && !nbd_quiesced) {
				uint64_t dl;
				clock_interval_to_deadline(wait_ms, NSEC_PER_MSEC, &dl);
				lck_mtx_lock(&d->lock);
				if (!d->ctrl_pending && d->q_head == NULL && !nbd_quiesced) {
					(void)lck_mtx_sleep_deadline(&d->lock, LCK_SLEEP_DEFAULT,
					    (event_t)d, THREAD_UNINT, dl);
				}
				lck_mtx_unlock(&d->lock);
			}
		}
	}

	nbd_close_socket(d);
}

static int
nbd_submit_job(struct nbd_unit *d, uint32_t op, uint64_t offset, void *buf, uint32_t len)
{
	struct nbd_job job;

	if (nbd_quiesced) {
		return ENXIO;
	}
	bzero(&job, sizeof(job));
	job.op = op;
	job.offset = offset;
	job.buf = buf;
	job.len = len;

	lck_mtx_lock(&d->lock);
	if (d->q_tail != NULL) {
		d->q_tail->next = &job;
	} else {
		d->q_head = &job;
	}
	d->q_tail = &job;
	thread_wakeup(d);
	while (!job.done && !nbd_quiesced) {
		(void)lck_mtx_sleep(&d->lock, LCK_SLEEP_DEFAULT, (event_t)&job, THREAD_UNINT);
	}
	lck_mtx_unlock(&d->lock);

	return job.done ? job.error : ENXIO;
}

static int
nbd_submit_ctrl(struct nbd_unit *d, enum nbd_ctrl_op op, uint32_t ip, uint16_t port, bool auto_conn)
{
	int res;

	if (nbd_quiesced) {
		return ENXIO;
	}
	lck_mtx_lock(&d->lock);
	while (d->ctrl_pending && !nbd_quiesced) {
		(void)lck_mtx_sleep(&d->lock, LCK_SLEEP_DEFAULT, (event_t)&d->ctrl_done, THREAD_UNINT);
	}
	if (nbd_quiesced) {
		lck_mtx_unlock(&d->lock);
		return ENXIO;
	}
	d->ctrl_op = op;
	d->ctrl_ip = ip;
	d->ctrl_port = port;
	d->ctrl_auto = auto_conn;
	d->ctrl_done = false;
	d->ctrl_pending = true;
	thread_wakeup(d);
	while (!d->ctrl_done && !nbd_quiesced) {
		(void)lck_mtx_sleep(&d->lock, LCK_SLEEP_DEFAULT, (event_t)&d->ctrl_done, THREAD_UNINT);
	}
	res = d->ctrl_done ? d->ctrl_result : ENXIO;
	lck_mtx_unlock(&d->lock);
	return res;
}

/* ---- BSD block/char device interface ---- */

static struct nbd_unit *
nbd_unit_for_dev(dev_t dev)
{
	int u = minor(dev);
	if (u < 0 || u >= NBD_MAX_DEVS) {
		return NULL;
	}
	return &nbd_units[u];
}

static int
nbd_dev_open(dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	struct nbd_unit *d = nbd_unit_for_dev(dev);
	bool conn, ro;
	uint32_t ip;
	uint16_t port;

	if (d == NULL || !nbd_enabled || nbd_quiesced) {
		return ENXIO;
	}
	lck_mtx_lock(&d->lock);
	conn = d->connected;
	ro = d->readonly;
	ip = d->host_ip;
	port = d->host_port;
	lck_mtx_unlock(&d->lock);

	if (!conn && ip != 0) {
		if (!hakios_net_is_up()) {
			return ENETDOWN;
		}
		if (nbd_submit_ctrl(d, NBD_CTRL_CONNECT, ip, port, d->auto_connect) == 0) {
			lck_mtx_lock(&d->lock);
			conn = d->connected;
			ro = d->readonly;
			lck_mtx_unlock(&d->lock);
		}
	}
	if (!conn) {
		return ENXIO;
	}
	if ((flags & FWRITE) && ro) {
		return EROFS;
	}
	return 0;
}

static int
nbd_dev_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
nbd_dev_strategy(buf_t bp)
{
	struct nbd_unit *d = nbd_unit_for_dev(buf_device(bp));
	bool is_read = (buf_flags(bp) & B_READ) != 0;
	uint64_t offset, total_bytes;
	size_t len;
	int error = 0;

	if (d == NULL || !nbd_enabled || nbd_quiesced) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}

	lck_mtx_lock(&d->lock);
	bool conn = d->connected;
	bool ro = d->readonly;
	total_bytes = d->sectors * (uint64_t)NBD_SECTOR_SIZE;
	lck_mtx_unlock(&d->lock);

	if (!conn || total_bytes == 0) {
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}
	if (!is_read && ro) {
		buf_seterror(bp, EROFS);
		buf_biodone(bp);
		return;
	}

	offset = (uint64_t)buf_blkno(bp) * (uint64_t)NBD_SECTOR_SIZE;
	len = buf_count(bp);
	if (offset >= total_bytes) {
		buf_seterror(bp, offset == total_bytes ? 0 : EINVAL);
		buf_setresid(bp, buf_count(bp));
		buf_biodone(bp);
		return;
	}
	if (offset + len > total_bytes) {
		len = (size_t)(total_bytes - offset);
	}

	caddr_t mapped = NULL;
	if ((error = buf_map(bp, &mapped)) != 0) {
		buf_seterror(bp, error);
		buf_biodone(bp);
		return;
	}

	uint8_t *ptr = (uint8_t *)mapped;
	size_t done = 0;
	while (done < len && error == 0) {
		uint32_t chunk = (uint32_t)MIN((size_t)NBD_MAX_XFER, len - done);
		error = nbd_submit_job(d, is_read ? HNBD_OP_READ : HNBD_OP_WRITE,
		    offset + done, ptr + done, chunk);
		if (error == 0) {
			done += chunk;
		}
	}
	buf_setresid(bp, (uint32_t)(buf_count(bp) - (error ? 0 : done)));
	buf_unmap(bp);
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
nbd_dev_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	struct nbd_unit *d = nbd_unit_for_dev(dev);

	if (d == NULL || !nbd_enabled) {
		return ENXIO;
	}

	lck_mtx_lock(&d->lock);
	bool conn = d->connected;
	bool ro = d->readonly;
	uint64_t sectors = d->sectors;
	lck_mtx_unlock(&d->lock);

	if (!conn) {
		return ENXIO;
	}

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = NBD_SECTOR_SIZE;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (*(uint32_t *)data != NBD_SECTOR_SIZE) {
			return EINVAL;
		}
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = 4096u;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = sectors;
		return 0;
	case DKIOCGETBASE:
		*(uint64_t *)data = 0;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = ro ? 0u : 1u;
		return 0;
	case DKIOCGETFEATURES:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = NBD_MAX_XFER / NBD_SECTOR_SIZE;
		return 0;
	case DKIOCGETMAXBYTECOUNTREAD:
	case DKIOCGETMAXBYTECOUNTWRITE:
	case DKIOCGETMAXSEGMENTBYTECOUNTREAD:
	case DKIOCGETMAXSEGMENTBYTECOUNTWRITE:
		*(uint64_t *)data = NBD_MAX_XFER;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*(uint64_t *)data = 64u;
		return 0;
	case DKIOCSYNCHRONIZE:
	case DKIOCSYNCHRONIZECACHE:
		if (ro) {
			return 0;
		}
		return nbd_submit_job(d, HNBD_OP_FLUSH, 0, NULL, 0);
	default:
		return ENOTTY;
	}
}

static int
nbd_dev_size(dev_t dev)
{
	struct nbd_unit *d = nbd_unit_for_dev(dev);
	if (d == NULL || !nbd_enabled || !d->connected) {
		return -1;
	}
	return (int)NBD_SECTOR_SIZE;
}

static int
nbd_dev_raw_io(dev_t dev, struct uio *uio, bool is_read)
{
	struct nbd_unit *d = nbd_unit_for_dev(dev);
	if (d == NULL || !nbd_enabled || nbd_quiesced) {
		return ENXIO;
	}
	lck_mtx_lock(&d->lock);
	bool conn = d->connected;
	bool ro = d->readonly;
	uint64_t total_bytes = d->sectors * (uint64_t)NBD_SECTOR_SIZE;
	lck_mtx_unlock(&d->lock);

	if (!conn || total_bytes == 0) {
		return ENXIO;
	}
	if (!is_read && ro) {
		return EROFS;
	}

	off_t off = uio_offset(uio);
	user_ssize_t resid = uio_resid(uio);
	if (off < 0 || resid < 0 || ((uint64_t)off % NBD_SECTOR_SIZE) != 0 ||
	    ((uint64_t)resid % NBD_SECTOR_SIZE) != 0) {
		return EINVAL;
	}
	if (resid == 0 || (uint64_t)off >= total_bytes) {
		return 0;
	}
	uint64_t todo = (uint64_t)resid;
	if ((uint64_t)off + todo > total_bytes) {
		todo = total_bytes - (uint64_t)off;
	}

	size_t buf_sz = (size_t)MIN(todo, (uint64_t)NBD_MAX_XFER);
	void *kbuf = kalloc_data(buf_sz, Z_WAITOK);
	if (kbuf == NULL) {
		return ENOMEM;
	}

	int err = 0;
	uint64_t cur = (uint64_t)off;
	while (todo > 0 && err == 0) {
		uint32_t chunk = (uint32_t)MIN(todo, (uint64_t)buf_sz);
		if (is_read) {
			err = nbd_submit_job(d, HNBD_OP_READ, cur, kbuf, chunk);
			if (err == 0) {
				err = uiomove((caddr_t)kbuf, (int)chunk, uio);
			}
		} else {
			err = uiomove((caddr_t)kbuf, (int)chunk, uio);
			if (err == 0) {
				err = nbd_submit_job(d, HNBD_OP_WRITE, cur, kbuf, chunk);
			}
		}
		cur += chunk;
		todo -= chunk;
	}
	kfree_data(kbuf, buf_sz);
	return err;
}

static int
nbd_dev_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return nbd_dev_raw_io(dev, uio, true);
}

static int
nbd_dev_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return nbd_dev_raw_io(dev, uio, false);
}

static const struct bdevsw nbd_bdevsw = {
	.d_open     = nbd_dev_open,
	.d_close    = nbd_dev_close,
	.d_strategy = nbd_dev_strategy,
	.d_ioctl    = nbd_dev_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = nbd_dev_size,
	.d_type     = D_DISK,
};

static const struct cdevsw nbd_cdevsw = {
	.d_open       = nbd_dev_open,
	.d_close      = nbd_dev_close,
	.d_read       = nbd_dev_read,
	.d_write      = nbd_dev_write,
	.d_ioctl      = nbd_dev_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

/* ---- sysctls: kern.hakios.nbd & kern.hakios.nbd_ctrl ---- */

static int
nbd_sysctl_status(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char out[1536];
	size_t off = 0;

	if (!nbd_enabled) {
		snprintf(out, sizeof(out), "nbd: disabled (pass boot-arg nbd=1)\n");
		return SYSCTL_OUT(req, out, strlen(out) + 1);
	}
	for (int i = 0; i < NBD_MAX_DEVS && off + 1 < sizeof(out); i++) {
		struct nbd_unit *d = &nbd_units[i];
		char ipstr[24];

		lck_mtx_lock(&d->lock);
		format_ipv4(d->host_ip, ipstr, sizeof(ipstr));
		int n = snprintf(out + off, sizeof(out) - off,
		    "nbd%d: %s host=%s:%u auto=%d size=%llu (%llu MiB) %s "
		    "rd=%llu(%lluB) wr=%llu(%lluB) fl=%llu err=%llu reconn=%llu last=%d\n",
		    i, d->connected ? "connected" : "disconnected",
		    ipstr, (unsigned)d->host_port, d->auto_connect ? 1 : 0,
		    (unsigned long long)d->size_bytes,
		    (unsigned long long)(d->size_bytes >> 20),
		    d->readonly ? "ro" : "rw",
		    (unsigned long long)d->reads, (unsigned long long)d->read_bytes,
		    (unsigned long long)d->writes, (unsigned long long)d->write_bytes,
		    (unsigned long long)d->flushes, (unsigned long long)d->errors,
		    (unsigned long long)d->reconnects, d->last_err);
		lck_mtx_unlock(&d->lock);
		if (n > 0) {
			off += (size_t)MIN((size_t)n, sizeof(out) - 1 - off);
		}
	}
	return SYSCTL_OUT(req, out, off + 1);
}

static int
nbd_sysctl_ctrl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	char cmd[128];
	int err;

	if (!nbd_enabled) {
		return ENXIO;
	}
	if (req->newptr == USER_ADDR_NULL) {
		const char *help = "connect [unit] [ip] [port] | auto [unit] [ip] [port] | disconnect [unit]\n";
		return SYSCTL_OUT(req, help, strlen(help) + 1);
	}
	if (req->newlen == 0 || req->newlen >= sizeof(cmd)) {
		return EINVAL;
	}
	bzero(cmd, sizeof(cmd));
	err = SYSCTL_IN(req, cmd, req->newlen);
	if (err != 0) {
		return err;
	}
	cmd[req->newlen] = '\0';

	/* Tokenize up to 4 whitespace-separated tokens */
	char *tok[4] = { NULL, NULL, NULL, NULL };
	int ntok = 0;
	char *p = cmd;
	while (*p && ntok < 4) {
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
			p++;
		}
		if (!*p) {
			break;
		}
		tok[ntok++] = p;
		while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
			p++;
		}
		if (*p) {
			*p++ = '\0';
		}
	}
	if (ntok < 1) {
		return EINVAL;
	}

	int unit = 0;
	if (ntok >= 2) {
		if (tok[1][0] < '0' || tok[1][0] > '9' || tok[1][1] != '\0') {
			return EINVAL;
		}
		unit = tok[1][0] - '0';
	}
	if (unit < 0 || unit >= NBD_MAX_DEVS) {
		return EINVAL;
	}
	struct nbd_unit *d = &nbd_units[unit];

	if (strcmp(tok[0], "disconnect") == 0) {
		return nbd_submit_ctrl(d, NBD_CTRL_DISCONNECT, 0, 0, false);
	}
	if (strcmp(tok[0], "connect") == 0 || strcmp(tok[0], "auto") == 0) {
		bool is_auto = strcmp(tok[0], "auto") == 0;
		uint32_t ip = d->host_ip ? d->host_ip : nbd_default_ip;
		uint16_t port = d->host_port ? d->host_port : nbd_default_port;
		if (ntok >= 3) {
			if (!parse_ipv4(tok[2], &ip, &port)) {
				return EINVAL;
			}
		}
		if (ntok >= 4) {
			uint32_t pv = 0;
			for (const char *q = tok[3]; *q; q++) {
				if (*q < '0' || *q > '9') {
					return EINVAL;
				}
				pv = pv * 10u + (uint32_t)(*q - '0');
				if (pv > 65535u) {
					return EINVAL;
				}
			}
			if (pv == 0) {
				return EINVAL;
			}
			port = (uint16_t)pv;
		}
		return nbd_submit_ctrl(d, NBD_CTRL_CONNECT, ip, port, is_auto);
	}
	return EINVAL;
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, nbd, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, nbd_sysctl_status, "A", "HakiOS NBD status");
SYSCTL_PROC(_kern_hakios, OID_AUTO, nbd_ctrl, CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, nbd_sysctl_ctrl, "A", "HakiOS NBD control");

/* ---- init ---- */

void
hakios_nbd_init(void)
{
	char arg[64];
	uint32_t vnet = 0;
	uint32_t target_ip = 0;
	uint16_t target_port = NBD_DEFAULT_PORT;
	char ipstr[24];

	bzero(arg, sizeof(arg));
	if (!PE_parse_boot_arg_str("nbd", arg, sizeof(arg)) || arg[0] == '\0' || strcmp(arg, "0") == 0) {
		return;
	}

	/* Default host IP: QEMU user-net gateway (10.0.2.2) when virtio_net=1, else USB ECM host (192.168.7.1) */
	if (PE_parse_boot_argn("virtio_net", &vnet, sizeof(vnet)) && vnet != 0) {
		nbd_default_ip = (10u << 24) | (0u << 16) | (2u << 8) | 2u;
	} else {
		nbd_default_ip = (192u << 24) | (168u << 16) | (7u << 8) | 1u;
	}

	if (strcmp(arg, "1") == 0 || strcmp(arg, "auto") == 0) {
		target_ip = nbd_default_ip;
	} else if (!parse_ipv4(arg, &target_ip, &target_port)) {
		printf("nbd: invalid boot-arg nbd=%s\n", arg);
		return;
	} else {
		nbd_default_ip = target_ip;
		nbd_default_port = target_port;
	}

	nbd_bmajor = bdevsw_add(-1, &nbd_bdevsw);
	if (nbd_bmajor < 0) {
		printf("nbd: bdevsw_add failed\n");
		return;
	}
	nbd_cmajor = cdevsw_add_with_bdev(-1, &nbd_cdevsw, nbd_bmajor);
	if (nbd_cmajor < 0) {
		printf("nbd: cdevsw_add_with_bdev failed\n");
		return;
	}

	nbd_enabled = true;
	for (int i = 0; i < NBD_MAX_DEVS; i++) {
		struct nbd_unit *d = &nbd_units[i];
		thread_t th = NULL;
		char bname[16], cname[16];

		bzero(d, sizeof(*d));
		d->index = i;
		d->host_ip = target_ip;
		d->host_port = target_port;
		d->auto_connect = (i == 0);
		d->readonly = true;
		lck_mtx_init(&d->lock, &nbd_lck_grp, LCK_ATTR_NULL);

		snprintf(bname, sizeof(bname), "nbd%d", i);
		snprintf(cname, sizeof(cname), "rnbd%d", i);
		if (devfs_make_node(makedev(nbd_bmajor, i), DEVFS_BLOCK, UID_ROOT,
		    GID_OPERATOR, 0600, "%s", bname) == NULL ||
		    devfs_make_node(makedev(nbd_cmajor, i), DEVFS_CHAR, UID_ROOT,
		    GID_OPERATOR, 0600, "%s", cname) == NULL) {
			printf("nbd: devfs_make_node failed for unit %d\n", i);
			continue;
		}
		if (kernel_thread_start(nbd_worker, d, &th) == KERN_SUCCESS && th != NULL) {
			thread_deallocate(th);
		}
	}

	format_ipv4(target_ip, ipstr, sizeof(ipstr));
	printf("nbd: ready (/dev/nbd0..%d, auto nbd0 -> %s:%u, max_xfer=%u KiB)\n",
	    NBD_MAX_DEVS - 1, ipstr, (unsigned)target_port, NBD_MAX_XFER / 1024u);
}
