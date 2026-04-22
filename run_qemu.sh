#!/bin/bash
# run_qemu.sh: cross-build PageForge for riscv64 and run it under QEMU.
#
# QEMU user-mode emulates the RISC-V instruction set and passes Linux
# syscalls straight through to the host kernel. It is the easiest way to
# run an rv64 binary on any machine without a full OS image.
#
# Install the toolchain:
#   sudo apt install gcc-riscv64-linux-gnu
#   sudo apt install qemu-user qemu-user-static

set -e

echo "==> Cross-building for riscv64..."
make clean
make

file ./pageforge

echo ""
echo "==> Running under qemu-riscv64..."
make run
