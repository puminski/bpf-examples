// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2017 - 2022 Intel Corporation. */

#include <errno.h>
#include <getopt.h>
#include <libgen.h>
#include <linux/bpf.h>
#include <linux/err.h>
#include <linux/if_link.h>
#include <linux/if_xdp.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/limits.h>
#include <linux/udp.h>
#include <arpa/inet.h>
#include <locale.h>
#include <net/ethernet.h>
#include <netinet/ether.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>

#include <xdp/xsk.h>
#include <xdp/libxdp.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "act.h"
#include "xdpact.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#ifndef AF_XDP
#define AF_XDP 44
#endif

#ifndef PF_XDP
#define PF_XDP AF_XDP
#endif

#ifndef SO_PREFER_BUSY_POLL
#define SO_PREFER_BUSY_POLL     69
#endif

#ifndef SO_BUSY_POLL_BUDGET
#define SO_BUSY_POLL_BUDGET     70
#endif

#define NUM_BUFFERS 2
#define NUM_FRAMES (4 * 1024)

#define MIN_PKT_SIZE 64
#define MAX_PKT_SIZE 9728 /* Max frame size supported by many NICs */
#define IS_EOP_DESC(options) (!((options) & XDP_PKT_CONTD))

#define DEBUG_HEXDUMP 0

#define VLAN_PRIO_MASK		0xe000 /* Priority Code Point */
#define VLAN_PRIO_SHIFT		13
#define VLAN_VID_MASK		0x0fff /* VLAN Identifier */
#define VLAN_VID__DEFAULT	1
#define VLAN_PRI__DEFAULT	0

#define NSEC_PER_SEC		1000000000UL
#define NSEC_PER_USEC		1000

#define SCHED_PRI__DEFAULT	0
#define STRERR_BUFSIZE          1024

typedef __u64 u64;
typedef __u32 u32;
typedef __u16 u16;
typedef __u8  u8;


static long tx_cycle_diff_min;
static long tx_cycle_diff_max;
static double tx_cycle_diff_ave;
static long tx_cycle_cnt;


static enum xdp_attach_mode opt_attach_mode = XDP_MODE_NATIVE;
static const char *opt_if = "";
static int opt_ifindex;
static int opt_queue;
static unsigned long opt_duration;
static unsigned long start_time;
static bool benchmark_done;
static u32 opt_batch_size = 64;
static int opt_pkt_count;
static u16 opt_pkt_size = MIN_PKT_SIZE;
static u32 opt_pkt_fill_pattern = 0x12345678;
static bool opt_vlan_tag;
static u16 opt_pkt_vlan_id = VLAN_VID__DEFAULT;
static u16 opt_pkt_vlan_pri = VLAN_PRI__DEFAULT;
static struct ether_addr opt_txdmac = {{ 0x00, 0x00, 0x00,
					 0xC9, 0xA0, 0x00 }};
static struct ether_addr opt_txsmac = {{ 0xec, 0xb1, 0xd7,
					 0x98, 0x3a, 0xc0 }};
static bool opt_extra_stats;
static bool opt_quiet;
static bool opt_app_stats;
static u32 irq_no;
static int irqs_at_init = -1;
static u32 sequence;
static int opt_interval = 10;
static int opt_retries = 3;
static u32 opt_xdp_bind_flags = XDP_USE_NEED_WAKEUP;
static u32 opt_umem_flags;
static int opt_unaligned_chunks;
static int opt_mmap_flags;
static int opt_xsk_frame_size = XSK_UMEM__DEFAULT_FRAME_SIZE;
static int frames_per_pkt;
static bool opt_need_wakeup = true;
static u32 opt_num_xsks = 1;
static int opt_schpolicy = SCHED_OTHER;
static int opt_schprio = SCHED_PRI__DEFAULT;
static bool opt_tstamp;
static struct xdp_program *xdp_prog;
static bool opt_frags;
static useconds_t opt_cycle = 10000; /* 10 ms */

struct vlan_ethhdr {
	unsigned char h_dest[6];
	unsigned char h_source[6];
	__be16 h_vlan_proto;
	__be16 h_vlan_TCI;
	__be16 h_vlan_encapsulated_proto;
};

#define PKTGEN_MAGIC 0xbe9be955
struct pktgen_hdr {
	__be32 pgh_magic;
	__be32 seq_num;
	__be32 tv_sec;
	__be32 tv_usec;
};

struct xsk_ring_stats {
	unsigned long rx_frags;
	unsigned long rx_npkts;
	unsigned long tx_frags;
	unsigned long tx_npkts;
	unsigned long rx_dropped_npkts;
	unsigned long rx_invalid_npkts;
	unsigned long tx_invalid_npkts;
	unsigned long rx_full_npkts;
	unsigned long rx_fill_empty_npkts;
	unsigned long tx_empty_npkts;
	unsigned long prev_rx_frags;
	unsigned long prev_rx_npkts;
	unsigned long prev_tx_frags;
	unsigned long prev_tx_npkts;
	unsigned long prev_rx_dropped_npkts;
	unsigned long prev_rx_invalid_npkts;
	unsigned long prev_tx_invalid_npkts;
	unsigned long prev_rx_full_npkts;
	unsigned long prev_rx_fill_empty_npkts;
	unsigned long prev_tx_empty_npkts;
};

struct xsk_driver_stats {
	unsigned long intrs;
	unsigned long prev_intrs;
};

