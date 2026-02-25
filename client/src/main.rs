mod compute;
mod testgen;
mod trace;

use std::io;
use std::path::PathBuf;
use std::time::Duration;

use clap::Parser;
use crossterm::event::{self, Event, KeyCode, KeyModifiers};
use crossterm::terminal::{disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen};
use crossterm::execute;
use ratatui::prelude::*;
use ratatui::widgets::*;

use compute::Filters;

// -- CLI args ----------------------------------------------------------------

#[derive(Parser)]
#[command(name = "pgwt-cli", about = "Interactive investigation client for pg_wait_tracer")]
struct Args {
    /// Path to trace directory containing .trace.lz4 files
    trace_dir: PathBuf,

    /// Start time (ISO 8601, relative like "1h", or "now")
    #[arg(long)]
    from: Option<String>,

    /// End time (ISO 8601, relative like "1h", or "now")
    #[arg(long)]
    to: Option<String>,

    /// Generate a synthetic test trace file and exit
    #[arg(long)]
    generate_test: bool,

    /// Print summary to stdout and exit (no TUI)
    #[arg(long)]
    dump: bool,
}

// -- App state ---------------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum View {
    Overview,
    Events,
    Sessions,
    Queries,
}

impl View {
    fn label(&self) -> &str {
        match self {
            View::Overview => "Overview",
            View::Events => "Events",
            View::Sessions => "Sessions",
            View::Queries => "Queries",
        }
    }

    fn all() -> &'static [View] {
        &[View::Overview, View::Events, View::Sessions, View::Queries]
    }
}

struct DrillEntry {
    filters: Filters,
    view: View,
    cursor: usize,
}

struct App {
    events: Vec<trace::TraceEvent>,
    time_from_ns: u64,
    time_to_ns: u64,
    wall_ms: f64,

    view: View,
    filters: Filters,
    cursor: usize,
    drill_stack: Vec<DrillEntry>,

    should_quit: bool,
}

impl App {
    fn new(events: Vec<trace::TraceEvent>) -> Self {
        let time_from_ns = events.first().map(|e| e.timestamp_ns).unwrap_or(0);
        let time_to_ns = events.last().map(|e| e.timestamp_ns).unwrap_or(0);
        let wall_ms = (time_to_ns - time_from_ns) as f64 / 1_000_000.0;

        Self {
            events,
            time_from_ns,
            time_to_ns,
            wall_ms,
            view: View::Overview,
            filters: Filters::default(),
            cursor: 0,
            drill_stack: Vec::new(),
            should_quit: false,
        }
    }

    fn filtered_events(&self) -> Vec<&trace::TraceEvent> {
        self.events
            .iter()
            .filter(|ev| ev.timestamp_ns >= self.time_from_ns && ev.timestamp_ns <= self.time_to_ns)
            .collect()
    }

    fn max_rows(&self) -> usize {
        match self.view {
            View::Overview => {
                let tm = compute::compute_time_model(&self.events, &self.filters, self.wall_ms);
                tm.rows.len()
            }
            View::Events => {
                compute::compute_top_events(&self.events, &self.filters, self.wall_ms).len()
            }
            View::Sessions => {
                compute::compute_top_sessions(&self.events, &self.filters, self.wall_ms).len()
            }
            View::Queries => {
                compute::compute_top_queries(&self.events, &self.filters, self.wall_ms).len()
            }
        }
    }

    fn drill_down(&mut self) {
        // Save current state
        self.drill_stack.push(DrillEntry {
            filters: self.filters.clone(),
            view: self.view,
            cursor: self.cursor,
        });

        match self.view {
            View::Overview => {
                // Drill from overview: filter to the selected class, pivot to Events
                let tm = compute::compute_time_model(&self.events, &self.filters, self.wall_ms);
                if let Some(row) = tm.rows.get(self.cursor) {
                    let name = row.name.trim_end_matches('*').to_string();
                    if name != "DB Time" {
                        self.filters.class = Some(name);
                    }
                }
                self.view = View::Events;
                self.cursor = 0;
            }
            View::Events => {
                // Drill from events: filter to the selected event, pivot to Sessions
                let rows = compute::compute_top_events(&self.events, &self.filters, self.wall_ms);
                if let Some(row) = rows.get(self.cursor) {
                    self.filters.event = Some(row.event_id);
                }
                self.view = View::Sessions;
                self.cursor = 0;
            }
            View::Sessions => {
                // Drill from sessions: filter to the selected PID, pivot to Queries
                let rows = compute::compute_top_sessions(&self.events, &self.filters, self.wall_ms);
                if let Some(row) = rows.get(self.cursor) {
                    self.filters.pid = Some(row.pid);
                }
                self.view = View::Queries;
                self.cursor = 0;
            }
            View::Queries => {
                // At the deepest level, no further drill
                self.drill_stack.pop(); // undo the save
            }
        }
    }

