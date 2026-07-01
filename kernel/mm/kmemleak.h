#pragma once
#include <stdint.h>

#define KMEMLEAK_BT_DEPTH 8

void kmemleak_track(void *ptr, uint64_t size);
void kmemleak_untrack(void *ptr);
void kmemleak_track_page(void *phys);
void kmemleak_untrack_page(void *phys);
void kmemleak_page_perm(void *phys);

/* scan → write report into buf → return leaked count (does NOT clear) */
int kmemleak_report(char *buf, uint64_t bufsz);

/* scan → return leaked count → clear all tracked entries (checkpoint) */
int kmemleak_checkpoint(void);

/* clear tracked entries without scan */
void kmemleak_clear(void);
