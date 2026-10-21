set pagination off
set confirm off
set breakpoint pending on
set $h = 0

break riscv_cpu_tlb_fill if address == 0xffffffffc0208018
commands
  silent
  printf "\n=== DATA TLB MISS: addr=%#lx access_type=%d mmu_idx=%d ===\n", address, access_type, mmu_idx
  bt 8
  continue
end

break get_physical_address if addr == 0xffffffffc0208018
commands
  silent
  printf "=== DATA get_physical_address(addr=%#lx) satp=%#lx access_type=%d ===\n", addr, env->satp, access_type
  continue
end

break /home/xisaka/qemu-build/src/qemu-10.2.1/accel/tcg/cputlb.c:1653 if (addr & ~0xfff) == 0xffffffffc0208000
commands
  silent
  set $h = $h + 1
  if $h <= 10
    printf "  [data tlb] addr=%#lx entry=%#lx hit=%d\n", addr, tlb_addr, ((addr & ~0xfff) == (tlb_addr & 0xfffffffffffff001))
  end
  continue
end

run
