#!/bin/sh
# detect_libbpf_usdt.sh — print "yes" if the system libbpf is recent enough to
# build pg_wait_tracer against, "no" otherwise.
#
# pg_wait_tracer needs USDT support, which arrived in libbpf 0.8:
#   - the userspace function bpf_program__attach_usdt()
#   - the BPF-side header <bpf/usdt.bpf.h>
# Ubuntu (CI), Rocky 9 / RHEL 9 (CRB) ship a new enough libbpf and print "yes"
# (no behaviour change). Rocky 8 / RHEL 8 ship libbpf 0.5.0 and print "no", so
# the Makefile builds a pinned libbpf from source instead.
#
# The check is a real link probe: bpf_program__attach_usdt is only DECLARED in
# libbpf >= 0.8, so the compile fails on 0.5.0 and we fall back.
set -eu

CC="${CC:-gcc}"
HDR="${USDT_HEADER:-/usr/include/bpf/usdt.bpf.h}"

tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

cat > "$tmpdir/probe.c" <<'EOF'
#include <bpf/libbpf.h>
int main(void) { return bpf_program__attach_usdt == 0; }
EOF

if "$CC" "$tmpdir/probe.c" -lbpf -o "$tmpdir/probe.out" >/dev/null 2>&1 \
   && [ -r "$HDR" ]; then
    echo yes
else
    echo no
fi
