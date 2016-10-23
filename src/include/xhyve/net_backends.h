/*-
 * Copyright (c) 2014 Vincenzo Maffione <v.maffione@gmail.com>
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

#ifndef __NET_BACKENDS_H__
#define __NET_BACKENDS_H__

#include <stdint.h>
#ifdef WITH_NETMAP
#include <net/netmap.h>
#include <net/netmap_virt.h>
#endif /* WITH_NETMAP */
#include "mevent.h"

extern int netmap_ioctl_counter;

typedef void (*net_backend_cb_t)(int, enum ev_type, void *param);

/* Interface between virtio-net and the network backend. */
struct net_backend;

struct net_backend *netbe_init(const char *devname,
			net_backend_cb_t cb, void *param);
void	netbe_cleanup(struct net_backend *be);
uint64_t netbe_get_cap(struct net_backend *be);
int	 netbe_set_cap(struct net_backend *be, uint64_t cap,
			    unsigned vnet_hdr_len);
void	netbe_send(struct net_backend *be, struct iovec *iov,
		   int iovcnt, uint32_t len, int more);
int	netbe_recv(struct net_backend *be, struct iovec *iov, int iovcnt);


/*
 * Network device capabilities taken from VirtIO standard.
 * Despite the name, these capabilities can be used by different frontents
 * (virtio-net, ptnet) and supported by different backends (netmap, tap, ...).
 */
#define	VIRTIO_NET_F_CSUM	(1 <<  0) /* host handles partial cksum */
#define	VIRTIO_NET_F_GUEST_CSUM	(1 <<  1) /* guest handles partial cksum */
#define	VIRTIO_NET_F_MAC	(1 <<  5) /* host supplies MAC */
#define	VIRTIO_NET_F_GSO_DEPREC	(1 <<  6) /* deprecated: host handles GSO */
#define	VIRTIO_NET_F_GUEST_TSO4	(1 <<  7) /* guest can rcv TSOv4 */
#define	VIRTIO_NET_F_GUEST_TSO6	(1 <<  8) /* guest can rcv TSOv6 */
#define	VIRTIO_NET_F_GUEST_ECN	(1 <<  9) /* guest can rcv TSO with ECN */
#define	VIRTIO_NET_F_GUEST_UFO	(1 << 10) /* guest can rcv UFO */
#define	VIRTIO_NET_F_HOST_TSO4	(1 << 11) /* host can rcv TSOv4 */
#define	VIRTIO_NET_F_HOST_TSO6	(1 << 12) /* host can rcv TSOv6 */
#define	VIRTIO_NET_F_HOST_ECN	(1 << 13) /* host can rcv TSO with ECN */
#define	VIRTIO_NET_F_HOST_UFO	(1 << 14) /* host can rcv UFO */
#define	VIRTIO_NET_F_MRG_RXBUF	(1 << 15) /* host can merge RX buffers */
#define	VIRTIO_NET_F_STATUS	(1 << 16) /* config status field available */
#define	VIRTIO_NET_F_CTRL_VQ	(1 << 17) /* control channel available */
#define	VIRTIO_NET_F_CTRL_RX	(1 << 18) /* control channel RX mode support */
#define	VIRTIO_NET_F_CTRL_VLAN	(1 << 19) /* control channel VLAN filtering */
#define	VIRTIO_NET_F_GUEST_ANNOUNCE \
				(1 << 21) /* guest can send gratuitous pkts */

/*
 * Fixed network header size
 */
struct virtio_net_rxhdr {
	uint8_t		vrh_flags;
	uint8_t		vrh_gso_type;
	uint16_t	vrh_hdr_len;
	uint16_t	vrh_gso_size;
	uint16_t	vrh_csum_start;
	uint16_t	vrh_csum_offset;
	uint16_t	vrh_bufs;
} /* __packed */ ;

/*
 * ptnetmap definitions
 */
struct ptnetmap_state {
	void		*netmap_priv;

	/* True if ptnetmap kthreads are running. */
	int		running;

	/* Feature acknoweledgement support. */
	unsigned long	features;
	unsigned long	acked_features;

	/* Info about netmap memory. */
	uint32_t	memsize;
	void		*mem;
};

/* Used to get read-only info. */
struct netmap_if_info {
	uint32_t nifp_offset;
	uint16_t num_tx_rings;
	uint16_t num_rx_rings;
	uint16_t num_tx_slots;
	uint16_t num_rx_slots;
};

#ifdef WITH_NETMAP
int ptn_memdev_attach(void *mem_ptr, struct netmap_pools_info *);
int ptnetmap_get_netmap_if(struct ptnetmap_state *ptn,
			   struct netmap_if_info *nif);
struct ptnetmap_state * get_ptnetmap(struct net_backend *be);
uint32_t ptnetmap_ack_features(struct ptnetmap_state *ptn,
			       uint32_t wanted_features);
int ptnetmap_get_hostmemid(struct ptnetmap_state *ptn);
int ptnetmap_create(struct ptnetmap_state *ptn, struct ptnetmap_cfg *cfg);
int ptnetmap_delete(struct ptnetmap_state *ptn);
#endif /* WITH_NETMAP */


#include "pci_emul.h"
int net_parsemac(char *mac_str, uint8_t *mac_addr);
void net_genmac(struct pci_devinst *pi, uint8_t *macaddr);

#endif /* __NET_BACKENDS_H__ */
