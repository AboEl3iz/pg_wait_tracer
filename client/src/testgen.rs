//! Generate a synthetic .trace.lz4 test file for PoC testing.

use std::io::{self, Write, Seek, SeekFrom};
use std::fs::File;
use std::path::Path;

const PGWT_TRACE_MAGIC: u32 = 0x54574750;
const PGWT_TRACE_VERSION: u16 = 1;
const PGWT_FLAG_LZ4: u16 = 0x0001;
const BLOCK_EVENTS: usize = 4096;

// Wait event IDs (class << 24 | event_index) — PG18 indices
const CPU: u32 = 0x00000000;
const IO_DATA_FILE_READ: u32 = 0x0A000015;  // IO:DataFileRead (index 21)
const IO_DATA_FILE_WRITE: u32 = 0x0A000018; // IO:DataFileWrite (index 24)
const LOCK_TRANSACTIONID: u32 = 0x03000005; // Lock:transactionid (index 5)
const LWLOCK_WAL_INSERT: u32 = 0x0100003D;  // LWLock:WALInsert (index 61)
const CLIENT_READ: u32 = 0x06000000;        // Client:ClientRead (index 0)
const TIMEOUT_PG_SLEEP: u32 = 0x09000002;   // Timeout:PgSleep (index 2)
const IPC_EXECUTE_GATHER: u32 = 0x0800000D;  // IPC:ExecuteGather (index 13)
const ACTIVITY_IDLE: u32 = 0x05000001;      // Activity:AutovacuumMain (index 1)

fn encode_varint(val: u64, out: &mut Vec<u8>) {
    let mut v = val;
    loop {
        let mut byte = (v & 0x7F) as u8;
        v >>= 7;
        if v != 0 {
            byte |= 0x80;
        }
        out.push(byte);
        if v == 0 {
            break;
        }
    }
}

struct Event {
    timestamp_ns: u64,
    pid: u32,
    old_event: u32,
    new_event: u32,
    duration_ns: u64,
    query_id: u64,
}

fn encode_block(events: &[Event]) -> Vec<u8> {
    let mut buf = Vec::new();

    // Column 1: timestamps, delta-encoded as varint
    let mut prev_ts: u64 = 0;
    for ev in events {
        let delta = ev.timestamp_ns.wrapping_sub(prev_ts);
        encode_varint(delta, &mut buf);
        prev_ts = ev.timestamp_ns;
    }

    // Column 2: PIDs as raw u32 LE
    for ev in events {
        buf.extend_from_slice(&ev.pid.to_le_bytes());
    }

    // Column 3: old_events as raw u32 LE
    for ev in events {
        buf.extend_from_slice(&ev.old_event.to_le_bytes());
    }

    // Column 4: new_events as raw u32 LE
    for ev in events {
        buf.extend_from_slice(&ev.new_event.to_le_bytes());
    }

    // Column 5: durations as varint
    for ev in events {
        encode_varint(ev.duration_ns, &mut buf);
    }

    // Column 6: query_ids as raw u64 LE
    for ev in events {
        buf.extend_from_slice(&ev.query_id.to_le_bytes());
    }

    buf
}

