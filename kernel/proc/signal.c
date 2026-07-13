#include "signal.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/syscall_setup.h"
#include "drivers/tty.h"
#include "lib/log.h"
#include "lib/string.h"
#include "proc.h"
#include "syscall/syscall.h"

_Static_assert(sizeof(mcontext_t) == 256, "mcontext_t must be 256 bytes");
_Static_assert(sizeof(siginfo_t) == 128, "siginfo_t must be 128 bytes");
_Static_assert(sizeof(ucontext_t) == 304, "ucontext_t must be 304 bytes");
_Static_assert(sizeof(rt_sigframe_t) == 440, "rt_sigframe_t must be 440 bytes");

static int sig_default_fatal(int sig) {
    switch (sig) {
    case SIGCHLD:
    case SIGCONT:
    case SIGWINCH:
        return 0;
    default:
        return 1;
    }
}

void proc_send_signal(struct proc *p, int sig) {
    if (sig < 1 || sig >= NSIG) return;
    proc_t *pp = (proc_t *) p;
    if (!pp || pp->state == PROC_UNUSED) return;

    if (pp->job_stopped && (sig == SIGCONT || sig == SIGKILL)) {
        pp->job_stopped = 0;
        pp->state = PROC_READY;
        proc_set_ready(pp);
    }

    __atomic_fetch_or(&pp->pending_sigs, (1ULL << (sig - 1)), __ATOMIC_RELAXED);

    if (__sync_bool_compare_and_swap(&pp->state, PROC_WAITING, PROC_READY)) {
        proc_set_ready(pp);
    }
}

static void setup_sigframe(proc_t *p, int sig, syscall_frame_t *f) {
    uint64_t user_rsp = cpu_get_user_rsp();

    bool use_alt =
        (p->sig_actions[sig - 1].sa_flags & SA_ONSTACK) && p->sig_altstack_size && !p->on_sigstack;
    uint64_t base = use_alt ? p->sig_altstack_sp + p->sig_altstack_size : user_rsp - 128;

    uint64_t sp = base - sizeof(rt_sigframe_t);
    sp = ((sp - 8) & ~(uint64_t) 0xF) + 8;

    if (!uptr_ok_w((void *) sp, sizeof(rt_sigframe_t))) proc_do_exit(-SIGSEGV);

    rt_sigframe_t *frame = (rt_sigframe_t *) sp;
    memset(frame, 0, sizeof(*frame));

    frame->pretcode = p->sig_actions[sig - 1].sa_restorer;
    frame->info.si_signo = sig;

    mcontext_t *mc = &frame->uc.uc_mcontext;
    mc->r8 = f->r8;
    mc->r9 = f->r9;
    mc->r10 = f->r10;
    mc->r11 = f->r11;
    mc->r12 = f->r12;
    mc->r13 = f->r13;
    mc->r14 = f->r14;
    mc->r15 = f->r15;
    mc->rdi = f->rdi;
    mc->rsi = f->rsi;
    mc->rbp = f->rbp;
    mc->rbx = f->rbx;
    mc->rdx = f->rdx;
    mc->rax = f->rax;
    mc->rcx = f->rcx;
    mc->rsp = user_rsp;
    mc->rip = f->rcx;
    mc->eflags = f->r11;
    mc->cs = GDT_USER_CODE_SEL;
    mc->fpstate = 0;

    frame->uc.uc_sigmask = p->sig_mask;

    p->sig_mask |= (1ULL << (sig - 1)) | p->sig_actions[sig - 1].sa_mask;
    p->sig_mask &= ~((1ULL << (SIGKILL - 1)) | (1ULL << (SIGSTOP - 1)));

    f->rcx = p->sig_actions[sig - 1].sa_handler;
    f->rdi = (uint64_t) sig;
    f->rsi = (uint64_t) &frame->info;
    f->rdx = (uint64_t) &frame->uc;
    f->r11 = 0x202ULL;

    cpu_set_user_rsp(sp);

    if (use_alt) p->on_sigstack = 1;

    if (p->sig_actions[sig - 1].sa_flags & SA_RESETHAND)
        p->sig_actions[sig - 1].sa_handler = SIG_DFL;
}

static void deliver_signal(proc_t *p, int sig, syscall_frame_t *f) {
    uint64_t handler = p->sig_actions[sig - 1].sa_handler;

    if (sig == SIGSTOP || (sig == SIGTSTP && handler == SIG_DFL)) {
        proc_job_stop(p, sig);
        return;
    }

    if (handler == SIG_IGN) return;

    if (handler == SIG_DFL) {
        if (!sig_default_fatal(sig)) return;
        proc_do_exit(-sig);
    }

    if (!p->sig_actions[sig - 1].sa_restorer) {
        log_warn("signal: sig %d has no restorer", sig);
        proc_do_exit(-sig);
    }

    setup_sigframe(p, sig, f);
}

void signal_check(syscall_frame_t *f) {
    proc_t *p = g_current_proc;
    if (!p) return;

    tty_check_signals();

    uint64_t pending = __atomic_load_n(&p->pending_sigs, __ATOMIC_RELAXED) & ~p->sig_mask;
    if (!pending) return;

    int idx = __builtin_ctzll(pending);
    int sig = idx + 1;

    __atomic_fetch_and(&p->pending_sigs, ~(1ULL << idx), __ATOMIC_RELAXED);

    if (p->tracer_pid && sig != SIGKILL) {
        proc_ptrace_stop(p, sig, 1, f, &f->r11);
        return;
    }

    deliver_signal(p, sig, f);
}
