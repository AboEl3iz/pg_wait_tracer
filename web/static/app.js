/* pgwt — Web Investigation Client */

// -- Wait class colors (Oracle ASH / RDS PI inspired) -------------------------

const WAIT_CLASSES = [
    { key: 'cpu',       label: 'CPU',       color: 'rgb(80,250,123)' },
    { key: 'io',        label: 'IO',        color: 'rgb(30,100,255)' },
    { key: 'lock',      label: 'Lock',      color: 'rgb(255,85,85)' },
    { key: 'lwlock',    label: 'LWLock',    color: 'rgb(255,121,198)' },
    { key: 'ipc',       label: 'IPC',       color: 'rgb(0,200,255)' },
    { key: 'client',    label: 'Client',    color: 'rgb(255,220,100)' },
    { key: 'timeout',   label: 'Timeout',   color: 'rgb(255,165,0)' },
    { key: 'bufferpin', label: 'BufferPin', color: 'rgb(0,210,180)' },
    { key: 'activity',  label: 'Activity',  color: 'rgb(150,100,255)' },
    { key: 'extension', label: 'Extension', color: 'rgb(190,150,255)' },
    { key: 'unknown',   label: 'Unknown',   color: 'rgb(180,180,180)' },
];

const CLASS_COLOR_MAP = {};
WAIT_CLASSES.forEach(c => {
    CLASS_COLOR_MAP[c.label.toLowerCase()] = c.color;
    CLASS_COLOR_MAP[c.key] = c.color;
});

// Palette for per-event series in drill-down charts
const EVENT_PALETTE = [
    'rgb(30,144,255)', 'rgb(255,99,71)', 'rgb(50,205,50)', 'rgb(255,215,0)',
    'rgb(138,43,226)', 'rgb(0,206,209)', 'rgb(255,140,0)', 'rgb(220,20,60)',
    'rgb(0,191,255)', 'rgb(255,105,180)', 'rgb(127,255,0)', 'rgb(255,69,0)',
    'rgb(75,0,130)', 'rgb(0,250,154)', 'rgb(255,182,193)', 'rgb(100,149,237)',
];

// -- State --------------------------------------------------------------------

const state = {
    ws: null,
    nextId: 1,
    pending: {},

    // Server info
    fromNs: 0,
    toNs: 0,
    numCpus: 0,

    // Current view window
    viewFrom: 0,
    viewTo: 0,

    // Filters + drill-down
    filters: {},
    breadcrumbs: [],

    // Active tab
    activeTab: 'overview',

    // Sort state per tab
    sortCol: {},
    sortAsc: {},

    // Zoom history
    zoomHistory: [],

    // Event series info for chart click handler (null = class mode)
    chartEventSeries: null,

    // Reconnect
    reconnectDelay: 2000,
    reconnectTimer: null,
};

// -- WebSocket ----------------------------------------------------------------

function connect() {
    if (state.reconnectTimer) {
        clearTimeout(state.reconnectTimer);
        state.reconnectTimer = null;
    }

    setStatus('Connecting...', 'connecting');
    const ws = new WebSocket('ws://' + location.host + '/ws');

    ws.onopen = () => {
        state.ws = ws;
        state.reconnectDelay = 2000;
        init();
    };

    ws.onclose = () => {
        state.ws = null;
        rejectAllPending('disconnected');
        scheduleReconnect();
    };

    ws.onerror = () => {
        // onclose will fire after onerror
    };

    ws.onmessage = (e) => {
        const data = JSON.parse(e.data);
        const p = state.pending[data.id];
        if (p) {
            clearTimeout(p.timer);
            delete state.pending[data.id];
            p.resolve(data);
        }
    };
}

function rejectAllPending(reason) {
    for (const id of Object.keys(state.pending)) {
        const p = state.pending[id];
        clearTimeout(p.timer);
        p.reject(new Error(reason));
    }
    state.pending = {};
}

function scheduleReconnect() {
    const delay = state.reconnectDelay;
    state.reconnectDelay = Math.min(delay * 2, 16000);
    setStatus('Reconnecting in ' + (delay / 1000) + 's...', 'error');
    state.reconnectTimer = setTimeout(connect, delay);
}

function send(cmd, params) {
    if (!state.ws || state.ws.readyState !== WebSocket.OPEN) {
        return Promise.reject(new Error('not connected'));
    }
    const id = state.nextId++;
    const msg = JSON.stringify({ id, cmd, ...params });
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            delete state.pending[id];
            reject(new Error('timeout'));
        }, 30000);
        state.pending[id] = { resolve, reject, timer };
        state.ws.send(msg);
    });
}

// -- Init ---------------------------------------------------------------------

async function init() {
    setStatus('Loading...', 'connecting');
    try {
        const info = await send('info');
        state.fromNs = info.from_ns;
        state.toNs = info.to_ns;
        state.numCpus = info.num_cpus;
        const FIFTEEN_MIN_NS = 900 * 1e9;
        state.serverNow = info.now_ns || info.to_ns;
        state.viewFrom = state.serverNow - FIFTEEN_MIN_NS;
        state.viewTo = state.serverNow;
        state.liveRangeSecs = 900;
        setStatus(info.num_cpus + ' CPUs, ~' + fmtCount(info.num_events) + ' events', 'connected');
        updateTimeRange();
        initChart();
        initChartResize();
        initTabs();
        initTimePicker();
        initLiveMode();
        await refresh();
        /* Start in live mode (last 15 min, auto-refresh) */
        startAutoRefresh(900);
    } catch (e) {
        setStatus('Error: ' + e.message, 'error');
    }
}

// -- Refresh ------------------------------------------------------------------

async function refresh() {
    /* Sequential, not parallel. The server processes one request at a time.
     * Parallel requests cause queueing, backlog, and timeouts. */
    await refreshChart();
    await refreshTable();
}

async function refreshChart() {
    try {
        const params = {
            from: state.viewFrom,
            to: state.viewTo,
            buckets: Math.min(Math.floor(chartEl.clientWidth / 4), 300),
            filters: state.filters,
        };
        // Request per-event breakdown when drilled into a class (but not a specific event)
        if (state.filters.class && !state.filters.event_id) {
            params.detail = 'events';
        }
        const data = await send('aas', params);
        console.log('[aas] from:', params.from, 'to:', params.to, 'buckets:', data?.buckets?.length, 'max_aas:', data?.max_aas);
        renderChart(data);
    } catch (e) { /* ignore on disconnect */ }
}

async function refreshTable() {
    if (state.activeTab === 'histogram') {
        await refreshHistogram();
        return;
    }
    if (state.activeTab === 'timeline') {
        await refreshTimeline();
        return;
    }
    if (state.activeTab === 'transitions') {
        await refreshTransitions();
        return;
    }
    if (state.activeTab === 'concurrency') {
        await refreshConcurrency();
        return;
    }
    try {
        const cmdMap = {
            overview: 'time_model',
            events: 'top_events',
            sessions: 'top_sessions',
            queries: 'top_queries',
        };
        const data = await send(cmdMap[state.activeTab], {
            from: state.viewFrom,
            to: state.viewTo,
            filters: state.filters,
        });
        renderTable(state.activeTab, data);
    } catch (e) { /* ignore on disconnect */ }
}

// -- UI helpers ---------------------------------------------------------------

function setStatus(text, cls) {
    const el = document.getElementById('status');
    el.textContent = text;
    el.className = 'status ' + cls;
}

function updateTimeRange() {
    const el = document.getElementById('time-range');
    const from = fmtTime(state.viewFrom);
    const to = fmtTime(state.viewTo);
    const dur = fmtDuration(state.viewTo - state.viewFrom);
    el.textContent = from + ' \u2013 ' + to + ' (' + dur + ')';
}

function zoomTo(from, to) {
    state.zoomHistory.push({ from: state.viewFrom, to: state.viewTo });
    if (state.zoomHistory.length > 10) state.zoomHistory.shift();
    state.viewFrom = from;
    state.viewTo = to;
    updateTimeRange();
    refresh();
}

function zoomOut() {
    if (state.zoomHistory.length > 0) {
        const prev = state.zoomHistory.pop();
        state.viewFrom = prev.from;
        state.viewTo = prev.to;
    } else {
        const mid = (state.viewFrom + state.viewTo) / 2;
        const halfSpan = state.viewTo - state.viewFrom;
        state.viewFrom = Math.max(state.fromNs, mid - halfSpan);
        state.viewTo = Math.min(state.toNs, mid + halfSpan);
    }
    updateTimeRange();
    refresh();
}

