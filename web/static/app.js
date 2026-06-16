/* pgwt — Web Investigation Client: bootstrap + router.
 *
 * B3 (complete): the old ~2000-line monolith is now fully restructured into
 * native ES modules (no build step — still embeddable via go:embed). THIS FILE
 * IS BOOTSTRAP-ONLY: WebSocket lifecycle, the time-window / live-mode controls,
 * tab routing, and view registration. It contains NO view-specific rendering and
 * NO module-level chart variables — every view (active, overview, events,
 * sessions, queries, histogram, timeline, transitions, concurrency) lives in
 * views/ as { id, requests, build (PURE), mount, enter, leave } and OWNS its own
 * ECharts instance (created on enter/mount, disposed in leave).
 *
 * State is explicit (lib/state.js): no grab-bag global mutated mid-flight.
 * Stale-response superseding is structural (lib/transport.js channels +
 * lib/view-manager.js epochs), replacing the old _refreshGen counters.
 */

import { ServerInfo, TimeRange, FilterStack } from './lib/state.js';
import { Transport } from './lib/transport.js';
import { ViewManager } from './lib/view-manager.js';
import { mountTable } from './lib/table.js';
import { classColor, fmtTime, fmtDuration, esc } from './lib/format.js';
import { controlStatus, controlMetrics, ControlUnavailable } from './lib/control.js';
import { mountEscalateControl, mountMetricsPanel } from './lib/panels.js';
import { createActiveView } from './views/active.js';
import { createOverviewView } from './views/overview.js';
import { createEventsView } from './views/events.js';
import { createSessionsView } from './views/sessions.js';
import { createQueriesView } from './views/queries.js';
import { createHistogramView } from './views/histogram.js';
import { createTimelineView } from './views/timeline.js';
import { createTransitionsView } from './views/transitions.js';
import { createConcurrencyView } from './views/concurrency.js';

// ── Core services ─────────────────────────────────────────────────────────────

const server = new ServerInfo();
const timeRange = new TimeRange(server);
const filters = new FilterStack();
const transport = new Transport();

let activeView = null;       // persistent AAS chart view ("active")
let vm = null;               // ViewManager for tab views

// ── Daemon control plane (B5) ─────────────────────────────────────────────────
// Latest daemon status/metrics from the control socket (proxied by pgwt-server
// as the `control` command). null until first poll; `available` flips false the
// first time the daemon is unreachable (static-trace replay) so we stop polling
// and hide the escalation UI. Views read `daemon.status` synchronously via the
// ctx hook to render the AAS escalation annotation + unavailable panels.
const daemon = { status: null, metrics: null, available: true, polled: false };
let metricsPanelOpen = false;
let daemonPollId = 0;

// Reconnect bookkeeping (lifecycle only — not request state).
let reconnectDelay = 2000;
let reconnectTimer = null;

// Auto-refresh (live mode) bookkeeping.
let autoRefreshId = 0;
let autoRefreshOn = false;

// Sort state per table view. getSort returns the current { key, asc } (or null
// for server order); toggleSort cycles desc→asc on repeated clicks of the same
// column. Kept here (not in a view) so it survives tab switches, matching the
// old per-tab behavior.
const tabSort = {};   // tab -> { key, asc }
function getSort(tab) { return tabSort[tab] || null; }
function toggleSort(tab, key) {
    const cur = tabSort[tab];
    if (cur && cur.key === key) tabSort[tab] = { key, asc: !cur.asc };
    else tabSort[tab] = { key, asc: false };
}

// ── DOM handles (resolved once at bootstrap) ──────────────────────────────────

let chartEl, tableEl, summaryEl, tooltipEl;

// ── Shared ctx handed to every view hook ──────────────────────────────────────

function makeCtx() {
    return {
        transport, server, timeRange, filters,
        echarts: window.echarts,
        chartEl, summaryEl, tooltipEl,
        mountTable,
        setStatus,
        onDrill: drill,
        onZoom: (from, to) => { stopAutoRefresh(); zoomTo(from, to); },
        // Table views: per-tab sort state + a re-render hook used after a
        // header-sort click (re-runs requests/build/mount under a fresh epoch).
        getSort, toggleSort,
        refresh: () => vm.refresh(),
        // Daemon control plane (B5): views read the latest escalation status to
        // render the AAS annotation + the unavailable/escalate panels, and can
        // re-poll it after an escalate/deescalate action.
        getEscalationStatus: () => daemon.status,
        refreshEscalationStatus: () => pollDaemon(),
        onEscalationChanged: () => { renderEscalateControl(); refreshActive(); },
    };
}

