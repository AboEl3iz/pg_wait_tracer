# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

TESTS     = test_wait_event test_cmdline test_event_writer test_event_reader

.PHONY: all clean

all: $(TESTS)

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o
	$(CC) $(CFLAGS) -o $@ $^

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

test_event_writer: test_event_writer.c $(BUILD)/event_writer.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4

test_event_reader: test_event_reader.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                   $(BUILD)/map_reader.o $(BUILD)/wait_event.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 -lbpf -lelf -lz

clean:
	rm -f $(TESTS)
