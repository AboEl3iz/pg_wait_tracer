//! Compute aggregates from trace events for each view.
//!
//! All compute functions take a slice of events (already filtered by time range)
//! and optional filters, and produce sorted result rows for display.

use std::collections::HashMap;
use crate::trace::{TraceEvent, class_name, event_name, is_idle_event, we_class};

// -- Filter ------------------------------------------------------------------

#[derive(Clone, Debug, Default)]
pub struct Filters {
    pub class: Option<String>,   // e.g. "IO", "Lock"
    pub event: Option<u32>,      // exact wait_event_info
    pub pid: Option<u32>,
    pub query_id: Option<u64>,
}

impl Filters {
    pub fn matches(&self, ev: &TraceEvent) -> bool {
        if let Some(ref cls) = self.class {
            if class_name(ev.old_event) != cls.as_str() {
                return false;
            }
        }
        if let Some(eid) = self.event {
            if ev.old_event != eid {
                return false;
            }
        }
        if let Some(pid) = self.pid {
            if ev.pid != pid {
                return false;
            }
        }
        if let Some(qid) = self.query_id {
            if ev.query_id != qid {
                return false;
            }
        }
        true
    }

    pub fn is_empty(&self) -> bool {
        self.class.is_none() && self.event.is_none() && self.pid.is_none() && self.query_id.is_none()
    }

    pub fn description(&self) -> Vec<String> {
        let mut parts = Vec::new();
        if let Some(ref c) = self.class {
            parts.push(format!("class={}", c));
        }
        if let Some(e) = self.event {
            parts.push(format!("event=0x{:08X}", e));
        }
        if let Some(p) = self.pid {
            parts.push(format!("pid={}", p));
        }
        if let Some(q) = self.query_id {
            parts.push(format!("query={}", q));
        }
        parts
    }
}

// -- Time Model (Overview) ---------------------------------------------------

#[derive(Debug, Clone)]
pub struct TimeModelRow {
    pub name: String,
    pub time_ms: f64,
    pub pct_db_time: f64,
    pub aas: f64,
    pub indent: u8, // 0 = top-level, 1 = sub-event
}

pub struct TimeModelResult {
    pub rows: Vec<TimeModelRow>,
    pub db_time_ms: f64,
    pub idle_time_ms: f64,
    pub aas: f64,
    pub wall_clock_ms: f64,
}

pub fn compute_time_model(events: &[TraceEvent], filters: &Filters, wall_ms: f64) -> TimeModelResult {
    let mut class_totals: HashMap<String, f64> = HashMap::new();
    let mut event_totals: HashMap<(String, u32), f64> = HashMap::new(); // (class, event_id) -> ms
    let mut db_time_ms: f64 = 0.0;
    let mut idle_time_ms: f64 = 0.0;

    for ev in events {
        if !filters.matches(ev) {
            continue;
        }
        let dur_ms = ev.duration_ns as f64 / 1_000_000.0;
        let cls = class_name(ev.old_event).to_string();

        if is_idle_event(ev.old_event) {
            idle_time_ms += dur_ms;
        } else {
            db_time_ms += dur_ms;
            *class_totals.entry(cls.clone()).or_default() += dur_ms;
            *event_totals.entry((cls, ev.old_event)).or_default() += dur_ms;
        }
    }

    let mut rows = Vec::new();

    // DB Time row
    rows.push(TimeModelRow {
        name: "DB Time".to_string(),
        time_ms: db_time_ms,
        pct_db_time: 100.0,
        aas: if wall_ms > 0.0 { db_time_ms / wall_ms } else { 0.0 },
        indent: 0,
    });

    // Sort classes by total time descending
    let mut classes: Vec<_> = class_totals.iter().collect();
    classes.sort_by(|a, b| b.1.partial_cmp(a.1).unwrap());

    for (cls, &total) in &classes {
        let pct = if db_time_ms > 0.0 { total / db_time_ms * 100.0 } else { 0.0 };
        let suffix = if cls.as_str() == "CPU" { "*" } else { "" };
        rows.push(TimeModelRow {
            name: format!("{}{}", cls, suffix),
            time_ms: total,
            pct_db_time: pct,
            aas: if wall_ms > 0.0 { total / wall_ms } else { 0.0 },
            indent: 1,
        });

        // Top 3 sub-events for this class (skip CPU — no sub-events)
        if cls.as_str() != "CPU" {
            let mut sub_events: Vec<_> = event_totals
                .iter()
                .filter(|((c, _), _)| c == *cls)
                .collect();
            sub_events.sort_by(|a, b| b.1.partial_cmp(a.1).unwrap());

            for ((_cls, _eid), &sub_ms) in sub_events.iter().take(3) {
                let sub_pct = if db_time_ms > 0.0 { sub_ms / db_time_ms * 100.0 } else { 0.0 };
                if sub_pct < 1.0 {
                    break;
                }
                rows.push(TimeModelRow {
                    name: event_name(*_eid),
                    time_ms: sub_ms,
                    pct_db_time: sub_pct,
                    aas: if wall_ms > 0.0 { sub_ms / wall_ms } else { 0.0 },
                    indent: 2,
                });
            }
        }
    }

    TimeModelResult {
        rows,
        db_time_ms,
        idle_time_ms,
        aas: if wall_ms > 0.0 { db_time_ms / wall_ms } else { 0.0 },
        wall_clock_ms: wall_ms,
    }
}

