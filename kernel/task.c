/* Kernel tasks: fixed slots, own stacks, round-robin on the PIT IRQ.
 * Task 0 is the boot thread (shell loop). Everything runs in ring 0. */
#include "fs_limits.h"

void console_puts(const char *text);
void console_putc(char ch);

typedef void (*print_fn)(const char *);
typedef void (*sleep_fn)(unsigned int ticks);
void *pkg_sym(const char *lib, const char *sym);
typedef void *(*pkg_sym_fn)(const char *lib, const char *sym);
typedef void (*task_fn)(print_fn print, sleep_fn sleep, pkg_sym_fn sym);

enum { TASK_MAX = 4, TASK_STACK = 16384, FRAME_REGS = 15 };
enum { TASK_FREE = 0, TASK_READY, TASK_DEAD };

struct task {
    unsigned long rsp;
    int state;
    task_fn entry;
    unsigned int ticks;
    char name[FS_NAME];
};

static struct task tasks[TASK_MAX] = {[0] = {.state = TASK_READY, .name = "shell"}};
static unsigned char stacks[TASK_MAX][TASK_STACK] __attribute__((aligned(16)));
static volatile int current;
static volatile unsigned int jiffies;

static void irq_off(unsigned long *flags) {
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(*flags)::"memory");
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

static __attribute__((noreturn)) void task_start(void) {
    int self = current;
    tasks[self].entry(console_puts, task_sleep, pkg_sym);
    console_puts("\ntask ");
    print_u((unsigned int)self);
    console_puts(" done\n");
    unsigned long flags;
    irq_off(&flags);
    tasks[self].state = TASK_DEAD;
    __asm__ volatile("sti" ::: "memory");
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* Returns the task id, or -1 when every slot is busy. */
int task_spawn(const char *name, void *code) {
    task_fn entry = (task_fn)code;
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
    unsigned short cs;
    unsigned short ss;
    __asm__ volatile("mov %%cs, %0" : "=r"(cs));
    __asm__ volatile("mov %%ss, %0" : "=r"(ss));
    unsigned long *top = (unsigned long *)(stacks[id] + TASK_STACK);
    /* iretq frame; the entry sees rsp % 16 == 8, as after a call. */
    *--top = ss;
    *--top = (unsigned long)(stacks[id] + TASK_STACK - 8);
    *--top = 0x202;
    *--top = cs;
    *--top = (unsigned long)task_start;
    for (int i = 0; i < FRAME_REGS; i++) {
        *--top = 0;
    }
    struct task *task = &tasks[id];
    unsigned int n = 0;
    while (name[n] != 0 && n + 1 < FS_NAME) {
        task->name[n] = name[n];
        n++;
    }
    task->name[n] = 0;
    task->entry = entry;
    task->ticks = 0;
    task->rsp = (unsigned long)top;
    task->state = TASK_READY;
    irq_restore(flags);
    return id;
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
