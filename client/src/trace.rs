//! Trace file reader: parses .trace.lz4 files produced by pg_wait_tracer daemon.
//!
//! File format:
//!   [FileHeader 28B] [Block0: BlockHeader 28B + LZ4 data] [Block1: ...] ...
//!   [Footer: BlockIndex N×16B + num_blocks 4B]

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

// -- Constants ---------------------------------------------------------------

const PGWT_TRACE_MAGIC: u32 = 0x54574750; // "PGWT" little-endian
const PGWT_FLAG_LZ4: u16 = 0x0001;

// -- On-disk structures (packed, little-endian) ------------------------------

#[derive(Debug, Clone, Copy)]
pub struct FileHeader {
    pub magic: u32,
    pub version: u16,
    pub flags: u16,
    pub pg_version: u32,
    pub start_time_ns: u64,  // CLOCK_REALTIME at file creation
    pub clock_offset_ns: u64, // CLOCK_MONOTONIC at file creation
}

#[derive(Debug, Clone, Copy)]
pub struct BlockHeader {
    pub first_timestamp_ns: u64,
    pub last_timestamp_ns: u64,
    pub num_events: u32,
    pub compressed_size: u32,
    pub uncompressed_size: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct BlockIndexEntry {
    pub timestamp_ns: u64,
    pub file_offset: u64,
}

// -- Event (decoded from columnar format) ------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct TraceEvent {
    pub timestamp_ns: u64,
    pub pid: u32,
    pub old_event: u32,
    pub new_event: u32,
    pub duration_ns: u64,
    pub query_id: u64,
}

// -- Wait event class extraction --------------------------------------------

pub const PG_WAIT_ACTIVITY: u8 = 0x05;

pub fn we_class(wait_event_info: u32) -> u8 {
    ((wait_event_info >> 24) & 0xFF) as u8
}

pub fn is_idle_event(wait_event_info: u32) -> bool {
    we_class(wait_event_info) == PG_WAIT_ACTIVITY
}

pub fn class_name(wait_event_info: u32) -> &'static str {
    if wait_event_info == 0 {
        return "CPU";
    }
    match we_class(wait_event_info) {
        0x01 => "LWLock",
        0x03 => "Lock",
        0x04 => "BufferPin",
        0x05 => "Activity",
        0x06 => "Client",
        0x07 => "Extension",
        0x08 => "IPC",
        0x09 => "Timeout",
        0x0A => "IO",
        _ => "Unknown",
    }
}

// -- Varint (unsigned LEB128) decoder ----------------------------------------

fn decode_varint(data: &[u8]) -> Option<(u64, usize)> {
    let mut result: u64 = 0;
    let mut shift = 0;
    for (i, &byte) in data.iter().enumerate() {
        if shift > 63 {
            return None;
        }
        result |= ((byte & 0x7F) as u64) << shift;
        shift += 7;
        if byte & 0x80 == 0 {
            return Some((result, i + 1));
        }
    }
    None
}

// -- Columnar block decoder --------------------------------------------------

fn decode_block(data: &[u8], num_events: usize) -> io::Result<Vec<TraceEvent>> {
    let mut pos = 0;

    // Column 1: timestamps, delta-encoded as varint
    let mut timestamps = Vec::with_capacity(num_events);
    let mut prev_ts: u64 = 0;
    for _ in 0..num_events {
        let (delta, n) = decode_varint(&data[pos..])
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "bad varint in timestamps"))?;
        pos += n;
        prev_ts = prev_ts.wrapping_add(delta);
        timestamps.push(prev_ts);
    }

    // Column 2: PIDs as raw u32
    let mut pids = Vec::with_capacity(num_events);
    for _ in 0..num_events {
        if pos + 4 > data.len() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated PIDs"));
        }
        let v = u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap());
        pids.push(v);
        pos += 4;
    }

    // Column 3: old_events as raw u32
    let mut old_events = Vec::with_capacity(num_events);
    for _ in 0..num_events {
        if pos + 4 > data.len() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated old_events"));
        }
        let v = u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap());
        old_events.push(v);
        pos += 4;
    }

    // Column 4: new_events as raw u32
    let mut new_events = Vec::with_capacity(num_events);
    for _ in 0..num_events {
        if pos + 4 > data.len() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated new_events"));
        }
        let v = u32::from_le_bytes(data[pos..pos + 4].try_into().unwrap());
        new_events.push(v);
        pos += 4;
    }

    // Column 5: durations as varint
    let mut durations = Vec::with_capacity(num_events);
    for _ in 0..num_events {
        let (val, n) = decode_varint(&data[pos..])
            .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "bad varint in durations"))?;
        pos += n;
        durations.push(val);
    }

    // Column 6: query_ids as raw u64
    let mut query_ids = Vec::with_capacity(num_events);
    for _ in 0..num_events {
        if pos + 8 > data.len() {
            return Err(io::Error::new(io::ErrorKind::UnexpectedEof, "truncated query_ids"));
        }
        let v = u64::from_le_bytes(data[pos..pos + 8].try_into().unwrap());
        query_ids.push(v);
        pos += 8;
    }

    // Assemble events
    let mut events = Vec::with_capacity(num_events);
    for i in 0..num_events {
        events.push(TraceEvent {
            timestamp_ns: timestamps[i],
            pid: pids[i],
            old_event: old_events[i],
            new_event: new_events[i],
            duration_ns: durations[i],
            query_id: query_ids[i],
        });
    }

    Ok(events)
}

