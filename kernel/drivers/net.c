/* SCos native - IPv4, ARP, ICMP, UDP, DHCP, DNS and a TCP client.
 *
 * What this is: the smallest network stack that can put a real page on the screen and be read afterwards.  It
 * owns everything above the Ethernet header and nothing below it - a driver hands over frames and takes frames,
 * and knows nothing about addresses.  That seam is where it is because the two halves fail differently: a card
 * that cannot transmit is a register problem with a register answer, while an address that cannot be leased is
 * a protocol problem with a packet answer, and a stack that mixed them would have nothing to say about either.
 *
 * What this deliberately is not: a router, a server, a fragment reassembler, or anything with threads.  This
 * kernel runs one foreground context and drives every device from it, and the stack follows that rule - there
 * is no interrupt handler here, and net_poll() is the whole of its I/O.  A blocking call is a loop that polls
 * and sleeps, bounded in milliseconds by the PIT tick, so the numbers in its report are wall-clock numbers.
 *
 * The receive path treats every packet as hostile, because everything on a wire is:
 *  - lengths are checked against the frame the driver reported before a header is read;
 *  - the IP header length and total length are checked against each other and against the frame, and the
 *    header checksum is verified, so a truncated or malformed header is dropped before it is believed;
 *  - fragments are dropped rather than reassembled (no reassembly means no reassembly bug);
 *  - each option and each DNS label is bounds-checked against the packet that carries it, and every loop over
 *    a count in a packet is also bounded by the bytes remaining;
 *  - a reply is only accepted from the address and port the request went to, with the identifier the request
 *    used, so an off-path packet cannot be mistaken for an answer.
 * The counters in net_info count what happened to those rules; frames_too_big and rx_errors are the two that
 * make a silent drop visible.
 */
#include "scos.h"
#include "net.h"

#define ETH_HLEN        14
static const uint8_t broadcast_mac[NET_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
static int dhcp_send(uint8_t kind, uint32_t requested);
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806
#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17
#define IP_PROTO_TCP    6

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68

/* The minimum size of the structures this file parses, named so a bounds check reads as one. */
#define BOOTP_FIXED     236
#define DHCP_OPTIONS    64
#define DNS_MAX_NAME    255
#define TCP_MSS         1460          /* what a 1500-byte Ethernet payload leaves after IPv4 and TCP */
#define ARP_CACHE       8
#define ARP_TTL_MS      120000u
#define ARP_RETRY_MS    1000u
#define DHCP_RETRY_MS   3000u
#define DHCP_TRIES      4
#define DNS_RETRY_MS    1500u
#define DNS_TRIES       3
#define TCP_RETRY_MS    600u
#define TCP_TRIES       (3000u / TCP_RETRY_MS)
#define NET_STATIC_SEED 0x53434f53u   /* IP identifiers and ports start somewhere that is not zero */

static struct net_nic *iface;
static int nic_count;
static uint32_t local_ip, local_mask, local_gw, local_dns;
static uint8_t local_mac[NET_ALEN];
static uint16_t ip_ident = (uint16_t)NET_STATIC_SEED;

static struct net_info counters;

struct arp_entry {
    uint32_t ip;
    uint8_t mac[NET_ALEN];
    int valid;
    uint64_t seen, asked;
};
static struct arp_entry arp[ARP_CACHE];

struct dhcp_client {
    int active;
    int lease;
    uint32_t xid;
    uint32_t offered, server, mask, gw, dns, lease_seconds;
    uint64_t sent;
    unsigned tries;
};
static struct dhcp_client dhcp;

struct ping_job {
    int active;
    uint32_t target;
    uint16_t ident;
    uint64_t sent;
    unsigned tries;
    uint32_t rtt;
    int answered;
};
static struct ping_job pinger;

struct dns_job {
    int active;
    uint16_t ident;
    uint16_t port;
    char name[DNS_MAX_NAME + 1];
    uint64_t sent;
    unsigned tries;
    uint32_t answer;
    int answered;
};
static struct dns_job dns_job_state;

enum { TCP_CLOSED = 0, TCP_SYN_SENT, TCP_ESTABLISHED, TCP_FIN_WAIT, TCP_PEER_CLOSED };

struct tcp_conn {
    int state;
    uint32_t raddr;
    uint16_t rport, lport;
    uint32_t iss, snd_una, snd_nxt;
    uint32_t rcv_nxt;
    uint8_t tx[TCP_MSS];
    size_t tx_len;
    uint64_t tx_sent;
    unsigned tx_tries;
    uint8_t rx[TCP_MSS];
    size_t rx_len;
    int peer_fin;
};
static struct tcp_conn tcp;
void net_tcp_receive(uint32_t src, const uint8_t *segment, size_t len);

/* ------------------------------------------------------------ byte order and checksums ------------------------------ */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

/* One's-complement sum over bytes, seeded, as RFC 1071 defines it.  Returned unfolded so a caller can add a
 * pseudo-header before folding: that is what makes UDP and TCP checksums use the same primitive. */
uint32_t net_checksum(const void *data, size_t len, uint32_t sum)
{
    const uint8_t *p = data;

    while (len > 1) { sum += ((uint32_t)p[0] << 8) | p[1]; p += 2; len -= 2; }
    if (len) sum += (uint32_t)p[0] << 8;
    return sum;
}

static uint16_t csum_fold(uint32_t sum)
{
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return (uint16_t)~sum;
}

static uint16_t ip_csum(const uint8_t *header)
{
    return csum_fold(net_checksum(header, 20, 0));
}

static uint32_t pseudo_sum(uint32_t src, uint32_t dst, uint8_t proto, uint16_t length)
{
    uint8_t pseudo[12];
    wr32(pseudo, src);
    wr32(pseudo + 4, dst);
    pseudo[8] = 0;
    pseudo[9] = proto;
    wr16(pseudo + 10, length);
    return net_checksum(pseudo, sizeof pseudo, 0);
}

/* ------------------------------------------------------------ unpredictable numbers -------------------------------- */

/* There is no random-number generator in this kernel, and the identifiers below must not be predictable: a lease
 * transaction ID, a resolver query ID, an initial sequence number and an ephemeral port are all things an
 * off-path attacker can otherwise guess and race - a forged ACK, a forged DNS answer, an injected segment.  The
 * mix takes what an attacker cannot see from outside this machine: the address the linker gave a static object
 * (`-fpie`, so it differs per boot), the tick at the time of the request, and an accumulating state.  It is not a
 * cryptographic RNG, and the file does not pretend it is one; it is enough that the values are not a fixed seed
 * plus a clock a listener can read off the wire. */
static uint32_t net_entropy(void)
{
    static uint32_t state = NET_STATIC_SEED;
    uintptr_t address = (uintptr_t)&state;

    state ^= (uint32_t)address ^ ((uint32_t)(address >> 17) * 2654435761u) ^ (uint32_t)net_now_ms();
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* An ephemeral local port, drawn from the same mix: a fixed starting port and a counter is a port an attacker
 * can predict and send resets to before the connection exists.  20000 and up, which is where this stack has
 * always started, and never zero. */
static uint16_t net_sport(void)
{
    uint16_t port = (uint16_t)(20000u + (net_entropy() % 40000u));
    return port ? port : 20000u;
}

/* ------------------------------------------------------------ interface bookkeeping ------------------------------- */

int net_nic_count(void) { return nic_count; }

int net_register_nic(struct net_nic *nic)
{
    if (!nic || !nic->transmit) return 0;
    nic_count++;
    if (iface) return 1;                    /* counted, reported, not routed: one address is one interface */
    iface = nic;
    memcpy(local_mac, nic->mac, NET_ALEN);
    memcpy(counters.mac, nic->mac, NET_ALEN);
    counters.present = 1;
    counters.state = nic->link ? NET_STATE_LINK : NET_STATE_DOWN;
    counters.link = nic->link;
    counters.speed_mbps = nic->speed_mbps;
    if (nic->name) {
        size_t n = strlen(nic->name);
        if (n > sizeof counters.driver - 1) n = sizeof counters.driver - 1;
        memcpy(counters.driver, nic->name, n);
        counters.driver[n] = 0;
    }
    return 1;
}

int net_link_up(void) { return iface && iface->link; }

void net_get_info(struct net_info *out)
{
    if (!out) return;
    if (iface) {
        counters.link = iface->link;
        counters.speed_mbps = iface->speed_mbps;
    }
    counters.ip = local_ip;
    counters.mask = local_mask;
    counters.gateway = local_gw;
    counters.dns = local_dns;
    counters.lease = dhcp.lease;
    counters.lease_seconds = dhcp.lease_seconds;
    counters.server = dhcp.server;
    counters.tcp_state = (uint32_t)tcp.state;
    if (!iface) counters.state = NET_STATE_ABSENT;
    else if (!iface->link) counters.state = NET_STATE_DOWN;
    else if (local_ip) counters.state = NET_STATE_UP;
    else if (dhcp.lease == NET_LEASE_FAILED) counters.state = NET_STATE_FAILED;
    else if (dhcp.active) counters.state = NET_STATE_DHCP;
    else counters.state = NET_STATE_LINK;
    *out = counters;
}

uint32_t net_ip(void) { return local_ip; }
uint32_t net_mask(void) { return local_mask; }
uint32_t net_gateway(void) { return local_gw; }
uint32_t net_dns(void) { return local_dns; }
uint64_t net_now_ms(void) { return (uint64_t)timer_ticks * 10u; }

/* ------------------------------------------------------------ addresses as text ------------------------------------ */

void net_format_ip(char *out, size_t cap, uint32_t ip)
{
    char text[16];
    size_t n = 0;

    if (!out || !cap) return;
    for (int i = 3; i >= 0; i--) {
        unsigned v = (ip >> (i * 8)) & 0xffu;
        char digits[3];
        unsigned d = 0;
        do { digits[d++] = (char)('0' + v % 10u); v /= 10u; } while (v && d < sizeof digits);
        while (d && n + d + 1 < sizeof text) text[n++] = digits[--d];
        if (i && n + 1 < sizeof text) text[n++] = '.';
    }
    text[n] = 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, text, n);
    out[n] = 0;
}

int net_parse_ip(const char *text, uint32_t *out)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int index = 0, digits = 0;

    if (!text || !out) return 0;
    for (const char *p = text; *p; p++) {
        if (*p >= '0' && *p <= '9') {
            if (digits >= 3) return 0;
            parts[index] = parts[index] * 10u + (uint32_t)(*p - '0');
            if (parts[index] > 255) return 0;
            digits++;
        } else if (*p == '.') {
            if (!digits || index == 3) return 0;
            index++;
            digits = 0;
        } else {
            return 0;
        }
    }
    if (index != 3 || !digits) return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

const char *net_state_name(int state)
{
    switch (state) {
        case NET_STATE_ABSENT: return "no supported adapter";
        case NET_STATE_DOWN:   return "adapter present, no link";
        case NET_STATE_LINK:   return "link up, no address";
        case NET_STATE_DHCP:   return "asking for a lease";
        case NET_STATE_UP:     return "up";
        case NET_STATE_FAILED: return "configuration failed";
        default:               return "unknown";
    }
}

const char *net_lease_name(int lease)
{
    switch (lease) {
        case NET_LEASE_NONE:   return "none";
        case NET_LEASE_OFFER:  return "offered";
        case NET_LEASE_BOUND:  return "bound";
        case NET_LEASE_FAILED: return "failed";
        default:               return "unknown";
    }
}

/* ------------------------------------------------------------ transmit --------------------------------------------- */

static int eth_send(const uint8_t *dst, uint16_t type, const void *payload, size_t len)
{
    static uint8_t frame[NET_FRAME_MAX];
    uint8_t *p = frame;
    size_t need = ETH_HLEN + len;

    if (!iface || !iface->transmit || !dst) return 0;
    if (need > sizeof frame) { counters.frames_too_big++; return 0; }
    memcpy(p, dst, NET_ALEN);
    memcpy(p + 6, local_mac, NET_ALEN);
    wr16(p + 12, type);
    if (len) memcpy(p + ETH_HLEN, payload, len);
    memset(p + need, 0, need < 60 ? 60 - need : 0);        /* a runt is padded on the wire, not in the counters */
    if (!iface->transmit(iface, p, need < 60 ? 60 : need)) { counters.dropped++; return 0; }
    counters.tx_packets++;
    counters.tx_bytes += (uint32_t)need;
    return 1;
}

/* ------------------------------------------------------------ ARP --------------------------------------------------- */

static struct arp_entry *arp_slot(void)
{
    struct arp_entry *oldest = &arp[0];
    for (int i = 0; i < ARP_CACHE; i++) {
        if (!arp[i].valid) return &arp[i];
        if (arp[i].seen < oldest->seen) oldest = &arp[i];
    }
    return oldest;
}

static struct arp_entry *arp_find(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE; i++)
        if (arp[i].valid && arp[i].ip == ip && net_now_ms() - arp[i].seen < ARP_TTL_MS) return &arp[i];
    return 0;
}

