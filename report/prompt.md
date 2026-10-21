# Lab2 提示词汇总（四段式）

本实验的模块划分如下：

| 编号 | 模块 | 涉及文件 | 负责人 |
|------|------|----------|--------|
| 1 | 物理内存初始化 | `kern/mm/pmm.c` | 裴家昌 |
| 2 | Best-Fit 连续内存分配 | `kern/mm/best_fit_pmm.c`、`kern/mm/pmm.c` | 裴家昌 |
| 3-1 | Buddy System 分配器 | 新建 `kern/mm/buddy_system_pmm.c/.h` | 王璞 |
| 3-2 | Buddy System 测试 | `kern/mm/buddy_system_pmm.c/.h`、`kern/init/init.c` | 王璞 |
| 4-1 | SLUB 对象分配器 | 新建 `kern/mm/slub.c/.h` | 李易洲 |
| 4-2 | SLUB 测试 | `kern/mm/slub.c/.h`、`kern/init/init.c` | 李易洲 |

---

## 一、物理内存初始化

````text
[PROMPT]
**任务**：修改 kern/mm/pmm.c，实现物理内存初始化功能，使内核能够根据设备树
（DTB）提供的物理内存信息建立可管理物理页的描述符布局，并把排除 OpenSBI、
内核镜像和 Page 描述符数组占用空间后的完整空闲页面交给当前 pmm_manager 管理。

涉及文件：
- kern/mm/pmm.c：实现 page_init()；
- kern/driver/dtb.c：提供 get_memory_base() 和 get_memory_size()；
- kern/mm/pmm.h、kern/mm/memlayout.h：提供 Page、地址转换和页状态宏；
- kern/init/entry.S：提供链接符号 end 和启动页表。

**操作要求**：你必须在项目中修改实际文件，而不是只展示代码。保留文件中已有的
内容，只补全标有 “lab2 function1 <学号>” 的位置。可以新增 static 辅助函数、
内部宏和静态变量，但不要修改 pmm.h、memlayout.h、mmu.h、list.h 等框架文件，
也不要修改已有的测试函数。

**输出要求**：直接进行文件操作。使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION]
中的信息，不要引入与本任务无关的功能。

[RELY]
// 常量与宏
#define PGSIZE 4096
#define PHYSICAL_MEMORY_OFFSET 0xFFFFFFFF40000000
#define ROUNDUP(a, n)    /* 向上对齐到 n 的整数倍 */
#define ROUNDDOWN(a, n)  /* 向下对齐到 n 的整数倍 */
#define PPN(la) (((uintptr_t)(la)) >> PGSHIFT)
#define PADDR(kva)       /* 内核虚拟地址 -> 物理地址，依赖 va_pa_offset */

// 地址转换（kern/mm/pmm.h）
static inline ppn_t page2ppn(struct Page *page) {
    return page - pages + nbase;
}
static inline uintptr_t page2pa(struct Page *page) {
    return page2ppn(page) << PGSHIFT;
}
static inline struct Page *pa2page(uintptr_t pa) {
    return &pages[PPN(pa) - nbase];
}

// 页描述符（kern/mm/memlayout.h）
struct Page {
    int ref;                 // 引用计数
    uint64_t flags;          // 状态标志
    unsigned int property;   // 空闲块大小，仅块头有效
    list_entry_t page_link;  // 空闲链表节点
};
// 可用宏：SetPageReserved / ClearPageReserved / PageReserved
//         SetPageProperty / ClearPageProperty / PageProperty
//         le2page(le, page_link)

// 全局变量（kern/mm/pmm.c）
struct Page *pages;
size_t npage;
const size_t nbase;          // DRAM_BASE / PGSIZE，本实验为 0x80000
uint64_t va_pa_offset;
const struct pmm_manager *pmm_manager;

