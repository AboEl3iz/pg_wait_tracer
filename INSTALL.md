# pg_wait_tracer — Installation Guide

## Supported Platforms

- **Linux kernel** >= 5.8 (BPF ring buffer, CO-RE)
- **Architecture**: x86_64, aarch64
- **PostgreSQL**: 17, 18 (full support); 14–16 (limited — see Troubleshooting)
- **Tested on**: Ubuntu 22.04/24.04, Rocky Linux 9, Oracle Linux 9, RHEL 9

## Prerequisites

### 1. Build Dependencies

#### Rocky Linux 9 / RHEL 9 / AlmaLinux 9 / Oracle Linux 9

```bash
# Enable CRB (CodeReady Builder) repo — required for libbpf-devel
sudo dnf config-manager --set-enabled crb                        # Rocky/RHEL/AlmaLinux
# sudo dnf config-manager --set-enabled ol9_codeready_builder    # Oracle Linux

# Core build tools (tar may be missing on minimal installs)
sudo dnf install -y gcc clang llvm make tar

# BPF toolchain (libbpf-devel requires CRB repo enabled above)
sudo dnf install -y bpftool libbpf-devel

# ELF and compression libraries
sudo dnf install -y elfutils-libelf-devel zlib-devel lz4-devel

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
sudo apt install -y libelf-dev zlib1g-dev liblz4-dev

# For DWARF-based st_query_id offset detection (optional)
sudo apt install -y binutils    # provides readelf
```

### 2. Kernel BTF Support

The kernel must expose BTF (BPF Type Format) data. Verify:

```bash
ls /sys/kernel/btf/vmlinux
```

If missing, install the BTF-enabled kernel:
- **Rocky Linux 9 / RHEL 9 / AlmaLinux 9**: Enabled by default
- **Oracle Linux 9**: `sudo dnf install -y kernel-uek` (UEK7+ has BTF)
- **Ubuntu**: Enabled by default on 22.04+

### 3. PostgreSQL

#### Rocky Linux 9 / RHEL 9 / Oracle Linux 9

```bash
# Install PGDG repository
sudo dnf install -y https://download.postgresql.org/pub/repos/yum/reporpms/EL-9-x86_64/pgdg-redhat-repo-latest.noarch.rpm

# Install PostgreSQL 17 (or 18)
sudo dnf install -y postgresql17-server postgresql17-contrib

# Initialize and start
sudo /usr/pgsql-17/bin/postgresql-17-setup initdb
sudo systemctl enable --now postgresql-17

# (Optional) Debug symbols for query_event view
sudo dnf install -y postgresql17-debuginfo
```

#### Ubuntu / Debian

```bash
# Install PostgreSQL 17 (or 18)
sudo apt install -y postgresql-17 postgresql-contrib-17

# (Optional) Debug symbols for query_event view
sudo apt install -y postgresql-17-dbgsym
```

### 4. Test Dependencies (optional)

Running the full test suite requires additional packages and PostgreSQL configuration.

#### Packages

```bash
# Rocky/RHEL/Oracle Linux 9:
sudo dnf install -y python3 bc

# Ubuntu:
sudo apt install -y python3 bc

# pgbench is included in postgresql-contrib (already installed above)
```

#### PostgreSQL Configuration

Tests connect as `postgres` user via `psql` and `pgbench`. On a fresh install
running tests as root, you need to allow local connections without a password:

```bash
# Find pg_hba.conf and switch local auth from peer to trust (test use only)
# Rocky/RHEL: /var/lib/pgsql/17/data/pg_hba.conf
# Ubuntu:     /etc/postgresql/17/main/pg_hba.conf
sudo sed -i 's/^local\s\+all\s\+all\s\+peer/local   all             all                                     trust/' \
    /var/lib/pgsql/17/data/pg_hba.conf    # adjust path for your OS/version
sudo systemctl reload postgresql-17
```

Enable extensions and settings used by tests:

