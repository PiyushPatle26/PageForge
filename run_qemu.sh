#!/bin/bash
# run_qemu.sh — build PageForge and run it inside QEMU user-mode.
#
# QEMU user-mode emulates just the CPU instruction set and forwards
# Linux syscalls to the host kernel. It's the simplest way to run
# a Linux binary on QEMU without needing a full OS image.
#
# Install QEMU user-mode:
#   sudo apt install qemu-user        (dynamic)
#   sudo apt install qemu-user-static (static, works cross-arch)

set -e

echo "==> Building..."
make clean
make

echo ""
echo "==> Running in QEMU user-mode..."
make qemu
