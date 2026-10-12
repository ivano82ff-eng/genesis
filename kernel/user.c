/* Ring 3 for disk programs: GDT with user segments + TSS, a page-frame pool,
 * one address space per task, the int 0x80 system call gate and the CPU
 * exception handlers that stop a faulting task instead of the whole system.
 *
 * Layout of a user address space (PML4 slot 1, everything else supervisor):
 *   USER_BASE            stub page (read-only, executable): print/sleep/lib/exit
 *   USER_BASE + 64 KiB   program image + USER_HEAP zeroed bytes
 *   USER_BASE + 4 MiB    libraries, 1 MiB slot each
 *   USER_STACK_TOP       stack (NX), 64 KiB, args string at the very top */
#include "fs_limits.h"
#include "user.h"

void console_puts(const char *text);
void console_putc(char ch);
void desktop_link(const char *from, const char *to);
int pkg_lib_blob(const char *name, const unsigned char **data, unsigned int *length);
void task_sleep(unsigned int n);
void idt_gate(int number, void *handler, unsigned char attr, unsigned char ist);
__attribute__((noreturn)) void task_user_died(const char *why); /* task.c */

extern char user_stub_start[], user_stub_end[];
extern char stub_print[], stub_sleep[], stub_sym[], stub_exit[];
extern void isr_syscall(void);
extern void isr_exc0(void), isr_exc1(void), isr_exc3(void), isr_exc4(void), isr_exc5(void),
    isr_exc6(void), isr_exc7(void), isr_exc8(void), isr_exc10(void), isr_exc11(void),
    isr_exc12(void), isr_exc13(void), isr_exc14(void), isr_exc16(void), isr_exc17(void),
    isr_exc19(void);

enum {
    PG_P = 1, PG_RW = 2, PG_U = 4,
    POOL_BASE = USER_POOL_ADDR, POOL_PAGES = 4096, /* 16 MiB of frames */
    USER_STACK_PAGES = 16, USER_HEAP = 64 * 1024,
};
#ifdef __x86_64__
#define PG_NX (1ul << 63)
#define ADDR_MASK 0x000ffffffffff000ul
#define PT_ENTRIES 512
/* Index shifts of the table levels above the page table: PML4, PDPT, PD. */
static const int pt_shifts[] = {39, 30, 21};
#else
/* i686: two-level 32-bit paging, no NX bit. */
#define PG_NX 0ul
#define ADDR_MASK 0xfffff000ul
#define PT_ENTRIES 1024
static const int pt_shifts[] = {22};
#define USER_PDE_FIRST (USER_BASE >> 22)
#define USER_PDE_LAST ((USER_END - 1) >> 22)
#endif
#define PT_LEVELS ((int)(sizeof pt_shifts / sizeof pt_shifts[0]))

static unsigned char frame_used[POOL_PAGES];
static unsigned int frames_in_use;
static int nx_ok;
static unsigned long kernel_cr3;

/* ---------- frames ---------- */

static unsigned long frame_alloc(void) {
    for (unsigned int i = 0; i < POOL_PAGES; i++) {
        if (!frame_used[i]) {
            frame_used[i] = 1;
            frames_in_use++;
            unsigned long *p = (unsigned long *)(POOL_BASE + (unsigned long)i * 4096ul);
            for (unsigned int k = 0; k < 4096 / sizeof(unsigned long); k++) {
                p[k] = 0;
            }
            return (unsigned long)p;
        }
    }
    return 0;
}

static void frame_free(unsigned long addr) {
    if (addr < POOL_BASE) {
        return;
    }
    unsigned long i = (addr - POOL_BASE) / 4096ul;
    if (i < POOL_PAGES && frame_used[i]) {
        frame_used[i] = 0;
        frames_in_use--;
    }
}

unsigned int user_pages_used(void) {
    return frames_in_use;
}

unsigned int user_pages_total(void) {
    return POOL_PAGES;
}

/* ---------- address spaces ---------- */

static unsigned long *table_of(unsigned long entry) {
    return (unsigned long *)(entry & ADDR_MASK);
}

