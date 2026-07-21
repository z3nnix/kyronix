#include "syscall_setup.h"
#include "cpu.h"
#include "gdt.h"
#include "lib/log.h"
#include "percpu.h"

#define MSR_EFER 0xC0000080
#define MSR_STAR 0xC0000081
#define MSR_LSTAR 0xC0000082
#define MSR_SFMASK 0xC0000084
#define MSR_KERNEL_GS_BASE 0xC0000102
#define MSR_GS_BASE 0xC0000101

cpu_local_t g_cpu_local[MAX_CPUS] __attribute__((aligned(64)));

#define KERNEL_STACK_PAGES 4
static uint8_t g_syscall_stack[KERNEL_STACK_PAGES * 4096] __attribute__((aligned(16)));

extern void syscall_entry(void);

void cpu_enable_sse(void) {
    uint64_t cr0 = read_cr0();
    cr0 = (cr0 & ~(1ULL << 2)) | (1ULL << 1);
    write_cr0(cr0);
    write_cr4(read_cr4() | (1ULL << 9) | (1ULL << 10));
    fpu_init();
}

static void cpu_local_init(uint32_t cpu_id, uint64_t rsp0) {
    g_cpu_local[cpu_id].kernel_rsp = rsp0;
    g_cpu_local[cpu_id].user_rsp = 0;
    g_cpu_local[cpu_id].cpu_id = cpu_id;
    g_cpu_local[cpu_id].lapic_id = 0;
    g_cpu_local[cpu_id].online = 0;
}

void syscall_init(void) {
    cpu_enable_sse();

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1ULL);
    wrmsr(MSR_STAR,
          ((uint64_t) (GDT_USER_DATA_SEL - 0x8) << 48) | ((uint64_t) GDT_KERNEL_CODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t) syscall_entry);
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10) | (1ULL << 18));

    uint64_t rsp0 = (uint64_t) (g_syscall_stack + sizeof(g_syscall_stack));
    cpu_local_init(0, rsp0);

    wrmsr(MSR_GS_BASE, (uint64_t) &g_cpu_local[0]);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t) &g_cpu_local[0]);
    gdt_set_kernel_stack(rsp0);

    log_info("SYSCALL: LSTAR=0x%016lx  stack=0x%016lx cpu[0]", (uint64_t) syscall_entry, rsp0);
}

void cpu_set_kernel_stack(uint64_t rsp) {
    __asm__ volatile("movq %0, %%gs:0" ::"r"(rsp) : "memory");
    gdt_set_kernel_stack(rsp);
}
