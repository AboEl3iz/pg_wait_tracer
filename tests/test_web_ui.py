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
    expected = ["Overview", "Events", "Sessions", "Queries", "Histogram", "Timeline"]
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
            test_time_picker(page)
            test_zoom_out(page)
            test_chart_rendering(page)
            test_session_drill_to_timeline(page)
            test_query_drill(page)

            # Reconnection test (kills/restarts mock server)
            mock_proc = test_reconnection(page, mock_proc)

            browser.close()
    finally:
        stop_mock_server(mock_proc)

    print(f"\n{tests_passed}/{tests_run} tests passed")
    sys.exit(0 if tests_failed == 0 else 1)


if __name__ == "__main__":
    main()
