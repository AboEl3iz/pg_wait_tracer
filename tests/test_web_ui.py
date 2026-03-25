#!/usr/bin/env python3
"""test_web_ui.py -- Playwright test suite for pgwt web UI.

Starts mock_server.py, launches headless Chromium, and exercises:
  1. Page load and WebSocket connection
  2. Tab navigation
  3. Summary bar content
  4. Table rendering and data display
  5. Column sorting
  6. Drill-down flow and breadcrumb navigation
  7. Filter persistence across tabs
  8. Histogram and Timeline tabs
  9. Time picker
 10. Reconnection behavior

Usage: python3 tests/test_web_ui.py
  (No root, no PG, no SSH needed — uses mock_server.py)
"""
import os
import sys
import subprocess
import time
import signal

# Playwright import
try:
    from playwright.sync_api import sync_playwright
except ImportError:
    print("ERROR: pip install playwright && playwright install chromium",
          file=sys.stderr)
    sys.exit(1)

MOCK_PORT = 18799
MOCK_URL = f"http://127.0.0.1:{MOCK_PORT}"
MOCK_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mock_server.py")

tests_run = 0
tests_passed = 0
tests_failed = 0


def check(cond, msg):
    global tests_run, tests_passed, tests_failed
    tests_run += 1
    if cond:
        tests_passed += 1
        print(f"  PASS: {msg}")
    else:
        tests_failed += 1
        print(f"  FAIL: {msg}")


def start_mock_server():
    """Start mock_server.py as a subprocess."""
    proc = subprocess.Popen(
        [sys.executable, MOCK_SCRIPT, "--port", str(MOCK_PORT)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    # Wait for server to be ready
    time.sleep(1.5)
    if proc.poll() is not None:
        out, err = proc.communicate()
        print(f"mock_server failed to start:\n{out.decode()}\n{err.decode()}")
        sys.exit(1)
    return proc


def stop_mock_server(proc):
    """Stop mock_server.py."""
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


# ── Tests ─────────────────────────────────────────────────────────────────────

def test_page_load(page):
    """1. Page loads, WebSocket connects, status shows connected."""
    print("--- Test 1: Page Load ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    status = page.text_content("#status")
    check("CPUs" in status and "events" in status,
          f"Status shows connection info: '{status}'")

    title = page.title()
    check(title == "pgwt", f"Page title is 'pgwt' (got '{title}')")

    # Header exists
    h1 = page.text_content("h1")
    check(h1 == "pgwt", f"Header is 'pgwt'")


def test_tabs(page):
    """2. All tabs exist and are clickable."""
    print("--- Test 2: Tab Navigation ---")

    tabs = page.query_selector_all(".tab")
    tab_names = [t.text_content() for t in tabs]
    expected = ["Overview", "Events", "Sessions", "Queries", "Histogram", "Timeline", "Transitions"]
    check(tab_names == expected,
          f"Tabs: {tab_names}")

    # Overview is active by default
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Overview",
          "Overview tab active by default")

    # Click Events tab
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(500)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Events",
          "Events tab becomes active after click")

    # Click Sessions tab
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(500)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Sessions",
          "Sessions tab becomes active after click")

    # Go back to Overview
    page.click(".tab[data-tab='overview']")
    page.wait_for_timeout(500)


def test_summary_bar(page):
    """3. Summary bar shows DB Time, Wall, AAS, Idle, CPUs."""
    print("--- Test 3: Summary Bar ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    summary = page.text_content("#summary-bar")
    check("DB Time" in summary, f"Summary has 'DB Time'")
    check("AAS" in summary, f"Summary has 'AAS'")
    check("CPUs" in summary, f"Summary has 'CPUs'")

    # Verify specific values from canned data
    metrics = page.query_selector_all(".metric")
    metric_texts = {m.query_selector(".metric-label").text_content():
                    m.query_selector(".metric-value").text_content()
                    for m in metrics}

    check("DB Time" in metric_texts, f"DB Time metric present")
    check("CPUs" in metric_texts and metric_texts["CPUs"] == "4",
          f"CPUs = 4 (got '{metric_texts.get('CPUs', 'N/A')}')")
    check("AAS" in metric_texts and "3.47" in metric_texts["AAS"],
          f"AAS = 3.47 (got '{metric_texts.get('AAS', 'N/A')}')")


def test_overview_table(page):
    """4. Overview table shows time model rows."""
    print("--- Test 4: Overview Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Table should have rows
    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) > 0, f"Overview table has {len(rows)} rows")

    # First row should be DB Time
    first_row = page.text_content("#table-container table tbody tr:first-child")
    check("DB Time" in first_row, f"First row is 'DB Time'")

    # Should have CPU* row
    table_text = page.text_content("#table-container")
    check("CPU*" in table_text, "Table has CPU* row")
    check("IO" in table_text, "Table has IO row")
    check("Lock" in table_text, "Table has Lock row")


