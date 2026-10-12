/* Tasks: fixed slots, own kernel stacks, round-robin on the PIT IRQ.
 * Task 0 is the boot thread (shell loop, ring 0). Programs from disk run in
 * ring 3, each in its own address space (see user.c). */
#ifdef __x86_64__
#define PUSHF_POP "pushfq; popq"
#else
#define PUSHF_POP "pushfl; popl"
#endif
#include "fs_limits.h"
#include "user.h"

void console_puts(const char *text);
void console_putc(char ch);


#ifdef __x86_64__
enum { TASK_MAX = 4, TASK_STACK = 16384, FRAME_REGS = 15 };
#else
enum { TASK_MAX = 4, TASK_STACK = 16384, FRAME_REGS = 8 }; /* pushad */
#endif
enum { TASK_FREE = 0, TASK_READY, TASK_DEAD };

struct task {
    unsigned long rsp;
    int state;
    unsigned int ticks;
    unsigned long cr3; /* user address space, 0 for kernel tasks */
    int quiet;         /* foreground program: no "task N done" line */
    char name[FS_NAME];
};

void task_crash_note(int id, const char *name, const char *why);
static void print_u(unsigned int value);

static struct task tasks[TASK_MAX] = {[0] = {.state = TASK_READY, .name = "shell"}};
static unsigned char stacks[TASK_MAX][TASK_STACK] __attribute__((aligned(16)));
/* FPU/SSE registers of each task, swapped on every task switch. */
static unsigned char fpu[TASK_MAX][512] __attribute__((aligned(16)));

static void fpu_reset(int id) {
    for (int i = 0; i < 512; i++) {
        fpu[id][i] = 0;
    }
    fpu[id][0] = 0x7f; /* FCW 0x037f */
    fpu[id][1] = 0x03;
    fpu[id][24] = 0x80; /* MXCSR 0x1f80 */
    fpu[id][25] = 0x1f;
}
static volatile int current;
static volatile unsigned int jiffies;

static void irq_off(unsigned long *flags) {
    __asm__ volatile(PUSHF_POP " %0; cli" : "=r"(*flags)::"memory");
}

static void irq_restore(unsigned long flags) {
    if ((flags & 0x200ul) != 0) {
        __asm__ volatile("sti" ::: "memory");
    }
}

/* Wait for n timer ticks (100 per second) without burning the CPU. */
void task_sleep(unsigned int n) {
    unsigned int start = jiffies;
    while (jiffies - start < n) {
        __asm__ volatile("hlt");
    }
}

int task_current(void) {
    return current;
}

/* Called from isr_irq0 with the saved frame; returns the frame to resume. */
unsigned long task_switch(unsigned long rsp) {
    jiffies++;
    tasks[current].rsp = rsp;
    tasks[current].ticks++;
    int prev = current;
    int next = current;
    for (int i = 1; i <= TASK_MAX; i++) {
        int cand = (current + i) % TASK_MAX;
        if (tasks[cand].state == TASK_READY) {
            next = cand;
            break;
        }
    }
    current = next;
    if (next != prev && tasks[prev].state == TASK_DEAD) {
        tasks[prev].state = TASK_FREE;
    }
    if (next != prev) {
#ifdef __x86_64__
        __asm__ volatile("fxsave64 %0" : "=m"(fpu[prev]));
        __asm__ volatile("fxrstor64 %0" ::"m"(fpu[next]));
#else
        __asm__ volatile("fxsave %0" : "=m"(fpu[prev]));
        __asm__ volatile("fxrstor %0" ::"m"(fpu[next]));
#endif
        unsigned long want = tasks[next].cr3 != 0 ? tasks[next].cr3 : user_kernel_cr3();
        unsigned long have;
        __asm__ volatile("mov %%cr3, %0" : "=r"(have));
        if (want != have) {
            __asm__ volatile("mov %0, %%cr3" ::"r"(want) : "memory");
        }
        user_set_kstack((unsigned long)(stacks[next] + TASK_STACK));
    }
    return tasks[next].rsp;
}

static void print_u(unsigned int value) {
    char digits[10];
    int count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value > 0);
    while (count > 0) {
        console_putc(digits[--count]);
    }
}

/* Release the address space of task ID (interrupts off, not running on it). */
static void task_release(int id) {
    if (tasks[id].cr3 != 0) {
        user_libs_forget(tasks[id].cr3);
        user_space_free(tasks[id].cr3);
        tasks[id].cr3 = 0;
    }
}

/* Start a program image in ring 3. Returns the task id, -1 when every slot is
 * busy, -2 when there is no memory for its address space. */
