/* pgwt — Web Investigation Client: bootstrap + router.
 *
 * B3 part 1: the 2000-line monolith is being restructured into native ES
 * modules (no build step — still embeddable via go:embed). This file is the
 * bootstrap + router. The migrated views (active = AAS chart, overview = time
 * model table) live in views/ as pure builders + thin mounts; the not-yet-
 * migrated tabs (events/sessions/queries/histogram/timeline/transitions/
 * concurrency) are wrapped as legacy view adapters so they ALL flow through the
 * same view-manager chokepoint and transport single-flight — which is what
 * keeps the B2 chaos races fixed while they await their turn to be migrated to
 * pure builders in B3 part 2.
 *
 * State is explicit (lib/state.js): no grab-bag global mutated mid-flight.
 * Stale-response superseding is structural (lib/transport.js channels +
 * lib/view-manager.js epochs), replacing the old _refreshGen counters.
 */

import { ServerInfo, TimeRange, FilterStack, FIFTEEN_MIN_NS } from './lib/state.js';
import { Transport } from './lib/transport.js';
import { ViewManager } from './lib/view-manager.js';
import { mountTable, buildTableModel } from './lib/table.js';
import {
    WAIT_CLASSES, EVENT_PALETTE, classColor,
    fmtTime, fmtTimeMs, fmtDuration, fmtMs, fmtUs, fmtCount, esc,
} from './lib/format.js';
import { eventsConfig, sessionsConfig, queriesConfig } from './lib/builders/table-configs.js';
import { createActiveView } from './views/active.js';
import { createOverviewView } from './views/overview.js';

// ── Core services ─────────────────────────────────────────────────────────────

const server = new ServerInfo();
const timeRange = new TimeRange(server);
const filters = new FilterStack();
const transport = new Transport();

let activeView = null;       // persistent AAS chart view ("active")
let vm = null;               // ViewManager for tab views

// Reconnect bookkeeping (lifecycle only — not request state).
let reconnectDelay = 2000;
let reconnectTimer = null;

// Auto-refresh (live mode) bookkeeping.
let autoRefreshId = 0;
let autoRefreshOn = false;

// Sort state per tab (legacy table views read/write this).
const sortCol = {};
const sortAsc = {};

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
        // Legacy adapters need these:
        sortCol, sortAsc,
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
        window.addEventListener('resize', onResize);

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
    vm.register(makeEventsView());
    vm.register(makeSessionsView());
    vm.register(makeQueriesView());
    vm.register(makeHistogramView());
    vm.register(makeTimelineView());
    vm.register(makeTransitionsView());
    vm.register(makeConcurrencyView());

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

// ══════════════════════════════════════════════════════════════════════════════
// LEGACY VIEW ADAPTERS — not yet migrated to pure builders (B3 part 2).
//
// Each wraps the original app.js render logic in the { id, requests, build,
// mount, enter, leave } contract so it flows through the view-manager chokepoint
// + transport single-flight. requests() returns the raw response (possibly
// several joined fetches), build() is identity (kept impure for now — the
// migration to a pure builder + Node tests happens per-view in part 2), and
// mount() runs the legacy DOM/ECharts rendering. enter/leave own any chart.
// ══════════════════════════════════════════════════════════════════════════════