pub fn generate_test_file(path: &Path) -> io::Result<()> {
    let mut file = File::create(path)?;

    // Timestamps: base = 1 hour ago in wall clock, monotonic offset = wall - 1_000_000_000
    let now_wall_ns = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap()
        .as_nanos() as u64;
    let start_wall_ns = now_wall_ns - 3600_000_000_000; // 1 hour ago
    let start_mono_ns = start_wall_ns - 1_000_000_000;  // arbitrary offset

    // Write file header (28 bytes)
    file.write_all(&PGWT_TRACE_MAGIC.to_le_bytes())?;
    file.write_all(&PGWT_TRACE_VERSION.to_le_bytes())?;
    file.write_all(&PGWT_FLAG_LZ4.to_le_bytes())?;
    file.write_all(&18u32.to_le_bytes())?; // pg_version = 18
    file.write_all(&start_wall_ns.to_le_bytes())?;
    file.write_all(&start_mono_ns.to_le_bytes())?;

    // ── RNG ──────────────────────────────────────────────────────────────
    let mut rng_state: u64 = 42;
    let mut cheap_random = || -> u64 {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        rng_state
    };

    let time_span_ns = 1800_000_000_000u64; // 30 minutes
    let end_mono_ns = start_mono_ns + time_span_ns;

    // ── Session roles ───────────────────────────────────────────────────
    // Each role defines weighted (event_id, base_duration_ns) pools and
    // an average gap between events (simulates think time / idle).
    //
    // 0 = oltp_fast:    short OLTP queries, mostly CPU, light IO
    // 1 = oltp_heavy:   complex OLTP, more IO and lock contention
    // 2 = reporting:    long seq scans, IO-dominated
    // 3 = batch_writer: INSERT/UPDATE heavy, IO writes + WAL
    // 4 = parallel:     parallel query workers, IPC waits
    // 5 = maintenance:  autovacuum-like, IO + LWLock

    struct SessionDef {
        pid: u32,
        role: usize,
        query_id: u64,
        start_frac: f64,  // fraction of time_span when session starts
        end_frac: f64,    // fraction of time_span when session ends
    }

    let sessions = vec![
        // 4 OLTP fast — core app workload, always on
        SessionDef { pid: 1001, role: 0, query_id: 111111111111111, start_frac: 0.00, end_frac: 1.00 },
        SessionDef { pid: 1002, role: 0, query_id: 222222222222222, start_frac: 0.00, end_frac: 1.00 },
        SessionDef { pid: 1003, role: 0, query_id: 333333333333333, start_frac: 0.02, end_frac: 0.95 },
        SessionDef { pid: 1004, role: 0, query_id: 444444444444444, start_frac: 0.05, end_frac: 1.00 },
        // 2 OLTP heavy — complex queries, join mid-way or run throughout
        SessionDef { pid: 1005, role: 1, query_id: 555555555555555, start_frac: 0.00, end_frac: 1.00 },
        SessionDef { pid: 1006, role: 1, query_id: 666666666666666, start_frac: 0.10, end_frac: 0.85 },
        // 1 reporting — long-running analytics, appears mid-window
        SessionDef { pid: 1007, role: 2, query_id: 777777777777777, start_frac: 0.20, end_frac: 0.70 },
        // 1 batch writer — ETL load, early portion
        SessionDef { pid: 1008, role: 3, query_id: 888888888888888, start_frac: 0.05, end_frac: 0.55 },
        // 1 parallel query — brief burst
        SessionDef { pid: 1009, role: 4, query_id: 999999999999999, start_frac: 0.35, end_frac: 0.65 },
        // 1 maintenance — autovacuum, runs in background throughout
        SessionDef { pid: 1010, role: 5, query_id: 101010101010101, start_frac: 0.00, end_frac: 1.00 },
    ];

    // Role event pools: Vec<(event_id, base_dur_ns)>
    // Weights come from repetition — more entries = higher probability
    let role_pools: Vec<Vec<(u32, u64)>> = vec![
        // 0: oltp_fast — CPU-dominant, fast queries
        vec![
            (CPU, 8_000_000), (CPU, 12_000_000), (CPU, 15_000_000),
            (CPU, 20_000_000), (CPU, 10_000_000),
            (IO_DATA_FILE_READ, 3_000_000), (IO_DATA_FILE_READ, 6_000_000),
            (LWLOCK_WAL_INSERT, 500_000),
            (CLIENT_READ, 2_000_000),
        ],
        // 1: oltp_heavy — more IO and locks
        vec![
            (CPU, 20_000_000), (CPU, 35_000_000), (CPU, 50_000_000),
            (IO_DATA_FILE_READ, 10_000_000), (IO_DATA_FILE_READ, 20_000_000),
            (IO_DATA_FILE_WRITE, 8_000_000),
            (LOCK_TRANSACTIONID, 5_000_000), (LOCK_TRANSACTIONID, 12_000_000),
            (LWLOCK_WAL_INSERT, 2_000_000),
            (CLIENT_READ, 4_000_000),
        ],
        // 2: reporting — IO-heavy seq scans
        vec![
            (CPU, 40_000_000), (CPU, 80_000_000),
            (IO_DATA_FILE_READ, 15_000_000), (IO_DATA_FILE_READ, 30_000_000),
            (IO_DATA_FILE_READ, 50_000_000), (IO_DATA_FILE_READ, 25_000_000),
            (IO_DATA_FILE_READ, 40_000_000),
            (LWLOCK_WAL_INSERT, 1_000_000),
        ],
        // 3: batch_writer — writes + WAL
        vec![
            (CPU, 10_000_000), (CPU, 20_000_000),
            (IO_DATA_FILE_WRITE, 5_000_000), (IO_DATA_FILE_WRITE, 12_000_000),
            (IO_DATA_FILE_WRITE, 20_000_000), (IO_DATA_FILE_WRITE, 8_000_000),
            (LWLOCK_WAL_INSERT, 3_000_000), (LWLOCK_WAL_INSERT, 6_000_000),
            (LOCK_TRANSACTIONID, 3_000_000),
        ],
        // 4: parallel query — IPC + CPU
        vec![
            (CPU, 25_000_000), (CPU, 40_000_000), (CPU, 60_000_000),
            (IPC_EXECUTE_GATHER, 5_000_000), (IPC_EXECUTE_GATHER, 15_000_000),
            (IPC_EXECUTE_GATHER, 10_000_000),
            (IO_DATA_FILE_READ, 20_000_000),
        ],
        // 5: maintenance (autovacuum) — IO + LWLock, slower pace
        vec![
            (CPU, 15_000_000),
            (IO_DATA_FILE_READ, 10_000_000), (IO_DATA_FILE_READ, 20_000_000),
            (IO_DATA_FILE_WRITE, 8_000_000), (IO_DATA_FILE_WRITE, 15_000_000),
            (LWLOCK_WAL_INSERT, 2_000_000), (LWLOCK_WAL_INSERT, 5_000_000),
        ],
    ];

    // Average gap between events per role (think time / idle between state transitions)
    let role_avg_gap_ns: Vec<u64> = vec![
        6_000_000,   // 0: oltp_fast — 6ms avg gap (fast tx turnover)
        12_000_000,  // 1: oltp_heavy — 12ms
        20_000_000,  // 2: reporting — 20ms (steady sequential reads)
        10_000_000,  // 3: batch_writer — 10ms
        15_000_000,  // 4: parallel — 15ms
        30_000_000,  // 5: maintenance — 30ms (slow background)
    ];

    // ── Time phases (overlapping effects) ───────────────────────────────
    // Phase effects modify event selection for affected sessions:
    //   0.00-0.10  warm-up: fewer sessions, lighter load
    //   0.10-0.30  normal: steady state
    //   0.30-0.45  IO storm: checkpoint flushes → all sessions get extra IO writes
    //   0.45-0.70  lock contention: hot-row updates → OLTP sessions get heavy locks
    //   0.70-0.85  recovery: contention drains, load normalizes
    //   0.85-1.00  cool-down: some sessions finish

    // Lock contention override pool (injected during lock phase for OLTP roles)
    let lock_spike: Vec<(u32, u64)> = vec![
        (LOCK_TRANSACTIONID, 20_000_000),
        (LOCK_TRANSACTIONID, 50_000_000),
        (LOCK_TRANSACTIONID, 80_000_000),
        (LOCK_TRANSACTIONID, 150_000_000),
    ];

    // IO storm override pool (injected during IO phase for all roles)
    let io_storm: Vec<(u32, u64)> = vec![
        (IO_DATA_FILE_WRITE, 15_000_000),
        (IO_DATA_FILE_WRITE, 30_000_000),
        (IO_DATA_FILE_WRITE, 50_000_000),
        (IO_DATA_FILE_READ, 20_000_000),
    ];

    // ── Per-session event generation ────────────────────────────────────
    let mut events = Vec::new();

    for sess in &sessions {
        let sess_start = start_mono_ns + (time_span_ns as f64 * sess.start_frac) as u64;
        let sess_end = start_mono_ns + (time_span_ns as f64 * sess.end_frac) as u64;
        let pool = &role_pools[sess.role];
        let avg_gap = role_avg_gap_ns[sess.role];

        let mut ts = sess_start;

        // Alternate between active bursts and idle gaps
        // Burst: 2-8 seconds of events, then idle: 0.5-3 seconds
        let mut burst_end = ts;
        let mut in_burst = true;

        while ts < sess_end {
            // Manage burst/idle cycling
            if ts >= burst_end {
                if in_burst {
                    // Start idle gap: 0.5s to 3s
                    let idle_ns = 500_000_000 + (cheap_random() % 2_500_000_000);
                    ts += idle_ns;
                    // Emit a Client:ClientRead event for the idle gap
                    if ts < sess_end {
                        events.push(Event {
                            timestamp_ns: ts.saturating_sub(idle_ns),
                            pid: sess.pid,
                            old_event: CLIENT_READ,
                            new_event: CPU,
                            duration_ns: idle_ns,
                            query_id: sess.query_id,
                        });
                    }
                    in_burst = false;
                    burst_end = ts;
                } else {
                    // Start new burst: 2s to 8s
                    let burst_ns = 2_000_000_000 + (cheap_random() % 6_000_000_000);
                    burst_end = ts + burst_ns;
                    in_burst = true;
                }
                continue;
            }

            // Determine time phase
            let frac = (ts - start_mono_ns) as f64 / time_span_ns as f64;

            // Pick event: phase overrides may inject special events
            let is_oltp = sess.role == 0 || sess.role == 1;
            let in_lock_phase = frac > 0.45 && frac < 0.70;
            let in_io_phase = frac > 0.30 && frac < 0.45;

            let (event_id, base_dur) = if in_lock_phase && is_oltp && cheap_random() % 3 == 0 {
                // 33% chance of lock spike for OLTP during contention phase
                let idx = (cheap_random() % lock_spike.len() as u64) as usize;
                lock_spike[idx]
            } else if in_io_phase && cheap_random() % 5 == 0 {
                // 20% chance of IO storm event for all roles during checkpoint
                let idx = (cheap_random() % io_storm.len() as u64) as usize;
                io_storm[idx]
            } else {
                let idx = (cheap_random() % pool.len() as u64) as usize;
                pool[idx]
            };

            // Jitter on duration: 0.3x to 2.5x (wider spread)
            let jitter = (cheap_random() % 220 + 30) as f64 / 100.0;
            let duration = (base_dur as f64 * jitter) as u64;

            // Occasionally switch query_id (simulate new transaction)
            let qid = if cheap_random() % 8 == 0 {
                // 12.5% chance of a different query
                let alt_qids = [
                    111111111111111u64, 222222222222222, 333333333333333,
                    444444444444444, 555555555555555, 666666666666666,
                ];
                alt_qids[(cheap_random() % alt_qids.len() as u64) as usize]
            } else {
                sess.query_id
            };

            events.push(Event {
                timestamp_ns: ts,
                pid: sess.pid,
                old_event: event_id,
                new_event: CPU,
                duration_ns: duration,
                query_id: qid,
            });

            // Next event: avg_gap with wide jitter (0.2x to 3x)
            let gap_jitter = (cheap_random() % 280 + 20) as f64 / 100.0;
            ts += (avg_gap as f64 * gap_jitter) as u64;
        }
    }

    // Sort all events by timestamp (sessions were generated independently)
    events.sort_by_key(|e| e.timestamp_ns);

    // Write blocks
    let mut block_index = Vec::new();

    for chunk in events.chunks(BLOCK_EVENTS) {
        let first_ts = chunk.first().unwrap().timestamp_ns;
        let last_ts = chunk.last().unwrap().timestamp_ns;

        let encoded = encode_block(chunk);
        let compressed = lz4_flex::compress(&encoded);

        let offset = file.stream_position()?;
        block_index.push((first_ts, offset));

        // Block header (28 bytes)
        file.write_all(&first_ts.to_le_bytes())?;
        file.write_all(&last_ts.to_le_bytes())?;
        file.write_all(&(chunk.len() as u32).to_le_bytes())?;
        file.write_all(&(compressed.len() as u32).to_le_bytes())?;
        file.write_all(&(encoded.len() as u32).to_le_bytes())?;

        // Compressed data
        file.write_all(&compressed)?;
    }

    // Write footer: block index entries
    for (ts, offset) in &block_index {
        file.write_all(&ts.to_le_bytes())?;
        file.write_all(&offset.to_le_bytes())?;
    }
    // Block count
    file.write_all(&(block_index.len() as u32).to_le_bytes())?;

    file.flush()?;
    Ok(())
}
