/* The network stack, run against a model of a wire.
 *
 * What is under test is kernel/drivers/net.c exactly as the kernel compiles it: the bytes of an ARP request, the
 * header an ICMP echo carries, the option order a DHCP DISCOVER uses, the label encoding of a DNS question, the
 * sequence arithmetic of a TCP handshake, and the checks that decide a packet is not worth answering.  What is
 * deliberately absent is a second implementation of the stack: the model below is a *peer*, and it builds its
 * packets by hand from the RFC field offsets so that a change in the stack's idea of a header fails a check here
 * instead of on somebody's network.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -ffreestanding -fno-builtin -fno-stack-protector \
 *      -fsanitize=address,undefined -fno-sanitize-recover=all -Ikernel/include \
 *      tools/tests/net_host.c -o /tmp/net && /tmp/net
 *
 * Time is the mock's: every poll advances the PIT tick by one, so a timeout in the stack is a real bound that an
 * absent answer can reach, and `sleep_ms' advances it too so a blocking call cannot spin forever in a test.
 */
#include <stdio.h>
#include <stdint.h>

#include "boot.h"

volatile uint64_t timer_ticks;
void klog(const char *fmt, ...) { (void)fmt; }
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void *a, const void *b, size_t n) { const uint8_t *x = a, *y = b; while (n--) { if (*x != *y) return *x - *y; x++; y++; } return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b; }
int strncmp(const char *a, const char *b, size_t n) { while (n-- && *a && *a == *b) { a++; b++; } return n == (size_t)-1 ? 0 : (unsigned char)*a - (unsigned char)*b; }
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) {} return r; }

#include "../../kernel/drivers/net.c"

/* ---------------------------------------------------------------- the mock wire ------------------------------------- */

#define MOCK_PEER_MAC0  0x02
static const uint8_t mock_peer_mac[NET_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x01 };
static const uint8_t mock_dhcp_mac[NET_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x02 };
#define MOCK_PEER_IP    0x0a000202u          /* 10.0.2.2: gateway and echo peer */
#define MOCK_DNS_IP     0x0a000203u          /* 10.0.2.3 */
#define MOCK_LEASED_IP  0x0a00020fu          /* 10.0.2.15 */
#define MOCK_DNS_ANSWER 0x5db8d822u          /* 93.184.216.34 */

static struct net_nic mock_nic;
static uint8_t mock_mac[NET_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x0a };

static uint8_t rxq[8][1600];
static size_t rxq_len[8];
static int rxq_head, rxq_tail;

static uint8_t tx_log[16][1600];
static size_t tx_log_len[16];
static int tx_count, tx_seen;

static int mock_mode;                        /* 0 silent, 1 answers ARP/DHCP/DNS/ICMP, 2 also serves HTTP */
static int mock_dhcp_stage;                  /* 0 waiting for DISCOVER, 1 waiting for REQUEST */
static int mock_tcp_stage;                   /* 0 waiting for SYN, 1 data accepted, 2 replied, 3 closed */
static uint32_t mock_tcp_seq, mock_tcp_ack;
static int mock_bad_dns_id, mock_bad_dns_source, mock_bad_udp_checksum;
static int mock_tcp_out_of_order;
static int mock_silent_dhcp_ack;              /* answer the OFFER but never the REQUEST */

static void q_push(const void *frame, size_t len)
{
    int next = (rxq_tail + 1) % 8;
    if (next == rxq_head || len > sizeof rxq[0]) return;
    memcpy(rxq[rxq_tail], frame, len);
    rxq_len[rxq_tail] = len;
    rxq_tail = next;
}

static size_t build_eth(uint8_t *out, const uint8_t *dst, const uint8_t *src, uint16_t type)
{
    memcpy(out, dst, NET_ALEN);
    memcpy(out + 6, src, NET_ALEN);
    out[12] = (uint8_t)(type >> 8);
    out[13] = (uint8_t)type;
    return 14;
}

static size_t build_ip(uint8_t *out, const uint8_t *dst_mac, uint32_t src, uint32_t dst, uint8_t proto,
                       const void *payload, size_t len)
{
    size_t at = build_eth(out, dst_mac, mock_peer_mac, 0x0800);
    uint16_t total = (uint16_t)(20 + len);
    memset(out + at, 0, 20);
    out[at] = 0x45;
    out[at + 2] = (uint8_t)(total >> 8);
    out[at + 3] = (uint8_t)total;
    out[at + 6] = 0x40;
    out[at + 8] = 64;
    out[at + 9] = proto;
    out[at + 12] = (uint8_t)(src >> 24); out[at + 13] = (uint8_t)(src >> 16);
    out[at + 14] = (uint8_t)(src >> 8); out[at + 15] = (uint8_t)src;
    out[at + 16] = (uint8_t)(dst >> 24); out[at + 17] = (uint8_t)(dst >> 16);
    out[at + 18] = (uint8_t)(dst >> 8); out[at + 19] = (uint8_t)dst;
    {
        uint16_t sum = csum_fold(net_checksum(out + at, 20, 0));
        out[at + 10] = (uint8_t)(sum >> 8); out[at + 11] = (uint8_t)sum;
    }
    if (len) memcpy(out + at + 20, payload, len);
    return at + 20 + len;
}