struct xsk_app_stats {
	unsigned long rx_empty_polls;
	unsigned long fill_fail_polls;
	unsigned long copy_tx_sendtos;
	unsigned long tx_wakeup_sendtos;
	unsigned long prev_rx_empty_polls;
	unsigned long prev_fill_fail_polls;
	unsigned long prev_copy_tx_sendtos;
	unsigned long prev_tx_wakeup_sendtos;
};

struct xsk_umem_info {
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_umem *umem;
	void *buffer;
};

struct xsk_socket_info {
     struct xsk_ring_cons rx;
     struct xsk_ring_prod tx;
     struct xsk_umem_info *umem;
     struct xsk_socket *xsk;
     struct xsk_ring_stats ring_stats;
     struct xsk_app_stats app_stats;
     struct xsk_driver_stats drv_stats;
     u32 outstanding_tx;
     struct act_socket  *act_xsk;
     struct act_index_table_info *index_table;
};

static const struct clockid_map {
	const char *name;
	clockid_t clockid;
} clockids_map[] = {
	{ "REALTIME", CLOCK_REALTIME },
	{ "TAI", CLOCK_TAI },
	{ "BOOTTIME", CLOCK_BOOTTIME },
	{ "MONOTONIC", CLOCK_MONOTONIC },
	{ NULL }
};

static const struct sched_map {
	const char *name;
	int policy;
} schmap[] = {
	{ "OTHER", SCHED_OTHER },
	{ "FIFO", SCHED_FIFO },
	{ NULL }
};

static int num_socks;
struct xsk_socket_info *xsks[MAX_SOCKS];
int sock;

static inline int get_batch_size(void)
{
	if (!opt_pkt_count)
	    opt_pkt_count = 1;
	return opt_pkt_count;
}

static int get_clockid(clockid_t *id, const char *name)
{
     const struct clockid_map *clk;

     for (clk = clockids_map; clk->name; clk++) {
	  if (strcasecmp(clk->name, name) == 0) {
	       *id = clk->clockid;
	       return 0;
	  }
     }

     return -1;
}

static int get_schpolicy(int *policy, const char *name)
{
	const struct sched_map *sch;

	for (sch = schmap; sch->name; sch++) {
		if (strcasecmp(sch->name, name) == 0) {
			*policy = sch->policy;
			return 0;
		}
	}

	return -1;
}


static void print_benchmark(bool running)
{
	const char *bench_str = "INVALID";

	bench_str = "txonly";
	
	printf("%s:%d %s ", opt_if, opt_queue, bench_str);
	if (opt_attach_mode == XDP_MODE_SKB)
		printf("xdp-skb ");
	else if (opt_attach_mode == XDP_MODE_NATIVE)
		printf("xdp-drv ");
	else
		printf("	");

	if (running) {
		printf("running...");
		fflush(stdout);
	}
}

static int xsk_get_xdp_stats(int fd, struct xsk_socket_info *xsk)
{
	struct xdp_statistics stats;
	socklen_t optlen;
	int err;

	optlen = sizeof(stats);
	err = getsockopt(fd, SOL_XDP, XDP_STATISTICS, &stats, &optlen);
	if (err)
		return err;

	if (optlen == sizeof(struct xdp_statistics)) {
		xsk->ring_stats.rx_dropped_npkts = stats.rx_dropped;
		xsk->ring_stats.rx_invalid_npkts = stats.rx_invalid_descs;
		xsk->ring_stats.tx_invalid_npkts = stats.tx_invalid_descs;
		xsk->ring_stats.rx_full_npkts = stats.rx_ring_full;
		xsk->ring_stats.rx_fill_empty_npkts = stats.rx_fill_ring_empty_descs;
		xsk->ring_stats.tx_empty_npkts = stats.tx_ring_empty_descs;
		return 0;
	}

	return -EINVAL;
}

static void dump_app_stats(long dt)
{
	int i;

	for (i = 0; i < num_socks && xsks[i]; i++) {
		char *fmt = "%-18s %'-14.0f %'-14lu\n";
		double rx_empty_polls_ps, fill_fail_polls_ps, copy_tx_sendtos_ps,
		     tx_wakeup_sendtos_ps;

		rx_empty_polls_ps = (xsks[i]->app_stats.rx_empty_polls -
					xsks[i]->app_stats.prev_rx_empty_polls) * 1000000000. / dt;
		fill_fail_polls_ps = (xsks[i]->app_stats.fill_fail_polls -
					xsks[i]->app_stats.prev_fill_fail_polls) * 1000000000. / dt;
		copy_tx_sendtos_ps = (xsks[i]->app_stats.copy_tx_sendtos -
					xsks[i]->app_stats.prev_copy_tx_sendtos) * 1000000000. / dt;
		tx_wakeup_sendtos_ps = (xsks[i]->app_stats.tx_wakeup_sendtos -
					xsks[i]->app_stats.prev_tx_wakeup_sendtos)
										* 1000000000. / dt;

		printf("\n%-18s %-14s %-14s\n", "", "calls/s", "count");
		printf(fmt, "rx empty polls", rx_empty_polls_ps, xsks[i]->app_stats.rx_empty_polls);
		printf(fmt, "fill fail polls", fill_fail_polls_ps,
							xsks[i]->app_stats.fill_fail_polls);
		printf(fmt, "copy tx sendtos", copy_tx_sendtos_ps,
							xsks[i]->app_stats.copy_tx_sendtos);
		printf(fmt, "tx wakeup sendtos", tx_wakeup_sendtos_ps,
							xsks[i]->app_stats.tx_wakeup_sendtos);


		xsks[i]->app_stats.prev_rx_empty_polls = xsks[i]->app_stats.rx_empty_polls;
		xsks[i]->app_stats.prev_fill_fail_polls = xsks[i]->app_stats.fill_fail_polls;
		xsks[i]->app_stats.prev_copy_tx_sendtos = xsks[i]->app_stats.copy_tx_sendtos;
		xsks[i]->app_stats.prev_tx_wakeup_sendtos = xsks[i]->app_stats.tx_wakeup_sendtos;

	}

}


