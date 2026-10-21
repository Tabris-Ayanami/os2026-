/*
 * Simplified SLUB allocator (two-layer design).
 *
 *   layer 1 (page level) : a slab is one physical page obtained from the
 *                          currently installed pmm_manager via alloc_pages(1)
 *                          and returned via free_pages(page, 1).
 *   layer 2 (object level): every size class keeps a list of slabs; a slab is
 *                          carved into fixed-size objects, and free objects are
 *                          chained through a singly-linked free list that lives
 *                          inside the free objects themselves.
 *
 * slub_alloc(size) rounds `size` up to the nearest size class and returns one
 * object; slub_free(ptr) pushes the object back onto its slab's free list.
 * A slab with no live objects is returned to the page allocator immediately,
 * which keeps the two layers consistent and makes the tests easy to verify.
 *
 * The page allocator hands back a `struct Page *`; the corresponding kernel
 * virtual address is obtained through page2pa() + va_pa_offset, which is the
 * direct map installed by the boot page table.
 */

#include <list.h>
#include <pmm.h>
#include <slub.h>
#include <stdio.h>
#include <string.h>

#define SLUB_MAX_SLABS 128

static const size_t slub_class_sizes[] = {
    16, 32, 64, 128, 256, 512, 1024, 2048,
};
#define SLUB_NCLASSES (sizeof(slub_class_sizes) / sizeof(slub_class_sizes[0]))

struct slub_slab {
    list_entry_t link;   // link in the owning cache's slab list
    struct Page *page;   // page obtained from layer 1
    char *base;          // kernel virtual address of the page
    size_t objsize;      // object size of this slab
    int total;           // number of objects the slab holds
    int inuse;           // number of allocated objects
    char *freelist;      // singly-linked free object list
    bool used;           // whether this descriptor is in use
};

struct slub_cache {
    size_t objsize;
    list_entry_t slabs;  // list of struct slub_slab.link
};

static struct slub_cache slub_caches[SLUB_NCLASSES];
static struct slub_slab slub_slab_pool[SLUB_MAX_SLABS];
static bool slub_initialized = 0;

#define le2slab(le, member) \
    to_struct((le), struct slub_slab, member)

/* slab_class_index - index of the smallest class that can hold `size` */
static int
slab_class_index(size_t size) {
    for (size_t i = 0; i < SLUB_NCLASSES; i++) {
        if (size <= slub_class_sizes[i]) {
            return (int)i;
        }
    }
    return -1;
}

void
slub_init(void) {
    for (size_t i = 0; i < SLUB_NCLASSES; i++) {
        slub_caches[i].objsize = slub_class_sizes[i];
        list_init(&slub_caches[i].slabs);
    }
    for (int i = 0; i < SLUB_MAX_SLABS; i++) {
        slub_slab_pool[i].used = 0;
    }
    slub_initialized = 1;
}

static struct slub_slab *
slab_descriptor_get(void) {
    for (int i = 0; i < SLUB_MAX_SLABS; i++) {
        if (!slub_slab_pool[i].used) {
            slub_slab_pool[i].used = 1;
            return &slub_slab_pool[i];
        }
    }
    return NULL;
}

/*
 * slab_create - allocate one page from layer 1 and carve it into objects.
 * The free list is built from the last object to the first, so the first
 * allocation returns the lowest-addressed object.
 */
static struct slub_slab *
slab_create(struct slub_cache *cache, size_t objsize) {
    struct slub_slab *slab = slab_descriptor_get();
    if (slab == NULL) {
        return NULL;
    }

    struct Page *page = alloc_pages(1);
    if (page == NULL) {
        slab->used = 0;
        return NULL;
    }

    slab->page = page;
    slab->base = (char *)(page2pa(page) + va_pa_offset);
    slab->objsize = objsize;
    slab->total = (int)(PGSIZE / objsize);
    slab->inuse = 0;
    slab->freelist = NULL;

    for (int i = slab->total - 1; i >= 0; i--) {
        char *obj = slab->base + (size_t)i * objsize;
        *(char **)obj = slab->freelist;
        slab->freelist = obj;
    }

    list_add(&cache->slabs, &slab->link);
    return slab;
}