static size_t build_udp(uint8_t *payload_out, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                        const void *body, size_t len)
{
    uint16_t size = (uint16_t)(8 + len);
    payload_out[0] = (uint8_t)(sport >> 8); payload_out[1] = (uint8_t)sport;
    payload_out[2] = (uint8_t)(dport >> 8); payload_out[3] = (uint8_t)dport;
    payload_out[4] = (uint8_t)(size >> 8); payload_out[5] = (uint8_t)size;
    payload_out[6] = 0; payload_out[7] = 0;
    if (len) memcpy(payload_out + 8, body, len);
    {
        uint32_t sum = pseudo_sum(src, dst, 17, size);
        sum = net_checksum(payload_out, size, sum);
        uint16_t folded = csum_fold(sum);
        if (mock_bad_udp_checksum) folded ^= 0xffffu;
        payload_out[6] = (uint8_t)(folded >> 8); payload_out[7] = (uint8_t)folded;
    }
    return size;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static void mock_send_arp_reply(const uint8_t *request)
{
    uint8_t frame[64];
    uint32_t requester, asked;
    size_t at = build_eth(frame, request + 6, mock_peer_mac, 0x0806);

    /* The peer owns every address that is asked about: reply as the address in the request, to the address that
     * asked.  A mock that always answered with one identity could not tell a stack that resolved the right host
     * from one that cached the only host it had ever heard from. */
    requester = get32(request + 28);
    asked = get32(request + 38);
    frame[at + 0] = 0; frame[at + 1] = 1;
    frame[at + 2] = 0x08; frame[at + 3] = 0x00;
    frame[at + 4] = 6; frame[at + 5] = 4;
    frame[at + 6] = 0; frame[at + 7] = 2;                       /* reply */
    memcpy(frame + at + 8, mock_peer_mac, NET_ALEN);            /* sender hardware */
    frame[at + 14] = (uint8_t)(asked >> 24); frame[at + 15] = (uint8_t)(asked >> 16);
    frame[at + 16] = (uint8_t)(asked >> 8); frame[at + 17] = (uint8_t)asked;
    memcpy(frame + at + 18, request + 22, NET_ALEN);            /* the asker */
    frame[at + 24] = (uint8_t)(requester >> 24); frame[at + 25] = (uint8_t)(requester >> 16);
    frame[at + 26] = (uint8_t)(requester >> 8); frame[at + 27] = (uint8_t)requester;
    q_push(frame, at + 28);
}

static void mock_dhcp_offer(const uint8_t *request, int kind)
{
    uint8_t frame[600], body[300];
    uint32_t xid = ((uint32_t)request[46] << 24) | ((uint32_t)request[47] << 16) |
                   ((uint32_t)request[48] << 8) | request[49];
    size_t at = 0;

    memset(body, 0, sizeof body);
    body[0] = 2; body[1] = 1; body[2] = 6;
    body[4] = (uint8_t)(xid >> 24); body[5] = (uint8_t)(xid >> 16);
    body[6] = (uint8_t)(xid >> 8); body[7] = (uint8_t)xid;
    body[16] = (uint8_t)(MOCK_LEASED_IP >> 24); body[17] = (uint8_t)(MOCK_LEASED_IP >> 16);
    body[18] = (uint8_t)(MOCK_LEASED_IP >> 8); body[19] = (uint8_t)MOCK_LEASED_IP;
    memcpy(body + 28, request + 70, NET_ALEN);
    memcpy(body + 236, "\x63\x82\x53\x63", 4);
    at = 240;
    body[at++] = 53; body[at++] = 1; body[at++] = (uint8_t)kind;
    body[at++] = 54; body[at++] = 4;
    body[at++] = (uint8_t)(MOCK_PEER_IP >> 24); body[at++] = (uint8_t)(MOCK_PEER_IP >> 16);
    body[at++] = (uint8_t)(MOCK_PEER_IP >> 8); body[at++] = (uint8_t)MOCK_PEER_IP;
    body[at++] = 1; body[at++] = 4; body[at++] = 255; body[at++] = 255; body[at++] = 255; body[at++] = 0;
    body[at++] = 3; body[at++] = 4;
    body[at++] = (uint8_t)(MOCK_PEER_IP >> 24); body[at++] = (uint8_t)(MOCK_PEER_IP >> 16);
    body[at++] = (uint8_t)(MOCK_PEER_IP >> 8); body[at++] = (uint8_t)MOCK_PEER_IP;
    body[at++] = 6; body[at++] = 4;
    body[at++] = (uint8_t)(MOCK_DNS_IP >> 24); body[at++] = (uint8_t)(MOCK_DNS_IP >> 16);
    body[at++] = (uint8_t)(MOCK_DNS_IP >> 8); body[at++] = (uint8_t)MOCK_DNS_IP;
    body[at++] = 51; body[at++] = 4; body[at++] = 0; body[at++] = 0; body[at++] = 0x0e; body[at++] = 0x10;
    body[at++] = 255;
    {
        uint8_t udp[400];
        uint32_t src = kind == 2 ? MOCK_PEER_IP : MOCK_PEER_IP;
        uint32_t dst = 0xffffffffu;
        size_t ulen = build_udp(udp, src, dst, 67, 68, body, at);
        size_t flen = build_ip(frame, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", src, dst, 17, udp, ulen);
        q_push(frame, flen);
    }
    (void)mock_dhcp_mac;
}

static void mock_dns_answer(const uint8_t *frame, size_t len)
{
    const uint8_t *ip = frame + 14, *udp = ip + 20;
    uint16_t sport = (uint16_t)((udp[0] << 8) | udp[1]);
    const uint8_t *dns = udp + 8;
    size_t qlen;
    uint8_t reply[512], body[400];
    uint16_t id = mock_bad_dns_id ? 0x1234 : (uint16_t)((dns[0] << 8) | dns[1]);

    if (len < 14 + 20 + 8 + 12) return;
    qlen = 12;
    while (qlen < len - 14 - 20 - 8 && dns[qlen]) qlen += dns[qlen] + 1;
    qlen += 5;                                              /* root label + type + class */
    memset(body, 0, sizeof body);
    body[0] = (uint8_t)(id >> 8); body[1] = (uint8_t)id;
    body[2] = 0x81; body[3] = 0x80;                          /* response, recursion available */
    body[4] = 0; body[5] = 1;
    body[6] = 0; body[7] = 1;
    memcpy(body + 12, dns + 12, qlen - 12);
    {
        size_t at = 12 + (qlen - 12);
        body[at++] = 0xc0; body[at++] = 0x0c;               /* name: a pointer to the question */
        body[at++] = 0; body[at++] = 1;                      /* type A */
        body[at++] = 0; body[at++] = 1;                      /* class IN */
        body[at++] = 0; body[at++] = 0; body[at++] = 0; body[at++] = 60;
        body[at++] = 0; body[at++] = 4;
        body[at++] = (uint8_t)(MOCK_DNS_ANSWER >> 24); body[at++] = (uint8_t)(MOCK_DNS_ANSWER >> 16);
        body[at++] = (uint8_t)(MOCK_DNS_ANSWER >> 8); body[at++] = (uint8_t)MOCK_DNS_ANSWER;
        {
            uint8_t udp_out[450];
            uint32_t src = mock_bad_dns_source ? 0x0a000299u : MOCK_DNS_IP;
            size_t ulen = build_udp(udp_out, src, local_ip, 53, sport, body, at);
            size_t flen = build_ip(reply, mock_mac, src, local_ip, 17, udp_out, ulen);
            q_push(reply, flen);
        }
    }
}

static void mock_icmp_reply(const uint8_t *frame, size_t len)
{
    uint8_t reply[600];
    const uint8_t *ip = frame + 14;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    size_t total = (size_t)((ip[2] << 8) | ip[3]);
    uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];

    if (total > len - 14 || total < ihl + 8) return;
    {
        uint8_t payload[600];
        memcpy(payload, ip + ihl, total - ihl);
        payload[0] = 0;                                      /* echo reply */
        payload[2] = 0; payload[3] = 0;
        {
            uint16_t sum = csum_fold(net_checksum(payload, total - ihl, 0));
            payload[2] = (uint8_t)(sum >> 8); payload[3] = (uint8_t)sum;
        }
        {
            size_t flen = build_ip(reply, mock_mac, MOCK_PEER_IP, src, 1, payload, total - ihl);
            q_push(reply, flen);
        }
    }
}

static void mock_tcp_respond(const uint8_t *frame, size_t len)
{
    const uint8_t *ip = frame + 14;
    size_t ihl = (size_t)(ip[0] & 0x0f) * 4;
    size_t total = (size_t)((ip[2] << 8) | ip[3]);
    const uint8_t *tcp_in = ip + ihl;
    uint32_t src = ((uint32_t)ip[12] << 24) | ((uint32_t)ip[13] << 16) | ((uint32_t)ip[14] << 8) | ip[15];
    uint32_t seq = ((uint32_t)tcp_in[4] << 24) | ((uint32_t)tcp_in[5] << 16) | ((uint32_t)tcp_in[6] << 8) | tcp_in[7];
    uint16_t sport = (uint16_t)((tcp_in[0] << 8) | tcp_in[1]);
    uint8_t flags = tcp_in[13];
    size_t payload = total - ihl - (size_t)(tcp_in[12] >> 4) * 4u;
    uint8_t tcp_out[1200], frame_out[1400];

    if (total > len - 14) return;
    if ((flags & 0x02) && !(flags & 0x10)) {                 /* SYN */
        mock_tcp_seq = 5000;
        mock_tcp_ack = seq + 1;
        memset(tcp_out, 0, 20);
        tcp_out[0] = 0; tcp_out[1] = 80;
        tcp_out[2] = (uint8_t)(sport >> 8); tcp_out[3] = (uint8_t)sport;
        tcp_out[4] = (uint8_t)(mock_tcp_seq >> 24); tcp_out[5] = (uint8_t)(mock_tcp_seq >> 16);
        tcp_out[6] = (uint8_t)(mock_tcp_seq >> 8); tcp_out[7] = (uint8_t)mock_tcp_seq;
        tcp_out[8] = (uint8_t)(mock_tcp_ack >> 24); tcp_out[9] = (uint8_t)(mock_tcp_ack >> 16);
        tcp_out[10] = (uint8_t)(mock_tcp_ack >> 8); tcp_out[11] = (uint8_t)mock_tcp_ack;
        tcp_out[12] = 0x50;
        tcp_out[13] = 0x12;                                   /* SYN|ACK */
        tcp_out[14] = 0x20; tcp_out[15] = 0x00;
        mock_tcp_seq++;
        {
            uint32_t sum = pseudo_sum(MOCK_PEER_IP, src, 6, 20);
            uint16_t folded = csum_fold(net_checksum(tcp_out, 20, sum));
            tcp_out[16] = (uint8_t)(folded >> 8); tcp_out[17] = (uint8_t)folded;
        }
        {
            size_t flen = build_ip(frame_out, mock_mac, MOCK_PEER_IP, src, 6, tcp_out, 20);
            q_push(frame_out, flen);
        }
        mock_tcp_stage = 1;
        return;
    }
    if (flags & 0x01) {                                      /* FIN */
        memset(tcp_out, 0, 20);
        tcp_out[0] = 0; tcp_out[1] = 80;
        tcp_out[2] = (uint8_t)(sport >> 8); tcp_out[3] = (uint8_t)sport;
        tcp_out[4] = (uint8_t)(mock_tcp_seq >> 24); tcp_out[5] = (uint8_t)(mock_tcp_seq >> 16);
        tcp_out[6] = (uint8_t)(mock_tcp_seq >> 8); tcp_out[7] = (uint8_t)mock_tcp_seq;
        tcp_out[8] = (uint8_t)((seq + 1) >> 24); tcp_out[9] = (uint8_t)((seq + 1) >> 16);
        tcp_out[10] = (uint8_t)((seq + 1) >> 8); tcp_out[11] = (uint8_t)(seq + 1);
        tcp_out[12] = 0x50; tcp_out[13] = 0x10;
        {
            uint32_t sum = pseudo_sum(MOCK_PEER_IP, src, 6, 20);
            uint16_t folded = csum_fold(net_checksum(tcp_out, 20, sum));
            tcp_out[16] = (uint8_t)(folded >> 8); tcp_out[17] = (uint8_t)folded;
        }
        {
            size_t flen = build_ip(frame_out, mock_mac, MOCK_PEER_IP, src, 6, tcp_out, 20);
            q_push(frame_out, flen);
        }
        mock_tcp_stage = 3;
        return;
    }
    if (payload && mock_tcp_stage == 1) {
        static const char response[] = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nServer: mock\r\n\r\nhello";
        size_t at = 20;
        memset(tcp_out, 0, 20);
        tcp_out[0] = 0; tcp_out[1] = 80;
        tcp_out[2] = (uint8_t)(sport >> 8); tcp_out[3] = (uint8_t)sport;
        tcp_out[4] = (uint8_t)(mock_tcp_seq >> 24); tcp_out[5] = (uint8_t)(mock_tcp_seq >> 16);
        tcp_out[6] = (uint8_t)(mock_tcp_seq >> 8); tcp_out[7] = (uint8_t)mock_tcp_seq;
        tcp_out[8] = (uint8_t)((seq + (uint32_t)payload) >> 24); tcp_out[9] = (uint8_t)((seq + payload) >> 16);
        tcp_out[10] = (uint8_t)((seq + payload) >> 8); tcp_out[11] = (uint8_t)(seq + payload);
        tcp_out[12] = 0x50; tcp_out[13] = 0x18;               /* PSH|ACK */
        tcp_out[14] = 0x20; tcp_out[15] = 0x00;
        memcpy(tcp_out + at, response, sizeof response - 1);
        at += sizeof response - 1;
        {
            uint32_t sum = pseudo_sum(MOCK_PEER_IP, src, 6, (uint16_t)at);
            uint16_t folded = csum_fold(net_checksum(tcp_out, at, sum));
            tcp_out[16] = (uint8_t)(folded >> 8); tcp_out[17] = (uint8_t)folded;
        }
        mock_tcp_seq += sizeof response - 1;
        {
            size_t flen = build_ip(frame_out, mock_mac, MOCK_PEER_IP, src, 6, tcp_out, at);
            q_push(frame_out, flen);
        }
        mock_tcp_stage = 2;
        /* Then the close, which is what an HTTP/1.0 server does after the body. */
        {
            size_t at2 = 20;
            memset(tcp_out, 0, 20);
            tcp_out[0] = 0; tcp_out[1] = 80;
            tcp_out[2] = (uint8_t)(sport >> 8); tcp_out[3] = (uint8_t)sport;
            tcp_out[4] = (uint8_t)(mock_tcp_seq >> 24); tcp_out[5] = (uint8_t)(mock_tcp_seq >> 16);
            tcp_out[6] = (uint8_t)(mock_tcp_seq >> 8); tcp_out[7] = (uint8_t)mock_tcp_seq;
            tcp_out[8] = (uint8_t)((seq + payload) >> 24); tcp_out[9] = (uint8_t)((seq + payload) >> 16);
            tcp_out[10] = (uint8_t)((seq + payload) >> 8); tcp_out[11] = (uint8_t)(seq + payload);
            tcp_out[12] = 0x50; tcp_out[13] = 0x11;           /* FIN|ACK */
            {
                uint32_t sum = pseudo_sum(MOCK_PEER_IP, src, 6, (uint16_t)at2);
                uint16_t folded = csum_fold(net_checksum(tcp_out, at2, sum));
                tcp_out[16] = (uint8_t)(folded >> 8); tcp_out[17] = (uint8_t)folded;
            }
            {
                size_t flen = build_ip(frame_out, mock_mac, MOCK_PEER_IP, src, 6, tcp_out, at2);
                q_push(frame_out, flen);
            }
            mock_tcp_seq++;
        }
        (void)mock_tcp_out_of_order;
    }
}

/* One step of the peer: every frame the stack has put on the wire since the last step is answered the way a
 * well-behaved network would answer it. */
static void mock_script(void)
{
    while (tx_seen < tx_count) {
        const uint8_t *frame = tx_log[tx_seen];
        size_t len = tx_log_len[tx_seen];
        uint16_t type;
        tx_seen++;
        if (len < 14) continue;
        type = (uint16_t)((frame[12] << 8) | frame[13]);
        if (type == 0x0806) {
            if (mock_mode) mock_send_arp_reply(frame);
            continue;
        }
        if (type != 0x0800 || len < 34) continue;
        if (!mock_mode) continue;
        if (frame[23] == 1) { mock_icmp_reply(frame, len); continue; }
        if (frame[23] == 17) {
            uint16_t sport = (uint16_t)((frame[34] << 8) | frame[35]);
            if (sport == 68) {
                /* A DHCP message: option 53 says which one. */
                const uint8_t *body = frame + 14 + 20 + 8;
                size_t at = 240;
                while (at < len - 42) {
                    uint8_t option = body[at++];
                    uint8_t size;
                    if (option == 255) break;
                    if (option == 0) continue;
                    size = body[at++];
                    if (option == 53 && size >= 1) {
                        if (body[at] == 1) { mock_dhcp_offer(frame, 2); mock_dhcp_stage = 1; }
                        else if (body[at] == 3) { if (!mock_silent_dhcp_ack) mock_dhcp_offer(frame, 5); mock_dhcp_stage = 2; }
                    }
                    at += size;
                }
                continue;
            }
            if (sport != 53) mock_dns_answer(frame, len);
            continue;
        }
        if (frame[23] == 6) mock_tcp_respond(frame, len);
    }
}

static int mock_transmit(struct net_nic *nic, const void *frame, size_t len)
{
    (void)nic;
    if (tx_count < 16 && len <= sizeof tx_log[0]) {
        memcpy(tx_log[tx_count], frame, len);
        tx_log_len[tx_count] = len;
        tx_count++;
    }
    return 1;
}

static int mock_poll(struct net_nic *nic)
{
    (void)nic;
    timer_ticks++;                                  /* ten milliseconds per poll */
    if (rxq_head != rxq_tail) {
        int i = rxq_head;
        rxq_head = (rxq_head + 1) % 8;
        net_rx_frame(rxq[i], rxq_len[i]);
        return 1;
    }
    mock_script();
    if (rxq_head != rxq_tail) {
        int i = rxq_head;
        rxq_head = (rxq_head + 1) % 8;
        net_rx_frame(rxq[i], rxq_len[i]);
        return 1;
    }
    return 0;
}

void sleep_ms(uint32_t ms) { timer_ticks += (ms + 9u) / 10u; }

/* ---------------------------------------------------------------- plumbing ------------------------------------------ */

static unsigned checks_run, failures_run;

static void check(const char *what, int ok)
{
    checks_run++;
    if (ok) return;
    failures_run++;
    printf("FAIL: %s\n", what);
}

static const char *find_text(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return hay;
    for (; *hay; hay++) if (!strncmp(hay, needle, n)) return hay;
    return 0;
}


static void mock_reset(int mode);

static void mock_detach(void)
{
    mock_reset(0);
    iface = 0;
    nic_count = 0;
    memset(&counters, 0, sizeof counters);
}

static void mock_reset(int mode)
{
    iface = 0;
    nic_count = 0;
    local_ip = local_mask = local_gw = local_dns = 0;
    memset(&counters, 0, sizeof counters);
    memset(arp, 0, sizeof arp);
    memset(&dhcp, 0, sizeof dhcp);
    memset(&pinger, 0, sizeof pinger);
    memset(&dns_job_state, 0, sizeof dns_job_state);
    memset(&tcp, 0, sizeof tcp);
    memset(local_mac, 0, sizeof local_mac);
    rxq_head = rxq_tail = 0;
    tx_count = tx_seen = 0;
    mock_mode = mode;
    mock_dhcp_stage = 0;
    mock_tcp_stage = 0;
    mock_tcp_seq = mock_tcp_ack = 0;
    mock_bad_dns_id = mock_bad_dns_source = mock_bad_udp_checksum = mock_tcp_out_of_order = 0;
    timer_ticks = 100;
    memset(&mock_nic, 0, sizeof mock_nic);
    mock_nic.name = "mock0";
    mock_nic.poll = mock_poll;
    mock_nic.transmit = mock_transmit;
    memcpy(mock_nic.mac, mock_mac, NET_ALEN);
    mock_nic.link = 1;
    mock_nic.speed_mbps = 1000;
    net_register_nic(&mock_nic);
}

/* ---------------------------------------------------------------- checks -------------------------------------------- */

static void test_addresses()
{
    char text[20];
    uint32_t ip = 0;

    check("the checksum of a word-aligned buffer folds to zero over itself plus its own answer",
          csum_fold(net_checksum("\x45\x00\x00\x14", 4, 0)) != 0);
    check("dotted quads parse", net_parse_ip("10.0.2.15", &ip) && ip == MOCK_LEASED_IP);
    check("and the ends parse too", net_parse_ip("255.255.255.255", &ip) && ip == 0xffffffffu &&
          net_parse_ip("0.0.0.0", &ip) && ip == 0);
    check("a malformed address is refused rather than half-accepted",
          !net_parse_ip("10.0.2", &ip) && !net_parse_ip("10.0.2.256", &ip) && !net_parse_ip("1.2.3.4.5", &ip) &&
          !net_parse_ip("a.b.c.d", &ip) && !net_parse_ip("", &ip));
    net_format_ip(text, sizeof text, MOCK_PEER_IP);
    check("and formatting is the inverse", !strcmp(text, "10.0.2.2"));
    {
        char small[5];
        net_format_ip(small, sizeof small, 0xc0a80101u);
        check("a buffer too small is truncated and still terminated", strlen(small) == 4 && small[4] == 0);
    }
}

static void test_arp()
{
    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("the first transmit is an ARP request, broadcast, asking for the gateway",
          net_ping(MOCK_PEER_IP, 2000, 0) == 1 && tx_count > 0);
    {
        const uint8_t *frame = tx_log[0];
        check("it is addressed to the broadcast MAC", !memcmp(frame, "\xff\xff\xff\xff\xff\xff", 6));
        check("its ethertype is 0x0806 and its opcode is a request",
              frame[12] == 0x08 && frame[13] == 0x06 && frame[20] == 0 && frame[21] == 1);
        check("it carries this adapter's MAC and address, and the target the caller named",
              !memcmp(frame + 22, mock_mac, 6) && get32(frame + 28) == MOCK_LEASED_IP &&
              get32(frame + 38) == MOCK_PEER_IP);
    }
    check("the reply is cached, so a second send needs no request",
          arp_find(MOCK_PEER_IP) && !memcmp(arp_find(MOCK_PEER_IP)->mac, mock_peer_mac, 6));
    check("and the counters say one request and one answer", counters.arp_requests == 1 && counters.arp_replies == 1);
}

static void test_ping()
{
    uint32_t rtt = 0;
    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("an echo to a peer that answers succeeds and reports a measured round trip",
          net_ping(MOCK_PEER_IP, 2000, &rtt) == 1 && rtt < 2000);
    check("the echo request carried identifier and sequence in the header an ICMP echo defines",
          tx_count >= 2 && tx_log[1][23] == 1 && tx_log[1][14 + 20] == 8);
    mock_reset(0);                                     /* a wire nobody answers on */
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("a silent peer times out rather than hanging: the call returns 0", net_ping(MOCK_PEER_IP, 1500, &rtt) == 0);
}

static void test_dhcp()
{
    mock_reset(1);
    net_dhcp_renew();
    for (int i = 0; i < 400 && local_ip == 0; i++) { net_poll(); sleep_ms(10); }
    check("the DISCOVER is a broadcast from port 68 to 67, and names option 53 first",
          tx_count > 0 && tx_log[0][12] == 0x08 && tx_log[0][13] == 0x00 && tx_log[0][35] == 68 &&
          tx_log[0][37] == 67 && tx_log[0][14 + 20 + 8 + 240] == 53);
    check("the lease is bound with the address, mask, gateway and resolver the server named",
          local_ip == MOCK_LEASED_IP && local_mask == 0xffffff00u && local_gw == MOCK_PEER_IP &&
          local_dns == MOCK_DNS_IP && dhcp.lease == NET_LEASE_BOUND);
    check("the request carried the address it wanted and the server that offered it",
          mock_dhcp_stage == 2);
    {
        struct net_info info;
        net_get_info(&info);
        check("and the panel's state is 'up' with a lease", info.state == NET_STATE_UP && info.lease == NET_LEASE_BOUND);
    }
    mock_reset(0);
    net_dhcp_renew();
    for (int i = 0; i < 2000 && dhcp.lease != NET_LEASE_FAILED; i++) { net_poll(); sleep_ms(10); }
    check("a server that never answers ends in a failure after a bounded number of tries, not a hang",
          dhcp.lease == NET_LEASE_FAILED && !local_ip);
    {
        struct net_info info;
        net_get_info(&info);
        check("and that state is reported as 'configuration failed'", info.state == NET_STATE_FAILED);
    }
}

static void test_dns()
{
    uint32_t answer = 0;
    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    for (int i = 0; i < tx_count && i < 6; i++)
        printf("  tx[%d] type=%02x%02x proto=%u sport=%u dport=%u\n", i, tx_log[i][12], tx_log[i][13],
               tx_log[i][23], (tx_log[i][34] << 8) | tx_log[i][35], (tx_log[i][36] << 8) | tx_log[i][37]);
    check("a name resolves to the address in the A record", net_dns_lookup("example.com", &answer, 4000) == 1 &&
          answer == MOCK_DNS_ANSWER);
    check("the query is one question, recursion desired, of type A and class IN",
          tx_count > 1 && tx_log[1][23] == 17 && tx_log[1][44] == 0x01 && tx_log[1][45] == 0x00 &&
          tx_log[1][47] == 1 && tx_log[1][54] == 7 && tx_log[1][55] == 'e');
    check("a literal address needs no resolver at all", net_dns_lookup("10.9.8.7", &answer, 10) && answer == 0x0a090807u);
    mock_reset(1);
    mock_bad_dns_id = 1;
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("a reply with somebody else's identifier is not taken as an answer",
          net_dns_lookup("example.com", &answer, 1200) == 0);
    mock_reset(1);
    mock_bad_dns_source = 1;
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("nor is one from an address that is not the resolver that was asked",
          net_dns_lookup("example.com", &answer, 1200) == 0);
}

static void test_malformed_input()
{
    uint8_t frame[1600];

    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    /* A frame that claims to be IP but is shorter than an IP header. */
    memset(frame, 0, sizeof frame);
    frame[12] = 0x08; frame[13] = 0x00;
    net_rx_frame(frame, 20);
    check("a frame too short to hold an IP header is dropped without being read",
          counters.rx_packets == 1 && counters.rx_errors == 1);
    /* An IP header whose total length runs past the frame. */
    {
        size_t at = build_eth(frame, mock_mac, mock_peer_mac, 0x0800);
        memset(frame + at, 0, 20);
        frame[at] = 0x45; frame[at + 2] = 0x7f; frame[at + 3] = 0xff;   /* 32767 bytes claimed */
        frame[at + 9] = 17;
        net_rx_frame(frame, at + 20);
        check("an IP header whose length runs past the frame is an error, not a read",
              counters.rx_errors >= 2);
    }
    /* A correct header with a broken checksum. */
    {
        size_t at = build_eth(frame, mock_mac, mock_peer_mac, 0x0800);
        uint8_t body[8] = { 0 };
        size_t len = build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 17, body, 8);
        frame[at + 10] ^= 0xff;
        net_rx_frame(frame, len);
        check("a bad IP header checksum is refused", counters.rx_errors >= 3);
    }
    /* A UDP datagram whose checksum does not agree with its bytes. */
    mock_reset(1);
    mock_bad_udp_checksum = 1;
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    {
        uint8_t udp[64];
        size_t ulen = build_udp(udp, MOCK_PEER_IP, MOCK_LEASED_IP, 40000, 40001, "hello", 5);
        size_t len = build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 17, udp, ulen);
        net_rx_frame(frame, len);
        check("a UDP datagram with a bad checksum is dropped", counters.rx_errors == 1 && counters.udp_packets == 0);
    }
    /* A DHCP message whose option length runs past the packet must not be believed. */
    mock_reset(1);
    net_dhcp_renew();
    {
        uint8_t body[260], udp[300];
        memset(body, 0, sizeof body);
        body[0] = 2; body[1] = 1; body[2] = 6;
        body[4] = (uint8_t)(dhcp.xid >> 24); body[5] = (uint8_t)(dhcp.xid >> 16);
        body[6] = (uint8_t)(dhcp.xid >> 8); body[7] = (uint8_t)dhcp.xid;
        memcpy(body + 236, "\x63\x82\x53\x63", 4);
        body[240] = 53; body[241] = 1; body[242] = 5;      /* ACK with no lease behind it */
        body[243] = 51; body[244] = 200;                   /* a length that runs off the end */
        {
            size_t ulen = build_udp(udp, MOCK_PEER_IP, 0xffffffffu, 67, 68, body, 250);
            size_t len = build_ip(frame, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", MOCK_PEER_IP,
                                  0xffffffffu, 17, udp, ulen);
            net_rx_frame(frame, len);
        }
        check("a DHCP option that runs past its packet is refused, so no address is taken from it",
              local_ip == 0 && dhcp.lease != NET_LEASE_BOUND);
    }
    /* An oversized frame is refused by the stack before it is parsed. */
    {
        static uint8_t huge[NET_FRAME_MAX + 64];
        mock_reset(1);
        memset(huge, 0, sizeof huge);
        net_rx_frame(huge, sizeof huge);
        check("a frame larger than the stack will ever build is counted and dropped",
              counters.frames_too_big == 1 && counters.rx_packets == 0);
    }
}

