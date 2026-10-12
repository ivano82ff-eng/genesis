/* Genesis Link: a private frame inside an Ethernet carrier. */

enum { GL_PING = 1, GL_PONG = 2, GL_TEXT = 3, GL_FILE = 4, GL_CHUNK = 5, GL_DONE = 6, GL_ACK = 7, GL_MAX = 128, ETH_TYPE_HI = 0x88, ETH_TYPE_LO = 0xb5 };

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(unsigned short port, unsigned short value) {
    __asm__ volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

static inline void outl(unsigned short port, unsigned int value) {
    __asm__ volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline unsigned int inl(unsigned short port) {
    unsigned int value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

extern void console_puts(const char *text);
extern void console_putc(char ch);

static unsigned char rx_buffer[8192 + 16 + 1600] __attribute__((aligned(16)));
static unsigned char tx_buffers[4][1600] __attribute__((aligned(16)));
static int tx_next;
static unsigned short io_base;
static unsigned int rx_offset;
static unsigned char mac[6];
static int link_ready;
static int pong_seen;
static int reply_pong;
/* The other end of the link, learnt from any good frame it sends. */
static unsigned char peer_mac[6];
static int peer_known;
/* File transfer (GL_FILE header, GL_CHUNK pieces, GL_DONE; each one is
 * acknowledged with GL_ACK + its offset so a slow receiver never drops). */
enum { XFER_MAX = 65536, CHUNK = 112 };
static unsigned char rx_file[XFER_MAX];
static char rx_name[64];
static unsigned int rx_total, rx_sum, rx_got;
static int rx_active;
static int ack_pending;
static unsigned int ack_offset;
static int ack_seen;
static unsigned int ack_seen_offset;
void link_file_received(const char *name, const unsigned char *data, unsigned int length);

static unsigned int le32(const unsigned char *p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8) | ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

static void put32(unsigned char *p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

/* Fletcher-style checksum over the whole file. */
static unsigned int file_sum(const unsigned char *data, unsigned int length) {
    unsigned int a = 1;
    unsigned int b = 0;
    for (unsigned int i = 0; i < length; i++) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static unsigned int pci_read(unsigned slot, unsigned offset) {
    unsigned int addr = 0x80000000u | (slot << 11) | (offset & 0xfcu);
    outl(0xcf8, addr);
    return inl(0xcfc);
}

static void pci_write(unsigned slot, unsigned offset, unsigned int value) {
    unsigned int addr = 0x80000000u | (slot << 11) | (offset & 0xfcu);
    outl(0xcf8, addr);
    outl(0xcfc, value);
}

static unsigned short gl_sum(const unsigned char *header, const unsigned char *payload, unsigned int length) {
    unsigned int sum = 0;
    for (int i = 0; i < 6; i++) {
        sum += header[i];
    }
    for (unsigned int i = 0; i < length; i++) {
        sum += payload[i];
    }
    return (unsigned short)sum;
}

static void cache_flush(void);

static int nic_kind; /* 1 = RTL8139, 2 = NE2000 (PCI) */
static int ne_send(const unsigned char *frame, unsigned int length);
void net_input(const unsigned char *frame, unsigned int length);

static int rtl_send(const unsigned char *frame, unsigned int length) {
    if (length < 60) {
        length = 60;
    }
    if (length > 1500) {
        return 0;
    }
    int desc = tx_next;
    tx_next = (tx_next + 1) & 3;
    unsigned short status_port = (unsigned short)(io_base + 0x10 + desc * 4);
    unsigned short addr_port = (unsigned short)(io_base + 0x20 + desc * 4);
    for (int i = 0; i < 1000000; i++) {
        if ((inl(status_port) & 0x2000u) == 0) {
            break;
        }
    }
    for (unsigned int i = 0; i < length; i++) {
        tx_buffers[desc][i] = frame[i];
    }
    cache_flush();
    outl(addr_port, (unsigned int)(unsigned long)tx_buffers[desc]);
    outl(status_port, length);
    for (int i = 0; i < 1000000; i++) {
        if ((inl(status_port) & 0x8000u) != 0) {
            return 1;
        }
    }
    return 0;
}

/* Send one Ethernet frame on whichever card was found. */
int nic_send(const unsigned char *frame, unsigned int length) {
    if (!link_ready) {
        return 0;
    }
    return nic_kind == 2 ? ne_send(frame, length) : rtl_send(frame, length);
}

static int link_send(unsigned char type, const unsigned char *payload, unsigned int length) {
    unsigned char frame[1600];
    if (!link_ready || length > GL_MAX) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        frame[i] = 0xff;
        frame[6 + i] = mac[i];
    }
    frame[12] = ETH_TYPE_HI;
    frame[13] = ETH_TYPE_LO;
    frame[14] = 'G';
    frame[15] = 'L';
    frame[16] = 1;
    frame[17] = type;
    frame[18] = (unsigned char)length;
    frame[19] = (unsigned char)(length >> 8);
    unsigned short sum = gl_sum(frame + 14, payload, length);
    frame[20] = (unsigned char)sum;
    frame[21] = (unsigned char)(sum >> 8);
    for (unsigned int i = 0; i < length; i++) {
        frame[22 + i] = payload[i];
    }
    unsigned int total = 22 + length;
    while (total < 60) {
        frame[total++] = 0;
    }
    return nic_send(frame, total);
}

static int same_mac(const unsigned char *addr) {
    for (int i = 0; i < 6; i++) {
        if (addr[i] != mac[i]) {
            return 0;
        }
    }
    return 1;
}

static void link_take(const unsigned char *frame, unsigned int length) {
    if (length >= 14 && (frame[12] != ETH_TYPE_HI || frame[13] != ETH_TYPE_LO)) {
        net_input(frame, length); /* ARP, IPv4: kernel/net.c */
        return;
    }
    if (length < 22) {
        return;
    }
    if (same_mac(frame + 6)) {
        return;
    }
    if (frame[14] != 'G' || frame[15] != 'L' || frame[16] != 1) {
        return;
    }
    unsigned int payload_len = (unsigned int)frame[18] | ((unsigned int)frame[19] << 8);
    if (payload_len > GL_MAX || 22 + payload_len > length) {
        return;
    }
    unsigned short expect = gl_sum(frame + 14, frame + 22, payload_len);
    unsigned short found = (unsigned short)((unsigned int)frame[20] | ((unsigned int)frame[21] << 8));
    if (expect != found) {
        return;
    }
    unsigned char type = frame[17];
    if (!peer_known) {
        static const char hex[] = "0123456789abcdef";
        char text[24];
        unsigned int n = 0;
        for (int i = 0; i < 6; i++) {
            peer_mac[i] = frame[6 + i];
            text[n++] = hex[frame[6 + i] >> 4];
            text[n++] = hex[frame[6 + i] & 15];
            if (i < 5) {
                text[n++] = ':';
            }
        }
        text[n] = 0;
        peer_known = 1;
        console_puts("link: peer ");
        console_puts(text);
        console_putc('\n');
    }
    const unsigned char *pl = frame + 22;
    if (type == GL_ACK && payload_len >= 4) {
        ack_seen = 1;
        ack_seen_offset = le32(pl);
        return;
    }
    if (type == GL_FILE && payload_len >= 9) {
        rx_total = le32(pl);
        rx_sum = le32(pl + 4);
        unsigned int n = 0;
        for (unsigned int i = 8; i < payload_len && n + 1 < sizeof rx_name; i++) {
            char ch = (char)pl[i];
            if (ch == '/' || ch < 32) {
                ch = '_';
            }
            rx_name[n++] = ch;
        }
        rx_name[n] = 0;
        rx_got = 0;
        rx_active = rx_total <= XFER_MAX && n > 0;
        ack_pending = 1;
        ack_offset = 0xffffffffu;
        return;
    }
    if (type == GL_CHUNK && payload_len >= 4) {
        unsigned int off = le32(pl);
        unsigned int n = payload_len - 4;
        if (rx_active && off == rx_got && off + n <= rx_total) {
            for (unsigned int i = 0; i < n; i++) {
                rx_file[off + i] = pl[4 + i];
            }
            rx_got += n;
        }
        ack_pending = 1;
        ack_offset = off; /* re-sent chunks are acknowledged again */
        return;
    }
    if (type == GL_DONE) {
        if (rx_active) {
            rx_active = 0;
            if (rx_got == rx_total && file_sum(rx_file, rx_total) == rx_sum) {
                link_file_received(rx_name, rx_file, rx_total);
            } else {
                console_puts("link: file damaged, dropped\n");
            }
        }
        ack_pending = 1;
        ack_offset = 0xfffffffeu;
        return;
    }
    if (type == GL_PING) {
        reply_pong = 1;
        return;
    }
    if (type == GL_PONG) {
        pong_seen = 1;
        return;
    }
    if (type != GL_TEXT) {
        return;
    }
    console_puts("link: ");
    for (unsigned int i = 0; i < payload_len; i++) {
        char ch = (char)frame[22 + i];
        if (ch >= 32 && ch <= 126) {
            console_putc(ch);
        }
    }
    console_putc('\n');
}

static void cache_flush(void) {
    __asm__ volatile("wbinvd" ::: "memory");
}

static void rtl_receive(void) {
    if ((inb(io_base + 0x3e) & 0x01) != 0) {
        outb(io_base + 0x3e, 0x01);
    }
    for (int n = 0; n < 8; n++) {
        if ((inb(io_base + 0x37) & 0x01) != 0) {
            return;
        }
        cache_flush();
        unsigned char *header = rx_buffer + rx_offset;
        unsigned short status = (unsigned short)(header[0] | (header[1] << 8));
        unsigned short packet_length = (unsigned short)(header[2] | (header[3] << 8));
        if ((status & 0x01) == 0 || packet_length < 4 || packet_length > 1600) {
            return;
        }
        link_take(header + 4, (unsigned int)packet_length - 4u);
        rx_offset = (rx_offset + packet_length + 4u + 3u) & ~3u;
        if (rx_offset >= 8192u) {
            rx_offset -= 8192u;
        }
        outw(io_base + 0x38, (unsigned short)(rx_offset - 16u));
    }
}

static void ne_receive(void);

static void link_poll_locked(void) {
    if (!link_ready) {
        return;
    }
    if (nic_kind == 2) {
        ne_receive();
    } else {
        rtl_receive();
    }
    if (reply_pong) {
        reply_pong = 0;
        link_send(GL_PONG, 0, 0);
    }
    if (ack_pending) {
        unsigned char p[4];
        ack_pending = 0;
        put32(p, ack_offset);
        link_send(GL_ACK, p, 4);
    }
}

/* The card is polled from the shell loop, guest_yield() and network waits in
 * any task. One poll at a time, with interrupts off so a timer switch cannot
 * start a second one half-way through a frame. */
static volatile int link_busy;

int link_polling(void) {
    return link_busy;
}

void link_poll(void) {
    unsigned long flags;
#ifdef __x86_64__
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags) :: "memory");
#else
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags) :: "memory");
#endif
    if (!link_busy) {
        link_busy = 1;
        link_poll_locked();
        link_busy = 0;
    }
    if (flags & 0x200ul) {
        __asm__ volatile("sti" ::: "memory");
    }
}

