/*
 * Buddy System physical memory manager.
 *
 * Physical memory is managed in blocks whose sizes are powers of two
 * (1, 2, 4, 8, ... pages).  Every block is aligned to its own size inside
 * the managed region, so for a block of S pages starting at page index I,
 * its buddy has index I ^ S.  A block can only be merged with its buddy
 * when the buddy is free and has exactly the same size.
 *
 * Implementation notes:
 *  - `buddy_base` is the first Page descriptor of the managed region and is
 *    used as the origin for all block indices.  This keeps the allocator
 *    independent of the global `pages` array, which also makes it possible
 *    to unit-test it on a private static array.
 *  - `property` on a free block's head page stores the block size in pages
 *    (always a power of two).
 *  - alloc_pages(n) rounds n up to the next power of two (the framework only
 *    promises "at least n pages"), so free_pages(base, n) must be called with
 *    the same n that was passed to alloc_pages.
 */

#include <buddy_system_pmm.h>
#include <list.h>
#include <pmm.h>
#include <stdio.h>
#include <string.h>

// One free list per block order (order o <=> block size 2^o pages).
// 32 orders is far more than any region we can describe.
#define BUDDY_MAX_ORDER 32

static list_entry_t buddy_free_lists[BUDDY_MAX_ORDER];
static unsigned int buddy_nr_free;   // total number of free pages
static struct Page *buddy_base;      // origin of the managed region
static size_t buddy_npages;          // number of pages in the region
static int buddy_max_order;          // order of the largest possible block

/* order_of - smallest o such that 2^o >= size */
static int
order_of(size_t size) {
    int o = 0;
    while (((size_t)1 << o) < size) {
        o++;
    }
    return o;
}

/* round_up_pow2 - smallest power of two >= n */
static size_t
round_up_pow2(size_t n) {
    size_t size = 1;
    while (size < n) {
        size <<= 1;
    }
    return size;
}

static inline size_t
block_index(struct Page *p) {
    return (size_t)(p - buddy_base);
}

/*
 * __buddy_make_free - place a block of `size` pages at `p` into the free list.
 * `p` must be aligned to `size` within the managed region.
 */
static void
__buddy_make_free(struct Page *p, size_t size) {
    p->flags = 0;
    p->property = size;
    SetPageProperty(p);
    set_page_ref(p, 0);
    list_add(&buddy_free_lists[order_of(size)], &(p->page_link));
    buddy_nr_free += size;
}

static void
buddy_init(void) {
    for (int o = 0; o < BUDDY_MAX_ORDER; o++) {
        list_init(&buddy_free_lists[o]);
    }
    buddy_nr_free = 0;
    buddy_base = NULL;
    buddy_npages = 0;
    buddy_max_order = 0;
}

static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);

    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    buddy_base = base;
    buddy_npages = n;

    // largest order whose block still fits in the region
    buddy_max_order = 0;
    while (((size_t)1 << (buddy_max_order + 1)) <= n) {
        buddy_max_order++;
    }

    /*
     * Decompose [0, n) into maximal aligned power-of-two blocks: from the
     * current offset, take the largest block that is aligned to its own size
     * and still fits in the remaining pages.  At most 2*log2(n) blocks.
     */
    size_t offset = 0;
    while (offset < n) {
        size_t size = 1;
        while (offset % (size * 2) == 0 && size * 2 <= n - offset) {
            size *= 2;
        }
        __buddy_make_free(base + offset, size);
        offset += size;
    }
}

static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);
    if (n > buddy_nr_free) {
        return NULL;
    }

    size_t size = round_up_pow2(n);
    int need = order_of(size);

    // find the smallest available block that can satisfy the request
    int o = need;
    while (o <= buddy_max_order && list_empty(&buddy_free_lists[o])) {
        o++;
    }
    if (o > buddy_max_order) {
        return NULL;
    }

    list_entry_t *le = list_next(&buddy_free_lists[o]);
    struct Page *p = le2page(le, page_link);
    list_del(&(p->page_link));
    ClearPageProperty(p);
    buddy_nr_free -= ((size_t)1 << o);

    // split the block until it has exactly the requested order
    while (o > need) {
        o--;
        size_t half = (size_t)1 << o;
        __buddy_make_free(p + half, half);
    }

    p->flags = 0;
    p->property = 0;
    set_page_ref(p, 0);
    return p;
}

