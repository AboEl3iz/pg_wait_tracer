# pgwt — SSH Architecture PoC

Minimal proof-of-concept to verify the client-server-over-SSH architecture
that Phase F is built on.

**What it proves**: Go binary on your laptop can spawn `ssh`, pipe JSON
requests/responses to a C binary on the DB server, and bridge them to a
browser via WebSocket — all through standard SSH port 22.

```
[macOS laptop]                        [DB server]
pgwt-poc (Go)                         pgwt-server-poc (C)
  ├─ spawns ssh ──────── SSH ──────>  ├─ reads JSON from stdin
  ├─ localhost:8384 HTTP server       ├─ writes JSON to stdout
  ├─ WebSocket bridge                 └─ reports host, cpus, pid
  └─ opens browser
```

## Prerequisites

**Laptop (macOS):**
```bash
brew install go    # Go 1.21+
```

**DB server (Rocky/Oracle Linux):**
```bash
# gcc is typically already installed; if not:
sudo dnf install -y gcc    # RHEL/Rocky/Oracle
# or
sudo apt install -y gcc    # Ubuntu/Debian
```

**SSH access:** You must be able to `ssh user@server` without issues
(key-based auth, agent, or password — whatever you normally use).

## Quick Start

### 1. Build the server (on DB host)

```bash
scp poc/server.c root@your-server:~
ssh root@your-server 'gcc -O2 -o pgwt-server-poc server.c'
```

Or if you prefer to build on the server directly:

```bash
ssh root@your-server
# paste or vi server.c, then:
gcc -O2 -o pgwt-server-poc server.c
```

### 2. Build the client (on macOS)

```bash
cd poc/web
go mod tidy
go build -o pgwt-poc .
```

### 3. Run

```bash
./pgwt-poc root@your-server /root/pgwt-server-poc
```

Your browser opens `http://localhost:8384` automatically.

### 4. Verify

- **Green status bar** — shows remote hostname, CPU count, remote PID
- **Ping button** — sends `{"cmd":"ping"}`, server responds with JSON
- **Info button** — sends `{"cmd":"info"}`, same idea
- **Ping x10** — measures round-trip latency through the full path:
  browser → WebSocket → Go → SSH → C → SSH → Go → WebSocket → browser

Expected latency: **2–10ms per round-trip** depending on network distance.
This is fast enough for an interactive web UI.

## What to Look For

**If it works** — you'll see server responses in the browser with the
correct remote hostname and CPU count. The latency test confirms the
SSH pipe is fast enough. Phase F is viable in your environment.

**If SSH hangs** — check that `ssh root@your-server echo hello` works
from the same terminal. The PoC uses your system's `ssh` binary, so it
inherits all your `~/.ssh/config`, agent keys, and ProxyJump settings.

**If WebSocket fails** — check that port 8384 is free on localhost.
The Go binary serves on `localhost:8384` (not exposed to network).

## Files

```
poc/
  server.c              # C server — 40 lines, reads stdin, writes stdout
  README.md             # This file
  web/
    main.go             # Go client — spawns ssh, HTTP server, WS bridge
    static/
      index.html        # Browser UI — WebSocket client, ping/info buttons
    go.mod
    go.sum
```

## Cleanup

```bash
# Remove remote binary
ssh root@your-server rm pgwt-server-poc server.c

# Remove local binary
rm poc/web/pgwt-poc
```