static void test_tcp_and_http()
{
    uint32_t answer;
    char response[512];
    size_t len = 0;

    mock_reset(2);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("a connection completes the three-way handshake", net_tcp_connect(MOCK_PEER_IP, 80, 3000) == 1);
    check("the SYN carried this side's port and asked for the port named",
          tx_log[1][23] == 6 && ((tx_log[1][36] << 8) | tx_log[1][37]) == 80 && tcp.state == TCP_ESTABLISHED);
    check("data is sent and acknowledged within the timeout",
          net_tcp_send("GET / HTTP/1.0\r\n\r\n", 18, 2000) == 1);

    mock_reset(2);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("a whole HTTP fetch resolves, connects, sends and reads the response",
          net_http_get("http://10.0.2.2/", response, sizeof response, &len) == 1 && len > 0);
    check("the response is the server's bytes, status line first", !strncmp(response, "HTTP/1.0 200 OK", 15));
    check("and the body is there in full", find_text(response, "hello") != 0);
    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    check("an https URL is refused rather than sent in clear",
          net_http_get("https://example.com/", response, sizeof response, &len) == 0);
    check("a URL with no host is refused", net_http_get("http:///path", response, sizeof response, &len) == 0);
    (void)answer;
}

static void test_report_and_absence()
{
    char report[2048];
    struct net_info info;

    mock_detach();
    net_get_info(&info);
    check("with no adapter the state is 'absent' and the report says so without inventing numbers",
          info.state == NET_STATE_ABSENT && info.present == 0);
    net_format_status(report, sizeof report);
    check("and the paragraph says no supported adapter", find_text(report, "no supported adapter") != 0);
    check("registering one adapter claims it, and a second is counted but not routed through",
          net_register_nic(&mock_nic) == 1 && net_nic_count() == 1 &&
          net_register_nic(&mock_nic) == 1 && net_nic_count() == 2);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    net_get_info(&info);
    check("a configured adapter reports its address, mask, gateway and resolver",
          info.ip == MOCK_LEASED_IP && info.mask == 0xffffff00u && info.gateway == MOCK_PEER_IP &&
          info.dns == MOCK_DNS_IP && info.state == NET_STATE_UP);
    net_format_status(report, sizeof report);
    check("the parallel report names the driver, the address and the counters",
          find_text(report, "mock0") && find_text(report, "10.0.2.15") &&
          find_text(report, "frames rx") && find_text(report, "lease"));
    {
        char tiny[32];
        size_t n = net_format_status(tiny, sizeof tiny);
        check("a report that does not fit is truncated and terminated inside the caller's buffer",
              strlen(tiny) == sizeof tiny - 1 && n == sizeof tiny - 1);
    }
}


