/*-
 * Copyright (c) 2014-2016 Vincenzo Maffione
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * This file implements multiple network backends (null, tap, netmap, ...),
 * to be used by network frontends such as virtio-net and ptnet.
 * The API to access the backend (e.g. send/receive packets, negotiate
 * features) is exported by net_backends.h.
 */

#define WITH_VMNET
#define USE_MEVENT 0
#include <sys/cdefs.h>
#include <sys/uio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>		/* u_short etc */
#include <net/if.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
//#include <pthread_np.h>
#include <poll.h>
#include <assert.h>

#include "mevent.h"
#include "net_backends.h"

#ifdef WITH_VMNET
#include <xhyve.h>
#include <support/uuid.h>
#include <dispatch/dispatch.h>
#include <vmnet/vmnet.h>
#endif /* WITH_VMNET */

#include <support/linker_set.h>

/*
 * Each network backend registers a set of function pointers that are
 * used to implement the net backends API.
 * This might need to be exposed if we implement backends in separate files.
 */
struct net_backend {
	const char *name;	/* name of the backend */
	/*
	 * The init and cleanup functions are used internally,
	 * virtio-net should never use it.
	 */
	int (*init)(struct net_backend *be, const char *devname,
				net_backend_cb_t cb, void *param);
	void (*cleanup)(struct net_backend *be);


	/*
	 * Called to serve a guest transmit request. The scatter-gather
	 * vector provided by the caller has 'iovcnt' elements and contains
	 * the packet to send. 'len' is the length of whole packet in bytes.
	 */
	int (*send)(struct net_backend *be, struct iovec *iov,
			int iovcnt, uint32_t len, int more);

	/*
	 * Called to serve guest receive request. When the function
	 * returns a positive value, the scatter-gather vector
	 * provided by the caller (having 'iovcnt' elements in it) will
	 * contain a chunk of the received packet. The 'more' flag will
	 * be set if the returned chunk was the last one for the current
	 * packet, and 0 otherwise. The function returns the chunk size
	 * in bytes, or 0 if the backend doesn't have a new packet to
	 * receive.
	 * Note that it may be necessary to call this callback many
	 * times to receive a single packet, depending of how big is
	 * buffers you provide.
	 */
	int (*recv)(struct net_backend *be, struct iovec *iov, int iovcnt);

	/*
	 * Ask the backend for the virtio-net features it is able to
	 * support. Possible features are TSO, UFO and checksum offloading
	 * in both rx and tx direction and for both IPv4 and IPv6.
	 */
	uint64_t (*get_cap)(struct net_backend *be);

	/*
	 * Tell the backend to enable/disable the specified virtio-net
	 * features (capabilities).
	 */
	int (*set_cap)(struct net_backend *be, uint64_t features,
		       unsigned int vnet_hdr_len);

	struct pci_vtnet_softc *sc;
	int fd;
	unsigned int be_vnet_hdr_len;
	unsigned int fe_vnet_hdr_len;
	uint32_t _pad1; /* XXX */
	void *priv;	/* Pointer to backend-specific data. */
};

SET_DECLARE(net_backend_s, struct net_backend);

#define VNET_HDR_LEN	sizeof(struct virtio_net_rxhdr)

#define WPRINTF(params) printf params

/* the null backend */
static int
netbe_null_init(struct net_backend *be, const char *devname,
			net_backend_cb_t cb, void *param)
{
	(void)devname; (void)cb; (void)param;
	be->fd = -1;
	return 0;
}

static void
netbe_null_cleanup(struct net_backend *be)
{
	(void)be;
}

static uint64_t
netbe_null_get_cap(struct net_backend *be)
{
	(void)be;
	return 0;
}

static int
netbe_null_set_cap(struct net_backend *be, uint64_t features,
			unsigned vnet_hdr_len)
{
	(void)be; (void)features; (void)vnet_hdr_len;
	return 0;
}

static int
netbe_null_send(struct net_backend *be, struct iovec *iov,
	int iovcnt, uint32_t len, int more)
{
	(void)be; (void)iov; (void)iovcnt; (void)len; (void)more;
	return 0; /* pretend we send */
}

static int
netbe_null_recv(struct net_backend *be, struct iovec *iov, int iovcnt)
{
	(void)be; (void)iov; (void)iovcnt;
	fprintf(stderr, "netbe_null_recv called ?\n");
	return -1; /* never called, i believe */
}

static struct net_backend n_be = {
	.name = "null",
	.init = netbe_null_init,
	.cleanup = netbe_null_cleanup,
	.send = netbe_null_send,
	.recv = netbe_null_recv,
	.get_cap = netbe_null_get_cap,
	.set_cap = netbe_null_set_cap,
};