// 可用函数
uint64_t get_memory_base(void);   // DTB 报告的物理内存起始地址
uint64_t get_memory_size(void);   // DTB 报告的物理内存大小
static void init_memmap(struct Page *base, size_t n);  // pmm.c 内部，转发给 pmm_manager
void cprintf(const char *fmt, ...);
void panic(const char *fmt, ...);
extern char end[];                // 内核各段结束的虚拟地址

[GUARANTEE]
必须实现或修改的接口：
```c
static void page_init(void);
```

你可以根据需要添加：
- static 辅助函数；
- 模块内部使用的宏；
- 模块内部的静态变量。

不得改变公开接口，也不得修改框架头文件和测试函数。

[SPECIFICATION]
## page_init

**Pre-Condition**：
- dtb_init() 已经执行，get_memory_base()/get_memory_size() 可能返回有效范围，
  也可能返回 0；
- 内核已经运行在虚拟地址空间，va_pa_offset 尚未设置；
- pmm_manager 已经由 init_pmm_manager() 初始化；
- 链接符号 end 指向内核各段结束的虚拟地址。

**Post-Condition**：
- va_pa_offset == PHYSICAL_MEMORY_OFFSET；
- npage == PPN(mem_begin + mem_size)；
- pages 指向 end 之后按页对齐的 Page 数组，数组覆盖物理页号 [nbase, npage)，
  共 npage - nbase 项；
- OpenSBI、内核镜像、Page 描述符数组所在页均为 PageReserved；
- 当前 pmm_manager 只接管描述符数组之后、按页对齐的完整空闲页；
- 串口输出探测到的物理内存范围。

**Case 1（正常情况）**：
- 条件：mem_begin != 0 且 mem_size != 0，并且描述符数组之后至少有一个完整页。
- 行为：设置 va_pa_offset；计算 mem_end；令 npage = PPN(mem_end)；在
  ROUNDUP(end, PGSIZE) 处建立 (npage - nbase) 个 Page 描述符并全部标记为
  PageReserved；计算
  freemem = ROUNDUP(PADDR(pages + (npage - nbase) * sizeof(struct Page)), PGSIZE)；
  free_end = ROUNDDOWN(mem_end, PGSIZE)；调用
  init_memmap(pa2page(freemem), (free_end - freemem) / PGSIZE)。
- 结果：空闲页数量等于 (free_end - freemem) / PGSIZE，且全部位于描述符数组之后。

**Case 2（DTB 信息无效）**：
- 条件：mem_begin == 0 或 mem_size == 0。
- 行为：调用 panic() 终止初始化，不设置 npage/pages，也不调用任何分配器接口。
- 结果：系统不会带着无效的物理内存信息继续运行。

**Case 3（没有完整空闲页）**：
- 条件：freemem >= free_end。
- 行为：不调用 init_memmap()，page_init() 正常返回。
- 结果：当前 pmm_manager 的空闲页数为 0，不会把不完整的尾部页面登记为空闲。

**Case 4（内存末尾不对齐）**：
- 条件：mem_end 不是 PGSIZE 的整数倍。
- 行为：free_end = ROUNDDOWN(mem_end, PGSIZE)，不足一页的尾部不登记。
- 结果：所有交给分配器的页面都是完整页，并且都位于 [mem_begin, mem_end) 内。

**Requirements**：
- 必须区分虚拟地址、物理地址、物理页号和 Page 描述符地址；
- 不得把 OpenSBI、内核镜像或描述符数组占用的页登记为空闲；
- 不得修改框架头文件和已有测试函数；
- 代码必须通过 -Wall -Werror 编译。
````

---

## 二、Best-Fit 连续内存分配

````text
[PROMPT]
**任务**：修改 kern/mm/best_fit_pmm.c，实现完整的 Best-Fit 连续物理内存管理器，
包括内部状态、管理函数和 pmm_manager 注册；同时修改 kern/mm/pmm.c 的
init_pmm_manager()，把当前管理器切换为 best_fit_pmm_manager。