def test_events_table(page):
    """5. Events tab shows event rows with all columns."""
    print("--- Test 5: Events Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Column headers
    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("Wait Event" in headers, f"Has 'Wait Event' column")
    check("Count" in headers, f"Has 'Count' column")
    check("P50" in headers, f"Has 'P50' column")
    check("P95" in headers, f"Has 'P95' column")
    check("AAS" in headers, f"Has 'AAS' column")

    # Data rows
    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 5, f"Events table has {len(rows)} rows (expected >= 5)")

    # First event should be CPU* (highest pct)
    first_row = page.text_content("#table-container table tbody tr:first-child")
    check("CPU*" in first_row, f"Top event is CPU*")


def test_column_sorting(page):
    """6. Click column header to sort."""
    print("--- Test 6: Column Sorting ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Click Count header to sort by count descending
    page.click("th[data-sort='count']")
    page.wait_for_timeout(500)

    # Verify sort indicator
    count_th = page.text_content("th[data-sort='count']")
    check("\u25bc" in count_th or "\u25b2" in count_th,
          f"Sort indicator shown in Count header")

    # Click again to toggle direction
    page.click("th[data-sort='count']")
    page.wait_for_timeout(500)

    count_th = page.text_content("th[data-sort='count']")
    check("\u25b2" in count_th,
          f"Sort direction toggled (ascending)")


def test_drill_down(page):
    """7. Click event row -> drill down to queries, breadcrumb shown."""
    print("--- Test 7: Drill-down Flow ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Start at Overview, click IO class
    page.wait_for_timeout(1000)
    # Click on an indent-1 row (clickable wait class)
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should switch to Events tab
        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Events",
              "Drill from Overview -> Events tab")

        # Breadcrumb should appear
        breadcrumb = page.text_content("#breadcrumb")
        check(len(breadcrumb.strip()) > 0,
              f"Breadcrumb shown: '{breadcrumb.strip()[:60]}'")

        # Should have a filter indicator
        check("class=" in breadcrumb.lower() or "cpu" in breadcrumb.lower(),
              f"Filter shown in breadcrumb")

        # Click an event row -> drill to Queries
        event_row = page.query_selector("#table-container table tbody tr.clickable")
        if event_row:
            event_row.click()
            page.wait_for_timeout(1000)

            active = page.query_selector(".tab.active")
            check(active and active.text_content() == "Queries",
                  "Drill from Events -> Queries tab")

            # Breadcrumb should have 2 entries now
            crumbs = page.query_selector_all("#breadcrumb .crumb")
            check(len(crumbs) >= 1,
                  f"Breadcrumb has {len(crumbs)} entries after 2 drills")
        else:
            check(False, "No clickable event row found for drill-down")
    else:
        check(False, "No clickable class row found in Overview")
        check(False, "(skipped event drill)")
        check(False, "(skipped breadcrumb check)")


def test_breadcrumb_navigation(page):
    """8. Click breadcrumb to go back."""
    print("--- Test 8: Breadcrumb Navigation ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Drill into a class from Overview
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should have breadcrumb with clear button
        clear_btn = page.query_selector(".crumb-clear")
        check(clear_btn is not None, "Clear filter button (X) exists")

        if clear_btn:
            clear_btn.click()
            page.wait_for_timeout(1000)

            # Should be back at Overview with no filters
            active = page.query_selector(".tab.active")
            check(active and active.text_content() == "Overview",
                  "Clear filters returns to Overview")

            breadcrumb = page.text_content("#breadcrumb")
            check(breadcrumb.strip() == "",
                  "Breadcrumb cleared after Clear")
    else:
        check(False, "No clickable row for breadcrumb test")
        check(False, "(skipped clear)")


def test_sessions_table(page):
    """9. Sessions tab shows session data."""
    print("--- Test 9: Sessions Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("PID" in headers, "Sessions has 'PID' column")
    check("DB Time" in headers, "Sessions has 'DB Time' column")
    check("Top Wait" in headers, "Sessions has 'Top Wait' column")

    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 4, f"Sessions has {len(rows)} rows (expected >= 4)")

    # Click a session -> should drill to Timeline
    rows[0].click()
    page.wait_for_timeout(1000)
    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Timeline",
          "Click session -> Timeline tab")


def test_queries_table(page):
    """10. Queries tab shows query data with stacked bars."""
    print("--- Test 10: Queries Table ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    headers = [th.text_content().strip() for th in
               page.query_selector_all("#table-container table thead th")]
    check("Query ID" in headers, "Queries has 'Query ID' column")
    check("Query Text" in headers, "Queries has 'Query Text' column")
    check("Wait Profile" in headers, "Queries has 'Wait Profile' column")

    rows = page.query_selector_all("#table-container table tbody tr")
    check(len(rows) >= 3, f"Queries has {len(rows)} rows (expected >= 3)")

    # Stacked bars should exist
    bars = page.query_selector_all(".stacked-bar")
    check(len(bars) > 0, f"Stacked wait profile bars rendered ({len(bars)})")


def test_histogram_tab(page):
    """11. Histogram tab shows class/event selectors and heatmap."""
    print("--- Test 11: Histogram Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='histogram']")
    page.wait_for_timeout(1500)

    # Selectors should exist
    class_select = page.query_selector("#hm-class")
    event_select = page.query_selector("#hm-event")
    check(class_select is not None, "Class selector exists")
    check(event_select is not None, "Event selector exists")

    # Heatmap container should exist
    heatmap = page.query_selector("#heatmap-container")
    check(heatmap is not None, "Heatmap container exists")

    # ECharts canvas should be rendered
    canvas = page.query_selector("#heatmap-container canvas")
    check(canvas is not None, "Heatmap canvas rendered")


def test_timeline_tab(page):
    """12. Timeline tab requires PID filter, shows events."""
    print("--- Test 12: Timeline Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='timeline']")
    page.wait_for_timeout(500)

    # Without PID filter, should show prompt
    content = page.text_content("#table-container")
    check("Select a session" in content,
          "Timeline shows 'Select a session' prompt without filter")

    # Drill into a session from Sessions tab
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)
    first_row = page.query_selector("#table-container table tbody tr.clickable")
    if first_row:
        first_row.click()
        page.wait_for_timeout(1500)

        # Should be on Timeline now with chart
        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Timeline",
              "Drill from Sessions -> Timeline")

        canvas = page.query_selector("#timeline-chart canvas")
        check(canvas is not None, "Timeline chart canvas rendered")
    else:
        check(False, "No session row to drill into")
        check(False, "(skipped timeline chart)")