/* An entry this stack is waiting for an answer about: asked, not yet answered, and asked recently enough that
 * the answer can still belong to that question.  This is the only way an unanswered address can be filled in,
 * which is what keeps an unsolicited frame from writing the cache. */
static struct arp_entry *arp_pending(uint32_t ip)
{
    uint64_t now = net_now_ms();

    for (int i = 0; i < ARP_CACHE; i++) {
        if (arp[i].ip != ip) continue;
        if (arp[i].valid && now - arp[i].seen < ARP_TTL_MS) return &arp[i];
        if (!arp[i].valid && now - arp[i].asked < ARP_RETRY_MS) return &arp[i];
    }
    return 0;
}

/* Forget every address this stack learned.  A lease that changed, or one a server has taken back, makes every
 * cached answer a statement about somebody else's address. */
static void arp_flush(void)
{
    memset(arp, 0, sizeof arp);
}

static int arp_request(uint32_t ip)
{
    static const uint8_t broadcast[NET_ALEN] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    uint8_t packet[28];
    struct arp_entry *slot = arp_find(ip);
    uint64_t now = net_now_ms();

    if (slot && now - slot->asked < ARP_RETRY_MS) return 1;    /* one request a second per address */
    wr16(packet, 1);                     /* Ethernet */
    wr16(packet + 2, ETH_TYPE_IP);
    packet[4] = NET_ALEN;                /* hardware length */
    packet[5] = 4;                       /* protocol length */
    wr16(packet + 6, 1);                 /* request */
    memcpy(packet + 8, local_mac, NET_ALEN);
    wr32(packet + 14, local_ip);
    memset(packet + 18, 0, NET_ALEN);
    wr32(packet + 24, ip);
    if (!eth_send(broadcast, ETH_TYPE_ARP, packet, sizeof packet)) return 0;
    counters.arp_requests++;
    if (!slot) slot = arp_slot();
    slot->ip = ip;
    slot->valid = 0;                     /* asked, not yet answered */
    slot->asked = now;
    slot->seen = now;
    return 1;
}

static int arp_resolve(uint32_t ip, uint8_t *out)
{
    struct arp_entry *slot = arp_find(ip);
    if (!slot) { arp_request(ip); return 0; }
    memcpy(out, slot->mac, NET_ALEN);
    return 1;
}

