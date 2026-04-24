#!/usr/bin/env bash
# record-web.sh — Record demos/web.gif end-to-end.
#
# Boots a local pgwt-server pointed at demos/local-trace, opens it in a real
# Chromium via Playwright, drives the demo flow (zoom + tab switching),
# stops the server, converts video to gif.
#
# Why a real browser instead of headless: ECharts canvas drawing renders
# correctly and animations look right.
#
# Requires:
#   pgwt-server already built (run demos/build-server-local.sh)
#   demos/local-trace/ populated (run demos/capture-trace.sh)
#   node + npm (for Playwright)
#   ffmpeg on PATH

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$HERE/.." && pwd)"
TRACE_DIR="${TRACE_DIR:-$HERE/local-trace}"
PORT="${PORT:-8384}"

[[ -x "$REPO_ROOT/pgwt-server" ]] || { echo "FATAL: build pgwt-server first (demos/build-server-local.sh)"; exit 1; }
[[ -d "$TRACE_DIR" ]] || { echo "FATAL: no trace dir at $TRACE_DIR (run demos/capture-trace.sh)"; exit 1; }

# Install Playwright once
if [[ ! -d "$HERE/node_modules/playwright" ]]; then
    echo "==> Installing Playwright (one-time) ..."
    (cd "$HERE" && npm install --no-fund --no-audit playwright >/dev/null 2>&1)
    (cd "$HERE" && npx playwright install chromium >/dev/null 2>&1)
fi

# Start pgwt web server (Go binary spawns pgwt-server locally via stdio)
# We don't have a "spawn locally" mode in pgwt — easiest is to run pgwt-server
# directly through a tiny shim that the Go web client expects (ssh wrapper).
# Simplest workaround: just run the Go web client with `--server-path` pointing
# to a wrapper that ignores the ssh args.
WRAPPER="$(mktemp)"
trap 'rm -f "$WRAPPER"; [[ -n "${PGWT_PID:-}" ]] && kill "$PGWT_PID" 2>/dev/null || true' EXIT

cat > "$WRAPPER" <<EOF
#!/usr/bin/env bash
exec "$REPO_ROOT/pgwt-server" "$TRACE_DIR"
EOF
chmod +x "$WRAPPER"

# Build the web client if missing
if ! command -v pgwt >/dev/null 2>&1 && [[ ! -x "$REPO_ROOT/web/pgwt" ]]; then
    echo "==> Building pgwt web client ..."
    (cd "$REPO_ROOT/web" && go build -o "$REPO_ROOT/web/pgwt" .)
fi
PGWT_BIN="${PGWT_BIN:-$REPO_ROOT/web/pgwt}"
[[ -x "$PGWT_BIN" ]] || PGWT_BIN="$(command -v pgwt)"

# pgwt expects user@host as positional arg. Use 'localhost' but override the
# server-path to point at our wrapper, and fake ssh by setting PATH such that
# 'ssh' is a passthrough. We use a per-invocation PATH dir.
SSH_SHIM_DIR="$(mktemp -d)"
trap 'rm -f "$WRAPPER"; rm -rf "$SSH_SHIM_DIR"; [[ -n "${PGWT_PID:-}" ]] && kill "$PGWT_PID" 2>/dev/null || true' EXIT
cat > "$SSH_SHIM_DIR/ssh" <<EOF
#!/usr/bin/env bash
# Drop the user@host arg, run the rest locally
shift  # drop user@host
exec "\$@"
EOF
chmod +x "$SSH_SHIM_DIR/ssh"

echo "==> Starting pgwt web client on port $PORT ..."
PATH="$SSH_SHIM_DIR:$PATH" "$PGWT_BIN" \
    --port "$PORT" \
    --server-path "$WRAPPER" \
    --trace-dir "$TRACE_DIR" \
    fake-user@localhost > "$HERE/.pgwt.log" 2>&1 &
PGWT_PID=$!

# Wait for HTTP to come up
for i in 1 2 3 4 5 6 7 8 9 10; do
    if curl -sf "http://localhost:$PORT/" > /dev/null 2>&1; then
        echo "==> pgwt web up at http://localhost:$PORT"
        break
    fi
    sleep 1
done

# Run Playwright to drive + record
PGWT_URL="http://localhost:$PORT/" node "$HERE/record-web.mjs"

echo "==> demos/web.gif ready"
ls -la "$HERE/web.gif"