int task_spawn_user(const char *name, const unsigned char *image, unsigned int len,
                    const char *args, int quiet) {
    unsigned long flags;
    irq_off(&flags);
    int id = -1;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_FREE) {
            id = i;
            break;
        }
    }
    if (id < 0) {
        irq_restore(flags);
        return -1;
    }
    fpu_reset(id);
    struct user_start st;
    unsigned long cr3 = user_space_new(image, len, args, &st);
    if (cr3 == 0) {
        irq_restore(flags);
        return -2;
    }
    unsigned long *top = (unsigned long *)(stacks[id] + TASK_STACK);
    *--top = USER_SS;
    *--top = st.rsp;
    *--top = 0x202;
    *--top = USER_CS;
    *--top = st.rip;
#ifdef __x86_64__
    /* r15 .. rax as isr_irq0 pops them; rdi/rsi/rdx/rcx carry the arguments. */
    unsigned long regs[FRAME_REGS] = {0};
    regs[9] = st.print;  /* rdi */
    regs[10] = st.sleep; /* rsi */
    regs[11] = st.sym;   /* rdx */
    regs[12] = st.args;  /* rcx */
#else
    /* i686: the popad frame of irq32.asm; arguments are on the user stack. */
    unsigned long regs[FRAME_REGS] = {0};
#endif
    for (int i = FRAME_REGS - 1; i >= 0; i--) {
        *--top = regs[i];
    }
    struct task *task = &tasks[id];
    unsigned int n = 0;
    while (name[n] != 0 && n + 1 < FS_NAME) {
        task->name[n] = name[n];
        n++;
    }
    task->name[n] = 0;
    task->ticks = 0;
    task->cr3 = cr3;
    task->quiet = quiet;
    task->rsp = (unsigned long)top;
    task->state = TASK_READY;
    irq_restore(flags);
    return id;
}

const char *task_current_name(void) {
    return tasks[current].name;
}

/* 1 while task ID still runs. */
int task_alive(int id) {
    return id > 0 && id < TASK_MAX && tasks[id].state == TASK_READY;
}

/* The current user task ends: normally (WHY == 0) or because of a fault.
 * Runs on the task's kernel stack inside a trap; never returns. */
void task_user_died(const char *why) {
    __asm__ volatile("cli" ::: "memory");
    int self = current;
    __asm__ volatile("mov %0, %%cr3" ::"r"(user_kernel_cr3()) : "memory");
    task_release(self);
    if (why != 0) {
        task_crash_note(self, tasks[self].name, why);
    } else if (!tasks[self].quiet) {
        console_puts("\ntask ");
        print_u((unsigned int)self);
        console_puts(" done\n");
    }
    __asm__ volatile("cli" ::: "memory");
    tasks[self].state = TASK_DEAD;
    __asm__ volatile("sti" ::: "memory");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

void cmd_ps(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_READY) {
            continue;
        }
        console_puts("ps: ");
        print_u((unsigned int)i);
        console_putc(' ');
        console_puts(tasks[i].name);
        console_putc(' ');
        print_u(tasks[i].ticks);
        console_putc('\n');
    }
}

/* Background tasks (everything but the shell) that are running now. */
int task_count(void) {
    int n = 0;
    for (int i = 1; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_READY) {
            n++;
        }
    }
    return n;
}

/* Name and ticks of task I if it runs; 0 when the slot is empty. */
int task_info(int i, char *name, unsigned int cap, unsigned int *ticks) {
    if (i < 0 || i >= TASK_MAX || tasks[i].state != TASK_READY) {
        return 0;
    }
    unsigned int n = 0;
    while (tasks[i].name[n] != 0 && n + 1 < cap) {
        name[n] = tasks[i].name[n];
        n++;
    }
    name[n] = 0;
    *ticks = tasks[i].ticks;
    return 1;
}

int task_max(void) {
    return TASK_MAX;
}

/* Stop background task ID. 1 on success, 0 if there is no such task,
 * -1 for the shell (task 0), which cannot be stopped. */
int task_kill(int id) {
    if (id == 0) {
        return -1;
    }
    if (id < 0 || id >= TASK_MAX) {
        return 0;
    }
    unsigned long flags;
    irq_off(&flags);
    int ok = tasks[id].state == TASK_READY;
    if (ok) {
        /* Never the running task: kill comes from the shell (task 0). */
        tasks[id].state = id == current ? TASK_DEAD : TASK_FREE;
        if (id != current) {
            task_release(id);
        }
    }
    irq_restore(flags);
    return ok;
}