/* Page-table entry for VA in address space CR3, created on demand. */
static unsigned long *pte_for(unsigned long cr3, unsigned long va, int create) {
    unsigned long *t = (unsigned long *)cr3;
    for (int lvl = 0; lvl < PT_LEVELS; lvl++) {
        unsigned long *e = &t[(va >> pt_shifts[lvl]) & (PT_ENTRIES - 1)];
        if (!(*e & PG_P)) {
            if (!create) {
                return 0;
            }
            unsigned long f = frame_alloc();
            if (f == 0) {
                return 0;
            }
            *e = f | PG_P | PG_RW | PG_U;
        }
        t = table_of(*e);
    }
    return &t[(va >> 12) & (PT_ENTRIES - 1)];
}

/* Map a fresh zeroed page at VA; returns its kernel (identity) address. */
static unsigned char *map_new(unsigned long cr3, unsigned long va, unsigned long flags) {
    unsigned long *pte = pte_for(cr3, va, 1);
    if (pte == 0) {
        return 0;
    }
    if (*pte & PG_P) {
        return (unsigned char *)(*pte & ADDR_MASK);
    }
    unsigned long f = frame_alloc();
    if (f == 0) {
        return 0;
    }
    if (!nx_ok) {
        flags &= ~PG_NX;
    }
    *pte = f | PG_P | PG_U | flags;
    return (unsigned char *)f;
}

/* Copy LEN bytes to user VA (pages mapped with FLAGS), plus EXTRA zero bytes. */
static int map_copy(unsigned long cr3, unsigned long va, const unsigned char *src, unsigned int len,
                    unsigned int extra, unsigned long flags) {
    unsigned long total = (unsigned long)len + extra;
    for (unsigned long off = 0; off < total; off += 4096) {
        unsigned char *page = map_new(cr3, va + off, flags);
        if (page == 0) {
            return 0;
        }
        for (unsigned long k = 0; k < 4096 && off + k < len; k++) {
            page[k] = src[off + k];
        }
    }
    return 1;
}

static void free_level(unsigned long *t, int lvl) {
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (!(t[i] & PG_P)) {
            continue;
        }
        if (lvl < PT_LEVELS) {
            free_level(table_of(t[i]), lvl + 1);
        }
        frame_free(t[i] & ADDR_MASK);
    }
}

void user_space_free(unsigned long cr3) {
    if (cr3 == 0) {
        return;
    }
    unsigned long *pml4 = (unsigned long *)cr3;
#ifdef __x86_64__
    unsigned long e = pml4[USER_PML4_SLOT];
    if (e & PG_P) {
        free_level(table_of(e), 1);
        frame_free(e & ADDR_MASK);
    }
#else
    for (unsigned long i = USER_PDE_FIRST; i <= USER_PDE_LAST; i++) {
        if (pml4[i] & PG_P) {
            free_level(table_of(pml4[i]), 1);
            frame_free(pml4[i] & ADDR_MASK);
        }
    }
#endif
    frame_free(cr3);
}

unsigned long user_kernel_cr3(void) {
    return kernel_cr3;
}