涉及文件：
- kern/mm/best_fit_pmm.c：实现分配器，保留文件中的 basic_check() 和
  best_fit_check()，不得修改；
- kern/mm/pmm.c：只修改 init_pmm_manager() 中的管理器选择。

**操作要求**：你必须在项目中修改实际文件。只在
“lab2 function2-2 <学号>”标记处补全。可以新增 static 辅助函数、内部宏和
静态变量，但不得修改 pmm.h、memlayout.h、list.h、default_pmm.c，以及
basic_check() 和 best_fit_check()。

**输出要求**：直接进行文件操作。使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION]
中的信息，不要引入与本任务无关的功能。

[RELY]
// 空闲区与链表（kern/mm/memlayout.h、libs/list.h）
typedef struct {
    list_entry_t free_list;   // 空闲链表头
    unsigned int nr_free;     // 空闲页总数
} free_area_t;

// 可用链表操作
list_init / list_add / list_add_before / list_del /
list_empty / list_next / list_prev / le2page

// struct Page 与页状态
struct Page {
    int ref;
    uint64_t flags;
    unsigned int property;
    list_entry_t page_link;
};
SetPageProperty / ClearPageProperty / PageProperty
SetPageReserved / PageReserved
set_page_ref / page_ref

// 管理器接口（kern/mm/pmm.h）
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};
extern const struct pmm_manager *pmm_manager;

// 文件内已有、但不得修改的测试函数
static void basic_check(void);
static void best_fit_check(void);

[GUARANTEE]
必须实现或修改的接口：
```c
static void best_fit_init(void);
static void best_fit_init_memmap(struct Page *base, size_t n);
static struct Page *best_fit_alloc_pages(size_t n);
static void best_fit_free_pages(struct Page *base, size_t n);
static size_t best_fit_nr_free_pages(void);

const struct pmm_manager best_fit_pmm_manager;
```

你可以根据需要添加 static 辅助函数、内部宏和静态变量；不得改变测试函数和
公开接口。

[SPECIFICATION]
## best_fit_init

**Pre-Condition**：
- 管理器尚未使用，free_list 尚未初始化。

**Post-Condition**：
- free_list 是空链表；
- nr_free == 0。

**Case 1**：首次调用，初始化空闲链表并把 nr_free 置 0。

## best_fit_init_memmap

**Pre-Condition**：
- base 指向一段连续 Page；
- [base, base+n) 中每一页都是 PageReserved；
- n > 0。

**Post-Condition**：
- 这些页不再是保留页；
- 首地址 base 的 property = n，且 PageProperty(base) = 1；
- 该块按物理地址递增插入 free_list；
- nr_free 增加 n。

**Case 1（空链表）**：把块直接加入链表头。

**Case 2（非空链表）**：按 base 的地址找到插入位置，插在第一个地址更大的块
之前；如果所有块地址都更小，则插到链表尾部。

**Case 3（n <= 0）**：用 assert 拒绝，不修改任何状态。

## best_fit_alloc_pages

**Pre-Condition**：
- n > 0；
- free_list 中可能有若干空闲块。

**Post-Condition**：
- 成功时返回一个空闲块的首地址，该块从链表中移除，PageProperty 被清除；
- 如果块大小大于 n，剩余部分作为新块插回链表；
- nr_free 减少 n；
- 失败时返回 NULL，链表和 nr_free 不变。

**Case 1（n > nr_free）**：直接返回 NULL。

**Case 2（找不到 property >= n 的块）**：返回 NULL，状态不变。

**Case 3（找到多个候选块）**：选择 property 最小且 >= n 的块；如果多个块大小
相同，选择链表中地址较小的那个。

**Case 4（块大小等于 n）**：整块从链表移除并返回。

**Case 5（块大小大于 n）**：移除首块，把 page + n 作为剩余块，property 设为
原大小 - n，并按原位置插回链表；返回首块。

## best_fit_free_pages