    fn drill_back(&mut self) {
        if let Some(entry) = self.drill_stack.pop() {
            self.filters = entry.filters;
            self.view = entry.view;
            self.cursor = entry.cursor;
        }
    }

    fn handle_key(&mut self, key: KeyCode, modifiers: KeyModifiers) {
        match key {
            KeyCode::Char('q') | KeyCode::Char('c') if modifiers.contains(KeyModifiers::CONTROL) => {
                self.should_quit = true;
            }
            KeyCode::Char('q') => self.should_quit = true,
            KeyCode::Up | KeyCode::Char('k') => {
                if self.cursor > 0 {
                    self.cursor -= 1;
                }
            }
            KeyCode::Down | KeyCode::Char('j') => {
                let max = self.max_rows();
                if max > 0 && self.cursor < max - 1 {
                    self.cursor += 1;
                }
            }
            KeyCode::Enter => self.drill_down(),
            KeyCode::Esc | KeyCode::Backspace => self.drill_back(),

            // View shortcuts
            KeyCode::Char('o') => { self.view = View::Overview; self.cursor = 0; }
            KeyCode::Char('e') => { self.view = View::Events; self.cursor = 0; }
            KeyCode::Char('s') => { self.view = View::Sessions; self.cursor = 0; }
            KeyCode::Char('r') => { self.view = View::Queries; self.cursor = 0; }

            // Clear filters
            KeyCode::Char('\\') => {
                self.filters = Filters::default();
                self.drill_stack.clear();
                self.cursor = 0;
            }
            _ => {}
        }
    }
}

// -- Rendering ---------------------------------------------------------------

fn render_header(app: &App, area: Rect, buf: &mut Buffer) {
    let from = chrono::DateTime::from_timestamp_nanos(app.time_from_ns as i64);
    let to = chrono::DateTime::from_timestamp_nanos(app.time_to_ns as i64);
    let dur_secs = app.wall_ms / 1000.0;

    let tm = compute::compute_time_model(&app.events, &app.filters, app.wall_ms);
    let header_text = format!(
        " {} — {}  ({:.0}s)    AAS: {:.2}    DB Time: {:.1}s",
        from.format("%H:%M:%S"),
        to.format("%H:%M:%S"),
        dur_secs,
        tm.aas,
        tm.db_time_ms / 1000.0,
    );

    let filter_desc = app.filters.description();
    let filter_text = if filter_desc.is_empty() {
        String::new()
    } else {
        format!("    Filters: {}", filter_desc.join(" > "))
    };

    Paragraph::new(format!("{}{}", header_text, filter_text))
        .style(Style::default().fg(Color::White).bg(Color::DarkGray))
        .render(area, buf);
}

fn render_tabs(app: &App, area: Rect, buf: &mut Buffer) {
    let titles: Vec<Line> = View::all()
        .iter()
        .map(|v| {
            let style = if *v == app.view {
                Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
            } else {
                Style::default().fg(Color::Gray)
            };
            Line::from(Span::styled(format!(" {} ", v.label()), style))
        })
        .collect();
    Tabs::new(titles)
        .select(View::all().iter().position(|v| *v == app.view).unwrap_or(0))
        .style(Style::default())
        .highlight_style(Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD))
        .divider("|")
        .render(area, buf);
}

fn render_overview(app: &App, area: Rect, buf: &mut Buffer) {
    let tm = compute::compute_time_model(&app.events, &app.filters, app.wall_ms);

    let rows: Vec<Row> = tm
        .rows
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let indent = "  ".repeat(r.indent as usize);
            let style = if i == app.cursor {
                Style::default().bg(Color::DarkGray).fg(Color::White).add_modifier(Modifier::BOLD)
            } else if r.indent == 0 {
                Style::default().add_modifier(Modifier::BOLD)
            } else {
                Style::default()
            };
            Row::new(vec![
                Cell::from(format!("{}{}", indent, r.name)),
                Cell::from(format!("{:.1}", r.time_ms)),
                Cell::from(format!("{:.1}%", r.pct_db_time)),
                Cell::from(format!("{:.2}", r.aas)),
            ])
            .style(style)
        })
        .collect();

    let table = Table::new(
        rows,
        [
            Constraint::Min(30),
            Constraint::Length(12),
            Constraint::Length(10),
            Constraint::Length(8),
        ],
    )
    .header(
        Row::new(vec!["Stat Name", "Time (ms)", "% DB Time", "AAS"])
            .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .bottom_margin(1),
    )
    .block(Block::default().title(" Time Model ").borders(Borders::ALL));

    Widget::render(table, area, buf);
}

