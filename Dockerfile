# ── PageForge — Reproducible build & test environment ─────────────────────────
# Matches the GitHub Actions CI exactly.
# Usage:
#   docker build -t pageforge-dev .
#   docker run --rm pageforge-dev          # build + test
#   docker run --rm -it pageforge-dev bash # interactive shell
# ──────────────────────────────────────────────────────────────────────────────

FROM ubuntu:22.04

LABEL maintainer="PageForge"
LABEL description="Build and test environment for PageForge Linux MM simulator"

ENV DEBIAN_FRONTEND=noninteractive

# Install all required tools
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    make \
    valgrind \
    qemu-user \
    qemu-user-static \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the whole project
COPY . .

# Default: build everything, run tests, run demo
CMD ["bash", "-c", "\
    echo '=== Building PageForge ===' && \
    make clean && \
    make && \
    echo '' && \
    echo '=== Running Unit Tests (Unity) ===' && \
    make test && \
    echo '' && \
    echo '=== Running Demo ===' && \
    make demo && \
    echo '' && \
    echo '=== Running under QEMU user-mode ===' && \
    qemu-x86_64-static ./pageforge \
"]