static void arp_receive(const uint8_t *packet, size_t len)
{
    uint16_t op;
    uint32_t sender_ip, target_ip;
    struct arp_entry *slot;

    if (len < 28) return;
    if (rd16(packet) != 1 || rd16(packet + 2) != ETH_TYPE_IP || packet[4] != NET_ALEN || packet[5] != 4)
        return;                                        /* not Ethernet/IPv4: nothing here speaks it */
    op = rd16(packet + 6);
    sender_ip = rd32(packet + 14);
    target_ip = rd32(packet + 24);
    if (op == 1 && target_ip == local_ip && local_ip) {
        /* A request for us: answer it from the frame the driver handed over, so the reply's addressing is the
         * request's own rather than anything this stack assumed. */
        static uint8_t reply[28];
        uint8_t peer[NET_ALEN];
        memcpy(peer, packet + 8, NET_ALEN);
        wr16(reply, 1);
        wr16(reply + 2, ETH_TYPE_IP);
        reply[4] = NET_ALEN;
        reply[5] = 4;
        wr16(reply + 6, 2);
        memcpy(reply + 8, local_mac, NET_ALEN);
        wr32(reply + 14, local_ip);
        memcpy(reply + 18, peer, NET_ALEN);
        wr32(reply + 24, sender_ip);
        (void)eth_send(peer, ETH_TYPE_ARP, reply, sizeof reply);
    }
    if (op != 1 && op != 2) return;
    /* Only an exchange this stack started may write the cache.  A reply for an address nobody asked about is a
     * frame any machine on the segment can send at any time - the classic way to own the gateway entry before the
     * real answer arrives - and a request from a stranger is not evidence about the stranger's own address
     * either.  So: a reply must match an entry that is still being resolved, and a request only refreshes an
     * entry that is already trusted. */
    slot = arp_pending(sender_ip);
    if (!slot) {
        if (op == 2) counters.arp_unsolicited++;
        return;
    }
    memcpy(slot->mac, packet + 8, NET_ALEN);
    slot->valid = 1;
    slot->seen = net_now_ms();
    if (op == 2) counters.arp_replies++;
}

/* ------------------------------------------------------------ IPv4 -------------------------------------------------- */

/* The next hop for an address: itself on-link, the gateway otherwise.  A zero address asks the caller to try
 * later - which is what a stack with no lease says instead of sending a packet from 0.0.0.0 by accident. */
static int next_hop(uint32_t dst, uint32_t *hop)
{
    if (!local_ip) return 0;
    if ((dst & local_mask) == (local_ip & local_mask)) { *hop = dst; return 1; }
    if (!local_gw) return 0;
    *hop = local_gw;
    return 1;
}

static int ip_send_ex(uint32_t src, uint32_t dst, uint8_t proto, const void *payload, size_t len, int broadcast)
{
    static uint8_t packet[20 + NET_MTU];
    uint8_t mac[NET_ALEN];

    if (!iface || len + 20 > sizeof packet) { counters.frames_too_big++; return 0; }
    if (broadcast) {
        memcpy(mac, broadcast_mac, NET_ALEN);
    } else {
        uint32_t hop;
        if (!next_hop(dst, &hop)) return 0;
        if (!arp_resolve(hop, mac)) return 0;
    }
    packet[0] = 0x45;                                   /* IPv4, five words of header */
    packet[1] = 0;
    wr16(packet + 2, (uint16_t)(20 + len));
    wr16(packet + 4, ip_ident++);
    wr16(packet + 6, 0x4000);                           /* don't fragment: this stack never reassembles */
    packet[8] = 64;
    packet[9] = proto;
    wr16(packet + 10, 0);
    wr32(packet + 12, src);
    wr32(packet + 16, dst);
    wr16(packet + 10, ip_csum(packet));
    if (len) memcpy(packet + 20, payload, len);
    return eth_send(mac, ETH_TYPE_IP, packet, 20 + len);
}

static int ip_send(uint32_t dst, uint8_t proto, const void *payload, size_t len)
{
    return ip_send_ex(local_ip, dst, proto, payload, len, 0);
}

static void icmp_receive(uint32_t src, const uint8_t *packet, size_t len)
{
    uint8_t type, code;

    if (len < 8) return;
    /* The checksum is the ICMP message's own, and it is the only thing that says the bytes are the bytes the
     * sender meant: the IP header's checksum covers the header alone. */
    if (csum_fold(net_checksum(packet, len, 0)) != 0) { counters.rx_errors++; return; }
    type = packet[0];
    code = packet[1];
    if (type == 8 && code == 0) {
        /* Echo request.  Answer with the request's own identifier and sequence, and its payload, which is what
         * makes this stack visible to another machine's ping. */
        static uint8_t reply[NET_MTU];
        size_t bytes = len > sizeof reply ? sizeof reply : len;
        memcpy(reply, packet, bytes);
        reply[0] = 0;
        reply[1] = 0;
        wr16(reply + 2, 0);
        memset(reply + 4, 0, 4);
        wr16(reply + 2, csum_fold(net_checksum(reply, bytes, 0)));
        (void)ip_send(src, IP_PROTO_ICMP, reply, bytes);
        counters.icmp_echo++;
        return;
    }
    if (type == 0 && code == 0) {
        if (pinger.active && src == pinger.target && len >= 8 && rd16(packet + 4) == pinger.ident) {
            pinger.rtt = (uint32_t)(net_now_ms() - pinger.sent);
            pinger.answered = 1;
            counters.icmp_echo++;
        }
    }
}

static void udp_receive(uint32_t src, uint32_t dst, const uint8_t *packet, size_t len);

