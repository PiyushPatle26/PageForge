# ---- PageForge, RISC-V (rv64) build ---------------------------------
#
# Builds for riscv64 (rv64gc, LP64D ABI) and runs everything under QEMU
# user-mode emulation. Set ARCH=host if you want a native binary, which
# is what valgrind needs since it has no riscv64 support.
#
#   make            cross-compile for riscv64
#   make run        build and run under qemu-riscv64
#   make test       build and run the Unity suite
#   make ARCH=host  build natively for this machine
#
# You need the cross toolchain:
#   sudo apt install gcc-riscv64-linux-gnu qemu-user qemu-user-static

ARCH ?= riscv64

ifeq ($(ARCH),riscv64)
  CROSS_COMPILE ?= riscv64-linux-gnu-
  QEMU          ?= qemu-riscv64
  ARCH_CFLAGS   ?= -march=rv64gc -mabi=lp64d
  # QEMU user-mode has no rv64 dynamic linker, so link everything static
  DEMO_LDFLAGS  ?= -static
else
  CROSS_COMPILE ?=
  QEMU          ?=
  ARCH_CFLAGS   ?=
  # Link the host demo dynamically. Statically linked glibc startup code
  # makes valgrind report false positives that bury the real ones.
  DEMO_LDFLAGS  ?=
endif

CC     = $(CROSS_COMPILE)gcc
CFLAGS = -Wall -Wextra -Iinclude -O2 $(ARCH_CFLAGS)

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
		echo "  Or build natively:  make ARCH=host"; \
		exit 1; \
	fi

pageforge: $(LIB_SRCS) src/main.c
	$(CC) $(CFLAGS) -static -o $@ $^
	@echo ""
	@echo "  Build OK ->  ./pageforge   ($(ARCH))"
	@echo "  Run:         make run"
	@echo "  Unit tests:  make test"

# ---- Standalone demo ------------------------------------------------
demo/pageforge_demo: $(LIB_SRCS) demo/demo.c
	$(CC) $(CFLAGS) $(DEMO_LDFLAGS) -o $@ $^

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
# With ARCH=host, QEMU is empty and the binary just runs.
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