static void int_exit(int sig)
{
	benchmark_done = true;
}

static void __exit_with_error(int error, const char *file, const char *func,
			      int line)
{
	fprintf(stderr, "%s:%s:%i: errno: %d/\"%s\"\n", file, func,
		line, error, strerror(error));

	exit(EXIT_FAILURE);
}

#define exit_with_error(error) __exit_with_error(error, __FILE__, __func__, __LINE__)

static void xdpsock_cleanup(void)
{
	struct xsk_umem *umem = xsks[0]->umem->umem;
	int i;

	for (i = 0; i < num_socks; i++) {
	    xsk_socket__delete(xsks[i]->xsk);
	}

	(void)xsk_umem__delete(umem);


}


static void hex_dump(void *pkt, size_t length, u64 addr)
{
	const unsigned char *address = (unsigned char *)pkt;
	const unsigned char *line = address;
	size_t line_size = 32;
	unsigned char c;
	char buf[32];
	int i = 0;

	if (!DEBUG_HEXDUMP)
		return;

	sprintf(buf, "addr=%llu", addr);
	printf("length = %zu\n", length);
	printf("%s | ", buf);
	while (length-- > 0) {
		printf("%02X ", *address++);
		if (!(++i % line_size) || (length == 0 && i % line_size)) {
			if (length == 0) {
				while (i++ % line_size)
					printf("__ ");
			}
			printf(" | ");	/* right close */
			while (line < address) {
				c = *line++;
				printf("%c", (c < 33 || c == 255) ? 0x2E : c);
			}
			printf("\n");
			if (length > 0)
				printf("%s | ", buf);
		}
	}
	printf("\n");
}

static void *memset32_htonl(void *dest, u32 val, u32 size)
{
	u32 *ptr = (u32 *)dest;
	int i;

	val = htonl(val);

	for (i = 0; i < (size & (~0x3)); i += 4)
		ptr[i >> 2] = val;

	for (; i < size; i++)
		((char *)dest)[i] = ((char *)&val)[i & 3];

	return dest;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static inline unsigned short from32to16(unsigned int x)
{
	/* add up 16-bit and 16-bit for 16+c bit */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static unsigned int do_csum(const unsigned char *buff, int len)
{
	unsigned int result = 0;
	int odd;

	if (len <= 0)
		goto out;
	odd = 1 & (unsigned long)buff;
	if (odd) {
#ifdef __LITTLE_ENDIAN
		result += (*buff << 8);
#else
		result = *buff;
#endif
		len--;
		buff++;
	}
	if (len >= 2) {
		if (2 & (unsigned long)buff) {
			result += *(unsigned short *)buff;
			len -= 2;
			buff += 2;
		}
		if (len >= 4) {
			const unsigned char *end = buff +
						   ((unsigned int)len & ~3);
			unsigned int carry = 0;

			do {
				unsigned int w = *(unsigned int *)buff;

				buff += 4;
				result += carry;
				result += w;
				carry = (w > result);
			} while (buff < end);
			result += carry;
			result = (result & 0xffff) + (result >> 16);
		}
		if (len & 2) {
			result += *(unsigned short *)buff;
			buff += 2;
		}
	}
	if (len & 1)
#ifdef __LITTLE_ENDIAN
		result += *buff;
#else
		result += (*buff << 8);
#endif
	result = from32to16(result);
	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);
out:
	return result;
}

/*
 *	This is a version of ip_compute_csum() optimized for IP headers,
 *	which always checksum on 4 octet boundaries.
 *	This function code has been taken from
 *	Linux kernel lib/checksum.c
 */
static inline __sum16 ip_fast_csum(const void *iph, unsigned int ihl)
{
	return (__sum16)~do_csum(iph, ihl * 4);
}

/*
 * Fold a partial checksum
 * This function code has been taken from
 * Linux kernel include/asm-generic/checksum.h
 */
static inline __sum16 csum_fold(__wsum csum)
{
	u32 sum = (u32)csum;

	sum = (sum & 0xffff) + (sum >> 16);
	sum = (sum & 0xffff) + (sum >> 16);
	return (__sum16)~sum;
}

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
static inline u32 from64to32(u64 x)
{
	/* add up 32-bit and 32-bit for 32+c bit */
	x = (x & 0xffffffff) + (x >> 32);
	/* add up carry.. */
	x = (x & 0xffffffff) + (x >> 32);
	return (u32)x;
}

__wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr,
			  __u32 len, __u8 proto, __wsum sum);

/*
 * This function code has been taken from
 * Linux kernel lib/checksum.c
 */
__wsum csum_tcpudp_nofold(__be32 saddr, __be32 daddr,
			  __u32 len, __u8 proto, __wsum sum)
{
	unsigned long long s = (u32)sum;

	s += (u32)saddr;
	s += (u32)daddr;
#ifdef __BIG_ENDIAN__
	s += proto + len;
#else
	s += (proto + len) << 8;
#endif
	return (__wsum)from64to32(s);
}

/*
 * This function has been taken from
 * Linux kernel include/asm-generic/checksum.h
 */
static inline __sum16
csum_tcpudp_magic(__be32 saddr, __be32 daddr, __u32 len,
		  __u8 proto, __wsum sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr, daddr, len, proto, sum));
}

