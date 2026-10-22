#include <default_pmm.h>
#include <best_fit_pmm.h>
#include <buddy_system_pmm.h>
#include <defs.h>
#include <error.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <sbi.h>
#include <stdio.h>
#include <string.h>
#include <riscv.h>
#include <dtb.h>

// virtual address of physical page array
struct Page *pages;
// amount of physical memory (in pages)
size_t npage = 0;
// the kernel image is mapped at VA=KERNBASE and PA=info.base
uint64_t va_pa_offset;
// memory starts at 0x80000000 in RISC-V
// DRAM_BASE defined in riscv.h as 0x80000000
const size_t nbase = DRAM_BASE / PGSIZE;

// virtual address of boot-time page directory
uintptr_t *satp_virtual = NULL;
// physical address of boot-time page directory
uintptr_t satp_physical;

// physical memory management
const struct pmm_manager *pmm_manager;


static void check_alloc_page(void);

// init_pmm_manager - initialize a pmm_manager instance
static void init_pmm_manager(void) {
#ifdef USE_BUDDY_PMM
    // optional: build with -DUSE_BUDDY_PMM to run the buddy allocator
    pmm_manager = &buddy_pmm_manager;
#else
    pmm_manager = &best_fit_pmm_manager;
#endif
    cprintf("memory management: %s\n", pmm_manager->name);
    pmm_manager->init();
}

// init_memmap - call pmm->init_memmap to build Page struct for free memory
static void init_memmap(struct Page *base, size_t n) {
    pmm_manager->init_memmap(base, n);
}

// alloc_pages - call pmm->alloc_pages to allocate a continuous n*PAGESIZE
// memory
struct Page *alloc_pages(size_t n) {
    return pmm_manager->alloc_pages(n);
}

// free_pages - call pmm->free_pages to free a continuous n*PAGESIZE memory
void free_pages(struct Page *base, size_t n) {
    pmm_manager->free_pages(base, n);
}

// nr_free_pages - call pmm->nr_free_pages to get the size (nr*PAGESIZE)
// of current free memory
size_t nr_free_pages(void) {
    return pmm_manager->nr_free_pages();
}

//lab2 function1 2412953

/* page_init - detect physical memory from DTB, build the Page descriptor
 * array, reserve OpenSBI/kernel/descriptor-array space, and hand the rest
 * of the complete pages to the pmm manager */
static void
page_init(void) {
    extern char end[];

    // virtual<->physical offset, required by PADDR/KADDR
    va_pa_offset = PHYSICAL_MEMORY_OFFSET;

    uint64_t mem_begin = get_memory_base();
    uint64_t mem_size = get_memory_size();
    uint64_t mem_end = mem_begin + mem_size;
    // This kernel's Page indexing and direct map assume DRAM_BASE.
    if (mem_begin != DRAM_BASE || mem_size == 0 || mem_end <= mem_begin) {
        panic("page_init: invalid physical memory info from DTB\n");
    }

    cprintf("detected physical memory: [0x%016lx, 0x%016lx), %ld MB\n",
            mem_begin, mem_end, mem_size / (1024 * 1024));

    uint64_t managed_end = mem_end < PHYSICAL_MEMORY_END
                               ? mem_end : PHYSICAL_MEMORY_END;
    managed_end = ROUNDDOWN(managed_end, PGSIZE);
    if (managed_end <= mem_begin) {
        panic("page_init: no complete physical pages in the managed range\n");
    }
    cprintf("kernel manages physical memory: [0x%016lx, 0x%016lx), %ld MB\n",
            mem_begin, managed_end, (managed_end - mem_begin) / (1024 * 1024));

    // the page descriptor array covers physical page numbers [nbase, npage)
    npage = PPN(managed_end);
    size_t page_count = npage - nbase;
    pages = (struct Page *)ROUNDUP((void *)end, PGSIZE);

    // Validate the descriptor storage before writing into it.
    uintptr_t pages_pa = PADDR(pages);
    if (pages_pa >= managed_end ||
        page_count > (managed_end - pages_pa) / sizeof(struct Page)) {
        panic("page_init: insufficient memory for Page descriptors\n");
    }

    // reserve every page first; the free region is released below
    for (size_t i = 0; i < page_count; i++) {
        pages[i].flags = 0;
        SetPageReserved(pages + i);
    }

    // free memory starts right after the descriptor array (page aligned),
    // and ends at the capped managed memory end (page aligned down); only
    // hand over the pages if at least one complete page is left
    uintptr_t freemem =
        ROUNDUP(pages_pa + sizeof(struct Page) * page_count, PGSIZE);
    uintptr_t free_end = managed_end;
    if (freemem < free_end) {
        init_memmap(pa2page(freemem), (free_end - freemem) / PGSIZE);
    }
}

/* pmm_init - initialize the physical memory management */
void pmm_init(void) {
    // We need to alloc/free the physical memory (granularity is 4KB or other size).
    // So a framework of physical memory manager (struct pmm_manager)is defined in pmm.h
    // First we should init a physical memory manager(pmm) based on the framework.
    // Then pmm can alloc/free the physical memory.
    // Now the first_fit/best_fit/worst_fit/buddy_system pmm are available.
    init_pmm_manager();

    // detect physical memory space, reserve already used memory,
    // then use pmm->init_memmap to create free page list
    page_init();

    // use pmm->check to verify the correctness of the alloc/free function in a pmm
    check_alloc_page();

    extern char boot_page_table_sv39[];
    satp_virtual = (pte_t*)boot_page_table_sv39;
    satp_physical = PADDR(satp_virtual);
    cprintf("satp virtual address: 0x%016lx\nsatp physical address: 0x%016lx\n", satp_virtual, satp_physical);
}

static void check_alloc_page(void) {
    pmm_manager->check();
    cprintf("check_alloc_page() succeeded!\n");
}
