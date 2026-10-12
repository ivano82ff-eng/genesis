/* genesis TCP/IP: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, DNS and a small TCP.
 *
 * Everything is polled: link_poll() (kernel/link.c) pulls frames off the card
 * and hands every non-Genesis-Link frame to net_input(). Blocking calls
 * (ping, DHCP, DNS, TCP) spin on link_poll() and the 100 Hz timer.
 *
 * TCP is deliberately simple: active open only, one segment in flight
 * (stop-and-wait with retransmission), in-order receive into a 16 KiB ring
 * whose free space is the advertised window. Enough for HTTP and chat. */

extern void console_puts(const char *text);
extern void console_putc(char ch);
extern void link_poll(void);
extern int link_polling(void);
extern int nic_send(const unsigned char *frame, unsigned int length);
extern const char *nic_info(unsigned char *out_mac);
extern unsigned int clock_ticks_total(void);

/* ---------- small helpers ---------- */

static unsigned int now_ms(void) {
    return clock_ticks_total() * 10u;
}

static void idle(void) {
    link_poll();
    unsigned long flags;
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0" : "=r"(flags));
#else
    __asm__ volatile("pushfl; popl %0" : "=r"(flags));
#endif
    if (flags & 0x200ul) {
        __asm__ volatile("hlt");
    }
}

static unsigned int be16(const unsigned char *p) {
    return ((unsigned int)p[0] << 8) | p[1];
}

static unsigned int be32(const unsigned char *p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static void put16(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
}

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static void copy(unsigned char *d, const unsigned char *s, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) d[i] = s[i];
}

static int same(const unsigned char *a, const unsigned char *b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static unsigned int sum_add(unsigned int sum, const unsigned char *p, unsigned int n) {
    for (unsigned int i = 0; i + 1 < n; i += 2) sum += be16(p + i);
    if (n & 1) sum += (unsigned int)p[n - 1] << 8;
    return sum;
}

static unsigned int sum_fold(unsigned int sum) {
    while (sum >> 16) sum = (sum & 0xffffu) + (sum >> 16);
    return ~sum & 0xffffu;
}

static void put_dec(unsigned int v) {
    char b[12];
    int n = 0;
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n) console_putc(b[--n]);
}

static void put_ip(const unsigned char *ip) {
    for (int i = 0; i < 4; i++) {
        put_dec(ip[i]);
        if (i < 3) console_putc('.');
    }
}

static void put_mac(const unsigned char *m) {
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 6; i++) {
        console_putc(hex[m[i] >> 4]);
        console_putc(hex[m[i] & 15]);
        if (i < 5) console_putc(':');
    }
}

/* "a.b.c.d" -> ip; returns the characters used, 0 if it is not an address */
static int parse_ip(const char *s, unsigned char *ip) {
    const char *start = s;
    for (int i = 0; i < 4; i++) {
        unsigned int v = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') { v = v * 10u + (unsigned int)(*s++ - '0'); digits++; }
        if (!digits || v > 255 || digits > 3) return 0;
        ip[i] = (unsigned char)v;
        if (i < 3) {
            if (*s != '.') return 0;
            s++;
        }
    }
    if (*s && *s != ' ' && *s != ':' && *s != '/') return 0;
    return (int)(s - start);
}

/* ---------- interface state ---------- */