static inline u16 udp_csum(u32 saddr, u32 daddr, u32 len,
			   u8 proto, u16 *udp_pkt)
{
	u32 csum = 0;
	u32 cnt = 0;

	/* udp hdr and data */
	for (; cnt < len; cnt += 2)
		csum += udp_pkt[cnt >> 1];

	return csum_tcpudp_magic(saddr, daddr, len, proto, csum);
}

#define ETH_FCS_SIZE 4

#define ETH_HDR_SIZE (opt_vlan_tag ? sizeof(struct vlan_ethhdr) : \
		      sizeof(struct ethhdr))
#define PKTGEN_HDR_SIZE (opt_tstamp ? sizeof(struct pktgen_hdr) : 0)
#define PKT_HDR_SIZE (ETH_HDR_SIZE + sizeof(struct iphdr) + \
		      sizeof(struct udphdr) + PKTGEN_HDR_SIZE)
#define PKTGEN_HDR_OFFSET (ETH_HDR_SIZE + sizeof(struct iphdr) + \
			   sizeof(struct udphdr))
#define PKTGEN_SIZE_MIN (PKTGEN_HDR_OFFSET + sizeof(struct pktgen_hdr) + \
			 ETH_FCS_SIZE)

#define PKT_SIZE		(opt_pkt_size - ETH_FCS_SIZE)
#define IP_PKT_SIZE		(PKT_SIZE - ETH_HDR_SIZE)
#define UDP_PKT_SIZE		(IP_PKT_SIZE - sizeof(struct iphdr))
#define UDP_PKT_DATA_SIZE	(UDP_PKT_SIZE - \
				 (sizeof(struct udphdr) + PKTGEN_HDR_SIZE))

static u8 pkt_data[MAX_PKT_SIZE];

static void gen_eth_hdr_data(void)
{
	struct pktgen_hdr *pktgen_hdr;
	struct udphdr *udp_hdr;
	struct iphdr *ip_hdr;

	if (opt_vlan_tag) {
		struct vlan_ethhdr *veth_hdr = (struct vlan_ethhdr *)pkt_data;
		u16 vlan_tci = 0;

		udp_hdr = (struct udphdr *)(pkt_data +
					    sizeof(struct vlan_ethhdr) +
					    sizeof(struct iphdr));
		ip_hdr = (struct iphdr *)(pkt_data +
					  sizeof(struct vlan_ethhdr));
		pktgen_hdr = (struct pktgen_hdr *)(pkt_data +
						   sizeof(struct vlan_ethhdr) +
						   sizeof(struct iphdr) +
						   sizeof(struct udphdr));
		/* ethernet & VLAN header */
		memcpy(veth_hdr->h_dest, &opt_txdmac, ETH_ALEN);
		memcpy(veth_hdr->h_source, &opt_txsmac, ETH_ALEN);
		veth_hdr->h_vlan_proto = htons(ETH_P_8021Q);
		vlan_tci = opt_pkt_vlan_id & VLAN_VID_MASK;
		vlan_tci |= (opt_pkt_vlan_pri << VLAN_PRIO_SHIFT) & VLAN_PRIO_MASK;
		veth_hdr->h_vlan_TCI = htons(vlan_tci);
		veth_hdr->h_vlan_encapsulated_proto = htons(ETH_P_IP);
	} else {
		struct ethhdr *eth_hdr = (struct ethhdr *)pkt_data;

		udp_hdr = (struct udphdr *)(pkt_data +
					    sizeof(struct ethhdr) +
					    sizeof(struct iphdr));
		ip_hdr = (struct iphdr *)(pkt_data +
					  sizeof(struct ethhdr));
		pktgen_hdr = (struct pktgen_hdr *)(pkt_data +
						   sizeof(struct ethhdr) +
						   sizeof(struct iphdr) +
						   sizeof(struct udphdr));
		/* ethernet header */
		memcpy(eth_hdr->h_dest, &opt_txdmac, ETH_ALEN);
		memcpy(eth_hdr->h_source, &opt_txsmac, ETH_ALEN);
		eth_hdr->h_proto = htons(ETH_P_IP);
	}


	/* IP header */
	ip_hdr->version = IPVERSION;
	ip_hdr->ihl = 0x5; /* 20 byte header */
	ip_hdr->tos = 0x0;
	ip_hdr->tot_len = htons(IP_PKT_SIZE);
	ip_hdr->id = 0;
	ip_hdr->frag_off = 0;
	ip_hdr->ttl = IPDEFTTL;
	ip_hdr->protocol = IPPROTO_UDP;
	ip_hdr->saddr = htonl(0x0a0a0a10);
	ip_hdr->daddr = htonl(0x0a0a0a20);

	/* IP header checksum */
	ip_hdr->check = 0;
	ip_hdr->check = ip_fast_csum((const void *)ip_hdr, ip_hdr->ihl);

	/* UDP header */
	udp_hdr->source = htons(0x1000);
	udp_hdr->dest = htons(0x1000);
	udp_hdr->len = htons(UDP_PKT_SIZE);

	if (opt_tstamp)
		pktgen_hdr->pgh_magic = htonl(PKTGEN_MAGIC);

	/* UDP data */
	memset32_htonl(pkt_data + PKT_HDR_SIZE, opt_pkt_fill_pattern,
		       UDP_PKT_DATA_SIZE);

	/* UDP header checksum */
	udp_hdr->check = 0;
	udp_hdr->check = udp_csum(ip_hdr->saddr, ip_hdr->daddr, UDP_PKT_SIZE,
				  IPPROTO_UDP, (u16 *)udp_hdr);
}