static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);

    size_t size = round_up_pow2(n);
    size_t index = block_index(base);
    assert(index + size <= buddy_npages);
    assert(index % size == 0);   // block must be aligned to its own size

    struct Page *p = base;
    for (; p != base + size; p++) {
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        set_page_ref(p, 0);
    }

    buddy_nr_free += size;

    // coalesce with free buddies of the same size
    while (size < ((size_t)1 << buddy_max_order)) {
        size_t buddy_index = block_index(base) ^ size;
        if (buddy_index >= buddy_npages) {
            break;
        }
        struct Page *buddy = buddy_base + buddy_index;
        if (!PageProperty(buddy) || buddy->property != size) {
            break;   // buddy is allocated or is the head of a larger block
        }
        list_del(&(buddy->page_link));
        ClearPageProperty(buddy);
        if (buddy < base) {
            base = buddy;
        }
        size <<= 1;
    }

    base->property = size;
    SetPageProperty(base);
    list_add(&buddy_free_lists[order_of(size)], &(base->page_link));
}

static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

static size_t
buddy_largest_free_block(void) {
    for (int o = buddy_max_order; o >= 0; o--) {
        if (!list_empty(&buddy_free_lists[o])) {
            return (size_t)1 << o;
        }
    }
    return 0;
}

/*
 * buddy_check - validate the *current* state of the manager without
 * destroying it.  Every free list must only contain aligned blocks whose
 * property matches the list's order, and the block sizes must add up to
 * nr_free.  A small alloc/free round trip then verifies both paths.
 */
static void
buddy_check(void) {
    assert(buddy_base != NULL);

    size_t total = 0;
    for (int o = 0; o <= buddy_max_order; o++) {
        list_entry_t *le = &buddy_free_lists[o];
        while ((le = list_next(le)) != &buddy_free_lists[o]) {
            struct Page *p = le2page(le, page_link);
            assert(PageProperty(p));
            assert(p->property == ((size_t)1 << o));
            size_t idx = block_index(p);
            assert(idx % p->property == 0);
            assert(idx + p->property <= buddy_npages);
            total += p->property;
        }
    }
    assert(total == buddy_nr_free);

    struct Page *p = buddy_alloc_pages(1);
    assert(p != NULL);
    buddy_free_pages(p, 1);
    assert(buddy_nr_free_pages() == total);
}

/* Saved global state, so the private-array tests can be run at boot
 * without disturbing a manager that is actually in use. */
struct buddy_saved_state {
    list_entry_t lists[BUDDY_MAX_ORDER];
    unsigned int nr_free;
    struct Page *base;
    size_t npages;
    int max_order;
};

static void
buddy_state_save(struct buddy_saved_state *s) {
    memcpy(s->lists, buddy_free_lists, sizeof(buddy_free_lists));
    s->nr_free = buddy_nr_free;
    s->base = buddy_base;
    s->npages = buddy_npages;
    s->max_order = buddy_max_order;
}