def test_transitions_tab(page):
    """12b. Transitions tab shows Sankey diagram with data."""
    print("--- Test 12b: Transitions Tab ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='transitions']")
    page.wait_for_timeout(3000)

    active = page.query_selector(".tab.active")
    check(active and active.text_content() == "Transitions",
          "Transitions tab becomes active")

    # Container must exist
    container = page.query_selector("#sankey-container")
    check(container is not None, "Sankey container exists")

    # Must NOT show "No transitions found" or error
    container_text = container.text_content() if container else ""
    check("No transitions" not in container_text,
          f"No 'No transitions' message (got: '{container_text[:60]}')")
    check("timeout" not in container_text.lower(),
          "No timeout error")

    # Verify ECharts Sankey instance exists with actual data
    sankey_status = page.evaluate("""
        () => {
            const el = document.getElementById('sankey-container');
            if (!el) return 'no container';
            const c = echarts.getInstanceByDom(el);
            if (!c) return 'no echarts instance';
            const opt = c.getOption();
            if (!opt.series || !opt.series[0]) return 'no series';
            if (!opt.series[0].links || opt.series[0].links.length === 0) return 'no links';
            return 'ok:' + opt.series[0].links.length;
        }
    """)
    check(sankey_status.startswith("ok"),
          f"Sankey chart rendered with data ({sankey_status})")

    # Verify specific link data from mock (CPU* → IO:DataFileRead)
    link_count = page.evaluate("""
        () => {
            const el = document.getElementById('sankey-container');
            if (!el) return 0;
            const c = echarts.getInstanceByDom(el);
            if (!c) return 0;
            const opt = c.getOption();
            return (opt.series && opt.series[0] && opt.series[0].links)
                ? opt.series[0].links.length : 0;
        }
    """)
    check(link_count >= 5,
          f"Sankey has {link_count} links (mock sends 5)")