function fmtTime(ns, bucketNs) {
    if (!ns) return '--';
    const d = new Date(ns / 1e6);
    const hms = d.toUTCString().slice(17, 25);  // "HH:MM:SS" in UTC
    if (!bucketNs || bucketNs >= 1000000000) return hms;
    const frac = (ns % 1000000000) / 1e9;
    if (bucketNs >= 1000000) return hms + '.' + frac.toFixed(3).slice(2);       // ms
    return hms + '.' + frac.toFixed(6).slice(2);                                // us
}

function fmtTimeMs(ms) {
    if (!ms) return '--';
    const d = new Date(ms);
    const hms = d.toUTCString().slice(17, 25);
    const frac = ms % 1000;
    return hms + '.' + String(frac).padStart(3, '0');
}

function fmtDuration(ns) {
    const s = ns / 1e9;
    if (s < 60) return s.toFixed(0) + 's';
    if (s < 3600) return (s / 60).toFixed(1) + 'm';
    return (s / 3600).toFixed(1) + 'h';
}

function fmtMs(ms) {
    if (ms >= 1000) return (ms / 1000).toFixed(1) + 's';
    if (ms >= 1) return ms.toFixed(1) + 'ms';
    return (ms * 1000).toFixed(0) + '\u00b5s';
}

function fmtCount(n) {
    if (n >= 1e6) return (n / 1e6).toFixed(1) + 'M';
    if (n >= 1e3) return (n / 1e3).toFixed(1) + 'K';
    return n.toString();
}

function fmtPct(p) { return p.toFixed(1) + '%'; }
function fmtAas(a) { return a.toFixed(2); }
function fmtUs(us) {
    if (us >= 1e6) return (us / 1e6).toFixed(1) + 's';
    if (us >= 1000) return (us / 1000).toFixed(1) + 'ms';
    return us.toFixed(0) + '\u00b5s';
}

// -- Color dots ---------------------------------------------------------------

function classColor(name) {
    if (!name) return null;
    // Try exact match first (e.g. "IO", "cpu")
    const lower = name.toLowerCase();
    if (CLASS_COLOR_MAP[lower]) return CLASS_COLOR_MAP[lower];
    // Extract class from "ClassName:EventName"
    const colon = name.indexOf(':');
    if (colon > 0) {
        const cls = name.substring(0, colon).toLowerCase();
        if (CLASS_COLOR_MAP[cls]) return CLASS_COLOR_MAP[cls];
    }
    // "CPU*" -> "cpu"
    const stripped = lower.replace('*', '');
    if (CLASS_COLOR_MAP[stripped]) return CLASS_COLOR_MAP[stripped];
    return null;
}

function dot(name) {
    const color = classColor(name);
    if (!color) return '';
    return '<span class="class-dot" style="background:' + color + '"></span>';
}

// -- Percentage bar -----------------------------------------------------------

function pctBar(pct, color) {
    const w = Math.min(Math.max(pct, 0), 100);
    const c = color || '#4fc3f7';
    return '<div class="pct-bar">' +
        '<div class="pct-fill" style="width:' + w.toFixed(1) + '%;background:' + c + '"></div>' +
        '<span>' + pct.toFixed(1) + '%</span></div>';
}

// -- Stacked bar (Performance Insights style) ---------------------------------

function stackedBar(classes, total) {
    if (!classes || !total || total <= 0) return '';
    let html = '<div class="stacked-bar">';
    for (let i = 0; i < WAIT_CLASSES.length && i < classes.length; i++) {
        const pct = classes[i] / total * 100;
        if (pct < 0.5) continue;
        html += '<div class="bar-seg" style="width:' + pct.toFixed(1) +
                '%;background:' + WAIT_CLASSES[i].color + '" title="' +
                WAIT_CLASSES[i].label + ': ' + fmtMs(classes[i]) +
                ' (' + pct.toFixed(1) + '%)"></div>';
    }
    html += '</div>';
    return html;
}

function eventStackedBar(events, total) {
    if (!events || !events.length || !total || total <= 0) return '';
    let html = '<div class="stacked-bar">';
    for (let i = 0; i < events.length; i++) {
        const pct = events[i].ms / total * 100;
        if (pct < 0.3) continue;
        const color = EVENT_PALETTE[i % EVENT_PALETTE.length];
        html += '<div class="bar-seg" style="width:' + pct.toFixed(1) +
                '%;background:' + color + '" title="' +
                esc(events[i].name) + ': ' + fmtMs(events[i].ms) +
                ' (' + pct.toFixed(1) + '%)"></div>';
    }
    html += '</div>';
    return html;
}

// -- Tabs ---------------------------------------------------------------------

function initTabs() {
    document.querySelectorAll('.tab').forEach(btn => {
        btn.addEventListener('click', () => {
            switchTab(btn.dataset.tab);
        });
    });
}

function switchTab(tab) {
    // Clean up charts when leaving custom tabs
    if (state.activeTab === 'histogram' && tab !== 'histogram') {
        if (heatmapChart) { heatmapChart.dispose(); heatmapChart = null; }
    }
    if (state.activeTab === 'timeline' && tab !== 'timeline') {
        if (timelineChart) { timelineChart.dispose(); timelineChart = null; }
    }
    if (state.activeTab === 'transitions' && tab !== 'transitions') {
        if (transitionsChart) { transitionsChart.dispose(); transitionsChart = null; }
    }
    if (state.activeTab === 'concurrency' && tab !== 'concurrency') {
        if (concurrencyChart) { concurrencyChart.dispose(); concurrencyChart = null; }
    }

    // Stop auto-refresh when switching to concurrency tab
    if (tab === 'concurrency') {
        stopAutoRefresh();
    }

    state.activeTab = tab;
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === tab);
    });
    refreshTable();
}

// -- Chart (ECharts) ----------------------------------------------------------

let chart = null;
const chartEl = document.getElementById('chart-container');

function initChart() {
    chart = echarts.init(chartEl, 'dark', { group: 'pgwt' });
    echarts.connect('pgwt');
    window.addEventListener('resize', () => {
        chart.resize();
        if (concurrencyChart) concurrencyChart.resize();
    });

    // -- Brush selection (Grafana-style click-drag zoom) --
    const overlay = document.createElement('div');
    overlay.className = 'brush-overlay';
    chartEl.appendChild(overlay);

    let brushStart = null;

    chartEl.addEventListener('mousedown', (e) => {
        if (e.button !== 0 || !chart) return;
        // Only start brush inside the chart grid area
        const rect = chartEl.getBoundingClientRect();
        const x = e.clientX - rect.left;
        const gridRect = chart.getModel().getComponent('grid').coordinateSystem.getRect();
        if (x < gridRect.x || x > gridRect.x + gridRect.width) return;
        const y = e.clientY - rect.top;
        if (y < gridRect.y || y > gridRect.y + gridRect.height) return;

        brushStart = { x: x, gridLeft: gridRect.x, gridWidth: gridRect.width };
        overlay.style.left = x + 'px';
        overlay.style.width = '0px';
        overlay.style.top = gridRect.y + 'px';
        overlay.style.height = gridRect.height + 'px';
        overlay.style.display = 'block';
        e.preventDefault();
    });

    document.addEventListener('mousemove', (e) => {
        if (!brushStart) return;
        const rect = chartEl.getBoundingClientRect();
        let x = e.clientX - rect.left;
        x = Math.max(brushStart.gridLeft, Math.min(x, brushStart.gridLeft + brushStart.gridWidth));
        const left = Math.min(brushStart.x, x);
        const width = Math.abs(x - brushStart.x);
        overlay.style.left = left + 'px';
        overlay.style.width = width + 'px';
    });

    document.addEventListener('mouseup', (e) => {
        if (!brushStart) return;
        overlay.style.display = 'none';
        const rect = chartEl.getBoundingClientRect();
        let endX = e.clientX - rect.left;
        endX = Math.max(brushStart.gridLeft, Math.min(endX, brushStart.gridLeft + brushStart.gridWidth));
        const minX = Math.min(brushStart.x, endX);
        const maxX = Math.max(brushStart.x, endX);
        brushStart = null;

        // Small drag (< 5px) = click → drill down into class/event
        if (maxX - minX < 5) {
            handleChartClick(minX, e.clientY - rect.top);
            return;
        }

        // Convert pixel to data index
        const opt = chart.getOption();
        if (!opt.xAxis || !opt.xAxis[0] || !opt.xAxis[0].data) return;
        const data = opt.xAxis[0].data;
        const startIdx = chart.convertFromPixel({ xAxisIndex: 0 }, minX);
        const endIdx = chart.convertFromPixel({ xAxisIndex: 0 }, maxX);
        if (startIdx >= 0 && endIdx < data.length && startIdx < endIdx) {
            stopAutoRefresh();  // Manual zoom stops auto-refresh
            zoomTo(data[startIdx], data[endIdx]);
        }
    });

    // Double-click to zoom out
    chartEl.addEventListener('dblclick', (e) => {
        e.preventDefault();
        zoomOut();
    });

}

