/* Genesis Link: a private frame inside an Ethernet carrier. */

enum { GL_PING = 1, GL_PONG = 2, GL_TEXT = 3, GL_MAX = 128, ETH_TYPE_HI = 0x88, ETH_TYPE_LO = 0xb5 };

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
    return rtl_send(frame, total);
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
    if (length < 22 || frame[12] != ETH_TYPE_HI || frame[13] != ETH_TYPE_LO) {
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

void link_poll(void) {
    if (!link_ready) {
        return;
    }
    rtl_receive();
    if (reply_pong) {
        reply_pong = 0;
        link_send(GL_PONG, 0, 0);
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

void link_init(void) {
    int slot = -1;
    for (unsigned index = 0; index < 32; index++) {
        unsigned int id = pci_read(index, 0);
        if (id == 0xffffffffu || id == 0) {
            continue;
        }
        if ((id & 0xffffu) == 0x10ecu && (id >> 16) == 0x8139u) {
            slot = (int)index;
            break;
        }
    }
    if (slot < 0) {
        return;
    }
    unsigned int bar = pci_read((unsigned)slot, 0x10);
    if ((bar & 1u) == 0) {
        return;
    }
    unsigned int command = pci_read((unsigned)slot, 0x04);
    pci_write((unsigned)slot, 0x04, command | 0x05u);
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
        return;
    }
    for (int i = 0; i < 6; i++) {
        mac[i] = inb(io_base + (unsigned short)i);
    }
    rx_offset = 0;
    outl(io_base + 0x30, (unsigned int)(unsigned long)rx_buffer);
    outw(io_base + 0x3c, 0x0005);
    outl(io_base + 0x44, 0x8fu);
    outb(io_base + 0x37, 0x0c);
    link_ready = 1;
    console_puts("link: up\n");
}
