#pragma once

/* Grid Store limits: FS_NAME is a whole path including its NUL, FS_PART one
 * path component, FS_DATA the biggest file. */
enum { FS_NAME = 64, FS_PART = 32, FS_DATA = 1024 * 1024, FS_MAX = 256 };
enum { PROG_LOAD_ADDR = 0x600000ul };
/* High RAM (QEMU runs with 128 MiB, identity mapped):
 *   0x0A00000 disk I/O buffer, 0x0B00000 shell scratch, 0x0C00000 editor text,
 *   0x0D00000 move scratch, 0x1000000.. file pool (48 MiB),
 *   0x4000000.. page frames for user tasks (16 MiB, see user.c). */
enum { FS_IO_ADDR = 0xA00000ul, FS_EDIT_ADDR = 0xB00000ul, EDITOR_BUF_ADDR = 0xC00000ul,
       FS_MOVE_ADDR = 0xD00000ul, FS_POOL_ADDR = 0x1000000ul, FS_POOL_SIZE = 48ul * 1024ul * 1024ul,
       USER_POOL_ADDR = 0x4000000ul };
