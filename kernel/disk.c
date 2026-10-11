#include "fs_limits.h"

/*
 * Genesis Grid Store, format GBF2 (on-disk version 2) - a tree of folders.
 *
 * Disk layout, 512-byte sectors:
 *   LBA 0            MBR written by install_system
 *   LBA 32           superblock: "GBF2", u32 block_count, boot_flags byte,
 *                    u32 free_count @12, u32 version @16, u32 entries @20
 *   LBA 33..40       allocation bitmap, one bit per 4 KiB block (up to 128 MiB)
 *   LBA 64..         kernel copy (install_system), below LBA 2048
 *   LBA 2048..2087   catalog: FS_MAX entries of 80 bytes:
 *                    path[64], u32 length, u32 first_block, u32 block_count, u8 type
 *                    (type 1 = file, 2 = folder; paths are absolute)
 *   LBA 2112 + 8*b   data block b (4 KiB). A file is one contiguous run of blocks.
 *
 * Saves are incremental and transactional: unchanged files keep their blocks,
 * changed files get new blocks that are free in both the committed and the
 * staged bitmap (so the last committed state stays intact), then bitmap,
 * catalog and superblock are written, superblock last.
 *
 * The older flat format GBF1 (20 files, 15-char names) is read once and
 * migrated: built-ins are re-seeded, libraries go to /lib, the rest to /home.
 */

enum { ATA_DATA = 0x1f0, ATA_SECCNT = 0x1f2, ATA_LBA0 = 0x1f3, ATA_LBA1 = 0x1f4, ATA_LBA2 = 0x1f5, ATA_DRIVE = 0x1f6, ATA_STATUS = 0x1f7 };
enum { FS_HEADER_LBA = 32, FS_BITMAP_LBA = 33, FS_CAT_LBA = 2048, FS_DATA_LBA = 2112, FS_FLAG_HOME = 1, FS_FLAG_SEED = 2 };
enum { GRID_VERSION = 2, GRID_BLOCK_SIZE = 4096, GRID_SECTORS_PER_BLOCK = 8 };
enum { GRID_ENTRY_BYTES = 80, GRID_BITMAP_SECTORS = 8, GRID_CAT_SECTORS = FS_MAX * GRID_ENTRY_BYTES / 512 };
enum { GRID_MAX_BLOCKS = GRID_BITMAP_SECTORS * 512u * 8u };
/* Default 32 MiB disk when the drive cannot tell its size. */
enum { GRID_DISK_SECTORS = 65536 };
/* GBF1, read only for migration. */
enum { OLD_DIR_LBA = 34, OLD_DATA_LBA = 2048, OLD_ENTRY = 84, OLD_MAX = 20, OLD_NAME = 16, OLD_DATA = 65536 };

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
extern int fs_load_entry(const char *abs, int is_dir, const unsigned char *data, unsigned int length,
                         unsigned int blk, unsigned int nblk);
extern int fs_slot(int i, const char **name, const unsigned char **data, unsigned int *length,
                   int *is_dir, int *dirty, unsigned int *blk, unsigned int *nblk);
extern void fs_slot_saved(int i, unsigned int blk, unsigned int nblk);
extern void fs_mark_all_dirty(void);
extern int fs_builtin_name(const char *name);
extern void console_puts(const char *text);

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
static int migrated;

static unsigned char grid_bitmap[GRID_BITMAP_SECTORS * 512];
static unsigned char grid_next[GRID_BITMAP_SECTORS * 512];
static unsigned char grid_cat[GRID_CAT_SECTORS * 512];
static unsigned int grid_block_count;
static unsigned int pend_blk[FS_MAX];
static unsigned int pend_n[FS_MAX];

static unsigned char *const file_io = (unsigned char *)FS_IO_ADDR;

static int grid_bit_get(const unsigned char *map, unsigned int block) {
    return (map[block >> 3] >> (block & 7u)) & 1u;
}