static void gen_eth_frame(struct xsk_umem_info *umem, u64 addr)
{
	static u32 len;
	u32 copy_len = opt_xsk_frame_size;

	if (!len)
		len = PKT_SIZE;

	if (len < opt_xsk_frame_size)
	     copy_len = len;

	memcpy(xsk_umem__get_data(umem->buffer, addr),
			pkt_data + PKT_SIZE - len, copy_len);

	len -= copy_len;
}


/* Fill the packet payload in <frame>  in UMEM using <val> */
static void modify_eth_frame(struct xsk_socket_info *xsk, int frame, u32 val)
{
     u8  *buffer, *payload;
     u64 addr;

     addr = frame * opt_xsk_frame_size;
     buffer = xsk_umem__get_data(xsk->umem->buffer, addr);
     payload = buffer + PKT_HDR_SIZE;
     memset32_htonl(payload, val,
		       UDP_PKT_DATA_SIZE);
     
}

static struct xsk_umem_info *xsk_configure_umem(void *buffer, u64 size)
{
	struct xsk_umem_info *umem;
	struct xsk_umem_config cfg = {
		/* We recommend that you set the fill ring size >= HW RX ring size +
		 * AF_XDP RX ring size. Make sure you fill up the fill ring
		 * with buffers at regular intervals, and you will with this setting
		 * avoid allocation failures in the driver. These are usually quite
		 * expensive since drivers have not been written to assume that
		 * allocation failures are common. For regular sockets, kernel
		 * allocated memory is used that only runs out in OOM situations
		 * that should be rare.
		 */
		.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS * 2,
		.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS,
		.frame_size = opt_xsk_frame_size,
		.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM,
		.flags = opt_umem_flags
	};
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		exit_with_error(errno);

	ret = xsk_umem__create(&umem->umem, buffer, size, &umem->fq, &umem->cq,
			       &cfg);
	if (ret)
		exit_with_error(-ret);

	umem->buffer = buffer;
	return umem;
}


static struct xsk_socket_info *xsk_configure_socket(struct xsk_umem_info *umem,
						    bool rx, bool tx)
{
	struct xsk_socket_config cfg;
	struct xsk_socket_info *xsk;
	struct xsk_ring_cons *rxr;
	struct xsk_ring_prod *txr;
	struct act_socket  *act_xsk;
	struct act_index_table_info *index_table;
	struct act_cyclic_socket_params act_xsk_params;
	int ret;

	
	xsk = calloc(1, sizeof(*xsk));
	if (!xsk)
		exit_with_error(errno);

	xsk->umem = umem;
	cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	cfg.libxdp_flags = 0;
	if (opt_attach_mode == XDP_MODE_SKB)
		cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
	else
		cfg.xdp_flags = XDP_FLAGS_DRV_MODE;
	cfg.bind_flags = opt_xdp_bind_flags;

	rxr = rx ? &xsk->rx : NULL;
	txr = tx ? &xsk->tx : NULL;
	ret = xsk_socket__create(&xsk->xsk, opt_if, opt_queue, umem->umem,
				 rxr, txr, &cfg);
	if (ret)
		exit_with_error(-ret);

	/* Prepare parameters for ACT socket configuration */
	act_xsk_params.ifname = opt_if;
	act_xsk_params.queue_id = opt_queue;
	act_xsk_params.umem = umem->umem;
	act_xsk_params.frame_size = opt_xsk_frame_size;
	act_xsk_params.tx = txr;
	act_xsk_params.comp = &umem->cq;
	act_xsk_params.cycle = opt_cycle;
	
	act_xsk = act_cyclic_socket_configure(xsk->xsk, &act_xsk_params);
	if (!act_xsk)
		exit_with_error(-ret);
	index_table = act_index_table_create(act_xsk, get_batch_size());
	if (!index_table)
		exit_with_error(-ret);

	xsk->act_xsk = act_xsk;
	xsk->index_table = index_table;
	xsk->app_stats.rx_empty_polls = 0;
	xsk->app_stats.fill_fail_polls = 0;
	xsk->app_stats.copy_tx_sendtos = 0;
	xsk->app_stats.tx_wakeup_sendtos = 0;

	xsk->app_stats.prev_rx_empty_polls = 0;
	xsk->app_stats.prev_fill_fail_polls = 0;
	xsk->app_stats.prev_copy_tx_sendtos = 0;
	xsk->app_stats.prev_tx_wakeup_sendtos = 0;


	return xsk;
}

static struct option long_options[] = {
	{"rxdrop", no_argument, 0, 'r'},
	{"txonly", no_argument, 0, 't'},
	{"l2fwd", no_argument, 0, 'l'},
	{"interface", required_argument, 0, 'i'},
	{"queue", required_argument, 0, 'q'},
	{"xdp-skb", no_argument, 0, 'S'},
	{"xdp-native", no_argument, 0, 'N'},
	{"interval", required_argument, 0, 'n'},
	{"retries", required_argument, 0, 'O'},
	{"zero-copy", no_argument, 0, 'z'},
	{"copy", no_argument, 0, 'c'},
	{"frame-size", required_argument, 0, 'f'},
	{"no-need-wakeup", no_argument, 0, 'm'},
	{"unaligned", no_argument, 0, 'u'},
	{"shared-umem", no_argument, 0, 'M'},
	{"force", no_argument, 0, 'F'},
	{"duration", required_argument, 0, 'd'},
	{"clock", required_argument, 0, 'w'},
	{"batch-size", required_argument, 0, 'b'},
	{"tx-pkt-count", required_argument, 0, 'C'},
	{"tx-pkt-size", required_argument, 0, 's'},
	{"tx-pkt-pattern", required_argument, 0, 'P'},
	{"tx-vlan", no_argument, 0, 'V'},
	{"tx-vlan-id", required_argument, 0, 'J'},
	{"tx-vlan-pri", required_argument, 0, 'K'},
	{"tx-dmac", required_argument, 0, 'G'},
	{"tx-smac", required_argument, 0, 'H'},
	{"tx-cycle", required_argument, 0, 'T'},
	{"tstamp", no_argument, 0, 'y'},
	{"policy", required_argument, 0, 'W'},
	{"schpri", required_argument, 0, 'U'},
	{"extra-stats", no_argument, 0, 'x'},
	{"quiet", no_argument, 0, 'Q'},
	{"app-stats", no_argument, 0, 'a'},
	{"irq-string", no_argument, 0, 'I'},
	{"busy-poll", no_argument, 0, 'B'},
	{0, 0, 0, 0}
};

