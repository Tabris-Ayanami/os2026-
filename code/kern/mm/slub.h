#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

/*
 * Simplified SLUB allocator.
 *
 * Layer 1: slabs are obtained from the page allocator (alloc_pages/free_pages).
 * Layer 2: each slab is carved into fixed-size objects of one size class;
 *          slub_alloc/slub_free hand out and recycle objects of any size
 *          (rounded up to the nearest class).
 */
void *slub_alloc(size_t size);
void slub_free(void *ptr);

/* Self-contained correctness tests (allocate real pages, restore them). */
void slub_test(void);

#endif /* !__KERN_MM_SLUB_H__ */