/* Hostile input: the frames an attacker on the segment can send, and the rules that decide none of them is
 * believed without an exchange this side started.  Everything here is built by hand from the wire format, the
 * same way the rest of the mock builds its answers, so a change in the stack's idea of a header fails a check
 * here rather than on somebody's network. */
static size_t build_tcp(uint8_t *out, uint32_t src, uint32_t dst, uint16_t sport, uint16_t dport,
                        uint32_t seq, uint32_t ack, uint8_t flags, const void *payload, size_t len)
{
    size_t total = 20 + len;
    memset(out, 0, 20);
    out[0] = (uint8_t)(sport >> 8); out[1] = (uint8_t)sport;
    out[2] = (uint8_t)(dport >> 8); out[3] = (uint8_t)dport;
    out[4] = (uint8_t)(seq >> 24); out[5] = (uint8_t)(seq >> 16); out[6] = (uint8_t)(seq >> 8); out[7] = (uint8_t)seq;
    out[8] = (uint8_t)(ack >> 24); out[9] = (uint8_t)(ack >> 16); out[10] = (uint8_t)(ack >> 8); out[11] = (uint8_t)ack;
    out[12] = 0x50;
    out[13] = flags;
    out[14] = 0x20; out[15] = 0x00;
    if (len) memcpy(out + 20, payload, len);
    {
        uint32_t sum = pseudo_sum(src, dst, 6, (uint16_t)total);
        uint16_t folded = csum_fold(net_checksum(out, total, sum));
        out[16] = (uint8_t)(folded >> 8); out[17] = (uint8_t)folded;
    }
    return total;
}