fn render_events(app: &App, area: Rect, buf: &mut Buffer) {
    let event_rows = compute::compute_top_events(&app.events, &app.filters, app.wall_ms);

    let rows: Vec<Row> = event_rows
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let style = if i == app.cursor {
                Style::default().bg(Color::DarkGray).fg(Color::White).add_modifier(Modifier::BOLD)
            } else {
                Style::default()
            };
            Row::new(vec![
                Cell::from(format!("{}", i + 1)),
                Cell::from(r.event_name.clone()),
                Cell::from(format!("{}", r.count)),
                Cell::from(format!("{:.1}", r.total_ms)),
                Cell::from(format!("{:.1}", r.avg_us)),
                Cell::from(format!("{:.1}", r.max_us)),
                Cell::from(format!("{:.1}%", r.pct_db)),
                Cell::from(format!("{:.2}", r.aas)),
            ])
            .style(style)
        })
        .collect();

    let table = Table::new(
        rows,
        [
            Constraint::Length(3),
            Constraint::Min(24),
            Constraint::Length(10),
            Constraint::Length(12),
            Constraint::Length(10),
            Constraint::Length(10),
            Constraint::Length(8),
            Constraint::Length(6),
        ],
    )
    .header(
        Row::new(vec!["#", "Wait Event", "Count", "Total(ms)", "Avg(us)", "Max(us)", "% DB", "AAS"])
            .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .bottom_margin(1),
    )
    .block(Block::default().title(" Top Events ").borders(Borders::ALL));

    Widget::render(table, area, buf);
}

fn render_sessions(app: &App, area: Rect, buf: &mut Buffer) {
    let session_rows = compute::compute_top_sessions(&app.events, &app.filters, app.wall_ms);

    let rows: Vec<Row> = session_rows
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let style = if i == app.cursor {
                Style::default().bg(Color::DarkGray).fg(Color::White).add_modifier(Modifier::BOLD)
            } else {
                Style::default()
            };
            Row::new(vec![
                Cell::from(format!("{}", i + 1)),
                Cell::from(format!("{}", r.pid)),
                Cell::from(format!("{:.1}", r.db_time_ms)),
                Cell::from(format!("{:.1}%", r.cpu_pct)),
                Cell::from(format!("{:.1}%", r.wait_pct)),
                Cell::from(r.top_wait.clone()),
            ])
            .style(style)
        })
        .collect();

    let table = Table::new(
        rows,
        [
            Constraint::Length(3),
            Constraint::Length(8),
            Constraint::Length(12),
            Constraint::Length(8),
            Constraint::Length(8),
            Constraint::Min(20),
        ],
    )
    .header(
        Row::new(vec!["#", "PID", "DB Time(ms)", "CPU%", "Wait%", "Top Wait"])
            .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .bottom_margin(1),
    )
    .block(Block::default().title(" Top Sessions ").borders(Borders::ALL));

    Widget::render(table, area, buf);
}

fn render_queries(app: &App, area: Rect, buf: &mut Buffer) {
    let query_rows = compute::compute_top_queries(&app.events, &app.filters, app.wall_ms);

    let rows: Vec<Row> = query_rows
        .iter()
        .enumerate()
        .map(|(i, r)| {
            let style = if i == app.cursor {
                Style::default().bg(Color::DarkGray).fg(Color::White).add_modifier(Modifier::BOLD)
            } else {
                Style::default()
            };
            Row::new(vec![
                Cell::from(format!("{}", i + 1)),
                Cell::from(format!("{}", r.query_id)),
                Cell::from(format!("{}", r.count)),
                Cell::from(format!("{:.1}", r.total_ms)),
                Cell::from(format!("{:.1}", r.avg_us)),
                Cell::from(format!("{:.1}%", r.pct_db)),
                Cell::from(r.top_wait.clone()),
            ])
            .style(style)
        })
        .collect();

    let table = Table::new(
        rows,
        [
            Constraint::Length(3),
            Constraint::Length(20),
            Constraint::Length(10),
            Constraint::Length(12),
            Constraint::Length(10),
            Constraint::Length(8),
            Constraint::Min(20),
        ],
    )
    .header(
        Row::new(vec!["#", "Query ID", "Count", "Total(ms)", "Avg(us)", "% DB", "Top Wait"])
            .style(Style::default().fg(Color::Cyan).add_modifier(Modifier::BOLD))
            .bottom_margin(1),
    )
    .block(Block::default().title(" Top Queries ").borders(Borders::ALL));

    Widget::render(table, area, buf);
}

