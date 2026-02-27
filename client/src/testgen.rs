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

    // Generate events: simulate 8 backends over 30 minutes
    let pids = [1001u32, 1002, 1003, 1004, 1005, 1006, 1007, 1008];
    let query_ids = [
        123456789012345u64,
        234567890123456,
        345678901234567,
        456789012345678,
    ];

    // Event distribution (weighted):
    // CPU 30%, IO:DataFileRead 25%, Lock:transactionid 15%, LWLock:WALInsert 10%,
    // IO:DataFileWrite 5%, Client:ClientRead 5%, Timeout:PgSleep 2%, Activity 8%
    let event_pool: Vec<(u32, u64)> = vec![
        // (event_id, avg_duration_ns)
        (CPU, 200_000),                    // 200us avg
        (CPU, 150_000),
        (CPU, 300_000),
        (IO_DATA_FILE_READ, 400_000),      // 400us avg
        (IO_DATA_FILE_READ, 800_000),
        (IO_DATA_FILE_WRITE, 300_000),
        (LOCK_TRANSACTIONID, 5_000_000),     // 5ms avg
        (LOCK_TRANSACTIONID, 50_000_000),    // 50ms some long ones
        (LWLOCK_WAL_INSERT, 100_000),      // 100us avg
        (CLIENT_READ, 1_000_000),          // 1ms avg
        (TIMEOUT_PG_SLEEP, 200_000_000),   // 200ms
        (ACTIVITY_IDLE, 500_000_000),      // 500ms idle
    ];

    let mut events = Vec::new();
    let mut ts = start_mono_ns;

    // Generate ~50000 events over 30 minutes
    let total_events = 50_000usize;
    let time_span_ns = 1800_000_000_000u64; // 30 minutes
    let time_step = time_span_ns / total_events as u64;

    let mut rng_state: u64 = 42;
    let mut cheap_random = || -> u64 {
        rng_state ^= rng_state << 13;
        rng_state ^= rng_state >> 7;
        rng_state ^= rng_state << 17;
        rng_state
    };

    for i in 0..total_events {
        let r = cheap_random();
        let pool_idx = (r % event_pool.len() as u64) as usize;
        let (event_id, base_dur) = event_pool[pool_idx];

        // Add jitter to duration (0.5x to 2x)
        let jitter = (cheap_random() % 150 + 50) as f64 / 100.0;
        let duration = (base_dur as f64 * jitter) as u64;

        let pid = pids[(cheap_random() % pids.len() as u64) as usize];
        let qid = if event_id == ACTIVITY_IDLE || event_id == CPU {
            if cheap_random() % 3 == 0 { 0 } else { query_ids[(cheap_random() % query_ids.len() as u64) as usize] }
        } else {
            query_ids[(cheap_random() % query_ids.len() as u64) as usize]
        };

        // Simulate a "spike" in the middle third (Lock:transactionid heavy)
        let event_id = if i > total_events / 3 && i < 2 * total_events / 3 {
            if cheap_random() % 3 == 0 {
                LOCK_TRANSACTIONID
            } else {
                event_id
            }
        } else {
            event_id
        };

        events.push(Event {
            timestamp_ns: ts,
            pid,
            old_event: event_id,
            new_event: CPU, // simplified: next state is always CPU
            duration_ns: duration,
            query_id: qid,
        });

        ts += time_step + (cheap_random() % (time_step / 2));
    }

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
