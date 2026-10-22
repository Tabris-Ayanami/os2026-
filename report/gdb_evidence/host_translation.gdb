# QEMU 10.2.1 source lines only. Set $kernel_va from the current bin/kernel
# before sourcing; see ../gdb_notes.md for commands and source-path mapping.
set $kernel_page = (unsigned long long)$kernel_va & 0xfffffffffffff000
set pagination off
set confirm off
set breakpoint pending on
set $hits = 0

# --- TLB miss call path (instruction fetch) ---
break riscv_cpu_tlb_fill if address == 0x80200000 || address == $kernel_va
commands
  silent
  printf "\n=== TLB MISS: address=%#lx access_type=%d mmu_idx=%d ===\n", address, access_type, mmu_idx
  bt 5
  continue
end

# --- translation entry ---
break get_physical_address if addr == 0x80200000 || addr == $kernel_va
commands
  silent
  printf "\n=== get_physical_address(addr=%#lx) access_type=%d first_stage=%d ===\n", addr, access_type, first_stage
  printf "    satp=%#lx  (MODE=%d, PPN=%#lx)\n", env->satp, (int)(env->satp >> 60), (unsigned long)(env->satp & 0xfffffffffffUL)
  continue
end

# --- page table walk: level loop, PTE address, PTE value, leaf, final PA ---
break target/riscv/cpu_helper.c:1325 if addr == $kernel_va
commands
  silent
  printf "  [walk] i=%d ptshift=%d levels=%d ptidxbits=%d ptesize=%d vm=%d base=%#lx\n", i, ptshift, levels, ptidxbits, ptesize, vm, base
  continue
end

break target/riscv/cpu_helper.c:1356 if addr == $kernel_va
commands
  silent
  printf "  [walk] idx=%#lx  pte_addr=base+idx*8=%#lx\n", idx, base + idx * ptesize
  continue
end

break target/riscv/cpu_helper.c:1373 if addr == $kernel_va
commands
  silent
  printf "  [walk] PTE loaded = %#lx\n", pte
  continue
end

break target/riscv/cpu_helper.c:1432 if addr == $kernel_va
commands
  silent
  printf "  [walk] leaf at level %d: ppn=%#lx ptshift=%d\n", i, ppn, ptshift
  continue
end

break target/riscv/cpu_helper.c:1600 if addr == $kernel_va
commands
  silent
  printf "  [walk] PA = ((ppn=%#lx) | (vpn & %#lx)) << 12 | off %#lx\n", ppn, ((unsigned long)1 << ptshift) - 1, addr & 0xfff
  continue
end

# --- TLB entry installation ---
break target/riscv/cpu_helper.c:1876 if address == $kernel_va
commands
  silent
  printf "  [tlb] install: va=%#lx pa=%#lx prot=%d size=%#lx\n", address, pa, prot, tlb_size
  continue
end

# --- TLB hit evidence: same page after the fill should not call tlb_fill ---
break accel/tcg/cputlb.c:1373 if page_addr == $kernel_page
commands
  silent
  set $hits = $hits + 1
  if $hits <= 8
    printf "  [tlb-lookup] va page=%#lx tlb_entry=%#lx hit=%d\n", page_addr, tlb_addr, page_addr == (tlb_addr & 0xfffffffffffff001)
  end
  continue
end

run
