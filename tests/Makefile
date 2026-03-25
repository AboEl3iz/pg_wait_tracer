# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

TESTS     = test_wait_event test_cmdline test_event_writer test_event_reader \
            test_bucket gen_test_traces gen_bench_traces test_concurrent_rw

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

test_bucket: test_bucket.c
	$(CC) $(CFLAGS) -o $@ $^

# Server-build objects for gen_test_traces
SERVER_OBJS = $(BUILD)/server_event_writer.o $(BUILD)/server_summary_writer.o \
              $(BUILD)/server_wait_event.o $(BUILD)/server_cJSON.o

gen_test_traces: gen_test_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

gen_bench_traces: gen_bench_traces.c $(SERVER_OBJS)
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

test_concurrent_rw: test_concurrent_rw.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                    $(BUILD)/map_reader.o $(BUILD)/wait_event.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 -lbpf -lelf -lz

clean:
	rm -f $(TESTS)
