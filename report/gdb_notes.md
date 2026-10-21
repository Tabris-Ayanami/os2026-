# Lab2 分支任务调试笔记

完整分析见 `report.md` 第九节；本文件保留关键证据和复现方式。

## 环境

- QEMU 10.2.1 源码编译，`buildtype=debug`、`optimization=0`、带 debug_info；
- 主机 gdb 17.1，guest gdb 为 `riscv64-unknown-elf-gdb` 10.1；
- 观测地址：`kern_init` 的虚拟地址 `0xffffffffc02000d8`；
- 启动页表：`boot_page_table_sv39`，物理地址 `0x80207000`，satp PPN = `0x80207`。

## 双 GDB 流程

```bash
# 终端 1：主机 gdb 调试 QEMU 源码
gdb --args /path/to/qemu-system-riscv64 \
    -machine virt -nographic -bios default \
    -kernel bin/ucore.img -s -S
(gdb) break get_physical_address
(gdb) break riscv_cpu_tlb_fill
(gdb) run

# 终端 2：RISC-V gdb 调试 guest
riscv64-unknown-elf-gdb
(gdb) file bin/kernel
(gdb) target remote localhost:1234
(gdb) continue
```

## 关键证据

### MMU 关闭（satp=0）

```
TLB MISS: address=0x80200000 access_type=2 mmu_idx=1
get_physical_address(addr=0x80200000)
    satp=0  (MODE=0, PPN=0)
```

`get_physical_address` 在 `VM_1_10_MBARE` 分支直接返回原地址。

### Sv39 翻译

```
TLB MISS: address=0xffffffffc02000d8 access_type=2 mmu_idx=1
get_physical_address(addr=0xffffffffc02000d8)
    satp=0x8000000000080207  (MODE=8, PPN=0x80207)
  [walk] i=0 ptshift=18 levels=3 ptidxbits=9 ptesize=8 vm=8 base=0x80207000
  [walk] idx=0x1ff  pte_addr=base+idx*8=0x80207ff8
  [walk] PTE loaded = 0x200000cf
  [walk] leaf at level 0: ppn=0x80000 ptshift=18
  [walk] PA = ((ppn=0x80000) | (vpn & 0x3ffff)) << 12 | off 0xd8
  [tlb] install: va=0xffffffffc02000d8 pa=0x802000d8 prot=7 size=0x1000
```

PTE `0x200000cf`：PPN = `0x80000`，标志位 `V|R|W|X|A|D`（U=0、G=0）。
因为根页表索引 511 是叶子，循环只执行 `i=0` 一次，`ptshift=18` 对应 1GiB 大页。

### TLB 命中

```
[tlb-lookup] va page=0xffffffffc0200000 tlb_entry=0xffffffffffffffff hit=0
[tlb-lookup] va page=0xffffffffc0200000 tlb_entry=0xffffffffc0200000 hit=1
...（连续 7 次 hit=1）
```

第一次 miss 填表后，同一页的后续访问不再进入 `riscv_cpu_tlb_fill`。

### 数据访问路径

```
DATA TLB MISS: addr=0xffffffffc0208018 access_type=1
#0 riscv_cpu_tlb_fill    cpu_helper.c:1764
#1 tlb_fill_align        cputlb.c:1255
#2 mmu_lookup1           cputlb.c:1656
#3 mmu_lookup            cputlb.c:1748
#4 do_st1_mmu            cputlb.c:2736
#5 helper_stb_mmu        ldst_common.c.inc:86
```

## 关键调用链

取指：

```
get_page_addr_code → probe_access_internal → tlb_fill_align
  → riscv_cpu_tlb_fill → get_physical_address → tlb_set_page
```

数据：

```
helper_stb_mmu → do_st1_mmu → mmu_lookup → mmu_lookup1
  → tlb_fill_align → riscv_cpu_tlb_fill → get_physical_address
```

## QEMU TLB 与真实 CPU TLB 的区别

| 维度 | 真实 CPU TLB | QEMU 模拟的 TLB |
|------|--------------|-----------------|
| 实现 | 硬件电路 | `accel/tcg/cputlb.c` 的软件结构 + TCG 内联查找 |
| 表项内容 | VPN→PPN + 权限 + ASID/VMID | 虚拟页地址 + host 地址偏移 + 软件标志 |
| 未命中 | 硬件 page table walker | 同步调用 `riscv_cpu_tlb_fill`，软件遍历 |
| 替换 | 硬件 LRU/随机 | 直接映射覆盖，另有 victim TLB |
| 刷新 | `sfence.vma` 由硬件执行 | `tlb_flush_*` C 函数 |

## 复现

在 `lab2` 分支根目录下：

```bash
cd code

# 主机 gdb（终端 1）
gdb -q -batch -x ../report/gdb_evidence/host_translation.gdb --args \
  /home/xisaka/qemu-build/src/qemu-10.2.1/build/qemu-system-riscv64 \
  -machine virt -nographic -bios default -kernel bin/ucore.img -s -S

# guest gdb（终端 2）
riscv64-unknown-elf-gdb -q -ex "file bin/kernel" \
  -ex "set pagination off" -ex "target remote localhost:1234" -ex "continue"

# 纯日志方式
qemu-system-riscv64 -machine virt -nographic -bios default \
  -kernel bin/ucore.img -d mmu -D mmu.log
```
