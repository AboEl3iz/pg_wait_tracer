# pgwt — SSH Architecture PoC

Minimal proof-of-concept to verify the client-server-over-SSH architecture
that Phase F is built on.

**What it proves**: Go binary on your laptop can spawn `ssh`, pipe JSON
requests/responses to a C binary on the DB server, and bridge them to a
browser via WebSocket — all through standard SSH port 22.

**Tested**: macOS (M-series) → SSH → Rocky Linux 9 (Hetzner, US).
Result: 94ms avg round-trip across the Atlantic. Local network
servers should be 5–20ms — plenty fast for an interactive web UI.

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
# gcc is NOT installed by default on minimal Rocky/Oracle images:
sudo dnf install -y gcc    # RHEL/Rocky/Oracle
# or
sudo apt install -y gcc    # Ubuntu/Debian
```

**SSH access:** You must be able to `ssh user@server` from your laptop.
Key-based auth, agent, password, `~/.ssh/config` aliases, ProxyJump —
whatever you normally use. The PoC spawns your system's `ssh` binary
so it inherits all your SSH config automatically.

No root SSH required. The PoC server runs as a regular user.

## Quick Start

### 1. Build the client (on macOS, one-time)

```bash
cd poc/web
go mod tidy       # downloads gorilla/websocket, generates go.sum
go build -o pgwt-poc .
```

### 2. Deploy the server (on DB host)

```bash
# Copy and compile (one-time per server)
scp poc/server.c user@your-server:~
ssh user@your-server 'sudo dnf install -y gcc && gcc -O2 -o pgwt-server-poc server.c'
```

Verify it works on the server:

```bash
ssh user@your-server 'echo "{\"cmd\":\"ping\"}" | ./pgwt-server-poc'
# Expected: {"cmd":"pong","host":"your-host","cpus":N,"pid":...,"ts":...}
```

### 3. Run

```bash
cd poc/web
./pgwt-poc user@your-server /home/user/pgwt-server-poc
```

Browser opens `http://localhost:8384` automatically.

SSH config aliases work too:
```bash
./pgwt-poc myserver /home/user/pgwt-server-poc
```

### 4. Verify

You should see in the browser:

1. **Green status bar** — `Connected: your-hostname (N CPUs, remote PID XXXX)`
2. **Ping button** — sends `{"cmd":"ping"}`, shows JSON response
3. **Info button** — sends `{"cmd":"info"}`, shows host details
4. **Ping x10** — the key test. Measures full round-trip latency:
   `browser → WebSocket → Go → SSH → C → SSH → Go → WebSocket → browser`

**Reference results** (Hetzner test, macOS → US east coast):
- Single ping: ~94ms
- 10 pings: 936ms total (94ms avg)

**Expected for local/office network**: 5–20ms avg.
Anything under 100ms is good enough for interactive web UI.

## Troubleshooting

**SSH hangs or "connection refused":**
```bash
# Test SSH independently first
ssh user@your-server echo hello
```
If this doesn't work, fix SSH access first. The PoC uses the same `ssh`.

**"gcc: command not found" on server:**
```bash
sudo dnf install -y gcc    # RHEL/Rocky/Oracle
```

**Port 8384 already in use:**
Kill any previous pgwt-poc process: `lsof -ti:8384 | xargs kill`

**Browser doesn't open automatically:**
Navigate to `http://localhost:8384` manually.

**WebSocket disconnects immediately:**
Check the terminal where pgwt-poc is running for SSH error messages.
The Go binary prints SSH stderr to its own stderr.

## Files

```
poc/
  server.c              # C server — 40 lines, reads stdin, writes stdout
  README.md             # This file
  web/
    main.go             # Go client — spawns ssh, HTTP server, WS bridge
    static/
      index.html        # Browser UI — WebSocket client, ping/info buttons
    go.mod              # Go module (one dependency: gorilla/websocket)
```

Note: `go.sum` is not checked in — `go mod tidy` generates it on first build.

## Cleanup

```bash
# Remove remote binary
ssh user@your-server 'rm -f pgwt-server-poc server.c'

# Remove local binary
rm -f poc/web/pgwt-poc
```