static void usage(const char *prog)
{
	const char *str =
		"  Usage: %s [OPTIONS]\n"
		"  Options:\n"
		"  -i, --interface=n	Run on interface n\n"
		"  -q, --queue=n	Use queue n (default 0)\n"
		"  -S, --xdp-skb=n	Use XDP skb-mod\n"
		"  -N, --xdp-native=n	Enforce XDP native mode\n"
		"  -n, --interval=n	How many cycles you run.\n"
		"  -O, --retries=n	Specify time-out retries (1s interval) attempt (default 3).\n"
		"  -z, --zero-copy      Force zero-copy mode.\n"
		"  -c, --copy           Force copy mode.\n"
		"  -m, --no-need-wakeup Turn off use of driver need wakeup flag.\n"
		"  -f, --frame-size=n   Set the frame size (must be a power of two in aligned mode, default is %d).\n"
		"  -u, --unaligned	Enable unaligned chunk placement\n"
		"  -M, --shared-umem	Enable XDP_SHARED_UMEM (cannot be used with -R)\n"
		"  -d, --duration=n	Duration in secs to run command.\n"
		"			Default: forever.\n"
		"  -b, --batch-size=n	Batch size for sending or receiving\n"
		"			packets. Default: %d\n"
		"  -C, --tx-pkt-count=n	Number of packets to send per cycle.\n"
		"  -s, --tx-pkt-size=n	Transmit packet size.\n"
		"			(Default: %d bytes)\n"
		"			Min size: %d, Max size %d.\n"
		"  -P, --tx-pkt-pattern=nPacket fill pattern. Default: 0x%x\n"
		"  -V, --tx-vlan        Send VLAN tagged  packets (For -t|--txonly)\n"
		"  -J, --tx-vlan-id=n   Tx VLAN ID [1-4095]. Default: %d (For -V|--tx-vlan)\n"
		"  -K, --tx-vlan-pri=n  Tx VLAN Priority [0-7]. Default: %d (For -V|--tx-vlan)\n"
		"  -G, --tx-dmac=<MAC>  Dest MAC addr of TX frame in aa:bb:cc:dd:ee:ff format (For -V|--tx-vlan)\n"
		"  -H, --tx-smac=<MAC>  Src MAC addr of TX frame in aa:bb:cc:dd:ee:ff format (For -V|--tx-vlan)\n"
		"  -T, --tx-cycle=n     Tx cycle time in micro-seconds (For -t|--txonly).\n"
		"  -y, --tstamp         Add time-stamp to packet (For -t|--txonly).\n"
		"  -W, --policy=POLICY  Schedule policy. Default: SCHED_OTHER\n"
		"  -U, --schpri=n       Schedule priority. Default: %d\n"
		"  -x, --extra-stats	Display extra statistics.\n"
		"  -Q, --quiet          Do not display any stats.\n"
		"  -a, --app-stats	Display application (syscall) statistics.\n"
		"  -I, --irq-string	Display driver interrupt statistics for interface associated with irq-string.\n"
		"  -B, --busy-poll      Busy poll.\n"
		"  -F, --frags		Enable frags (multi-buffer) support\n"
		"\n";
	fprintf(stderr, str, prog, XSK_UMEM__DEFAULT_FRAME_SIZE,
		opt_batch_size, MIN_PKT_SIZE, MIN_PKT_SIZE,
		MAX_PKT_SIZE, opt_pkt_fill_pattern,
		VLAN_VID__DEFAULT, VLAN_PRI__DEFAULT,
		SCHED_PRI__DEFAULT);

	exit(EXIT_FAILURE);
}