// Handle a click on the chart at pixel (px, py) — find the topmost series
function handleChartClick(px, py) {
    if (!chart) return;
    const opt = chart.getOption();
    if (!opt.xAxis || !opt.xAxis[0] || !opt.xAxis[0].data) return;
    const xData = opt.xAxis[0].data;

    // Get grid rect for pixel → data conversion
    const grid = chart.getModel().getComponent('grid');
    if (!grid || !grid.coordinateSystem) return;
    const gridRect = grid.coordinateSystem.getRect();

    // X: pixel → data index
    const xRatio = (px - gridRect.x) / gridRect.width;
    const dataIdx = Math.round(xRatio * (xData.length - 1));
    if (dataIdx < 0 || dataIdx >= xData.length) return;

    // Y: pixel → AAS value (y-axis is inverted: top=max, bottom=0)
    const yRatio = 1 - (py - gridRect.y) / gridRect.height;
    const yMax = opt.yAxis[0].max || 1;
    const yVal = yRatio * yMax;
    if (yVal < 0) return;

    // Walk stacked series bottom-to-top: find which series the click lands in
    const seriesList = opt.series.filter(s => s.stack === 'aas');
    let cumulative = 0;
    let clickedSeries = null;
    for (const s of seriesList) {
        const val = s.data[dataIdx] || 0;
        cumulative += val;
        if (yVal <= cumulative) {
            clickedSeries = s;
            break;
        }
    }
    if (!clickedSeries) return;

    if (state.chartEventSeries) {
        // Event-breakdown mode: click event → drill to event_id
        const evSeries = state.chartEventSeries.find(s => s.name === clickedSeries.name);
        if (evSeries) {
            drillDown('event_id', evSeries.event_id, clickedSeries.name);
        }
    } else {
        // Class mode: click class → drill to class
        const wc = WAIT_CLASSES.find(c => c.label === clickedSeries.name);
        if (wc) {
            drillDown('class', wc.key, wc.label);
        }
    }
}

// -- Chart resize handle --------------------------------------------------

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
        if (chart) chart.resize();
    }

    function onUp() {
        document.removeEventListener('mousemove', onDrag);
        document.removeEventListener('mouseup', onUp);
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
    }
}

// -- Time picker ----------------------------------------------------------

function initTimePicker() {
    const rangeBtn = document.getElementById('time-range');
    const picker = document.getElementById('time-picker');
    const zoomOutBtn = document.getElementById('zoom-out-btn');

    // Toggle picker on time range click
    rangeBtn.addEventListener('click', (e) => {
        e.stopPropagation();
        const visible = picker.style.display !== 'none';
        picker.style.display = visible ? 'none' : 'block';
        if (!visible) {
            // Pre-fill custom inputs
            document.getElementById('tp-from').value = nsToDatetimeLocal(state.viewFrom);
            document.getElementById('tp-to').value = nsToDatetimeLocal(state.viewTo);
        }
    });

    // Close picker on outside click
    document.addEventListener('click', (e) => {
        if (!picker.contains(e.target) && e.target !== rangeBtn) {
            picker.style.display = 'none';
        }
    });
    picker.addEventListener('click', (e) => e.stopPropagation());

    // Quick range buttons — "last N" ranges auto-refresh
    picker.querySelectorAll('.tp-quick button').forEach(btn => {
        btn.addEventListener('click', () => {
            const secs = parseInt(btn.dataset.range);
            picker.style.display = 'none';
            picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');

            // Stop any existing auto-refresh
            stopAutoRefresh();

            if (secs === 0) {
                // "All" — full range, no auto-refresh
                zoomTo(state.fromNs, state.toNs);
            } else {
                // "Last N" — always relative to server wall clock
                state.liveRangeSecs = secs;
                const end = state.serverNow || state.toNs;
                const from = end - secs * 1e9;
                zoomTo(from, end);
                startAutoRefresh(secs);
            }
        });
    });

    // Custom range apply
    document.getElementById('tp-apply').addEventListener('click', () => {
        const fromStr = document.getElementById('tp-from').value;
        const toStr = document.getElementById('tp-to').value;
        if (!fromStr || !toStr) return;
        const fromNs = new Date(fromStr + 'Z').getTime() * 1e6;
        const toNs = new Date(toStr + 'Z').getTime() * 1e6;
        if (fromNs >= toNs) return;
        picker.style.display = 'none';
        picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
        stopAutoRefresh();  // Custom range stops auto-refresh
        zoomTo(Math.max(state.fromNs, fromNs), Math.min(state.toNs, toNs));
    });

    // Zoom out button
    zoomOutBtn.addEventListener('click', () => { stopAutoRefresh(); zoomOut(); });
}

function nsToDatetimeLocal(ns) {
    const d = new Date(ns / 1e6);
    // Format as YYYY-MM-DDTHH:MM:SS for datetime-local input
    const pad = (n) => String(n).padStart(2, '0');
    return d.getUTCFullYear() + '-' + pad(d.getUTCMonth() + 1) + '-' + pad(d.getUTCDate()) +
        'T' + pad(d.getUTCHours()) + ':' + pad(d.getUTCMinutes()) + ':' + pad(d.getUTCSeconds());
}

