#pragma once

#include <stdint.h>

int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd,
                 uint64_t off);
int64_t sys_munmap(uint64_t addr, uint64_t len);
int64_t sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
int64_t sys_madvise(void *addr, uint64_t len, int advice);
int64_t sys_mremap(uint64_t old_addr, uint64_t old_sz, uint64_t new_sz, uint64_t flags,
                   uint64_t new_addr);
