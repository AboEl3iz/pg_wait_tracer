# tests/Makefile — build C unit tests for pg_wait_tracer
# Requires: main project already built (make in parent dir)

CC       ?= gcc
CFLAGS    = -g -O2 -Wall -I../src -I../include
BUILD     = ../build

TESTS     = test_wait_event test_cmdline test_event_writer test_event_reader \
            test_trace_v2 test_sampler test_bucket gen_test_traces \
            gen_bench_traces test_concurrent_rw

.PHONY: all clean

all: $(TESTS)

test_wait_event: test_wait_event.c $(BUILD)/wait_event.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^

test_cmdline: test_cmdline.c $(BUILD)/cmdline.o
	$(CC) $(CFLAGS) -o $@ $^

test_event_writer: test_event_writer.c $(BUILD)/event_writer.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4

test_event_reader: test_event_reader.c $(BUILD)/event_reader.o $(BUILD)/event_writer.o \
                   $(BUILD)/map_reader.o $(BUILD)/wait_event.o $(BUILD)/cJSON.o
	$(CC) $(CFLAGS) -o $@ $^ -llz4 -lbpf -lelf -lz

test_bucket: test_bucket.c
	$(CC) $(CFLAGS) -o $@ $^

# test_trace_v2: format round-trip (v2 typed blocks). Built against the
# server objects (-DPGWT_SERVER) so it needs no BPF skeleton — buildable on
# a box without bpftool and in CI's server-only jobs.
test_trace_v2: test_trace_v2.c $(BUILD)/server_event_writer.o \
               $(BUILD)/server_event_reader.o
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^ -llz4

# test_sampler: sampler core (process_vm_readv read + batch encode + per-pid
# fallback). Compiles sampler.c with -DPGWT_SERVER so only the BPF-free core
# is built — no skeleton, no bpftool, runnable in CI's server-only jobs.
test_sampler: test_sampler.c ../src/sampler.c
	$(CC) $(CFLAGS) -DPGWT_SERVER -o $@ $^

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