/* slab_destroy - return the slab's page to layer 1 */
static void
slab_destroy(struct slub_slab *slab) {
    list_del(&slab->link);
    free_pages(slab->page, 1);
    slab->used = 0;
}

void *
slub_alloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    int ci = slab_class_index(size);
    if (ci < 0) {
        return NULL;   // request larger than the biggest class
    }
    if (!slub_initialized) {
        slub_init();
    }

    struct slub_cache *cache = &slub_caches[ci];

    // prefer an existing slab that still has free objects
    struct slub_slab *slab = NULL;
    list_entry_t *le = &cache->slabs;
    while ((le = list_next(le)) != &cache->slabs) {
        struct slub_slab *s = le2slab(le, link);
        if (s->freelist != NULL) {
            slab = s;
            break;
        }
    }
    if (slab == NULL) {
        slab = slab_create(cache, cache->objsize);
        if (slab == NULL) {
            return NULL;
        }
    }

    void *obj = slab->freelist;
    slab->freelist = *(char **)obj;
    slab->inuse++;
    return obj;
}

/* slab_lookup - find the slab that owns `ptr`, or NULL if it is unknown */
static struct slub_slab *
slab_lookup(void *ptr) {
    char *p = (char *)ptr;
    for (size_t i = 0; i < SLUB_NCLASSES; i++) {
        list_entry_t *le = &slub_caches[i].slabs;
        while ((le = list_next(le)) != &slub_caches[i].slabs) {
            struct slub_slab *s = le2slab(le, link);
            if (p >= s->base && p < s->base + PGSIZE) {
                return s;
            }
        }
    }
    return NULL;
}

void
slub_free(void *ptr) {
    if (ptr == NULL) {
        return;
    }
    struct slub_slab *slab = slab_lookup(ptr);
    if (slab == NULL) {
        return;   // not an object handed out by slub_alloc
    }

    // the pointer must be the start of an object in this slab
    size_t offset = (size_t)((char *)ptr - slab->base);
    if (offset % slab->objsize != 0 || offset >= PGSIZE) {
        return;
    }

    *(char **)ptr = slab->freelist;
    slab->freelist = (char *)ptr;
    slab->inuse--;

    // release an empty slab back to the page allocator
    if (slab->inuse == 0) {
        slab_destroy(slab);
    }
}

/* ---------------------------------------------------------------------- */
/* Tests                                                                  */
/* ---------------------------------------------------------------------- */

#define SLUB_TEST_MAX_OBJECTS 512
static void *slub_test_ptrs[SLUB_TEST_MAX_OBJECTS];

static void
slub_test_reset(void) {
    // every test frees what it allocates, so this only resets bookkeeping
    slub_init();
}

