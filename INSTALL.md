# pg_wait_tracer — Installation Guide

## Supported Platforms

- **Linux kernel** >= 5.8 (BPF ring buffer, CO-RE)
- **Architecture**: x86_64, aarch64
- **PostgreSQL**: 14, 15, 16, 17, 18
- **Tested on**: Ubuntu 22.04/24.04, Oracle Linux 9, RHEL 9

## Prerequisites

### 1. Build Dependencies

#### Oracle Linux 9 / RHEL 9 / AlmaLinux 9

```bash
# Enable CRB (CodeReady Builder) repo for development packages
sudo dnf config-manager --set-enabled ol9_codeready_builder  # Oracle Linux
# sudo dnf config-manager --set-enabled crb                  # RHEL/AlmaLinux

# Core build tools
sudo dnf install -y gcc clang llvm make

# BPF toolchain
sudo dnf install -y bpftool libbpf-devel

# ELF and compression libraries
sudo dnf install -y elfutils-libelf-devel zlib-devel

# For DWARF-based st_query_id offset detection (optional, for query_event view)
sudo dnf install -y elfutils    # provides readelf
```

#### Ubuntu 22.04 / 24.04 / Debian 12

```bash
# Core build tools
sudo apt install -y gcc clang llvm make

# BPF toolchain
sudo apt install -y linux-tools-common linux-tools-$(uname -r) libbpf-dev

# ELF and compression libraries
sudo apt install -y libelf-dev zlib1g-dev

# For DWARF-based st_query_id offset detection (optional)
sudo apt install -y binutils    # provides readelf
```

### 2. Kernel BTF Support

The kernel must expose BTF (BPF Type Format) data. Verify:

```bash
ls /sys/kernel/btf/vmlinux
```

If missing, install the BTF-enabled kernel:
- **Oracle Linux 9**: `sudo dnf install -y kernel-uek` (UEK7+ has BTF)
- **Ubuntu**: Enabled by default on 22.04+
- **RHEL 9**: Enabled by default

### 3. PostgreSQL

#### Oracle Linux 9

```bash
# Install PostgreSQL repo
sudo dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-9-x86_64/pgdg-redhat-repo-latest.noarch.rpm

# Install PostgreSQL 16 (or 17, 18)
sudo dnf install -y postgresql16-server postgresql16-contrib

# Initialize and start
sudo /usr/pgsql-16/bin/postgresql-16-setup initdb
sudo systemctl enable --now postgresql-16

# (Optional) Debug symbols for query_event view
sudo dnf install -y postgresql16-debuginfo
```

#### Ubuntu / Debian

```bash
# Install PostgreSQL 16 (or 17, 18)
sudo apt install -y postgresql-16 postgresql-contrib-16

# (Optional) Debug symbols for query_event view
sudo apt install -y postgresql-16-dbgsym
```

### 4. Test Dependencies (optional)

```bash
# Python 3 (for integration tests)
# Oracle Linux 9:
sudo dnf install -y python3

# Ubuntu:
sudo apt install -y python3

# pgbench (included in postgresql-contrib)
# pg_wait_sampling extension (for cross-validation tests, optional)
```

## Build

```bash
cd pg_wait_tracer
make clean
make
```

This produces the `pg_wait_tracer` binary in the project root.

Build steps performed automatically:
1. Generate `vmlinux.h` from kernel BTF via `bpftool`
2. Compile BPF program with `clang`
3. Generate BPF skeleton header via `bpftool gen skeleton`
4. Compile and link userspace C code with `gcc`

## Quick Test

```bash
# Must run as root (requires CAP_SYS_ADMIN for hardware watchpoints)
sudo ./pg_wait_tracer --pid $(pgrep -xo postgres) --interval 5 --duration 10 -v
```

## Run Tests

```bash
# Build and run all tests (requires root + running PostgreSQL)
sudo tests/run_all.sh

# Target a specific PG version on multi-instance hosts
sudo tests/run_all.sh --pg-version 16

# Or specify the postmaster PID directly
sudo tests/run_all.sh --pid 12345
```

## Troubleshooting

### "symbol 'my_wait_event_info' not found"

The postgres binary must be compiled with symbols (not stripped). Verify:

```bash
nm /usr/lib/postgresql/16/bin/postgres | grep my_wait_event_info
```

If empty, you may need the `-debuginfo` or `-dbgsym` package, or a postgres build
that preserves the symbol table.

### "st_query_id offset not found"

The `query_event` view requires knowing the offset of `st_query_id` in
`PgBackendStatus`. Three detection methods are tried automatically:

1. **DWARF debug symbols** (preferred) — install `postgresql-XX-dbgsym` or
   `postgresql-XX-debuginfo`
2. **Known offset table** — built-in for PG17/18 on x86_64 (offset=424)
3. **Disabled** — other views (time_model, system_event, session_event, histogram)
   still work

### "Cannot find load base" or "/proc permission denied"

Ensure running as root (`sudo`) or with `CAP_SYS_PTRACE` + `CAP_SYS_ADMIN`.

### "bpftool: command not found"

```bash
# Oracle Linux / RHEL:
sudo dnf install -y bpftool

# Ubuntu (bpftool is in linux-tools):
sudo apt install -y linux-tools-$(uname -r)
```

### Kernel BTF missing (`/sys/kernel/btf/vmlinux` not found)

The kernel must be compiled with `CONFIG_DEBUG_INFO_BTF=y`. Oracle Linux UEK7+
and Ubuntu 22.04+ have this by default. Older kernels may need an upgrade.