function renderChart(data) {
    if (!chart || !data.buckets || data.buckets.length === 0) return;

    const bns = data.bucket_ns || 0;
    const times = data.buckets.map(b => b.t);
    const isEventBreakdown = data.breakdown === 'events' && data.series;

    // Store event series info for click handler
    state.chartEventSeries = isEventBreakdown ? data.series : null;

    let series;
    let legendData;

    if (isEventBreakdown) {
        // Per-event breakdown mode: dynamic series from server
        series = data.series.map((s, idx) => ({
            name: s.name,
            type: 'line',
            stack: 'aas',
            areaStyle: { opacity: 0.85 },
            lineStyle: { width: 0 },
            emphasis: { focus: 'series' },
            symbol: 'none',
            color: EVENT_PALETTE[idx % EVENT_PALETTE.length],
            data: data.buckets.map(b => +(b.aas[idx] || 0).toFixed(4)),
        }));
        legendData = data.series.map(s => s.name);
    } else {
        // Class breakdown mode (default)
        series = WAIT_CLASSES.map(wc => ({
            name: wc.label,
            type: 'line',
            stack: 'aas',
            areaStyle: { opacity: 0.85 },
            lineStyle: { width: 0 },
            emphasis: { focus: 'series' },
            symbol: 'none',
            color: wc.color,
            data: data.buckets.map(b => +(b[wc.key] || 0).toFixed(4)),
        }));
        legendData = WAIT_CLASSES.map(c => c.label);
    }

    // CPU reference line
    const markData = [];
    if (state.numCpus > 0) {
        markData.push({
            yAxis: state.numCpus,
            label: {
                formatter: state.numCpus + ' CPUs',
                position: 'insideEndTop',
                color: '#fff',
                fontSize: 11,
            },
            lineStyle: { color: '#E53935', type: 'dashed', width: 2 },
        });
    }

    const yMax = Math.max(
        data.max_aas * 1.2,
        state.numCpus > 0 ? state.numCpus * 1.5 : 0,
        1
    );

    const option = {
        backgroundColor: 'transparent',
        tooltip: {
            trigger: 'axis',
            axisPointer: { type: 'cross' },
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: function(params) {
                if (!params.length) return '';
                let t = fmtTime(params[0].axisValue, bns);
                let total = 0;
                let items = [];
                for (let i = params.length - 1; i >= 0; i--) {
                    const p = params[i];
                    if (p.value > 0.001) {
                        items.push({ name: p.seriesName, value: p.value, color: p.color });
                        total += p.value;
                    }
                }
                let lines = items.map(function(it) {
                    const pct = total > 0 ? (it.value / total * 100).toFixed(0) : '0';
                    return '<span style="color:' + it.color + '">\u25cf</span> ' +
                        it.name + ': <b>' + it.value.toFixed(2) + '</b> (' + pct + '%)';
                });
                return '<b>' + t + '</b><br>Total AAS: <b>' +
                    total.toFixed(2) + '</b><br>' + lines.join('<br>');
            },
        },
        legend: {
            data: legendData,
            bottom: 0,
            textStyle: { color: '#888', fontSize: 11 },
            itemWidth: 12,
            itemHeight: 8,
        },
        grid: {
            left: 50, right: 20, top: 30, bottom: 40,
        },
        dataZoom: [
            { type: 'inside', xAxisIndex: 0, zoomOnMouseWheel: false, moveOnMouseWheel: false },
        ],
        xAxis: {
            type: 'category',
            data: times,
            axisLabel: {
                color: '#888',
                fontSize: 10,
                formatter: function(v) { return fmtTime(v, bns); },
            },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'value',
            name: 'Active Sessions',
            nameTextStyle: { color: '#888', fontSize: 11 },
            min: 0,
            max: yMax,
            axisLabel: { color: '#888', fontSize: 10 },
            splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        series: [
            ...series,
            // CPU markLine as a dummy series
            {
                name: '_cpu_line',
                type: 'line',
                data: [],
                markLine: {
                    silent: true,
                    symbol: 'none',
                    data: markData,
                },
            },
        ],
    };

    chart.setOption(option, true);

    // Concurrency moved to dedicated tab (not overlay)
}

// Concurrency overlay removed — moved to dedicated Concurrency tab

// -- Tables -------------------------------------------------------------------

const TABLE_CONFIGS = {
    overview: {
        columns: [
            { key: 'name', label: 'Stat Name', format: (r) => {
                if (r.indent >= 1) return dot(r.name) + esc(r.name);
                return esc(r.name);
            }},
            { key: 'ms', label: 'Time', cls: 'num', format: (r) => fmtMs(r.ms) },
            { key: 'pct', label: '%DB Time', cls: 'num', format: (r) => {
                if (r.indent === 0 && r.name === 'DB Time') return fmtPct(r.pct);
                const color = classColor(r.name) || '#4fc3f7';
                return pctBar(r.pct, color);
            }},
            { key: 'aas', label: 'AAS', cls: 'num', format: (r) => fmtAas(r.aas) },
        ],
        rowClass: (r) => {
            let c = '';
            if (r.indent === 1) c += ' indent-1 clickable';
            if (r.indent === 2) c += ' indent-2';
            return c;
        },
        onClick: (r) => {
            if (r.indent !== 1) return;
            const cls = r.name.replace('*', '');
            drillDown('class', cls, cls);
        },
    },
    events: {
        columns: [
            { key: 'name', label: 'Wait Event', format: (r) => dot(r.name) + esc(r.name) },
            { key: 'count', label: 'Count', cls: 'num', format: (r) => fmtCount(r.count) },
            { key: 'total_ms', label: 'Total', cls: 'num', format: (r) => fmtMs(r.total_ms) },
            { key: 'avg_us', label: 'Avg', cls: 'num', format: (r) => fmtUs(r.avg_us) },
            { key: 'p50_us', label: 'P50', cls: 'num', format: (r) => fmtUs(r.p50_us) },
            { key: 'p95_us', label: 'P95', cls: 'num', format: (r) => fmtUs(r.p95_us) },
            { key: 'p99_us', label: 'P99', cls: 'num', format: (r) => fmtUs(r.p99_us) },
            { key: 'max_us', label: 'Max', cls: 'num', format: (r) => fmtUs(r.max_us) },
            { key: 'pct', label: '%DB', cls: 'num', format: (r) => {
                const color = classColor(r.name) || '#4fc3f7';
                return pctBar(r.pct, color);
            }},
            { key: 'aas', label: 'AAS', cls: 'num', format: (r) => fmtAas(r.aas) },
        ],
        rowClass: () => 'clickable',
        onClick: (r) => {
            drillDown('event_id', r.event_id, r.name);
        },
    },
    sessions: {
        columns: [
            { key: 'pid', label: 'PID', format: (r) => r.pid },
            { key: 'type', label: 'Backend', format: (r) => esc(r.type || 'unknown') },
            { key: 'user', label: 'User', format: (r) => esc(r.user || '') },
            { key: 'db', label: 'Database', format: (r) => esc(r.db || '') },
            { key: 'db_time_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.db_time_ms) },
            { key: 'cpu_pct', label: 'CPU%', cls: 'num', format: (r) => pctBar(r.cpu_pct, 'rgb(80,250,123)') },
            { key: 'wait_pct', label: 'Wait%', cls: 'num', format: (r) => {
                const color = classColor(r.top_wait) || '#F44336';
                return pctBar(r.wait_pct, color);
            }},
            { key: 'top_wait', label: 'Top Wait', format: (r) => dot(r.top_wait) + esc(r.top_wait) },
        ],
        rowClass: () => 'clickable',
        onClick: (r) => {
            drillDown('pid', r.pid, 'PID ' + r.pid);
        },
    },
    queries: {
        columns: [
            { key: 'query_id', label: 'Query ID', format: (r) =>
                '<span class="query-id">' + r.query_id + '</span>' },
            { key: 'text', label: 'Query Text', format: (r) => {
                if (!r.text) return '<span style="color:#555">—</span>';
                const truncated = esc(r.text.substring(0, 120)) +
                    (r.text.length > 120 ? '...' : '');
                return '<span class="qt-hover" data-fulltext="' +
                    esc(r.text).replace(/"/g, '&quot;') + '">' +
                    dot(r.top_wait) + truncated + '</span>';
            }},
            { key: 'total_ms', label: 'Time', cls: 'num', format: (r) => fmtMs(r.total_ms) },
            { key: 'pct', label: '%DB', cls: 'num', format: (r) => fmtPct(r.pct) },
            { key: 'classes', label: 'Wait Profile', format: (r) =>
                r.events ? eventStackedBar(r.events, r.total_ms)
                         : stackedBar(r.classes, r.total_ms) },
            { key: 'count', label: 'Waits', cls: 'num', format: (r) => fmtCount(r.count) },
            { key: 'avg_us', label: 'Avg Wait', cls: 'num', format: (r) => fmtUs(r.avg_us) },
        ],
        rowClass: () => 'clickable',
        onClick: (r) => {
            drillDown('query_id', r.query_id, r.text ? r.text.substring(0, 40) : 'Query ' + r.query_id);
        },
    },
};

// -- Summary bar (above overview table) ---------------------------------------

function renderSummary(data) {
    const el = document.getElementById('summary-bar');
    if (!data || !data.rows || data.rows.length === 0) {
        el.innerHTML = '';
        return;
    }

    const dbRow = data.rows[0]; // "DB Time" is always first
    const idleRow = data.rows.find(r => r.indent === 0 && r.name.indexOf('Idle') >= 0);

    let html = '<div class="metric"><span class="metric-label">DB Time</span>' +
        '<span class="metric-value">' + fmtMs(dbRow.ms) + '</span></div>';

    if (data.wall_ms) {
        html += '<div class="metric"><span class="metric-label">Wall</span>' +
            '<span class="metric-value">' + fmtMs(data.wall_ms) + '</span></div>';
    }

    html += '<div class="metric"><span class="metric-label">AAS</span>' +
        '<span class="metric-value">' + fmtAas(dbRow.aas) + '</span></div>';

    if (idleRow) {
        html += '<div class="metric"><span class="metric-label">Idle</span>' +
            '<span class="metric-value">' + fmtMs(idleRow.ms) + '</span></div>';
    }

    html += '<div class="metric"><span class="metric-label">CPUs</span>' +
        '<span class="metric-value">' + state.numCpus + '</span></div>';

    el.innerHTML = html;
}

// -- Table rendering ----------------------------------------------------------

function renderTable(tab, data) {
    const container = document.getElementById('table-container');
    const summaryEl = document.getElementById('summary-bar');
    const config = TABLE_CONFIGS[tab];

    if (!config || !data) {
        summaryEl.innerHTML = '';
        container.innerHTML = '<div class="loading">No data</div>';
        return;
    }

    // Summary bar only for overview
    if (tab === 'overview') {
        renderSummary(data);
    } else {
        summaryEl.innerHTML = '';
    }

    const rows = data.rows || [];
    if (rows.length === 0) {
        container.innerHTML = '<div class="loading">No data for selected range</div>';
        return;
    }

    // Sort (skip overview — it has a meaningful hierarchy)
    const sortKey = state.sortCol[tab];
    if (sortKey && tab !== 'overview') {
        const asc = state.sortAsc[tab];
        rows.sort((a, b) => {
            const va = a[sortKey], vb = b[sortKey];
            if (typeof va === 'number' && typeof vb === 'number')
                return asc ? va - vb : vb - va;
            return asc ? String(va).localeCompare(String(vb))
                       : String(vb).localeCompare(String(va));
        });
    }

    let html = '<table><thead><tr>';
    for (const col of config.columns) {
        const arrow = state.sortCol[tab] === col.key
            ? (state.sortAsc[tab] ? ' \u25b2' : ' \u25bc') : '';
        html += '<th class="' + (col.cls || '') + '" data-sort="' + col.key + '">' +
                col.label + arrow + '</th>';
    }
    html += '</tr></thead><tbody>';

    for (let ri = 0; ri < rows.length; ri++) {
        const row = rows[ri];
        const cls = config.rowClass ? config.rowClass(row) : '';
        html += '<tr class="' + cls + '" data-row="' + ri + '">';
        for (const col of config.columns) {
            html += '<td class="' + (col.cls || '') + '">' + col.format(row) + '</td>';
        }
        html += '</tr>';
    }
    html += '</tbody></table>';
    container.innerHTML = html;

    // Attach click handlers
    container.querySelectorAll('th[data-sort]').forEach(th => {
        th.addEventListener('click', () => {
            const key = th.dataset.sort;
            if (state.sortCol[tab] === key) {
                state.sortAsc[tab] = !state.sortAsc[tab];
            } else {
                state.sortCol[tab] = key;
                state.sortAsc[tab] = false;
            }
            renderTable(tab, data);
        });
    });

    container.querySelectorAll('tr.clickable').forEach(tr => {
        tr.addEventListener('click', () => {
            const idx = parseInt(tr.dataset.row);
            if (config.onClick && rows[idx]) {
                config.onClick(rows[idx]);
            }
        });
    });

    // Query text tooltip
    container.querySelectorAll('.qt-hover').forEach(el => {
        el.addEventListener('mouseenter', (e) => {
            const text = el.getAttribute('data-fulltext');
            if (!text || text.length <= 120) return;
            const tip = document.getElementById('query-tooltip');
            tip.textContent = text;
            tip.style.display = 'block';
            positionTooltip(tip, e);
        });
        el.addEventListener('mousemove', (e) => {
            const tip = document.getElementById('query-tooltip');
            if (tip.style.display === 'block') positionTooltip(tip, e);
        });
        el.addEventListener('mouseleave', () => {
            document.getElementById('query-tooltip').style.display = 'none';
        });
    });
}

function positionTooltip(tip, e) {
    const pad = 12;
    let x = e.clientX + pad;
    let y = e.clientY + pad;
    // Keep within viewport
    const rect = tip.getBoundingClientRect();
    if (x + rect.width > window.innerWidth - pad)
        x = e.clientX - rect.width - pad;
    if (y + rect.height > window.innerHeight - pad)
        y = e.clientY - rect.height - pad;
    tip.style.left = x + 'px';
    tip.style.top = y + 'px';
}

// -- Drill-down ---------------------------------------------------------------

function drillDown(filterKey, filterValue, label) {
    state.breadcrumbs.push({
        label: label,
        filters: { ...state.filters },
        tab: state.activeTab,
    });
    state.filters[filterKey] = filterValue;

    // Auto-pivot: update tab state without triggering a separate refresh
    const pivotMap = { class: 'events', event_id: 'queries', pid: 'timeline', query_id: 'events' };
    if (pivotMap[filterKey]) {
        const tab = pivotMap[filterKey];
        state.activeTab = tab;
        document.querySelectorAll('.tab').forEach(b => {
            b.classList.toggle('active', b.dataset.tab === tab);
        });
    }

    updateBreadcrumb();
    refresh();
}

function drillUp(index) {
    const crumb = state.breadcrumbs[index];
    state.filters = { ...crumb.filters };
    state.breadcrumbs = state.breadcrumbs.slice(0, index);
    // Set tab state without triggering a separate refreshTable
    const tab = crumb.tab;
    state.activeTab = tab;
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === tab);
    });
    updateBreadcrumb();
    refresh();
}