void
slub_test(void) {
    cprintf("========== SLUB Test ==========\n");

    /* 1. single small object: rounded up to the smallest class, then freed */
    slub_test_reset();
    size_t before = nr_free_pages();
    void *p = slub_alloc(1);
    assert(p != NULL);
    assert((uintptr_t)p % 16 == 0);
    assert(nr_free_pages() == before - 1);   // first slab takes one page
    slub_free(p);
    assert(nr_free_pages() == before);       // empty slab is released
    cprintf("[SLUB Test 1] single alloc/free: PASS\n");

    /* 2. a full slab: all objects distinct, data survives, one page used */
    slub_test_reset();
    before = nr_free_pages();
    int n = (int)(PGSIZE / 16);              // 256 objects in one slab
    for (int i = 0; i < n; i++) {
        slub_test_ptrs[i] = slub_alloc(16);
        assert(slub_test_ptrs[i] != NULL);
    }
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            assert(slub_test_ptrs[i] != slub_test_ptrs[j]);
        }
    }
    for (int i = 0; i < n; i++) {
        *(uint64_t *)slub_test_ptrs[i] = 0xdead0000UL + (uint64_t)i;
    }
    for (int i = 0; i < n; i++) {
        assert(*(uint64_t *)slub_test_ptrs[i] == 0xdead0000UL + (uint64_t)i);
    }
    assert(nr_free_pages() == before - 1);   // exactly one slab page

    void *extra = slub_alloc(16);            // forces a second slab
    assert(extra != NULL);
    assert(nr_free_pages() == before - 2);
    slub_free(extra);
    for (int i = 0; i < n; i++) {
        slub_free(slub_test_ptrs[i]);
    }
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 2] full slab and second slab: PASS\n");

    /* 3. freeing one object makes it reusable by the next allocation */
    slub_test_reset();
    before = nr_free_pages();
    void *a = slub_alloc(64);
    void *b = slub_alloc(64);
    void *c = slub_alloc(64);
    assert(a != NULL && b != NULL && c != NULL);
    assert(nr_free_pages() == before - 1);   // all three share one slab
    slub_free(b);
    void *d = slub_alloc(64);
    assert(d == b);                          // LIFO reuse, no new page
    assert(nr_free_pages() == before - 1);
    slub_free(a);
    slub_free(c);
    slub_free(d);
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 3] object reuse inside a slab: PASS\n");

    /* 4. every size class: allocation succeeds and objects are size-aligned */
    slub_test_reset();
    before = nr_free_pages();
    for (size_t i = 0; i < SLUB_NCLASSES; i++) {
        void *obj = slub_alloc(slub_class_sizes[i]);
        assert(obj != NULL);
        assert((uintptr_t)obj % slub_class_sizes[i] == 0);
        slub_free(obj);
        assert(nr_free_pages() == before);
    }
    cprintf("[SLUB Test 4] all size classes and alignment: PASS\n");

    /* 5. objects of different classes are independent and do not alias */
    slub_test_reset();
    before = nr_free_pages();
    size_t live = 0;
    for (size_t i = 0; i < SLUB_NCLASSES; i++) {
        for (int k = 0; k < 4; k++) {
            void *obj = slub_alloc(slub_class_sizes[i]);
            assert(obj != NULL);
            memset(obj, (int)(i * 4 + k + 1), slub_class_sizes[i]);
            slub_test_ptrs[live++] = obj;
        }
    }
    // every object must still hold its own pattern: no overlap between classes
    for (size_t i = 0; i < live; i++) {
        struct slub_slab *s = slab_lookup(slub_test_ptrs[i]);
        assert(s != NULL);
        unsigned char pattern = (unsigned char)(i + 1);
        for (size_t b = 0; b < s->objsize; b++) {
            assert(((unsigned char *)slub_test_ptrs[i])[b] == pattern);
        }
    }
    for (size_t i = 0; i < live; i++) {
        slub_free(slub_test_ptrs[i]);
    }
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 5] class independence: PASS\n");

    /* 6. invalid requests are rejected */
    slub_test_reset();
    before = nr_free_pages();
    assert(slub_alloc(0) == NULL);
    assert(slub_alloc(2049) == NULL);
    assert(slub_alloc(4096) == NULL);
    slub_free(NULL);                          // must be harmless
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 6] invalid requests: PASS\n");

    /* 7. free order does not matter: FIFO and LIFO both restore the pages */
    slub_test_reset();
    before = nr_free_pages();
    for (int i = 0; i < 8; i++) {
        slub_test_ptrs[i] = slub_alloc(32);
        assert(slub_test_ptrs[i] != NULL);
    }
    for (int i = 0; i < 8; i++) {
        slub_free(slub_test_ptrs[i]);         // FIFO
    }
    assert(nr_free_pages() == before);

    for (int i = 0; i < 8; i++) {
        slub_test_ptrs[i] = slub_alloc(32);
        assert(slub_test_ptrs[i] != NULL);
    }
    for (int i = 7; i >= 0; i--) {
        slub_free(slub_test_ptrs[i]);         // LIFO
    }
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 7] free ordering: PASS\n");

    /* 8. stress: mixed sizes, repeated alloc/free, pages fully restored */
    slub_test_reset();
    before = nr_free_pages();
    for (int round = 0; round < 100; round++) {
        size_t size = slub_class_sizes[round % SLUB_NCLASSES];
        void *obj = slub_alloc(size);
        assert(obj != NULL);
        *(uint64_t *)obj = (uint64_t)round;
        assert(*(uint64_t *)obj == (uint64_t)round);
        slub_free(obj);
    }
    assert(nr_free_pages() == before);
    cprintf("[SLUB Test 8] mixed-size stress: PASS\n");

    cprintf("========== SLUB Test PASSED ==========\n");
}
