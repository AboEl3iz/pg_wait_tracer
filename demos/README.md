# demos/

Recording tooling for the demo gifs in the project README.

## What's here

| File | Purpose |
|------|---------|
| `web.gif` | Browser UI demo, embedded in main README |
| `dump.gif` | Terminal `pgwt-server --dump` demo, embedded in main README |
| `dump.txt` | Raw text dump (101 lines) — reference output |
| `dump.tape` | VHS tape that renders `dump.gif` |
| `record-web.mjs` | Playwright script that drives + records the web UI |
| `record-web.sh` | Wrapper that boots `pgwt` locally and runs the Playwright script |
| `workload.sh` | The 60s mixed-wait workload — uploaded to the VM |
| `capture-trace.sh` | Provisions a Hetzner VM, builds, runs workload, downloads trace |
| `build-server-local.sh` | Builds `pgwt-server` on macOS / Linux for local replay |
| `Makefile` | Orchestrator: `make all` does everything |
| `PROMPTS.md` | LLM prompts for re-driving the web demo via an MCP browser |

## Reproducing the gifs from scratch

Prerequisites:

- **Hetzner Cloud account** + API token in `HCLOUD_TOKEN` env var
- SSH public key already uploaded to your Hetzner account (script picks the
  one matching `~/.ssh/id_ed25519.pub` or `id_rsa.pub`)
- Local tools: `bash`, `rsync`, `ssh`, `scp`, `jq`, `curl`, `make`, `clang`/`gcc`
- For the `dump.gif`: [`vhs`](https://github.com/charmbracelet/vhs) (`brew install vhs`)
- For the `web.gif`: `node` + `npm`, `ffmpeg`, `go`
- macOS: `brew install lz4` (Linux usually has `liblz4-dev` packaged)

Then:

```bash
cd demos
export HCLOUD_TOKEN=...   # your Hetzner Cloud API token

make all                  # provision VM → capture trace → render both gifs
make clean-vm             # tear down the VM (saves ~€0.007/hr)
```

Or step by step:

```bash
make trace      # ~3 min provisioning + 60s workload
make dump-gif   # ~30 s
make web-gif    # ~30 s (boots Playwright headed Chromium)
make clean-vm   # delete the VM
```

The trace files land in `demos/local-trace/` (gitignored, ~63 MB compressed).

## Iterating on a gif without re-capturing

Once `demos/local-trace/` exists, you can re-render either gif freely without
spinning up a new VM:

```bash
# Tweak demos/dump.tape, then:
make dump-gif

# Tweak demos/record-web.mjs, then:
make web-gif
```

## How the trace was made

`workload.sh` runs on the VM in parallel with `pg_wait_tracer --daemon`:

1. **pgbench TPC-B** — 8 clients, 60s, scale 10 → Lock (row), WAL, ClientRead, LWLock:BufferContent
2. **5× injected `LOCK TABLE pgbench_branches IN EXCLUSIVE MODE` + 2s sleep** — seeds Lock:relation and forces queue contention
3. **3× heavy reads** (`SELECT count(*) FROM pgbench_accounts WHERE aid % 7 = 0`) — surfaces IO + buffer pin activity

Result: ~7M wait-event transitions, ~91 s wall clock, AAS 6.4. Wait class
breakdown (DB time): Client 32%, Lock 21%, Extension 16%, CPU 12%, LWLock 9%,
IO 7% (mostly WalSync).

## Why two recording mechanisms

- **VHS for `dump.gif`** — terminal output, no browser needed. Fast,
  deterministic, tiny dependency footprint.
- **Playwright for `web.gif`** — the web UI is canvas-heavy (ECharts) and
  needs a real Chromium. Playwright drives clicks/drags and records video;
  `ffmpeg` post-processes to gif with a two-pass palette for clean colors.

## VM costs

Hetzner cpx22 = €0.0066/hr at the time of writing. A full `make all` run
(provision → capture → cleanup) takes ~5 minutes total. Don't forget
`make clean-vm` afterwards.

## Troubleshooting

- **"Local SSH key not found in Hetzner"** — upload your pubkey to Hetzner first:
  `hcloud ssh-key create --name $(hostname) --public-key-from-file ~/.ssh/id_ed25519.pub`
- **`vhs` rendering blank** — your `~/.zshrc` may set odd PS1 that confuses VHS.
  The tape uses `setopt interactive_comments` and absolute path to `pgwt-server`
  to avoid PATH surprises.
- **Playwright fails to find tabs** — the UI's tab labels may have changed;
  edit `record-web.mjs` (it uses `getByRole('tab', { name: ... })`).
- **`web.gif` too large** — bump `GIF_FPS` (env var, default 8) down or shrink
  the `scale=` filter in the ffmpeg call inside `record-web.mjs`.
