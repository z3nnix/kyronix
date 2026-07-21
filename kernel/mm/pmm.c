#include "pmm.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/percpu.h"
#include "lib/log.h"
#include "lib/string.h"
#include "llfree.h"
#include "llfree_inner.h"
#ifdef CONFIG_KMEMLEAK
#include "kmemleak.h"
#endif

unsigned long __popcountdi2(unsigned long val) {
    unsigned long count = 0;
    for (int i = 0; i < 64; i++) count += (val >> i) & 1;
    return count;
}

uint64_t g_hhdm_offset;

static llfree_t g_llfree;
static uint8_t *g_meta_local;
static uint8_t *g_meta_trees;
static uint8_t *g_meta_lower;

static uint64_t g_total_frames;
static uint64_t g_free_frames;
static uint64_t g_usable_pages;
static uint64_t g_alloc_total;
static uint64_t g_free_total;
static uint64_t g_first_frame;

#define ZPOOL_SIZE 32
static void *g_zpool[ZPOOL_SIZE];
static uint64_t g_zpool_top;

static inline uint32_t pmm_cpu_id(void) { return this_cpu_id(); }

static inline uint8_t order_for_pages(uint64_t n) {
    if (n == 0) return 0;
    uint8_t order = 0;
    uint64_t p = 1;
    while (p < n) {
        p <<= 1;
        order++;
    }
    return order;
}

void pmm_init(struct limine_memmap_response *mmap, uint64_t hhdm_offset, uint64_t kernel_end_phys) {
    g_hhdm_offset = hhdm_offset;

    log_info("PMM: memory map (%lu entries):", mmap->entry_count);
    uint64_t highest = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        log_info("  [%lu] base=0x%016lx  length=0x%016lx  type=%lu", i, e->base, e->length,
                 e->type);
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        uint64_t top = e->base + e->length;
        if (top > highest) highest = top;
    }

    g_total_frames = highest >> PAGE_SHIFT;
    g_usable_pages = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE) g_usable_pages += e->length >> PAGE_SHIFT;
    }

    log_info("PMM: highest=0x%016lx  total_frames=%lu", highest, g_total_frames);

    llfree_classing_t classing = llfree_classing_simple(MAX_CPUS);
    llfree_meta_size_t ms = llfree_metadata_size(&classing, g_total_frames);

    uint64_t meta_total = ms.local + ms.trees + ms.lower;

    uint64_t region_base = 0;
    uint64_t region_len = 0;
    for (uint64_t i = 0; i < mmap->entry_count; i++) {
        struct limine_memmap_entry *e = mmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE) continue;
        if (e->length > region_len) {
            region_base = e->base;
            region_len = e->length;
        }
    }
    if (!region_base) {
        log_error("PMM: no usable region for LLFree metadata");
        return;
    }

    uint64_t meta_phys = region_base;
    if (kernel_end_phys >= region_base && kernel_end_phys < region_base + region_len) {
        meta_phys = PAGE_ALIGN_UP(kernel_end_phys);
    }

    g_meta_local = (uint8_t *) (meta_phys + hhdm_offset);
    g_meta_trees = g_meta_local + ms.local;
    g_meta_lower = g_meta_trees + ms.trees;

    g_first_frame = (meta_phys + meta_total + PAGE_SIZE - 1) >> PAGE_SHIFT;
    uint64_t end_frame = (region_base + region_len) >> PAGE_SHIFT;
    uint64_t managed_frames = end_frame - g_first_frame;

    if (managed_frames < 512) {
        log_error("PMM: not enough frames for LLFree (need >= 512, have %lu)", managed_frames);
        return;
    }

    log_info("PMM: LLFree meta  local=%lu  trees=%lu  lower=%lu  (total %lu KiB)", ms.local,
             ms.trees, ms.lower, meta_total >> 10);
    log_info("PMM: LLFree range  first_frame=%lu  managed=%lu  region=0x%016lx", g_first_frame,
             managed_frames, region_base);

    llfree_meta_t meta = { .local = g_meta_local, .trees = g_meta_trees, .lower = g_meta_lower };
    llfree_result_t r = llfree_init(&g_llfree, managed_frames, LLFREE_INIT_FREE, meta, &classing);
    if (!llfree_is_ok(r)) {
        log_error("PMM: llfree_init failed (error=%u)", r.error);
        return;
    }

    g_free_frames = managed_frames;

    log_info("PMM: %llu MiB free  (%lu pages, %lu total)",
             (unsigned long long) (g_free_frames * PAGE_SIZE) >> 20, g_free_frames, g_total_frames);

    for (g_zpool_top = 0; g_zpool_top < ZPOOL_SIZE; g_zpool_top++) {
        void *phys = pmm_alloc();
        if (!phys) break;
        memset(phys_to_virt((uint64_t) phys), 0, PAGE_SIZE);
        g_zpool[g_zpool_top] = phys;
    }
    log_info("PMM: zero-page pool filled (%lu pages)", g_zpool_top);
}

