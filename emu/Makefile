# SOPHIE emulator tier - Makefile (host build, spec section 10).
#
#   make        build lib + tests + bench
#   make test   build and run the full test suite
#   make bench  run C benchmark, then JS reference benchmark if node exists
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS = -Icore -Imach -Itests

CORE_SRC = core/arm26.c mach/mem.c
MACH_SRC = mach/memc.c mach/vidc.c mach/ioc.c mach/machine.c
LIB_SRC  = $(CORE_SRC) $(MACH_SRC)
LIB_OBJ  = $(LIB_SRC:.c=.o)

TESTS = tests/arm26_test tests/arm26_ext_test tests/mmio_test tests/machine_test

.PHONY: all test bench clean

all: $(TESTS) tests/bench tools/romboot

tests/arm26_test: tests/arm26_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/arm26_ext_test: tests/arm26_ext_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/mmio_test: tests/mmio_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/machine_test: tests/machine_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/bench: tests/bench.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tools/romboot: tools/romboot.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

-include $(LIB_OBJ:.o=.d) $(TESTS:=.d) tests/bench.d tools/romboot.d

test: $(TESTS)
	./tests/arm26_test
	./tests/arm26_ext_test
	./tests/mmio_test
	./tests/machine_test

bench: tests/bench
	./tests/bench 1000000
	@if command -v node >/dev/null 2>&1; then \
		echo "--- JS reference (emu/ref) ---"; \
		node ref/bench.mjs 1000000; \
		node ref/arm26.test.mjs; \
	else \
		echo "node not found; skipping JS reference benchmark"; \
	fi

clean:
	rm -f $(LIB_OBJ) $(LIB_OBJ:.o=.d) tests/*.o tests/*.d tools/*.o tools/*.d $(TESTS) tests/bench tools/romboot