**Pre-Condition**：
- base 指向一段连续 Page；
- [base, base+n) 中每一页都不是保留页，也不是空闲块头；
- n > 0。

**Post-Condition**：
- 这些页的 flags 清零，ref 置 0；
- base 的 property = n，且 PageProperty(base) = 1；
- 块按物理地址递增插入 free_list；
- 与前后相邻的空闲块合并；
- nr_free 增加 n。

**Case 1（空链表）**：直接插入。

**Case 2（只与前一相邻块合并）**：前一块 p 满足
p + p->property == base 时，把 n 加到 p->property，删除 base 的节点，
并把 base 改为 p。

**Case 3（只与后一相邻块合并）**：后一块 q 满足
base + base->property == q 时，把 q->property 加到 base->property，
清除 q 的 PageProperty 并删除 q 的节点。

**Case 4（前后都相邻）**：先与前一合并，再与后一合并，最终得到一个更大的块。

**Case 5（都不相邻）**：只插入，不合并。

## best_fit_nr_free_pages

**Pre-Condition**：无。

**Post-Condition**：返回当前空闲页总数 nr_free。

**Requirements**：
- 空闲链表必须始终按物理地址递增；
- 不得修改 basic_check() / best_fit_check()；
- 代码必须通过 -Wall -Werror 编译；
- 最后定义 best_fit_pmm_manager，并在 init_pmm_manager() 中切换。
````

---

## 三、Buddy System

### 3-1 分配器模块

````text
[PROMPT]
**任务**：新建 kern/mm/buddy_system_pmm.c 和 kern/mm/buddy_system_pmm.h，实现
Buddy System 物理内存管理器，并注册 buddy_pmm_manager；在 kern/mm/pmm.c 中
提供可选的切换入口（例如使用 -D USE_BUDDY_PMM），默认仍使用 best_fit。

涉及文件：
- 新建 kern/mm/buddy_system_pmm.c/.h：分配器实现；
- kern/mm/pmm.c：只在 init_pmm_manager() 中增加可选切换；
- kern/init/init.c：后续由测试 prompt 添加自检调用。

**操作要求**：你必须创建并修改实际文件。可以新增 static 辅助函数、内部宏和
静态变量，但不得修改 pmm.h、memlayout.h、list.h、default_pmm.c、
best_fit_pmm.c。

**输出要求**：直接进行文件操作。使用下面 [RELY]、[GUARANTEE]、[SPECIFICATION]
中的信息，不要实现与本任务无关的功能。

[RELY]
// 管理器接口
struct pmm_manager {
    const char *name;
    void (*init)(void);
    void (*init_memmap)(struct Page *base, size_t n);
    struct Page *(*alloc_pages)(size_t n);
    void (*free_pages)(struct Page *base, size_t n);
    size_t (*nr_free_pages)(void);
    void (*check)(void);
};

// struct Page 与页状态
struct Page {
    int ref;
    uint64_t flags;
    unsigned int property;
    list_entry_t page_link;
};
SetPageReserved / PageReserved
SetPageProperty / ClearPageProperty / PageProperty
set_page_ref / page_ref

// 链表
list_init / list_add / list_add_before / list_del /
list_empty / list_next / list_prev / le2page

// 常量
#define PGSIZE 4096

[GUARANTEE]
必须实现或修改的接口：
```c
static void buddy_init(void);
static void buddy_init_memmap(struct Page *base, size_t n);
static struct Page *buddy_alloc_pages(size_t n);
static void buddy_free_pages(struct Page *base, size_t n);
static size_t buddy_nr_free_pages(void);
static void buddy_check(void);

const struct pmm_manager buddy_pmm_manager;
```

你可以根据需要添加 static 辅助函数（例如 order_of、round_up_pow2、
block_index、__buddy_make_free）、内部宏和静态变量。

[SPECIFICATION]
## 数据结构与不变量