// ── WebSocket lifecycle ───────────────────────────────────────────────────────

function connect() {
    if (reconnectTimer) { clearTimeout(reconnectTimer); reconnectTimer = null; }
    setStatus('Connecting...', 'connecting');
    const ws = new WebSocket('ws://' + location.host + '/ws');

    ws.onopen = () => {
        transport.attach(ws);
        reconnectDelay = 2000;
        init();
    };
    ws.onclose = () => {
        transport.ws = null;
        transport.rejectAll('disconnected');
        stopDaemonPoll();
        scheduleReconnect();
    };
    ws.onerror = () => { /* onclose follows */ };
}

function scheduleReconnect() {
    const delay = reconnectDelay;
    reconnectDelay = Math.min(delay * 2, 16000);
    setStatus('Reconnecting in ' + (delay / 1000) + 's...', 'error');
    reconnectTimer = setTimeout(connect, delay);
}

// ── Init ──────────────────────────────────────────────────────────────────────

async function init() {
    setStatus('Loading...', 'connecting');
    try {
        const info = await transport.send('info');
        server.update(info);
        timeRange.initDefault();
        setStatus(server.numCpus + ' CPUs', 'connected');

        updateTimeRange();
        initChartView();
        initChartResize();
        initTabs();
        initTimePicker();
        initLiveMode();
        initMetricsButton();
        window.addEventListener('resize', onResize);

        // Poll the daemon control plane BEFORE the first paint so the AAS
        // escalation annotation + the escalate control are correct on load.
        await pollDaemon();
        startDaemonPoll();

        await refresh();
        startAutoRefresh(900);   // start in live mode (last 15 min)
    } catch (e) {
        setStatus('Error: ' + e.message, 'error');
    }
}

// ── Refresh orchestration ─────────────────────────────────────────────────────
//
// One refresh = (1) re-render the persistent active/AAS chart, then (2) ask the
// view-manager to refresh the active tab view. Both run under transport
// single-flight + the view-manager epoch chokepoint, so a user action mid-flight
// supersedes stale responses without any manual generation counters.

async function refresh() {
    await refreshActive();
    await vm.refresh();
}

async function refreshActive() {
    if (!activeView) return;
    const ctx = activeCtx();
    let data;
    try {
        data = await activeView.requests(ctx);
    } catch (e) {
        return;  // superseded / disconnected: keep current paint
    }
    let model;
    try { model = activeView.build(data, ctx); } catch (e) { return; }
    try { activeView.mount(chartEl, model, ctx); } catch (e) { /* best-effort */ }
}

// ctx for the persistent active view: it manages its own single-flight channel
// (not tied to the tab view-manager) so tab switches never cancel the AAS fetch.
function activeCtx() {
    return Object.assign(makeCtx(), {
        viewId: 'active',
        channel: (name) => 'active.' + name,
    });
}

// ── Chart view (persistent AAS) ───────────────────────────────────────────────

function initChartView() {
    activeView = createActiveView();
    activeView.enter(activeCtx());
    // Double-click to zoom out (kept from the old chart behavior).
    chartEl.addEventListener('dblclick', (e) => { e.preventDefault(); stopAutoRefresh(); zoomOut(); });
}

function onResize() {
    if (activeView && activeView.resize) activeView.resize();
    if (vm.active && vm.active.resize) vm.active.resize();
}

// ── Tab routing via the view-manager ──────────────────────────────────────────

