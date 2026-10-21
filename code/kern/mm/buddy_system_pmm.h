#ifndef __KERN_MM_BUDDY_SYSTEM_PMM_H__
#define __KERN_MM_BUDDY_SYSTEM_PMM_H__

#include <pmm.h>

extern const struct pmm_manager buddy_pmm_manager;

/*
 * Self-contained correctness tests for the buddy allocator.
 * They run on a private, statically allocated Page array, so they can be
 * executed at boot without disturbing the currently installed pmm_manager.
 */
void buddy_test(void);

#endif /* !__KERN_MM_BUDDY_SYSTEM_PMM_H__ */
