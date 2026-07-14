#pragma once

#include <stdint.h>

int64_t sys_ptrace(int64_t request, int64_t pid, uint64_t addr, uint64_t data);
