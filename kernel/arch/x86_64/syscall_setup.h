#pragma once
#include "mm/vmm.h"
#include "percpu.h"
#include <stdint.h>

void syscall_init(void);
void cpu_enable_sse(void);
void cpu_set_kernel_stack(uint64_t rsp);

static inline uint64_t cpu_get_user_rsp(void) {
    uint64_t v;
    __asm__ volatile("movq %%gs:8, %0" : "=r"(v));
    return v;
}
static inline void cpu_set_user_rsp(uint64_t v) {
    __asm__ volatile("movq %0, %%gs:8" ::"r"(v) : "memory");
}