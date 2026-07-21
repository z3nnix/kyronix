#pragma once
#include "arch/x86_64/atomic.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/spinlock.h"
#include "boot/limine.h"
#include <stdint.h>

extern volatile uint32_t g_cpu_count;
extern volatile uint32_t g_aps_ready;
extern volatile uint32_t g_kernel_ready;

void smp_init(void);
void smp_boot_aps(void);

__attribute__((noreturn)) void ap_sched_loop(void);