// -- Single file reader ------------------------------------------------------

pub struct TraceFileReader {
    file: File,
    pub header: FileHeader,
    pub block_index: Vec<BlockIndexEntry>,
    pub mono_to_wall: i64, // wall_ns = mono_ns + mono_to_wall
}

impl TraceFileReader {
    pub fn open(path: &Path) -> io::Result<Self> {
        let mut file = File::open(path)?;

        // Read file header (28 bytes)
        let mut buf = [0u8; 28];
        file.read_exact(&mut buf)?;
        let header = FileHeader {
            magic: u32::from_le_bytes(buf[0..4].try_into().unwrap()),
            version: u16::from_le_bytes(buf[4..6].try_into().unwrap()),
            flags: u16::from_le_bytes(buf[6..8].try_into().unwrap()),
            pg_version: u32::from_le_bytes(buf[8..12].try_into().unwrap()),
            start_time_ns: u64::from_le_bytes(buf[12..20].try_into().unwrap()),
            clock_offset_ns: u64::from_le_bytes(buf[20..28].try_into().unwrap()),
        };

        if header.magic != PGWT_TRACE_MAGIC {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                format!("bad magic: 0x{:08X}, expected 0x{:08X}", header.magic, PGWT_TRACE_MAGIC),
            ));
        }
        if header.flags & PGWT_FLAG_LZ4 == 0 {
            return Err(io::Error::new(
                io::ErrorKind::InvalidData,
                "only LZ4-compressed trace files are supported",
            ));
        }

        // Read footer: last 4 bytes = num_blocks (u32)
        file.seek(SeekFrom::End(-4))?;
        let mut nb_buf = [0u8; 4];
        file.read_exact(&mut nb_buf)?;
        let num_blocks = u32::from_le_bytes(nb_buf) as usize;

        // Read block index: num_blocks × 16 bytes, right before the 4-byte count
        let index_size = num_blocks * 16;
        file.seek(SeekFrom::End(-(4 + index_size as i64)))?;
        let mut index_buf = vec![0u8; index_size];
        file.read_exact(&mut index_buf)?;

        let mut block_index = Vec::with_capacity(num_blocks);
        for i in 0..num_blocks {
            let off = i * 16;
            block_index.push(BlockIndexEntry {
                timestamp_ns: u64::from_le_bytes(index_buf[off..off + 8].try_into().unwrap()),
                file_offset: u64::from_le_bytes(index_buf[off + 8..off + 16].try_into().unwrap()),
            });
        }

        let mono_to_wall = header.start_time_ns as i64 - header.clock_offset_ns as i64;

        Ok(Self {
            file,
            header,
            block_index,
            mono_to_wall,
        })
    }

    /// Decode a single block by index. Returns events.
    pub fn decode_block(&mut self, block_idx: usize) -> io::Result<Vec<TraceEvent>> {
        let entry = &self.block_index[block_idx];
        self.file.seek(SeekFrom::Start(entry.file_offset))?;

        // Read block header (28 bytes)
        let mut hdr_buf = [0u8; 28];
        self.file.read_exact(&mut hdr_buf)?;
        let bh = BlockHeader {
            first_timestamp_ns: u64::from_le_bytes(hdr_buf[0..8].try_into().unwrap()),
            last_timestamp_ns: u64::from_le_bytes(hdr_buf[8..16].try_into().unwrap()),
            num_events: u32::from_le_bytes(hdr_buf[16..20].try_into().unwrap()),
            compressed_size: u32::from_le_bytes(hdr_buf[20..24].try_into().unwrap()),
            uncompressed_size: u32::from_le_bytes(hdr_buf[24..28].try_into().unwrap()),
        };

        // Read compressed data
        let mut compressed = vec![0u8; bh.compressed_size as usize];
        self.file.read_exact(&mut compressed)?;

        // Decompress
        let decompressed = lz4_flex::decompress(&compressed, bh.uncompressed_size as usize)
            .map_err(|e| io::Error::new(io::ErrorKind::InvalidData, format!("LZ4 decompress: {}", e)))?;

        decode_block(&decompressed, bh.num_events as usize)
    }

    /// Read all events from all blocks (optionally filtered by monotonic time range).
    pub fn read_events(
        &mut self,
        from_mono: Option<u64>,
        to_mono: Option<u64>,
    ) -> io::Result<Vec<TraceEvent>> {
        let start_block = match from_mono {
            Some(ts) => self.find_block(ts),
            None => 0,
        };

        let mut all_events = Vec::new();
        for i in start_block..self.block_index.len() {
            // Skip blocks entirely before our range
            if let Some(to) = to_mono {
                if self.block_index[i].timestamp_ns > to {
                    break;
                }
            }

            let events = self.decode_block(i)?;
            for ev in events {
                if let Some(from) = from_mono {
                    if ev.timestamp_ns < from {
                        continue;
                    }
                }
                if let Some(to) = to_mono {
                    if ev.timestamp_ns > to {
                        break;
                    }
                }
                all_events.push(ev);
            }
        }

        Ok(all_events)
    }

    /// Binary search for the first block containing events at or after mono_ns.
    fn find_block(&self, mono_ns: u64) -> usize {
        match self.block_index.binary_search_by_key(&mono_ns, |e| e.timestamp_ns) {
            Ok(i) => i,
            Err(i) => if i > 0 { i - 1 } else { 0 },
        }
    }

    /// Convert monotonic ns to wall-clock ns.
    pub fn mono_to_wall_ns(&self, mono_ns: u64) -> u64 {
        (mono_ns as i64 + self.mono_to_wall) as u64
    }
}