DATA_SET(net_backend_s, n_be);


/* the tap backend */

struct tap_priv {
	struct mevent *mevp; /* USE_MEVENT */
	pthread_t sthrd; /* !USE_MEVENT */
	net_backend_cb_t cb;
	void *cb_param;
};

static void
tap_cleanup(struct net_backend *be)
{
	struct tap_priv *priv = be->priv;

	if (be->priv) {
		mevent_delete(priv->mevp);
		free(be->priv);
		be->priv = NULL;
	}
	if (be->fd != -1) {
		close(be->fd);
		be->fd = -1;
	}
}

#if !USE_MEVENT
/* the thread that runs the callback */
static void *
pci_vtnet_tap_select_func(void *param)
{
        struct net_backend *be = param;
	struct tap_priv *priv = be->priv;
        fd_set rfd;

        pthread_setname_np("net:tap:rx");

        assert(be->fd != -1);

        FD_ZERO(&rfd);
        FD_SET(be->fd, &rfd);

        while (1) {
                if (select((be->fd + 1), &rfd, NULL, NULL, NULL) == -1) {
                        abort();
                }
		priv->cb(be->fd, EVF_READ, priv->cb_param);
        }

        return (NULL);
}

#endif /* !USE_MEVENT */

static int
tap_init(struct net_backend *be, const char *devname,
	 net_backend_cb_t cb, void *param)
{
	char tbuf[80];
	int fd;
	int opt = 1;
	struct tap_priv *priv;

	if (cb == NULL) {
		WPRINTF(("TAP backend requires non-NULL callback\n"));
		return -1;
	}

	priv = calloc(1, sizeof(struct tap_priv));
	if (priv == NULL) {
		WPRINTF(("tap_priv alloc failed\n"));
		return -1;
	}

	strcpy(tbuf, "/dev/");
	strlcat(tbuf, devname, sizeof(tbuf));

	fd = open(tbuf, O_RDWR);
	if (fd == -1) {
		WPRINTF(("open of tap device %s failed\n", tbuf));
		goto error;
	}

	/*
	 * Set non-blocking and register for read
	 * notifications with the event loop
	 */
	if (ioctl(fd, FIONBIO, &opt) < 0) {
		WPRINTF(("tap device O_NONBLOCK failed\n"));
		goto error;
	}

	be->fd = fd;
	be->priv = priv;
#if USE_MEVENT
	priv->mevp = mevent_add(fd, EVF_READ, cb, param);
	if (priv->mevp == NULL) {
		WPRINTF(("Could not register event\n"));
		goto error;
	}

#else /* !USE_MEVENT */
	priv->cb = cb;
	priv->cb_param = param;
	if (pthread_create(&priv->sthrd, NULL, pci_vtnet_tap_select_func, be)) {
		WPRINTF(("Could not create tap receive thread\n"));
		goto error;
	}
	/* the callback is run in the thread */
#endif /* !USE_MEVENT */


	return 0;

error:
	tap_cleanup(be);
	return -1;
}

/*
 * Called to send a buffer chain out to the tap device
 */
static int
tap_send(struct net_backend *be, struct iovec *iov, int iovcnt, uint32_t len,
	int more)
{
	static char pad[60]; /* all zero bytes */

	(void)more;
	/*
	 * If the length is < 60, pad out to that and add the
	 * extra zero'd segment to the iov. It is guaranteed that
	 * there is always an extra iov available by the caller.
	 */
	if (len < 60) {
		iov[iovcnt].iov_base = pad;
		iov[iovcnt].iov_len = (size_t)(60 - len);
		iovcnt++;
	}

	return (int)writev(be->fd, iov, iovcnt);
}

static int
tap_recv(struct net_backend *be, struct iovec *iov, int iovcnt)
{
	int ret;

	/* Should never be called without a valid tap fd */
	assert(be->fd != -1);

	ret = (int)readv(be->fd, iov, iovcnt);

	if (ret < 0 && errno == EWOULDBLOCK) {
		return 0;
	}

	return ret;
}

static uint64_t
tap_get_cap(struct net_backend *be)
{
	(void)be;
	return 0; // nothing extra
}

static int
tap_set_cap(struct net_backend *be, uint64_t features,
		 unsigned vnet_hdr_len)
{
	(void)be;
	return (features || vnet_hdr_len) ? -1 : 0;
}

static struct net_backend tap_backend = {
	.name = "tap|vmmnet",
	.init = tap_init,
	.cleanup = tap_cleanup,
	.send = tap_send,
	.recv = tap_recv,
	.get_cap = tap_get_cap,
	.set_cap = tap_set_cap,
};