static void parse_command_line(int argc, char **argv)
{
	int option_index, c;

	opterr = 0;

	for (;;) {
		c = getopt_long(argc, argv,
				"rtli:q:SNn:O:czf:muMd:b:C:s:P:VJ:K:G:H:T:yW:U:xQaRF",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'i':
			opt_if = optarg;
			break;
		case 'q':
			opt_queue = atoi(optarg);
			break;
		case 'S':
			opt_attach_mode = XDP_MODE_SKB;
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'N':
			/* default, set below */
			break;
		case 'n':
			opt_interval = atoi(optarg);
			break;
		case 'O':
			opt_retries = atoi(optarg);
			break;
		case 'z':
			opt_xdp_bind_flags |= XDP_ZEROCOPY;
			break;
		case 'c':
			opt_xdp_bind_flags |= XDP_COPY;
			break;
		case 'u':
			opt_umem_flags |= XDP_UMEM_UNALIGNED_CHUNK_FLAG;
			opt_unaligned_chunks = 1;
			opt_mmap_flags = MAP_HUGETLB;
			break;
		case 'f':
			opt_xsk_frame_size = atoi(optarg);
			break;
		case 'm':
			opt_need_wakeup = false;
			opt_xdp_bind_flags &= ~XDP_USE_NEED_WAKEUP;
			break;
		case 'M':
			opt_num_xsks = MAX_SOCKS;
			break;
		case 'd':
			opt_duration = atoi(optarg);
			opt_duration *= 1000000000;
			break;
		case 'b':
			opt_batch_size = atoi(optarg);
			break;
		case 'C':
			opt_pkt_count = atoi(optarg);
			break;
		case 's':
			opt_pkt_size = atoi(optarg);
			if (opt_pkt_size > (MAX_PKT_SIZE) ||
			    opt_pkt_size < MIN_PKT_SIZE) {
				fprintf(stderr,
					"ERROR: Invalid frame size %d\n",
					opt_pkt_size);
				usage(basename(argv[0]));
			}
			break;
		case 'P':
			opt_pkt_fill_pattern = strtol(optarg, NULL, 16);
			break;
		case 'V':
			opt_vlan_tag = true;
			break;
		case 'J':
			opt_pkt_vlan_id = atoi(optarg);
			break;
		case 'K':
			opt_pkt_vlan_pri = atoi(optarg);
			break;
		case 'G':
			if (!ether_aton_r(optarg,
					  (struct ether_addr *)&opt_txdmac)) {
				fprintf(stderr, "Invalid dmac address:%s\n",
					optarg);
				usage(basename(argv[0]));
			}
			break;
		case 'H':
			if (!ether_aton_r(optarg,
					  (struct ether_addr *)&opt_txsmac)) {
				fprintf(stderr, "Invalid smac address:%s\n",
					optarg);
				usage(basename(argv[0]));
			}
			break;
		case 'y':
			opt_tstamp = 1;
			break;
		case 'W':
			if (get_schpolicy(&opt_schpolicy, optarg)) {
				fprintf(stderr,
					"ERROR: Invalid policy %s. Default to SCHED_OTHER.\n",
					optarg);
				opt_schpolicy = SCHED_OTHER;
			}
			break;
		case 'U':
			opt_schprio = atoi(optarg);
			break;
		case 'x':
			opt_extra_stats = 1;
			break;
		case 'Q':
			opt_quiet = 1;
			break;
		case 'a':
			opt_app_stats = 1;
			break;
		case 'F':
			opt_frags = true;
			break;
		default:
			usage(basename(argv[0]));
		}
	}

	opt_ifindex = if_nametoindex(opt_if);
	if (!opt_ifindex) {
		fprintf(stderr, "ERROR: interface \"%s\" does not exist\n",
			opt_if);
		usage(basename(argv[0]));
	}

	if ((opt_xsk_frame_size & (opt_xsk_frame_size - 1)) &&
	    !opt_unaligned_chunks) {
		fprintf(stderr, "--frame-size=%d is not a power of two\n",
			opt_xsk_frame_size);
		usage(basename(argv[0]));
	}


	if (opt_frags)
		opt_xdp_bind_flags |= XDP_USE_SG;
}

static void kick_tx(struct xsk_socket_info *xsk)
{
	int ret;
	ret = sendto(xsk_socket__fd(xsk->xsk), NULL, 0, MSG_DONTWAIT, NULL, 0);

	if (ret >= 0 || errno == ENOBUFS || errno == EAGAIN ||
	    errno == EBUSY || errno == ENETDOWN)
		return;
	exit_with_error(errno);
}



static inline void complete_tx_only(struct xsk_socket_info *xsk,
				    int batch_size)
{
	unsigned int rcvd;
	u32 idx;

	if (!xsk->outstanding_tx)
		return;

	if (!opt_need_wakeup || xsk_ring_prod__needs_wakeup(&xsk->tx)) {
		xsk->app_stats.tx_wakeup_sendtos++;
		kick_tx(xsk);
	}

	rcvd = xsk_ring_cons__peek(&xsk->umem->cq, batch_size, &idx);
	if (rcvd > 0) {
		xsk_ring_cons__release(&xsk->umem->cq, rcvd);
		xsk->outstanding_tx -= rcvd;
	}
}



static int tx_prepare(struct xsk_socket_info *xsk, u32 *frame_nb,
		   int batch_size, unsigned long tx_ns, int *index)
{
	u32 idx = 0, tv_sec, tv_usec;
	unsigned int i;

	while (xsk_ring_prod__reserve(&xsk->tx, batch_size, &idx) <
				      batch_size) {
		complete_tx_only(xsk, batch_size);
		if (benchmark_done)
			return 0;
	}

	*index = idx;
	
	if (opt_tstamp) {
		tv_sec = (u32)(tx_ns / NSEC_PER_SEC);
		tv_usec = (u32)((tx_ns % NSEC_PER_SEC) / 1000);
	}

	for (i = 0; i < batch_size; ) {
	  u32 len = PKT_SIZE;


	  struct xdp_desc *tx_desc = xsk_ring_prod__tx_desc(&xsk->tx,
							    idx + i);
	  tx_desc->addr = *frame_nb * opt_xsk_frame_size;
	  tx_desc->len = len;
	  tx_desc->options = 0;
	  xsk->ring_stats.tx_npkts++;
		
	  *frame_nb = (*frame_nb + NUM_BUFFERS) % NUM_FRAMES;
	  i++;

	  if (opt_tstamp) {
	    struct pktgen_hdr *pktgen_hdr;
	    u64 addr = tx_desc->addr;
	    char *pkt;

	    pkt = xsk_umem__get_data(xsk->umem->buffer, addr);
	    pktgen_hdr = (struct pktgen_hdr *)(pkt + PKTGEN_HDR_OFFSET);

	    pktgen_hdr->seq_num = htonl(sequence++);
	    pktgen_hdr->tv_sec = htonl(tv_sec);
	    pktgen_hdr->tv_usec = htonl(tv_usec);

	    hex_dump(pkt, PKT_SIZE, addr);
	  }
	}

	xsk_ring_prod__submit(&xsk->tx, batch_size);
	xsk->outstanding_tx += batch_size;
	xsk->ring_stats.tx_frags += batch_size;
	return batch_size / frames_per_pkt;
}