fn render_footer(area: Rect, buf: &mut Buffer) {
    Paragraph::new(" Enter=drill  Esc=back  o/e/s/r=view  \\=clear filters  q=quit")
        .style(Style::default().fg(Color::White).bg(Color::DarkGray))
        .render(area, buf);
}

fn draw(app: &App, frame: &mut Frame) {
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Length(1), // header
            Constraint::Length(1), // tabs
            Constraint::Min(5),   // main content
            Constraint::Length(1), // footer
        ])
        .split(frame.area());

    render_header(app, chunks[0], frame.buffer_mut());
    render_tabs(app, chunks[1], frame.buffer_mut());

    match app.view {
        View::Overview => render_overview(app, chunks[2], frame.buffer_mut()),
        View::Events => render_events(app, chunks[2], frame.buffer_mut()),
        View::Sessions => render_sessions(app, chunks[2], frame.buffer_mut()),
        View::Queries => render_queries(app, chunks[2], frame.buffer_mut()),
    }

    render_footer(chunks[3], frame.buffer_mut());
}

// -- Main --------------------------------------------------------------------

fn main() -> io::Result<()> {
    let args = Args::parse();

    // Generate test data if requested
    if args.generate_test {
        let test_path = args.trace_dir.join("test-data.trace.lz4");
        eprintln!("Generating test trace file: {}", test_path.display());
        testgen::generate_test_file(&test_path)?;
        eprintln!("Done. Run without --generate-test to view.");
        return Ok(());
    }

    // Load events
    eprintln!("Loading trace files from {}...", args.trace_dir.display());
    let events = trace::load_events(&args.trace_dir, None, None)?;
    if events.is_empty() {
        eprintln!("No events found in trace files.");
        return Ok(());
    }
    eprintln!("Loaded {} events.", events.len());

    let app = App::new(events);

    // Dump mode — print summary and exit
    if args.dump {
        let tm = compute::compute_time_model(&app.events, &app.filters, app.wall_ms);
        println!("=== Time Model ===");
        println!("Time range: {:.1}s", app.wall_ms / 1000.0);
        println!("AAS: {:.2}", tm.aas);
        println!();
        for row in &tm.rows {
            let indent = "  ".repeat(row.indent as usize);
            println!("{}{:<30} {:>10.1} ms  {:>6.1}%  AAS {:.2}",
                indent, row.name, row.time_ms, row.pct_db_time, row.aas);
        }

        println!("\n=== Top Events ===");
        let events_view = compute::compute_top_events(&app.events, &app.filters, app.wall_ms);
        for (i, row) in events_view.iter().take(10).enumerate() {
            println!("  {:>2}  {:<28} {:>8} {:>10.1}ms {:>8.1}us {:>6.1}%  AAS {:.2}",
                i + 1, row.event_name, row.count, row.total_ms, row.avg_us, row.pct_db, row.aas);
        }

        println!("\n=== Top Sessions ===");
        let sessions = compute::compute_top_sessions(&app.events, &app.filters, app.wall_ms);
        for (i, row) in sessions.iter().take(10).enumerate() {
            println!("  {:>2}  PID {:>6}  {:>10.1}ms  CPU {:>5.1}%  Wait {:>5.1}%  {}",
                i + 1, row.pid, row.db_time_ms, row.cpu_pct, row.wait_pct, row.top_wait);
        }

        println!("\n=== Top Queries ===");
        let queries = compute::compute_top_queries(&app.events, &app.filters, app.wall_ms);
        for (i, row) in queries.iter().take(10).enumerate() {
            println!("  {:>2}  {:>20} {:>8} {:>10.1}ms {:>6.1}%  {}",
                i + 1, row.query_id, row.count, row.total_ms, row.pct_db, row.top_wait);
        }

        return Ok(());
    }

    let mut app = app;

    // Setup terminal
    enable_raw_mode()?;
    let mut stdout = io::stdout();
    execute!(stdout, EnterAlternateScreen)?;
    let backend = CrosstermBackend::new(stdout);
    let mut terminal = Terminal::new(backend)?;

    // Event loop
    loop {
        terminal.draw(|frame| draw(&app, frame))?;

        if event::poll(Duration::from_millis(100))? {
            if let Event::Key(key) = event::read()? {
                app.handle_key(key.code, key.modifiers);
            }
        }

        if app.should_quit {
            break;
        }
    }

    // Restore terminal
    disable_raw_mode()?;
    execute!(terminal.backend_mut(), LeaveAlternateScreen)?;
    Ok(())
}