// -- Top Events --------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct EventRow {
    pub event_id: u32,
    pub event_name: String, // "Class:Event" or hex for now
    pub count: u64,
    pub total_ms: f64,
    pub avg_us: f64,
    pub max_us: f64,
    pub pct_db: f64,
    pub aas: f64,
}

pub fn compute_top_events(events: &[TraceEvent], filters: &Filters, wall_ms: f64) -> Vec<EventRow> {
    struct Accum {
        count: u64,
        total_ns: u64,
        max_ns: u64,
    }

    let mut db_time_ns: u64 = 0;
    let mut map: HashMap<u32, Accum> = HashMap::new();

    for ev in events {
        if !filters.matches(ev) || is_idle_event(ev.old_event) {
            continue;
        }
        db_time_ns += ev.duration_ns;
        let e = map.entry(ev.old_event).or_insert(Accum {
            count: 0,
            total_ns: 0,
            max_ns: 0,
        });
        e.count += 1;
        e.total_ns += ev.duration_ns;
        if ev.duration_ns > e.max_ns {
            e.max_ns = ev.duration_ns;
        }
    }

    let db_time_ms = db_time_ns as f64 / 1_000_000.0;
    let mut rows: Vec<EventRow> = map
        .into_iter()
        .map(|(eid, acc)| {
            let total_ms = acc.total_ns as f64 / 1_000_000.0;
            let name = if eid == 0 {
                "CPU*".to_string()
            } else {
                event_name(eid)
            };
            EventRow {
                event_id: eid,
                event_name: name,
                count: acc.count,
                total_ms,
                avg_us: if acc.count > 0 {
                    acc.total_ns as f64 / acc.count as f64 / 1000.0
                } else {
                    0.0
                },
                max_us: acc.max_ns as f64 / 1000.0,
                pct_db: if db_time_ms > 0.0 {
                    total_ms / db_time_ms * 100.0
                } else {
                    0.0
                },
                aas: if wall_ms > 0.0 { total_ms / wall_ms } else { 0.0 },
            }
        })
        .collect();

    rows.sort_by(|a, b| b.total_ms.partial_cmp(&a.total_ms).unwrap());
    rows
}

// -- Top Sessions ------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct SessionRow {
    pub pid: u32,
    pub db_time_ms: f64,
    pub cpu_pct: f64,
    pub wait_pct: f64,
    pub top_wait: String,
    pub top_wait_id: u32,
}

