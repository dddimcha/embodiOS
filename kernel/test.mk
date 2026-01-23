# Kernel test makefile - QEMU-based in-kernel testing
# This runs tests in the actual kernel environment via QEMU
.PHONY: test clean legacy-test

# Variables
CC ?= gcc
CFLAGS = -Wall -Wextra -I include

# New QEMU-based test target (default)
# Runs tests in actual kernel ring 0 context
# Usage: make -f test.mk test           - runs all tests
#        make -f test.mk test TESTNAME=foo - runs single test
test:
	@echo "Running kernel unit tests in QEMU..."
	@cd .. && bash scripts/run_kernel_tests.sh $(TESTNAME)

# Legacy userspace test targets (deprecated - to be removed after migration)
# These are kept temporarily for pre-commit hooks until migration is complete
legacy-test: test_pmm test_slab
	@echo "Running legacy userspace tests..."
	@./test/test_pmm_precommit
	@./test/test_slab_precommit
	@echo "All legacy tests passed!"

test_pmm:
	@$(CC) $(CFLAGS) -o test/test_pmm_precommit test/test_pmm.c

test_slab:
	@$(CC) $(CFLAGS) -o test/test_slab_precommit test/test_slab.c

clean:
	@rm -f test/test_*_precommit