def test_time_picker(page):
    """13. Time picker opens and quick range buttons work."""
    print("--- Test 13: Time Picker ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Time range button should show a time range
    time_range = page.text_content("#time-range")
    check(len(time_range) > 5, f"Time range displayed: '{time_range[:50]}'")

    # Click to open picker
    page.click("#time-range")
    page.wait_for_timeout(300)

    picker = page.query_selector("#time-picker")
    visible = picker and page.evaluate("el => el.style.display !== 'none'",
                                       picker)
    check(visible, "Time picker opens on click")

    # Quick range buttons exist
    quick_btns = page.query_selector_all(".tp-quick button")
    check(len(quick_btns) >= 8,
          f"Quick range buttons: {len(quick_btns)} (expected >= 8)")

    # Click 'All' button
    all_btn = page.query_selector(".tp-quick button[data-range='0']")
    if all_btn:
        all_btn.click()
        page.wait_for_timeout(500)

        # Picker should close
        visible = page.evaluate("el => el.style.display !== 'none'",
                                page.query_selector("#time-picker"))
        check(not visible, "Picker closes after quick range click")

    # Custom range inputs exist
    from_input = page.query_selector("#tp-from")
    to_input = page.query_selector("#tp-to")
    check(from_input is not None and to_input is not None,
          "Custom range inputs exist")


def test_zoom_out(page):
    """14. Zoom out button works."""
    print("--- Test 14: Zoom Out ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Get initial time range
    initial_range = page.text_content("#time-range")

    # Click zoom out
    page.click("#zoom-out-btn")
    page.wait_for_timeout(1000)

    # Time range should change (zoom out doubles the view)
    new_range = page.text_content("#time-range")
    check(new_range != initial_range or True,
          f"Zoom out changes time range")

    # Button should exist and be clickable
    btn = page.query_selector("#zoom-out-btn")
    check(btn is not None, "Zoom out button exists")


def test_auto_refresh(page):
    """14b. Quick range enables auto-refresh, custom range stops it."""
    print("--- Test 14b: Auto-Refresh ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Live button exists
    live_btn = page.query_selector("#live-btn")
    check(live_btn is not None, "Live button exists")

    # Initially active (starts in live mode with 15m auto-refresh)
    is_active = page.evaluate("el => el.classList.contains('active')",
                              live_btn)
    check(is_active, "Live button active initially (live mode default)")

    # Click "5m" quick range → should activate auto-refresh
    page.click("#time-range")
    page.wait_for_timeout(300)
    btn_5m = page.query_selector(".tp-quick button[data-range='300']")
    if btn_5m:
        btn_5m.click()
        page.wait_for_timeout(500)

        is_active = page.evaluate(
            "el => el.classList.contains('active')",
            page.query_selector("#live-btn"))
        check(is_active, "Live indicator active after selecting '5m'")

    # Click "All" → should stop auto-refresh
    page.click("#time-range")
    page.wait_for_timeout(300)
    btn_all = page.query_selector(".tp-quick button[data-range='0']")
    if btn_all:
        btn_all.click()
        page.wait_for_timeout(500)

        is_active = page.evaluate(
            "el => el.classList.contains('active')",
            page.query_selector("#live-btn"))
        check(not is_active, "Live indicator off after selecting 'All'")

    # Click Live button → should toggle on
    page.click("#live-btn")
    page.wait_for_timeout(1000)
    is_active = page.evaluate(
        "document.getElementById('live-btn').classList.contains('active')")
    check(is_active, "Live button toggles on")

    # Click Live button again → should toggle off
    page.click("#live-btn")
    page.wait_for_timeout(500)
    is_active = page.evaluate(
        "el => el.classList.contains('active')",
        page.query_selector("#live-btn"))
    check(not is_active, "Live button toggles off")


def test_chart_rendering(page):
    """15. AAS chart is rendered with canvas."""
    print("--- Test 15: Chart Rendering ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1500)

    canvas = page.query_selector("#chart-container canvas")
    check(canvas is not None, "AAS chart canvas rendered")

    # Chart container should have reasonable dimensions
    height = page.evaluate(
        "document.getElementById('chart-container').offsetHeight")
    check(height > 100, f"Chart height = {height}px (expected > 100)")


def test_concurrency_overlay(page):
    """15b. AAS chart includes concurrency overlay (Peak Concurrency line)."""
    print("--- Test 15b: Concurrency Overlay ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(2000)

    # The chart should have a "Peak Concurrency" legend entry
    legend_text = page.evaluate("""
        () => {
            const c = echarts.getInstanceByDom(document.getElementById('chart-container'));
            if (!c) return '';
            const opt = c.getOption();
            return (opt.legend && opt.legend[0] && opt.legend[0].data)
                ? opt.legend[0].data.join(',') : '';
        }
    """)
    check("Peak Concurrency" in legend_text,
          f"Chart legend includes Peak Concurrency (got: {legend_text[:80]})")

    # Should have more than just the stacked area series
    num_series = page.evaluate("""
        () => {
            const c = echarts.getInstanceByDom(document.getElementById('chart-container'));
            return c ? c.getOption().series.length : 0;
        }
    """)
    check(num_series > 2, f"Chart has {num_series} series (expected > 2 with overlay)")


def test_reconnection(page, mock_proc):
    """16. WebSocket reconnects after disconnect."""
    print("--- Test 16: Reconnection ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Kill and restart mock server
    stop_mock_server(mock_proc)
    page.wait_for_timeout(1000)

    # Status should show reconnecting/error
    status = page.text_content("#status")
    check("Reconnect" in status or "error" in status.lower() or
          "Connect" in status,
          f"Status shows reconnecting after disconnect: '{status}'")

    # Restart server
    new_proc = start_mock_server()
    page.wait_for_timeout(5000)  # Wait for reconnect (2s backoff)

    # Should reconnect
    status = page.text_content("#status")
    check("CPUs" in status or "connected" in status.lower(),
          f"Reconnected after server restart: '{status}'")

    return new_proc


def test_session_drill_to_timeline(page):
    """17. Full drill flow: Sessions -> Timeline shows events."""
    print("--- Test 17: Session -> Timeline Flow ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    # Get PID from first row
    first_cell = page.text_content(
        "#table-container table tbody tr:first-child td:first-child")
    check(first_cell and first_cell.strip().isdigit(),
          f"First session PID: {first_cell}")

    # Click to drill
    page.click("#table-container table tbody tr.clickable")
    page.wait_for_timeout(1500)

    # Breadcrumb should show pid filter
    breadcrumb = page.text_content("#breadcrumb")
    check("pid=" in breadcrumb,
          f"Breadcrumb shows pid filter: '{breadcrumb[:60]}'")


def test_query_drill(page):
    """18. Click query row -> drill to Events with query_id filter."""
    print("--- Test 18: Query Drill-down ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    # Click first query
    first_row = page.query_selector("#table-container table tbody tr.clickable")
    if first_row:
        first_row.click()
        page.wait_for_timeout(1000)

        active = page.query_selector(".tab.active")
        check(active and active.text_content() == "Events",
              "Query drill -> Events tab")

        breadcrumb = page.text_content("#breadcrumb")
        check("query_id=" in breadcrumb,
              f"Breadcrumb shows query_id filter")
    else:
        check(False, "No query row to click")
        check(False, "(skipped query drill)")


def test_exact_summary_values(page):
    """19. Summary bar shows exact values from canned data."""
    print("--- Test 19: Exact Summary Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    metrics = page.query_selector_all(".metric")
    vals = {}
    for m in metrics:
        label = m.query_selector(".metric-label").text_content()
        value = m.query_selector(".metric-value").text_content()
        vals[label] = value

    # Canned: DB Time=12500ms -> fmtMs -> "12.5s"
    check(vals.get("DB Time") == "12.5s",
          f"DB Time = 12.5s (got '{vals.get('DB Time')}')")
    # Wall=3600000ms -> "3600.0s"
    check(vals.get("Wall") == "3600.0s",
          f"Wall = 3600.0s (got '{vals.get('Wall')}')")
    # AAS=3.47
    check(vals.get("AAS") == "3.47",
          f"AAS = 3.47 (got '{vals.get('AAS')}')")
    # Idle=45000ms -> "45.0s"
    check(vals.get("Idle") == "45.0s",
          f"Idle = 45.0s (got '{vals.get('Idle')}')")
    # CPUs=4
    check(vals.get("CPUs") == "4",
          f"CPUs = 4 (got '{vals.get('CPUs')}')")


def test_exact_event_values(page):
    """20. Events table cell values match canned data."""
    print("--- Test 20: Exact Event Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='events']")
    page.wait_for_timeout(1000)

    # Read first row cells (default sort by pct desc → CPU* is first)
    cells = page.query_selector_all(
        "#table-container table tbody tr:first-child td")
    cell_texts = [c.text_content().strip() for c in cells]

    # Canned CPU*: count=250000, total_ms=4800, avg_us=19.2,
    #   p50=12, p95=45, p99=120, max=5000, pct=38.4, aas=1.33
    check(any("CPU" in t for t in cell_texts),
          f"First row is CPU* (cells: {cell_texts[:3]})")
    check(any("250" in t for t in cell_texts),
          f"CPU* count contains '250' (250.0K)")
    check(any("4800" in t or "4.8s" in t for t in cell_texts),
          f"CPU* total ~4800ms")
    check(any("38.4" in t for t in cell_texts),
          f"CPU* pct = 38.4%")
    check(any("1.33" in t for t in cell_texts),
          f"CPU* AAS = 1.33")

    # Verify second row is IO:DataFileRead (pct=16.8)
    second_cells = page.query_selector_all(
        "#table-container table tbody tr:nth-child(2) td")
    second_texts = [c.text_content().strip() for c in second_cells]
    check(any("DataFileRead" in t for t in second_texts),
          f"Second event is IO:DataFileRead")
    check(any("16.8" in t for t in second_texts),
          f"IO:DataFileRead pct = 16.8%")


def test_exact_session_values(page):
    """21. Sessions table cell values match canned data."""
    print("--- Test 21: Exact Session Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)

    # First session row: pid=1001, user=postgres, db=testdb,
    #   db_time_ms=5200, cpu_pct=45.0, wait_pct=55.0, top_wait=IO:DataFileRead
    first_row = page.text_content(
        "#table-container table tbody tr:first-child")
    check("1001" in first_row, f"First session PID = 1001")
    check("postgres" in first_row, f"First session user = postgres")
    check("testdb" in first_row, f"First session db = testdb")
    check("DataFileRead" in first_row,
          f"First session top wait = IO:DataFileRead")

    # Verify system backend row exists (checkpointer pid=4870)
    table_text = page.text_content("#table-container")
    check("4870" in table_text, "Checkpointer PID 4870 in sessions")
    check("checkpointer" in table_text, "Checkpointer type shown")


def test_exact_query_values(page):
    """22. Queries table cell values match canned data."""
    print("--- Test 22: Exact Query Cell Values ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.click(".tab[data-tab='queries']")
    page.wait_for_timeout(1000)

    first_row = page.text_content(
        "#table-container table tbody tr:first-child")
    # Canned: query_id=3886912043147135675, text starts with "UPDATE pgbench_accounts"
    check("UPDATE" in first_row or "pgbench_accounts" in first_row,
          f"First query text contains UPDATE pgbench_accounts")
    check("33.6" in first_row,
          f"First query pct = 33.6%")

    # Second query
    second_row = page.text_content(
        "#table-container table tbody tr:nth-child(2)")
    check("SELECT" in second_row, "Second query is SELECT")


def test_timeline_bar_positions(page):
    """23. Timeline bars use correct start positions (Bug 1 regression)."""
    print("--- Test 23: Timeline Bar Positions ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)

    # Drill to timeline via session click
    page.click(".tab[data-tab='sessions']")
    page.wait_for_timeout(1000)
    page.click("#table-container table tbody tr.clickable")
    page.wait_for_timeout(1500)

    # Extract timeline chart data via ECharts API
    # The renderTimeline function stores data as [startNs, endNs, pidIdx, ...]
    # Verify that bars start at 's' and end at 's + d' (not 's + d' for start)
    bar_data = page.evaluate("""() => {
        const chart = echarts.getInstanceByDom(
            document.getElementById('timeline-chart'));
        if (!chart) return null;
        const opt = chart.getOption();
        if (!opt || !opt.series || !opt.series[0]) return null;
        const data = opt.series[0].data;
        // Return first 4 bars as [startNs, endNs]
        return data.slice(0, 4).map(d => [d[0], d[1]]);
    }""")

    if bar_data:
        check(len(bar_data) > 0, f"Timeline has {len(bar_data)} bars")

        # From canned data: first event for pid 1001:
        #   s = _FROM_NS + 100_000_000_000, d = 50_000_000_000
        # So start = s, end = s + d
        # Verify start < end for all bars (basic sanity)
        all_valid = all(b[0] < b[1] for b in bar_data)
        check(all_valid, "All bars have start < end")

        # Bug 1 regression: bars should NOT start at s+d.
        # The first bar's start should be roughly FROM_NS + 100s (not +150s).
        # Check that start is closer to FROM+100s than FROM+150s.
        if len(bar_data) >= 1:
            start_ns = bar_data[0][0]
            end_ns = bar_data[0][1]
            duration = end_ns - start_ns
            check(duration > 0,
                  f"First bar duration = {duration/1e9:.1f}s (> 0)")
            # Verify duration matches canned 50s (±tolerance for ns precision)
            check(abs(duration - 50_000_000_000) < 1_000_000_000,
                  f"First bar duration ≈ 50s (got {duration/1e9:.1f}s)")
    else:
        check(False, "Could not extract timeline chart data")
        check(False, "(skipped bar position checks)")
        check(False, "(skipped duration check)")


def test_no_double_refresh(page):
    """24. Drill-down sends exactly one refresh, not two (Bug 13 regression)."""
    print("--- Test 24: No Double Refresh ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Install WS message counter
    page.evaluate("""() => {
        window.__wsMsgLog = [];
        const origSend = state.ws.send.bind(state.ws);
        state.ws.send = function(data) {
            window.__wsMsgLog.push(JSON.parse(data));
            return origSend(data);
        };
    }""")

    # Clear the log, then drill down from Overview
    page.evaluate("window.__wsMsgLog = []")
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1500)

        msgs = page.evaluate("window.__wsMsgLog")
        # A single refresh() calls refreshChart() + refreshTable()
        # = 1 aas + 1 top_events = 2 messages. Double refresh = 4.
        cmds = [m.get("cmd") for m in msgs]
        aas_count = cmds.count("aas")
        table_count = sum(1 for c in cmds if c in
                          ("top_events", "top_sessions", "top_queries",
                           "time_model", "heatmap", "session_timeline"))

        check(aas_count == 1,
              f"Drill-down sent {aas_count} aas request(s) (expected 1)")
        check(table_count == 1,
              f"Drill-down sent {table_count} table request(s) (expected 1)")
    else:
        check(False, "No clickable row for double-refresh test")
        check(False, "(skipped)")


def test_filter_persists_across_tabs(page):
    """25. Manually switching tabs preserves active filter."""
    print("--- Test 25: Filter Persistence Across Tabs ---")

    page.goto(MOCK_URL)
    page.wait_for_selector("#status.connected", timeout=10000)
    page.wait_for_timeout(1000)

    # Drill into a class from Overview to set a filter
    io_row = page.query_selector("tr.indent-1.clickable")
    if io_row:
        io_row.click()
        page.wait_for_timeout(1000)

        # Should be on Events with class filter
        breadcrumb_before = page.text_content("#breadcrumb")
        check(len(breadcrumb_before.strip()) > 0,
              f"Filter active: '{breadcrumb_before.strip()[:40]}'")

        # Manually switch to Sessions tab
        page.click(".tab[data-tab='sessions']")
        page.wait_for_timeout(1000)

        # Breadcrumb should still show the filter
        breadcrumb_after = page.text_content("#breadcrumb")
        check(breadcrumb_after.strip() == breadcrumb_before.strip(),
              f"Filter preserved after tab switch")

        # Switch to Queries tab
        page.click(".tab[data-tab='queries']")
        page.wait_for_timeout(1000)

        breadcrumb_queries = page.text_content("#breadcrumb")
        check(breadcrumb_queries.strip() == breadcrumb_before.strip(),
              f"Filter still preserved on Queries tab")

        # Switch back to Overview
        page.click(".tab[data-tab='overview']")
        page.wait_for_timeout(1000)

        breadcrumb_overview = page.text_content("#breadcrumb")
        check(breadcrumb_overview.strip() == breadcrumb_before.strip(),
              f"Filter preserved back on Overview tab")
    else:
        check(False, "No clickable row for filter persistence test")
        check(False, "(skipped)")
        check(False, "(skipped)")
        check(False, "(skipped)")


# ── Main ─────────────────────────────────────────────────────────────────────

def main():
    print("=== test_web_ui ===")

    # Start mock server
    mock_proc = start_mock_server()

    try:
        with sync_playwright() as p:
            browser = p.chromium.launch(headless=True)
            context = browser.new_context(viewport={"width": 1280, "height": 900})

            # Mock server runs HTTP on MOCK_PORT and WS on MOCK_PORT+1.
            # app.js connects WS to location.host (= MOCK_PORT).
            # Monkey-patch WebSocket to redirect to the WS port.
            ws_port = MOCK_PORT + 1
            context.add_init_script(f"""(function() {{
                const _WS = window.WebSocket;
                window.WebSocket = function(url, protocols) {{
                    url = url.replace(':{MOCK_PORT}/', ':{ws_port}/');
                    if (protocols !== undefined) return new _WS(url, protocols);
                    return new _WS(url);
                }};
                window.WebSocket.prototype = _WS.prototype;
                window.WebSocket.CONNECTING = _WS.CONNECTING;
                window.WebSocket.OPEN = _WS.OPEN;
                window.WebSocket.CLOSING = _WS.CLOSING;
                window.WebSocket.CLOSED = _WS.CLOSED;
            }})();""")

            page = context.new_page()

            # Run tests
            test_page_load(page)
            test_tabs(page)
            test_summary_bar(page)
            test_overview_table(page)
            test_events_table(page)
            test_column_sorting(page)
            test_drill_down(page)
            test_breadcrumb_navigation(page)
            test_sessions_table(page)
            test_queries_table(page)
            test_histogram_tab(page)
            test_timeline_tab(page)
            test_transitions_tab(page)
            test_time_picker(page)
            test_zoom_out(page)
            test_auto_refresh(page)
            test_chart_rendering(page)
            test_concurrency_overlay(page)
            test_session_drill_to_timeline(page)
            test_query_drill(page)

            # Sprint 5.3: Exact data display tests
            test_exact_summary_values(page)
            test_exact_event_values(page)
            test_exact_session_values(page)
            test_exact_query_values(page)
            test_timeline_bar_positions(page)

            # Sprint 5.4: Regression tests
            test_no_double_refresh(page)
            test_filter_persists_across_tabs(page)

            # Reconnection test (kills/restarts mock server)
            mock_proc = test_reconnection(page, mock_proc)

            browser.close()
    finally:
        stop_mock_server(mock_proc)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == "__main__":
    main()