DATA_SET(net_backend_s, tap_backend);

#ifdef WITH_NETMAP

/*
 * The netmap backend
 */

/* The virtio-net features supported by netmap. */
#define NETMAP_FEATURES (VIRTIO_NET_F_CSUM | VIRTIO_NET_F_HOST_TSO4 | \
		VIRTIO_NET_F_HOST_TSO6 | VIRTIO_NET_F_HOST_UFO | \
		VIRTIO_NET_F_GUEST_CSUM | VIRTIO_NET_F_GUEST_TSO4 | \
		VIRTIO_NET_F_GUEST_TSO6 | VIRTIO_NET_F_GUEST_UFO)

#define NETMAP_POLLMASK (POLLIN | POLLRDNORM | POLLRDBAND)

struct netmap_priv {
	char ifname[IFNAMSIZ];
	struct nm_desc *nmd;
	uint16_t memid;
	struct netmap_ring *rx;
	struct netmap_ring *tx;
	pthread_t evloop_tid;
	net_backend_cb_t cb;
	void *cb_param;

	struct ptnetmap_state ptnetmap;
};

static void *
netmap_evloop_thread(void *param)
{
	struct net_backend *be = param;
	struct netmap_priv *priv = be->priv;
	struct pollfd pfd;
	int ret;

	for (;;) {
		pfd.fd = be->fd;
		pfd.events = NETMAP_POLLMASK;
		ret = poll(&pfd, 1, INFTIM);
		if (ret == -1 && errno != EINTR) {
			WPRINTF(("netmap poll failed, %d\n", errno));
		} else if (ret == 1 && (pfd.revents & NETMAP_POLLMASK)) {
			priv->cb(pfd.fd, EVF_READ, priv->cb_param);
		}
	}

	return NULL;
}

static void
nmreq_init(struct nmreq *req, char *ifname)
{
	memset(req, 0, sizeof(*req));
	strncpy(req->nr_name, ifname, sizeof(req->nr_name));
	req->nr_version = NETMAP_API;
}

static int
netmap_set_vnet_hdr_len(struct net_backend *be, int vnet_hdr_len)
{
	int err;
	struct nmreq req;
	struct netmap_priv *priv = be->priv;

	nmreq_init(&req, priv->ifname);
	req.nr_cmd = NETMAP_BDG_VNET_HDR;
	req.nr_arg1 = vnet_hdr_len;
	err = ioctl(be->fd, NIOCREGIF, &req);
	if (err) {
		WPRINTF(("Unable to set vnet header length %d\n",
				vnet_hdr_len));
		return err;
	}

	be->be_vnet_hdr_len = vnet_hdr_len;

	return 0;
}

static int
netmap_has_vnet_hdr_len(struct net_backend *be, unsigned vnet_hdr_len)
{
	int prev_hdr_len = be->be_vnet_hdr_len;
	int ret;

	if (vnet_hdr_len == prev_hdr_len) {
		return 1;
	}

	ret = netmap_set_vnet_hdr_len(be, vnet_hdr_len);
	if (ret) {
		return 0;
	}

	netmap_set_vnet_hdr_len(be, prev_hdr_len);

	return 1;
}

static uint64_t
netmap_get_cap(struct net_backend *be)
{
	return netmap_has_vnet_hdr_len(be, VNET_HDR_LEN) ?
			NETMAP_FEATURES : 0;
}

static int
netmap_set_cap(struct net_backend *be, uint64_t features,
	       unsigned vnet_hdr_len)
{
	return netmap_set_vnet_hdr_len(be, vnet_hdr_len);
}

/* Store and return the features we agreed upon. */
uint32_t
ptnetmap_ack_features(struct ptnetmap_state *ptn, uint32_t wanted_features)
{
	ptn->acked_features = ptn->features & wanted_features;

	return ptn->acked_features;
}

struct ptnetmap_state *
get_ptnetmap(struct net_backend *be)
{
	struct netmap_priv *priv = be ? be->priv : NULL;
	struct netmap_pools_info pi;
	struct nmreq req;
	int err;

	/* Check that this is a ptnetmap backend. */
	if (!be || be->set_cap != netmap_set_cap ||
			!(priv->nmd->req.nr_flags & NR_PTNETMAP_HOST)) {
		return NULL;
	}

	nmreq_init(&req, priv->ifname);
	req.nr_cmd = NETMAP_POOLS_INFO_GET;
	nmreq_pointer_put(&req, &pi);
	err = ioctl(priv->nmd->fd, NIOCREGIF, &req);
	if (err) {
		return NULL;
	}

	err = ptn_memdev_attach(priv->nmd->mem, &pi);
	if (err) {
		return NULL;
	}

	return &priv->ptnetmap;
}

