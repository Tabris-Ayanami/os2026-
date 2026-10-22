# QEMU 10.2.1 source lines only. Set $data_va from the current kernel edata
# before sourcing; see ../gdb_notes.md for commands and source-path mapping.
set $data_page = (unsigned long long)$data_va & 0xfffffffffffff000
set pagination off
set confirm off
set breakpoint pending on
set $h = 0

break riscv_cpu_tlb_fill if address == $data_va
commands
  silent
  printf "\n=== DATA TLB MISS: addr=%#lx access_type=%d mmu_idx=%d ===\n", address, access_type, mmu_idx
  bt 8
  continue
end

break get_physical_address if addr == $data_va
commands
  silent
  printf "=== DATA get_physical_address(addr=%#lx) satp=%#lx access_type=%d ===\n", addr, env->satp, access_type
  continue
end

break accel/tcg/cputlb.c:1653 if (addr & ~0xfff) == $data_page
commands
  silent
  set $h = $h + 1
  if $h <= 10
    printf "  [data tlb] addr=%#lx entry=%#lx hit=%d\n", addr, tlb_addr, ((addr & ~0xfff) == (tlb_addr & 0xfffffffffffff001))
  end
  continue
end

run
