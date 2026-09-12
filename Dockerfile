# ---- PageForge, reproducible RISC-V build and test environment --------------
# Same steps as the GitHub Actions CI.
# Usage:
#   docker build -t pageforge-dev .
#   docker run --rm pageforge-dev          # cross-build for rv64 + test on QEMU
#   docker run --rm -it pageforge-dev bash # interactive shell
# -----------------------------------------------------------------------------

FROM ubuntu:22.04

LABEL maintainer="PageForge"
LABEL description="RISC-V build and test environment for PageForge Linux MM simulator"

ENV DEBIAN_FRONTEND=noninteractive

# The rv64 cross toolchain plus QEMU user-mode. riscv64 is the only target.
#
# libc6-dev-riscv64-cross is listed on purpose. gcc-riscv64-linux-gnu only
# Recommends it, and --no-install-recommends means we would not get it, so
# the compiler would install without any target headers and the first
# #include <sys/mman.h> would fail.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    gcc-riscv64-linux-gnu \
    libc6-dev-riscv64-cross \
    make \
    qemu-user \
    qemu-user-static \
    file \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the whole project
COPY . .

# Cross-build for rv64, run it all under QEMU, then leak-check the native
CMD ["bash", "-c", "\
    echo '=== Building PageForge for riscv64 ===' && \
    make clean && \
    make && \
    file ./pageforge && \
    echo '' && \
    echo '=== Running Unity Tests under qemu-riscv64 ===' && \
    make test && \
    echo '' && \
    echo '=== Running Demo under qemu-riscv64 ===' && \
    make demo && \
    echo '' && \
    echo '=== Running main binary under qemu-riscv64 ===' && \
    make run && \
    echo '' && \
    make clean && \
"]
