#include "fs_limits.h"

enum { ATA_DATA = 0x1f0, ATA_SECCNT = 0x1f2, ATA_LBA0 = 0x1f3, ATA_LBA1 = 0x1f4, ATA_LBA2 = 0x1f5, ATA_DRIVE = 0x1f6, ATA_STATUS = 0x1f7 };
enum { FS_HEADER_LBA = 32, FS_DATA_LBA = 2048, FS_FLAG_HOME = 1 };

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

static inline unsigned short inw(unsigned short port) {
    unsigned short value;
    __asm__ volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

extern void fs_clear_all(void);
extern int fs_export(int index, char *name, unsigned int name_cap, unsigned char *data, unsigned int *length);
extern void fs_import(const char *name, const unsigned char *data, unsigned int length);

static void ata_delay(void) {
    inb(0x3f6);
    inb(0x3f6);
    inb(0x3f6);
    inb(0x3f6);
}

static int ata_wait_ready(void) {
    for (int i = 0; i < 1000000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if (status == 0xff) {
            continue;
        }
        if ((status & 0x80) == 0 && (status & 0x40) != 0 && (status & 0x01) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ata_ready(void) {
    ata_delay();
    if (ata_wait_ready()) {
        return 1;
    }
    outb(0x3f6, 0x04);
    ata_delay();
    outb(0x3f6, 0x00);
    ata_delay();
    outb(ATA_DRIVE, 0xe0);
    ata_delay();
    return ata_wait_ready();
}

static int ata_data(void) {
    for (int i = 0; i < 200000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if ((status & 0x80) == 0 && (status & 0x08) != 0) {
            return 1;
        }
        if ((status & 0x80) == 0 && (status & 0x01) != 0) {
            return 0;
        }
    }
    return 0;
}

static void ata_select(unsigned int lba) {
    outb(ATA_DRIVE, (unsigned char)(0xe0 | ((lba >> 24) & 0x0f)));
    ata_delay();
    outb(ATA_SECCNT, 1);
    outb(ATA_LBA0, (unsigned char)lba);
    outb(ATA_LBA1, (unsigned char)(lba >> 8));
    outb(ATA_LBA2, (unsigned char)(lba >> 16));
}

static int ata_read(unsigned int lba, unsigned char *buf) {
    ata_select(lba);
    if (!ata_ready()) {
        return 0;
    }
    ata_select(lba);
    outb(ATA_STATUS, 0x20);
    if (!ata_data()) {
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        unsigned short word = inw(ATA_DATA);
        buf[i * 2] = (unsigned char)word;
        buf[i * 2 + 1] = (unsigned char)(word >> 8);
    }
    return 1;
}

static int ata_write(unsigned int lba, const unsigned char *buf) {
    ata_select(lba);
    if (!ata_ready()) {
        return 0;
    }
    ata_select(lba);
    outb(ATA_STATUS, 0x30);
    if (!ata_data()) {
        return 0;
    }
    for (int i = 0; i < 256; i++) {
        unsigned short word = (unsigned short)(buf[i * 2] | (buf[i * 2 + 1] << 8));
        outw(ATA_DATA, word);
    }
    for (int i = 0; i < 200000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if ((status & 0x80) == 0) {
            return (status & 0x01) == 0;
        }
    }
    return 0;
}

static unsigned int read_u32(const unsigned char *buf) {
    return (unsigned int)buf[0] | ((unsigned int)buf[1] << 8) | ((unsigned int)buf[2] << 16) |
           ((unsigned int)buf[3] << 24);
}

static void write_u32(unsigned char *buf, unsigned int value) {
    buf[0] = (unsigned char)value;
    buf[1] = (unsigned char)(value >> 8);
    buf[2] = (unsigned char)(value >> 16);
    buf[3] = (unsigned char)(value >> 24);
}

static unsigned char boot_flags;

static int header_ok(const unsigned char *sector, char ver) {
    return sector[0] == 'G' && sector[1] == 'N' && sector[2] == 'S' && sector[3] == (unsigned char)ver;
}

static int ata_flush(void) {
    ata_select(0);
    if (!ata_ready()) {
        return 0;
    }
    outb(ATA_STATUS, 0xe7);
    for (int i = 0; i < 200000; i++) {
        unsigned char status = inb(ATA_STATUS);
        if ((status & 0x80) == 0) {
            return (status & 0x01) == 0;
        }
    }
    return 0;
}

static int read_file_bytes(unsigned int lba, unsigned int length, unsigned char *dest) {
    unsigned char sector[512];
    unsigned int got = 0;
    while (got < length) {
        if (!ata_read(lba, sector)) {
            return 0;
        }
        unsigned int chunk = length - got;
        if (chunk > 512u) {
            chunk = 512u;
        }
        for (unsigned int b = 0; b < chunk; b++) {
            dest[got + b] = sector[b];
        }
        got += chunk;
        lba++;
    }
    return 1;
}

static int write_file_bytes(unsigned int lba, const unsigned char *src, unsigned int length) {
    unsigned char sector[512];
    unsigned int put = 0;
    while (put < length || (put == 0 && length == 0)) {
        for (int b = 0; b < 512; b++) {
            sector[b] = 0;
        }
        unsigned int chunk = length - put;
        if (chunk > 512u) {
            chunk = 512u;
        }
        for (unsigned int b = 0; b < chunk; b++) {
            sector[b] = src[put + b];
        }
        if (!ata_write(lba, sector)) {
            return 0;
        }
        put += chunk;
        lba++;
        if (length == 0) {
            break;
        }
    }
    return 1;
}

/* Keep the kernel image small for the MBR loader; use high RAM for I/O. */
static unsigned char *const file_io = (unsigned char *)0xA00000ul;

static int disk_load_gns2(const unsigned char *header) {
    boot_flags = header[8];
    unsigned int count = read_u32(header + 4);
    if (count > FS_MAX) {
        count = FS_MAX;
    }
    fs_clear_all();
    for (unsigned int i = 0; i < count; i++) {
        const unsigned char *entry = header + 16 + i * 24;
        char name[FS_NAME];
        unsigned int n = 0;
        while (n + 1 < FS_NAME && entry[n] != 0) {
            name[n] = (char)entry[n];
            n++;
        }
        name[n] = 0;
        unsigned int lba = read_u32(entry + 16);
        unsigned int length = read_u32(entry + 20);
        if (name[0] == 0 || length > FS_DATA) {
            continue;
        }
        if (length > 0 && !read_file_bytes(lba, length, file_io)) {
            return 0;
        }
        fs_import(name, file_io, length);
    }
    return 1;
}

static int disk_load_gns1(const unsigned char *header, int from_header) {
    boot_flags = from_header ? header[8] : 0;
    unsigned int count = read_u32(header + 4);
    if (count > FS_MAX) {
        count = FS_MAX;
    }
    fs_clear_all();
    unsigned char sector[512];
    for (unsigned int i = 0; i < count; i++) {
        if (!ata_read(1 + i, sector)) {
            return 0;
        }
        char name[FS_NAME];
        unsigned int n = 0;
        while (n + 1 < FS_NAME && sector[n] != 0) {
            name[n] = (char)sector[n];
            n++;
        }
        name[n] = 0;
        unsigned int length = read_u32(sector + 16);
        if (length > 128u) {
            length = 128u;
        }
        if (name[0] == 0) {
            continue;
        }
        fs_import(name, sector + 20, length);
    }
    return 1;
}

static int disk_load_once(void) {
    unsigned char sector[512];
    if (ata_read(FS_HEADER_LBA, sector) && header_ok(sector, '2')) {
        return disk_load_gns2(sector);
    }
    if (ata_read(FS_HEADER_LBA, sector) && header_ok(sector, '1')) {
        return disk_load_gns1(sector, 1);
    }
    if (ata_read(0, sector) && header_ok(sector, '1')) {
        return disk_load_gns1(sector, 0);
    }
    return 0;
}

int disk_load(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (disk_load_once()) {
            return 1;
        }
    }
    return 0;
}

static int disk_save_once(void) {
    unsigned char header[512];
    for (int b = 0; b < 512; b++) {
        header[b] = 0;
    }
    header[0] = 'G';
    header[1] = 'N';
    header[2] = 'S';
    header[3] = '2';
    header[8] = boot_flags;
    unsigned int count = 0;
    unsigned int lba = FS_DATA_LBA;
    for (int i = 0; i < FS_MAX; i++) {
        char name[FS_NAME];
        unsigned int length = 0;
        if (!fs_export(i, name, FS_NAME, file_io, &length)) {
            continue;
        }
        unsigned char *entry = header + 16 + count * 24;
        unsigned int n = 0;
        while (name[n] != 0 && n + 1 < FS_NAME) {
            entry[n] = (unsigned char)name[n];
            n++;
        }
        write_u32(entry + 16, lba);
        write_u32(entry + 20, length);
        if (!write_file_bytes(lba, file_io, length)) {
            return 0;
        }
        unsigned int sectors = length == 0 ? 1u : (length + 511u) / 512u;
        lba += sectors;
        count++;
    }
    write_u32(header + 4, count);
    if (!ata_write(FS_HEADER_LBA, header)) {
        return 0;
    }
    ata_flush();
    return 1;
}

int disk_save(void);

void disk_request_home(void) {
    boot_flags = (unsigned char)(boot_flags | FS_FLAG_HOME);
    disk_save();
}

int disk_consume_home(void) {
    if ((boot_flags & FS_FLAG_HOME) == 0) {
        return 0;
    }
    boot_flags = (unsigned char)(boot_flags & (unsigned char)~FS_FLAG_HOME);
    return disk_save();
}

int disk_save(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (disk_save_once()) {
            return 1;
        }
    }
    return 0;
}

extern const unsigned char mbr_bin[];
extern const unsigned char mbr_bin_end[];
extern char kernel_load_start[];
extern char kernel_image_end[];
extern char _start[];
extern void console_puts(const char *text);

int install_system(void) {
    /* Text and data only — BSS is cleared by the boot path. */
    unsigned int bytes = (unsigned int)(kernel_image_end - kernel_load_start);
    if (bytes == 0 || bytes > 2u * 1024u * 1024u) {
        console_puts("install: failed\n");
        return 0;
    }
    unsigned int sectors = (bytes + 511u) / 512u;
    const unsigned int lba = 64;
    unsigned char sector[512];
    const unsigned char *src = (const unsigned char *)kernel_load_start;
    for (unsigned int i = 0; i < sectors; i++) {
        for (int b = 0; b < 512; b++) {
            unsigned int offset = i * 512u + (unsigned int)b;
            sector[b] = offset < bytes ? src[offset] : 0;
        }
        if (!ata_write(lba + i, sector)) {
            console_puts("install: failed\n");
            return 0;
        }
    }
    unsigned char mbr[512];
    unsigned int mbr_len = (unsigned int)(mbr_bin_end - mbr_bin);
    for (int b = 0; b < 512; b++) {
        mbr[b] = 0;
    }
    for (unsigned int b = 0; b < mbr_len && b < 512u; b++) {
        mbr[b] = mbr_bin[b];
    }
    write_u32(mbr + 0x1a0, lba);
    write_u32(mbr + 0x1a4, sectors);
    write_u32(mbr + 0x1a8, (unsigned int)(unsigned long)_start);
    mbr[0x1fe] = 0x55;
    mbr[0x1ff] = 0xaa;
    if (!ata_write(0, mbr)) {
        console_puts("install: failed\n");
        return 0;
    }
    ata_flush();
    console_puts("install: ok\n");
    return 1;
}