// -- Multi-file scanner ------------------------------------------------------

pub struct TraceFileEntry {
    pub path: PathBuf,
    pub start_wall_ns: u64,
}

/// Scan a directory for .trace.lz4 files, return sorted by start time.
pub fn scan_trace_dir(dir: &Path) -> io::Result<Vec<TraceFileEntry>> {
    let mut entries = Vec::new();

    for entry in std::fs::read_dir(dir)? {
        let entry = entry?;
        let name = entry.file_name();
        let name_str = name.to_string_lossy();
        if !name_str.ends_with(".trace.lz4") {
            continue;
        }

        // Read just the header to get start_time_ns
        let path = entry.path();
        match TraceFileReader::open(&path) {
            Ok(reader) => {
                entries.push(TraceFileEntry {
                    path,
                    start_wall_ns: reader.header.start_time_ns,
                });
            }
            Err(e) => {
                eprintln!("WARN: skipping {}: {}", path.display(), e);
            }
        }
    }

    entries.sort_by_key(|e| e.start_wall_ns);
    Ok(entries)
}

/// Load all events from all trace files in a directory, within a wall-clock time range.
pub fn load_events(
    dir: &Path,
    from_wall_ns: Option<u64>,
    to_wall_ns: Option<u64>,
) -> io::Result<Vec<TraceEvent>> {
    let files = scan_trace_dir(dir)?;
    let mut all_events = Vec::new();

    for entry in &files {
        let mut reader = TraceFileReader::open(&entry.path)?;

        let from_mono = from_wall_ns.map(|w| (w as i64 - reader.mono_to_wall) as u64);
        let to_mono = to_wall_ns.map(|w| (w as i64 - reader.mono_to_wall) as u64);

        let events = reader.read_events(from_mono, to_mono)?;
        all_events.extend(events.into_iter().map(|mut ev| {
            // Convert timestamps to wall-clock for uniform handling
            ev.timestamp_ns = reader.mono_to_wall_ns(ev.timestamp_ns);
            ev
        }));
    }

    all_events.sort_by_key(|e| e.timestamp_ns);
    Ok(all_events)
}