/* Mimic cyclic transmission */
static void tx_cyclic_transmission( useconds_t cycle_time, int batch_size)
{
     int cycle_no, i, j,  buffer_no, frame_no;
     struct act_index_table_info *index_table;
     for (cycle_no = 0; cycle_no < opt_interval; cycle_no++)
     {
	  printf("Staring cycle_no = %d\n", cycle_no);
	  for (i = 0; i < num_socks; i++)
	  {       
	       index_table = xsks[i]->index_table;
	       /* Use 2 buffers per stream */
	       buffer_no = cycle_no % 2;
	       for (j = 0; j < batch_size; j++)
	       {
		    frame_no =  2* j + buffer_no;
		    printf("Packet %d, buffer_no = %d, frame_no %d\n", j, buffer_no, frame_no);
		    modify_eth_frame(xsks[i], frame_no, frame_no);
		    act_ring_prod_update_index(index_table,j, buffer_no);
		    if (benchmark_done)
			 return;
	       }
	  }
	  usleep( cycle_time);
     }
}

static void tx_only_all(void)
{
     u32 frame_nb[MAX_SOCKS] = {};
     unsigned long next_tx_ns = 0;
     int i;
     int batch_size = get_batch_size(), idx = 0;
     unsigned long tx_ns = 0;
     struct timespec next;
     int tx_cnt = 0;
     long diff;
     int err;

     /* Start transmission */
     for (i = 0; i < num_socks; i++)
     {
	  tx_cnt += tx_prepare(xsks[i], &frame_nb[i], batch_size, tx_ns, &idx);
 	  act_cyclic_socket_start(xsks[i]->act_xsk, idx, batch_size);
     }
     
     /* Run test with selected cycle  */
     tx_cyclic_transmission( opt_cycle, batch_size);

     /* Close the socket */
     for (i = 0; i < num_socks; i++)
     {
	  act_cyclic_socket_close(xsks[i]->act_xsk);
     }


	
}



static void apply_setsockopt(struct xsk_socket_info *xsk)
{
	int sock_opt;

	sock_opt = 1;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_PREFER_BUSY_POLL,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = 20;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);

	sock_opt = opt_batch_size;
	if (setsockopt(xsk_socket__fd(xsk->xsk), SOL_SOCKET, SO_BUSY_POLL_BUDGET,
		       (void *)&sock_opt, sizeof(sock_opt)) < 0)
		exit_with_error(errno);
}

int main(int argc, char **argv)
{
	struct rlimit r = {RLIM_INFINITY, RLIM_INFINITY};
	bool rx = false, tx = false;
	struct sched_param schparam;
	struct xsk_umem_info *umem;
	int i, ret;
	void *bufs;

	parse_command_line(argc, argv);
	opt_quiet = 1;

	if (setrlimit(RLIMIT_MEMLOCK, &r)) {
	     fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
		     strerror(errno));
	     exit(EXIT_FAILURE);
	}


	/* Reserve memory for the umem. Use hugepages if unaligned chunk mode */
	bufs = mmap(NULL, NUM_FRAMES * opt_xsk_frame_size,
		    PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS | opt_mmap_flags, -1, 0);
	if (bufs == MAP_FAILED) {
		printf("ERROR: mmap failed\n");
		exit(EXIT_FAILURE);
	}

	/* Create sockets... */
	umem = xsk_configure_umem(bufs, NUM_FRAMES * opt_xsk_frame_size);
	
	tx = true;
	for (i = 0; i < opt_num_xsks; i++)
		xsks[num_socks++] = xsk_configure_socket(umem, rx, tx);

	for (i = 0; i < opt_num_xsks; i++)
		apply_setsockopt(xsks[i]);

	
	if (opt_tstamp && opt_pkt_size < PKTGEN_SIZE_MIN)
	  opt_pkt_size = PKTGEN_SIZE_MIN;

	gen_eth_hdr_data();

	/* Fill entire UMEM with identical Eth frames */
	for (i = 0; i < NUM_FRAMES ; i++)
	  gen_eth_frame(umem, i * opt_xsk_frame_size);
	
	frames_per_pkt = (opt_pkt_size - 1) / XSK_UMEM__DEFAULT_FRAME_SIZE + 1;



	signal(SIGINT, int_exit);
	signal(SIGTERM, int_exit);
	signal(SIGABRT, int_exit);

	setlocale(LC_ALL, "");


	/* Configure sched priority for better wake-up accuracy */
	memset(&schparam, 0, sizeof(schparam));
	schparam.sched_priority = opt_schprio;
	ret = sched_setscheduler(0, opt_schpolicy, &schparam);
	if (ret) {
		fprintf(stderr, "Error(%d) in setting priority(%d): %s\n",
			errno, opt_schprio, strerror(errno));
		goto out;
	}

	tx_only_all();

out:
	benchmark_done = true;

	xdpsock_cleanup();

	munmap(bufs, NUM_FRAMES * opt_xsk_frame_size);

	return 0;
}