static void ip_receive(const void *frame, size_t len)
{
    const uint8_t *packet = frame;
    size_t header, total;
    uint32_t src, dst;

    if (len < 14 + 20) { counters.rx_errors++; return; }
    if ((packet[14] >> 4) != 4) return;
    header = (size_t)(packet[14] & 0x0fu) * 4u;
    if (header < 20 || header > len - 14) return;
    total = rd16(packet + 16);
    if (total < header || total > len - 14) { counters.rx_errors++; return; }
    if (csum_fold(net_checksum(packet + 14, header, 0)) != 0) { counters.rx_errors++; return; }
    if (rd16(packet + 20) & 0x1fffu) return;                 /* a fragment: dropped, never reassembled */
    src = rd32(packet + 26);
    dst = rd32(packet + 30);
    if (local_ip && dst != local_ip && dst != 0xffffffffu) return;
    /* An echo request addressed to a broadcast address is answered by every machine that hears it, from the
     * address of whoever it names: that is a reflector, not a ping. */
    if (dst == 0xffffffffu && packet[14 + 9] == IP_PROTO_ICMP) return;
    packet += 14;
    switch (packet[9]) {
        case IP_PROTO_ICMP:
            icmp_receive(src, packet + header, total - header);
            break;
        case IP_PROTO_UDP:
            udp_receive(src, dst, packet + header, total - header);
            break;
        case IP_PROTO_TCP:
            net_tcp_receive(src, packet + header, total - header);
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------ UDP --------------------------------------------------- */

static int udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const void *payload, size_t len)
{
    uint8_t packet[8 + NET_MTU];
    uint32_t sum;
    int broadcast = dst == 0xffffffffu;
    uint32_t src = broadcast ? 0u : local_ip;

    if (len + 8 > sizeof packet) { counters.frames_too_big++; return 0; }
    wr16(packet, sport);
    wr16(packet + 2, dport);
    wr16(packet + 4, (uint16_t)(8 + len));
    wr16(packet + 6, 0);
    if (len) memcpy(packet + 8, payload, len);
    sum = pseudo_sum(src, dst, IP_PROTO_UDP, (uint16_t)(8 + len));
    sum = net_checksum(packet, 8 + len, sum);
    wr16(packet + 6, csum_fold(sum));
    return ip_send_ex(src, dst, IP_PROTO_UDP, packet, 8 + len, broadcast);
}

/* ------------------------------------------------------------ DHCP -------------------------------------------------- */

static const uint8_t dhcp_magic[4] = { 99, 130, 83, 99 };

void net_dhcp_renew(void)
{
    dhcp.active = 1;
    dhcp.lease = NET_LEASE_NONE;
    dhcp.offered = 0;
    dhcp.server = 0;
    dhcp.tries = 0;
    dhcp.sent = 0;
    dhcp.xid = net_entropy();
    local_ip = 0;
    (void)dhcp_send(1, 0);                     /* DISCOVER: the first thing a machine with no address says */
    dhcp.sent = net_now_ms();
}

static int dhcp_send(uint8_t kind, uint32_t requested)
{
    static uint8_t packet[BOOTP_FIXED + DHCP_OPTIONS];
    size_t at = BOOTP_FIXED;
    /* A REQUEST is unicast to the server that made the offer - but only once this machine has an address to
     * send it from.  With no address there is no route, so the REQUEST is broadcast like everything else, which
     * is what every client does on a fresh lease and the only form a server can answer. */
    uint32_t target = (kind == 3 && dhcp.server && local_ip) ? dhcp.server : 0xffffffffu;

    memset(packet, 0, sizeof packet);
    packet[0] = 1;                                  /* request */
    packet[1] = 1;                                  /* Ethernet */
    packet[2] = NET_ALEN;
    packet[3] = 0;
    wr32(packet + 4, dhcp.xid);
    wr16(packet + 8, 0);
    wr16(packet + 10, 0x8000);                      /* broadcast: no address yet, so no unicast can arrive */
    memcpy(packet + 28, local_mac, NET_ALEN);
    memcpy(packet + BOOTP_FIXED, dhcp_magic, 4);
    at += 4;
    packet[at++] = 53; packet[at++] = 1; packet[at++] = kind;
    packet[at++] = 55; packet[at++] = 4; packet[at++] = 1; packet[at++] = 3; packet[at++] = 6; packet[at++] = 51;
    packet[at++] = 61; packet[at++] = 7; packet[at++] = 1; memcpy(packet + at, local_mac, NET_ALEN); at += 7;
    if (kind == 3 && requested) {                   /* REQUEST names the lease it wants */
        packet[at++] = 50; packet[at++] = 4;
        wr32(packet + at, requested);
        at += 4;
        if (dhcp.server) {
            packet[at++] = 54; packet[at++] = 4;
            wr32(packet + at, dhcp.server);
            at += 4;
        }
    }
    packet[at++] = 255;
    return udp_send(target, DHCP_CLIENT_PORT, DHCP_SERVER_PORT, packet, at);
}

static void dhcp_receive(uint32_t src, const uint8_t *packet, size_t len)
{
    uint8_t kind = 0;
    uint32_t offered = 0, server = 0, mask = 0, gw = 0, dns = 0, lease = 0;
    size_t at;
    int have_end = 0;

    if (len < BOOTP_FIXED + 4) return;
    if (packet[0] != 2 || packet[1] != 1 || packet[2] != NET_ALEN) return;
    if (rd32(packet + 4) != dhcp.xid) {
        /* The transaction ID decides whether an answer is to this machine's question, except where the answer
         * *is* about this machine's address: a NAK for the address in use is a server taking the lease back,
         * and it belongs to no request this stack still has a record of. */
        if (!(local_ip && rd32(packet + 16) == local_ip)) return;
    }
    if (memcmp(packet + BOOTP_FIXED, dhcp_magic, 4) != 0) { counters.rx_errors++; return; }
    offered = rd32(packet + 16);                                 /* yiaddr */
    at = BOOTP_FIXED + 4;
    while (at < len) {
        uint8_t option = packet[at++];
        uint8_t size;
        if (option == 255) { have_end = 1; break; }
        if (option == 0) continue;
        if (at >= len) break;
        size = packet[at++];
        if ((size_t)size > len - at) break;                      /* a length that runs past the packet */
        switch (option) {
            case 53:
                if (size >= 1) kind = packet[at];
                break;
            case 1:
                if (size >= 4) mask = rd32(packet + at);
                break;
            case 3:
                if (size >= 4) gw = rd32(packet + at);
                break;
            case 6:
                if (size >= 4) dns = rd32(packet + at);
                break;
            case 51:
                if (size >= 4) lease = rd32(packet + at);
                break;
            case 54:
                if (size >= 4) server = rd32(packet + at);
                break;
            default:
                break;
        }
        at += size;
    }
    if (!have_end) return;                                       /* truncated: treated as damaged, not as an offer */
    /* A server this stack is already talking to is the only machine allowed to finish the transaction, and a
     * request names the address it wants: an ACK for a different address than the one asked for is a machine
     * trying to move this one onto an address it never agreed to. */
    if (!dhcp.active && kind != 6) return;                       /* no transaction is open for anything else */
    if (dhcp.server && src != dhcp.server && (kind == 2 || kind == 5 || kind == 6)) return;
    if (kind == 5 && dhcp.offered && offered != dhcp.offered) return;
    if (kind == 2 && offered) {                                  /* OFFER */
        dhcp.offered = offered;
        dhcp.server = server;
        dhcp.mask = mask;
        dhcp.gw = gw;
        dhcp.dns = dns;
        dhcp.lease_seconds = lease;
        dhcp.lease = NET_LEASE_OFFER;
        (void)dhcp_send(3, offered);
        dhcp.sent = net_now_ms();
        return;
    }
    if (kind == 5) {                                             /* ACK */
        local_ip = offered;
        local_mask = mask ? mask : 0xffffff00u;
        local_gw = gw;
        local_dns = dns ? dns : gw;
        dhcp.lease = NET_LEASE_BOUND;
        dhcp.lease_seconds = lease;
        dhcp.active = 0;
        counters.state = NET_STATE_UP;
        return;
    }
    if (kind == 6) {                                             /* NAK: the lease is not this machine's */
        /* A NAK reaches a machine two ways: while a request is outstanding (the server will not give the
         * address after all) and while a lease is already in use (a server that has reassigned the address, or
         * never had it).  The second is the dangerous one: keeping an address the network has just told this
         * machine it does not own keeps answering ARP for it, and every frame the real owner sends collides.
         * So the address, everything cached about it and the connection that used it go, and a DISCOVER
         * starts again from the top - with a new transaction ID, because the old transaction is dead. */
        if (local_ip) {
            local_ip = local_mask = local_gw = local_dns = 0;
            arp_flush();
            tcp.state = TCP_CLOSED;
            counters.state = NET_STATE_DOWN;
        }
        net_dhcp_renew();
        return;
    }
}

/* ------------------------------------------------------------ DNS --------------------------------------------------- */

static int dns_encode(const char *name, uint8_t *out, size_t cap, size_t *used)
{
    size_t at = 0;
    const char *label = name;

    if (!name || !*name) return 0;
    while (*label) {
        const char *dot = label;
        size_t len;
        while (*dot && *dot != '.') dot++;
        len = (size_t)(dot - label);
        if (!len || len > 63) return 0;
        if (at + 1 + len >= cap) return 0;
        out[at++] = (uint8_t)len;
        memcpy(out + at, label, len);
        at += len;
        if (*dot == '.') {
            label = dot + 1;
            if (!*label) break;                 /* a trailing dot is a legal, empty root label */
        } else {
            break;
        }
    }
    if (at + 1 > cap) return 0;
    out[at++] = 0;
    *used = at;
    return 1;
}

/* Skips one (possibly compressed) name inside a message and returns the offset after it, or 0 when the name
 * runs past the message.  Compression is followed only as far as a bound, so a loop of pointers is a refusal. */
static size_t dns_skip_name(const uint8_t *message, size_t len, size_t at)
{
    size_t after = 0;                            /* where the name ends in the *original* position, once a
                                                  * compression pointer has been followed */

    for (unsigned jumps = 0; jumps < 32; jumps++) {
        if (at >= len) return 0;
        uint8_t c = message[at];
        if (c == 0) return after ? after : at + 1;
        if ((c & 0xc0) == 0xc0) {
            if (at + 1 >= len) return 0;
            if (!after) after = at + 2;           /* two bytes of pointer, wherever they point */
            {
                size_t target = (size_t)(((c & 0x3fu) << 8) | message[at + 1]);
                if (target >= len) return 0;
                at = target;
            }
            continue;
        }
        if ((c & 0xc0) != 0) return 0;
        if (at + 1 + c > len) return 0;
        at += 1 + c;
    }
    return 0;
}

/* True when the (possibly compressed) name at `at` is exactly the encoded name `want`.  Labels are compared
 * without regard to case, which is what the protocol says they mean, and compression is expanded so an answer
 * that points back at the question matches.  The jump bound is the same as dns_skip_name's, so a loop of
 * pointers is a refusal rather than a walk. */
static int dns_name_is(const uint8_t *message, size_t len, size_t at, const uint8_t *want, size_t want_len)
{
    size_t used = 0;

    for (unsigned jumps = 0; jumps < 32; jumps++) {
        if (at >= len) return 0;
        {
            uint8_t c = message[at];
            if (c == 0) return used + 1 == want_len && want[used] == 0;
            if ((c & 0xc0) == 0xc0) {
                if (at + 1 >= len) return 0;
                at = (size_t)(((c & 0x3fu) << 8) | message[at + 1]);
                if (at >= len) return 0;
                continue;
            }
            if ((c & 0xc0) != 0 || c + 1u > want_len - used || at + 1u + c > len) return 0;
            if (want[used] != c) return 0;
            for (size_t i = 0; i < c; i++) {
                uint8_t a = message[at + 1 + i], b = want[used + 1 + i];
                if (a >= 'A' && a <= 'Z') a = (uint8_t)(a + 32);
                if (b >= 'A' && b <= 'Z') b = (uint8_t)(b + 32);
                if (a != b) return 0;
            }
            used += 1 + c;
            at += 1 + c;
        }
    }
    return 0;
}

/* Copies the (possibly compressed) name at `at` out in its uncompressed form, which is what the CNAME chain of
 * an answer has to be compared as.  Bounded by the same jump count. */
static int dns_name_copy(const uint8_t *message, size_t len, size_t at, uint8_t *out, size_t cap, size_t *used)
{
    size_t w = 0;

    for (unsigned jumps = 0; jumps < 32; jumps++) {
        if (at >= len || w + 1 >= cap) return 0;
        {
            uint8_t c = message[at];
            if (c == 0) { out[w++] = 0; *used = w; return 1; }
            if ((c & 0xc0) == 0xc0) {
                if (at + 1 >= len) return 0;
                at = (size_t)(((c & 0x3fu) << 8) | message[at + 1]);
                if (at >= len) return 0;
                continue;
            }
            if ((c & 0xc0) != 0 || w + 1 + c >= cap || at + 1u + c > len) return 0;
            out[w++] = c;
            memcpy(out + w, message + at + 1, c);
            w += c;
            at += 1u + c;
        }
    }
    return 0;
}

static int dns_send_query(void)
{
    uint8_t packet[12 + DNS_MAX_NAME + 2 * 4 + 8];
    size_t at = 12, qlen;

    memset(packet, 0, 12);
    wr16(packet, dns_job_state.ident);
    wr16(packet + 2, 0x0100);                 /* standard query, recursion desired */
    wr16(packet + 4, 1);                      /* one question */
    if (!dns_encode(dns_job_state.name, packet + at, sizeof packet - at - 4, &qlen)) return 0;
    at += qlen;
    wr16(packet + at, 1);                     /* type A */
    wr16(packet + at + 2, 1);                 /* class IN */
    at += 4;
    return udp_send(local_dns, dns_job_state.port, 53, packet, at);
}

/* Send, polling, until the frame is actually on the wire.  The first attempt at any new destination cannot go
 * out: its MAC is not known yet, and the ARP answer only arrives through net_poll().  A stack that treated that
 * as a lost message would fail every first exchange with a host it had not spoken to recently. */
static int dns_send_retry(uint32_t budget_ms)
{
    uint64_t deadline = net_now_ms() + budget_ms;

    for (;;) {
        if (dns_send_query()) return 1;
        if (net_now_ms() >= deadline) return 0;
        net_poll();
        sleep_ms(1);
    }
}

static void dns_receive(uint32_t src, uint16_t sport, const uint8_t *packet, size_t len)
{
    size_t at, want_len = 0, chain_len = 0, name_at;
    unsigned questions, answers;
    uint8_t want[DNS_MAX_NAME + 2], chain[DNS_MAX_NAME + 2];

    if (!dns_job_state.active || len < 12) return;
    if (src != local_dns || sport != 53) return;              /* only the resolver we asked */
    if (rd16(packet) != dns_job_state.ident) return;
    if ((rd16(packet + 2) & 0x8000u) == 0) return;            /* not a response */
    questions = rd16(packet + 4);
    answers = rd16(packet + 6);
    if (!answers || answers > 32) return;
    /* The answer has to be about the name this stack asked about.  The transaction ID and the source port
     * already make an off-path forgery expensive, and neither of them travels in clear as part of the answer
     * itself; the name does, so this is the one check that ties the address in an answer to the name that was
     * asked for, whichever resolver happens to answer. */
    if (questions != 1) return;
    if (!dns_encode(dns_job_state.name, want, sizeof want, &want_len)) return;
    at = dns_skip_name(packet, len, 12);
    if (!at || at + 4 > len) return;
    if (!dns_name_is(packet, len, 12, want, want_len)) return;
    at += 4;
    memcpy(chain, want, want_len);
    chain_len = want_len;
    for (unsigned i = 0; i < answers; i++) {
        uint16_t type, klass, rdlength;
        int mine;

        name_at = at;
        at = dns_skip_name(packet, len, at);
        if (!at || at + 10 > len) return;
        mine = dns_name_is(packet, len, name_at, chain, chain_len);
        type = rd16(packet + at);
        klass = rd16(packet + at + 2);
        rdlength = rd16(packet + at + 8);
        at += 10;
        if (at + rdlength > len) return;
        if (!mine) {                                          /* another name's record: nothing to do with us */
            at += rdlength;
            continue;
        }
        if (type == 1 && klass == 1 && rdlength == 4) {       /* an A record is the only answer this asks for */
            dns_job_state.answer = rd32(packet + at);
            dns_job_state.answered = 1;
            return;
        }
        if (type == 5 && klass == 1) {                        /* CNAME: the name the rest of the answer is about */
            if (!dns_name_is(packet, len, at, chain, chain_len)) return;
            if (!dns_name_copy(packet, len, at, chain, sizeof chain, &chain_len)) return;
        }
        at += rdlength;
    }
}

static void udp_receive(uint32_t src, uint32_t dst, const uint8_t *packet, size_t len)
{
    uint16_t sport, dport, size;
    uint32_t sum;

    if (len < 8) return;
    sport = rd16(packet);
    dport = rd16(packet + 2);
    size = rd16(packet + 4);
    if (size < 8 || size > len) { counters.rx_errors++; return; }
    if (rd16(packet + 6)) {                                   /* zero means "not computed", as IPv4 allows */
        sum = pseudo_sum(src, dst, IP_PROTO_UDP, size);
        if (csum_fold(net_checksum(packet, size, sum)) != 0) { counters.rx_errors++; return; }
    }
    counters.udp_packets++;
    if (dport == DHCP_CLIENT_PORT) { dhcp_receive(src, packet + 8, size - 8); return; }
    if (dns_job_state.active && dport == dns_job_state.port) { dns_receive(src, sport, packet + 8, size - 8); return; }
}

/* ------------------------------------------------------------ TCP --------------------------------------------------- */

static int tcp_send_segment(uint8_t flags, const void *payload, size_t len, uint32_t seq)
{
    uint8_t segment[20 + TCP_MSS];
    uint8_t mac[NET_ALEN];
    uint32_t hop, sum;
    uint16_t window = (uint16_t)(sizeof tcp.rx - tcp.rx_len);

    if (len + 20 > sizeof segment) return 0;
    if (!next_hop(tcp.raddr, &hop)) return 0;
    if (!arp_resolve(hop, mac)) return 0;
    wr16(segment, tcp.lport);
    wr16(segment + 2, tcp.rport);
    wr32(segment + 4, seq);
    wr32(segment + 8, tcp.rcv_nxt);
    segment[12] = 0x50;
    segment[13] = flags;
    wr16(segment + 14, window);
    wr16(segment + 16, 0);
    wr16(segment + 18, 0);
    if (len) memcpy(segment + 20, payload, len);
    sum = pseudo_sum(local_ip, tcp.raddr, IP_PROTO_TCP, (uint16_t)(20 + len));
    sum = net_checksum(segment, 20 + len, sum);
    wr16(segment + 16, csum_fold(sum));
    return ip_send(tcp.raddr, IP_PROTO_TCP, segment, 20 + len);
}

void net_tcp_receive(uint32_t src, const uint8_t *segment, size_t len)
{
    size_t header;
    uint32_t seq, ack;
    uint8_t flags;
    size_t payload;

    if (len < 20) { counters.rx_errors++; return; }
    /* The segment's own checksum, over the IPv4 pseudo-header, before anything in it is believed - including
     * the reset below, which would otherwise let any machine that can spoof a source address make this stack
     * emit traffic with a checksum this side never computed. */
    {
        uint32_t sum = pseudo_sum(src, local_ip, IP_PROTO_TCP, (uint16_t)len);
        if (csum_fold(net_checksum(segment, len, sum)) != 0) { counters.rx_errors++; return; }
    }
    if (tcp.state == TCP_CLOSED) {
        /* Nothing is listening: answer a stray SYN with a reset so the peer stops retrying. */
        if (segment[13] & 0x02) {
            uint8_t reset[20];
            uint16_t sport = rd16(segment), dport = rd16(segment + 2);
            memset(reset, 0, sizeof reset);
            wr16(reset, dport);
            wr16(reset + 2, sport);
            wr32(reset + 4, rd32(segment + 8));
            reset[12] = 0x50;
            reset[13] = 0x04;                              /* RST */
            wr32(reset + 8, 0);
            {
                uint32_t sum = pseudo_sum(local_ip, src, IP_PROTO_TCP, (uint16_t)sizeof reset);
                wr16(reset + 16, csum_fold(net_checksum(reset, sizeof reset, sum)));
            }
            (void)ip_send(src, IP_PROTO_TCP, reset, sizeof reset);
        }
        return;
    }
    if (src != tcp.raddr || rd16(segment) != tcp.rport || rd16(segment + 2) != tcp.lport) return;
    header = (size_t)(segment[12] >> 4) * 4u;
    if (header < 20 || header > len) { counters.rx_errors++; return; }
    seq = rd32(segment + 4);
    ack = rd32(segment + 8);
    flags = segment[13];
    payload = len - header;
    counters.tcp_packets++;

    if (flags & 0x04) {                                    /* RST */
        tcp.state = TCP_CLOSED;
        return;
    }
    if (tcp.state == TCP_SYN_SENT) {
        if ((flags & 0x12) == 0x12 && ack == tcp.iss + 1) { /* SYN|ACK */
            tcp.rcv_nxt = seq + 1;
            tcp.snd_una = ack;
            tcp.snd_nxt = ack;
            tcp.state = TCP_ESTABLISHED;
            (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt); /* ACK */
        }
        return;
    }
    if (tcp.state == TCP_ESTABLISHED || tcp.state == TCP_FIN_WAIT) {
        if (ack && (int32_t)(ack - tcp.snd_una) > 0) tcp.snd_una = ack;      /* forward progress only */
        if (payload) {
            if (seq == tcp.rcv_nxt) {
                /* A segment longer than the receive buffer cannot be stored, and storing part of it would
                 * advance the sequence number by bytes the application never saw.  It is refused as a peer
                 * exceeding the MSS this stack announced, and acknowledged so the peer retransmits smaller. */
                if (payload > TCP_MSS) {
                    counters.rx_errors++;
                    (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);
                    return;
                }
                /* One segment is held for the application at a time.  A second one arriving before that has
                 * been read must not overwrite it: dropping the old bytes while acknowledging the new would
                 * hand the application a response with a hole in it and no way to notice.  It is re-acknowledged
                 * instead, with the zero window this stack now advertises, and the peer will send it again once
                 * the window opens. */
                if (tcp.rx_len) {
                    (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);
                    return;
                }
                memcpy(tcp.rx, segment + header, payload);
                tcp.rx_len = payload;
                tcp.rcv_nxt += (uint32_t)payload;
                (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);
            } else if ((int32_t)(seq - tcp.rcv_nxt) > 0) {
                (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);  /* a gap: re-acknowledge what we have */
            }
            /* A retransmission of bytes already taken is acknowledged by the ack above and not re-delivered. */
        }
        if (flags & 0x01) {                                 /* FIN */
            /* The FIN sits after the last byte of the stream, so it can only be taken once every byte before
             * it has been received: a FIN that describes bytes this stack refused is re-acknowledged and left
             * for the peer to repeat with the data.  The close itself waits for the application to read what
             * is already buffered, so net_tcp_recv() cannot report the end of a stream it has not delivered. */
            if (seq + (uint32_t)payload == tcp.rcv_nxt && !tcp.rx_len) {
                tcp.rcv_nxt++;
                tcp.peer_fin = 1;
                tcp.state = TCP_PEER_CLOSED;
            }
            (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);
        }
    }
}

int net_tcp_open(void) { return tcp.state != TCP_CLOSED; }

int net_tcp_connect(uint32_t ip, uint16_t port, uint32_t timeout_ms)
{
    uint64_t deadline;

    if (!iface || !local_ip) return 0;
    net_tcp_close();
    memset(&tcp, 0, sizeof tcp);
    tcp.raddr = ip;
    tcp.rport = port;
    tcp.lport = net_sport();
    tcp.iss = net_entropy();
    tcp.snd_nxt = tcp.iss;
    tcp.state = TCP_SYN_SENT;
    deadline = net_now_ms() + timeout_ms;
    for (unsigned tries = 0; ; tries++) {
        if (!tcp_send_segment(0x02, 0, 0, tcp.iss)) {         /* SYN, or the ARP that has to precede it */
            if (net_now_ms() >= deadline) { tcp.state = TCP_CLOSED; return 0; }
            net_poll();
            sleep_ms(1);
            continue;
        }
        uint64_t sent = net_now_ms();
        while (net_now_ms() - sent < TCP_RETRY_MS) {
            net_poll();
            if (tcp.state == TCP_ESTABLISHED) return 1;
            if (tcp.state == TCP_CLOSED) return 0;
            if (net_now_ms() >= deadline) { tcp.state = TCP_CLOSED; return 0; }
            sleep_ms(1);
        }
        if (net_now_ms() >= deadline || tries >= 4) { tcp.state = TCP_CLOSED; return 0; }
    }
}

int net_tcp_send(const void *data, size_t len, uint32_t timeout_ms)
{
    uint64_t deadline;

    if (tcp.state != TCP_ESTABLISHED && tcp.state != TCP_PEER_CLOSED) return 0;
    while (len) {
        size_t chunk = len > TCP_MSS ? TCP_MSS : len;
        memcpy(tcp.tx, data, chunk);
        tcp.tx_len = chunk;
        tcp.tx_tries = 0;
        tcp.tx_sent = net_now_ms();
        if (!tcp_send_segment(0x18, tcp.tx, chunk, tcp.snd_nxt)) return 0;   /* PSH|ACK */
        tcp.snd_nxt += (uint32_t)chunk;
        deadline = net_now_ms() + timeout_ms;
        while ((int32_t)(tcp.snd_una - tcp.snd_nxt) < 0) {
            net_poll();
            if (tcp.state == TCP_CLOSED) return 0;
            if (net_now_ms() - tcp.tx_sent > TCP_RETRY_MS) {
                if (++tcp.tx_tries > TCP_TRIES) return 0;
                if (!tcp_send_segment(0x18, tcp.tx, tcp.tx_len, tcp.snd_nxt - (uint32_t)tcp.tx_len)) return 0;
                tcp.tx_sent = net_now_ms();
            }
            if (net_now_ms() >= deadline) return 0;
            sleep_ms(1);
        }
        data = (const uint8_t *)data + chunk;
        len -= chunk;
    }
    return 1;
}

int net_tcp_recv(void *buffer, size_t cap, uint32_t timeout_ms)
{
    uint64_t deadline = net_now_ms() + timeout_ms;

    for (;;) {
        if (tcp.rx_len) {
            /* A short read takes the front of the buffer and keeps the rest: the sequence number has already
             * been advanced over every byte in it, so a byte dropped here is a byte the peer believes was
             * delivered and will never send again. */
            size_t n = tcp.rx_len > cap ? cap : tcp.rx_len;
            memcpy(buffer, tcp.rx, n);
            if (n < tcp.rx_len) {
                memmove(tcp.rx, tcp.rx + n, tcp.rx_len - n);
                tcp.rx_len -= n;
            } else {
                tcp.rx_len = 0;
                if (tcp.peer_fin && tcp.state != TCP_CLOSED) tcp.state = TCP_PEER_CLOSED;
            }
            /* The window in that ACK has just gone from zero back to a segment's worth: without it the peer
             * has no reason to send the segment it was told to hold. */
            (void)tcp_send_segment(0x10, 0, 0, tcp.snd_nxt);
            return (int)n;
        }
        if (tcp.state == TCP_PEER_CLOSED || tcp.state == TCP_CLOSED) return 0;
        if (net_now_ms() >= deadline) return -1;
        net_poll();
        sleep_ms(1);
    }
}

void net_tcp_close(void)
{
    if (tcp.state == TCP_ESTABLISHED) {
        (void)tcp_send_segment(0x11, 0, 0, tcp.snd_nxt);      /* FIN|ACK */
        tcp.snd_nxt++;
    }
    tcp.state = TCP_CLOSED;
}

/* ------------------------------------------------------------ ping and resolver -------------------------------------- */

int net_ping(uint32_t ip, uint32_t timeout_ms, uint32_t *rtt_ms)
{
    uint64_t deadline;
    uint16_t ident = (uint16_t)net_entropy();

    if (!iface || !local_ip) return 0;
    memset(&pinger, 0, sizeof pinger);
    pinger.active = 1;
    pinger.target = ip;
    pinger.ident = ident;
    deadline = net_now_ms() + timeout_ms;
    for (unsigned tries = 0; tries < 4; tries++) {
        uint8_t request[8 + 32];
        memset(request, 0, sizeof request);
        request[0] = 8;
        wr16(request + 4, ident);
        wr16(request + 6, (uint16_t)(tries + 1));
        for (size_t i = 8; i < sizeof request; i++) request[i] = (uint8_t)(i * 7u + tries);
        wr16(request + 2, csum_fold(net_checksum(request, sizeof request, 0)));
        pinger.sent = net_now_ms();
        while (!ip_send(ip, IP_PROTO_ICMP, request, sizeof request)) {
            if (net_now_ms() - pinger.sent > 1000u || net_now_ms() >= deadline) break;
            net_poll();
            sleep_ms(1);
        }
        while (net_now_ms() - pinger.sent < 1000u) {
            net_poll();
            if (pinger.answered) {
                if (rtt_ms) *rtt_ms = pinger.rtt;
                pinger.active = 0;
                return 1;
            }
            if (net_now_ms() >= deadline) break;
            sleep_ms(1);
        }
        if (net_now_ms() >= deadline) break;
    }
    pinger.active = 0;
    return 0;
}

int net_dns_lookup(const char *name, uint32_t *ip, uint32_t timeout_ms)
{
    uint64_t deadline;
    size_t n;

    if (!iface || !local_ip || !local_dns || !name || !*name) return 0;
    if (net_parse_ip(name, ip)) return 1;                    /* a literal needs no resolver */
    memset(&dns_job_state, 0, sizeof dns_job_state);
    n = strlen(name);
    if (n > DNS_MAX_NAME) return 0;
    memcpy(dns_job_state.name, name, n);
    dns_job_state.name[n] = 0;
    dns_job_state.active = 1;
    dns_job_state.ident = (uint16_t)net_entropy();
    dns_job_state.port = net_sport();
    deadline = net_now_ms() + timeout_ms;
    for (unsigned tries = 0; tries < DNS_TRIES; tries++) {
        if (!dns_send_retry(1000)) break;
        dns_job_state.sent = net_now_ms();
        while (net_now_ms() - dns_job_state.sent < DNS_RETRY_MS) {
            net_poll();
            if (dns_job_state.answered) {
                *ip = dns_job_state.answer;
                dns_job_state.active = 0;
                return 1;
            }
            if (net_now_ms() >= deadline) break;
            sleep_ms(1);
        }
        if (net_now_ms() >= deadline) break;
    }
    dns_job_state.active = 0;
    return 0;
}

/* ------------------------------------------------------------ HTTP -------------------------------------------------- */

static int parse_url(const char *url, char *host, size_t host_cap, uint16_t *port, char *path, size_t path_cap)
{
    const char *p = url, *slash;
    size_t hlen;

    if (!url) return 0;
    if (!strncmp(p, "http://", 7)) p += 7;
    else {
        for (const char *q = p; q[0] && q[1] && q[2]; q++) if (q[0] == ':' && q[1] == '/' && q[2] == '/') return 0;
    }                                                        /* https and everything else: not this stack */
    slash = 0;
    for (const char *q = p; *q; q++) if (*q == '/') { slash = q; break; }
    if (!slash) slash = p + strlen(p);
    {
        const char *colon = 0;
        for (const char *q = p; q < slash; q++) if (*q == ':') colon = q;
        if (colon) {
            *port = 0;
            for (const char *q = colon + 1; q < slash; q++) {
                if (*q < '0' || *q > '9') return 0;
                *port = (uint16_t)(*port * 10 + (*q - '0'));
            }
            if (!*port) return 0;
            hlen = (size_t)(colon - p);
        } else {
            *port = 80;
            hlen = (size_t)(slash - p);
        }
    }
    if (!hlen || hlen >= host_cap) return 0;
    /* A URL is text this kernel turns into a request line and a Host header.  A control character in either
     * part is how a "URL" smuggles a second header, or a second request, onto the connection. */
    for (size_t i = 0; i < hlen; i++) {
        unsigned char c = (unsigned char)p[i];
        if (c <= 0x20 || c == 0x7f) return 0;
    }
    memcpy(host, p, hlen);
    host[hlen] = 0;
    if (*slash) {
        size_t plen = strlen(slash);
        if (plen >= path_cap) return 0;
        for (size_t i = 0; i < plen; i++) {
            unsigned char c = (unsigned char)slash[i];
            if (c < 0x20 || c == 0x7f) return 0;
        }
        memcpy(path, slash, plen + 1);
    } else {
        strcpy(path, "/");
    }
    return 1;
}

static size_t build_http_request(char *out, size_t cap, const char *host, const char *path)
{
    const char *pieces[6];
    size_t used = 0;

    pieces[0] = "GET ";
    pieces[1] = path;
    pieces[2] = " HTTP/1.0\r\nHost: ";
    pieces[3] = host;
    pieces[4] = "\r\nUser-Agent: SCos/1.0\r\nConnection: close\r\n\r\n";
    pieces[5] = 0;
    for (int i = 0; pieces[i]; i++) {
        size_t n = strlen(pieces[i]);
        if (used + n + 1 > cap) return 0;
        memcpy(out + used, pieces[i], n);
        used += n;
    }
    out[used] = 0;
    return used;
}

int net_http_get(const char *url, char *response, size_t cap, size_t *len)
{
    char host[128], path[256];
    uint16_t port;
    uint32_t address;
    char request[512];
    size_t used = 0, request_len;
    int n;

    if (!response || !cap || !len) return 0;
    *len = 0;
    if (!parse_url(url, host, sizeof host, &port, path, sizeof path)) return 0;
    if (!net_dns_lookup(host, &address, 4000)) return 0;
    if (!net_tcp_connect(address, port, 5000)) return 0;
    request_len = build_http_request(request, sizeof request, host, path);
    if (!request_len || request_len >= sizeof request) { net_tcp_close(); return 0; }
    if (!net_tcp_send(request, request_len, 4000)) { net_tcp_close(); return 0; }
    for (;;) {
        if (used + 1 >= cap) break;
        n = net_tcp_recv(response + used, cap - used - 1, 5000);
        if (n <= 0) break;
        used += (size_t)n;
    }
    net_tcp_close();
    response[used] = 0;
    *len = used;
    return used > 0;
}

/* ------------------------------------------------------------ timers and the poll loop ------------------------------ */

void net_timers(void)
{
    uint64_t now = net_now_ms();

    /* A cable is plugged in after the desktop is up more often than before it, and a PHY that has just been
     * reset takes a second or two to report a link.  net_start() is called once at boot, so without this the
     * machine that boots with the cable out never gets an address, no matter how long it is left.  The address
     * is asked for as soon as the link appears; an attempt that already failed is retried on the same slow
     * clock the attempt itself used, so a machine with no server is not flooding the segment. */
    if (iface && iface->link && !local_ip && !dhcp.active && dhcp.lease != NET_LEASE_BOUND) {
        uint64_t wait = dhcp.lease == NET_LEASE_FAILED ? DHCP_RETRY_MS * 4u : DHCP_RETRY_MS;
        if (dhcp.sent > now || now - dhcp.sent >= wait) net_dhcp_renew();
    }

    if (dhcp.active) {
        if (dhcp.lease == NET_LEASE_OFFER && now - dhcp.sent > DHCP_RETRY_MS) {
            (void)dhcp_send(3, dhcp.offered);                       /* REQUEST again */
            dhcp.sent = now;
        } else if (dhcp.lease != NET_LEASE_OFFER && now - dhcp.sent > DHCP_RETRY_MS) {
            if (dhcp.tries++ >= DHCP_TRIES) {
                dhcp.active = 0;
                dhcp.lease = NET_LEASE_FAILED;
                counters.state = NET_STATE_FAILED;
            } else {
                (void)dhcp_send(1, 0);                              /* DISCOVER */
                dhcp.sent = now;
            }
        }
    }
}

void net_rx_frame(const void *frame, size_t len)
{
    const uint8_t *f = frame;
    uint16_t type;

    if (!f || len < ETH_HLEN) { counters.rx_errors++; return; }
    if (len > NET_FRAME_MAX) { counters.frames_too_big++; return; }
    counters.rx_packets++;
    counters.rx_bytes += (uint32_t)len;
    /* A card that reports the Ethernet FCS is not supposed to, but some do; the type field is the test. */
    type = rd16(f + 12);
    if (type == ETH_TYPE_ARP) { arp_receive(f + ETH_HLEN, len - ETH_HLEN); return; }
    if (type != ETH_TYPE_IP) return;
    ip_receive(f, len);
}

void net_poll(void)
{
    if (!iface) return;
    for (int i = 0; i < NET_RX_BURST; i++) if (!iface->poll || !iface->poll(iface)) break;
    net_timers();
}

/* The driver side of bring-up.  The two Realtek probes are weak references on purpose: the host harness
 * compiles this file on its own against a model of a card and has no PCI bus to scan, and a link with no
 * driver objects in it still produces a stack that reports "no supported adapter" instead of failing to
 * link.  The kernel links the strong definitions from rtl8139.c and rtl8168.c. */
__attribute__((weak)) int rtl8139_probe(void) { return 0; }
__attribute__((weak)) int rtl8168_probe(void) { return 0; }

void net_hardware_init(void)
{
    if (nic_count) return;                 /* idempotent: a card already receiving is not reinitialized */
    if (rtl8139_probe()) return;
    (void)rtl8168_probe();
}

void net_start(void)
{
    if (!iface) return;
    if (iface->link && !local_ip && !dhcp.active && dhcp.lease != NET_LEASE_BOUND) net_dhcp_renew();
}

int net_set_static(uint32_t ip, uint32_t mask, uint32_t gateway, uint32_t dns)
{
    if (!iface || !ip) return 0;
    local_ip = ip;
    local_mask = mask ? mask : 0xffffff00u;
    local_gw = gateway;
    local_dns = dns ? dns : gateway;
    dhcp.active = 0;
    dhcp.lease = NET_LEASE_NONE;
    counters.state = NET_STATE_UP;
    return 1;
}

/* ------------------------------------------------------------ the status paragraph --------------------------------- */

static size_t put_text(char *out, size_t cap, size_t used, const char *text)
{
    size_t n = strlen(text);
    if (used + n + 1 > cap) n = cap > used + 1 ? cap - used - 1 : 0;
    memcpy(out + used, text, n);
    out[used + n] = 0;
    return used + n;
}

static size_t put_num(char *out, size_t cap, size_t used, uint32_t value)
{
    char digits[11];
    unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value && n < sizeof digits);
    if (used + n + 1 > cap) n = cap > used + 1 ? (unsigned)(cap - used - 1) : 0;
    for (unsigned i = 0; i < n; i++) out[used + i] = digits[n - 1 - i];
    out[used + n] = 0;
    return used + n;
}