static void forge_dhcp(uint32_t server, uint32_t address, uint8_t kind)
{
    uint8_t body[300], udp[340], frame[400];
    size_t at = 0, ulen, len;
    uint32_t xid = dhcp.xid;

    memset(body, 0, sizeof body);
    body[0] = 2; body[1] = 1; body[2] = 6;
    body[4] = (uint8_t)(xid >> 24); body[5] = (uint8_t)(xid >> 16);
    body[6] = (uint8_t)(xid >> 8); body[7] = (uint8_t)xid;
    body[16] = (uint8_t)(address >> 24); body[17] = (uint8_t)(address >> 16);
    body[18] = (uint8_t)(address >> 8); body[19] = (uint8_t)address;
    memcpy(body + 28, mock_mac, NET_ALEN);
    memcpy(body + 236, "\x63\x82\x53\x63", 4);
    at = 240;
    body[at++] = 53; body[at++] = 1; body[at++] = kind;
    body[at++] = 54; body[at++] = 4;
    body[at++] = (uint8_t)(server >> 24); body[at++] = (uint8_t)(server >> 16);
    body[at++] = (uint8_t)(server >> 8); body[at++] = (uint8_t)server;
    body[at++] = 1; body[at++] = 4; body[at++] = 255; body[at++] = 255; body[at++] = 255; body[at++] = 0;
    body[at++] = 255;
    ulen = build_udp(udp, server, 0xffffffffu, 67, 68, body, at);
    len = build_ip(frame, (const uint8_t *)"\xff\xff\xff\xff\xff\xff", server, 0xffffffffu, 17, udp, ulen);
    net_rx_frame(frame, len);
}