static void
buddy_state_restore(const struct buddy_saved_state *s) {
    memcpy(buddy_free_lists, s->lists, sizeof(buddy_free_lists));
    buddy_nr_free = s->nr_free;
    buddy_base = s->base;
    buddy_npages = s->npages;
    buddy_max_order = s->max_order;
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

#define BUDDY_TEST_NPAGES 512

// Private region used by the tests; never handed to the real allocator.
static struct Page buddy_test_pages[BUDDY_TEST_NPAGES];

static void
buddy_test_reset(void) {
    for (int i = 0; i < BUDDY_TEST_NPAGES; i++) {
        buddy_test_pages[i].flags = 0;
        buddy_test_pages[i].property = 0;
        buddy_test_pages[i].ref = 0;
        SetPageReserved(&buddy_test_pages[i]);
    }
    buddy_init();
    buddy_init_memmap(buddy_test_pages, BUDDY_TEST_NPAGES);
}

static size_t
buddy_test_index(struct Page *p) {
    return block_index(p);
}

void
buddy_test(void) {
    // the tests run on a private Page array; preserve the state of any
    // manager that may be installed on the real physical memory
    struct buddy_saved_state saved;
    buddy_state_save(&saved);

    cprintf("========== Buddy System Test ==========\n");

    /* 1. initial state: one fully coalesced block covering the region */
    buddy_test_reset();
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 1] init and decomposition: PASS\n");

    /* 2. allocate one page, then free it: everything must coalesce back */
    struct Page *p = buddy_alloc_pages(1);
    assert(p != NULL);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES - 1);
    buddy_free_pages(p, 1);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 2] alloc/free/full coalescing: PASS\n");

    /* 3. non power-of-two request: 3 pages consume a 4-page block */
    buddy_test_reset();
    p = buddy_alloc_pages(3);
    assert(p != NULL);
    assert(buddy_test_index(p) % 4 == 0);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES - 4);
    buddy_free_pages(p, 3);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 3] non power-of-two request: PASS\n");

    /* 4. exhaustion: the whole region, then failure, then recovery */
    buddy_test_reset();
    p = buddy_alloc_pages(BUDDY_TEST_NPAGES);
    assert(p != NULL);
    assert(buddy_nr_free_pages() == 0);
    assert(buddy_alloc_pages(1) == NULL);
    buddy_free_pages(p, BUDDY_TEST_NPAGES);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 4] exhaustion and recovery: PASS\n");

    /* 5. fragmentation: no merge with allocated buddies, full merge after */
    buddy_test_reset();
    struct Page *ps[8];
    for (int i = 0; i < 8; i++) {
        ps[i] = buddy_alloc_pages(1);
        assert(ps[i] != NULL);
    }
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES - 8);

    for (int i = 0; i < 8; i += 2) {
        buddy_free_pages(ps[i], 1);
    }
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES - 4);
    // buddies of the freed blocks are still allocated, so no coalescing
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES / 2);

    for (int i = 1; i < 8; i += 2) {
        buddy_free_pages(ps[i], 1);
    }
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 5] fragmentation and buddy merging: PASS\n");

    /* 6. every block is aligned to its own size */
    buddy_test_reset();
    size_t sizes[] = {1, 2, 4, 8, 16, 32, 64};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        p = buddy_alloc_pages(sizes[i]);
        assert(p != NULL);
        assert(buddy_test_index(p) % sizes[i] == 0);
        buddy_free_pages(p, sizes[i]);
        assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    }
    cprintf("[Buddy Test 6] block alignment: PASS\n");

    /* 7. oversized request fails without touching the free count */
    buddy_test_reset();
    assert(buddy_alloc_pages(BUDDY_TEST_NPAGES + 1) == NULL);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    p = buddy_alloc_pages(BUDDY_TEST_NPAGES / 2);
    assert(p != NULL);
    assert(buddy_alloc_pages(BUDDY_TEST_NPAGES / 2 + 1) == NULL);
    buddy_free_pages(p, BUDDY_TEST_NPAGES / 2);
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 7] oversized request: PASS\n");

    /* 8. stress: repeated alloc/free stays consistent */
    buddy_test_reset();
    for (int i = 0; i < 100; i++) {
        p = buddy_alloc_pages(1);
        assert(p != NULL);
        buddy_free_pages(p, 1);
    }
    assert(buddy_nr_free_pages() == BUDDY_TEST_NPAGES);
    assert(buddy_largest_free_block() == BUDDY_TEST_NPAGES);
    cprintf("[Buddy Test 8] alloc/free stress: PASS\n");

    buddy_state_restore(&saved);

    cprintf("========== Buddy System Test PASSED ==========\n");
}

const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};