- 每个空闲块的大小是 2 的幂次页，块按自身大小对齐；
- 块起始页号为 I、大小为 S 时，伙伴页号是 I ^ S；
- 每个阶维护一条空闲链表，块头 page->property 保存块大小；
- nr_free 等于所有空闲链表中块大小之和。

## buddy_init

**Pre-Condition**：无。

**Post-Condition**：
- 所有阶的空闲链表为空；
- nr_free == 0；
- 区域基址、页数、最大阶尚未设置。

**Case 1**：首次初始化。

## buddy_init_memmap

**Pre-Condition**：
- base 指向连续 Page；
- [base, base+n) 全部是 PageReserved；
- n > 0。

**Post-Condition**：
- 每页的 flags/property 清零，ref 置 0；
- 记录 buddy_base = base、buddy_npages = n、
  buddy_max_order = floor(log2(n))；
- 把区域分解为尽量大且按自身大小对齐的 2 的幂次块，加入对应阶链表；
- nr_free = n。

**Case 1（n 是 2 的幂且 base 对齐）**：分解为单个块。

**Case 2（n 不是 2 的幂）**：分解为最多 O(log n) 个块，大小之和为 n。

**Case 3（n <= 0）**：用 assert 拒绝。

## buddy_alloc_pages

**Pre-Condition**：n > 0。

**Post-Condition**：
- 成功时返回大小至少为 n 的 2 的幂次块的首地址；
- 块已从空闲链表移除；
- nr_free 减少实际分配的块大小；
- 失败时返回 NULL，状态不变。

**Case 1（n > nr_free）**：返回 NULL。

**Case 2（没有足够大的阶）**：返回 NULL。

**Case 3（找到更大的块）**：反复二分，直到得到大小为 round_up_pow2(n) 的块；
每次二分把另一半挂入低一阶链表。

**Case 4（精确匹配）**：直接摘下返回。

## buddy_free_pages

**Pre-Condition**：
- base 是此前从该管理器分配出去的块首地址；
- n > 0，且 round_up_pow2(n) 与分配时的块大小一致；
- base 按块大小对齐。

**Post-Condition**：
- 释放的块与空闲且等大的伙伴反复合并；
- 合并后的块挂回对应阶链表；
- nr_free 增加实际释放的块大小。

**Case 1（伙伴被占用）**：不合并。

**Case 2（伙伴空闲但大小不同）**：不合并。

**Case 3（伙伴空闲且等大）**：合并，大小翻倍，继续检查新伙伴。

**Case 4（对齐或范围非法）**：assert 失败。

## buddy_nr_free_pages

**Pre-Condition**：无。

**Post-Condition**：返回 nr_free。

## buddy_check

**Pre-Condition**：管理器处于真实使用状态。

**Post-Condition**：
- 每个空闲链表中的块大小等于该阶；
- 块按自身大小对齐；
- 所有块大小之和等于 nr_free；
- 完成一次 alloc/free 往返，往返后 nr_free 恢复。

**Case 1（状态一致）**：正常返回。

**Case 2（状态不一致）**：assert 失败。

**Requirements**：
- 分配时把 n 向上取整到 2 的幂；释放时使用与分配相同的 n，保证块大小一致；
- 只合并空闲且等大的伙伴；
- buddy_check() 不得重置或覆盖当前真实状态；
- 代码必须通过 -Wall -Werror 编译；
- 提供 #ifdef USE_BUDDY_PMM 切换入口，默认仍为 best_fit。
````

### 3-2 测试函数

````text
[PROMPT]
**任务**：为 Buddy System 分配器增加自检函数 buddy_test()，在内核启动时验证
初始化、分配、释放、合并和异常路径；测试使用私有静态 Page 数组，不得干扰
当前已安装的 pmm_manager 和真实物理内存。

涉及文件：
- kern/mm/buddy_system_pmm.c：添加测试函数，与实现放在同一文件，便于调用
  static 函数；