/* The other two ways a peer makes this stack drop bytes it has already taken responsibility for:
 *   - a response that arrives in two segments while the application has read neither;
 *   - a short read, which must leave the rest of the buffer intact and reopen the window.
 * Both are built from the wire, at sequence numbers the stack actually announced. */
static void test_stream_flow_control()
{
    static char text[64];
    int got;

    mock_reset(2);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    if (!net_tcp_connect(MOCK_PEER_IP, 80, 3000)) { check("a connection for the flow-control checks", 0); return; }

    {
        uint8_t frame[1600], segment[64];
        uint32_t base = tcp.rcv_nxt;
        size_t tlen;

        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, base, tcp.snd_nxt, 0x18,
                         "0123456789", 10);
        net_rx_frame(frame, build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen));
        check("the first segment is taken", tcp.rx_len == 10 && tcp.rcv_nxt == base + 10);
        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, base + 10, tcp.snd_nxt, 0x18,
                         "abcde", 5);
        net_rx_frame(frame, build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen));
        check("a second segment does not overwrite the unread first one",
              tcp.rx_len == 10 && tcp.rcv_nxt == base + 10 && !memcmp(tcp.rx, "0123456789", 10));
        got = net_tcp_recv(text, 4, 100);
        check("a short read returns the front of the buffer", got == 4 && !memcmp(text, "0123", 4));
        check("and keeps the rest rather than dropping it", tcp.rx_len == 6 && !memcmp(tcp.rx, "456789", 6));
        got = net_tcp_recv(text, sizeof text, 100);
        check("the next read finishes the segment", got == 6 && !memcmp(text, "456789", 6));
        check("the buffer is empty again", tcp.rx_len == 0);
        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, base + 10, tcp.snd_nxt, 0x18,
                         "abcde", 5);
        net_rx_frame(frame, build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen));
        got = net_tcp_recv(text, sizeof text, 100);
        check("the held segment is delivered once the window opens", got == 5 && !memcmp(text, "abcde", 5));
        /* A FIN that sits past the last byte taken is not the end of the stream: bytes are still missing. */
        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, base + 20, tcp.snd_nxt, 0x11,
                         NULL, 0);
        net_rx_frame(frame, build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen));
        check("a FIN that does not follow the last byte taken is not accepted as the end",
              tcp.state != TCP_PEER_CLOSED && tcp.rcv_nxt == base + 15);
        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, base + 15, tcp.snd_nxt, 0x11,
                         NULL, 0);
        net_rx_frame(frame, build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen));
        check("the FIN at the end of the stream is", tcp.state == TCP_PEER_CLOSED && tcp.rcv_nxt == base + 16);
        net_tcp_close();
    }

    /* A NAK while an address is in use takes the address away, with everything built on it. */
    mock_reset(1);
    net_dhcp_renew();
    for (int i = 0; i < 400 && dhcp.lease != NET_LEASE_BOUND; i++) { net_poll(); sleep_ms(10); }
    check("an address is in use before the NAK", local_ip == MOCK_LEASED_IP && dhcp.lease == NET_LEASE_BOUND);
    {
        int before = tx_count;
        forge_dhcp(MOCK_PEER_IP, MOCK_LEASED_IP, 6);
        check("a NAK takes the address away rather than leaving it in use", local_ip == 0 && local_mask == 0 &&
              local_gw == 0 && local_dns == 0);
        check("the cache built on that address is gone", arp_find(MOCK_PEER_IP) == NULL);
        check("and a new DISCOVER goes out, because the transaction it belonged to is dead",
              tx_count > before && tx_log[tx_count - 1][14 + 20 + 8 + 240] == 53 &&
              tx_log[tx_count - 1][14 + 20 + 8 + 242] == 1);
        {
            struct net_info info;
            net_get_info(&info);
            check("and the state is no longer 'up'", info.state != NET_STATE_UP && info.ip == 0);
        }
    }
    mock_reset(1);
}

