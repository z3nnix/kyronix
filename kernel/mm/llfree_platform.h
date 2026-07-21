#pragma once

#include "arch/x86_64/cpu.h"
#include "lib/log.h"
#include "lib/printf.h"
#include "llfree_types.h"
#include <stdatomic.h>
#include <stdint.h>

#define ll_align(align) __attribute__((aligned(align)))
#define unlikely(x) __builtin_expect(!!(x), 0)

#define PRIuS "zu"
#define PRIxS "zx"

#ifdef NDEBUG
#define assert(x) ((void) (x))
#else
#define assert(x)                                                                                  \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            log_error("LLFREE ASSERT: %s:%d: %s", __FILE__, __LINE__, #x);                         \
            cpu_halt();                                                                            \
        }                                                                                          \
    } while (0)
#endif

#define llfree_warn(str, ...) log_warn("LLFree: " str, ##__VA_ARGS__)

#ifndef NO_LOG
#define llfree_info_start() log_info("LLFree: ")
#define llfree_info_cont(str, ...) klog_printf(KLOG_INFO, str, ##__VA_ARGS__)
#define llfree_info_end() klog_printf(KLOG_INFO, "\n")
#define llfree_info(str, ...) log_info("LLFree: " str, ##__VA_ARGS__)
#else
#define llfree_info_start() (void) 0
#define llfree_info_cont(str, ...) (void) 0
#define llfree_info_end() (void) 0
#define llfree_info(str, ...) (void) 0
#endif

#ifdef DEBUG
#define llfree_debug(str, ...) log_debug("LLFree: " str, ##__VA_ARGS__)
#else
#define llfree_debug(str, ...) (void) 0
#endif

static const int ATOM_LOAD_ORDER = memory_order_acquire;
static const int ATOM_UPDATE_ORDER = memory_order_acq_rel;
static const int ATOM_STORE_ORDER = memory_order_release;

#define atom_cmp_exchange(obj, expected, desired)                                                  \
    ({                                                                                             \
        llfree_debug("cmpxchg");                                                                   \
        atomic_compare_exchange_strong_explicit((obj), (expected), (desired), ATOM_UPDATE_ORDER,   \
                                                ATOM_LOAD_ORDER);                                  \
    })
#define atom_cmp_exchange_weak(obj, expected, desired)                                             \
    ({                                                                                             \
        llfree_debug("cmpxchg");                                                                   \
        atomic_compare_exchange_weak_explicit((obj), (expected), (desired), ATOM_UPDATE_ORDER,     \
                                              ATOM_LOAD_ORDER);                                    \
    })

#define atom_swap(obj, desired)                                                                    \
    ({                                                                                             \
        llfree_debug("swap");                                                                      \
        atomic_exchange_explicit(obj, desired, ATOM_UPDATE_ORDER);                                 \
    })

#define atom_load(obj)                                                                             \
    ({                                                                                             \
        llfree_debug("load");                                                                      \
        atomic_load_explicit(obj, ATOM_LOAD_ORDER);                                                \
    })
#define atom_store(obj, val)                                                                       \
    ({                                                                                             \
        llfree_debug("store");                                                                     \
        atomic_store_explicit(obj, val, ATOM_STORE_ORDER);                                         \
    })

#define atom_update(atom_ptr, old_val, fn, ...)                                                    \
    ({                                                                                             \
        llfree_debug("update");                                                                    \
        bool _ret = false;                                                                         \
        (old_val) = atomic_load_explicit(atom_ptr, ATOM_LOAD_ORDER);                               \
        while (true) {                                                                             \
            __typeof(old_val) value = (old_val);                                                   \
            if (!(fn) (&value, ##__VA_ARGS__)) break;                                              \
            if (atomic_compare_exchange_weak_explicit((atom_ptr), &(old_val), value,               \
                                                      ATOM_UPDATE_ORDER, ATOM_LOAD_ORDER)) {       \
                _ret = true;                                                                       \
                break;                                                                             \
            }                                                                                      \
        }                                                                                          \
        _ret;                                                                                      \
    })

#ifndef STD
#define STD 1
#endif