```bash
psql -U postgres -c "ALTER SYSTEM SET compute_query_id = 'on'"
psql -U postgres -c "ALTER SYSTEM SET max_parallel_workers_per_gather = 4"
sudo systemctl restart postgresql-17
```

Initialize pgbench:

```bash
pgbench -U postgres -i postgres    # any scale works; -s 50 for more realistic IO
```

#### Optional: pg_wait_sampling (for cross-validation tests)

The `test_cross_pg_wait_sampling` and Extension event tests require pg_wait_sampling.
These tests are **gracefully skipped** if the extension is not installed.

```bash
# Rocky/RHEL/Oracle Linux 9 (PG17 example):
sudo dnf install -y pg_wait_sampling_17

# Ubuntu (PG17 example):
sudo apt install -y postgresql-17-pg-wait-sampling
```

Add to shared_preload_libraries and restart:

```bash
psql -U postgres -c "ALTER SYSTEM SET shared_preload_libraries = 'pg_stat_statements, pg_wait_sampling'"
sudo systemctl restart postgresql-17
```

### 5. Rust Toolchain (for the investigation client)

The interactive investigation client (`pgwt-cli`) is written in Rust. Install
the Rust toolchain if you want to build it:

```bash
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
```

## Build

### Daemon (C)

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

### Investigation Client (Rust)

```bash
cd client
cargo build --release
```

This produces `client/target/release/pgwt-cli`. The client reads trace files
produced by the daemon (both rotated `.trace.lz4` and the live `current.trace`)
and provides an interactive TUI with an AAS stacked bar chart for investigation.
It does not require root or a running PostgreSQL instance.

**Pixel-perfect chart rendering:** On terminals that support graphics protocols
(iTerm2, Kitty, WezTerm, foot, Sixel-capable terminals), the AAS chart renders
as true pixel graphics. On standard terminals, it falls back to half-block
Unicode characters. Protocol detection is automatic.

```bash
# Quick test with synthetic data
./client/target/release/pgwt-cli /tmp/test --generate-test
./client/target/release/pgwt-cli /tmp/test

# Analyze real traces (works while daemon is running)
./client/target/release/pgwt-cli /var/lib/pgwt/traces

# Non-interactive summary
./client/target/release/pgwt-cli /var/lib/pgwt/traces --dump
```

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

The postgres binary must export the `my_wait_event_info` symbol. Verify:

```bash
# Check dynamic symbol table (works even on stripped binaries)
readelf -s --dyn-syms /usr/pgsql-17/bin/postgres | grep my_wait_event_info

# Or via nm (requires non-stripped binary)
nm /usr/pgsql-17/bin/postgres | grep my_wait_event_info
```

PGDG-packaged binaries are typically stripped but retain dynamic symbols (`readelf`
will find them). If neither works, you may need a postgres build that preserves
the symbol table.

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
# Rocky / RHEL / Oracle Linux:
sudo dnf install -y bpftool

# Ubuntu (bpftool is in linux-tools):
sudo apt install -y linux-tools-$(uname -r)
```

### Kernel BTF missing (`/sys/kernel/btf/vmlinux` not found)

The kernel must be compiled with `CONFIG_DEBUG_INFO_BTF=y`. Rocky Linux 9,
RHEL 9, Oracle Linux UEK7+, and Ubuntu 22.04+ have this by default. Older
kernels may need an upgrade.

### PostgreSQL version compatibility

pg_wait_tracer uses a hardware watchpoint on the `my_wait_event_info` global
variable to trace wait events. PostgreSQL 17+ writes wait events through
`*my_wait_event_info` (global pointer dereference), which the watchpoint captures.
PostgreSQL 14–16 writes directly to `MyProc->wait_event_info` (PGPROC struct field),
bypassing the global pointer. As a result, **full wait event tracing requires
PostgreSQL 17 or later**. On PG14–16 the tracer will start but will not capture
wait events correctly.
