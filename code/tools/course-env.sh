#!/usr/bin/env bash
# Source this file in each terminal used for the course experiment.
export OS_LAB_QEMU=/home/asus/.local/opt/qemu-4.1.1/bin/qemu-system-riscv64
export PATH="/home/asus/.local/opt/qemu-4.1.1/bin:/home/asus/toolchains/riscv-elf-toolchains/bin:$PATH"
export LD_LIBRARY_PATH="/home/asus/toolchains/riscv-elf-toolchains/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
