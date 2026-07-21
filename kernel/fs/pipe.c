#include "pipe.h"
#include "arch/x86_64/pit.h"
#include "lib/log.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include <stdbool.h>

#define PIPE_MAGIC 0x4b59504950454d47ULL
#define EPIPE 32
#define EAGAIN 11
#define EIO 5

pipe_t *pipe_alloc(void) {
    pipe_t *p = (pipe_t *) kcalloc(1, sizeof(pipe_t));
    if (p) {
        p->magic = PIPE_MAGIC;
        p->lock.lock = 0;
    }
    return p;
}

void pipe_free(pipe_t *p) {
    if (!p) return;
    spin_lock(&p->lock);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &g_proctable[i];
        if (t->state != PROC_WAITING || t->blocked_pipe != p) continue;
        t->blocked_pipe = NULL;
        t->wakeup_tick = 0;
        if (__sync_bool_compare_and_swap(&t->state, PROC_WAITING, PROC_READY)) proc_set_ready(t);
    }
    p->magic = 0;
    spin_unlock(&p->lock);
    kfree(p);
}

static bool pipe_valid(pipe_t *p) {
    uintptr_t addr = (uintptr_t) p;
    if (!p || addr < HEAP_START || addr >= HEAP_MAX || (addr & 7)) return false;
    return p->magic == PIPE_MAGIC;
}

/* wake every proc blocked on this pipe in one direction (want_read=1 -> readers) */
void pipe_wake(pipe_t *p, int want_read) {
    spin_lock(&p->lock);
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &g_proctable[i];
        if (t->state != PROC_WAITING || t->blocked_pipe != p) continue;
        if (t->blocked_pipe_read != want_read) continue;
        t->state = PROC_READY;
        proc_set_ready(t);
    }
    spin_unlock(&p->lock);
}

int64_t pipe_read(pipe_t *p, void *buf, uint64_t len) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            if (p->count == 0) {
                if (p->write_refs == 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }
                if (done > 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }

                proc_t *_rp = g_current_proc;
                if (_rp) {
                    _rp->wakeup_tick = g_ticks + 10;
                    _rp->blocked_pipe = p;
                    _rp->blocked_pipe_read = 1;
                    proc_set_timer(_rp);
                }
                p->waiting_reader = _rp;
                spin_unlock(&p->lock);
                sched_yield_blocking();
                p->waiting_reader = NULL;
                if (_rp) {
                    _rp->blocked_pipe = NULL;
                    _rp->wakeup_tick = 0;
                }
                goto restart_read;
            }
            out[done++] = p->buf[p->rpos];
            p->rpos = (p->rpos + 1) % PIPE_BUFSZ;
            p->count--;
        }
        spin_unlock(&p->lock);
        break;
    restart_read:;
    }

    pipe_wake(p, 0); /* space freed: wake all blocked writers */
    return (int64_t) done;
}

int64_t pipe_peek(pipe_t *p, void *buf, uint64_t len, uint64_t skip) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            if (p->count <= skip + done) {
                if (p->write_refs == 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }
                if (done > 0) {
                    spin_unlock(&p->lock);
                    return (int64_t) done;
                }

                proc_t *_rp = g_current_proc;
                if (_rp) {
                    _rp->wakeup_tick = g_ticks + 10;
                    _rp->blocked_pipe = p;
                    _rp->blocked_pipe_read = 1;
                    proc_set_timer(_rp);
                }
                p->waiting_reader = _rp;
                spin_unlock(&p->lock);
                sched_yield_blocking();
                p->waiting_reader = NULL;
                if (_rp) {
                    _rp->blocked_pipe = NULL;
                    _rp->wakeup_tick = 0;
                }
                goto restart_peek;
            }

            uint32_t pos = (p->rpos + skip + done) % PIPE_BUFSZ;
            out[done++] = p->buf[pos];
        }
        spin_unlock(&p->lock);
        break;
    restart_peek:;
    }

    return (int64_t) done;
}

int64_t pipe_write(pipe_t *p, const void *buf, uint64_t len) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    if (p->read_refs == 0) {
        proc_send_signal(g_current_proc, SIGPIPE);
        return -(int64_t) EPIPE;
    }
    if (len == 0) return 0;

    const uint8_t *in = (const uint8_t *) buf;
    uint64_t done = 0;

    for (;;) {
        spin_lock(&p->lock);
        while (done < len) {
            while (p->count == PIPE_BUFSZ) {
                if (p->read_refs == 0) {
                    spin_unlock(&p->lock);
                    proc_send_signal(g_current_proc, SIGPIPE);
                    return done ? (int64_t) done : -(int64_t) EPIPE;
                }

                proc_t *_wp = g_current_proc;
                if (_wp) {
                    _wp->blocked_pipe = p;
                    _wp->blocked_pipe_read = 0;
                }
                p->waiting_writer = _wp;
                spin_unlock(&p->lock);
                pipe_wake(p, 1); /* let readers drain so space frees up */
                sched_yield_blocking();
                p->waiting_writer = NULL;
                if (_wp) _wp->blocked_pipe = NULL;
                goto restart_write;
            }
            uint32_t wpos = (p->rpos + p->count) % PIPE_BUFSZ;
            p->buf[wpos] = in[done++];
            p->count++;
        }
        spin_unlock(&p->lock);
        break;
    restart_write:;
    }

    pipe_wake(p, 1); /* data available: wake all blocked readers */
    return (int64_t) done;
}

int pipe_anc_send(pipe_t *p, void **files, int nfds) {
    if (!pipe_valid(p) || !files || nfds <= 0 || nfds > PIPE_ANC_MAXFDS) return -1;
    spin_lock(&p->lock);
    uint32_t next = (p->anc_wr + 1) % PIPE_ANC_SLOTS;
    if (next == p->anc_rd) {
        spin_unlock(&p->lock);
        return -1;
    }
    pipe_anc_t *slot = &p->anc_q[p->anc_wr];
    slot->nfds = nfds;
    for (int i = 0; i < nfds; i++) slot->files[i] = files[i];
    p->anc_wr = next;
    spin_unlock(&p->lock);
    return 0;
}

int pipe_anc_recv(pipe_t *p, void **out, int max) {
    if (!pipe_valid(p) || !out || max <= 0) return 0;
    spin_lock(&p->lock);
    if (p->anc_rd == p->anc_wr) {
        spin_unlock(&p->lock);
        return 0;
    }
    pipe_anc_t *slot = &p->anc_q[p->anc_rd];
    int n = slot->nfds < max ? slot->nfds : max;
    for (int i = 0; i < n; i++) out[i] = slot->files[i];
    p->anc_rd = (p->anc_rd + 1) % PIPE_ANC_SLOTS;
    spin_unlock(&p->lock);
    return n;
}
