# pg_wait_tracer
# Requires: clang, llvm-strip, bpftool, libbpf-dev, libelf-dev, zlib1g-dev, liblz4-dev

CLANG      ?= clang
LLVM_STRIP ?= llvm-strip
BPFTOOL    ?= bpftool
CC         ?= gcc

SRC_DIR    = src
BPF_DIR    = src/bpf
INC_DIR    = include
BUILD_DIR  = build
TARGET     = pg_wait_tracer

ARCH       := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

BPF_CFLAGS = -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH) \
             -I$(INC_DIR) -I$(SRC_DIR)

CFLAGS     = -g -O2 -Wall -Wextra -Wno-unused-parameter \
             -I$(INC_DIR) -I$(SRC_DIR)
LDFLAGS    = -lbpf -lelf -lz -llz4

USER_SRCS  = $(SRC_DIR)/pg_wait_tracer.c \
             $(SRC_DIR)/daemon.c \
             $(SRC_DIR)/backend.c \
             $(SRC_DIR)/discovery.c \
             $(SRC_DIR)/map_reader.c \
             $(SRC_DIR)/event_stream.c \
             $(SRC_DIR)/output.c \
             $(SRC_DIR)/wait_event.c \
             $(SRC_DIR)/perf_event.c \
             $(SRC_DIR)/cmdline.c \
             $(SRC_DIR)/snapshot.c \
             $(SRC_DIR)/event_writer.c \
             $(SRC_DIR)/event_reader.c \
             $(SRC_DIR)/replay.c \
             $(SRC_DIR)/summary_writer.c \
             $(SRC_DIR)/query_text.c

USER_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(USER_SRCS))

# pgwt-server: lightweight replay server (no BPF dependencies)
SERVER_SRCS = $(SRC_DIR)/server.c \
              $(SRC_DIR)/compute.c \
              $(SRC_DIR)/event_reader.c \
              $(SRC_DIR)/event_writer.c \
              $(SRC_DIR)/summary_writer.c \
              $(SRC_DIR)/summary_reader.c \
              $(SRC_DIR)/wait_event.c
SERVER_OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/server_%.o,$(SERVER_SRCS))
SERVER_LDFLAGS = -lz -llz4

.PHONY: all clean

all: $(TARGET) pgwt-server

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Step 1: Generate vmlinux.h from kernel BTF
$(INC_DIR)/vmlinux.h:
	@echo "  VMLINUX  $@"
	@$(BPFTOOL) btf dump file /sys/kernel/btf/vmlinux format c > $@

# Step 2: Compile BPF program
$(BUILD_DIR)/pg_wait_tracer.bpf.o: $(BPF_DIR)/pg_wait_tracer.bpf.c \
                                    $(INC_DIR)/vmlinux.h \
                                    $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  BPF      $@"
	@$(CLANG) $(BPF_CFLAGS) -c $< -o $@
	@$(LLVM_STRIP) -g $@

# Step 3: Generate BPF skeleton
$(INC_DIR)/pg_wait_tracer.skel.h: $(BUILD_DIR)/pg_wait_tracer.bpf.o
	@echo "  SKEL     $@"
	@$(BPFTOOL) gen skeleton $< > $@

# Step 4: Compile userspace C (all depend on skeleton + shared header)
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(INC_DIR)/pg_wait_tracer.skel.h \
                  $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@"
	@$(CC) $(CFLAGS) -c $< -o $@

# Step 5: Link
$(TARGET): $(USER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "  DONE     $@"

# pgwt-server: compile WITHOUT skeleton dependency, with -DPGWT_SERVER
$(BUILD_DIR)/server_%.o: $(SRC_DIR)/%.c $(SRC_DIR)/pg_wait_tracer.h | $(BUILD_DIR)
	@echo "  CC       $@ (server)"
	@$(CC) $(CFLAGS) -DPGWT_SERVER -c $< -o $@

pgwt-server: $(SERVER_OBJS)
	@echo "  LINK     $@"
	@$(CC) $(CFLAGS) $^ -o $@ $(SERVER_LDFLAGS)
	@echo "  DONE     $@"

clean:
	rm -rf $(BUILD_DIR) $(TARGET) pgwt-server
	rm -f $(INC_DIR)/vmlinux.h $(INC_DIR)/pg_wait_tracer.skel.h