- kern/mm/buddy_system_pmm.h：声明 buddy_test()；
- kern/init/init.c：在 pmm_init() 之后调用 buddy_test()。

**操作要求**：你必须修改实际文件。不得修改分配器本身的行为；可以新增 static
测试辅助函数。测试输出使用 cprintf，检查使用 assert。

**输出要求**：直接进行文件操作。使用下面 [RELY]、[GUARANTEE]、
[SPECIFICATION] 中的信息。

[RELY]
// 分配器的 static 函数（同文件可见）
buddy_init / buddy_init_memmap / buddy_alloc_pages /
buddy_free_pages / buddy_nr_free_pages

// 私有测试区域
static struct Page buddy_test_pages[512];

// 可用宏和函数
SetPageReserved / PageReserved
assert / cprintf

// 全局状态（同文件可见）
buddy_free_lists / buddy_nr_free / buddy_base /
buddy_npages / buddy_max_order

[GUARANTEE]
必须实现或修改的接口：
```c
void buddy_test(void);
```

你可以根据需要添加 static 辅助函数（例如 reset、largest_free_block、
block_index）。

[SPECIFICATION]
## buddy_test

**Pre-Condition**：
- 内核已完成 pmm_init()；
- 当前 pmm_manager 可以是 best_fit 或 buddy；
- 私有测试数组尚未初始化。

**Post-Condition**：
- 8 组测试全部执行并通过；
- 模块的全局状态恢复到测试前的值；
- 真实物理内存和当前 pmm_manager 的状态未被改变；
- 输出每组的 PASS 信息。

**Case 1（初始化与分解）**：初始化 512 页区域；nr_free == 512；
最大空闲块 == 512。

**Case 2（单页分配、释放、完全合并）**：分配 1 页再释放；nr_free 恢复；
最大空闲块恢复为 512。

**Case 3（非 2 的幂）**：alloc(3) 实际占用 4 页，返回块按 4 页对齐，
释放后完全恢复。

**Case 4（耗尽与恢复）**：一次分配整个区域后 nr_free == 0，再次分配返回 NULL；
释放后恢复。

**Case 5（碎片化）**：连续分配 8 个单页，释放偶数下标；此时伙伴仍被占用，
最大空闲块保持为 256；再释放奇数下标，最大空闲块恢复为 512。

**Case 6（对齐）**：对 1、2、4、8、16、32、64 页分别分配，检查返回块下标
按自身大小对齐；释放后 nr_free 恢复。

**Case 7（超大请求）**：请求超过空闲页时返回 NULL 且 nr_free 不变；
剩余空间不足时也返回 NULL。

**Case 8（压力）**：重复 100 次分配 1 页/释放 1 页，nr_free 和最大空闲块
保持不变。

**Requirements**：
- 测试必须在私有静态 Page 数组上运行，不能调用真实的 alloc_pages/free_pages；
- 如果测试会改变模块的全局静态状态（空闲链表、buddy_base 等），必须在测试前
  保存、测试后恢复，避免污染正在使用的管理器；
- 每组测试前后要复位私有区域；
- 不得修改 basic_check()/best_fit_check() 或框架文件。
````

---

## 四、SLUB

### 4-1 分配器模块

````text
[PROMPT]
**任务**：新建 kern/mm/slub.c 和 kern/mm/slub.h，实现一个简化的 SLUB 对象
分配器。第一层用页分配器获取和归还页，第二层把页切成固定大小的对象，对外提供
任意大小的分配与释放。

涉及文件：
- 新建 kern/mm/slub.c/.h；
- kern/init/init.c：后续由测试 prompt 添加 slub_test() 调用；
- Makefile 会自动编译 kern/mm 下的新 .c 文件，无需修改。

**操作要求**：你必须创建并修改实际文件。可以新增 static 辅助函数、内部宏和
静态变量，但不得修改 pmm.h、memlayout.h、list.h 等框架文件，也不得与
default_pmm、best_fit_pmm、buddy_system_pmm 的实现冲突。