int
ptnetmap_get_netmap_if(struct ptnetmap_state *ptn, struct netmap_if_info *nif)
{
	struct netmap_priv *priv = ptn->netmap_priv;

	memset(nif, 0, sizeof(*nif));
	if (priv->nmd == NULL) {
		return EINVAL;
	}

	nif->nifp_offset = priv->nmd->req.nr_offset;
	nif->num_tx_rings = priv->nmd->req.nr_tx_rings;
	nif->num_rx_rings = priv->nmd->req.nr_rx_rings;
	nif->num_tx_slots = priv->nmd->req.nr_tx_slots;
	nif->num_rx_slots = priv->nmd->req.nr_rx_slots;

	return 0;
}

int
ptnetmap_get_hostmemid(struct ptnetmap_state *ptn)
{
	struct netmap_priv *priv = ptn->netmap_priv;

	if (priv->nmd == NULL) {
		return EINVAL;
	}

	return priv->memid;
}

int
ptnetmap_create(struct ptnetmap_state *ptn, struct ptnetmap_cfg *cfg)
{
	struct netmap_priv *priv = ptn->netmap_priv;
	struct nmreq req;
	int err;

	if (ptn->running) {
		return 0;
	}

	/* XXX We should stop the netmap evloop here. */

	/* Ask netmap to create kthreads for this interface. */
	nmreq_init(&req, priv->ifname);
	nmreq_pointer_put(&req, cfg);
	req.nr_cmd = NETMAP_PT_HOST_CREATE;
	err = ioctl(priv->nmd->fd, NIOCREGIF, &req);
	if (err) {
		fprintf(stderr, "%s: Unable to create ptnetmap kthreads on "
			"%s [errno=%d]", __func__, priv->ifname, errno);
		return err;
	}

	ptn->running = 1;

	return 0;
}

int
ptnetmap_delete(struct ptnetmap_state *ptn)
{
	struct netmap_priv *priv = ptn->netmap_priv;
	struct nmreq req;
	int err;

	if (!ptn->running) {
		return 0;
	}

	/* Ask netmap to delete kthreads for this interface. */
	nmreq_init(&req, priv->ifname);
	req.nr_cmd = NETMAP_PT_HOST_DELETE;
	err = ioctl(priv->nmd->fd, NIOCREGIF, &req);
	if (err) {
		fprintf(stderr, "%s: Unable to create ptnetmap kthreads on "
			"%s [errno=%d]", __func__, priv->ifname, errno);
		return err;
	}

	ptn->running = 0;

	return 0;
}

static int
netmap_init(struct net_backend *be, const char *devname,
	    net_backend_cb_t cb, void *param)
{
	const char *ndname = "/dev/netmap";
	struct netmap_priv *priv = NULL;
	struct nmreq req;
	int ptnetmap = (cb == NULL);

	priv = calloc(1, sizeof(struct netmap_priv));
	if (priv == NULL) {
		WPRINTF(("Unable alloc netmap private data\n"));
		return -1;
	}

	strncpy(priv->ifname, devname, sizeof(priv->ifname));
	priv->ifname[sizeof(priv->ifname) - 1] = '\0';

	memset(&req, 0, sizeof(req));
	req.nr_flags = ptnetmap ? NR_PTNETMAP_HOST : 0;

	priv->nmd = nm_open(priv->ifname, &req, NETMAP_NO_TX_POLL, NULL);
	if (priv->nmd == NULL) {
		WPRINTF(("Unable to nm_open(): device '%s', "
				"interface '%s', errno (%s)\n",
				ndname, devname, strerror(errno)));
		free(priv);
		return -1;
	}

	priv->memid = priv->nmd->req.nr_arg2;
	priv->tx = NETMAP_TXRING(priv->nmd->nifp, 0);
	priv->rx = NETMAP_RXRING(priv->nmd->nifp, 0);
	priv->cb = cb;
	priv->cb_param = param;
	be->fd = priv->nmd->fd;
	be->priv = priv;

	priv->ptnetmap.netmap_priv = priv;
	priv->ptnetmap.features = 0;
	priv->ptnetmap.acked_features = 0;
	priv->ptnetmap.running = 0;
	if (ptnetmap) {
		if (netmap_has_vnet_hdr_len(be, VNET_HDR_LEN)) {
			priv->ptnetmap.features |= PTNETMAP_F_VNET_HDR;
		}
	} else {
		char tname[40];

		/* Create a thread for netmap poll. */
		pthread_create(&priv->evloop_tid, NULL, netmap_evloop_thread, (void *)be);
		snprintf(tname, sizeof(tname), "netmap-evloop-%p", priv);
		pthread_set_name_np(priv->evloop_tid, tname);
	}

	return 0;
}