function clearFilters() {
    state.filters = {};
    state.breadcrumbs = [];
    updateBreadcrumb();
    // Set tab state without triggering a separate refreshTable
    state.activeTab = 'overview';
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === 'overview');
    });
    refresh();
}

function updateBreadcrumb() {
    const el = document.getElementById('breadcrumb');
    if (state.breadcrumbs.length === 0 && Object.keys(state.filters).length === 0) {
        el.innerHTML = '';
        return;
    }

    let html = '';
    state.breadcrumbs.forEach((crumb, i) => {
        if (i > 0) html += '<span class="crumb-sep">\u203a</span>';
        html += '<span class="crumb" data-idx="' + i + '">' +
            dot(crumb.label) + esc(crumb.label) + '</span>';
    });

    // Current filter
    const filterParts = [];
    for (const [k, v] of Object.entries(state.filters)) {
        filterParts.push(k + '=' + v);
    }
    if (filterParts.length > 0) {
        if (state.breadcrumbs.length > 0) html += '<span class="crumb-sep">\u203a</span>';
        html += '<span style="color:#4fc3f7">' + esc(filterParts.join(', ')) + '</span>';
        html += ' <span class="crumb-clear" title="Clear all filters">\u2715</span>';
    }

    el.innerHTML = html;

    el.querySelectorAll('.crumb').forEach(crumb => {
        crumb.addEventListener('click', () => {
            drillUp(parseInt(crumb.dataset.idx));
        });
    });

    const clearBtn = el.querySelector('.crumb-clear');
    if (clearBtn) {
        clearBtn.addEventListener('click', clearFilters);
    }
}

function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// -- Histogram heatmap --------------------------------------------------------

let heatmapChart = null;
let heatmapEvents = []; // cached event list for selector

async function refreshHistogram() {
    const container = document.getElementById('table-container');
    const summaryEl = document.getElementById('summary-bar');
    summaryEl.innerHTML = '';

    // Build the selector + heatmap container if not present
    if (!document.getElementById('heatmap-container')) {
        container.innerHTML =
            '<div class="event-selector">' +
            '  <label>Class</label>' +
            '  <select id="hm-class"><option value="">All</option></select>' +
            '  <label>Event</label>' +
            '  <select id="hm-event"><option value="">All</option></select>' +
            '</div>' +
            '<div id="heatmap-container"></div>';

        document.getElementById('hm-class').addEventListener('change', onClassChange);
        document.getElementById('hm-event').addEventListener('change', onEventChange);
    }

    // Fetch event list (for selector)
    try {
        const evData = await send('top_events', {
            from: state.viewFrom, to: state.viewTo, filters: {},
        });
        heatmapEvents = evData.rows || [];
        populateSelectors();
    } catch (e) { /* ignore */ }

    await fetchAndRenderHeatmap();
}

function populateSelectors() {
    const classSelect = document.getElementById('hm-class');
    const eventSelect = document.getElementById('hm-event');
    if (!classSelect || !eventSelect) return;

    // Gather unique classes
    const classes = new Set();
    heatmapEvents.forEach(r => { if (r.class) classes.add(r.class); });

    const prevClass = classSelect.value;
    classSelect.innerHTML = '<option value="">All</option>';
    for (const cls of classes) {
        const opt = document.createElement('option');
        opt.value = cls;
        opt.textContent = cls;
        classSelect.appendChild(opt);
    }

    // Restore class selection from filter
    if (state.filters.class) {
        classSelect.value = state.filters.class;
    } else if (prevClass) {
        classSelect.value = prevClass;
    }

    // Populate events filtered by selected class
    populateEventSelect();
}

