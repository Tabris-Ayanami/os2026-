set pagination off
set confirm off
set breakpoint pending on
set $hits = 0

# --- TLB miss call path (instruction fetch) ---
break riscv_cpu_tlb_fill if address == 0x80200000 || address == 0xffffffffc02000d8
commands
  silent
  printf "\n=== TLB MISS: address=%#lx access_type=%d mmu_idx=%d ===\n", address, access_type, mmu_idx
  bt 5
  continue
end

# --- translation entry ---
break get_physical_address if addr == 0x80200000 || addr == 0xffffffffc02000d8
commands
  silent
  printf "\n=== get_physical_address(addr=%#lx) access_type=%d first_stage=%d ===\n", addr, access_type, first_stage
  printf "    satp=%#lx  (MODE=%d, PPN=%#lx)\n", env->satp, (int)(env->satp >> 60), (unsigned long)(env->satp & 0xfffffffffffUL)
  continue
end

# --- page table walk: level loop, PTE address, PTE value, leaf, final PA ---
break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1325 if addr == 0xffffffffc02000d8
commands
  silent
  printf "  [walk] i=%d ptshift=%d levels=%d ptidxbits=%d ptesize=%d vm=%d base=%#lx\n", i, ptshift, levels, ptidxbits, ptesize, vm, base
  continue
end

break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1356 if addr == 0xffffffffc02000d8
commands
  silent
  printf "  [walk] idx=%#lx  pte_addr=base+idx*8=%#lx\n", idx, base + idx * ptesize
  continue
end

break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1373 if addr == 0xffffffffc02000d8
commands
  silent
  printf "  [walk] PTE loaded = %#lx\n", pte
  continue
end

break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1432 if addr == 0xffffffffc02000d8
commands
  silent
  printf "  [walk] leaf at level %d: ppn=%#lx ptshift=%d\n", i, ppn, ptshift
  continue
end

break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1600 if addr == 0xffffffffc02000d8
commands
  silent
  printf "  [walk] PA = ((ppn=%#lx) | (vpn & %#lx)) << 12 | off %#lx\n", ppn, ((unsigned long)1 << ptshift) - 1, addr & 0xfff
  continue
end

# --- TLB entry installation ---
break /home/xisaka/qemu-build/src/qemu-10.2.1/target/riscv/cpu_helper.c:1876 if address == 0xffffffffc02000d8
commands
  silent
  printf "  [tlb] install: va=%#lx pa=%#lx prot=%d size=%#lx\n", address, pa, prot, tlb_size
  continue
end

# --- TLB hit evidence: same page after the fill should not call tlb_fill ---
break /home/xisaka/qemu-build/src/qemu-10.2.1/accel/tcg/cputlb.c:1373 if page_addr == 0xffffffffc0200000
commands
  silent
  set $hits = $hits + 1
  if $hits <= 8
    printf "  [tlb-lookup] va page=%#lx tlb_entry=%#lx hit=%d\n", page_addr, tlb_addr, page_addr == (tlb_addr & 0xfffffffffffff001)
  end
  continue
end

run