function initTabs() {
    vm = new ViewManager(makeCtx());
    vm.setContainer(tableEl);

    vm.register(createOverviewView());
    vm.register(createEventsView());
    vm.register(createSessionsView());
    vm.register(createQueriesView());
    vm.register(createHistogramView());
    vm.register(createTimelineView());
    vm.register(createTransitionsView());
    vm.register(createConcurrencyView());

    // Enter the default tab without a network refresh (init() does the first).
    vm.active = vm.views['overview'];
    vm.active.enter(vm._viewCtx());

    document.querySelectorAll('.tab').forEach(btn => {
        btn.addEventListener('click', () => switchTab(btn.dataset.tab));
    });
}

function setActiveTabButton(tab) {
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === tab);
    });
}

async function switchTab(tab) {
    if (tab === 'concurrency' || tab === 'transitions') stopAutoRefresh();
    setActiveTabButton(tab);
    summaryEl.innerHTML = '';
    tableEl.innerHTML = '<div class="loading">Loading...</div>';
    // The active/AAS chart reflects the same window across tabs; refresh it too.
    await refreshActive();
    await vm.switchTo(tab);
}

// Switch the active tab WITHOUT triggering a separate refresh (used by drill
// navigation, which refreshes once afterwards). Returns nothing.
function setTabView(tab) {
    if (vm.active && vm.activeId() !== tab) {
        try { vm.active.leave(); } catch (e) { /* best-effort */ }
        vm.active = vm.views[tab];
        try { vm.active.enter(vm._viewCtx()); } catch (e) { /* best-effort */ }
    }
    setActiveTabButton(tab);
}

// ── Drill-down / breadcrumb ───────────────────────────────────────────────────

const PIVOT = { class: 'events', event_id: 'queries', pid: 'timeline', query_id: 'events' };

function drill(intent) {
    if (!intent) return;
    filters.drill(intent.filterKey, intent.filterValue, intent.label, vm.activeId());
    const pivot = PIVOT[intent.filterKey];
    if (pivot) setTabView(pivot);
    updateBreadcrumb();
    refresh();
}

function drillUp(index) {
    const view = filters.drillUp(index);
    if (view) setTabView(view);
    updateBreadcrumb();
    refresh();
}

function clearFilters() {
    filters.clear();
    setTabView('overview');
    updateBreadcrumb();
    refresh();
}

function updateBreadcrumb() {
    const el = document.getElementById('breadcrumb');
    if (filters.isEmpty()) { el.innerHTML = ''; return; }

    let html = '';
    filters.breadcrumbs.forEach((crumb, i) => {
        if (i > 0) html += '<span class="crumb-sep">›</span>';
        html += '<span class="crumb" data-idx="' + i + '">' +
            dotHtml(crumb.label) + esc(crumb.label) + '</span>';
    });

    if (Object.keys(filters.filters).length > 0) {
        if (filters.breadcrumbs.length > 0) html += '<span class="crumb-sep">›</span>';
        const label = filters.currentLabel ||
            Object.entries(filters.filters).map(([k, v]) => k + '=' + v).join(', ');
        html += '<span style="color:#4fc3f7">' + dotHtml(label) + esc(label) + '</span>';
        html += ' <span class="crumb-clear" title="Clear all filters">✕</span>';
    }
    el.innerHTML = html;

    el.querySelectorAll('.crumb').forEach(crumb => {
        crumb.addEventListener('click', () => drillUp(parseInt(crumb.dataset.idx)));
    });
    const clearBtn = el.querySelector('.crumb-clear');
    if (clearBtn) clearBtn.addEventListener('click', clearFilters);
}

function dotHtml(name) {
    const color = classColor(name);
    if (!color) return '';
    return '<span class="class-dot" style="background:' + color + '"></span>';
}

// ── Time window / zoom ────────────────────────────────────────────────────────

function setStatus(text, cls) {
    const el = document.getElementById('status');
    el.textContent = text;
    el.className = 'status ' + cls;
}

function updateTimeRange() {
    const el = document.getElementById('time-range');
    const from = fmtTime(timeRange.from);
    const to = fmtTime(timeRange.to);
    const dur = fmtDuration(timeRange.span());
    el.textContent = from + ' – ' + to + ' (' + dur + ')';
}

async function zoomTo(from, to) {
    timeRange.zoomTo(from, to);
    updateTimeRange();
    await refresh();
}

async function zoomOut() {
    timeRange.zoomOut();
    updateTimeRange();
    await refresh();
}

