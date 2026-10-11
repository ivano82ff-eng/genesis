#include "fs_limits.h"

/*
 * Genesis Grid Store (GBF1) — a flat block store, no directory tree.
 *
 * Disk layout, 512-byte sectors:
 *   LBA 0            MBR written by install_system
 *   LBA 32           superblock: "GBF1", u32 block_count, boot_flags byte, u32 free_count
 *   LBA 33           allocation bitmap, one bit per 4 KiB block
 *   LBA 34..37       flat catalog, up to FS_MAX entries of 84 bytes:
 *                    name[16], u32 length, u32 chunks[16]
 *   LBA 2048 + 8*b   data block b (4 KiB, 8 sectors)
 *
 * A file is a grid of up to 16 chunk numbers; 16 * 4 KiB = FS_DATA. Chunks
 * are scattered freely across the store — the catalog assembles them.
 *
 * Saves are transactional: new chunks are allocated on a scratch bitmap and
 * written to blocks that are still marked busy, so a failed save leaves the
 * last committed state untouched. Metadata is written last, superblock last
 * of all. A failed unlink can only leak blocks, never corrupt a file.
 */

enum { ATA_DATA = 0x1f0, ATA_SECCNT = 0x1f2, ATA_LBA0 = 0x1f3, ATA_LBA1 = 0x1f4, ATA_LBA2 = 0x1f5, ATA_DRIVE = 0x1f6, ATA_STATUS = 0x1f7 };
enum { FS_HEADER_LBA = 32, FS_BITMAP_LBA = 33, FS_DIR_LBA = 34, FS_DATA_LBA = 2048, FS_FLAG_HOME = 1, FS_FLAG_SEED = 2 };
enum { GRID_BLOCK_SIZE = 4096, GRID_SECTORS_PER_BLOCK = 8, GRID_CHUNKS = 16, GRID_NO_CHUNK = 0xFFFFFFFFu };
enum { GRID_ENTRY_BYTES = 84, GRID_BITMAP_SECTORS = 1, GRID_DIR_SECTORS = 4 };
enum { GRID_MAX_BLOCKS = GRID_BITMAP_SECTORS * 512u * 8u };
/* 8 MiB disk minus the 1 MiB system area, in 4 KiB blocks. */
enum { GRID_BLOCKS = 1792 };

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

/* Committed state; the *_next buffers stage a new one until it is fully on disk. */
static unsigned char grid_bitmap[GRID_BITMAP_SECTORS * 512];
static unsigned char grid_dir[GRID_DIR_SECTORS * 512];
static unsigned char grid_bitmap_next[GRID_BITMAP_SECTORS * 512];
static unsigned char grid_dir_next[GRID_DIR_SECTORS * 512];
static unsigned int grid_block_count = GRID_BLOCKS;

/* Keep the kernel image small for the MBR loader; use high RAM for I/O. */
static unsigned char *const file_io = (unsigned char *)0xA00000ul;

static int grid_bit_get(const unsigned char *map, unsigned int block) {
    return (map[block >> 3] >> (block & 7u)) & 1u;
}

static void grid_bit_set(unsigned char *map, unsigned int block) {
    map[block >> 3] |= (unsigned char)(1u << (block & 7u));
}

static void grid_bit_clear(unsigned char *map, unsigned int block) {
    map[block >> 3] &= (unsigned char)~(1u << (block & 7u));
}

static unsigned int grid_count_free(const unsigned char *map, unsigned int blocks) {
    unsigned int free_count = 0;
    for (unsigned int b = 0; b < blocks; b++) {
        if (!grid_bit_get(map, b)) {
            free_count++;
        }
    }
    return free_count;
}

static int grid_alloc_block(unsigned char *map, unsigned int blocks, unsigned int *out) {
    for (unsigned int b = 0; b < blocks; b++) {
        if (!grid_bit_get(map, b)) {
            grid_bit_set(map, b);
            *out = b;
            return 1;
        }
    }
    return 0;
}

static unsigned int chunk_lba(unsigned int block) {
    return FS_DATA_LBA + block * GRID_SECTORS_PER_BLOCK;
}

static int grid_read_chunk(unsigned int block, unsigned char *dest) {
    unsigned char sector[512];
    for (unsigned int s = 0; s < GRID_SECTORS_PER_BLOCK; s++) {
        if (!ata_read(chunk_lba(block) + s, sector)) {
            return 0;
        }
        for (unsigned int i = 0; i < 512; i++) {
            dest[s * 512u + i] = sector[i];
        }
    }
    return 1;
}

