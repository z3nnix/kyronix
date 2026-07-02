#include "proc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/syscall_setup.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "syscall/syscall.h"

_Static_assert(__builtin_offsetof(proc_t, space) == 16, "sched.S PROC_SPACE");
_Static_assert(__builtin_offsetof(proc_t, kstack_top) == 32, "sched.S PROC_KSTACK_TOP");
_Static_assert(__builtin_offsetof(proc_t, kstack_rsp) == 40, "sched.S PROC_KSTACK_RSP");
_Static_assert(__builtin_offsetof(proc_t, user_rsp) == 48, "sched.S PROC_USER_RSP");
_Static_assert(__builtin_offsetof(proc_t, fs_base) == 88, "sched.S PROC_FS_BASE");
_Static_assert(__builtin_offsetof(proc_t, fpu_state) == 3312, "sched.S PROC_FPU_STATE");

#define KSTACK_VA_BASE 0xffff920000000000ULL
#define KSTACK_VA_STRIDE ((KSTACK_PAGES + 1) * PAGE_SIZE)

static uint64_t g_kstack_va_bump = KSTACK_VA_BASE;

proc_t g_proctable[PROC_MAX];
proc_t *g_current_proc = NULL;

void proc_init(void) {
    memset(g_proctable, 0, sizeof(g_proctable));
    log_info("PROC: table initialised  (%d slots)", PROC_MAX);
}

proc_t *proc_alloc(uint32_t ppid) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (g_proctable[i].state != PROC_UNUSED) continue;

        proc_t *p = &g_proctable[i];
        memset(p, 0, sizeof(*p));

        p->state = PROC_READY;
        p->pid = (uint32_t) (i + 1);
        p->ppid = ppid;
        p->pgid = (uint32_t) (i + 1);
        p->wait_for = 0;

        uint64_t guard_va = g_kstack_va_bump;
        g_kstack_va_bump += KSTACK_VA_STRIDE;
        for (int pg = 0; pg < KSTACK_PAGES; pg++) {
            void *phys = pmm_alloc_zeroed();
            if (!phys) {
                /* free already-mapped pages */
                for (int j = 0; j < pg; j++) {
                    uint64_t va = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
                    vmm_unmap(&g_kernel_space, va);
                    if (pa) pmm_free((void *) pa);
                }
                p->state = PROC_UNUSED;
                return NULL;
            }
            uint64_t va = guard_va + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
            if (vmm_map(&g_kernel_space, va, (uint64_t) phys, VMM_KDATA) < 0) {
                pmm_free(phys);
                for (int j = 0; j < pg; j++) {
                    uint64_t jva = guard_va + PAGE_SIZE + (uint64_t) j * PAGE_SIZE;
                    uint64_t pa = vmm_virt_to_phys(&g_kernel_space, jva);
                    vmm_unmap(&g_kernel_space, jva);
                    if (pa) pmm_free((void *) pa);
                }
                p->state = PROC_UNUSED;
                return NULL;
            }
        }
        p->kstack_guard = guard_va;
        p->kstack = (uint8_t *) (guard_va + PAGE_SIZE);
        p->kstack_top = guard_va + KSTACK_VA_STRIDE;

        p->fds = (vfs_file_t **) kcalloc(VFS_FD_MAX, sizeof(vfs_file_t *));
        if (!p->fds) {
            proc_kstack_free(p);
            p->state = PROC_UNUSED;
            return NULL;
        }
        p->fds_refcnt = (uint32_t *) kmalloc(sizeof(uint32_t));
        if (!p->fds_refcnt) {
            kfree(p->fds);
            p->fds = NULL;
            proc_kstack_free(p);
            p->state = PROC_UNUSED;
            return NULL;
        }
        *p->fds_refcnt = 1;

        p->mmap_bump = 0x0000500000000000ULL;
        p->umask = 0022;
        p->cwd[0] = '/';
        p->cwd[1] = '\0';

        /* default x87 fpu + sse state: clean area, mask all exceptions */
        memset(p->fpu_state, 0, sizeof(p->fpu_state));
        ((uint16_t *) p->fpu_state)[0] = 0x037F;        /* FCW */
        ((uint32_t *) (p->fpu_state + 24))[0] = 0x1F80; /* MXCSR */

        return p;
    }
    return NULL;
}
static proc_t *g_reap_thread;