// ── Chart resize handle ───────────────────────────────────────────────────────

function initChartResize() {
    const handle = document.getElementById('chart-resize');
    let startY = 0, startH = 0;
    handle.addEventListener('mousedown', (e) => {
        e.preventDefault();
        startY = e.clientY;
        startH = chartEl.offsetHeight;
        document.addEventListener('mousemove', onDrag);
        document.addEventListener('mouseup', onUp);
        document.body.style.cursor = 'ns-resize';
        document.body.style.userSelect = 'none';
    });
    function onDrag(e) {
        const h = Math.max(120, startH + e.clientY - startY);
        chartEl.style.height = h + 'px';
        if (activeView && activeView.resize) activeView.resize();
    }
    function onUp() {
        document.removeEventListener('mousemove', onDrag);
        document.removeEventListener('mouseup', onUp);
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
    }
}

// ── Time picker ───────────────────────────────────────────────────────────────

function initTimePicker() {
    const rangeBtn = document.getElementById('time-range');
    const picker = document.getElementById('time-picker');
    const zoomOutBtn = document.getElementById('zoom-out-btn');

    rangeBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        const visible = picker.style.display !== 'none';
        picker.style.display = visible ? 'none' : 'block';
        if (!visible) {
            document.getElementById('tp-from').value = nsToDatetimeLocal(timeRange.from);
            document.getElementById('tp-to').value = nsToDatetimeLocal(timeRange.to);
        }
    });

    document.addEventListener('click', (e) => {
        if (!picker.contains(e.target) && e.target !== rangeBtn) picker.style.display = 'none';
    });
    picker.addEventListener('click', (e) => e.stopPropagation());

    picker.querySelectorAll('.tp-quick button').forEach(btn => {
        btn.addEventListener('click', async () => {
            const secs = parseInt(btn.dataset.range);
            picker.style.display = 'none';
            picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            stopAutoRefresh();
            const myId = autoRefreshId;  // capture; only restart if still latest
            if (secs === 0) {
                await zoomTo(server.fromNs, server.toNs);
            } else {
                timeRange.anchorLive(secs);
                updateTimeRange();
                await refresh();
                if (autoRefreshId === myId) startAutoRefresh(secs);
            }
        });
    });

    document.getElementById('tp-apply').addEventListener('click', () => {
        const fromStr = document.getElementById('tp-from').value;
        const toStr = document.getElementById('tp-to').value;
        if (!fromStr || !toStr) return;
        const fromNs = new Date(fromStr + 'Z').getTime() * 1e6;
        const toNs = new Date(toStr + 'Z').getTime() * 1e6;
        if (fromNs >= toNs) return;
        picker.style.display = 'none';
        picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
        stopAutoRefresh();
        zoomTo(fromNs, toNs);
    });

    zoomOutBtn.addEventListener('click', () => { stopAutoRefresh(); zoomOut(); });
}

function nsToDatetimeLocal(ns) {
    const d = new Date(ns / 1e6);
    const pad = (n) => String(n).padStart(2, '0');
    return d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) +
        'T' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + ':' + pad(d.getUTCSeconds());
}

// ── Live mode / auto-refresh ──────────────────────────────────────────────────

function startAutoRefresh(rangeSecs) {
    stopAutoRefresh();
    autoRefreshOn = true;
    setLiveButton(true);
    const myId = ++autoRefreshId;
    (async function loop() {
        await new Promise(r => setTimeout(r, 5000));
        while (autoRefreshId === myId) {
            try {
                const info = await transport.send('info', {});
                if (autoRefreshId !== myId) break;
                if (info) {
                    server.update(info);
                    timeRange.anchorLive(rangeSecs);
                    updateTimeRange();
                }
                if (autoRefreshId !== myId) break;
                await refresh();
            } catch (e) { /* ignore */ }
            if (autoRefreshId !== myId) break;
            await new Promise(r => setTimeout(r, 5000));
        }
    })();
}

function stopAutoRefresh() {
    autoRefreshOn = false;
    autoRefreshId++;   // invalidate any running loop
    setLiveButton(false);
}