static unsigned char my_mac[6];
static unsigned char my_ip[4], my_mask[4], my_gw[4], my_dns[4];
static int configured;
static unsigned short ip_ident = 1;
static const unsigned char bcast_mac[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

static int have_card(void) {
    return nic_info(my_mac) != 0;
}

/* ---------- ARP ---------- */

struct arp_entry { unsigned char ip[4], mac[6]; int used; };
static struct arp_entry arp_table[16];
static int arp_next;

static void arp_learn(const unsigned char *ip, const unsigned char *mac) {
    for (int i = 0; i < 16; i++) {
        if (arp_table[i].used && same(arp_table[i].ip, ip, 4)) {
            copy(arp_table[i].mac, mac, 6);
            return;
        }
    }
    struct arp_entry *e = &arp_table[arp_next];
    arp_next = (arp_next + 1) % 16;
    e->used = 1;
    copy(e->ip, ip, 4);
    copy(e->mac, mac, 6);
}

static int arp_find(const unsigned char *ip, unsigned char *mac) {
    for (int i = 0; i < 16; i++) {
        if (arp_table[i].used && same(arp_table[i].ip, ip, 4)) {
            copy(mac, arp_table[i].mac, 6);
            return 1;
        }
    }
    return 0;
}

static void arp_send(int op, const unsigned char *dst_mac, const unsigned char *target_mac,
                     const unsigned char *target_ip) {
    unsigned char f[60] = {0};
    copy(f, dst_mac, 6);
    copy(f + 6, my_mac, 6);
    put16(f + 12, 0x0806);
    put16(f + 14, 1);
    put16(f + 16, 0x0800);
    f[18] = 6;
    f[19] = 4;
    put16(f + 20, (unsigned int)op);
    copy(f + 22, my_mac, 6);
    copy(f + 28, my_ip, 4);
    copy(f + 32, target_mac, 6);
    copy(f + 38, target_ip, 4);
    nic_send(f, 60);
}

static void arp_input(const unsigned char *f, unsigned int len) {
    if (len < 42 || be16(f + 14) != 1 || be16(f + 16) != 0x0800) return;
    unsigned int op = be16(f + 20);
    arp_learn(f + 28, f + 22);
    if (op == 1 && configured && same(f + 38, my_ip, 4)) {
        arp_send(2, f + 22, f + 22, f + 28);
    }
}

static int on_link(const unsigned char *ip) {
    for (int i = 0; i < 4; i++) {
        if ((ip[i] & my_mask[i]) != (my_ip[i] & my_mask[i])) return 0;
    }
    return 1;
}

/* MAC for the next hop to IP (the gateway when IP is off-link). */
static int resolve_mac(const unsigned char *ip, unsigned char *mac) {
    static const unsigned char all[4] = {255, 255, 255, 255};
    if (same(ip, all, 4)) {
        copy(mac, bcast_mac, 6);
        return 1;
    }
    const unsigned char *hop = on_link(ip) ? ip : my_gw;
    if (arp_find(hop, mac)) return 1;
    static const unsigned char zero[6] = {0};
    if (link_polling()) {
        /* Called while answering a frame (interrupts off, the card busy):
         * cannot wait here. Ask, drop this packet; the sender retries. */
        arp_send(1, bcast_mac, zero, hop);
        return 0;
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        arp_send(1, bcast_mac, zero, hop);
        unsigned int until = now_ms() + 1000;
        while ((int)(until - now_ms()) > 0) {
            idle();
            if (arp_find(hop, mac)) return 1;
        }
    }
    return 0;
}

/* ---------- IPv4 ---------- */

/* Send PAYLOAD as an IPv4 packet. Resolving the next hop may poll the card
 * (and so answer other packets), so every caller keeps its own buffer. */
static int ip_send(const unsigned char *dst, int proto, const unsigned char *payload, unsigned int len) {
    unsigned char mac[6];
    static unsigned char f[1514]; /* filled and sent with no polling in between */
    if (len > 1480) return 0;
    if (!resolve_mac(dst, mac)) return 0;
    copy(f + 34, payload, len);
    copy(f, mac, 6);
    copy(f + 6, my_mac, 6);
    put16(f + 12, 0x0800);
    unsigned char *ip = f + 14;
    ip[0] = 0x45;
    ip[1] = 0;
    put16(ip + 2, 20 + len);
    put16(ip + 4, ip_ident++);
    put16(ip + 6, 0x4000); /* don't fragment */
    ip[8] = 64;
    ip[9] = (unsigned char)proto;
    put16(ip + 10, 0);
    copy(ip + 12, my_ip, 4);
    copy(ip + 16, dst, 4);
    put16(ip + 10, sum_fold(sum_add(0, ip, 20)));
    return nic_send(f, 34 + len);
}

/* ---------- ICMP ---------- */

static int echo_seen;
static unsigned int echo_seq_seen;

static void icmp_input(const unsigned char *ip, const unsigned char *p, unsigned int len) {
    if (len < 8) return;
    if (p[0] == 8) { /* echo request: answer it */
        static unsigned char r[1480];
        if (len > 1480) return;
        copy(r, p, len);
        r[0] = 0;
        put16(r + 2, 0);
        put16(r + 2, sum_fold(sum_add(0, r, len)));
        unsigned char src[4];
        copy(src, ip + 12, 4);
        ip_send(src, 1, r, len);
    } else if (p[0] == 0 && be16(p + 4) == 0x6e65) {
        echo_seen = 1;
        echo_seq_seen = be16(p + 6);
    }
}

/* ---------- UDP ---------- */

enum { UDP_SOCKS = 8, UDP_QUEUE = 4 };
struct udp_dgram { unsigned char src[4]; unsigned int port, len; unsigned char data[1472]; };
struct udp_sock { int used; unsigned int port; int head, count; struct udp_dgram q[UDP_QUEUE]; };
static struct udp_sock udp_socks[UDP_SOCKS];
static unsigned int next_port = 49152;

static unsigned int ephemeral(void) {
    next_port++;
    if (next_port > 65000) next_port = 49152;
    return next_port;
}

int udp_open(unsigned int port) {
    for (int i = 0; i < UDP_SOCKS; i++) {
        if (!udp_socks[i].used) {
            udp_socks[i].used = 1;
            udp_socks[i].port = port ? port : ephemeral();
            udp_socks[i].head = udp_socks[i].count = 0;
            return i;
        }
    }
    return -1;
}

void udp_close(int s) {
    if (s >= 0 && s < UDP_SOCKS) udp_socks[s].used = 0;
}

int udp_send(int s, const unsigned char *dst, unsigned int port, const unsigned char *data, unsigned int len) {
    if (s < 0 || s >= UDP_SOCKS || !udp_socks[s].used || len > 1472) return -1;
    unsigned char u[1480];
    put16(u, udp_socks[s].port);
    put16(u + 2, port);
    put16(u + 4, 8 + len);
    put16(u + 6, 0);
    copy(u + 8, data, len);
    unsigned int sum = sum_add(0, my_ip, 4);
    sum = sum_add(sum, dst, 4);
    sum += 17 + 8 + len;
    sum = sum_fold(sum_add(sum, u, 8 + len));
    put16(u + 6, sum ? sum : 0xffff);
    return ip_send(dst, 17, u, 8 + len) ? (int)len : -1;
}

/* Wait up to MS for a datagram; returns its length, -1 on timeout. */
int udp_recv(int s, unsigned char *buf, unsigned int max, unsigned char *src, unsigned int *port, unsigned int ms) {
    if (s < 0 || s >= UDP_SOCKS || !udp_socks[s].used) return -1;
    struct udp_sock *k = &udp_socks[s];
    unsigned int until = now_ms() + ms;
    while (k->count == 0) {
        if ((int)(until - now_ms()) <= 0) return -1;
        idle();
    }
    struct udp_dgram *d = &k->q[k->head];
    unsigned int n = d->len < max ? d->len : max;
    copy(buf, d->data, n);
    if (src) copy(src, d->src, 4);
    if (port) *port = d->port;
    k->head = (k->head + 1) % UDP_QUEUE;
    k->count--;
    return (int)n;
}

static void udp_input(const unsigned char *ip, const unsigned char *u, unsigned int len) {
    if (len < 8) return;
    unsigned int ulen = be16(u + 4);
    if (ulen < 8 || ulen > len) return;
    unsigned int dport = be16(u + 2);
    for (int i = 0; i < UDP_SOCKS; i++) {
        struct udp_sock *k = &udp_socks[i];
        if (!k->used || k->port != dport) continue;
        if (k->count == UDP_QUEUE) return;
        struct udp_dgram *d = &k->q[(k->head + k->count) % UDP_QUEUE];
        copy(d->src, ip + 12, 4);
        d->port = be16(u);
        d->len = ulen - 8;
        copy(d->data, u + 8, d->len);
        k->count++;
        return;
    }
}

/* ---------- TCP ---------- */

enum { T_CLOSED, T_SYN_SENT, T_EST, T_FIN_WAIT1, T_FIN_WAIT2, T_CLOSE_WAIT, T_LAST_ACK, T_DONE };
enum { TCP_SOCKS = 4, TCP_RX = 16384, TCP_MSS = 1024 };
struct tcp_sock {
    int state;
    int reset;
    unsigned int lport, rport;
    unsigned char rip[4];
    unsigned int snd_una, snd_nxt, rcv_nxt;
    unsigned int peer_win;
    int fin_seen;
    unsigned int rx_head, rx_count;
    unsigned char rx[TCP_RX];
};
static struct tcp_sock tcp_socks[TCP_SOCKS];

static void tcp_out(struct tcp_sock *t, unsigned int flags, unsigned int seq, const unsigned char *data,
                    unsigned int len) {
    unsigned char h[1480];
    unsigned int hl = (flags & 0x02) ? 24 : 20;
    put16(h, t->lport);
    put16(h + 2, t->rport);
    put32(h + 4, seq);
    put32(h + 8, (flags & 0x10) ? t->rcv_nxt : 0);
    h[12] = (unsigned char)((hl / 4) << 4);
    h[13] = (unsigned char)flags;
    unsigned int win = TCP_RX - t->rx_count;
    put16(h + 14, win > 65535 ? 65535 : win);
    put16(h + 16, 0);
    put16(h + 18, 0);
    if (hl == 24) { /* MSS option */
        h[20] = 2;
        h[21] = 4;
        put16(h + 22, TCP_MSS);
    }
    copy(h + hl, data, len);
    unsigned int sum = sum_add(0, my_ip, 4);
    sum = sum_add(sum, t->rip, 4);
    sum += 6 + hl + len;
    put16(h + 16, sum_fold(sum_add(sum, h, hl + len)));
    ip_send(t->rip, 6, h, hl + len);
}

static void tcp_input(const unsigned char *ip, const unsigned char *h, unsigned int len) {
    if (len < 20) return;
    unsigned int hl = (unsigned int)(h[12] >> 4) * 4u;
    if (hl < 20 || hl > len) return;
    unsigned int sport = be16(h), dport = be16(h + 2);
    unsigned int seq = be32(h + 4), ack = be32(h + 8);
    unsigned int flags = h[13];
    struct tcp_sock *t = 0;
    for (int i = 0; i < TCP_SOCKS; i++) {
        struct tcp_sock *k = &tcp_socks[i];
        if (k->state != T_CLOSED && k->lport == dport && k->rport == sport && same(k->rip, ip + 12, 4)) {
            t = k;
            break;
        }
    }
    if (!t) return;
    const unsigned char *data = h + hl;
    unsigned int dlen = len - hl;
    if (flags & 0x04) { /* RST */
        t->reset = 1;
        t->state = T_DONE;
        return;
    }
    if (t->state == T_SYN_SENT) {
        if ((flags & 0x12) == 0x12 && ack == t->snd_nxt) {
            t->rcv_nxt = seq + 1;
            t->snd_una = ack;
            t->peer_win = be16(h + 14);
            t->state = T_EST;
            tcp_out(t, 0x10, t->snd_nxt, 0, 0);
        }
        return;
    }
    if (flags & 0x10) {
        if ((int)(ack - t->snd_una) > 0 && (int)(ack - t->snd_nxt) <= 0) t->snd_una = ack;
        t->peer_win = be16(h + 14);
        if (t->state == T_FIN_WAIT1 && t->snd_una == t->snd_nxt) t->state = T_FIN_WAIT2;
        if (t->state == T_LAST_ACK && t->snd_una == t->snd_nxt) t->state = T_DONE;
    }
    int need_ack = 0;
    if (dlen > 0) {
        need_ack = 1;
        if (seq == t->rcv_nxt && dlen <= TCP_RX - t->rx_count) {
            for (unsigned int i = 0; i < dlen; i++) {
                t->rx[(t->rx_head + t->rx_count + i) % TCP_RX] = data[i];
            }
            t->rx_count += dlen;
            t->rcv_nxt += dlen;
        }
    }
    if ((flags & 0x01) && seq + dlen == t->rcv_nxt && !t->fin_seen) {
        t->fin_seen = 1;
        t->rcv_nxt++;
        need_ack = 1;
        if (t->state == T_EST) t->state = T_CLOSE_WAIT;
        else if (t->state == T_FIN_WAIT1 || t->state == T_FIN_WAIT2) t->state = T_DONE;
    }
    if (need_ack) tcp_out(t, 0x10, t->snd_nxt, 0, 0);
}

int tcp_open(const unsigned char *ip, unsigned int port) {
    struct tcp_sock *t = 0;
    int idx = -1;
    for (int i = 0; i < TCP_SOCKS; i++) {
        if (tcp_socks[i].state == T_CLOSED) { t = &tcp_socks[i]; idx = i; break; }
    }
    if (!t || !configured) return -1;
    t->reset = 0;
    t->fin_seen = 0;
    t->rx_head = t->rx_count = 0;
    t->lport = ephemeral();
    t->rport = port;
    copy(t->rip, ip, 4);
    t->snd_una = clock_ticks_total() * 2654435761u;
    t->snd_nxt = t->snd_una + 1;
    t->rcv_nxt = 0;
    t->state = T_SYN_SENT;
    for (int attempt = 0; attempt < 4; attempt++) {
        tcp_out(t, 0x02, t->snd_una, 0, 0);
        unsigned int until = now_ms() + 1000u * (unsigned int)(attempt + 1);
        while ((int)(until - now_ms()) > 0) {
            idle();
            if (t->state == T_EST) return idx;
            if (t->state == T_DONE) { t->state = T_CLOSED; return -1; }
        }
    }
    t->state = T_CLOSED;
    return -1;
}

static struct tcp_sock *tcp_get(int s) {
    if (s < 0 || s >= TCP_SOCKS || tcp_socks[s].state == T_CLOSED) return 0;
    return &tcp_socks[s];
}

int tcp_send(int s, const unsigned char *data, unsigned int len) {
    struct tcp_sock *t = tcp_get(s);
    if (!t) return -1;
    unsigned int done = 0;
    while (done < len) {
        if (t->state != T_EST && t->state != T_CLOSE_WAIT) return -1;
        unsigned int n = len - done;
        if (n > TCP_MSS) n = TCP_MSS;
        if (t->peer_win && n > t->peer_win) n = t->peer_win;
        unsigned int seq = t->snd_nxt;
        t->snd_nxt += n;
        int acked = 0;
        for (int attempt = 0; attempt < 8 && !acked; attempt++) {
            tcp_out(t, 0x18, seq, data + done, n);
            unsigned int until = now_ms() + 500u * (unsigned int)(attempt + 1);
            while ((int)(until - now_ms()) > 0) {
                idle();
                if (t->snd_una == t->snd_nxt) { acked = 1; break; }
                if (t->state == T_DONE) return -1;
            }
        }
        if (!acked) return -1;
        done += n;
    }
    return (int)len;
}

/* Up to MAX bytes; 0 when the peer has closed and everything is read,
 * -2 on timeout, -1 on error/reset. */
int tcp_recv(int s, unsigned char *buf, unsigned int max, unsigned int ms) {
    struct tcp_sock *t = tcp_get(s);
    if (!t) return -1;
    unsigned int until = now_ms() + ms;
    while (t->rx_count == 0) {
        if (t->fin_seen || t->state == T_DONE) return t->reset ? -1 : 0;
        if ((int)(until - now_ms()) <= 0) return -2;
        idle();
    }
    int was_full = t->rx_count > TCP_RX - TCP_MSS;
    unsigned int n = t->rx_count < max ? t->rx_count : max;
    for (unsigned int i = 0; i < n; i++) buf[i] = t->rx[(t->rx_head + i) % TCP_RX];
    t->rx_head = (t->rx_head + n) % TCP_RX;
    t->rx_count -= n;
    if (was_full && t->state != T_DONE) tcp_out(t, 0x10, t->snd_nxt, 0, 0); /* window update */
    return (int)n;
}

void tcp_close(int s) {
    struct tcp_sock *t = tcp_get(s);
    if (!t) return;
    if (t->state == T_EST || t->state == T_CLOSE_WAIT) {
        t->state = t->state == T_EST ? T_FIN_WAIT1 : T_LAST_ACK;
        unsigned int seq = t->snd_nxt;
        t->snd_nxt++;
        for (int attempt = 0; attempt < 3; attempt++) {
            tcp_out(t, 0x11, seq, 0, 0);
            unsigned int until = now_ms() + 500;
            while ((int)(until - now_ms()) > 0) {
                idle();
                if (t->state == T_DONE || t->state == T_FIN_WAIT2) break;
            }
            if (t->state == T_DONE || t->state == T_FIN_WAIT2) break;
        }
    }
    t->state = T_CLOSED;
}

/* ---------- frame input ---------- */

void net_input(const unsigned char *f, unsigned int len) {
    if (len < 14) return;
    unsigned int type = be16(f + 12);
    if (type == 0x0806) {
        arp_input(f, len);
        return;
    }
    if (type != 0x0800 || len < 34) return;
    const unsigned char *ip = f + 14;
    if ((ip[0] >> 4) != 4) return;
    unsigned int ihl = (unsigned int)(ip[0] & 15) * 4u;
    unsigned int total = be16(ip + 2);
    if (ihl < 20 || total < ihl || 14 + total > len) return;
    if ((be16(ip + 6) & 0x3fff) != 0) return; /* fragments: not supported */
    static const unsigned char all[4] = {255, 255, 255, 255};
    int for_us = configured ? same(ip + 16, my_ip, 4) : 0;
    if (!for_us && !same(ip + 16, all, 4) && !(ip[9] == 17 && !configured)) return;
    if (sum_fold(sum_add(0, ip, ihl)) != 0) return;
    if (configured && for_us) arp_learn(ip + 12, f + 6); /* replies come back the same way */
    const unsigned char *p = ip + ihl;
    unsigned int plen = total - ihl;
    if (ip[9] == 1 && for_us) icmp_input(ip, p, plen);
    else if (ip[9] == 17) udp_input(ip, p, plen);
    else if (ip[9] == 6 && for_us) tcp_input(ip, p, plen);
}

/* ---------- DHCP ---------- */

static int dhcp_option(const unsigned char *opt, unsigned int len, int code, unsigned char *out, unsigned int want) {
    unsigned int i = 0;
    while (i < len) {
        int c = opt[i];
        if (c == 255) break;
        if (c == 0) { i++; continue; }
        if (i + 1 >= len) break;
        unsigned int l = opt[i + 1];
        if (i + 2 + l > len) break;
        if (c == code) {
            if (out) copy(out, opt + i + 2, l < want ? l : want);
            return (int)l;
        }
        i += 2 + l;
    }
    return -1;
}

static unsigned int dhcp_build(unsigned char *b, unsigned int xid, int type, const unsigned char *req_ip,
                               const unsigned char *server) {
    for (int i = 0; i < 300; i++) b[i] = 0;
    b[0] = 1;
    b[1] = 1;
    b[2] = 6;
    put32(b + 4, xid);
    put16(b + 10, 0x8000); /* broadcast replies: we have no address yet */
    copy(b + 28, my_mac, 6);
    put32(b + 236, 0x63825363);
    unsigned int n = 240;
    b[n++] = 53; b[n++] = 1; b[n++] = (unsigned char)type;
    if (req_ip) { b[n++] = 50; b[n++] = 4; copy(b + n, req_ip, 4); n += 4; }
    if (server) { b[n++] = 54; b[n++] = 4; copy(b + n, server, 4); n += 4; }
    b[n++] = 55; b[n++] = 3; b[n++] = 1; b[n++] = 3; b[n++] = 6;
    b[n++] = 255;
    return n < 300 ? 300 : n;
}

static int dhcp_wait(int s, unsigned int xid, int want, unsigned char *yiaddr, unsigned char *opts,
                     unsigned int *optlen) {
    static unsigned char b[1472];
    unsigned int until = now_ms() + 2000;
    while ((int)(until - now_ms()) > 0) {
        int n = udp_recv(s, b, sizeof b, 0, 0, until - now_ms());
        if (n < 240 || b[0] != 2 || be32(b + 4) != xid || be32(b + 236) != 0x63825363) continue;
        unsigned char type = 0;
        dhcp_option(b + 240, (unsigned int)n - 240, 53, &type, 1);
        if (type == 6) return -1; /* NAK */
        if (type != want) continue;
        copy(yiaddr, b + 16, 4);
        *optlen = (unsigned int)n - 240;
        copy(opts, b + 240, *optlen);
        return 1;
    }
    return 0;
}

int net_dhcp(void) {
    static unsigned char b[600], opts[1300];
    static const unsigned char all[4] = {255, 255, 255, 255};
    if (!have_card()) return 0;
    int s = udp_open(68);
    if (s < 0) return 0;
    int saved = configured;
    configured = 0;
    for (int i = 0; i < 4; i++) { my_ip[i] = 0; my_mask[i] = 0; }
    int ok = 0;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        unsigned int xid = 0x67656e00u ^ clock_ticks_total() ^ ((unsigned int)my_mac[5] << 24);
        unsigned char offer[4], server[4] = {0};
        unsigned int optlen;
        unsigned int n = dhcp_build(b, xid, 1, 0, 0);
        udp_send(s, all, 67, b, n);
        if (dhcp_wait(s, xid, 2, offer, opts, &optlen) != 1) continue;
        dhcp_option(opts, optlen, 54, server, 4);
        n = dhcp_build(b, xid, 3, offer, server);
        udp_send(s, all, 67, b, n);
        unsigned char got[4];
        if (dhcp_wait(s, xid, 5, got, opts, &optlen) != 1) continue;
        copy(my_ip, got, 4);
        static const unsigned char def_mask[4] = {255, 255, 255, 0};
        copy(my_mask, def_mask, 4);
        dhcp_option(opts, optlen, 1, my_mask, 4);
        for (int i = 0; i < 4; i++) my_gw[i] = my_dns[i] = 0;
        dhcp_option(opts, optlen, 3, my_gw, 4);
        dhcp_option(opts, optlen, 6, my_dns, 4);
        ok = 1;
    }
    udp_close(s);
    configured = ok ? 1 : saved;
    return ok;
}

