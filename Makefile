# pg_wait_tracer — Phase 1: CLI daemon
# Requires: clang, llvm-strip, bpftool, libbpf-dev, libelf-dev, zlib1g-dev

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
             $(SRC_DIR)/replay.c

USER_OBJS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(USER_SRCS))

.PHONY: all clean

all: $(TARGET)

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

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
	rm -f $(INC_DIR)/vmlinux.h $(INC_DIR)/pg_wait_tracer.skel.h