static void
netmap_cleanup(struct net_backend *be)
{
	struct netmap_priv *priv = be->priv;

	if (be->priv) {
		if (priv->ptnetmap.running) {
			ptnetmap_delete(&priv->ptnetmap);
		}
		nm_close(priv->nmd);
		free(be->priv);
		be->priv = NULL;
	}
	be->fd = -1;
}

/* A fast copy routine only for multiples of 64 bytes, non overlapped. */
static inline void
pkt_copy(const void *_src, void *_dst, int l)
{
    const uint64_t *src = _src;
    uint64_t *dst = _dst;
    if (l >= 1024) {
        bcopy(src, dst, l);
        return;
    }
    for (; l > 0; l -= 64) {
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
        *dst++ = *src++;
    }
}

static int
netmap_send(struct net_backend *be, struct iovec *iov,
	    int iovcnt, uint32_t size, int more)
{
	struct netmap_priv *priv = be->priv;
	struct netmap_ring *ring;
	int nm_buf_size;
	int nm_buf_len;
	uint32_t head;
	void *nm_buf;
	int j;

	if (iovcnt <= 0 || size <= 0) {
		D("Wrong iov: iovcnt %d size %d", iovcnt, size);
		return 0;
	}

	ring = priv->tx;
	head = ring->head;
	if (head == ring->tail) {
		RD(1, "No space, drop %d bytes", size);
		goto txsync;
	}
	nm_buf = NETMAP_BUF(ring, ring->slot[head].buf_idx);
	nm_buf_size = ring->nr_buf_size;
	nm_buf_len = 0;

	for (j = 0; j < iovcnt; j++) {
		int iov_frag_size = iov[j].iov_len;
		void *iov_frag_buf = iov[j].iov_base;

		/* Split each iovec fragment over more netmap slots, if
		   necessary. */
		for (;;) {
			int copylen;

			copylen = iov_frag_size < nm_buf_size ? iov_frag_size : nm_buf_size;
			pkt_copy(iov_frag_buf, nm_buf, copylen);

			iov_frag_buf += copylen;
			iov_frag_size -= copylen;
			nm_buf += copylen;
			nm_buf_size -= copylen;
			nm_buf_len += copylen;

			if (iov_frag_size == 0) {
				break;
			}

			ring->slot[head].len = nm_buf_len;
			ring->slot[head].flags = NS_MOREFRAG;
			head = nm_ring_next(ring, head);
			if (head == ring->tail) {
				/* We ran out of netmap slots while
				 * splitting the iovec fragments. */
				RD(1, "No space, drop %d bytes", size);
				goto txsync;
			}
			nm_buf = NETMAP_BUF(ring, ring->slot[head].buf_idx);
			nm_buf_size = ring->nr_buf_size;
			nm_buf_len = 0;
		}
	}

	/* Complete the last slot, which must not have NS_MOREFRAG set. */
	ring->slot[head].len = nm_buf_len;
	ring->slot[head].flags = 0;
	head = nm_ring_next(ring, head);

	/* Now update ring->head and ring->cur. */
	ring->head = ring->cur = head;

	if (more) {// && nm_ring_space(ring) > 64
		return 0;
	}
txsync:
	ioctl(be->fd, NIOCTXSYNC, NULL);

	return 0;
}

static int
netmap_recv(struct net_backend *be, struct iovec *iov, int iovcnt)
{
	struct netmap_priv *priv = be->priv;
	struct netmap_slot *slot = NULL;
	struct netmap_ring *ring;
	void *iov_frag_buf;
	int iov_frag_size;
	int totlen = 0;
	uint32_t head;

	assert(iovcnt);

	ring = priv->rx;
	head = ring->head;
	iov_frag_buf = iov->iov_base;
	iov_frag_size = iov->iov_len;

	do {
		int nm_buf_len;
		void *nm_buf;

		if (head == ring->tail) {
			return 0;
		}

		slot = ring->slot + head;
		nm_buf = NETMAP_BUF(ring, slot->buf_idx);
		nm_buf_len = slot->len;

		for (;;) {
			int copylen = nm_buf_len < iov_frag_size ? nm_buf_len : iov_frag_size;

			pkt_copy(nm_buf, iov_frag_buf, copylen);
			nm_buf += copylen;
			nm_buf_len -= copylen;
			iov_frag_buf += copylen;
			iov_frag_size -= copylen;
			totlen += copylen;

			if (nm_buf_len == 0) {
				break;
			}

			iov++;
			iovcnt--;
			if (iovcnt == 0) {
				/* No space to receive. */
				D("Short iov, drop %d bytes", totlen);
				return -ENOSPC;
			}
			iov_frag_buf = iov->iov_base;
			iov_frag_size = iov->iov_len;
		}

		head = nm_ring_next(ring, head);

	} while (slot->flags & NS_MOREFRAG);

	/* Release slots to netmap. */
	ring->head = ring->cur = head;

	return totlen;
}

