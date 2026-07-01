#pragma once
#include <stddef.h>
#include <stdint.h>

#define HEAP_START 0xffff910000000000ULL
#define HEAP_MAX 0xffff920000000000ULL

void heap_init(void);
void *kmalloc(uint64_t size);
void *kcalloc(uint64_t nmemb, uint64_t size);
void *krealloc(void *ptr, uint64_t new_size);
void kfree(void *ptr);
void heap_stats(void);
int64_t heap_alloc_delta(void);
uint64_t heap_brk(void);
void heap_walk_used(void (*callback)(void *data, uint64_t size, void *user), void *user);
