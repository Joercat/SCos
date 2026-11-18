/* SCos native - what the network layer offers the rest of the kernel.
 *
 * The header is deliberately free of scos.h: the stack below it is exercised by a host harness that compiles
 * net.c against a model of a card (tools/tests/net_host.c), and a header that pulled in the whole desktop would
 * drag the compositor into that harness for no reason.  Everything here is plain fixed-width integers and
 * borrowed C types, so the same declarations compile in the kernel and on the host.
 */
#ifndef SCOS_NET_H
#define SCOS_NET_H

#include <stddef.h>
#include <stdint.h>

#define NET_ALEN        6
#define NET_MTU         1500
#define NET_FRAME_MAX   (NET_MTU + 32)   /* an Ethernet frame this stack will build or accept */
/* How many frames one poll may take out of a driver before returning.  Bounded so a flooded card cannot keep
 * the compositor out of its own loop: the WM calls net_poll() once a frame, and a poll that never returns is a
 * frozen desktop. */
#define NET_RX_BURST    16

/* One adapter, as a driver hands it over.  The driver owns the rings and the registers; the stack owns every
 * byte above the Ethernet header.  `poll' drains the card's receive ring and calls net_rx_frame() for each
 * frame; `transmit' puts one frame on the wire and returns 1 when the card took it.  Nothing here is called
 * from an interrupt: this OS drives the card from the foreground, which is the same rule USB follows here. */
struct net_nic {
    const char *name;                            /* "rtl8139", "rtl8168", ... for the report */
    void *priv;                                  /* the driver's own state */
    int  (*poll)(struct net_nic *);
    int  (*transmit)(struct net_nic *, const void *frame, size_t len);
    uint8_t mac[NET_ALEN];
    int  link;                                   /* 1 when the PHY reports a link */
    uint32_t speed_mbps;
};

/* What the panel and the Network window show.  Every field is a measurement or a state this stack set itself;
 * `state' is a number rather than a string so a test can assert a transition instead of parsing prose. */
enum {
    NET_STATE_ABSENT = 0,        /* no supported adapter was found */
    NET_STATE_DOWN,              /* adapter present, no link */
    NET_STATE_LINK,              /* link up, no address yet */
    NET_STATE_DHCP,              /* asking for a lease */
    NET_STATE_UP,                /* address configured */
    NET_STATE_FAILED             /* last configuration attempt failed; the counters say why */
};

enum {
    NET_LEASE_NONE = 0,
    NET_LEASE_OFFER,
    NET_LEASE_BOUND,
    NET_LEASE_FAILED
};

struct net_info {
    int present;                     /* a driver claimed an adapter */
    int state;                       /* NET_STATE_* */
    char driver[24];
    uint8_t mac[NET_ALEN];
    int link;
    uint32_t speed_mbps;
    int lease;                       /* NET_LEASE_* */
    uint32_t ip, mask, gateway, dns;
    uint32_t lease_seconds;
    uint32_t server;                 /* the DHCP server that handed the lease over */
    uint32_t rx_packets, tx_packets, rx_bytes, tx_bytes, dropped;
    uint32_t arp_requests, arp_replies, icmp_echo, udp_packets, tcp_packets;
    uint32_t arp_unsolicited;        /* replies for an address this stack never asked about, refused */
    uint32_t tcp_state;              /* 0 closed, 1 syn sent, 2 established, 3 closing */
    uint32_t frames_too_big;         /* frames a driver offered that no valid header could describe */
    uint16_t rx_errors;
};

/* Drivers call this once, after their rings are live.  The first adapter registered is the one the stack uses:
 * this OS has one address, one route and one resolver, and pretending otherwise would mean a routing table
 * nobody asked for.  Further adapters are still counted and reported, never used. */
int  net_register_nic(struct net_nic *nic);
int  net_nic_count(void);

/* Bring-up.  net_hardware_init() is the driver side (PCI scan and register programming); net_start() is the
 * policy side and begins address configuration.  Both are idempotent. */
void net_hardware_init(void);
void net_start(void);
void net_poll(void);                              /* foreground: drain the card, run the timers */
int  net_link_up(void);

/* Configuration.  DHCP is the default; a static address is available for machines with no server. */
void net_dhcp_renew(void);
int  net_set_static(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns);

/* Addresses, in host byte order (0x0a00020f is 10.0.2.15), the way every other number in this OS is. */
void net_get_info(struct net_info *out);
uint32_t net_ip(void);
uint32_t net_mask(void);
uint32_t net_gateway(void);
uint32_t net_dns(void);
const uint8_t *net_mac(void);

/* Text helpers the console and the panel share: dotted quad in and out, and the state in words. */
void net_format_ip(char *out, size_t cap, uint32_t ip);
int  net_parse_ip(const char *text, uint32_t *out);
const char *net_state_name(int state);
const char *net_lease_name(int lease);
/* A whole status paragraph, for the terminal, the TTY and the Network window.  Returns the number of bytes
 * written, which is always less than the capacity: a report that does not fit is cut and terminated rather than
 * overflowing the caller's buffer, and the cut is visible as a shorter string. */
size_t net_format_status(char *out, size_t cap);

/* The three probes this stack can run without a resolver being correct, each bounded in time and reported with
 * a measured result rather than a promise: how many milliseconds an echo took, what a name resolved to. */
int net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms);
int net_dns_lookup(const char *name, uint32_t *ip, uint32_t timeout_ms);

/* A single outbound TCP connection.  One at a time, because one is what a browser window needs and because two
 * would need the state machine to be a table rather than a struct.  Blocking calls poll while they wait. */
int  net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms);
int  net_tcp_send(const void *data, size_t len, uint32_t timeout_ms);
int  net_tcp_recv(void *buffer, size_t cap, uint32_t timeout_ms);   /* >0 bytes, 0 peer closed, <0 error */
void net_tcp_close(void);
int  net_tcp_open(void);

/* One HTTP/1.0 GET, because there is no TLS in this kernel and a plain-text fetch is the honest limit of what
 * "network support" can demonstrate today.  Returns 1 and the response (status line, headers and body) with
 * `*len' set, 0 on a refused or failed fetch. */
int net_http_get(const char *url, char *response, size_t cap, size_t *len);

/* Diagnostics entry points the host harness drives: the receive path, one frame at a time, and the timer. */
void net_rx_frame(const void *frame, size_t len);
void net_timers(void);
uint64_t net_now_ms(void);
uint32_t net_checksum(const void *data, size_t len, uint32_t sum);

#endif /* SCOS_NET_H */
