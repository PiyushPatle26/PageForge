# ---- PageForge, RISC-V (rv64) build ---------------------------------
#
# Builds for riscv64 (rv64gc, LP64D ABI) and runs everything under QEMU
# user-mode emulation. riscv64 is the only supported target.
#
#   make            cross-compile for riscv64
#   make run        build and run under qemu-riscv64
#   make test       build and run the Unity suite
#
# You need the cross toolchain:
#   sudo apt install gcc-riscv64-linux-gnu qemu-user qemu-user-static

CROSS_COMPILE ?= riscv64-linux-gnu-
QEMU          ?= qemu-riscv64
ARCH_CFLAGS   ?= -march=rv64gc -mabi=lp64d

CC     = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 $(ARCH_CFLAGS)

# The standalone binaries use no libc at all: raw ecall syscalls, our own
# _start, our own memset/memcpy. The Unity test build is the one exception,
# since Unity itself needs libc.
FREE_CFLAGS = -ffreestanding -fno-stack-protector -fno-builtin \
              -DPAGEFORGE_FREESTANDING
FREE_LDFLAGS = -nostdlib -static

# ---- Library sources. No main here, each binary brings its own. -----
LIB_SRCS = src/my_syscall.c \
           src/my_io.c      \
           src/my_paging.c  \
           src/my_buddy.c   \
           src/my_slab.c    \
           src/my_alloc.c

# ---- Phony targets --------------------------------------------------
.PHONY: all run qemu test demo clean toolchain-check

# Default: build the main demo binary (static so QEMU can run it)
all: toolchain-check pageforge

# Fail with something useful instead of "gcc: command not found"
toolchain-check:
	@if ! command -v $(CC) > /dev/null 2>&1; then \
		echo "ERROR: $(CC) not found."; \
		echo "  Install it:  sudo apt install gcc-riscv64-linux-gnu"; \
		exit 1; \
	fi

pageforge: $(LIB_SRCS) src/main.c
	$(CC) $(CFLAGS) $(FREE_CFLAGS) $(FREE_LDFLAGS) -o $@ $^
	@echo ""
	@echo "  Build OK ->  ./pageforge   (riscv64, no libc)"
	@echo "  Run:         make run"
	@echo "  Unit tests:  make test"

# ---- Standalone demo ------------------------------------------------
demo/pageforge_demo: $(LIB_SRCS) demo/demo.c
	$(CC) $(CFLAGS) $(FREE_CFLAGS) $(FREE_LDFLAGS) -o $@ $^

demo: toolchain-check demo/pageforge_demo
	@echo ""
	$(QEMU) ./demo/pageforge_demo

# ---- Unity test runner ----------------------------------------------
TEST_SRCS = tests/test_pageforge.c \
            tests/vendor/unity/unity.c \
            $(LIB_SRCS)

tests/run_tests: $(TEST_SRCS)
	$(CC) -Iinclude -Itests -Wall -Wextra $(ARCH_CFLAGS) -static -O0 -g -o $@ $^

test: toolchain-check tests/run_tests
	@echo ""
	$(QEMU) ./tests/run_tests

# ---- Run / QEMU -----------------------------------------------------
run: all
	@if [ -n "$(QEMU)" ] && ! command -v $(QEMU) > /dev/null 2>&1; then \
		echo "QEMU not found. Install: sudo apt install qemu-user qemu-user-static"; \
		exit 1; \
	fi
	$(QEMU) ./pageforge

qemu: run

# ---- Clean ----------------------------------------------------------
clean:
	rm -f pageforge demo/pageforge_demo tests/run_tests