**输出要求**：直接进行文件操作。使用下面 [RELY]、[GUARANTEE]、
[SPECIFICATION] 中的信息；不要使用 C 标准库，字符串操作使用 libs/string.h。

[RELY]
// 页级分配
struct Page *alloc_pages(size_t n);
void free_pages(struct Page *base, size_t n);
size_t nr_free_pages(void);

// 地址转换与常量
static inline uintptr_t page2pa(struct Page *page);
extern uint64_t va_pa_offset;
#define PGSIZE 4096
// 直接映射：物理地址 PA 对应的内核虚拟地址为 PA + va_pa_offset

// 链表与内存操作
list_init / list_add / list_del / list_empty / list_next / le2page
memset / memcpy（libs/string.h）

// struct Page
struct Page {
    int ref;
    uint64_t flags;
    unsigned int property;
    list_entry_t page_link;
};

// bool 在 libs/defs.h 中定义为 int，没有 true/false 宏，使用 1/0

[GUARANTEE]
必须实现或修改的接口：
```c
void slub_init(void);
void *slub_alloc(size_t size);
void slub_free(void *ptr);
```

你可以根据需要添加 static 辅助函数（例如 slab_class_index、
slab_descriptor_get、slab_create、slab_destroy、slab_lookup）、内部宏和
静态变量。

[SPECIFICATION]
## 数据结构

- size class：16、32、64、128、256、512、1024、2048 字节；
- 每个 class 维护一个 cache，cache 中挂一条 slab 链表；
- slab 以页为单位，一个 slab 对应一个物理页；
- slab 描述符字段：所属链表节点、struct Page、页的内核虚拟地址、objsize、
  total、inuse、freelist、used；
- 空闲对象用对象自身的内存保存 next 指针（要求对象大小至少 8 字节）。

## slub_init

**Pre-Condition**：无。

**Post-Condition**：
- 所有 cache 的 slab 链表为空；
- 描述符池全部标记为未使用。

**Case 1**：首次初始化；重复调用应保持空状态（或由调用方保证只调用一次）。

## slub_alloc

**Pre-Condition**：size 可以是任意值。

**Post-Condition**：
- 成功时返回一个对象指针，该对象已从所属 slab 的 freelist 摘下，inuse 加 1；
- 失败时返回 NULL，状态不变。

**Case 1（size == 0）**：返回 NULL。

**Case 2（size > 2048）**：返回 NULL。

**Case 3（有空闲对象的 slab）**：从该 slab 的 freelist 摘下对象返回。

**Case 4（没有可用 slab）**：用 alloc_pages(1) 申请一页，按 objsize 切成
total 个对象，建立 freelist，再返回一个对象。

**Case 5（页分配失败）**：返回 NULL，不留下半初始化的 slab。

## slub_free

**Pre-Condition**：ptr 可能是 NULL、非法指针，或 slub_alloc 返回的对象。

**Post-Condition**：
- 合法对象被头插回所属 slab 的 freelist，inuse 减 1；
- inuse 为 0 时，该 slab 被摘链并把页归还给第一层；
- 非法输入不改变状态。

**Case 1（ptr == NULL）**：直接返回。

**Case 2（找不到所属 slab）**：安全返回，不崩溃。

**Case 3（指针未按 objsize 对齐）**：安全返回。

**Case 4（slab 仍有其他对象）**：只归还对象，不释放页。

**Case 5（slab 变为空）**：从 cache 链表摘下，调用 free_pages(page, 1)，
描述符标记为未使用。

**Requirements**：
- 对象按自身大小对齐（页对齐 + 大小整除 PGSIZE）；
- 不得修改框架文件；
- 代码必须通过 -Wall -Werror 编译；
- slub_free() 必须对非法指针安全。
````

### 4-2 测试函数