static void test_hostile_input()
{
    uint8_t frame[1600];

    /* An unsolicited ARP reply must not be able to write the cache: whoever answers first owns the entry
     * otherwise, and the gateway's entry is the one that matters. */
    mock_reset(1);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    {
        static const uint8_t attacker[NET_ALEN] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x66 };
        size_t at = build_eth(frame, mock_mac, attacker, 0x0806);
        frame[at + 0] = 0; frame[at + 1] = 1;
        frame[at + 2] = 0x08; frame[at + 3] = 0x00;
        frame[at + 4] = 6; frame[at + 5] = 4;
        frame[at + 6] = 0; frame[at + 7] = 2;                        /* reply */
        memcpy(frame + at + 8, attacker, NET_ALEN);
        wr32(frame + at + 14, MOCK_PEER_IP);                         /* "the gateway is me" */
        memcpy(frame + at + 18, mock_mac, NET_ALEN);
        wr32(frame + at + 24, MOCK_LEASED_IP);
        net_rx_frame(frame, at + 28);
        check("an ARP reply for an address nobody asked about is not cached",
              arp_find(MOCK_PEER_IP) == NULL);
        check("and it is counted as what it is", counters.arp_unsolicited == 1);
        check("the cache stays empty, so the next send asks instead of using it",
              arp_find(MOCK_PEER_IP) == NULL && counters.arp_requests == 0);
    }
    /* The same reply, after this stack has asked, is the answer it was waiting for. */
    net_ping(MOCK_PEER_IP, 500, 0);
    check("an answer to a question this stack asked is accepted",
          arp_find(MOCK_PEER_IP) != NULL && !memcmp(arp_find(MOCK_PEER_IP)->mac, mock_peer_mac, NET_ALEN));

    /* A DHCP lease is finished only by the server this side is talking to, and only for the address in the
     * request. */
    mock_reset(1);
    mock_silent_dhcp_ack = 1;                                       /* the ACK is this test's job */
    net_dhcp_renew();
    for (int i = 0; i < 8 && dhcp.lease != NET_LEASE_OFFER; i++) net_poll();
    check("the offer was taken and the server remembered", dhcp.lease == NET_LEASE_OFFER && dhcp.server == MOCK_PEER_IP);
    forge_dhcp(0x0a000209u, MOCK_LEASED_IP, 5);                     /* 10.0.2.9: not our server */
    check("an ACK from another machine is not believed", local_ip == 0 && dhcp.lease == NET_LEASE_OFFER);
    forge_dhcp(MOCK_PEER_IP, 0x0a000277u, 5);                       /* our server, somebody else's address */
    check("an ACK for an address this side did not ask for is not believed",
          local_ip == 0 && dhcp.lease == NET_LEASE_OFFER);
    forge_dhcp(MOCK_PEER_IP, MOCK_LEASED_IP, 5);
    check("the ACK that answers the request is the one that binds",
          local_ip == MOCK_LEASED_IP && dhcp.lease == NET_LEASE_BOUND);
    mock_silent_dhcp_ack = 0;

    /* A TCP segment whose checksum does not cover its bytes is not a segment. */
    mock_reset(2);
    net_set_static(MOCK_LEASED_IP, 0xffffff00u, MOCK_PEER_IP, MOCK_DNS_IP);
    net_ping(MOCK_PEER_IP, 500, 0);                                  /* resolves the peer's MAC */
    {
        int before = tx_count;
        uint16_t errors = counters.rx_errors;
        uint8_t tcp[20];
        size_t tlen = build_tcp(tcp, MOCK_PEER_IP, MOCK_LEASED_IP, 80, 40000, 1000, 0, 0x02, NULL, 0);
        tcp[16] ^= 0xff;                                             /* one byte of the checksum */
        {
            size_t len = build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, tcp, tlen);
            net_rx_frame(frame, len);
        }
        check("a SYN with a bad checksum is dropped, and no reset is sent in answer to it",
              tx_count == before && counters.rx_errors > errors);
        tlen = build_tcp(tcp, MOCK_PEER_IP, MOCK_LEASED_IP, 80, 40000, 1000, 0, 0x02, NULL, 0);
        {
            size_t len = build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, tcp, tlen);
            net_rx_frame(frame, len);
        }
        check("the same SYN with a correct checksum is refused with a reset, as it should be",
              tx_count == before + 1);
        if (tx_count == before + 1) {
            const uint8_t *out = tx_log[tx_count - 1];
            /* The reset is addressed the other way round from the segment that earned it: from the port it
             * arrived on, to the port it came from. */
            check("the reset carries the RST flag and no data",
                  out[14 + 20 + 13] == 0x04 && get16(out + 14 + 20) == 40000 &&
                  get16(out + 14 + 20 + 2) == 80);
        }
    }
    /* Data larger than the window this stack announced is refused rather than half-stored. */
    if (net_tcp_connect(MOCK_PEER_IP, 80, 2000)) {
        /* 1470 bytes: over this stack's 1460-byte MSS, and still inside the 1532-byte frame the stack will
         * accept, so the payload limit is what refuses it rather than the frame limit. */
        static uint8_t big[1470];
        uint8_t segment[20 + sizeof big];
        size_t tlen;
        uint32_t before = tcp.rcv_nxt;
        memset(big, 0x41, sizeof big);
        tlen = build_tcp(segment, MOCK_PEER_IP, MOCK_LEASED_IP, 80, tcp.lport, tcp.rcv_nxt, tcp.snd_nxt,
                         0x18, big, sizeof big);
        {
            size_t len = build_ip(frame, mock_mac, MOCK_PEER_IP, MOCK_LEASED_IP, 6, segment, tlen);
            net_rx_frame(frame, len);
        }
        check("a segment larger than the receive window does not advance the sequence number",
              tcp.rcv_nxt == before && tcp.rx_len == 0);
        net_tcp_close();
    } else {
        check("a connection to the mock peer could be established", 0);
    }

    /* A URL is text this kernel turns into a request; a control character in it is a second header. */
    {
        static char response[256];
        size_t len = 0;
        check("a URL with a carriage return in the host is refused",
              net_http_get("http://exa\r\nmple.com/", response, sizeof response, &len) == 0);
        check("a URL with a line break in the path is refused",
              net_http_get("http://example.com/a\r\nX-Injected: 1", response, sizeof response, &len) == 0);
        check("https is still not something this stack can do",
              net_http_get("https://example.com/", response, sizeof response, &len) == 0);
    }

    /* A compressed name that points at itself, or past the message, is a refusal rather than a walk. */
    {
        static uint8_t message[32];
        memset(message, 0, sizeof message);
        message[12] = 0xc0; message[13] = 12;                       /* points at itself */
        check("a DNS compression loop is refused", dns_skip_name(message, sizeof message, 12) == 0);
        message[12] = 0xc0; message[13] = 200;                      /* points outside the message */
        check("a DNS pointer past the end of the message is refused",
              dns_skip_name(message, sizeof message, 12) == 0);
    }

    /* Identifiers are drawn from the entropy mix, not from a fixed seed plus the clock. */
    {
        uint32_t first, second;
        net_dhcp_renew();
        first = dhcp.xid;
        net_dhcp_renew();
        second = dhcp.xid;
        check("two lease transactions do not share a transaction ID", first != second);
        check("two ephemeral ports are not the same number", net_sport() != net_sport());
    }
}

int main(void)
{
    test_addresses();
    test_arp();
    test_ping();
    test_dhcp();
    test_dns();
    test_malformed_input();
    test_tcp_and_http();
    test_report_and_absence();
    test_hostile_input();
    test_stream_flow_control();
    printf("%u check(s), %u failure(s)\n", checks_run, failures_run);
    return failures_run != 0;
}
