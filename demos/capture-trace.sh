#!/usr/bin/env bash
# capture-trace.sh — End-to-end trace capture on a fresh Hetzner VM.
#
# Steps:
#   1. Provision Rocky 9 + PG18 VM via tests/hetzner-vm.sh (cloud-init does
#      everything: PG18, pg_wait_sampling, pgbench init scale 10, build deps)
#   2. rsync local repo to VM, run `make` to build pg_wait_tracer + pgwt-server
#   3. Start pg_wait_tracer --daemon -T /var/lib/pgwt/traces in tmux
#   4. Run demos/workload.sh on the VM (60s pgbench TPC-B + locks + reads)
#   5. Stop tracer, rsync trace dir back to demos/local-trace/
#
# By default does NOT delete the VM — the IP is printed so you can re-run
# capture or `pgwt root@IP` to interact live. Use `make clean-vm` (or
# tests/hetzner-vm.sh delete <ID>) when done.
#
# Requires:
#   HCLOUD_TOKEN env var
#   ssh / scp / rsync
#   ~/.ssh/id_ed25519.pub (or id_rsa.pub) registered in your Hetzner account
#
# Env overrides:
#   VM_TYPE — Hetzner server type (default: cpx22 = 2 vCPU, 4 GB)
#   TRACE_DIR — local destination for trace files (default: demos/local-trace)
#   REUSE_VM_IP — skip provisioning, use this IP (must already have repo built)

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DEMOS_DIR="$REPO_ROOT/demos"
LOCAL_TRACE_DIR="${TRACE_DIR:-$DEMOS_DIR/local-trace}"
VM_TYPE="${VM_TYPE:-cpx22}"

[[ -n "${HCLOUD_TOKEN:-}" ]] || {
    echo "FATAL: HCLOUD_TOKEN not set (export your Hetzner Cloud API token)" >&2
    exit 1
}

# --- 1. Provision (or reuse) ---
if [[ -n "${REUSE_VM_IP:-}" ]]; then
    VM_IP="$REUSE_VM_IP"
    echo "==> Reusing VM at $VM_IP"
else
    echo "==> Provisioning Hetzner VM ($VM_TYPE) ..."
    read -r VM_ID VM_IP < <("$REPO_ROOT/tests/hetzner-vm.sh" create --type "$VM_TYPE" | tail -1)
    echo "==> VM ready: id=$VM_ID ip=$VM_IP"
    echo "$VM_ID" > "$DEMOS_DIR/.vm-id"
    echo "$VM_IP" > "$DEMOS_DIR/.vm-ip"
fi

SSH="ssh -o StrictHostKeyChecking=no root@$VM_IP"
SCP="scp -o StrictHostKeyChecking=no"

# --- 2. Sync + build ---
echo "==> Syncing repo to VM and building ..."
rsync -a --delete \
    --exclude=.git --exclude=build --exclude=pg_wait_tracer --exclude=pgwt-server \
    --exclude='*.o' --exclude=node_modules --exclude=demos/local-trace \
    -e "ssh -o StrictHostKeyChecking=no" \
    "$REPO_ROOT/" "root@$VM_IP:/root/pg_wait_tracer/"

$SSH "set -e
    mkdir -p /root/pg_wait_tracer/include
    cd /root/pg_wait_tracer && make 2>&1 | tail -20
    command -v tmux >/dev/null || dnf install -y tmux >/dev/null 2>&1
    mkdir -p /var/lib/pgwt/traces
"

# --- 3. Start tracer in tmux ---
echo "==> Starting pg_wait_tracer --daemon in tmux ..."
$SSH "tmux kill-server 2>/dev/null || true
    tmux new-session -d -s pgwt -x 200 -y 50
    tmux new-window -t pgwt -n tracer
    tmux send-keys -t pgwt:tracer 'cd /root/pg_wait_tracer && ./pg_wait_tracer --daemon -T /var/lib/pgwt/traces 2>&1 | tee /tmp/tracer.log' C-m
    sleep 3
    tail -20 /tmp/tracer.log
"

# --- 4. Run workload ---
echo "==> Running workload on VM ..."
$SCP "$DEMOS_DIR/workload.sh" "root@$VM_IP:/root/workload.sh"
$SSH "PATH=/usr/pgsql-18/bin:\$PATH chmod +x /root/workload.sh && /root/workload.sh 2>&1 | tee /tmp/workload.log | tail -20"

# --- 5. Stop tracer, sync traces back ---
echo "==> Stopping tracer ..."
$SSH "tmux send-keys -t pgwt:tracer C-c 2>/dev/null; sleep 2; tail -5 /tmp/tracer.log"

echo "==> Downloading trace files to $LOCAL_TRACE_DIR ..."
mkdir -p "$LOCAL_TRACE_DIR"
rsync -a --delete -e "ssh -o StrictHostKeyChecking=no" \
    "root@$VM_IP:/var/lib/pgwt/traces/" "$LOCAL_TRACE_DIR/"
ls -la "$LOCAL_TRACE_DIR"

echo ""
echo "==> Trace captured. Next steps:"
echo "    make dump-gif    # render demos/dump.gif"
echo "    make web-gif     # render demos/web.gif"
echo "    make clean-vm    # delete the Hetzner VM (saves \$\$\$)"
echo ""
echo "VM still running at: root@$VM_IP"
[[ -n "${VM_ID:-}" ]] && echo "VM id: $VM_ID  (saved to demos/.vm-id)"