/* Bring the interface up on first use: DHCP. */
static int ensure_up(void) {
    if (configured) return 1;
    if (!have_card()) {
        console_puts("net: no network card\n");
        return 0;
    }
    if (net_dhcp()) return 1;
    console_puts("net: no DHCP answer (set it by hand: net set IP MASK GW DNS)\n");
    return 0;
}

/* ---------- DNS ---------- */

int net_resolve(const char *name, unsigned char *ip) {
    if (parse_ip(name, ip)) return 1;
    if (!ensure_up()) return 0;
    static unsigned char q[512], r[1472];
    unsigned int n = 12;
    unsigned int id = (clock_ticks_total() * 7919u) & 0xffffu;
    for (int i = 0; i < 12; i++) q[i] = 0;
    put16(q, id);
    q[2] = 0x01; /* recursion desired */
    put16(q + 4, 1);
    const char *s = name;
    while (*s && *s != ' ' && *s != '/' && *s != ':') {
        unsigned int lenpos = n++;
        unsigned int l = 0;
        while (*s && *s != '.' && *s != ' ' && *s != '/' && *s != ':') {
            if (n >= 400) return 0;
            q[n++] = (unsigned char)*s++;
            l++;
        }
        if (l == 0 || l > 63) return 0;
        q[lenpos] = (unsigned char)l;
        if (*s == '.') s++;
    }
    q[n++] = 0;
    put16(q + n, 1); n += 2; /* A */
    put16(q + n, 1); n += 2; /* IN */
    int sock = udp_open(0);
    if (sock < 0) return 0;
    int found = 0;
    for (int attempt = 0; attempt < 3 && !found; attempt++) {
        udp_send(sock, my_dns, 53, q, n);
        int got = udp_recv(sock, r, sizeof r, 0, 0, 2000);
        if (got < 12 || be16(r) != id || (r[3] & 0x0f) != 0) {
            if (got >= 12 && be16(r) == id) break; /* NXDOMAIN and friends */
            continue;
        }
        unsigned int qd = be16(r + 4), an = be16(r + 6);
        unsigned int i = 12;
        for (unsigned int k = 0; k < qd; k++) {
            while (i < (unsigned int)got && r[i] && (r[i] & 0xc0) != 0xc0) i += r[i] + 1u;
            i += (i < (unsigned int)got && (r[i] & 0xc0) == 0xc0) ? 2 : 1;
            i += 4;
        }
        for (unsigned int k = 0; k < an && i + 10 <= (unsigned int)got; k++) {
            while (i < (unsigned int)got && r[i] && (r[i] & 0xc0) != 0xc0) i += r[i] + 1u;
            i += (i < (unsigned int)got && (r[i] & 0xc0) == 0xc0) ? 2 : 1;
            if (i + 10 > (unsigned int)got) break;
            unsigned int type = be16(r + i), cls = be16(r + i + 2), rdlen = be16(r + i + 8);
            i += 10;
            if (type == 1 && cls == 1 && rdlen == 4 && i + 4 <= (unsigned int)got) {
                copy(ip, r + i, 4);
                found = 1;
                break;
            }
            i += rdlen;
        }
        break;
    }
    udp_close(sock);
    return found;
}