static struct net_backend netmap_backend = {
	.name = "netmap|vale",
	.init = netmap_init,
	.cleanup = netmap_cleanup,
	.send = netmap_send,
	.recv = netmap_recv,
	.get_cap = netmap_get_cap,
	.set_cap = netmap_set_cap,
};

DATA_SET(net_backend_s, netmap_backend);

#endif /* WITH_NETMAP */

#ifdef WITH_VMNET


/* the vmnet backend (ocaml, vmnet) */

struct vmnet_state {
	interface_ref iface;
	unsigned int mtu;
	unsigned int max_packet_size;
	uint8_t mac[6];
	uint8_t pad[2];
};

/*
 * Create an interface for the guest using Apple's vmnet framework.
 *
 * The interface works in VMNET_SHARED_MODE which allows for packets
 * of the guest to reach other guests and the Internet.
 *
 * See also: https://developer.apple.com/library/mac/documentation/vmnet/Reference/vmnet_Reference/index.html
 */
static int
vmnet_init(struct net_backend *be, const char *devname,
         net_backend_cb_t cb, void *param)
{
	//struct pci_vtnet_softc *sc;
	xpc_object_t interface_desc;
	uuid_t uuid;
	__block interface_ref iface;
	__block vmnet_return_t iface_status;
	dispatch_semaphore_t iface_created;
	dispatch_queue_t if_create_q;
	dispatch_queue_t if_q;
	struct vmnet_state *vms;
	uint32_t uuid_status;

	(void)devname;
	interface_desc = xpc_dictionary_create(NULL, NULL, 0);
	xpc_dictionary_set_uint64(interface_desc, vmnet_operation_mode_key,
		VMNET_SHARED_MODE);

	if (guest_uuid_str != NULL) {
		uuid_from_string(guest_uuid_str, &uuid, &uuid_status);
		if (uuid_status != uuid_s_ok) {
			return (-1);
		}
	} else {
		uuid_generate_random(uuid);
	}

	xpc_dictionary_set_uuid(interface_desc, vmnet_interface_id_key, uuid);
	iface = NULL;
	iface_status = 0;

	vms = malloc(sizeof(struct vmnet_state));

	if (!vms) {
	return (-1);
	}

	if_create_q = dispatch_queue_create("org.xhyve.vmnet.create",
		DISPATCH_QUEUE_SERIAL);

	iface_created = dispatch_semaphore_create(0);

	iface = vmnet_start_interface(interface_desc, if_create_q,
		^(vmnet_return_t status, xpc_object_t interface_param)
		{
			iface_status = status;

			if (status != VMNET_SUCCESS || !interface_param) {
				dispatch_semaphore_signal(iface_created);
				return;
			}

			/* XXX should it come from the command line ? */
			if (sscanf(xpc_dictionary_get_string(interface_param,
					vmnet_mac_address_key),
					"%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
					&vms->mac[0], &vms->mac[1], &vms->mac[2], &vms->mac[3],
					&vms->mac[4], &vms->mac[5]) != 6) {
				assert(0);
			}

			vms->mtu = (unsigned)
				xpc_dictionary_get_uint64(interface_param, vmnet_mtu_key);
			vms->max_packet_size = (unsigned)
				xpc_dictionary_get_uint64(interface_param,
					vmnet_max_packet_size_key);
			dispatch_semaphore_signal(iface_created);
		});

	dispatch_semaphore_wait(iface_created, DISPATCH_TIME_FOREVER);
	dispatch_release(if_create_q);

	if (iface == NULL || iface_status != VMNET_SUCCESS) {
		printf("virtio_net: Could not create vmnet interface, "
			"permission denied or no entitlement?\n");
		free(vms);
		return (-1);
	}

	vms->iface = iface;
	be->priv = vms;

	if_q = dispatch_queue_create("org.xhyve.vmnet.iface_q", 0);

	vmnet_interface_set_event_callback(iface, VMNET_INTERFACE_PACKETS_AVAILABLE,
		if_q, ^(UNUSED interface_event_t event_id, UNUSED xpc_object_t event)
		{
			cb(be->fd, EVF_READ, param);
		});

	return (0);
}

