#pragma once
#include <stdint.h>

typedef int (*fat32_read_fn)(void *ctx, uint64_t lba, uint32_t count, void *buf);

int fat32_mount(fat32_read_fn read, void *ctx, const char *mountpoint);
int fat32_mount_mem(const void *image, uint64_t size, const char *mountpoint);