static int grid_write_chunk(unsigned int block, const unsigned char *src, unsigned int length) {
    unsigned char sector[512];
    for (unsigned int s = 0; s < GRID_SECTORS_PER_BLOCK; s++) {
        unsigned int offset = s * 512u;
        for (int i = 0; i < 512; i++) {
            sector[i] = 0;
        }
        unsigned int part = length > offset ? length - offset : 0;
        if (part > 512u) {
            part = 512u;
        }
        for (unsigned int i = 0; i < part; i++) {
            sector[i] = src[offset + i];
        }
        if (!ata_write(chunk_lba(block) + s, sector)) {
            return 0;
        }
    }
    return 1;
}

static int grid_name_eq(const unsigned char *entry, const char *name) {
    for (unsigned int i = 0; i < FS_NAME; i++) {
        if (entry[i] != (unsigned char)name[i]) {
            return 0;
        }
        if (entry[i] == 0) {
            return 1;
        }
    }
    return 0;
}

static void grid_write_super(unsigned char *sector, unsigned int blocks, unsigned int free_count) {
    for (int i = 0; i < 512; i++) {
        sector[i] = 0;
    }
    sector[0] = 'G';
    sector[1] = 'B';
    sector[2] = 'F';
    sector[3] = '1';
    write_u32(sector + 4, blocks);
    sector[8] = boot_flags;
    write_u32(sector + 12, free_count);
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

/* Write the staged bitmap, catalog, and superblock, then adopt them. */
static int grid_commit(unsigned int blocks) {
    unsigned char sector[512];
    unsigned int free_count = grid_count_free(grid_bitmap_next, blocks);
    for (unsigned int s = 0; s < GRID_BITMAP_SECTORS; s++) {
        if (!ata_write(FS_BITMAP_LBA + s, grid_bitmap_next + s * 512)) {
            return 0;
        }
    }
    for (unsigned int s = 0; s < GRID_DIR_SECTORS; s++) {
        if (!ata_write(FS_DIR_LBA + s, grid_dir_next + s * 512)) {
            return 0;
        }
    }
    grid_write_super(sector, blocks, free_count);
    if (!ata_write(FS_HEADER_LBA, sector)) {
        return 0;
    }
    for (unsigned int i = 0; i < GRID_BITMAP_SECTORS * 512; i++) {
        grid_bitmap[i] = grid_bitmap_next[i];
    }
    for (unsigned int i = 0; i < GRID_DIR_SECTORS * 512; i++) {
        grid_dir[i] = grid_dir_next[i];
    }
    grid_block_count = blocks;
    ata_flush();
    return 1;
}

static int disk_load_once(void) {
    unsigned char sector[512];
    if (!ata_read(FS_HEADER_LBA, sector)) {
        return 0;
    }
    if (sector[0] != 'G' || sector[1] != 'B' || sector[2] != 'F' || sector[3] != '1') {
        return 0;
    }
    unsigned int blocks = read_u32(sector + 4);
    if (blocks == 0 || blocks > GRID_MAX_BLOCKS) {
        return 0;
    }
    boot_flags = sector[8];
    for (unsigned int s = 0; s < GRID_BITMAP_SECTORS; s++) {
        if (!ata_read(FS_BITMAP_LBA + s, grid_bitmap + s * 512)) {
            return 0;
        }
    }
    for (unsigned int s = 0; s < GRID_DIR_SECTORS; s++) {
        if (!ata_read(FS_DIR_LBA + s, grid_dir + s * 512)) {
            return 0;
        }
    }
    grid_block_count = blocks;
    fs_clear_all();
    for (unsigned int i = 0; i < FS_MAX; i++) {
        const unsigned char *entry = grid_dir + i * GRID_ENTRY_BYTES;
        char name[FS_NAME];
        unsigned int n = 0;
        while (n + 1 < FS_NAME && entry[n] != 0) {
            name[n] = (char)entry[n];
            n++;
        }
        name[n] = 0;
        unsigned int length = read_u32(entry + 16);
        if (name[0] == 0 || length > FS_DATA) {
            continue;
        }
        unsigned int chunks = (length + GRID_BLOCK_SIZE - 1u) / GRID_BLOCK_SIZE;
        int ok = 1;
        for (unsigned int k = 0; k < chunks; k++) {
            if (read_u32(entry + 20 + k * 4) >= blocks) {
                ok = 0;
                break;
            }
        }
        if (!ok) {
            continue;
        }
        for (unsigned int k = 0; k < chunks; k++) {
            unsigned int block = read_u32(entry + 20 + k * 4);
            if (!grid_read_chunk(block, file_io + k * GRID_BLOCK_SIZE)) {
                return 0;
            }
        }
        fs_import(name, file_io, length);
    }
    return 1;
}

static int disk_load_retry(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (disk_load_once()) {
            return 1;
        }
    }
    return 0;
}