````text
[PROMPT]
**任务**：为 SLUB 分配器增加自检函数 slub_test()，在内核启动时验证不同大小
对象的分配和释放；测试使用真实页分配器，但必须在结束后归还全部页面。

涉及文件：
- kern/mm/slub.c：添加测试函数；
- kern/mm/slub.h：声明 slub_test()；
- kern/init/init.c：在 pmm_init() 之后调用 slub_test()。

**操作要求**：你必须修改实际文件。不得改变分配器行为；测试对象指针放在静态
数组中，避免占用内核栈；每项测试结束必须释放全部对象。

**输出要求**：直接进行文件操作。使用 assert 和 cprintf，并在每项测试后检查
nr_free_pages()。

[RELY]
slub_alloc / slub_free / slub_init；
nr_free_pages()；
assert / cprintf / memset；
static void *slub_test_ptrs[512]；

[GUARANTEE]
必须实现或修改的接口：
```c
void slub_test(void);
```

你可以根据需要添加 static 辅助函数（例如 slub_test_reset、slab_lookup）。

[SPECIFICATION]
## slub_test

**Pre-Condition**：
- pmm_init() 已执行；
- 当前 pmm_manager 可用。

**Post-Condition**：
- 8 组测试全部执行并通过；
- 所有对象已释放；
- 每项测试结束后 nr_free_pages() 回到测试前的值；
- 输出每组的 PASS 信息。

**Case 1（单对象）**：alloc(1) 返回按 16 字节对齐的指针；页数减 1；
释放后页数恢复。

**Case 2（满 slab）**：分配 256 个 16 字节对象；全部非空且互不相同；
写入特征值后能正确读回；页数只减 1；再分配一个对象触发第二个 slab，
页数再减 1；释放后恢复。

**Case 3（对象复用）**：同一 class 分配 3 个对象（共用一页）；释放中间一个
再分配，必须复用同一地址且不新增页；全部释放后页数恢复。

**Case 4（所有 class）**：对 8 个 size class 分别分配，检查地址按自身大小
对齐，释放后页数恢复。

**Case 5（类间独立性）**：每个 class 分配 4 个对象并写入不同填充值；
最后逐字节校验所有对象，确认不同 class 的对象不重叠。

**Case 6（非法请求）**：alloc(0)、alloc(2049)、alloc(4096) 返回 NULL；
free(NULL) 安全；free 未知指针安全；页数不变。

**Case 7（释放顺序）**：同一 class 分配 8 个对象，分别按 FIFO 和 LIFO 释放；
页数都能恢复。

**Case 8（混合压力）**：100 轮不同 class 的分配、写入、读回、释放；
最终页数恢复。

**Requirements**：
- 测试必须释放所有对象；正常路径不得泄漏页；
- 每组测试开始前复位分配器元数据；
- 不得修改框架文件或其他分配器。
````

---

## 五、分支任务的辅助提问


````text
我在用双 GDB 调试 RISC-V ucore 内核在 QEMU 中的地址翻译过程：
- 主机 gdb 调试 QEMU 源码（target/riscv/cpu_helper.c、accel/tcg/cputlb.c）；
- riscv64-unknown-elf-gdb 通过 gdbstub 调试 guest；
- 想观察一条取指/访存指令的虚拟地址如何经过 TLB 查找和页表遍历变成物理地址。

请帮我：
1. 在 QEMU 源码中找出 TLB 命中检查、TLB 未命中后的 tlb_fill、以及
   get_physical_address 的页表遍历循环，给出函数名和关键行号；
2. 说明 Sv39 下 levels/ptidxbits/ptesize 的含义，以及叶子 PTE、1GiB/2MiB/4KiB
   大页是如何提前终止循环的；
3. 给出用条件断点只观察某个虚拟地址的 gdb 命令；
4. 解释 QEMU 模拟的 TLB 与真实 CPU TLB 在表项内容、替换策略、未命中处理上的区别；
5. 如果我看到页表遍历只执行了一次（levels=3），可能是什么原因？
````

---