void proc_reap_pending(void) {
    proc_t *p = g_reap_thread;
    if (!p || p == g_current_proc) return; /* never free the stack we are running on */
    g_reap_thread = NULL;
    proc_kstack_free(p);    /* idempotent: clears kstack_guard */
    p->state = PROC_UNUSED; /* slot reusable only now that its stack is gone */
}

void proc_defer_thread_reap(proc_t *p) {
    proc_reap_pending(); /* flush any previous one first (not on its stack) */
    g_reap_thread = p;
}

void proc_kstack_free(proc_t *p) {
    if (!p->kstack_guard) return;
    for (int pg = 0; pg < KSTACK_PAGES; pg++) {
        uint64_t va = p->kstack_guard + PAGE_SIZE + (uint64_t) pg * PAGE_SIZE;
        uint64_t pa = vmm_virt_to_phys(&g_kernel_space, va);
        vmm_unmap(&g_kernel_space, va);
        if (pa) pmm_free((void *) pa);
    }
    p->kstack_guard = 0;
    p->kstack = NULL;
}

proc_t *proc_find(uint32_t pid) {
    for (int i = 0; i < PROC_MAX; i++)
        if (g_proctable[i].state != PROC_UNUSED && g_proctable[i].pid == pid)
            return &g_proctable[i];
    return NULL;
}

proc_t *proc_next_ready(proc_t *skip) {
    for (int i = 0; i < PROC_MAX; i++) {
        if (&g_proctable[i] == skip) continue;
        if (g_proctable[i].state == PROC_READY) return &g_proctable[i];
    }
    return NULL;
}
// WTF IM G
proc_t *proc_idle_until_ready(proc_t *skip) {
    for (;;) {
        proc_t *nt = proc_next_ready(skip);
        if (nt) return nt;
        sti();
        hlt();
        cli();
    }
}

void sched_yield_blocking(void) {
    proc_t *p = g_current_proc;
    proc_t *next = proc_next_ready(p);
    if (!next) {
        sti();
        hlt();
        cli();
        return;
    }

    p->state = PROC_WAITING;
    next->state = PROC_RUNNING;
    vfs_set_fdtable(next->fds);
    g_current_space = next->space;
    cpu_set_kernel_stack(next->kstack_top);

    sched_switch(next);

    p->state = PROC_RUNNING;
    vfs_set_fdtable(p->fds);
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);
}

void proc_ptrace_stop(proc_t *p, int sig, int frame_kind, void *frame, uint64_t *rflags_slot) {
    p->ptrace_stop_sig = (uint8_t) sig;
    p->ptrace_frame_kind = (uint8_t) frame_kind;
    p->ptrace_frame = frame;
    p->ptrace_stopped = 1;
    p->state = PROC_WAITING;

    proc_t *tracer = proc_find(p->tracer_pid);
    if (tracer && tracer->state != PROC_UNUSED) {
        proc_send_signal(tracer, SIGCHLD);
        if (tracer->state == PROC_WAITING) tracer->state = PROC_READY;
    }

    while (p->ptrace_stopped) {
        proc_t *next = proc_next_ready(p);
        if (!next) {
            sti();
            hlt();
            cli();
            continue;
        }
        next->state = PROC_RUNNING;
        vfs_set_fdtable(next->fds);
        g_current_space = next->space;
        cpu_set_kernel_stack(next->kstack_top);
        sched_switch(next);
    }

    if (rflags_slot) {
        if (p->ptrace_step) {
            *rflags_slot |= 0x100ULL;
        } else {
            *rflags_slot &= ~0x100ULL;
        }
    }
    p->ptrace_step = 0;
    p->ptrace_frame_kind = 0;
    p->ptrace_frame = NULL;

    p->state = PROC_RUNNING;
    vfs_set_fdtable(p->fds);
    g_current_space = p->space;
    cpu_set_kernel_stack(p->kstack_top);
}