static int
vmnet_recv(struct net_backend *be, struct iovec *iov, int iovcnt)
{
	struct vmnet_state *vms = be->priv;
        vmnet_return_t r;
        struct vmpktdesc v;
        int pktcnt;
        int i;

        v.vm_pkt_size = 0;

        for (i = 0; i < iovcnt; i++) {
                v.vm_pkt_size += iov[i].iov_len;
        }

        assert(v.vm_pkt_size >= vms->max_packet_size);

        v.vm_pkt_iov = iov;
        v.vm_pkt_iovcnt = (uint32_t)iovcnt;
        v.vm_flags = 0; /* TODO no clue what this is */

        pktcnt = 1;

        r = vmnet_read(vms->iface, &v, &pktcnt);

        assert(r == VMNET_SUCCESS);

        if (pktcnt < 1) {
#if 0
		if (errno == EWOULDBLOCK)
			return 0;
#endif
                return (-1);
        }

        return ((int)v.vm_pkt_size);
}

/*
 * Called to send a buffer chain out to the tap device
 */
static int
vmnet_send(struct net_backend *be, struct iovec *iov, int iovcnt, uint32_t len,
	int more)
{
	static char pad[60]; /* all zero bytes */
        struct vmnet_state *vms = be->priv;
	vmnet_return_t r;
	struct vmpktdesc v;
	int pktcnt;

	(void)more;
	/*
	 * If the length is < 60, pad out to that and add the
	 * extra zero'd segment to the iov. It is guaranteed that
	 * there is always an extra iov available by the caller.
	 */
	if (len < 60) {
		iov[iovcnt].iov_base = pad;
		iov[iovcnt].iov_len = (size_t)(60 - len);
		iovcnt++;
	}

	assert(len <= vms->max_packet_size);

	v.vm_pkt_size = len;
	v.vm_pkt_iov = iov;
	v.vm_pkt_iovcnt = (uint32_t)iovcnt;
	v.vm_flags = 0; /* TODO no clue what this is */

	pktcnt = 1;

	r = vmnet_write(vms->iface, &v, &pktcnt);

	assert(r == VMNET_SUCCESS);
	return (int)len; /* success */
}

static struct net_backend vmnet_backend = {
	.name = "vmnet",
	.init = vmnet_init,
	.cleanup = NULL, // XXX vmnet_cleanup,
	.send = vmnet_send,
	.recv = vmnet_recv,
	.get_cap = netbe_get_cap,
	.set_cap = netbe_set_cap,
};

DATA_SET(net_backend_s, vmnet_backend);
#endif /* WITH_VMNET */

/*
 * make sure a backend is properly initialized
 */
static void
netbe_fix(struct net_backend *be)
{
	if (be == NULL)
		return;
	if (be->name == NULL) {
		fprintf(stderr, "missing name for %p\n", be);
		be->name = "unnamed netbe";
	}
	if (be->init == NULL) {
		fprintf(stderr, "missing init for %p %s\n", be, be->name);
		be->init = netbe_null_init;
	}
	if (be->cleanup == NULL) {
		fprintf(stderr, "missing cleanup for %p %s\n", be, be->name);
		be->cleanup = netbe_null_cleanup;
	}
	if (be->send == NULL) {
		fprintf(stderr, "missing send for %p %s\n", be, be->name);
		be->send = netbe_null_send;
	}
	if (be->recv == NULL) {
		fprintf(stderr, "missing recv for %p %s\n", be, be->name);
		be->recv = netbe_null_recv;
	}
	if (be->get_cap == NULL) {
		fprintf(stderr, "missing get_cap for %p %s\n",
			be, be->name);
		be->get_cap = netbe_null_get_cap;
	}
	if (be->set_cap == NULL) {
		fprintf(stderr, "missing set_cap for %p %s\n",
			be, be->name);
		be->set_cap = netbe_null_set_cap;
	}
}

/*
 * keys is a set of prefixes separated by '|',
 * return 1 if the leftmost part of name matches one prefix.
 */
static const char *
netbe_name_match(const char *keys, const char *name)
{
	const char *n = name, *good = keys;
	char c;

	if (!keys || !name)
		return NULL;
	while ( (c = *keys++) ) {
		if (c == '|') { /* reached the separator */
			if (good)
				break;
			/* prepare for new round */
			n = name;
			good = keys;
		} else if (good && c != *n++) {
			good = NULL; /* drop till next keyword */
		}
	}
	return good;
}

/*
 * Initialize a backend and attach to the frontend.
 * This is called during frontend initialization.
 * devname is the backend-name as supplied on the command line,
 * 	e.g. -s 2:0,frontend-name,backend-name[,other-args]
 * cb is the receive callback supplied by the frontend,
 *	and it is invoked in the event loop when a receive
 *	event is generated in the hypervisor,
 * param is a pointer to the frontend, and normally used as
 *	the argument for the callback.
 */