static void grid_bit_set(unsigned char *map, unsigned int block) {
    map[block >> 3] |= (unsigned char)(1u << (block & 7u));
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

/* First run of N blocks free in `map` (and in `also`, when given). */
static int grid_find_run(const unsigned char *map, const unsigned char *also, unsigned int blocks,
                         unsigned int n, unsigned int *out) {
    unsigned int run = 0;
    for (unsigned int b = 0; b < blocks; b++) {
        if (grid_bit_get(map, b) || (also != 0 && grid_bit_get(also, b))) {
            run = 0;
            continue;
        }
        if (++run == n) {
            *out = b + 1 - n;
            return 1;
        }
    }
    return 0;
}

static int read_sectors(unsigned int lba, unsigned int count, unsigned char *dest) {
    for (unsigned int s = 0; s < count; s++) {
        if (!ata_read(lba + s, dest + s * 512u)) {
            return 0;
        }
    }
    return 1;
}

static int write_sectors(unsigned int lba, unsigned int count, const unsigned char *src, unsigned int length) {
    unsigned char sector[512];
    for (unsigned int s = 0; s < count; s++) {
        unsigned int offset = s * 512u;
        for (unsigned int i = 0; i < 512u; i++) {
            sector[i] = offset + i < length ? src[offset + i] : 0;
        }
        if (!ata_write(lba + s, sector)) {
            return 0;
        }
    }
    return 1;
}

/* Disk size from ATA IDENTIFY (LBA28 sector count), in Grid Store blocks. */
static unsigned int grid_disk_blocks(void) {
    unsigned int sectors = GRID_DISK_SECTORS;
    outb(ATA_DRIVE, 0xe0);
    ata_delay();
    if (ata_ready()) {
        outb(ATA_STATUS, 0xec);
        if (ata_data()) {
            unsigned short id[256];
            for (int i = 0; i < 256; i++) {
                id[i] = inw(ATA_DATA);
            }
            unsigned int n = (unsigned int)id[60] | ((unsigned int)id[61] << 16);
            if (n > FS_DATA_LBA + 64u) {
                sectors = n;
            }
        }
    }
    unsigned int blocks = (sectors - FS_DATA_LBA) / GRID_SECTORS_PER_BLOCK;
    return blocks > GRID_MAX_BLOCKS ? GRID_MAX_BLOCKS : blocks;
}

static void grid_write_super(unsigned char *sector, unsigned int blocks, unsigned int free_count) {
    for (int i = 0; i < 512; i++) {
        sector[i] = 0;
    }
    sector[0] = 'G';
    sector[1] = 'B';
    sector[2] = 'F';
    sector[3] = '2';
    write_u32(sector + 4, blocks);
    sector[8] = boot_flags;
    write_u32(sector + 12, free_count);
    write_u32(sector + 16, GRID_VERSION);
    write_u32(sector + 20, FS_MAX);
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

/* GBF1: the old flat catalog. Files land in RAM; the next save writes GBF2. */
static int disk_migrate_gbf1(void) {
    unsigned char dir[4 * 512];
    if (!read_sectors(OLD_DIR_LBA, 4, dir)) {
        return 0;
    }
    fs_clear_all();
    fs_load_entry("/home", 1, 0, 0, 0, 0);
    fs_load_entry("/lib", 1, 0, 0, 0, 0);
    unsigned int count = 0;
    for (unsigned int i = 0; i < OLD_MAX; i++) {
        const unsigned char *entry = dir + i * OLD_ENTRY;
        char name[OLD_NAME];
        unsigned int n = 0;
        while (n + 1 < OLD_NAME && entry[n] != 0 && entry[n] != '/') {
            name[n] = (char)entry[n];
            n++;
        }
        name[n] = 0;
        unsigned int length = read_u32(entry + 16);
        if (name[0] == 0 || length > OLD_DATA || fs_builtin_name(name)) {
            continue;
        }
        unsigned int chunks = (length + GRID_BLOCK_SIZE - 1u) / GRID_BLOCK_SIZE;
        int ok = 1;
        for (unsigned int k = 0; k < chunks && ok; k++) {
            unsigned int block = read_u32(entry + 20 + k * 4);
            ok = block < 8192u &&
                 read_sectors(OLD_DATA_LBA + block * 8u, 8, file_io + k * GRID_BLOCK_SIZE);
        }
        if (!ok) {
            continue;
        }
        int lib = length >= 4 && file_io[0] == 'G' && file_io[1] == 'L' && file_io[2] == 'I' && file_io[3] == 'B';
        char path[FS_NAME];
        const char *prefix = lib ? "/lib/" : "/home/";
        unsigned int p = 0;
        while (*prefix != 0) {
            path[p++] = *prefix++;
        }
        for (unsigned int k = 0; k <= n; k++) {
            path[p++] = name[k];
        }
        if (fs_load_entry(path, 0, file_io, length, 0, 0)) {
            count++;
        }
    }
    fs_mark_all_dirty();
    boot_flags = 0;
    grid_block_count = grid_disk_blocks();
    for (unsigned int i = 0; i < sizeof grid_bitmap; i++) {
        grid_bitmap[i] = 0;
    }
    migrated = 1;
    char msg[] = "disk: migrated GBF1 -> GBF2, 000 files kept\n";
    msg[29] = (char)('0' + count / 100u % 10u);
    msg[30] = (char)('0' + count / 10u % 10u);
    msg[31] = (char)('0' + count % 10u);
    console_puts(msg);
    return 1;
}

static int disk_load_once(void) {
    unsigned char sector[512];
    if (!ata_read(FS_HEADER_LBA, sector)) {
        return 0;
    }
    if (sector[0] == 'G' && sector[1] == 'B' && sector[2] == 'F' && sector[3] == '1') {
        return disk_migrate_gbf1();
    }
    if (sector[0] != 'G' || sector[1] != 'B' || sector[2] != 'F' || sector[3] != '2' ||
        read_u32(sector + 16) != GRID_VERSION) {
        return 0;
    }
    unsigned int blocks = read_u32(sector + 4);
    if (blocks == 0 || blocks > GRID_MAX_BLOCKS) {
        return 0;
    }
    boot_flags = sector[8];
    unsigned int grown = grid_disk_blocks(); /* the host may have enlarged the image */
    if (grown > blocks) {
        blocks = grown;
    }
    if (!read_sectors(FS_BITMAP_LBA, GRID_BITMAP_SECTORS, grid_bitmap) ||
        !read_sectors(FS_CAT_LBA, GRID_CAT_SECTORS, grid_cat)) {
        return 0;
    }
    grid_block_count = blocks;
    fs_clear_all();
    for (unsigned int i = 0; i < FS_MAX; i++) {
        const unsigned char *entry = grid_cat + i * GRID_ENTRY_BYTES;
        char name[FS_NAME];
        unsigned int n = 0;
        while (n + 1 < FS_NAME && entry[n] != 0) {
            name[n] = (char)entry[n];
            n++;
        }
        name[n] = 0;
        unsigned int length = read_u32(entry + 64);
        unsigned int blk = read_u32(entry + 68);
        unsigned int nblk = read_u32(entry + 72);
        unsigned char type = entry[76];
        if (name[0] != '/') {
            continue;
        }
        if (type == 2) {
            fs_load_entry(name, 1, 0, 0, 0, 0);
            continue;
        }
        if (type != 1 || length > FS_DATA || length > nblk * GRID_BLOCK_SIZE || blk + nblk > blocks ||
            blk + nblk < blk) {
            continue;
        }
        if (!read_sectors(FS_DATA_LBA + blk * GRID_SECTORS_PER_BLOCK, (length + 511u) / 512u, file_io)) {
            return 0;
        }
        fs_load_entry(name, 0, file_io, length, blk, nblk);
    }
    return 1;
}

int disk_load(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        if (disk_load_once()) {
            return 1;
        }
    }
    return 0;
}

/* Did this boot convert an old disk? (kernel re-seeds built-ins then saves) */
int disk_consume_migrated(void) {
    int m = migrated;
    migrated = 0;
    return m;
}

/* 1 saved, 0 I/O error (retry), -1 no room. */
static int disk_save_once(void) {
    unsigned int blocks = grid_block_count;
    if (blocks == 0 || blocks > GRID_MAX_BLOCKS) {
        blocks = grid_disk_blocks();
    }
    for (unsigned int i = 0; i < sizeof grid_next; i++) {
        grid_next[i] = 0;
    }
    for (unsigned int i = 0; i < sizeof grid_cat; i++) {
        grid_cat[i] = 0;
    }
    const char *name;
    const unsigned char *data;
    unsigned int length, blk, nblk;
    int is_dir, dirty;
    /* Unchanged files keep their blocks. */
    for (int i = 0; i < FS_MAX; i++) {
        pend_n[i] = 0;
        if (!fs_slot(i, &name, &data, &length, &is_dir, &dirty, &blk, &nblk) || is_dir || dirty) {
            continue;
        }
        if (nblk == 0 || blk + nblk > blocks) {
            continue;
        }
        for (unsigned int b = 0; b < nblk; b++) {
            grid_bit_set(grid_next, blk + b);
        }
        pend_blk[i] = blk;
        pend_n[i] = nblk;
    }
    unsigned int slot = 0;
    for (int i = 0; i < FS_MAX; i++) {
        if (!fs_slot(i, &name, &data, &length, &is_dir, &dirty, &blk, &nblk)) {
            continue;
        }
        unsigned char *entry = grid_cat + slot * GRID_ENTRY_BYTES;
        slot++;
        for (unsigned int n = 0; name[n] != 0 && n + 1 < FS_NAME; n++) {
            entry[n] = (unsigned char)name[n];
        }
        if (is_dir) {
            entry[76] = 2;
            continue;
        }
        unsigned int need = (length + GRID_BLOCK_SIZE - 1u) / GRID_BLOCK_SIZE;
        if (need == 0) {
            need = 1;
        }
        if (pend_n[i] == 0) {
            unsigned int start = 0;
            if (!grid_find_run(grid_next, grid_bitmap, blocks, need, &start) &&
                !grid_find_run(grid_next, 0, blocks, need, &start)) {
                console_puts("disk: full\n");
                return -1;
            }
            for (unsigned int b = 0; b < need; b++) {
                grid_bit_set(grid_next, start + b);
            }
            if (!write_sectors(FS_DATA_LBA + start * GRID_SECTORS_PER_BLOCK, (length + 511u) / 512u, data,
                               length)) {
                return 0;
            }
            pend_blk[i] = start;
            pend_n[i] = need;
        }
        write_u32(entry + 64, length);
        write_u32(entry + 68, pend_blk[i]);
        write_u32(entry + 72, pend_n[i]);
        entry[76] = 1;
    }
    unsigned char sector[512];
    if (!write_sectors(FS_BITMAP_LBA, GRID_BITMAP_SECTORS, grid_next, sizeof grid_next) ||
        !write_sectors(FS_CAT_LBA, GRID_CAT_SECTORS, grid_cat, sizeof grid_cat)) {
        return 0;
    }
    grid_write_super(sector, blocks, grid_count_free(grid_next, blocks));
    if (!ata_write(FS_HEADER_LBA, sector)) {
        return 0;
    }
    ata_flush();
    for (unsigned int i = 0; i < sizeof grid_bitmap; i++) {
        grid_bitmap[i] = grid_next[i];
    }
    grid_block_count = blocks;
    for (int i = 0; i < FS_MAX; i++) {
        if (pend_n[i] != 0) {
            fs_slot_saved(i, pend_blk[i], pend_n[i]);
        }
    }
    return 1;
}

int disk_save(void) {
    for (int attempt = 0; attempt < 4; attempt++) {
        int r = disk_save_once();
        if (r != 0) {
            return r > 0;
        }
    }
    return 0;
}

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
    if (lba + sectors > FS_CAT_LBA) {
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
