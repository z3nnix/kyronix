#pragma once
#include "fs/vfs.h"
#include "mm/vmm.h"
#include "proc/signal.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROC_UNUSED 0
#define PROC_RUNNING 1
#define PROC_READY 2
#define PROC_WAITING 3
#define PROC_ZOMBIE 4
#define PROC_DYING 5

#define PROC_MAX 64
#define KSTACK_PAGES 16
#define KSTACK_SIZE (KSTACK_PAGES * 4096ULL)

typedef struct proc {
    int state;
    int pgid;
    uint32_t pid;
    uint32_t ppid;
    vmm_space_t *space;
    uint8_t *kstack;
    uint64_t kstack_top;
    uint64_t kstack_rsp;
    uint64_t user_rsp;
    int exit_code;
    int wait_for;
    uint64_t brk;
    uint64_t brk_base;
    uint64_t mmap_bump;
    uint64_t fs_base;
    vfs_file_t **fds;
    uint32_t *fds_refcnt;
    uint64_t pending_sigs;
    uint64_t sig_mask;
    k_sigaction_t sig_actions[NSIG];
    char cwd[512];
    uint64_t wakeup_tick;
    uint64_t alarm_tick;
    char exe_path[512];
    uint32_t *cleartid_addr;
    uint8_t is_thread;
    uint32_t uid, gid;
    uint32_t euid, egid;
    uint32_t suid, sgid;
    uint32_t fsuid, fsgid;
    uint32_t umask;
    uint64_t kstack_guard;
    uint64_t itimer_value_ms;
    uint64_t itimer_interval_ms;
    uint64_t itimer_next_tick;
    void *blocked_pipe;
    int blocked_pipe_read;
    uint8_t fpu_state[512] __attribute__((aligned(16)));
    uint64_t sig_altstack_sp;
    uint64_t sig_altstack_size;
    uint8_t on_sigstack;
    uint64_t pages_alloc;
    uint64_t pages_freed;
    uint32_t jail_id;
    uint8_t jail_exempt;

    uint32_t tracer_pid;
    uint8_t ptrace_stopped;
    uint8_t ptrace_reported;
    uint8_t ptrace_stop_sig;
    uint8_t ptrace_syscall_trace;
    uint8_t ptrace_in_syscall;
    uint8_t ptrace_step;
    uint8_t ptrace_frame_kind;
    void *ptrace_frame;
} proc_t;

extern proc_t g_proctable[PROC_MAX] __attribute__((aligned(16)));

#include "arch/x86_64/percpu.h"
static inline proc_t **__g_current_proc_slot(void) {
    uint32_t cpu_id;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(cpu_id));
    return (proc_t **) &g_cpu_local[cpu_id].current;
}
#define g_current_proc (*__g_current_proc_slot())

void proc_init(void);
proc_t *proc_alloc(uint32_t ppid);
proc_t *proc_create_idle(uint32_t cpu_id, void (*entry)(void));
void proc_kstack_free(proc_t *p);
void proc_defer_thread_reap(proc_t *p);
void proc_ptrace_stop(proc_t *p, int sig, int frame_kind, void *frame, uint64_t *rflags_slot);
void proc_reap_pending(void);
proc_t *proc_find(uint32_t pid);
proc_t *proc_next_ready(proc_t *skip);
proc_t *sched_claim_next(proc_t *skip);
proc_t *proc_idle_until_ready(proc_t *skip);
void sched_switch(proc_t *next);
void sched_yield_blocking(void);
extern void proc_resume_frame(void);
__attribute__((noreturn)) void proc_do_exit(int code);
