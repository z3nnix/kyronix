#pragma once
#include "atomic.h"
#include "cpu.h"
#include "percpu.h"

static inline void spin_lock(spinlock_t *s) {
    for (;;) {
        if (__sync_bool_compare_and_swap(&s->lock, 0, 1)) break;
        while (__atomic_load_n(&s->lock, __ATOMIC_RELAXED)) cpu_relax();
    }
}

static inline void spin_unlock(spinlock_t *s) { __atomic_store_n(&s->lock, 0, __ATOMIC_RELEASE); }

static inline int spin_trylock(spinlock_t *s) {
    return __sync_bool_compare_and_swap(&s->lock, 0, 1);
}

typedef struct {
    spinlock_t lock;
    uint64_t flags;
} spinlock_irqsave_t;

static inline void spin_lock_irqsave(spinlock_irqsave_t *s) {
    s->flags = irq_save();
    spin_lock(&s->lock);
}

static inline void spin_unlock_irqrestore(spinlock_irqsave_t *s) {
    spin_unlock(&s->lock);
    irq_restore(s->flags);
}

typedef struct {
    volatile uint64_t lock;
    uint32_t owner;
    int depth;
} bkl_t;

extern bkl_t g_kernel_lock;

static inline void kernel_lock(void) {
    uint32_t cpu = this_cpu_id();
    if (__atomic_load_n(&g_kernel_lock.owner, __ATOMIC_RELAXED) == cpu) {
        __atomic_fetch_add(&g_kernel_lock.depth, 1, __ATOMIC_RELAXED);
        return;
    }
    for (;;) {
        if (__sync_bool_compare_and_swap(&g_kernel_lock.lock, 0, 1)) {
            __atomic_store_n(&g_kernel_lock.owner, cpu, __ATOMIC_RELAXED);
            return;
        }
        while (__atomic_load_n(&g_kernel_lock.lock, __ATOMIC_RELAXED)) cpu_relax();
    }
}

static inline void kernel_unlock(void) {
    if (__atomic_load_n(&g_kernel_lock.depth, __ATOMIC_RELAXED) > 0) {
        __atomic_fetch_sub(&g_kernel_lock.depth, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&g_kernel_lock.owner, ~0u, __ATOMIC_RELAXED);
    __atomic_store_n(&g_kernel_lock.lock, 0, __ATOMIC_RELEASE);
}