int disk_load(void) {
    return disk_load_retry();
}

static int disk_save_once(void) {
    unsigned int blocks = grid_block_count;
    if (blocks == 0 || blocks > GRID_MAX_BLOCKS) {
        blocks = GRID_BLOCKS;
    }
    for (unsigned int i = 0; i < GRID_BITMAP_SECTORS * 512; i++) {
        grid_bitmap_next[i] = 0;
    }
    for (unsigned int i = 0; i < GRID_DIR_SECTORS * 512; i++) {
        grid_dir_next[i] = 0;
    }
    for (unsigned int i = 0; i < FS_MAX; i++) {
        char name[FS_NAME];
        unsigned int length = 0;
        if (!fs_export(i, name, FS_NAME, file_io, &length)) {
            continue;
        }
        unsigned int chunks = (length + GRID_BLOCK_SIZE - 1u) / GRID_BLOCK_SIZE;
        unsigned char *entry = grid_dir_next + i * GRID_ENTRY_BYTES;
        unsigned int n = 0;
        while (name[n] != 0 && n + 1 < FS_NAME) {
            entry[n] = (unsigned char)name[n];
            n++;
        }
        entry[n] = 0;
        write_u32(entry + 16, length);
        for (unsigned int k = 0; k < GRID_CHUNKS; k++) {
            write_u32(entry + 20 + k * 4, GRID_NO_CHUNK);
        }
        for (unsigned int k = 0; k < chunks; k++) {
            unsigned int block = 0;
            if (!grid_alloc_block(grid_bitmap_next, blocks, &block)) {
                return 0;
            }
            write_u32(entry + 20 + k * 4, block);
            unsigned int bytes = length - k * GRID_BLOCK_SIZE;
            if (bytes > GRID_BLOCK_SIZE) {
                bytes = GRID_BLOCK_SIZE;
            }
            if (!grid_write_chunk(block, file_io + k * GRID_BLOCK_SIZE, bytes)) {
                return 0;
            }
        }
    }
    return grid_commit(blocks);
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

/* Set by the host importer (src/import.ts); cleared by the next save. */
int disk_consume_seed(void) {
    if ((boot_flags & FS_FLAG_SEED) == 0) {
        return 0;
    }
    boot_flags = (unsigned char)(boot_flags & (unsigned char)~FS_FLAG_SEED);
    return 1;
}

int disk_save(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (disk_save_once()) {
            return 1;
        }
    }
    return 0;
}

int grid_unlink(const char *name) {
    if (grid_block_count == 0 || name[0] == 0) {
        return 0;
    }
    for (unsigned int s = 0; s < GRID_BITMAP_SECTORS * 512; s++) {
        grid_bitmap_next[s] = grid_bitmap[s];
    }
    for (unsigned int i = 0; i < GRID_DIR_SECTORS * 512; i++) {
        grid_dir_next[i] = grid_dir[i];
    }
    for (unsigned int i = 0; i < FS_MAX; i++) {
        unsigned char *entry = grid_dir_next + i * GRID_ENTRY_BYTES;
        if (!grid_name_eq(entry, name)) {
            continue;
        }
        for (unsigned int k = 0; k < GRID_CHUNKS; k++) {
            unsigned int block = read_u32(entry + 20 + k * 4);
            if (block < grid_block_count) {
                grid_bit_clear(grid_bitmap_next, block);
            }
        }
        for (unsigned int b = 0; b < GRID_ENTRY_BYTES; b++) {
            entry[b] = 0;
        }
        return grid_commit(grid_block_count);
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
    if (lba + sectors > FS_DATA_LBA) {
        console_puts("install: too big\n");
        return 0;
    }
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
