#pragma once
#include "cpu.h"

#define GDT_NULL 0x00
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA 0x18
#define GDT_USER_CODE 0x20
#define GDT_USER_DATA_SEL (GDT_USER_DATA | 0x3)
#define GDT_USER_CODE_SEL (GDT_USER_CODE | 0x3)
#define GDT_TSS 0x28

void gdt_init(void);
void gdt_ap_load(uint32_t cpu_id);
void gdt_set_kernel_stack(uint64_t rsp0);