/* Builds the address space of a new task. 0 when memory runs out. */
unsigned long user_space_new(const unsigned char *image, unsigned int len, const char *args,
                             struct user_start *start) {
    unsigned long cr3 = frame_alloc();
    if (cr3 == 0) {
        return 0;
    }
    unsigned long *pml4 = (unsigned long *)cr3;
    unsigned long *kpml4 = (unsigned long *)kernel_cr3;
#ifdef __x86_64__
    pml4[0] = kpml4[0]; /* the kernel, supervisor-only (no U bit) */
#else
    for (unsigned long i = 0; i < PT_ENTRIES; i++) {
        if (i < USER_PDE_FIRST || i > USER_PDE_LAST) {
            pml4[i] = kpml4[i]; /* 4 MiB kernel pages, supervisor-only */
        }
    }
#endif
    unsigned int stub_len = (unsigned int)(user_stub_end - user_stub_start);
    int ok = map_copy(cr3, USER_BASE, (const unsigned char *)user_stub_start, stub_len, 0, 0) &&
             map_copy(cr3, USER_CODE, image, len, USER_HEAP, PG_RW);
    unsigned char *top = 0;
    for (int i = 1; ok && i <= USER_STACK_PAGES; i++) {
        top = map_new(cr3, USER_STACK_TOP - (unsigned long)i * 4096ul, PG_RW | PG_NX);
        ok = top != 0;
    }
    if (!ok) {
        user_space_free(cr3);
        return 0;
    }
    /* Last page of the stack: args string at the top, then the stack. */
    unsigned char *last = (unsigned char *)(*pte_for(cr3, USER_STACK_TOP - 4096, 0) & ADDR_MASK);
    unsigned int n = 0;
    while (args != 0 && args[n] != 0 && n < 200) {
        n++;
    }
    while (n > 0 && args[n - 1] == ' ') {
        n--;
    }
    unsigned long args_va = USER_STACK_TOP - 256;
    for (unsigned int i = 0; i < n; i++) {
        last[4096 - 256 + i] = (unsigned char)args[i];
    }
    last[4096 - 256 + n] = 0;
    /* Return address of the entry: the exit stub; rsp % 16 == 8 as after a call. */
#ifdef __x86_64__
    unsigned long rsp = args_va - 16 - 8;
    *(unsigned long *)(last + (rsp - (USER_STACK_TOP - 4096))) =
        USER_BASE + (unsigned long)(stub_exit - user_stub_start);
#else
    /* cdecl: return address, then print, sleep, sym, args on the stack. */
    unsigned long rsp = args_va - 16 - 4 - 16;
    unsigned long *frame = (unsigned long *)(last + (rsp - (USER_STACK_TOP - 4096)));
    frame[0] = USER_BASE + (unsigned long)(stub_exit - user_stub_start);
    frame[1] = USER_BASE + (unsigned long)(stub_print - user_stub_start);
    frame[2] = USER_BASE + (unsigned long)(stub_sleep - user_stub_start);
    frame[3] = USER_BASE + (unsigned long)(stub_sym - user_stub_start);
    frame[4] = n > 0 ? args_va : 0;
#endif
    start->rip = USER_CODE;
    start->rsp = rsp;
    start->print = USER_BASE + (unsigned long)(stub_print - user_stub_start);
    start->sleep = USER_BASE + (unsigned long)(stub_sleep - user_stub_start);
    start->sym = USER_BASE + (unsigned long)(stub_sym - user_stub_start);
    start->args = n > 0 ? args_va : 0;
    return cr3;
}