function makeTableTabView(id, cmd, config) {
    return {
        id,
        async requests(ctx) {
            return ctx.transport.request(ctx.channel('table'), cmd, {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },
        build(data) { return data; },
        mount(el, data) {
            renderLegacyTable(el, id, config, data);
        },
        enter() {}, leave() {},
    };
}

function makeEventsView()   { return makeTableTabView('events', 'top_events', eventsConfig); }
function makeSessionsView() { return makeTableTabView('sessions', 'top_sessions', sessionsConfig); }
function makeQueriesView()  { return makeTableTabView('queries', 'top_queries', queriesConfig); }

function renderLegacyTable(container, tab, config, data) {
    summaryEl.innerHTML = '';
    if (!config || !data) {
        container.innerHTML = '<div class="loading">No data</div>';
        return;
    }
    const rows = data.rows || [];
    if (rows.length === 0) {
        container.innerHTML = '<div class="loading">No data for selected range</div>';
        return;
    }
    const sort = sortCol[tab] ? { key: sortCol[tab], asc: sortAsc[tab] } : null;
    const model = buildTableModel(config, rows, sort);
    mountTable(container, config, model, {
        sort,
        onSort: (key) => {
            if (sortCol[tab] === key) sortAsc[tab] = !sortAsc[tab];
            else { sortCol[tab] = key; sortAsc[tab] = false; }
            renderLegacyTable(container, tab, config, data);
        },
        onRowClick: (row) => { const i = config.onClick(row); if (i) drill(i); },
        tooltipEl,
    });
}

// ── Histogram (heatmap) ───────────────────────────────────────────────────────

function makeHistogramView() {
    let chart = null;
    let events = [];   // cached event list for selectors
    const view = {
        id: 'histogram',
        async requests(ctx) {
            const evData = await ctx.transport.request(ctx.channel('events'),
                'top_events', { from: ctx.timeRange.from, to: ctx.timeRange.to, filters: {} });
            events = (evData && evData.rows) || [];
            // Selector values determine the heatmap filter; read after mount of
            // the selector shell, so fetch heatmap with current filters here.
            const f = currentHeatmapFilters();
            const hm = await ctx.transport.request(ctx.channel('heatmap'), 'heatmap', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                buckets: Math.min(Math.floor(window.innerWidth / 6), 300),
                filters: f,
            });
            return { events, heatmap: hm };
        },
        build(data) { return data; },
        mount(el, data) {
            summaryEl.innerHTML = '';
            ensureHeatmapShell(el);
            populateSelectors(events);
            renderHeatmap(data.heatmap);
        },
        enter() {},
        leave() { if (chart) { chart.dispose(); chart = null; } },
    };

    function currentHeatmapFilters() {
        const f = { ...filters.filters };
        const cs = document.getElementById('hm-class');
        const es = document.getElementById('hm-event');
        if (cs && cs.value) f.class = cs.value;
        if (es && es.value) f.event_id = parseInt(es.value);
        return f;
    }

    function ensureHeatmapShell(container) {
        if (document.getElementById('heatmap-container')) return;
        container.innerHTML =
            '<div class="event-selector">' +
            '  <label>Class</label>' +
            '  <select id="hm-class"><option value="">All</option></select>' +
            '  <label>Event</label>' +
            '  <select id="hm-event"><option value="">All</option></select>' +
            '</div>' +
            '<div id="heatmap-container"></div>';
        document.getElementById('hm-class').addEventListener('change', () => {
            populateEventSelect(events);
            document.getElementById('hm-event').value = '';
            fetchAndRenderHeatmap();
        });
        document.getElementById('hm-event').addEventListener('change', fetchAndRenderHeatmap);
    }

    async function fetchAndRenderHeatmap() {
        const ctx = vm._viewCtx();
        try {
            const hm = await ctx.transport.request(ctx.channel('heatmap'), 'heatmap', {
                from: timeRange.from, to: timeRange.to,
                buckets: Math.min(Math.floor(window.innerWidth / 6), 300),
                filters: currentHeatmapFilters(),
            });
            if (vm.activeId() !== 'histogram') return;  // chokepoint
            renderHeatmap(hm);
        } catch (e) { /* superseded / disconnect */ }
    }

    function renderHeatmap(data) {
        const el = document.getElementById('heatmap-container');
        if (!el) return;
        if (!data || !data.cells || data.cells.length === 0) {
            if (chart) { chart.dispose(); chart = null; }
            el.innerHTML = '<div class="loading">No data for selected event/range</div>';
            return;
        }
        if (!chart) chart = window.echarts.init(el, 'dark');
        const hbns = data.bucket_ns || 0;
        const timeLabels = data.times.map(t => fmtTime(t, hbns));
        const latLabels = data.labels || [];
        const hmData = data.cells.map(c => [c[0], c[1], c[2]]);
        chart.setOption({
            backgroundColor: 'transparent',
            tooltip: {
                position: 'top', backgroundColor: '#1e1e3a', borderColor: '#333',
                textStyle: { color: '#e0e0e0', fontSize: 12 },
                formatter: (p) => '<b>' + timeLabels[p.data[0]] + '</b><br>' +
                    'Latency: ' + latLabels[p.data[1]] + '<br>' +
                    'Count: <b>' + p.data[2].toLocaleString() + '</b>',
            },
            grid: { left: 90, right: 40, top: 10, bottom: 60 },
            xAxis: { type: 'category', data: timeLabels,
                axisLabel: { color: '#888', fontSize: 10 },
                axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false } },
            yAxis: { type: 'category', data: latLabels,
                axisLabel: { color: '#888', fontSize: 10 },
                axisLine: { lineStyle: { color: '#333' } }, splitArea: { show: false } },
            visualMap: { min: 0, max: data.max_count || 1, calculable: false,
                orient: 'horizontal', left: 'center', bottom: 0,
                itemWidth: 12, itemHeight: 120,
                textStyle: { color: '#888', fontSize: 10 },
                inRange: { color: ['#1a5276', '#2196F3', '#4CAF50', '#FFEB3B', '#FF9800', '#F44336'] } },
            series: [{ type: 'heatmap', data: hmData,
                emphasis: { itemStyle: { borderColor: '#fff', borderWidth: 1 } },
                itemStyle: { borderWidth: 0 } }],
        }, true);
    }

    function populateSelectors(evs) {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const classes = new Set();
        evs.forEach(r => { if (r.class) classes.add(r.class); });
        const prevClass = classSelect.value;
        classSelect.innerHTML = '<option value="">All</option>';
        for (const cls of classes) {
            const opt = document.createElement('option');
            opt.value = cls; opt.textContent = cls; classSelect.appendChild(opt);
        }
        if (filters.filters.class) classSelect.value = filters.filters.class;
        else if (prevClass) classSelect.value = prevClass;
        populateEventSelect(evs);
    }

    function populateEventSelect(evs) {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const selClass = classSelect.value;
        const filtered = selClass ? evs.filter(r => r.class === selClass) : evs;
        const prevEvent = eventSelect.value;
        eventSelect.innerHTML = '<option value="">All' + (selClass ? ' ' + selClass : '') + '</option>';
        for (const ev of filtered.slice(0, 50)) {
            const opt = document.createElement('option');
            opt.value = ev.event_id;
            opt.textContent = ev.name + ' (' + fmtCount(ev.count) + ')';
            eventSelect.appendChild(opt);
        }
        if (filters.filters.event_id) eventSelect.value = filters.filters.event_id;
        else if (prevEvent) eventSelect.value = prevEvent;
    }

    return view;
}