function populateEventSelect() {
    const classSelect = document.getElementById('hm-class');
    const eventSelect = document.getElementById('hm-event');
    if (!classSelect || !eventSelect) return;

    const selClass = classSelect.value;
    const filtered = selClass
        ? heatmapEvents.filter(r => r.class === selClass)
        : heatmapEvents;

    const prevEvent = eventSelect.value;
    eventSelect.innerHTML = '<option value="">All' + (selClass ? ' ' + selClass : '') + '</option>';
    for (const ev of filtered.slice(0, 50)) {
        const opt = document.createElement('option');
        opt.value = ev.event_id;
        opt.textContent = ev.name + ' (' + fmtCount(ev.count) + ')';
        eventSelect.appendChild(opt);
    }

    // Restore event selection from filter
    if (state.filters.event_id) {
        eventSelect.value = state.filters.event_id;
    } else if (prevEvent) {
        eventSelect.value = prevEvent;
    }
}

function onClassChange() {
    populateEventSelect();
    document.getElementById('hm-event').value = '';
    fetchAndRenderHeatmap();
}

function onEventChange() {
    fetchAndRenderHeatmap();
}

async function fetchAndRenderHeatmap() {
    const classSelect = document.getElementById('hm-class');
    const eventSelect = document.getElementById('hm-event');
    if (!classSelect || !eventSelect) return;

    // Build filters combining existing breadcrumb filters + selector
    const filters = { ...state.filters };
    if (classSelect.value) filters.class = classSelect.value;
    if (eventSelect.value) filters.event_id = parseInt(eventSelect.value);

    try {
        const data = await send('heatmap', {
            from: state.viewFrom,
            to: state.viewTo,
            buckets: Math.min(Math.floor(window.innerWidth / 6), 300),
            filters: filters,
        });
        renderHeatmap(data);
    } catch (e) { /* ignore */ }
}

function renderHeatmap(data) {
    const el = document.getElementById('heatmap-container');
    if (!el) return;

    if (!data || !data.cells || data.cells.length === 0) {
        if (heatmapChart) { heatmapChart.dispose(); heatmapChart = null; }
        el.innerHTML = '<div class="loading">No data for selected event/range</div>';
        return;
    }

    if (!heatmapChart) {
        heatmapChart = echarts.init(el, 'dark');
        window.addEventListener('resize', () => { if (heatmapChart) heatmapChart.resize(); });
    }

    const hbns = data.bucket_ns || 0;
    const timeLabels = data.times.map(t => fmtTime(t, hbns));
    const latLabels = data.labels || [];

    // Convert sparse cells to echarts format [x, y, value]
    const hmData = data.cells.map(c => [c[0], c[1], c[2]]);

    const option = {
        backgroundColor: 'transparent',
        tooltip: {
            position: 'top',
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: function(p) {
                return '<b>' + timeLabels[p.data[0]] + '</b><br>' +
                    'Latency: ' + latLabels[p.data[1]] + '<br>' +
                    'Count: <b>' + p.data[2].toLocaleString() + '</b>';
            },
        },
        grid: {
            left: 90, right: 40, top: 10, bottom: 60,
        },
        xAxis: {
            type: 'category',
            data: timeLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } },
            splitArea: { show: false },
        },
        yAxis: {
            type: 'category',
            data: latLabels,
            axisLabel: { color: '#888', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } },
            splitArea: { show: false },
        },
        visualMap: {
            min: 0,
            max: data.max_count || 1,
            calculable: false,
            orient: 'horizontal',
            left: 'center',
            bottom: 0,
            itemWidth: 12,
            itemHeight: 120,
            textStyle: { color: '#888', fontSize: 10 },
            inRange: {
                color: ['#1a5276', '#2196F3', '#4CAF50',
                        '#FFEB3B', '#FF9800', '#F44336'],
            },
        },
        series: [{
            type: 'heatmap',
            data: hmData,
            emphasis: {
                itemStyle: { borderColor: '#fff', borderWidth: 1 },
            },
            itemStyle: { borderWidth: 0 },
        }],
    };

    heatmapChart.setOption(option, true);
}

// -- Session Timeline ---------------------------------------------------------

let timelineChart = null;

async function refreshTimeline() {
    const container = document.getElementById('table-container');
    const summaryEl = document.getElementById('summary-bar');
    summaryEl.innerHTML = '';

    if (!state.filters.pid && !state.filters.query_id) {
        container.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
            'Select a session (PID) or query to view timeline</div>';
        return;
    }

    try {
        const data = await send('session_timeline', {
            from: state.viewFrom,
            to: state.viewTo,
            filters: state.filters,
        });
        renderTimeline(container, data);
    } catch (e) {
        container.innerHTML = '<div style="padding:20px;color:#f44">Error: ' + esc(e.message) + '</div>';
    }
}

function renderTimeline(container, data) {
    if (!data || !data.events || data.events.length === 0) {
        if (timelineChart) { timelineChart.dispose(); timelineChart = null; }
        container.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
            'No events for selected session/range</div>';
        return;
    }

    // Truncation warning + chart container
    let html = '';
    if (data.truncated) {
        html += '<div style="padding:8px 20px;font-size:12px;color:#ffd700;background:#3d3200;' +
            'border-bottom:1px solid #555">Showing ' + data.events.length + ' of ' +
            data.total_count + ' events. Zoom in for more detail.</div>';
    }
    const numPids = data.pids.length;
    const chartHeight = Math.max(200, numPids * 50 + 80);
    html += '<div id="timeline-chart" style="height:' + chartHeight + 'px;padding:10px 20px"></div>';
    container.innerHTML = html;

    const el = document.getElementById('timeline-chart');
    if (timelineChart) { timelineChart.dispose(); timelineChart = null; }
    timelineChart = echarts.init(el, 'dark');
    window.addEventListener('resize', () => { if (timelineChart) timelineChart.resize(); });

    // Map PIDs to Y-axis categories
    const pidLabels = data.pids.map(p => 'PID ' + p);
    const pidIndexMap = {};
    data.pids.forEach((p, i) => { pidIndexMap[p] = i; });

    // Build data: [startNs, endNs, pidIdx, name, classIdx, queryId, durNs]
    const barData = data.events.map(ev => [
        ev.s, ev.s + ev.d, pidIndexMap[ev.p] || 0,
        ev.n, ev.c, ev.q, ev.d
    ]);

    const option = {
        backgroundColor: 'transparent',
        tooltip: {
            trigger: 'item',
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: function(params) {
                const d = params.data;
                let s = '<b>' + esc(d[3]) + '</b><br>';
                s += 'Duration: <b>' + fmtUs(d[6] / 1000) + '</b><br>';
                s += 'Start: ' + fmtTime(d[0]) + '<br>';
                if (d[5] && d[5] !== '0') s += 'Query: ' + d[5];
                return s;
            },
        },
        grid: { left: 100, right: 20, top: 20, bottom: 40 },
        xAxis: {
            type: 'value',
            min: state.viewFrom,
            max: state.viewTo,
            axisLabel: {
                color: '#888', fontSize: 10,
                formatter: function(v) { return fmtTime(v); },
            },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'category',
            data: pidLabels,
            axisLabel: { color: '#aaa', fontSize: 11 },
            axisLine: { lineStyle: { color: '#333' } },
        },
        series: [{
            type: 'custom',
            renderItem: function(params, api) {
                const startVal = api.value(0);
                const endVal = api.value(1);
                const catIdx = api.value(2);
                const classIdx = api.value(4);

                const start = api.coord([startVal, catIdx]);
                const end = api.coord([endVal, catIdx]);
                const bandWidth = api.size([0, 1])[1];

                const color = WAIT_CLASSES[classIdx] ? WAIT_CLASSES[classIdx].color : '#888';
                const rectHeight = bandWidth * 0.6;
                const width = Math.max(end[0] - start[0], 1);

                return {
                    type: 'rect',
                    shape: {
                        x: start[0],
                        y: start[1] - rectHeight / 2,
                        width: width,
                        height: rectHeight,
                    },
                    style: { fill: color },
                    styleEmphasis: { fill: color, opacity: 0.8, lineWidth: 1, stroke: '#fff' },
                };
            },
            encode: { x: [0, 1], y: 2 },
            data: barData,
        }],
    };

    timelineChart.setOption(option, true);
}