static size_t put_ip(char *out, size_t cap, size_t used, uint32_t ip)
{
    char text[20];
    net_format_ip(text, sizeof text, ip);
    return put_text(out, cap, used, text);
}

size_t net_format_status(char *out, size_t cap)
{
    char mac[18];
    size_t used = 0;
    struct net_info info;

    if (!out || !cap) return 0;
    out[0] = 0;
    net_get_info(&info);
    used = put_text(out, cap, used, "net: ");
    used = put_text(out, cap, used, net_state_name(info.state));
    if (info.present) {
        for (int i = 0; i < NET_ALEN; i++) {
            mac[i * 3] = "0123456789abcdef"[(info.mac[i] >> 4) & 15];
            mac[i * 3 + 1] = "0123456789abcdef"[info.mac[i] & 15];
            mac[i * 3 + 2] = i == NET_ALEN - 1 ? 0 : ':';
        }
        used = put_text(out, cap, used, ", ");
        used = put_text(out, cap, used, info.driver);
        used = put_text(out, cap, used, " mac ");
        used = put_text(out, cap, used, mac);
        if (info.link) {
            used = put_text(out, cap, used, ", link ");
            used = put_num(out, cap, used, info.speed_mbps);
            used = put_text(out, cap, used, " Mb/s");
        } else {
            used = put_text(out, cap, used, ", no link");
        }
        if (info.ip) { used = put_text(out, cap, used, ", "); used = put_ip(out, cap, used, info.ip); }
        if (info.mask) { used = put_text(out, cap, used, " mask "); used = put_ip(out, cap, used, info.mask); }
        if (info.gateway) { used = put_text(out, cap, used, " gw "); used = put_ip(out, cap, used, info.gateway); }
        if (info.dns) { used = put_text(out, cap, used, " dns "); used = put_ip(out, cap, used, info.dns); }
        used = put_text(out, cap, used, ", lease ");
        used = put_text(out, cap, used, net_lease_name(info.lease));
        if (info.lease_seconds) {
            used = put_text(out, cap, used, " ");
            used = put_num(out, cap, used, info.lease_seconds);
            used = put_text(out, cap, used, " s");
        }
    }
    used = put_text(out, cap, used, ".\nframes rx ");
    used = put_num(out, cap, used, info.rx_packets);
    used = put_text(out, cap, used, " tx ");
    used = put_num(out, cap, used, info.tx_packets);
    used = put_text(out, cap, used, " (");
    used = put_num(out, cap, used, info.rx_bytes);
    used = put_text(out, cap, used, "/");
    used = put_num(out, cap, used, info.tx_bytes);
    used = put_text(out, cap, used, " B), dropped ");
    used = put_num(out, cap, used, info.dropped);
    used = put_text(out, cap, used, ", errors ");
    used = put_num(out, cap, used, info.rx_errors);
    used = put_text(out, cap, used, ", too big ");
    used = put_num(out, cap, used, info.frames_too_big);
    used = put_text(out, cap, used, "; arp ");
    used = put_num(out, cap, used, info.arp_requests);
    used = put_text(out, cap, used, " asked/");
    used = put_num(out, cap, used, info.arp_replies);
    used = put_text(out, cap, used, " answered, ");
    used = put_num(out, cap, used, info.arp_unsolicited);
    used = put_text(out, cap, used, " ignored, icmp ");
    used = put_num(out, cap, used, info.icmp_echo);
    used = put_text(out, cap, used, ", udp ");
    used = put_num(out, cap, used, info.udp_packets);
    used = put_text(out, cap, used, ", tcp ");
    used = put_num(out, cap, used, info.tcp_packets);
    used = put_text(out, cap, used, ", tcp state ");
    used = put_num(out, cap, used, info.tcp_state);
    used = put_text(out, cap, used, ".\n");
    return used;
}