// ── Session timeline ──────────────────────────────────────────────────────────

function makeTimelineView() {
    let chart = null;
    return {
        id: 'timeline',
        async requests(ctx) {
            const f = ctx.filters.filters;
            if (!f.pid && !f.query_id) return { prompt: true };
            return ctx.transport.request(ctx.channel('table'), 'session_timeline', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },
        build(data) { return data; },
        mount(el, data) {
            summaryEl.innerHTML = '';
            if (data && data.prompt) {
                el.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                    'Select a session (PID) or query to view timeline</div>';
                return;
            }
            renderTimeline(el, data);
        },
        enter() {},
        leave() { if (chart) { chart.dispose(); chart = null; } },
    };

    function renderTimeline(container, data) {
        if (!data || !data.events || data.events.length === 0) {
            if (chart) { chart.dispose(); chart = null; }
            container.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                'No events for selected session/range</div>';
            return;
        }
        let html = '';
        if (data.truncated) {
            html += '<div style="padding:8px 20px;font-size:12px;color:#ffd700;background:#3d3200;' +
                'border-bottom:1px solid #555">Showing ' + data.events.length + ' of ' +
                data.total_count + ' events. Zoom in for more detail.</div>';
        }
        const chartHeight = Math.max(200, data.pids.length * 50 + 80);
        html += '<div id="timeline-chart" style="height:' + chartHeight + 'px;padding:10px 20px"></div>';
        container.innerHTML = html;

        const el = document.getElementById('timeline-chart');
        if (chart) { chart.dispose(); chart = null; }
        chart = window.echarts.init(el, 'dark');

        const pidLabels = data.pids.map(p => 'PID ' + p);
        const pidIndexMap = {};
        data.pids.forEach((p, i) => { pidIndexMap[p] = i; });
        const barData = data.events.map(ev => [
            ev.s, ev.s + ev.d, pidIndexMap[ev.p] || 0, ev.n, ev.c, ev.q, ev.d,
        ]);

        chart.setOption({
            backgroundColor: 'transparent',
            tooltip: {
                trigger: 'item', backgroundColor: '#1e1e3a', borderColor: '#333',
                textStyle: { color: '#e0e0e0', fontSize: 12 },
                formatter: (params) => {
                    const d = params.data;
                    let s = '<b>' + esc(d[3]) + '</b><br>';
                    s += 'Duration: <b>' + fmtUs(d[6] / 1000) + '</b><br>';
                    s += 'Start: ' + fmtTime(d[0]) + '<br>';
                    if (d[5] && d[5] !== '0') s += 'Query: ' + d[5];
                    return s;
                },
            },
            grid: { left: 100, right: 20, top: 20, bottom: 40 },
            xAxis: { type: 'value', min: timeRange.from, max: timeRange.to,
                axisLabel: { color: '#888', fontSize: 10, formatter: (v) => fmtTime(v) },
                axisLine: { lineStyle: { color: '#333' } } },
            yAxis: { type: 'category', data: pidLabels,
                axisLabel: { color: '#aaa', fontSize: 11 },
                axisLine: { lineStyle: { color: '#333' } } },
            series: [{
                type: 'custom',
                renderItem: (params, api) => {
                    const startVal = api.value(0), endVal = api.value(1);
                    const catIdx = api.value(2), classIdx = api.value(4);
                    const start = api.coord([startVal, catIdx]);
                    const end = api.coord([endVal, catIdx]);
                    const bandWidth = api.size([0, 1])[1];
                    const color = WAIT_CLASSES[classIdx] ? WAIT_CLASSES[classIdx].color : '#888';
                    const rectHeight = bandWidth * 0.6;
                    const width = Math.max(end[0] - start[0], 1);
                    return { type: 'rect',
                        shape: { x: start[0], y: start[1] - rectHeight / 2, width, height: rectHeight },
                        style: { fill: color },
                        styleEmphasis: { fill: color, opacity: 0.8, lineWidth: 1, stroke: '#fff' } };
                },
                encode: { x: [0, 1], y: 2 },
                data: barData,
            }],
        }, true);
    }
}

// ── Transitions DFG ───────────────────────────────────────────────────────────

function makeTransitionsView() {
    let chart = null;
    const view = {
        id: 'transitions',
        async requests(ctx) {
            const data = await ctx.transport.request(ctx.channel('table'), 'transitions', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(), num_buckets: 200,
            });
            let variants = null;
            try {
                variants = await ctx.transport.request(ctx.channel('variants'), 'variants', {
                    from: ctx.timeRange.from, to: ctx.timeRange.to,
                    filters: ctx.filters.snapshot(), num_buckets: 20,
                });
            } catch (e) { /* variants optional */ }
            return { transitions: data, variants };
        },
        build(data) { return data; },
        mount(el, data) {
            summaryEl.innerHTML = '';
            renderTransitions(el, data.transitions);
            if (data.variants) appendVariants(el, data.variants);
        },
        enter() {},
        leave() { if (chart) { chart.dispose(); chart = null; } },
    };

    function renderTransitions(container, data) {
        if (!data || !data.links || data.links.length === 0) {
            container.innerHTML = '<p style="color:#888;padding:20px">No transitions found</p>';
            return;
        }
        const maxEdgeCount = Math.max(...data.links.map(l => l.value));
        const nodeMap = {};
        (data.nodes || []).forEach(n => { nodeMap[n.name] = n; });

        container.innerHTML =
            '<div style="padding:10px 20px;display:flex;align-items:center;gap:12px">' +
                '<span style="color:#888;font-size:12px">Simplify:</span>' +
                '<input type="range" id="dfg-slider" min="0" max="100" value="20" ' +
                    'style="width:200px;accent-color:#4fc3f7">' +
                '<span id="dfg-slider-val" style="color:#888;font-size:12px">20%</span>' +
                '<span style="color:#666;font-size:11px;margin-left:8px">' +
                    `${Number(data.total).toLocaleString()} transitions</span>` +
            '</div>' +
            '<div id="dfg-container" style="width:100%;height:550px;background:#1a1a2e"></div>';

        function renderDFG(threshold) {
            const minCount = maxEdgeCount * threshold / 100;
            const visibleLinks = data.links.filter(l => l.value >= minCount);
            const visibleNodes = new Set();
            visibleLinks.forEach(l => { visibleNodes.add(l.source); visibleNodes.add(l.target); });
            if (visibleNodes.size === 0) {
                document.getElementById('dfg-container').innerHTML =
                    '<p style="color:#666;padding:40px;text-align:center">No transitions above threshold</p>';
                return;
            }
            const maxNodeMs = Math.max(...[...visibleNodes].map(n => (nodeMap[n]?.total_ms || 1)));
            const maxLinkVal = Math.max(...visibleLinks.map(l => l.value));
            const sortedNodes = [...visibleNodes]
                .map(name => ({ name, ms: (nodeMap[name]?.total_ms || 0) }))
                .sort((a, b) => b.ms - a.ms);
            const n = sortedNodes.length;
            const cols = Math.ceil(Math.sqrt(n * 1.8));
            const rows = Math.ceil(n / cols);
            const el = document.getElementById('dfg-container');
            const cW = el.clientWidth || 800, cH = el.clientHeight || 550;
            const padX = 80, padY = 60;
            const cellW = (cW - padX * 2) / Math.max(cols - 1, 1);
            const cellH = (cH - padY * 2) / Math.max(rows - 1, 1);

            const ecNodes = sortedNodes.map((nd, i) => {
                const info = nodeMap[nd.name] || {};
                const ms = nd.ms;
                const cls = (info.class || 'unknown').toLowerCase();
                const color = classColor(nd.name) || classColor(cls) || '#888';
                const size = Math.max(15, Math.min(120, 15 + Math.sqrt(ms / maxNodeMs) * 105));
                const timeStr = ms >= 1000 ? (ms / 1000).toFixed(1) + 's' : ms.toFixed(0) + 'ms';
                const col = i % cols, row = Math.floor(i / cols);
                return { name: nd.name, x: padX + col * cellW, y: padY + row * cellH,
                    symbolSize: size,
                    itemStyle: { color, borderColor: color, borderWidth: 2 },
                    label: { show: true, position: 'bottom', fontSize: 10, color: '#ccc',
                        formatter: (nd.name.indexOf(':') > 0 ? nd.name.substring(nd.name.indexOf(':') + 1) : nd.name) +
                            '\n' + timeStr, lineHeight: 14 },
                    tooltip: { formatter: `<b>${nd.name}</b><br/>Total time: ${timeStr}` },
                    value: ms };
            });
            const ecLinks = visibleLinks.map(l => {
                const w = Math.max(1, Math.min(10, (l.value / maxLinkVal) * 10));
                const pct = (l.value / data.total * 100).toFixed(1);
                const avg = l.value > 0 ? (l.duration_ms / l.value).toFixed(1) : '?';
                return { source: l.source, target: l.target,
                    lineStyle: { width: w, color: '#555',
                        curveness: l.source === l.target ? 0.8 : 0.3,
                        opacity: 0.4 + (l.value / maxLinkVal) * 0.5 },
                    tooltip: { formatter: `<b>${l.source}</b> → <b>${l.target}</b><br/>` +
                        `Count: ${l.value.toLocaleString()} (${pct}%)<br/>Avg dwell: ${avg} ms` },
                    value: l.value };
            });

            if (chart) chart.dispose();
            chart = window.echarts.init(el, 'dark');
            chart.setOption({
                backgroundColor: 'transparent',
                tooltip: { backgroundColor: '#1e1e3a', borderColor: '#333',
                    textStyle: { color: '#e0e0e0', fontSize: 12 } },
                series: [{ type: 'graph', layout: 'none', roam: true, draggable: true,
                    data: ecNodes, links: ecLinks,
                    edgeSymbol: ['none', 'arrow'], edgeSymbolSize: [0, 8],
                    emphasis: { focus: 'adjacency', lineStyle: { width: 4, opacity: 1 } },
                    lineStyle: { curveness: 0.3 } }],
            }, true);
        }

        renderDFG(20);
        const slider = document.getElementById('dfg-slider');
        const sliderVal = document.getElementById('dfg-slider-val');
        slider.addEventListener('input', () => {
            sliderVal.textContent = slider.value + '%';
            renderDFG(+slider.value);
        });
    }

    function appendVariants(container, vdata) {
        const variantDiv = document.createElement('div');
        if (vdata.exec && vdata.exec.variants && vdata.exec.variants.length > 0)
            renderVariantSection(vdata.exec, 'Execution', variantDiv);
        if (vdata.plan && vdata.plan.variants && vdata.plan.variants.length > 0)
            renderVariantSection(vdata.plan, 'Planning', variantDiv);
        if (variantDiv.innerHTML) container.appendChild(variantDiv);
    }

    return view;
}

