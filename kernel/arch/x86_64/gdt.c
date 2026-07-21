#include "gdt.h"
#include "percpu.h"
#include <stddef.h>

typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

typedef struct {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t base_mid;
    uint8_t type;
    uint8_t lim_hi_flags;
    uint8_t base_hi1;
    uint32_t base_hi2;
    uint32_t zero;
} __attribute__((packed)) tss_desc_t;

typedef struct {
    uint64_t null;
    uint64_t kernel_code;
    uint64_t kernel_data;
    uint64_t user_data;
    uint64_t user_code;
    tss_desc_t tss[MAX_CPUS];
} __attribute__((packed, aligned(16))) gdt_t;

static gdt_t g_gdt;

static tss_t g_tss_bsp __attribute__((aligned(16)));
static tss_t g_tss_ap[MAX_CPUS - 1] __attribute__((aligned(16)));

#define IST_STACK_SIZE 16384u
static uint8_t g_ist_df[IST_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t g_ist_nmi[IST_STACK_SIZE] __attribute__((aligned(16)));

#define GDT_TSS_BASE 0x28
#define GDT_TSS_SEL(n) (GDT_TSS_BASE + (n) * 0x10)

static inline tss_t *tss_for_cpu(uint32_t cpu_id) {
    return (cpu_id == 0) ? &g_tss_bsp : &g_tss_ap[cpu_id - 1];
}

static void tss_init(tss_t *tss) {
    tss->iopb_offset = (uint16_t) sizeof(tss_t);
    tss->ist[0] = (uint64_t) (g_ist_df + IST_STACK_SIZE);
    tss->ist[1] = (uint64_t) (g_ist_nmi + IST_STACK_SIZE);
}

static void gdt_set_tss_desc(uint32_t cpu_id, uint64_t base) {
    uint32_t limit = (uint32_t) (sizeof(tss_t) - 1);
    g_gdt.tss[cpu_id] = (tss_desc_t) {
        .limit_lo = (uint16_t) (limit & 0xFFFF),
        .base_lo = (uint16_t) (base & 0xFFFF),
        .base_mid = (uint8_t) ((base >> 16) & 0xFF),
        .type = 0x89,
        .lim_hi_flags = (uint8_t) ((limit >> 16) & 0x0F),
        .base_hi1 = (uint8_t) ((base >> 24) & 0xFF),
        .base_hi2 = (uint32_t) (base >> 32),
        .zero = 0,
    };
}

static inline void gdt_load_and_set_segments(void) {
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) gdtr = {
        .limit = (uint16_t) (sizeof(g_gdt) - 1),
        .base = (uint64_t) &g_gdt,
    };

    __asm__ volatile("lgdt   %0                  \n\t"
                     "pushq  $0x08               \n\t"
                     "leaq   1f(%%rip), %%rax    \n\t"
                     "pushq  %%rax               \n\t"
                     "lretq                      \n\t"
                     "1:                         \n\t"
                     "movw   $0x10, %%ax         \n\t"
                     "movw   %%ax,  %%ds         \n\t"
                     "movw   %%ax,  %%es         \n\t"
                     "movw   %%ax,  %%ss         \n\t"
                     "xorw   %%ax,  %%ax         \n\t"
                     "movw   %%ax,  %%fs         \n\t"
                     "movw   %%ax,  %%gs         \n\t"
                     :
                     : "m"(gdtr)
                     : "rax", "memory");
}

void gdt_init(void) {
    g_gdt.null = 0ULL;
    g_gdt.kernel_code = 0x00AF9A000000FFFFULL;
    g_gdt.kernel_data = 0x00CF92000000FFFFULL;
    g_gdt.user_data = 0x00CFF2000000FFFFULL;
    g_gdt.user_code = 0x00AFFA000000FFFFULL;

    tss_init(&g_tss_bsp);
    gdt_set_tss_desc(0, (uint64_t) &g_tss_bsp);

    gdt_load_and_set_segments();

    uint16_t tss_sel = GDT_TSS_SEL(0);
    __asm__ volatile("ltr %0" ::"r"(tss_sel) : "memory");
}

void gdt_ap_load(uint32_t cpu_id) {
    tss_t *tss = tss_for_cpu(cpu_id);
    tss_init(tss);
    gdt_set_tss_desc(cpu_id, (uint64_t) tss);

    gdt_load_and_set_segments();

    uint16_t tss_sel = GDT_TSS_SEL(cpu_id);
    __asm__ volatile("ltr %0" ::"r"(tss_sel) : "memory");
}

void gdt_set_kernel_stack(uint64_t rsp0) {
    uint32_t cpu_id;
    __asm__ volatile("movl %%gs:16, %0" : "=r"(cpu_id));
    tss_for_cpu(cpu_id)->rsp0 = rsp0;
}
