# Minimal test makefile for pre-commit hooks
.PHONY: test clean

CC ?= gcc
CFLAGS = -Wall -Wextra -I include

test: test_pmm test_slab
	@echo "Running kernel unit tests..."
	@./test/test_pmm_precommit
	@./test/test_slab_precommit
	@echo "All tests passed!"

test_pmm:
	@$(CC) $(CFLAGS) -o test/test_pmm_precommit test/test_pmm.c

test_slab:
	@$(CC) $(CFLAGS) -o test/test_slab_precommit test/test_slab.c

clean:
	@rm -f test/test_*_precommit