function renderVariantSection(vdata, title, container) {
    const totalMs = vdata.variants.reduce((s, v) => s + v.total_ms, 0);
    let html = '<div style="padding:10px 20px">' +
        `<h3 style="color:#ccc;margin:10px 0 5px">${title} Flow Patterns ` +
        `<span style="color:#666;font-size:12px">(${vdata.total.toLocaleString()} instances → ` +
        `${vdata.num_variants} patterns)</span></h3>`;
    vdata.variants.forEach((v, idx) => {
        const pctTime = totalMs > 0 ? (v.total_ms / totalMs * 100) : 0;
        const avgStr = v.avg_ms >= 1000 ? (v.avg_ms / 1000).toFixed(1) + 's' :
                       v.avg_ms >= 1 ? v.avg_ms.toFixed(1) + 'ms' : (v.avg_ms * 1000).toFixed(0) + 'μs';
        const p95Str = v.p95_ms >= 1000 ? (v.p95_ms / 1000).toFixed(1) + 's' :
                       v.p95_ms >= 1 ? v.p95_ms.toFixed(1) + 'ms' : (v.p95_ms * 1000).toFixed(0) + 'μs';
        const totalStr = v.total_ms >= 1000 ? (v.total_ms / 1000).toFixed(1) + 's' : v.total_ms.toFixed(0) + 'ms';
        const stepTotalMs = v.steps.reduce((s, st) => s + (st.avg_ms || 0), 0) || 1;
        let flowHtml = '<div style="display:flex;height:28px;border-radius:4px;overflow:hidden;' +
            'background:#1a1a2e;border:1px solid #2a2a4a;margin:4px 0">';
        v.steps.forEach(st => {
            const w = Math.max(2, (st.avg_ms / stepTotalMs) * 100);
            const color = classColor(st.name) || classColor(st.class) || '#888';
            const label = st.name.indexOf(':') > 0 ? st.name.substring(st.name.indexOf(':') + 1) : st.name;
            const durStr = st.avg_ms >= 1 ? st.avg_ms.toFixed(1) + 'ms' :
                           st.avg_ms >= 0.001 ? (st.avg_ms * 1000).toFixed(0) + 'μs' : '';
            const loopMark = st.loop ? ' ×N' : '';
            flowHtml += `<div style="width:${w.toFixed(1)}%;background:${color};opacity:0.8;` +
                `display:flex;align-items:center;justify-content:center;overflow:hidden;` +
                `font-size:9px;color:#fff;white-space:nowrap;padding:0 3px;min-width:2px" ` +
                `title="${st.name} ${durStr}${loopMark}">` +
                `${w > 8 ? label + (loopMark ? loopMark : '') : ''}` +
                `${w > 15 ? ' ' + durStr : ''}</div>`;
        });
        flowHtml += '</div>';
        let stepsText = v.steps.map(st => {
            const label = st.name.indexOf(':') > 0 ? st.name.substring(st.name.indexOf(':') + 1) : st.name;
            const dur = st.avg_ms >= 1 ? st.avg_ms.toFixed(1) + 'ms' : (st.avg_ms * 1000).toFixed(0) + 'μs';
            if (st.loop) return `[${label}(${dur})…]×N`;
            return `${label}(${dur})`;
        }).join(' → ');
        let queryHtml = '';
        if (v.top_query_id) {
            const qid = v.top_query_id;
            const preview = v.query_text
                ? (v.query_text.length > 100 ? v.query_text.substring(0, 100) + '...' : v.query_text) : '';
            queryHtml = `<div style="color:#666;font-size:10px;font-family:monospace;margin-top:2px;` +
                `white-space:nowrap;overflow:hidden;text-overflow:ellipsis" title="${esc(v.query_text || '')}">` +
                `<span style="color:#888">${qid}</span>${preview ? ' ' + esc(preview) : ''}</div>`;
        }
        html += `<div style="background:#1e1e3a;border:1px solid #2a2a4a;border-radius:6px;padding:10px;margin:6px 0">` +
            `<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">` +
                `<span style="color:#ccc;font-size:13px;font-weight:500">#${idx + 1}</span>` +
                `<span style="color:#888;font-size:11px">` +
                    `<b style="color:#4fc3f7">${pctTime.toFixed(1)}%</b> of time` +
                    ` · ${v.exec_count.toLocaleString()} exec · ${v.num_queries} queries` +
                    ` · avg ${avgStr} · p95 ${p95Str} · total ${totalStr}` +
                    (v.avg_loop_n > 1.5 ? ` · ~${v.avg_loop_n.toFixed(0)}× loop` : '') +
                `</span></div>` + flowHtml +
            `<div style="color:#888;font-size:10px;margin-top:2px">${stepsText}</div>` + queryHtml +
        `</div>`;
    });
    html += '</div>';
    const section = document.createElement('div');
    section.innerHTML = html;
    container.appendChild(section);
}

