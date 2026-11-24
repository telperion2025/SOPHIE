# SOPHIE - top-level Makefile.
# Host build (spec section 10): plain make, C11, no heavier build system.
#
#   make          build the emulator library, tests and benchmark
#   make test     build and run the full test suite
#   make bench    run the C benchmark (and the JS reference bench if node exists)
#   make clean

CC      ?= cc
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra
SUBDIRS  = emu

.PHONY: all test bench host-x11 clean $(SUBDIRS)

all: $(SUBDIRS)

$(SUBDIRS):
	$(MAKE) -C $@ CC="$(CC)" CFLAGS="$(CFLAGS)"

test:
	$(MAKE) -C emu CC="$(CC)" CFLAGS="$(CFLAGS)" test

bench:
	$(MAKE) -C emu CC="$(CC)" CFLAGS="$(CFLAGS)" bench

host-x11:
	$(MAKE) -C emu CC="$(CC)" CFLAGS="$(CFLAGS)" host-x11

clean:
	$(MAKE) -C emu clean