int link_up(void) {
    return link_ready;
}

int link_send_text(const char *text) {
    unsigned char payload[GL_MAX];
    unsigned int length = 0;
    while (text[length] != 0 && length < GL_MAX) {
        payload[length] = (unsigned char)text[length];
        length++;
    }
    return link_send(GL_TEXT, payload, length);
}

int link_ping(void) {
    pong_seen = 0;
    for (int attempt = 0; attempt < 6; attempt++) {
        if (!link_send(GL_PING, 0, 0)) {
            return 0;
        }
        for (int i = 0; i < 500000; i++) {
            link_poll();
            if (pong_seen) {
                return 1;
            }
        }
    }
    return 0;
}

static int rtl_init(unsigned slot) {
    unsigned int bar = pci_read(slot, 0x10);
    if ((bar & 1u) == 0) {
        return 0;
    }
    unsigned int command = pci_read(slot, 0x04);
    pci_write(slot, 0x04, command | 0x05u);
    io_base = (unsigned short)(bar & ~3u);
    outb(io_base + 0x52, 0x00);
    outb(io_base + 0x37, 0x10);
    int reset = 0;
    for (int i = 0; i < 1000000; i++) {
        if ((inb(io_base + 0x37) & 0x10) == 0) {
            reset = 1;
            break;
        }
    }
    if (!reset) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + (unsigned short)i);
    }
    rx_offset = 0;
    outl(io_base + 0x30, (unsigned int)(unsigned long)rx_buffer);
    outw(io_base + 0x3c, 0x0005);
    outl(io_base + 0x44, 0x8fu);
    outb(io_base + 0x37, 0x0c);
    return 1;
}