void *pmm_alloc(void) {
    llfree_request_t req = { .order = 0, .class = 0, .local = ll_some(pmm_cpu_id()) };
    llfree_result_t r = llfree_get(&g_llfree, frame_id_none(), req);
    if (!llfree_is_ok(r)) return NULL;

    g_free_frames--;
    g_alloc_total++;
    void *phys = (void *) ((r.frame.value + g_first_frame) << PAGE_SHIFT);
#ifdef CONFIG_KMEMLEAK
    kmemleak_track_page(phys);
#endif
    return phys;
}

void *pmm_alloc_zeroed(void) {
    if (g_zpool_top > 0) {
        g_zpool_top--;
        void *phys = g_zpool[g_zpool_top];
        g_zpool[g_zpool_top] = NULL;
        g_free_frames--;
        g_alloc_total++;
#ifdef CONFIG_KMEMLEAK
        kmemleak_track_page(phys);
#endif
        return phys;
    }
    void *phys = pmm_alloc();
    if (!phys) return NULL;
    memset(phys_to_virt((uint64_t) phys), 0, PAGE_SIZE);
    return phys;
}

void *pmm_alloc_contiguous(uint64_t n) {
    if (n == 0) return NULL;
    if (n == 1) return pmm_alloc();

    uint8_t order = order_for_pages(n);
    if (order > LLFREE_MAX_ORDER) return NULL;

    llfree_request_t req = { .order = order, .class = 0, .local = ll_some(pmm_cpu_id()) };
    llfree_result_t r = llfree_get(&g_llfree, frame_id_none(), req);
    if (!llfree_is_ok(r)) return NULL;

    g_free_frames -= (1ULL << order);
    g_alloc_total += (1ULL << order);
    void *phys = (void *) ((r.frame.value + g_first_frame) << PAGE_SHIFT);
#ifdef CONFIG_KMEMLEAK
    for (uint64_t i = 0; i < n; i++)
        kmemleak_track_page((void *) ((uint64_t) phys + i * PAGE_SIZE));
#endif
    return phys;
}

void pmm_free(void *phys) {
    if (!phys) return;
    uint64_t frame = ((uint64_t) phys >> PAGE_SHIFT) - g_first_frame;
    llfree_request_t req = { .order = 0, .class = 0, .local = ll_some(pmm_cpu_id()) };
    llfree_result_t r = llfree_put(&g_llfree, frame_id(frame), req);
    if (llfree_is_ok(r)) {
        g_free_frames++;
        g_free_total++;
    }
#ifdef CONFIG_KMEMLEAK
    kmemleak_untrack_page(phys);
#endif
}

uint64_t pmm_free_pages(void) { return g_free_frames; }
uint64_t pmm_total_pages(void) { return g_total_frames; }
uint64_t pmm_usable_pages(void) { return g_usable_pages; }
uint64_t pmm_alloc_total(void) { return g_alloc_total; }
uint64_t pmm_free_total(void) { return g_free_total; }