pub fn compute_top_sessions(events: &[TraceEvent], filters: &Filters, _wall_ms: f64) -> Vec<SessionRow> {
    struct Accum {
        total_ns: u64,
        cpu_ns: u64,
        event_totals: HashMap<u32, u64>,
    }

    let mut map: HashMap<u32, Accum> = HashMap::new();

    for ev in events {
        if !filters.matches(ev) || is_idle_event(ev.old_event) {
            continue;
        }
        let a = map.entry(ev.pid).or_insert(Accum {
            total_ns: 0,
            cpu_ns: 0,
            event_totals: HashMap::new(),
        });
        a.total_ns += ev.duration_ns;
        if ev.old_event == 0 {
            a.cpu_ns += ev.duration_ns;
        }
        *a.event_totals.entry(ev.old_event).or_default() += ev.duration_ns;
    }

    let mut rows: Vec<SessionRow> = map
        .into_iter()
        .map(|(pid, acc)| {
            let db_time = acc.total_ns as f64;
            let cpu_pct = if db_time > 0.0 {
                acc.cpu_ns as f64 / db_time * 100.0
            } else {
                0.0
            };
            let (top_wait_id, _top_ns) = acc
                .event_totals
                .iter()
                .filter(|(&eid, _)| eid != 0) // skip CPU
                .max_by_key(|(_, &ns)| ns)
                .map(|(&eid, &ns)| (eid, ns))
                .unwrap_or((0, 0));
            let top_wait = if top_wait_id == 0 {
                "CPU*".to_string()
            } else {
                event_name(top_wait_id)
            };

            SessionRow {
                pid,
                db_time_ms: acc.total_ns as f64 / 1_000_000.0,
                cpu_pct,
                wait_pct: 100.0 - cpu_pct,
                top_wait,
                top_wait_id,
            }
        })
        .collect();

    rows.sort_by(|a, b| b.db_time_ms.partial_cmp(&a.db_time_ms).unwrap());
    rows
}

// -- Top Queries -------------------------------------------------------------

#[derive(Debug, Clone)]
pub struct QueryRow {
    pub query_id: u64,
    pub count: u64,
    pub total_ms: f64,
    pub avg_us: f64,
    pub pct_db: f64,
    pub top_wait: String,
    pub top_wait_id: u32,
}

pub fn compute_top_queries(events: &[TraceEvent], filters: &Filters, _wall_ms: f64) -> Vec<QueryRow> {
    struct Accum {
        count: u64,
        total_ns: u64,
        event_totals: HashMap<u32, u64>,
    }

    let mut db_time_ns: u64 = 0;
    let mut map: HashMap<u64, Accum> = HashMap::new();

    for ev in events {
        if !filters.matches(ev) || is_idle_event(ev.old_event) || ev.query_id == 0 {
            continue;
        }
        db_time_ns += ev.duration_ns;
        let a = map.entry(ev.query_id).or_insert(Accum {
            count: 0,
            total_ns: 0,
            event_totals: HashMap::new(),
        });
        a.count += 1;
        a.total_ns += ev.duration_ns;
        *a.event_totals.entry(ev.old_event).or_default() += ev.duration_ns;
    }

    let db_time_ms = db_time_ns as f64 / 1_000_000.0;
    let mut rows: Vec<QueryRow> = map
        .into_iter()
        .map(|(qid, acc)| {
            let total_ms = acc.total_ns as f64 / 1_000_000.0;
            let (top_wait_id, _) = acc
                .event_totals
                .iter()
                .max_by_key(|(_, &ns)| ns)
                .map(|(&eid, &ns)| (eid, ns))
                .unwrap_or((0, 0));
            let top_wait = if top_wait_id == 0 {
                "CPU*".to_string()
            } else {
                event_name(top_wait_id)
            };

            QueryRow {
                query_id: qid,
                count: acc.count,
                total_ms,
                avg_us: if acc.count > 0 {
                    acc.total_ns as f64 / acc.count as f64 / 1000.0
                } else {
                    0.0
                },
                pct_db: if db_time_ms > 0.0 {
                    total_ms / db_time_ms * 100.0
                } else {
                    0.0
                },
                top_wait,
                top_wait_id,
            }
        })
        .collect();

    rows.sort_by(|a, b| b.total_ms.partial_cmp(&a.total_ms).unwrap());
    rows
}

