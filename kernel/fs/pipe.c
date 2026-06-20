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
    if (p) p->magic = PIPE_MAGIC;
    return p;
}

void pipe_free(pipe_t *p) {
    if (p) p->magic = 0;
    kfree(p);
}

static bool pipe_valid(pipe_t *p) {
    uintptr_t addr = (uintptr_t) p;
    if (!p || addr < HEAP_START || addr >= HEAP_MAX || (addr & 7)) return false;
    return p->magic == PIPE_MAGIC;
}

/* wake every proc blocked on this pipe in one direction (want_read=1 -> readers) */
void pipe_wake(pipe_t *p, int want_read) {
    for (int i = 0; i < PROC_MAX; i++) {
        proc_t *t = &g_proctable[i];
        if (t->state != PROC_WAITING || t->blocked_pipe != p) continue;
        if (t->blocked_pipe_read != want_read) continue;
        t->state = PROC_READY;
    }
}

int64_t pipe_read(pipe_t *p, void *buf, uint64_t len) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    while (done < len) {
        if (p->count == 0) {
            if (p->write_refs == 0) break;
            if (done > 0)
                break; /* return data already available
                            dont wait to fill buf */
            proc_t *_rp = g_current_proc;
            if (_rp) {
                _rp->wakeup_tick = g_ticks + 10;
                _rp->blocked_pipe = p;
                _rp->blocked_pipe_read = 1;
            }
            p->waiting_reader = _rp;
            sched_yield_blocking();
            p->waiting_reader = NULL;
            if (_rp) {
                _rp->blocked_pipe = NULL;
                _rp->wakeup_tick = 0;
            }
            continue;
        }
        out[done++] = p->buf[p->rpos];
        p->rpos = (p->rpos + 1) % PIPE_BUFSZ;
        p->count--;
    }
    pipe_wake(p, 0); /* space freed: wake all blocked writers */

    return (int64_t) done;
}

int64_t pipe_peek(pipe_t *p, void *buf, uint64_t len, uint64_t skip) {
    if (!pipe_valid(p)) return -(int64_t) EIO;
    uint8_t *out = (uint8_t *) buf;
    uint64_t done = 0;

    while (done < len) {
        if (p->count <= skip + done) {
            if (p->write_refs == 0) break;
            if (done > 0) break;
            proc_t *_rp = g_current_proc;
            if (_rp) {
                _rp->wakeup_tick = g_ticks + 10;
                _rp->blocked_pipe = p;
                _rp->blocked_pipe_read = 1;
            }
            p->waiting_reader = _rp;
            sched_yield_blocking();
            p->waiting_reader = NULL;
            if (_rp) {
                _rp->blocked_pipe = NULL;
                _rp->wakeup_tick = 0;
            }
            continue;
        }

        uint32_t pos = (p->rpos + skip + done) % PIPE_BUFSZ;
        out[done++] = p->buf[pos];
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

    while (done < len) {
        while (p->count == PIPE_BUFSZ) {
            if (p->read_refs == 0) {
                proc_send_signal(g_current_proc, SIGPIPE);
                return done ? (int64_t) done : -(int64_t) EPIPE;
            }
            proc_t *_wp = g_current_proc;
            if (_wp) {
                _wp->blocked_pipe = p;
                _wp->blocked_pipe_read = 0;
            }
            p->waiting_writer = _wp;
            pipe_wake(p, 1); /* let readers drain so space frees up */
            sched_yield_blocking();
            p->waiting_writer = NULL;
            if (_wp) _wp->blocked_pipe = NULL;
        }
        uint32_t wpos = (p->rpos + p->count) % PIPE_BUFSZ;
        p->buf[wpos] = in[done++];
        p->count++;
    }

    pipe_wake(p, 1); /* data available: wake all blocked readers */
    return (int64_t) done;
}

int pipe_anc_send(pipe_t *p, void **files, int nfds) {
    if (!pipe_valid(p) || !files || nfds <= 0 || nfds > PIPE_ANC_MAXFDS) return -1;
    uint32_t next = (p->anc_wr + 1) % PIPE_ANC_SLOTS;
    if (next == p->anc_rd) return -1;
    pipe_anc_t *slot = &p->anc_q[p->anc_wr];
    slot->nfds = nfds;
    for (int i = 0; i < nfds; i++) slot->files[i] = files[i];
    p->anc_wr = next;
    return 0;
}

int pipe_anc_recv(pipe_t *p, void **out, int max) {
    if (!pipe_valid(p) || !out || max <= 0) return 0;
    if (p->anc_rd == p->anc_wr) return 0;
    pipe_anc_t *slot = &p->anc_q[p->anc_rd];
    int n = slot->nfds < max ? slot->nfds : max;
    for (int i = 0; i < n; i++) out[i] = slot->files[i];
    p->anc_rd = (p->anc_rd + 1) % PIPE_ANC_SLOTS;
    return n;
}
