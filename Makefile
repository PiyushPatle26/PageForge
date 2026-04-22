CC     = gcc
CFLAGS = -Wall -Wextra -Iinclude -O2

# ── Library sources (no main — every binary supplies its own) ────────
LIB_SRCS = src/my_syscall.c \
           src/my_io.c      \
           src/my_paging.c  \
           src/my_buddy.c   \
           src/my_slab.c    \
           src/my_alloc.c

# ── Phony targets ────────────────────────────────────────────────────
.PHONY: all run qemu test demo clean

# Default: build the main demo binary (static so QEMU can run it)
all: pageforge

pageforge: $(LIB_SRCS) src/main.c
	$(CC) $(CFLAGS) -static -o $@ $^
	@echo ""
	@echo "  Build OK ->  ./pageforge"
	@echo "  Run native:  make run"
	@echo "  Run QEMU:    make qemu"
	@echo "  Unit tests:  make test"

# ── Standalone demo ──────────────────────────────────────────────────
demo/pageforge_demo: $(LIB_SRCS) demo/demo.c
	$(CC) $(CFLAGS) -o $@ $^

demo: demo/pageforge_demo
	@echo ""
	./demo/pageforge_demo

# ── Unity test runner ────────────────────────────────────────────────
TEST_SRCS = tests/test_pageforge.c \
            tests/vendor/unity/unity.c \
            $(LIB_SRCS)

tests/run_tests: $(TEST_SRCS)
	$(CC) -Iinclude -Itests -Wall -Wextra -O0 -g -o $@ $^

test: tests/run_tests
	@echo ""
	./tests/run_tests

# ── Run / QEMU ───────────────────────────────────────────────────────
run: pageforge
	./pageforge

qemu: pageforge
	@if command -v qemu-x86_64 > /dev/null 2>&1; then \
		qemu-x86_64 ./pageforge; \
	elif command -v qemu-x86_64-static > /dev/null 2>&1; then \
		qemu-x86_64-static ./pageforge; \
	else \
		echo "QEMU not found — install: sudo apt install qemu-user"; \
		./pageforge; \
	fi

# ── Clean ────────────────────────────────────────────────────────────
clean:
	rm -f pageforge demo/pageforge_demo tests/run_tests
