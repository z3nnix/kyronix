#pragma once
#include <stdint.h>

typedef struct {
    volatile uint64_t v;
} atomic64_t;

#define ATOMIC64_INIT(v) ((atomic64_t) { .v = (v) })

static inline uint64_t atomic64_load(const atomic64_t *a) {
    __asm__ volatile("" ::: "memory");
    return a->v;
}

static inline void atomic64_store(atomic64_t *a, uint64_t v) {
    __asm__ volatile("" ::: "memory");
    a->v = v;
}

static inline uint64_t atomic64_xchg(atomic64_t *a, uint64_t v) {
    __asm__ volatile("xchgq %0, %1" : "+r"(v), "+m"(a->v)::"memory");
    return v;
}

static inline uint64_t atomic64_cmpxchg(atomic64_t *a, uint64_t old, uint64_t new) {
    __asm__ volatile("lock cmpxchgq %2, %1" : "+a"(old), "+m"(a->v) : "r"(new) : "memory");
    return old;
}

static inline uint64_t atomic64_fetch_add(atomic64_t *a, uint64_t v) {
    __asm__ volatile("lock xaddq %0, %1" : "+r"(v), "+m"(a->v)::"memory");
    return v;
}

static inline void atomic64_inc(atomic64_t *a) {
    __asm__ volatile("lock incq %0" : "+m"(a->v)::"memory");
}

static inline void atomic64_dec(atomic64_t *a) {
    __asm__ volatile("lock decq %0" : "+m"(a->v)::"memory");
}

typedef struct {
    volatile uint32_t v;
} atomic32_t;

#define ATOMIC32_INIT(v) ((atomic32_t) { .v = (v) })

static inline uint32_t atomic32_load(const atomic32_t *a) {
    __asm__ volatile("" ::: "memory");
    return a->v;
}

static inline void atomic32_store(atomic32_t *a, uint32_t v) {
    __asm__ volatile("" ::: "memory");
    a->v = v;
}

static inline uint32_t atomic32_xchg(atomic32_t *a, uint32_t v) {
    __asm__ volatile("xchgl %0, %1" : "+r"(v), "+m"(a->v)::"memory");
    return v;
}

static inline uint32_t atomic32_cmpxchg(atomic32_t *a, uint32_t old, uint32_t new) {
    __asm__ volatile("lock cmpxchgl %2, %1" : "+a"(old), "+m"(a->v) : "r"(new) : "memory");
    return old;
}

static inline void atomic32_inc(atomic32_t *a) {
    __asm__ volatile("lock incl %0" : "+m"(a->v)::"memory");
}

static inline void atomic32_dec(atomic32_t *a) {
    __asm__ volatile("lock decl %0" : "+m"(a->v)::"memory");
}

static inline void mb(void) { __asm__ volatile("mfence" ::: "memory"); }

static inline void rmb(void) { __asm__ volatile("lfence" ::: "memory"); }

static inline void wmb(void) { __asm__ volatile("sfence" ::: "memory"); }
