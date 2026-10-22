# Lab2 分支任务调试笔记

完整分析见 `report.md` 第九节；本文件保留关键证据和复现方式。

## 环境

- QEMU 10.2.1 源码编译，`buildtype=debug`、`optimization=0`、带 debug_info；
- 主机 gdb 17.1，guest gdb 为 `riscv64-unknown-elf-gdb` 10.1；
- 观测地址：`kern_init` 的虚拟地址 `0xffffffffc02000d8`；
- 启动页表：`boot_page_table_sv39`，物理地址 `0x80207000`，satp PPN = `0x80207`。

## 双 GDB 流程

```text
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

## 复现（2026-10-23 修订）

上面的地址、日志和截图来自 2026-10-21 的 QEMU 10.2.1 调试；重新编译后地址可能变化。
两份 `.gdb` 脚本的源码行号针对 **QEMU 10.2.1 原版源码**，不能直接用于 4.1.1。
主机需要普通 GDB、带调试符号的 QEMU，以及 RISC-V 工具链。

先在分支的 `code` 目录构建内核。下面的 `QEMU_SRC`、`QEMU_DEBUG` 都要替换为本机路径：

```bash
cd code
make
QEMU_SRC=/absolute/path/to/qemu-10.2.1
QEMU_DEBUG="$QEMU_SRC/build/qemu-system-riscv64"
KERNEL_VA=$(riscv64-unknown-elf-nm -n bin/kernel | awk '$3 == "kern_init" {print "0x" $1}')
DATA_VA=$(riscv64-unknown-elf-nm -n bin/kernel | awk '$3 == "edata" {print "0x" $1}')
test -n "$KERNEL_VA" && test -n "$DATA_VA" || exit 1
printf 'kern_init=%s, edata=%s\n' "$KERNEL_VA" "$DATA_VA"

# 终端 1：观察取指。源码搜索路径用于在移动源码目录后解析相对文件名。
gdb -q -batch \
  -ex "directory $QEMU_SRC" \
  -ex "set substitute-path /home/xisaka/qemu-build/src/qemu-10.2.1 $QEMU_SRC" \
  -ex "set \$kernel_va = (unsigned long long)$KERNEL_VA" \
  -x ../report/gdb_evidence/host_translation.gdb --args \
  "$QEMU_DEBUG" -machine virt -nographic -bios default \
  -kernel bin/ucore.img -s -S
```

`set substitute-path` 的第一个路径是旧二进制记录的源码路径；如果使用自己重新编译的
QEMU 且源码未移动，可以省略。若调试信息记录了其他旧路径，应以该路径替换。
运行前用 `info sources` 或 `info line target/riscv/cpu_helper.c:1325` 确认源码能找到。

另开终端 2，同样进入 `code` 目录：

```bash
riscv64-unknown-elf-gdb -q -ex "file bin/kernel" \
  -ex "set pagination off" -ex "target remote localhost:1234" -ex "continue"
```

观察数据访问时，结束上一组调试，重新启动终端 1；保留上面的路径和地址变量，改用：

```bash
gdb -q -batch \
  -ex "directory $QEMU_SRC" \
  -ex "set substitute-path /home/xisaka/qemu-build/src/qemu-10.2.1 $QEMU_SRC" \
  -ex "set \$data_va = (unsigned long long)$DATA_VA" \
  -x ../report/gdb_evidence/host_data_path.gdb --args \
  "$QEMU_DEBUG" -machine virt -nographic -bios default \
  -kernel bin/ucore.img -s -S
```

再运行终端 2 的连接命令。`edata` 是此内核启动时 `memset(edata, 0, end-edata)`
使用的起点；观察其他指令时应改成实际访存地址。不能沿用旧日志中的地址。
GDB 条件断点可能很慢，结束调试时同时退出 guest GDB 和主机 GDB/QEMU。

只需要确认地址转换结果时，可以用日志方式：

```bash
qemu-system-riscv64 -machine virt -nographic -bios default \
  -kernel bin/ucore.img -d mmu -D mmu.log
```

## QEMU 4.1.1 与 10.2.1 的源码位置对照

4.1.1 的位置已根据原版源码重新核对；10.2.1 的位置对应本次原有调试日志。
两版共享“TLB 查找 → 未命中填充 → 页表遍历 → 安装翻译”的逻辑，但中间函数并不相同。

| 操作 | QEMU 4.1.1 | QEMU 10.2.1 |
|---|---|---|
| 取指的 C 路径 TLB 命中检查 | `accel/tcg/cputlb.c:1023` 的 `get_page_addr_code`，1030 行 `tlb_hit` | `get_page_addr_code_hostp` → `probe_access_internal`，`cputlb.c:1373` |
| 数据加载慢路径的 TLB 检查 | `load_helper`，`cputlb.c:1248`；1271 行 `tlb_hit` | `mmu_lookup` → `mmu_lookup1` |
| 数据存储慢路径的 TLB 检查 | `store_helper`，`cputlb.c:1501`；1519 行 `tlb_hit` | `do_st1_mmu` → `mmu_lookup` → `mmu_lookup1`；1653 行检查 |
| 架构相关填充入口 | `target/riscv/cpu_helper.c:435`，`riscv_cpu_tlb_fill` | 同名函数，`cpu_helper.c:1761` |
| 地址翻译函数 | `get_physical_address`，`cpu_helper.c:155` | 同名函数，`cpu_helper.c:1187` |
| Bare 模式分支 | `cpu_helper.c:196` | `cpu_helper.c:1267–1282` |
| 页表循环 / 取 PTE 地址 | `cpu_helper.c:237 / 242` | `cpu_helper.c:1325 / 1356` |
| 安装翻译 | `cpu_helper.c:472` 的 `tlb_set_page` | `cpu_helper.c:1876` |

实际访存命中的快路径还可能由 TCG 生成的主机机器码直接执行；表中的 C helper 是
可观察的相应路径，不能理解成每条命中访存都调用一次 C 函数。

使用 4.1.1 时先在这些位置手工设置断点，并按该版本的局部变量调整输出命令；
`first_stage`、`tlb_size` 等 10.2.1 局部变量不能直接搬用。本次修订只核对了 4.1.1
源码位置和脚本参数化方式，没有重新执行协作者的 10.2.1 双 GDB 会话。