struct net_backend *
netbe_init(const char *devname, net_backend_cb_t cb, void *param)
{
	struct net_backend **pbe, *be, *tbe = NULL;
	int err;

	/*
	 * Find the network backend depending on the user-provided
	 * device name. net_backend_s is built using a linker set.
	 */
	SET_FOREACH(pbe, net_backend_s) {
		if (netbe_name_match((*pbe)->name, devname)) {
			tbe = *pbe;
			break;
		}
	}
	if (tbe == NULL)
		return NULL; /* or null backend ? */
	be = calloc(1, sizeof(*be));
	*be = *tbe;	/* copy the template */
	netbe_fix(be); /* make sure we have all fields */
	be->fd = -1;
	be->priv = NULL;
	be->sc = param;
	be->be_vnet_hdr_len = 0;
	be->fe_vnet_hdr_len = 0;

	/* initialize the backend */
	err = be->init(be, devname, cb, param);
	if (err) {
		free(be);
		be = NULL;
	}
	return be;
}

void
netbe_cleanup(struct net_backend *be)
{
	if (be == NULL)
		return;
	be->cleanup(be);
	free(be);
}

uint64_t
netbe_get_cap(struct net_backend *be)
{
	if (be == NULL)
		return 0;
	return be->get_cap(be);
}

int
netbe_set_cap(struct net_backend *be, uint64_t features,
	      unsigned vnet_hdr_len)
{
	int ret;

	if (be == NULL)
		return 0;

	/* There are only three valid lengths. */
	if (vnet_hdr_len && vnet_hdr_len != VNET_HDR_LEN
		&& vnet_hdr_len != (VNET_HDR_LEN - sizeof(uint16_t)))
		return -1;

	be->fe_vnet_hdr_len = vnet_hdr_len;

	ret = be->set_cap(be, features, vnet_hdr_len);
	assert(be->be_vnet_hdr_len == 0 ||
	       be->be_vnet_hdr_len == be->fe_vnet_hdr_len);

	return ret;
}

static __inline struct iovec *
iov_trim(struct iovec *iov, int *iovcnt, unsigned int tlen)
{
	struct iovec *riov;

	/* XXX short-cut: assume first segment is >= tlen */
	assert(iov[0].iov_len >= tlen);

	iov[0].iov_len -= tlen;
	if (iov[0].iov_len == 0) {
		assert(*iovcnt > 1);
		*iovcnt -= 1;
		riov = &iov[1];
	} else {
		iov[0].iov_base = (void *)((uintptr_t)iov[0].iov_base + tlen);
		riov = &iov[0];
	}

	return (riov);
}

void
netbe_send(struct net_backend *be, struct iovec *iov, int iovcnt, uint32_t len,
	   int more)
{
	if (be == NULL)
		return;
#if 0
	int i;
	D("sending iovcnt %d len %d iovec %p", iovcnt, len, iov);
	for (i=0; i < iovcnt; i++)
		D("   %3d: %4d %p", i, (int)iov[i].iov_len, iov[i].iov_base);
#endif
	if (be->be_vnet_hdr_len != be->fe_vnet_hdr_len) {
		/* Here we are sure be->be_vnet_hdr_len is 0. */
		iov = iov_trim(iov, &iovcnt, be->fe_vnet_hdr_len);
	}

	be->send(be, iov, iovcnt, len, more);
}

/*
 * can return -1 in case of errors
 */
int
netbe_recv(struct net_backend *be, struct iovec *iov, int iovcnt)
{
	unsigned int hlen = 0; /* length of prepended virtio-net header */
	int ret;

	if (be == NULL)
		return -1;

	if (be->be_vnet_hdr_len != be->fe_vnet_hdr_len) {
		struct virtio_net_rxhdr *vh;

		/* Here we are sure be->be_vnet_hdr_len is 0. */
		hlen = be->fe_vnet_hdr_len;
		/*
		 * Get a pointer to the rx header, and use the
		 * data immediately following it for the packet buffer.
		 */
		vh = iov[0].iov_base;
		iov = iov_trim(iov, &iovcnt, hlen);

		/*
		 * Here we are sure be->fe_vnet_hdr_len is 0.
		 * The only valid field in the rx packet header is the
		 * number of buffers if merged rx bufs were negotiated.
		 */
		memset(vh, 0, hlen);

		if (hlen == VNET_HDR_LEN) {
			vh->vrh_bufs = 1;
		}
	}

	ret = be->recv(be, iov, iovcnt);
	if (ret > 0) {
		ret += hlen;
	}

	return ret;
}