function setLiveButton(on) {
    const liveBtn = document.getElementById('live-btn');
    if (!liveBtn) return;
    liveBtn.classList.toggle('active', on);
    liveBtn.textContent = on ? 'Live ●' : 'Live';
}

function initLiveMode() {
    const btn = document.getElementById('live-btn');
    if (!btn) return;
    btn.addEventListener('click', () => {
        if (autoRefreshOn) {
            stopAutoRefresh();
        } else {
            timeRange.anchorLive(300);
            updateTimeRange();
            startAutoRefresh(300);
            refresh();
        }
    });
}

// ── Daemon control plane (B5) ─────────────────────────────────────────────────
//
// One poll fetches status (+ metrics if the panel is open) over the control
// proxy. A daemon-not-running reply (static-trace replay) flips `available`
// false: we hide the escalation UI and stop polling — no console noise, the UI
// just degrades to "no escalation here". When a daemon IS present, the escalate
// header control + (optionally) the metrics panel reflect live state, and the
// AAS chart re-renders so its escalation annotation tracks the current window.

async function pollDaemon() {
    if (!daemon.available && daemon.polled) return;
    try {
        const status = await controlStatus(transport);
        daemon.status = status;
        daemon.available = true;
        daemon.polled = true;
        if (metricsPanelOpen) {
            try { daemon.metrics = await controlMetrics(transport); }
            catch (e) { /* metrics optional; keep last */ }
        }
        renderEscalateControl();
        renderMetricsPanel();
    } catch (e) {
        // ControlUnavailable (no daemon) or transport error: degrade silently.
        daemon.polled = true;
        if (e instanceof ControlUnavailable) {
            daemon.available = false;
            daemon.status = null;
            renderEscalateControl();
            renderMetricsPanel();
        }
        // Transport errors (transient disconnect): keep last status, retry next tick.
    }
}

function startDaemonPoll() {
    stopDaemonPoll();
    const myId = ++daemonPollId;
    (async function loop() {
        while (daemonPollId === myId) {
            await new Promise(r => setTimeout(r, 5000));
            if (daemonPollId !== myId) break;
            if (!daemon.available) break;   // no daemon: stop the loop
            await pollDaemon();
        }
    })();
}

function stopDaemonPoll() { daemonPollId++; }

function renderEscalateControl() {
    const el = document.getElementById('escalate-control');
    const metricsBtn = document.getElementById('metrics-btn');
    if (!el) return;
    const supported = daemon.available && daemon.status &&
                      daemon.status.escalation_supported;
    if (metricsBtn) metricsBtn.style.display = supported ? '' : 'none';
    mountEscalateControl(el, Object.assign(makeCtx(), { viewId: 'control' }));
}

function renderMetricsPanel() {
    const el = document.getElementById('daemon-metrics');
    if (!el) return;
    if (metricsPanelOpen && daemon.available && daemon.metrics) {
        mountMetricsPanel(el, daemon.metrics, daemon.status);
        el.style.display = '';
    } else {
        el.style.display = 'none';
    }
}

function initMetricsButton() {
    const btn = document.getElementById('metrics-btn');
    if (!btn) return;
    btn.addEventListener('click', async () => {
        metricsPanelOpen = !metricsPanelOpen;
        btn.classList.toggle('active', metricsPanelOpen);
        if (metricsPanelOpen) {
            try { daemon.metrics = await controlMetrics(transport); }
            catch (e) { /* keep last */ }
        }
        renderMetricsPanel();
    });
}

// ── Start ─────────────────────────────────────────────────────────────────────

function boot() {
    chartEl = document.getElementById('aas-chart-container');
    tableEl = document.getElementById('table-container');
    summaryEl = document.getElementById('summary-bar');
    tooltipEl = document.getElementById('query-tooltip');

    // Debug/test handle: the UI no longer has a single mutable global `state`,
    // so expose the explicit modules for Playwright assertions (read-only use).
    window.__pgwt = { server, timeRange, filters, transport,
        get activeTab() { return vm ? vm.activeId() : null; },
        // B5: read-only accessors for Playwright assertions on the daemon state.
        daemon,
        daemonTier() { return daemon.status ? daemon.status.tier : null; } };

    connect();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
} else {
    boot();
}
