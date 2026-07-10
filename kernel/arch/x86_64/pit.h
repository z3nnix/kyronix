#pragma once
#include <stdint.h>

#define PIT_CHANNEL0 0x40
#define PIT_CMD      0x43

extern volatile uint64_t g_ticks;
extern uint64_t g_epoch_base;
void pit_init(void);