// -- Wait Class (for AAS chart) -----------------------------------------------

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum WaitClass {
    Cpu = 0,
    Io = 1,
    Lock = 2,
    LwLock = 3,
    Ipc = 4,
    Client = 5,
    Timeout = 6,
    BufferPin = 7,
    Activity = 8,
    Extension = 9,
    Unknown = 10,
}

pub const NUM_WAIT_CLASSES: usize = 11;

pub fn wait_class(event_id: u32) -> WaitClass {
    if event_id == 0 {
        return WaitClass::Cpu;
    }
    match we_class(event_id) {
        0x0A => WaitClass::Io,
        0x03 => WaitClass::Lock,
        0x01 => WaitClass::LwLock,
        0x08 => WaitClass::Ipc,
        0x06 => WaitClass::Client,
        0x09 => WaitClass::Timeout,
        0x04 => WaitClass::BufferPin,
        0x05 => WaitClass::Activity,
        0x07 => WaitClass::Extension,
        _    => WaitClass::Unknown,
    }
}

// -- AAS Buckets (for stacked bar chart) --------------------------------------

#[derive(Clone, Debug)]
pub struct AasBucket {
    pub start_ns: u64,
    pub class_aas: [f64; NUM_WAIT_CLASSES],
}

pub struct AasBucketResult {
    pub buckets: Vec<AasBucket>,
    pub bucket_ns: u64,
    pub max_aas: f64,
}

pub fn compute_aas_buckets(
    events: &[TraceEvent],
    filters: &Filters,
    from_ns: u64,
    to_ns: u64,
    num_buckets: usize,
) -> AasBucketResult {
    let range_ns = to_ns.saturating_sub(from_ns);
    if range_ns == 0 || num_buckets == 0 {
        return AasBucketResult {
            buckets: Vec::new(),
            bucket_ns: 1_000_000_000,
            max_aas: 0.0,
        };
    }

    // bucket_ns = max(1s, ceil(range / num_buckets))
    let bucket_ns = ((range_ns + num_buckets as u64 - 1) / num_buckets as u64)
        .max(1_000_000_000);
    let actual_buckets = ((range_ns + bucket_ns - 1) / bucket_ns) as usize;

    let mut buckets: Vec<AasBucket> = (0..actual_buckets)
        .map(|i| AasBucket {
            start_ns: from_ns + i as u64 * bucket_ns,
            class_aas: [0.0; NUM_WAIT_CLASSES],
        })
        .collect();

    for ev in events {
        if !filters.matches(ev) || is_idle_event(ev.old_event) {
            continue;
        }
        let ev_start = ev.timestamp_ns;
        let ev_end = ev.timestamp_ns + ev.duration_ns;

        if ev_end <= from_ns || ev_start >= to_ns {
            continue;
        }

        let class_idx = wait_class(ev.old_event) as usize;
        let first_bucket = if ev_start <= from_ns {
            0
        } else {
            ((ev_start - from_ns) / bucket_ns) as usize
        };
        let last_bucket = if ev_end >= to_ns {
            actual_buckets - 1
        } else {
            ((ev_end - from_ns).saturating_sub(1) / bucket_ns) as usize
        };
        let last_bucket = last_bucket.min(actual_buckets - 1);

        for b in first_bucket..=last_bucket {
            let b_start = from_ns + b as u64 * bucket_ns;
            let b_end = b_start + bucket_ns;
            let overlap_start = ev_start.max(b_start);
            let overlap_end = ev_end.min(b_end);
            if overlap_end > overlap_start {
                buckets[b].class_aas[class_idx] += (overlap_end - overlap_start) as f64;
            }
        }
    }

    // Convert accumulated ns → AAS = accumulated_ns / bucket_ns
    let mut max_aas: f64 = 0.0;
    for bucket in &mut buckets {
        let mut total = 0.0;
        for v in bucket.class_aas.iter_mut() {
            *v /= bucket_ns as f64;
            total += *v;
        }
        if total > max_aas {
            max_aas = total;
        }
    }

    AasBucketResult { buckets, bucket_ns, max_aas }
}