/* ---------- ping ---------- */

static int net_ping(const char *host) {
    unsigned char dst[4];
    if (!ensure_up()) return 0;
    if (!net_resolve(host, dst)) {
        console_puts("ping: unknown host\n");
        return 0;
    }
    int received = 0;
    for (unsigned int seq = 1; seq <= 4; seq++) {
        unsigned char p[40];
        p[0] = 8;
        p[1] = 0;
        put16(p + 2, 0);
        put16(p + 4, 0x6e65);
        put16(p + 6, seq);
        for (int i = 0; i < 32; i++) p[8 + i] = (unsigned char)('a' + i % 26);
        put16(p + 2, sum_fold(sum_add(0, p, 40)));
        echo_seen = 0;
        unsigned int start = now_ms();
        if (!ip_send(dst, 1, p, 40)) {
            console_puts("ping: no route to host\n");
            return 0;
        }
        int got = 0;
        while ((int)(start + 1000 - now_ms()) > 0) {
            idle();
            if (echo_seen && echo_seq_seen == seq) { got = 1; break; }
        }
        if (got) {
            received++;
            console_puts("ping: reply from ");
            put_ip(dst);
            console_puts(" seq=");
            put_dec(seq);
            console_puts(" time=");
            put_dec(now_ms() - start);
            console_puts("ms\n");
            while ((int)(start + 1000 - now_ms()) > 0) idle();
        } else {
            console_puts("ping: timeout seq=");
            put_dec(seq);
            console_putc('\n');
        }
    }
    console_puts("ping: 4 sent, ");
    put_dec((unsigned int)received);
    console_puts(" received\n");
    return received > 0;
}

