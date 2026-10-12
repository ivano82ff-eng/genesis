#pragma once

#ifdef __x86_64__
#define USER_PML4_SLOT 1
#define USER_BASE 0x8000000000ul
#else
/* i686: 32-bit paging, the kernel identity-maps 4 GiB with 4 MiB pages except
 * the 8 MiB user window at 1 GiB (page-directory slots 256 and 257). */
#define USER_BASE 0x40000000ul
#endif
#define USER_CODE (USER_BASE + 0x10000ul)
#define USER_LIB_BASE (USER_BASE + 0x400000ul)
#define USER_LIB_SLOT_SIZE 0x100000ul
#define USER_LIB_SLOTS 3
#define USER_STACK_TOP (USER_BASE + 0x800000ul)
#define USER_END USER_STACK_TOP
#define USER_CS 0x23
#define USER_SS 0x1b

enum { SYS_EXIT = 0, SYS_PRINT = 1, SYS_SLEEP = 2, SYS_LIB = 3 };

/* Saved by irq.asm: the general registers, then vector + error code, then
 * the CPU's iretq frame. */
#ifdef __x86_64__
struct trap_frame {
    unsigned long r15, r14, r13, r12, r11, r10, r9, r8, rbp, rdi, rsi, rdx, rcx, rbx, rax;
    unsigned long vector, err;
    unsigned long rip, cs, rflags, rsp, ss;
};
#else
/* i686 (irq32.asm): pushad order, then vector + error code, then the iret
 * frame; esp/ss exist only for traps from ring 3. The system call number is
 * in eax and its argument in edi, so the 64-bit field names fit. */
struct trap_frame {
    unsigned long rdi, rsi, rbp, esp_dummy, rbx, rdx, rcx, rax;
    unsigned long vector, err;
    unsigned long rip, cs, rflags, rsp, ss;
};
#endif

struct user_start {
    unsigned long rip, rsp, print, sleep, sym, args;
};

unsigned long user_space_new(const unsigned char *image, unsigned int len, const char *args,
                             struct user_start *start);
void user_space_free(unsigned long cr3);
void user_libs_forget(unsigned long cr3);
unsigned long user_kernel_cr3(void);
void user_set_kstack(unsigned long top);
unsigned int user_pages_used(void);
unsigned int user_pages_total(void);
void user_init(void);