// ── Concurrency ───────────────────────────────────────────────────────────────

function makeConcurrencyView() {
    let chart = null;
    return {
        id: 'concurrency',
        async requests(ctx) {
            const chartWidth = (chartEl && chartEl.clientWidth) || 800;
            const numBuckets = Math.min(Math.floor(chartWidth / 4), 300);
            return ctx.transport.request(ctx.channel('table'), 'concurrency', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                buckets: numBuckets, filters: ctx.filters.snapshot(),
            });
        },
        build(data) { return data; },
        mount(el, data) {
            summaryEl.innerHTML = '';
            if (!data || !data.peaks || data.peaks.length === 0) {
                el.innerHTML = '<p style="color:#888;padding:20px">No concurrency data</p>';
                return;
            }
            render(el, data);
        },
        enter() {},
        leave() { if (chart) { chart.dispose(); chart = null; } },
    };

    function render(container, data) {
        container.innerHTML =
            '<div id="concurrency-chart" style="width:100%;height:350px;"></div>' +
            '<div id="burst-table" style="padding:10px;"></div>';
        const el = document.getElementById('concurrency-chart');
        if (chart) { chart.dispose(); chart = null; }
        chart = window.echarts.init(el, 'dark');

        const times = data.peaks.map(p => p.t);
        const bns = data.bucket_ns || 0;
        const peakData = data.peaks.map(p => p.max || 0);
        const peakEvents = data.peaks.map(p => p.event || '');
        const burstPoints = (data.bursts || []).map(b => {
            const idx = data.peaks.findIndex(p => p.t >= b.timestamp_ns);
            return { coord: [Math.max(0, idx), peakData[Math.max(0, idx)] || b.sessions],
                value: b.sessions, symbol: 'triangle',
                symbolSize: Math.min(10 + b.sessions * 2, 30),
                itemStyle: { color: '#f44' },
                label: { show: true, formatter: b.sessions + '', color: '#fff', fontSize: 10 } };
        });

        chart.setOption({
            title: { text: 'Peak Concurrent Sessions per Wait Event', left: 'center',
                textStyle: { color: '#ccc', fontSize: 14 } },
            tooltip: { trigger: 'axis', axisPointer: { type: 'cross' },
                backgroundColor: '#1e1e3a', borderColor: '#333',
                textStyle: { color: '#e0e0e0', fontSize: 12 },
                formatter: (params) => {
                    if (!params.length) return '';
                    const idx = params[0].dataIndex;
                    const ev = peakEvents[idx] || 'none';
                    return fmtTime(params[0].axisValue, bns) + '<br/>' +
                        'Peak: <b>' + params[0].value + ' sessions</b><br/>Event: ' + ev;
                } },
            grid: { left: 50, right: 20, top: 50, bottom: 40 },
            xAxis: { type: 'category', data: times,
                axisLabel: { color: '#888', fontSize: 10, formatter: (v) => fmtTime(v, bns) },
                axisLine: { lineStyle: { color: '#333' } } },
            yAxis: { type: 'value', name: 'Simultaneous Sessions',
                nameTextStyle: { color: '#888', fontSize: 11 }, min: 0,
                axisLabel: { color: '#888' }, splitLine: { lineStyle: { color: '#2a2a4a' } } },
            series: [{ type: 'line', data: peakData,
                areaStyle: { color: 'rgba(248,170,170,0.15)' },
                lineStyle: { color: '#f8a', width: 2 }, itemStyle: { color: '#f8a' },
                symbol: 'none',
                markPoint: burstPoints.length > 0 ? { data: burstPoints } : undefined }],
        }, true);

        const topPeaks = data.peaks.filter(p => p.max > 1).sort((a, b) => b.max - a.max).slice(0, 10);
        let html = '<h3 style="color:#ccc;margin:10px 0 5px">Top Peak Moments</h3>' +
            '<table class="data-table"><thead><tr>' +
            '<th>Time</th><th>Wait Event</th><th>Simultaneous Sessions</th></tr></thead><tbody>';
        topPeaks.forEach(p => {
            html += `<tr><td>${fmtTimeMs(p.t_ms)}</td><td>${p.event || 'CPU*'}</td><td><b>${p.max}</b></td></tr>`;
        });
        html += '</tbody></table>';

        const bursts = data.bursts || [];
        if (bursts.length > 0) {
            html += '<h3 style="color:#ccc;margin:15px 0 5px">Burst Events ' +
                '<span style="color:#666;font-size:12px">(4+ sessions within 10ms)</span></h3>' +
                '<table class="data-table"><thead><tr>' +
                '<th>Time</th><th>Wait Event</th><th>Sessions</th><th>PIDs</th></tr></thead><tbody>';
            bursts.forEach(b => {
                const time = fmtTimeMs(b.timestamp_ms);
                const pids = (b.pids || []).slice(0, 8).join(', ') + (b.pids && b.pids.length > 8 ? '...' : '');
                html += `<tr><td>${time}</td><td>${b.event}</td><td><b>${b.sessions}</b></td><td>${pids}</td></tr>`;
            });
            html += '</tbody></table>';
        } else {
            html += '<p style="color:#666;margin-top:15px">No burst events detected</p>';
        }
        document.getElementById('burst-table').innerHTML = html;
    }
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
        get activeTab() { return vm ? vm.activeId() : null; } };

    connect();
}

if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', boot);
} else {
    boot();
}