/* ---------- shell tools ---------- */

static void print_bytes(const unsigned char *b, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = b[i];
        if (c == '\r') continue;
        if (c < 32 && c != '\n' && c != '\t') c = '.';
        console_putc((char)c);
    }
}

static const char *skip_sp(const char *s) {
    while (*s == ' ') s++;
    return s;
}

static int starts(const char *s, const char *p) {
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

static unsigned int parse_uint(const char **ps) {
    unsigned int v = 0;
    const char *s = *ps;
    while (*s >= '0' && *s <= '9') v = v * 10u + (unsigned int)(*s++ - '0');
    *ps = s;
    return v;
}

static void cmd_http(const char *url) {
    url = skip_sp(url);
    if (starts(url, "get ")) url = skip_sp(url + 4);
    if (starts(url, "http://")) url += 7;
    else if (starts(url, "https://")) {
        console_puts("http: https is not supported (no TLS yet)\n");
        return;
    }
    char host[128];
    unsigned int n = 0;
    while (*url && *url != '/' && *url != ':' && *url != ' ' && n + 1 < sizeof host) host[n++] = *url++;
    host[n] = 0;
    if (n == 0) {
        console_puts("http: usage: http http://host[:port]/path\n");
        return;
    }
    unsigned int port = 80;
    if (*url == ':') {
        url++;
        port = parse_uint(&url);
    }
    const char *path = *url == '/' ? url : "/";
    unsigned int plen = 0;
    while (path[plen] && path[plen] != ' ') plen++;
    if (!ensure_up()) return;
    unsigned char ip[4];
    if (!net_resolve(host, ip)) {
        console_puts("http: unknown host\n");
        return;
    }
    int s = tcp_open(ip, port);
    if (s < 0) {
        console_puts("http: cannot connect\n");
        return;
    }
    static unsigned char req[512];
    unsigned int k = 0;
    const char *parts[] = {"GET ", 0, " HTTP/1.0\r\nHost: ", host, "\r\nUser-Agent: genesis\r\nConnection: close\r\n\r\n"};
    for (int i = 0; i < 5; i++) {
        if (i == 1) {
            for (unsigned int j = 0; j < plen && k < 400; j++) req[k++] = (unsigned char)path[j];
            continue;
        }
        for (const char *c = parts[i]; *c && k < 500; c++) req[k++] = (unsigned char)*c;
    }
    if (tcp_send(s, req, k) < 0) {
        console_puts("http: send failed\n");
        tcp_close(s);
        return;
    }
    static unsigned char buf[1024];
    unsigned int total = 0;
    for (;;) {
        int got = tcp_recv(s, buf, sizeof buf, 5000);
        if (got <= 0) break;
        print_bytes(buf, got);
        total += (unsigned int)got;
    }
    tcp_close(s);
    console_puts("\nhttp: ");
    put_dec(total);
    console_puts(" bytes\n");
}

/* nc HOST PORT [TEXT]: connect, send TEXT and a newline, print the answer */
static void cmd_nc(const char *args) {
    args = skip_sp(args);
    char host[128];
    unsigned int n = 0;
    while (*args && *args != ' ' && n + 1 < sizeof host) host[n++] = *args++;
    host[n] = 0;
    args = skip_sp(args);
    unsigned int port = parse_uint(&args);
    if (n == 0 || port == 0 || port > 65535) {
        console_puts("nc: usage: nc HOST PORT [TEXT]\n");
        return;
    }
    args = skip_sp(args);
    if (!ensure_up()) return;
    unsigned char ip[4];
    if (!net_resolve(host, ip)) {
        console_puts("nc: unknown host\n");
        return;
    }
    int s = tcp_open(ip, port);
    if (s < 0) {
        console_puts("nc: cannot connect\n");
        return;
    }
    console_puts("nc: connected\n");
    if (*args) {
        static unsigned char line[512];
        unsigned int k = 0;
        while (args[k] && k < 500) { line[k] = (unsigned char)args[k]; k++; }
        line[k++] = '\n';
        tcp_send(s, line, k);
    }
    static unsigned char buf[1024];
    for (;;) {
        int got = tcp_recv(s, buf, sizeof buf, 3000);
        if (got <= 0) break;
        print_bytes(buf, got);
    }
    tcp_close(s);
    console_puts("\nnc: closed\n");
}

/* udp HOST PORT TEXT: one datagram out, print one answer */
static void cmd_udp(const char *args) {
    args = skip_sp(args);
    char host[128];
    unsigned int n = 0;
    while (*args && *args != ' ' && n + 1 < sizeof host) host[n++] = *args++;
    host[n] = 0;
    args = skip_sp(args);
    unsigned int port = parse_uint(&args);
    args = skip_sp(args);
    if (n == 0 || port == 0 || port > 65535) {
        console_puts("udp: usage: udp HOST PORT TEXT\n");
        return;
    }
    unsigned char ip[4];
    if (!ensure_up()) return;
    if (!net_resolve(host, ip)) {
        console_puts("udp: unknown host\n");
        return;
    }
    int s = udp_open(0);
    unsigned int k = 0;
    while (args[k]) k++;
    udp_send(s, ip, port, (const unsigned char *)args, k);
    static unsigned char buf[1472];
    int got = udp_recv(s, buf, sizeof buf, 0, 0, 2000);
    if (got >= 0) {
        console_puts("udp: ");
        print_bytes(buf, got);
        console_putc('\n');
    } else {
        console_puts("udp: no answer\n");
    }
    udp_close(s);
}

static void cmd_net_show(void) {
    unsigned char m[6];
    const char *card = nic_info(m);
    if (!card) {
        console_puts("net: no network card\n");
        return;
    }
    console_puts("net: ");
    console_puts(card);
    console_puts(" mac ");
    put_mac(m);
    if (!configured) {
        console_puts(" (no address: dhcp or net set)\n");
        return;
    }
    console_puts(" ip ");
    put_ip(my_ip);
    console_puts(" mask ");
    put_ip(my_mask);
    console_puts(" gw ");
    put_ip(my_gw);
    console_puts(" dns ");
    put_ip(my_dns);
    console_putc('\n');
}

/* Returns 1 when LINE was a network command. */
int net_command(const char *line) {
    if (starts(line, "ifconfig") && (line[8] == 0 || line[8] == ' ')) {
        cmd_net_show();
        return 1;
    }
    if ((starts(line, "net") && line[3] == 0) || starts(line, "net show")) {
        cmd_net_show();
        return 1;
    }
    if (starts(line, "net set ")) {
        const char *s = skip_sp(line + 8);
        unsigned char a[4][4];
        for (int i = 0; i < 4; i++) {
            int used = parse_ip(s, a[i]);
            if (!used) {
                console_puts("net: usage: net set IP MASK GATEWAY DNS\n");
                return 1;
            }
            s = skip_sp(s + used);
        }
        copy(my_ip, a[0], 4);
        copy(my_mask, a[1], 4);
        copy(my_gw, a[2], 4);
        copy(my_dns, a[3], 4);
        configured = have_card();
        cmd_net_show();
        return 1;
    }
    if (starts(line, "dhcp") && (line[4] == 0 || line[4] == ' ')) {
        if (!have_card()) {
            console_puts("dhcp: no network card\n");
        } else if (net_dhcp()) {
            console_puts("dhcp: ");
            put_ip(my_ip);
            console_puts(" gw ");
            put_ip(my_gw);
            console_puts(" dns ");
            put_ip(my_dns);
            console_putc('\n');
        } else {
            console_puts("dhcp: no answer\n");
        }
        return 1;
    }
    if (starts(line, "ping ")) {
        net_ping(skip_sp(line + 5));
        return 1;
    }
    if (starts(line, "dns ")) {
        unsigned char ip[4];
        const char *name = skip_sp(line + 4);
        if (net_resolve(name, ip)) {
            console_puts("dns: ");
            console_puts(name);
            console_puts(" -> ");
            put_ip(ip);
            console_putc('\n');
        } else if (configured) {
            console_puts("dns: not found\n");
        }
        return 1;
    }
    if (starts(line, "http ") || (starts(line, "http") && line[4] == 0)) {
        cmd_http(line + 4);
        return 1;
    }
    if (starts(line, "nc ") || (starts(line, "nc") && line[2] == 0)) {
        cmd_nc(line + 2);
        return 1;
    }
    if (starts(line, "udp ") || (starts(line, "udp") && line[3] == 0)) {
        cmd_udp(line + 3);
        return 1;
    }
    return 0;
}

/* Socket calls for programs (system calls and cc builtins). A host can be a
 * name or a dotted address. */
int net_tcp_connect(const char *host, unsigned int port) {
    unsigned char ip[4];
    if (!ensure_up() || !net_resolve(host, ip)) return -1;
    return tcp_open(ip, port);
}