/* ---- NE2000 (PCI 10ec:8029): QEMU ne2k_pci and the v86 browser card ---- */
enum { NE_CR = 0x00, NE_PSTART = 0x01, NE_PSTOP = 0x02, NE_BNRY = 0x03, NE_TPSR = 0x04, NE_TBCR0 = 0x05,
       NE_TBCR1 = 0x06, NE_ISR = 0x07, NE_RSAR0 = 0x08, NE_RSAR1 = 0x09, NE_RBCR0 = 0x0a, NE_RBCR1 = 0x0b,
       NE_RCR = 0x0c, NE_TCR = 0x0d, NE_DCR = 0x0e, NE_IMR = 0x0f, NE_DATA = 0x10, NE_RESET = 0x1f,
       NE_TX_PAGE = 0x40, NE_RX_START = 0x46, NE_RX_STOP = 0x80 };

static inline unsigned short inw_(unsigned short port) {
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void ne_dma_setup(unsigned int addr, unsigned int count, unsigned char cmd) {
    outb(io_base + NE_RBCR0, (unsigned char)count);
    outb(io_base + NE_RBCR1, (unsigned char)(count >> 8));
    outb(io_base + NE_RSAR0, (unsigned char)addr);
    outb(io_base + NE_RSAR1, (unsigned char)(addr >> 8));
    outb(io_base + NE_CR, cmd);
}

/* Read COUNT bytes of card memory at ADDR (16-bit transfers). */
static void ne_read(unsigned int addr, unsigned char *out, unsigned int count) {
    unsigned int even = (count + 1u) & ~1u;
    ne_dma_setup(addr, even, 0x0a);
    for (unsigned int i = 0; i < even; i += 2) {
        unsigned short w = inw_(io_base + NE_DATA);
        out[i] = (unsigned char)w;
        if (i + 1 < count) {
            out[i + 1] = (unsigned char)(w >> 8);
        }
    }
    for (int i = 0; i < 100000 && (inb(io_base + NE_ISR) & 0x40) == 0; i++) {
    }
    outb(io_base + NE_ISR, 0x40);
}

static int ne_send(const unsigned char *frame, unsigned int length) {
    if (length < 60) {
        length = 60;
    }
    if (length > 1514) {
        return 0;
    }
    for (int i = 0; i < 1000000 && (inb(io_base + NE_CR) & 0x04) != 0; i++) {
    }
    unsigned int even = (length + 1u) & ~1u;
    outb(io_base + NE_ISR, 0x40);
    ne_dma_setup((unsigned int)NE_TX_PAGE << 8, even, 0x12);
    for (unsigned int i = 0; i < even; i += 2) {
        unsigned char lo = frame[i];
        unsigned char hi = i + 1 < length ? frame[i + 1] : 0;
        outw(io_base + NE_DATA, (unsigned short)(lo | (hi << 8)));
    }
    for (int i = 0; i < 100000 && (inb(io_base + NE_ISR) & 0x40) == 0; i++) {
    }
    outb(io_base + NE_ISR, 0x40);
    outb(io_base + NE_TPSR, NE_TX_PAGE);
    outb(io_base + NE_TBCR0, (unsigned char)length);
    outb(io_base + NE_TBCR1, (unsigned char)(length >> 8));
    outb(io_base + NE_CR, 0x26);
    for (int i = 0; i < 1000000; i++) {
        if ((inb(io_base + NE_CR) & 0x04) == 0) {
            outb(io_base + NE_ISR, 0x0a);
            return 1;
        }
    }
    return 0;
}

static void ne_receive(void) {
    static unsigned char frame[1600];
    for (int n = 0; n < 16; n++) {
        outb(io_base + NE_CR, 0x62);
        unsigned char curr = inb(io_base + 0x07);
        outb(io_base + NE_CR, 0x22);
        unsigned char bnry = inb(io_base + NE_BNRY);
        unsigned char page = (unsigned char)(bnry + 1);
        if (page >= NE_RX_STOP) {
            page = NE_RX_START;
        }
        if (page == curr) {
            outb(io_base + NE_ISR, 0x15); /* RX, RX error, overwrite */
            return;
        }
        unsigned char hdr[4];
        ne_read((unsigned int)page << 8, hdr, 4);
        unsigned char next = hdr[1];
        unsigned int len = (unsigned int)hdr[2] | ((unsigned int)hdr[3] << 8);
        if (next < NE_RX_START || next >= NE_RX_STOP || len < 4 + 14 || len > 1600 + 4) {
            /* lost sync: drop everything that is queued */
            outb(io_base + NE_BNRY, curr == NE_RX_START ? NE_RX_STOP - 1 : (unsigned char)(curr - 1));
            return;
        }
        unsigned int body = len - 4;
        unsigned int start = ((unsigned int)page << 8) + 4;
        unsigned int ring_end = (unsigned int)NE_RX_STOP << 8;
        if (start + body > ring_end) {
            unsigned int first = ring_end - start;
            ne_read(start, frame, first);
            ne_read((unsigned int)NE_RX_START << 8, frame + first, body - first);
        } else {
            ne_read(start, frame, body);
        }
        outb(io_base + NE_BNRY, next == NE_RX_START ? NE_RX_STOP - 1 : (unsigned char)(next - 1));
        if ((hdr[0] & 0x01) != 0) {
            link_take(frame, body);
        }
    }
}

static int ne_init(unsigned slot) {
    unsigned int bar = pci_read(slot, 0x10);
    if ((bar & 1u) == 0) {
        return 0;
    }
    unsigned int command = pci_read(slot, 0x04);
    pci_write(slot, 0x04, command | 0x05u);
    io_base = (unsigned short)(bar & ~3u);
    outb(io_base + NE_RESET, inb(io_base + NE_RESET));
    for (int i = 0; i < 100000 && (inb(io_base + NE_ISR) & 0x80) == 0; i++) {
    }
    outb(io_base + NE_ISR, 0xff);
    outb(io_base + NE_CR, 0x21);
    outb(io_base + NE_DCR, 0x49);
    outb(io_base + NE_RBCR0, 0);
    outb(io_base + NE_RBCR1, 0);
    outb(io_base + NE_IMR, 0);
    outb(io_base + NE_ISR, 0xff);
    outb(io_base + NE_RCR, 0x20);
    outb(io_base + NE_TCR, 0x02);
    unsigned char prom[32];
    ne_read(0, prom, 32);
    for (int i = 0; i < 6; i++) {
        mac[i] = prom[i * 2];
    }
    outb(io_base + NE_PSTART, NE_RX_START);
    outb(io_base + NE_PSTOP, NE_RX_STOP);
    outb(io_base + NE_BNRY, NE_RX_START);
    outb(io_base + NE_TPSR, NE_TX_PAGE);
    outb(io_base + NE_CR, 0x61);
    for (int i = 0; i < 6; i++) {
        outb(io_base + 0x01 + i, mac[i]);
    }
    for (int i = 0; i < 8; i++) {
        outb(io_base + 0x08 + i, 0xff);
    }
    outb(io_base + 0x07, NE_RX_START + 1);
    outb(io_base + NE_CR, 0x22);
    outb(io_base + NE_ISR, 0xff);
    outb(io_base + NE_TCR, 0x00);
    outb(io_base + NE_RCR, 0x04); /* our MAC and broadcast */
    return 1;
}

void link_init(void) {
    for (unsigned index = 0; index < 32 && nic_kind == 0; index++) {
        unsigned int id = pci_read(index, 0);
        if (id == 0xffffffffu || id == 0 || (id & 0xffffu) != 0x10ecu) {
            continue;
        }
        if ((id >> 16) == 0x8139u && rtl_init(index)) {
            nic_kind = 1;
        } else if ((id >> 16) == 0x8029u && ne_init(index)) {
            nic_kind = 2;
        }
    }
    if (nic_kind == 0) {
        return;
    }
    link_ready = 1;
    console_puts("link: up\n");
}

/* The card's name and MAC for `net`. */
const char *nic_info(unsigned char *out_mac) {
    for (int i = 0; i < 6; i++) {
        out_mac[i] = mac[i];
    }
    return nic_kind == 1 ? "rtl8139" : nic_kind == 2 ? "ne2k" : 0;
}

/* Send one frame and wait for its acknowledgement (offset `want`). */
static int send_acked(unsigned char type, const unsigned char *payload, unsigned int length, unsigned int want) {
    for (int attempt = 0; attempt < 8; attempt++) {
        ack_seen = 0;
        if (!link_send(type, payload, length)) {
            return 0;
        }
        for (int i = 0; i < 400000; i++) {
            link_poll();
            if (ack_seen && ack_seen_offset == want) {
                return 1;
            }
        }
    }
    return 0;
}

/* Genesis Link file transfer: header, chunks, done; every frame acked. */
int link_send_file(const char *name, const unsigned char *data, unsigned int length) {
    if (!link_ready || length > XFER_MAX) {
        return 0;
    }
    unsigned char p[GL_MAX];
    put32(p, length);
    put32(p + 4, file_sum(data, length));
    unsigned int n = 8;
    for (unsigned int i = 0; name[i] != 0 && n < GL_MAX; i++) {
        p[n++] = (unsigned char)name[i];
    }
    if (!send_acked(GL_FILE, p, n, 0xffffffffu)) {
        return 0;
    }
    for (unsigned int off = 0; off < length; off += CHUNK) {
        unsigned int k = length - off < CHUNK ? length - off : CHUNK;
        put32(p, off);
        for (unsigned int i = 0; i < k; i++) {
            p[4 + i] = data[off + i];
        }
        if (!send_acked(GL_CHUNK, p, 4 + k, off)) {
            return 0;
        }
    }
    return send_acked(GL_DONE, 0, 0, 0xfffffffeu);
}

/* The peer's MAC as text, or 0 if no peer has been seen. */
int link_peer(char *out) {
    if (!peer_known) {
        return 0;
    }
    static const char hex[] = "0123456789abcdef";
    unsigned int n = 0;
    for (int i = 0; i < 6; i++) {
        out[n++] = hex[peer_mac[i] >> 4];
        out[n++] = hex[peer_mac[i] & 15];
        if (i < 5) {
            out[n++] = ':';
        }
    }
    out[n] = 0;
    return 1;
}

/* Say hello without waiting: a peer that answers shows up on the desktop. */
void link_hello(void) {
    if (link_ready) {
        link_send(GL_PING, 0, 0);
    }
}
