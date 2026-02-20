# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

TESTS     = test_wait_event test_cmdline

.PHONY: all clean

all: $(TESTS)

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o
	$(CC) $(CFLAGS) -o $@ $^

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f $(TESTS)
