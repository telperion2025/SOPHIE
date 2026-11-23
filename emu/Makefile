# SOPHIE emulator tier - Makefile (host build, spec section 10).
#
#   make        build lib + tests + bench + sophie frontend (headless host layer)
#   make test   build and run the full test suite
#   make bench  run C benchmark, then JS reference benchmark if node exists
#   make host-x11  additionally build tools/sophie with the X11 window backend
#                  (libX11 via dlopen; needs libX11 at runtime, not build time)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
CPPFLAGS = -Icore -Imach -Ihost -Itests

CORE_SRC = core/arm26.c mach/mem.c
MACH_SRC = mach/memc.c mach/vidc.c mach/ioc.c mach/machine.c
HOST_SRC = host/host.c host/host_headless.c host/keymap.c
LIB_SRC  = $(CORE_SRC) $(MACH_SRC) $(HOST_SRC)
LIB_OBJ  = $(LIB_SRC:.c=.o)

TESTS = tests/arm26_test tests/arm26_ext_test tests/mmio_test tests/machine_test tests/host_test

.PHONY: all test bench clean host-x11

all: $(TESTS) tests/bench tools/romboot tools/sophie

host-x11: tools/sophie-x11

tests/arm26_test: tests/arm26_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/arm26_ext_test: tests/arm26_ext_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/mmio_test: tests/mmio_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/machine_test: tests/machine_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/host_test: tests/host_test.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tests/bench: tests/bench.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tools/romboot: tools/romboot.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

tools/sophie: tools/sophie.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

# X11 window backend variant (dlopen'd libX11; runtime-only dependency)
host/host_x11wrap.o: host/host.c
	$(CC) $(CFLAGS) -DSOPHIE_HOST_X11 $(CPPFLAGS) -MMD -MP -c -o $@ $<

host/host_x11.o: host/host_x11.c
	$(CC) $(CFLAGS) -DSOPHIE_HOST_X11 $(CPPFLAGS) -MMD -MP -c -o $@ $<

tools/sophie-x11: tools/sophie.o $(CORE_SRC:.c=.o) $(MACH_SRC:.c=.o) \
                  host/host_x11wrap.o host/host_headless.o host/keymap.o host/host_x11.o
	$(CC) $(CFLAGS) -o $@ $^ -ldl

%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

-include $(LIB_OBJ:.o=.d) $(TESTS:=.d) tests/bench.d tools/romboot.d tools/sophie.d

test: $(TESTS)
	./tests/arm26_test
	./tests/arm26_ext_test
	./tests/mmio_test
	./tests/machine_test
	./tests/host_test

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
	rm -f $(LIB_OBJ) $(LIB_OBJ:.o=.d) host/*.o host/*.d tests/*.o tests/*.d tools/*.o tools/*.d \
	      $(TESTS) tests/bench tools/romboot tools/sophie tools/sophie-x11