static unsigned long read_cr3(void) {
    unsigned long v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

/* 1 when [addr, addr+len) is mapped user memory of the current task. */
static int user_ok(unsigned long addr, unsigned long len) {
    if (addr < USER_BASE || addr >= USER_END || len > USER_END - addr) {
        return 0;
    }
    unsigned long cr3 = read_cr3();
    for (unsigned long p = addr & ~4095ul; p < addr + len; p += 4096) {
        unsigned long *pte = pte_for(cr3, p, 0);
        if (pte == 0 || (*pte & (PG_P | PG_U)) != (PG_P | PG_U)) {
            return 0;
        }
    }
    return 1;
}

/* Copies a user string (at most CAP-1 bytes); -1 if it is not readable. */
static int user_str(unsigned long addr, char *out, unsigned int cap) {
    for (unsigned int i = 0; i + 1 < cap; i++) {
        if ((i == 0 || ((addr + i) & 4095ul) == 0) && !user_ok(addr + i, 1)) {
            return -1;
        }
        out[i] = *(const char *)(addr + i);
        if (out[i] == 0) {
            return (int)i;
        }
    }
    out[cap - 1] = 0;
    return (int)cap - 1;
}

/* ---------- per-task libraries ---------- */

static struct {
    unsigned long cr3;
    char name[FS_NAME];
    unsigned long va;
} libmap[16];

static void s_copy(char *d, const char *s, unsigned int cap) {
    unsigned int n = 0;
    while (s[n] != 0 && n + 1 < cap) {
        d[n] = s[n];
        n++;
    }
    d[n] = 0;
}

static int s_eq(const char *a, const char *b) {
    while (*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void user_libs_forget(unsigned long cr3) {
    for (int i = 0; i < 16; i++) {
        if (libmap[i].cr3 == cr3) {
            libmap[i].cr3 = 0;
        }
    }
}

/* Maps installed library NAME into the current task; its user address or 0. */
static unsigned long lib_map(const char *name) {
    unsigned long cr3 = read_cr3();
    const unsigned char *data;
    unsigned int len;
    int used = 0;
    int free_i = -1;
    for (int i = 0; i < 16; i++) {
        if (libmap[i].cr3 == cr3) {
            if (s_eq(libmap[i].name, name)) {
                if (pkg_lib_blob(name, &data, &len)) {
                    return libmap[i].va;
                }
                return 0;
            }
            used++;
        } else if (libmap[i].cr3 == 0 && free_i < 0) {
            free_i = i;
        }
    }
    if (!pkg_lib_blob(name, &data, &len) || free_i < 0 || used >= USER_LIB_SLOTS ||
        len > USER_LIB_SLOT_SIZE) {
        return 0;
    }
    unsigned long va = USER_LIB_BASE + (unsigned long)used * USER_LIB_SLOT_SIZE;
    if (!map_copy(cr3, va, data, len, 4096, PG_RW)) {
        return 0;
    }
    libmap[free_i].cr3 = cr3;
    s_copy(libmap[free_i].name, name, FS_NAME);
    libmap[free_i].va = va;
    return va;
}

/* ---------- traps ---------- */

static void put_hex(unsigned long v) {
    char s[19];
    s[0] = '0';
    s[1] = 'x';
    int n = 0;
    char d[16];
    do {
        d[n++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    int k = 2;
    while (n > 0) {
        s[k++] = d[--n];
    }
    s[k] = 0;
    console_puts(s);
}

static const char *exc_name(unsigned long v) {
    switch (v) {
    case 0: return "divide error";
    case 1: return "debug trap";
    case 3: return "breakpoint";
    case 4: return "overflow";
    case 5: return "bound range";
    case 6: return "invalid opcode";
    case 7: return "no FPU";
    case 8: return "double fault";
    case 10: return "bad TSS";
    case 11: return "segment not present";
    case 12: return "stack fault";
    case 13: return "general protection fault";
    case 14: return "page fault";
    case 16: return "FPU error";
    case 17: return "alignment check";
    case 19: return "SIMD error";
    default: return "CPU exception";
    }
}

static __attribute__((noreturn)) void panic(struct trap_frame *f) {
    __asm__ volatile("cli");
    unsigned long cr2;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    console_puts("\nKERNEL PANIC: ");
    console_puts(exc_name(f->vector));
    console_puts(" rip ");
    put_hex(f->rip);
    console_puts(" err ");
    put_hex(f->err);
    if (f->vector == 14) {
        console_puts(" addr ");
        put_hex(cr2);
    }
#ifdef __x86_64__
    unsigned long ksp = f->rsp;
#else
    unsigned long ksp = (unsigned long)&f->rsp; /* no esp/ss in a ring-0 frame */
#endif
    console_puts(" rsp ");
    put_hex(ksp);
    console_puts("\ncode");
    for (unsigned long a = f->rip - 8; a < f->rip + 8; a++) {
        console_puts(" ");
        put_hex(*(const unsigned char *)a);
    }
    console_puts("\nstack");
    for (int i = 0; i < 6; i++) {
        console_puts(" ");
        put_hex(((const unsigned long *)ksp)[i]);
    }    console_puts("\nsystem halted\n");
    for (;;) {
        __asm__ volatile("cli; hlt");
    }
}

static char crash_text[96];

static void crash_cat(unsigned int *n, const char *s) {
    while (*s != 0 && *n + 1 < sizeof crash_text) {
        crash_text[(*n)++] = *s++;
    }
    crash_text[*n] = 0;
}

static void crash_hex(unsigned int *n, unsigned long v) {
    char d[16];
    int k = 0;
    do {
        d[k++] = "0123456789abcdef"[v & 15];
        v >>= 4;
    } while (v != 0);
    crash_cat(n, "0x");
    while (k > 0 && *n + 1 < sizeof crash_text) {
        crash_text[(*n)++] = d[--k];
    }
    crash_text[*n] = 0;
}

/* "page fault at 0x0" etc.; the task layer adds "task N (name): ". */
static __attribute__((noreturn)) void user_fault(struct trap_frame *f) {
    unsigned int n = 0;
    crash_text[0] = 0;
    crash_cat(&n, exc_name(f->vector));
    if (f->vector == 14) {
        unsigned long cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        crash_cat(&n, " at ");
        crash_hex(&n, cr2);
        crash_cat(&n, (f->err & 16) ? " (exec" : (f->err & 2) ? " (write" : " (read");
        crash_cat(&n, cr2 < USER_BASE ? ", kernel memory)" : ")");
        crash_cat(&n, " rip ");
        crash_hex(&n, f->rip);
    } else {
        crash_cat(&n, " at ");
        crash_hex(&n, f->rip);
    }
    task_user_died(crash_text);
}

static __attribute__((noreturn)) void bad_pointer(const char *call, unsigned long p) {
    unsigned int n = 0;
    crash_text[0] = 0;
    crash_cat(&n, "bad pointer ");
    crash_hex(&n, p);
    crash_cat(&n, " in ");
    crash_cat(&n, call);
    task_user_died(crash_text);
}

const char *task_current_name(void);

static void sys_print(unsigned long p) {
    char buf[512];
    for (unsigned int total = 0; total < 65536;) {
        int n = user_str(p, buf, sizeof buf);
        if (n < 0) {
            bad_pointer("print", p);
        }
        if (n == 0) {
            return;
        }
        int cut = n;
        if (n == (int)sizeof buf - 1) {
            /* never split a UTF-8 character between two console writes */
            while (cut > 1 && ((unsigned char)buf[cut] & 0xc0) == 0x80) {
                cut--;
            }
            if (cut > 1 && ((unsigned char)buf[cut - 1] & 0xc0) == 0xc0) {
                cut--;
            }
            buf[cut] = 0;
        }
        console_puts(buf);
        if (n < (int)sizeof buf - 1) {
            return;
        }
        p += (unsigned long)cut;
        total += (unsigned int)cut;
    }
}

void fault_handler(struct trap_frame *f) {
    int user = (f->cs & 3) == 3;
    if (f->vector != 0x80) {
        if (!user) {
            panic(f);
        }
        user_fault(f);
    }
    __asm__ volatile("sti" ::: "memory");
    switch (f->rax) {
    case SYS_EXIT:
        task_user_died(0);
    case SYS_PRINT:
        sys_print(f->rdi);
        f->rax = 0;
        return;
    case SYS_SLEEP:
        task_sleep(f->rdi > 360000 ? 360000 : (unsigned int)f->rdi);
        f->rax = 0;
        return;
    case SYS_LIB: {
        char name[FS_NAME];
        if (user_str(f->rdi, name, sizeof name) < 0) {
            bad_pointer("lib", f->rdi);
        }
        f->rax = lib_map(name);
        if (f->rax != 0) {
            desktop_link(task_current_name(), name);
        }
        return;
    }
    default:
        f->rax = (unsigned long)-1;
        return;
    }
}

/* ---------- GDT + TSS ---------- */

#ifdef __x86_64__
struct tss {
    unsigned int reserved0;
    unsigned long rsp[3];
    unsigned long reserved1;
    unsigned long ist[7];
    unsigned long reserved2;
    unsigned short reserved3;
    unsigned short iomap;
} __attribute__((packed));

static struct tss tss;
static unsigned long gdt[7];
static unsigned char df_stack[8192] __attribute__((aligned(16)));

void user_set_kstack(unsigned long top) {
    tss.rsp[0] = top;
}

static void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00209a0000000000ul; /* 0x08 kernel code */
    gdt[2] = 0x0000920000000000ul; /* 0x10 kernel data */
    gdt[3] = 0x0000f20000000000ul; /* 0x18 user data (0x1b) */
    gdt[4] = 0x0020fa0000000000ul; /* 0x20 user code (0x23) */
    unsigned long base = (unsigned long)&tss;
    unsigned long limit = sizeof tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ul << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    gdt[6] = base >> 32;
    tss.iomap = sizeof tss; /* no I/O bitmap: user in/out faults */
    tss.ist[0] = (unsigned long)(df_stack + sizeof df_stack);
    struct {
        unsigned short limit;
        unsigned long base;
    } __attribute__((packed)) ptr = {sizeof gdt - 1, (unsigned long)gdt};
    __asm__ volatile(
        "lgdt %0\n\t"
        "pushq $0x08\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "pushq %%rax\n\t"
        "lretq\n"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%ss\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(ptr)
        : "rax", "memory");
}

#else
/* i686 TSS: only esp0/ss0 matter (ring 3 -> ring 0 stack switch). */
struct tss {
    unsigned int link, esp0, ss0, esp1, ss1, esp2, ss2, cr3, eip, eflags;
    unsigned int eax, ecx, edx, ebx, esp, ebp, esi, edi;
    unsigned int es, cs, ss, ds, fs, gs, ldt;
    unsigned short trap, iomap;
} __attribute__((packed));

static struct tss tss;
static unsigned long long gdt[6];

void user_set_kstack(unsigned long top) {
    tss.esp0 = top;
}

static void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x00cf9a000000ffffull; /* 0x08 kernel code, flat 4 GiB */
    gdt[2] = 0x00cf92000000ffffull; /* 0x10 kernel data */
    gdt[3] = 0x00cff2000000ffffull; /* 0x18 user data (0x1b) */
    gdt[4] = 0x00cffa000000ffffull; /* 0x20 user code (0x23) */
    unsigned long long base = (unsigned long)&tss;
    unsigned long long limit = sizeof tss - 1;
    gdt[5] = (limit & 0xffff) | ((base & 0xffffff) << 16) | (0x89ull << 40) |
             (((limit >> 16) & 0xf) << 48) | (((base >> 24) & 0xff) << 56);
    tss.ss0 = 0x10;
    tss.iomap = sizeof tss; /* no I/O bitmap: user in/out faults */
    struct {
        unsigned short limit;
        unsigned long base;
    } __attribute__((packed)) ptr = {sizeof gdt - 1, (unsigned long)gdt};
    __asm__ volatile(
        "lgdt %0\n\t"
        "ljmp $0x08, $1f\n"
        "1:\n\t"
        "mov $0x10, %%ax\n\t"
        "mov %%ax, %%ss\n\t"
        /* ds/es/fs/gs = user data (DPL 3): usable in ring 0 and valid after
         * iret to ring 3, so traps never have to reload them. */
        "mov $0x1b, %%ax\n\t"
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov $0x28, %%ax\n\t"
        "ltr %%ax\n\t"
        :
        : "m"(ptr)
        : "eax", "memory");
}
#endif

void user_init(void) {
    kernel_cr3 = read_cr3();
#ifdef __x86_64__
    unsigned int a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000001u));
    if (d & (1u << 20)) {
        unsigned int lo, hi;
        __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xc0000080u));
        lo |= 1u << 11; /* EFER.NXE */
        __asm__ volatile("wrmsr" ::"a"(lo), "d"(hi), "c"(0xc0000080u));
        nx_ok = 1;
    }
#endif
    /* SSE for programs and viewers: CR0.EM off, MP on; CR4.OSFXSR + OSXMMEXCPT. */
    unsigned long cr;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr));
    cr = (cr & ~(1ul << 2)) | (1ul << 1);
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr));
    cr |= (1ul << 9) | (1ul << 10);
    __asm__ volatile("mov %0, %%cr4" ::"r"(cr));
    gdt_init();
    void *h[20] = {isr_exc0, isr_exc1, 0, isr_exc3, isr_exc4, isr_exc5, isr_exc6, isr_exc7,
                   isr_exc8, 0, isr_exc10, isr_exc11, isr_exc12, isr_exc13, isr_exc14, 0,
                   isr_exc16, isr_exc17, 0, isr_exc19};
    for (int i = 0; i < 20; i++) {
        if (h[i] != 0) {
            idt_gate(i, h[i], 0x8e, i == 8 ? 1 : 0);
        }
    }
    idt_gate(0x80, isr_syscall, 0xee, 0);
}