// -- Concurrency tab ----------------------------------------------------------

let concurrencyChart = null;

async function refreshConcurrency() {
    const container = document.getElementById('table-container');

    // Use same bucket count as main AAS chart for synchronized xAxis
    const numBuckets = Math.min(Math.floor(chartEl.clientWidth / 4), 300);

    let data;
    try {
        data = await send('concurrency', {
            from: state.viewFrom,
            to: state.viewTo,
            buckets: numBuckets,
            filters: state.filters,
        });
    } catch (e) {
        container.innerHTML = '<p style="color:#888;padding:20px">Concurrency: ' + e.message + '</p>';
        return;
    }

    if (!data || !data.peaks || data.peaks.length === 0) {
        container.innerHTML = '<p style="color:#888;padding:20px">No concurrency data</p>';
        return;
    }

    // Build HTML: chart + burst table
    container.innerHTML =
        '<div id="concurrency-chart" style="width:100%;height:350px;"></div>' +
        '<div id="burst-table" style="padding:10px;"></div>';

    // -- Peak concurrency line chart --
    const el = document.getElementById('concurrency-chart');
    if (concurrencyChart) { concurrencyChart.dispose(); concurrencyChart = null; }
    concurrencyChart = echarts.init(el, 'dark', { group: 'pgwt' });
    echarts.connect('pgwt');

    const times = data.peaks.map(p => p.t);
    const bns = data.bucket_ns || 0;
    const peakData = data.peaks.map(p => p.max || 0);
    const peakEvents = data.peaks.map(p => p.event || '');

    // Burst markers on the line
    const burstPoints = (data.bursts || []).map(b => {
        const idx = data.peaks.findIndex(p => p.t >= b.timestamp_ns);
        return {
            coord: [Math.max(0, idx), peakData[Math.max(0, idx)] || b.sessions],
            value: b.sessions,
            symbol: 'triangle',
            symbolSize: Math.min(10 + b.sessions * 2, 30),
            itemStyle: { color: '#f44' },
            label: { show: true, formatter: b.sessions + '', color: '#fff', fontSize: 10 },
        };
    });

    concurrencyChart.setOption({
        title: {
            text: 'Peak Concurrent Sessions per Wait Event',
            left: 'center',
            textStyle: { color: '#ccc', fontSize: 14 },
        },
        tooltip: {
            trigger: 'axis',
            axisPointer: { type: 'cross' },
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            formatter: params => {
                if (!params.length) return '';
                const idx = params[0].dataIndex;
                const ev = peakEvents[idx] || 'none';
                return fmtTime(params[0].axisValue, bns) + '<br/>' +
                       'Peak: <b>' + params[0].value + ' sessions</b><br/>' +
                       'Event: ' + ev;
            },
        },
        grid: { left: 50, right: 20, top: 50, bottom: 40 },
        xAxis: {
            type: 'category',
            data: times,
            axisLabel: {
                color: '#888', fontSize: 10,
                formatter: v => fmtTime(v, bns),
            },
            axisLine: { lineStyle: { color: '#333' } },
        },
        yAxis: {
            type: 'value',
            name: 'Simultaneous Sessions',
            nameTextStyle: { color: '#888', fontSize: 11 },
            min: 0,
            axisLabel: { color: '#888' },
            splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        series: [{
            type: 'line',
            data: peakData,
            areaStyle: { color: 'rgba(248,170,170,0.15)' },
            lineStyle: { color: '#f8a', width: 2 },
            itemStyle: { color: '#f8a' },
            symbol: 'none',
            markPoint: burstPoints.length > 0 ? { data: burstPoints } : undefined,
        }],
    }, true);

    // -- Peak moments table --
    // Show the top N buckets by peak concurrency
    const topPeaks = data.peaks
        .filter(p => p.max > 1)
        .sort((a, b) => b.max - a.max)
        .slice(0, 10);

    let html = '<h3 style="color:#ccc;margin:10px 0 5px">Top Peak Moments</h3>' +
        '<table class="data-table"><thead><tr>' +
        '<th>Time</th><th>Wait Event</th><th>Simultaneous Sessions</th>' +
        '</tr></thead><tbody>';

    topPeaks.forEach(p => {
        html += `<tr><td>${fmtTimeMs(p.t_ms)}</td><td>${p.event || 'CPU*'}</td>` +
                `<td><b>${p.max}</b></td></tr>`;
    });
    html += '</tbody></table>';

    // -- Burst table --
    const bursts = data.bursts || [];
    if (bursts.length > 0) {
        html += '<h3 style="color:#ccc;margin:15px 0 5px">Burst Events ' +
                '<span style="color:#666;font-size:12px">(4+ sessions within 10ms)</span></h3>' +
                '<table class="data-table"><thead><tr>' +
                '<th>Time</th><th>Wait Event</th><th>Sessions</th><th>PIDs</th>' +
                '</tr></thead><tbody>';

        bursts.forEach(b => {
            const time = fmtTimeMs(b.timestamp_ms);
            const pids = (b.pids || []).slice(0, 8).join(', ') +
                         (b.pids && b.pids.length > 8 ? '...' : '');
            html += `<tr><td>${time}</td><td>${b.event}</td>` +
                    `<td><b>${b.sessions}</b></td><td>${pids}</td></tr>`;
        });
        html += '</tbody></table>';
    } else {
        html += '<p style="color:#666;margin-top:15px">No burst events detected</p>';
    }

    document.getElementById('burst-table').innerHTML = html;
}

// -- Auto-refresh for "last N" ranges -----------------------------------------

let autoRefreshInterval = null;

function startAutoRefresh(rangeSecs) {
    stopAutoRefresh();
    const liveBtn = document.getElementById('live-btn');
    if (liveBtn) {
        liveBtn.classList.add('active');
        liveBtn.textContent = 'Live ●';
    }

    /* Use a sequential loop, not setInterval. Each tick waits for the
     * previous one to complete before scheduling the next. This prevents
     * request pile-up that causes timeouts on the transitions tab. */
    autoRefreshInterval = true;  /* flag, not timer ID */
    (async function autoRefreshLoop() {
        while (autoRefreshInterval) {
            try {
                const info = await send('info', {});
                if (info) {
                    if (info.to_ns) state.toNs = info.to_ns;
                    if (info.from_ns) state.fromNs = info.from_ns;
                    state.serverNow = info.now_ns || info.to_ns;
                    state.viewFrom = state.serverNow - rangeSecs * 1e9;
                    state.viewTo = state.serverNow;
                }
                await refresh();
            } catch (e) { /* ignore on disconnect */ }
            /* Wait 5 seconds AFTER completion before next tick */
            await new Promise(r => setTimeout(r, 5000));
        }
    })();
}

function stopAutoRefresh() {
    autoRefreshInterval = null;  /* stops the loop */
    const liveBtn = document.getElementById('live-btn');
    if (liveBtn) {
        liveBtn.classList.remove('active');
        liveBtn.textContent = 'Live';
    }
}

function initLiveMode() {
    const btn = document.getElementById('live-btn');
    if (!btn) return;

    btn.addEventListener('click', () => {
        if (autoRefreshInterval) {
            stopAutoRefresh();
        } else {
            // Default: last 5 minutes from server wall clock
            state.liveRangeSecs = 300;
            const end = state.serverNow || state.toNs;
            state.viewFrom = end - 300e9;
            state.viewTo = end;
            startAutoRefresh(300);
            refresh();
        }
    });
}

// -- Transitions Matrix -------------------------------------------------------

let transitionsChart = null;

async function refreshTransitions() {
    const container = document.getElementById('table-container');

    let data;
    try {
        data = await send('transitions', {
            from: state.viewFrom,
            to: state.viewTo,
            filters: state.filters,
            num_buckets: 200,  // Get enough pairs to build a complete matrix
        });
    } catch (e) {
        container.innerHTML = '<p style="color:#888;padding:20px">Transitions error: ' + e.message + '</p>';
        return;
    }

    if (!data || !data.links || data.links.length === 0) {
        container.innerHTML = '<p style="color:#888;padding:20px">No transitions found</p>';
        return;
    }

    // Build transition lookup: { "from|to": { count, duration_ms } }
    const lookup = {};
    const fromTotals = {};  // total outgoing transitions per source
    data.links.forEach(l => {
        lookup[l.source + '|' + l.target] = l;
        fromTotals[l.source] = (fromTotals[l.source] || 0) + l.value;
    });

    // Collect unique event names, sorted by total involvement (from + to)
    const involvement = {};
    data.links.forEach(l => {
        involvement[l.source] = (involvement[l.source] || 0) + l.value;
        involvement[l.target] = (involvement[l.target] || 0) + l.value;
    });
    const events = Object.keys(involvement)
        .sort((a, b) => involvement[b] - involvement[a])
        .slice(0, 15);  // Cap at 15 for readability

    // Keep full Type:Event format for labels
    const shortName = name => name;

    // Build heatmap data: [toIdx, fromIdx, count]
    const heatData = [];
    let maxCount = 0;
    for (let fi = 0; fi < events.length; fi++) {
        for (let ti = 0; ti < events.length; ti++) {
            const key = events[fi] + '|' + events[ti];
            const l = lookup[key];
            const count = l ? l.value : 0;
            if (count > maxCount) maxCount = count;
            heatData.push([ti, fi, count]);
        }
    }

    // Build HTML layout
    container.innerHTML =
        '<div id="transitions-chart" style="width:100%;height:500px;"></div>' +
        '<div id="transitions-detail" style="padding:10px;"></div>' +
        '<div id="transitions-table" style="padding:10px;"></div>';

    // -- Heatmap --
    const el = document.getElementById('transitions-chart');
    if (transitionsChart) { transitionsChart.dispose(); transitionsChart = null; }
    transitionsChart = echarts.init(el, 'dark');

    const shortLabels = events.map(shortName);

    transitionsChart.setOption({
        title: {
            text: `Transition Matrix (${Number(data.total).toLocaleString()} total)`,
            subtext: 'Rows = From state, Columns = To state. Click a row to see outgoing transitions.',
            left: 'center',
            textStyle: { color: '#ccc', fontSize: 14 },
            subtextStyle: { color: '#666', fontSize: 11 },
        },
        tooltip: {
            formatter: p => {
                if (!p.data || p.data[2] === 0) return '';
                const from = events[p.data[1]];
                const to = events[p.data[0]];
                const count = p.data[2];
                const pct = (count / data.total * 100).toFixed(1);
                const fromPct = fromTotals[from]
                    ? (count / fromTotals[from] * 100).toFixed(0) : '?';
                const l = lookup[from + '|' + to];
                const dur = l ? (l.duration_ms / count).toFixed(1) : '?';
                return `<b>${from}</b> → <b>${to}</b><br/>` +
                       `Count: ${count.toLocaleString()} (${pct}% of all)<br/>` +
                       `${fromPct}% of outgoing from ${shortName(from)}<br/>` +
                       `Avg dwell in source: ${dur} ms`;
            },
        },
        grid: { left: 200, right: 40, top: 80, bottom: 160 },
        xAxis: {
            type: 'category',
            data: shortLabels,
            position: 'bottom',
            axisLabel: { color: '#aaa', fontSize: 10, rotate: 45 },
            axisLine: { lineStyle: { color: '#333' } },
            splitArea: { show: true, areaStyle: { color: ['rgba(255,255,255,0.02)', 'rgba(0,0,0,0)'] } },
        },
        yAxis: {
            type: 'category',
            data: shortLabels,
            axisLabel: { color: '#aaa', fontSize: 10 },
            axisLine: { lineStyle: { color: '#333' } },
            splitArea: { show: true, areaStyle: { color: ['rgba(255,255,255,0.02)', 'rgba(0,0,0,0)'] } },
        },
        visualMap: {
            min: 0,
            max: maxCount || 1,
            calculable: true,
            orient: 'horizontal',
            left: 'center',
            bottom: 0,
            inRange: { color: ['#1a1a2e', '#16213e', '#0f3460', '#e94560', '#ff6b6b'] },
            textStyle: { color: '#888' },
        },
        series: [{
            type: 'heatmap',
            data: heatData.filter(d => d[2] > 0),
            label: {
                show: true,
                fontSize: 9,
                color: '#ddd',
                formatter: p => {
                    if (p.data[2] === 0) return '';
                    return p.data[2] >= 1000
                        ? (p.data[2] / 1000).toFixed(0) + 'K'
                        : p.data[2].toString();
                },
            },
            emphasis: {
                itemStyle: { shadowBlur: 10, shadowColor: 'rgba(0,0,0,0.5)' },
            },
        }],
    }, true);

    // Click on heatmap cell → show detail for that "from" state
    transitionsChart.on('click', p => {
        if (!p.data) return;
        const fromIdx = p.data[1];
        showFromDetail(events[fromIdx], events, lookup, fromTotals, data.total);
    });

    // -- Top transitions table --
    const sorted = data.links.slice().sort((a, b) => b.value - a.value).slice(0, 30);
    let html = '<h3 style="color:#ccc;margin:10px 0 5px">Top Transitions</h3>' +
        '<table class="data-table"><thead><tr>' +
        '<th>#</th><th>From</th><th>To</th><th>Count</th><th>% Total</th><th>Avg Dwell (ms)</th>' +
        '</tr></thead><tbody>';
    sorted.forEach((l, i) => {
        const pct = (l.value / data.total * 100).toFixed(1);
        const avg = (l.duration_ms / l.value).toFixed(1);
        const fromColor = classColor(l.source) || '#888';
        const toColor = classColor(l.target) || '#888';
        html += `<tr style="cursor:pointer" data-from="${esc(l.source)}">` +
            `<td>${i + 1}</td>` +
            `<td>${dot(l.source)} ${esc(l.source)}</td>` +
            `<td>${dot(l.target)} ${esc(l.target)}</td>` +
            `<td>${l.value.toLocaleString()}</td>` +
            `<td>${pct}%</td>` +
            `<td>${avg}</td></tr>`;
    });
    html += '</tbody></table>';
    document.getElementById('transitions-table').innerHTML = html;

    // Click table row → show detail for that "from" state
    document.querySelectorAll('#transitions-table tr[data-from]').forEach(row => {
        row.addEventListener('click', () => {
            showFromDetail(row.dataset.from, events, lookup, fromTotals, data.total);
        });
    });
}

function showFromDetail(fromEvent, events, lookup, fromTotals, total) {
    const detailEl = document.getElementById('transitions-detail');
    const totalOut = fromTotals[fromEvent] || 0;
    if (totalOut === 0) {
        detailEl.innerHTML = '';
        return;
    }

    // Gather all outgoing transitions for this source
    const outgoing = events
        .map(to => {
            const l = lookup[fromEvent + '|' + to];
            return l ? { to, count: l.value, duration_ms: l.duration_ms } : null;
        })
        .filter(Boolean)
        .sort((a, b) => b.count - a.count);

    let html = `<h3 style="color:#ccc;margin:10px 0 5px">` +
        `${dot(fromEvent)} After <b>${esc(fromEvent)}</b> ` +
        `<span style="color:#666">(${totalOut.toLocaleString()} outgoing)</span></h3>` +
        '<table class="data-table"><thead><tr>' +
        '<th>Next State</th><th>Count</th><th>% of Outgoing</th><th>Distribution</th><th>Avg Dwell (ms)</th>' +
        '</tr></thead><tbody>';
    outgoing.forEach(o => {
        const pct = (o.count / totalOut * 100);
        const avg = (o.duration_ms / o.count).toFixed(1);
        const barW = Math.min(pct, 100);
        const color = classColor(o.to) || '#4fc3f7';
        html += `<tr>` +
            `<td>${dot(o.to)} ${esc(o.to)}</td>` +
            `<td>${o.count.toLocaleString()}</td>` +
            `<td>${pct.toFixed(1)}%</td>` +
            `<td><div class="pct-bar"><div class="pct-fill" style="width:${barW.toFixed(1)}%;background:${color}"></div></div></td>` +
            `<td>${avg}</td></tr>`;
    });
    html += '</tbody></table>';
    detailEl.innerHTML = html;
    detailEl.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
}

// -- Start --------------------------------------------------------------------

connect();
