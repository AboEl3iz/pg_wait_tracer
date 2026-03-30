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
        setStatus(info.num_cpus + ' CPUs', 'connected');
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
            buckets: Math.min(Math.floor(apexEl.clientWidth / 4), 300),
            filters: state.filters,
        };
        // Request per-event breakdown when drilled into a class (but not a specific event)
        if (state.filters.class && !state.filters.event_id) {
            params.detail = 'events';
        }
        const data = await send('aas', params);
        renderApexChart(data);
        // Update status with current window info
        if (data && data.buckets) {
            const dur = fmtDuration(state.viewTo - state.viewFrom);
            const maxAas = data.max_aas ? data.max_aas.toFixed(1) : '0';
            setStatus(state.numCpus + ' CPUs · ' + dur + ' window · peak ' + maxAas + ' AAS', 'connected');
        }
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
        transitionsChart = null;  // DFG is raw SVG, no dispose needed
    }
    if (state.activeTab === 'concurrency' && tab !== 'concurrency') {
        if (concurrencyChart) { concurrencyChart.dispose(); concurrencyChart = null; }
    }

    // Stop auto-refresh when switching to analysis tabs
    if (tab === 'concurrency' || tab === 'transitions') {
        stopAutoRefresh();
    }

    state.activeTab = tab;
    document.querySelectorAll('.tab').forEach(b => {
        b.classList.toggle('active', b.dataset.tab === tab);
    });
    refreshChart();
    refreshTable();
}

// -- Chart container ----------------------------------------------------------

const chartEl = document.getElementById('chart-container');  // hidden, kept for legacy refs

function initChart() {
    /* AAS chart is now ApexCharts (rendered in apex-chart-container).
     * ECharts is still used for heatmap, timeline, concurrency, transitions. */
    window.addEventListener('resize', () => {
        if (concurrencyChart) concurrencyChart.resize();
    });
}

// -- Chart resize handle --------------------------------------------------

function initChartResize() {
    const handle = document.getElementById('chart-resize');
    let startY = 0, startH = 0;

    handle.addEventListener('mousedown', (e) => {
        e.preventDefault();
        startY = e.clientY;
        startH = apexEl.offsetHeight;
        document.addEventListener('mousemove', onDrag);
        document.addEventListener('mouseup', onUp);
        document.body.style.cursor = 'ns-resize';
        document.body.style.userSelect = 'none';
    });

    function onDrag(e) {
        const h = Math.max(120, startH + e.clientY - startY);
        apexEl.style.height = h + 'px';
        if (apexChart) apexChart.updateOptions({ chart: { height: h } }, false, false, false);
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

// -- ApexCharts AAS (main chart) ----------------------------------------------

let apexChart = null;
const apexEl = document.getElementById('apex-chart-container');

function renderApexChart(data) {
    if (!data || !data.buckets || data.buckets.length === 0) return;

    const isEventBreakdown = data.breakdown === 'events' && data.series;

    // Build series with raw values (categories, not datetime — preserves ns precision)
    let seriesList, colorList;
    const bns = data.bucket_ns || 0;
    const categories = data.buckets.map(b => b.t);  // raw ns timestamps

    if (isEventBreakdown) {
        seriesList = data.series.map((s, idx) => ({
            name: s.name,
            data: data.buckets.map(b => +(b.aas[idx] || 0).toFixed(4)),
        }));
        colorList = data.series.map((_, idx) => EVENT_PALETTE[idx % EVENT_PALETTE.length]);
    } else {
        seriesList = WAIT_CLASSES.map(wc => ({
            name: wc.label,
            data: data.buckets.map(b => +(b[wc.key] || 0).toFixed(4)),
        }));
        colorList = WAIT_CLASSES.map(c => c.color);
    }

    const yMax = Math.max(
        data.max_aas * 1.2,
        state.numCpus > 0 ? state.numCpus * 1.5 : 0,
        1
    );

    const options = {
        chart: {
            type: 'area',
            height: 300,
            stacked: true,
            animations: { enabled: false },
            toolbar: { show: false },
            zoom: {
                enabled: true,
                type: 'x',
                autoScaleYaxis: false,
            },
            events: {
                zoomed: function(ctx, { xaxis }) {
                    if (xaxis.min != null && xaxis.max != null) {
                        stopAutoRefresh();
                        // xaxis.min/max are category indices
                        const fromIdx = Math.max(0, Math.floor(xaxis.min));
                        const toIdx = Math.min(categories.length - 1, Math.ceil(xaxis.max));
                        state.viewFrom = categories[fromIdx];
                        state.viewTo = categories[toIdx];
                        updateTimeRange();
                        refresh();
                    }
                },
            },
            background: 'transparent',
        },
        theme: { mode: 'dark' },
        colors: colorList,
        series: seriesList,
        xaxis: {
            type: 'category',
            categories: categories,
            labels: {
                style: { colors: '#888', fontSize: '10px' },
                formatter: function(val) { return fmtTime(val, bns); },
                rotate: 0,
                hideOverlappingLabels: true,
            },
            tickAmount: 10,
            axisBorder: { color: '#333' },
            axisTicks: { color: '#333' },
            crosshairs: {
                show: true,
                stroke: { color: '#666', width: 1, dashArray: 3 },
            },
        },
        yaxis: {
            min: 0,
            max: yMax,
            title: {
                text: 'Active Sessions',
                style: { color: '#888', fontSize: '11px' },
            },
            labels: {
                formatter: v => v.toFixed(1),
                style: { colors: '#888', fontSize: '10px' },
            },
        },
        annotations: {
            yaxis: state.numCpus > 0 ? [{
                y: state.numCpus,
                borderColor: '#E53935',
                strokeDashArray: 4,
                label: {
                    text: state.numCpus + ' CPUs',
                    position: 'right',
                    textAnchor: 'end',
                    borderColor: 'transparent',
                    style: {
                        background: '#E53935',
                        color: '#fff',
                        fontSize: '11px',
                        padding: { left: 5, right: 5, top: 2, bottom: 2 },
                    },
                },
            }] : [],
        },
        tooltip: {
            shared: true,
            theme: 'dark',
            custom: function({ series, dataPointIndex, w }) {
                const t = fmtTime(categories[dataPointIndex], bns);
                let total = 0;
                let items = [];
                for (let i = series.length - 1; i >= 0; i--) {
                    const val = series[i][dataPointIndex] || 0;
                    if (val > 0.001) {
                        items.push({
                            name: w.globals.seriesNames[i],
                            value: val,
                            color: w.globals.colors[i],
                        });
                        total += val;
                    }
                }
                let html = '<div style="padding:8px;background:#1e1e3a;border:1px solid #333;border-radius:4px">' +
                    '<b>' + t + '</b><br>Total AAS: <b>' + total.toFixed(2) + '</b><br>';
                items.forEach(it => {
                    const pct = total > 0 ? (it.value / total * 100).toFixed(0) : '0';
                    html += '<span style="color:' + it.color + '">\u25cf</span> ' +
                        it.name + ': <b>' + it.value.toFixed(2) + '</b> (' + pct + '%)<br>';
                });
                html += '</div>';
                return html;
            },
        },
        stroke: {
            curve: 'straight',
            width: 1,
        },
        fill: {
            type: 'solid',
            opacity: 0.85,
        },
        legend: { show: false },
        grid: {
            borderColor: '#2a2a4a',
            padding: { left: 10, right: 10 },
        },
        dataLabels: { enabled: false },
    };

    if (apexChart) {
        apexChart.updateOptions(options, true, false, false);
    } else {
        apexChart = new ApexCharts(apexEl, options);
        apexChart.render();
    }

    // -- HTML legend (completely isolated from ECharts) --
    // Uses ONLY apexChart.showSeries/hideSeries/updateOptions — zero DOM queries
    const seriesNames = seriesList.map(s => s.name);
    const seriesColors = colorList;
    if (!state.apexSelected) state.apexSelected = new Set(seriesNames.map((_, i) => i));

    let legDiv = document.getElementById('apex-custom-legend');
    if (!legDiv) {
        legDiv = document.createElement('div');
        legDiv.id = 'apex-custom-legend';
        legDiv.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px;justify-content:center;padding:8px 20px;background:#1e1e3a;';
        // Insert after apex-chart-container, NOT inside it
        apexEl.parentNode.insertBefore(legDiv, apexEl.nextSibling);
    }

    legDiv.innerHTML = seriesNames.map((name, i) =>
        `<span class="aleg" data-i="${i}" style="display:inline-flex;align-items:center;gap:4px;` +
        `cursor:pointer;padding:3px 10px;border-radius:4px;font-size:11px;color:#ccc;` +
        `border:1px solid ${seriesColors[i]};border-left:3px solid ${seriesColors[i]};` +
        `user-select:none;transition:opacity 0.1s">` +
        `<span style="width:8px;height:8px;border-radius:2px;background:${seriesColors[i]}"></span>` +
        `${esc(name)}</span>`
    ).join('');

    function apexApplySelection() {
        seriesNames.forEach((n, i) => {
            if (state.apexSelected.has(i)) apexChart.showSeries(n);
            else apexChart.hideSeries(n);
        });
        legDiv.querySelectorAll('.aleg').forEach(el => {
            el.style.opacity = state.apexSelected.has(+el.dataset.i) ? '1' : '0.3';
        });
    }

    let hoverActive = false;

    function apexShowOnly(idx) {
        // Hide/show via API — properly removes from stack
        seriesNames.forEach((n, i) => {
            if (i === idx) apexChart.showSeries(n);
            else apexChart.hideSeries(n);
        });
    }

    function apexRestoreSelection() {
        seriesNames.forEach((n, i) => {
            if (state.apexSelected.has(i)) apexChart.showSeries(n);
            else apexChart.hideSeries(n);
        });
    }

    legDiv.querySelectorAll('.aleg').forEach(el => {
        const idx = +el.dataset.i;

        el.addEventListener('mouseenter', () => {
            hoverActive = true;
            apexShowOnly(idx);
        });
        el.addEventListener('mouseleave', () => {
            hoverActive = false;
            apexRestoreSelection();
        });

        el.addEventListener('click', (e) => {
            if (e.metaKey || e.ctrlKey) {
                // Cmd+Click: multiselect toggle
                if (state.apexSelected.has(idx)) {
                    if (state.apexSelected.size > 1) state.apexSelected.delete(idx);
                } else {
                    state.apexSelected.add(idx);
                }
            } else {
                // Click: solo (or restore all if already solo)
                if (state.apexSelected.size === 1 && state.apexSelected.has(idx)) {
                    state.apexSelected = new Set(seriesNames.map((_, i) => i));
                } else {
                    state.apexSelected = new Set([idx]);
                }
            }
            apexApplySelection();
        });
    });

    apexApplySelection();
}

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
        label: state.currentFilterLabel || '',
        filters: { ...state.filters },
        tab: state.activeTab,
    });
    state.filters[filterKey] = filterValue;
    state.currentFilterLabel = label;

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
    state.currentFilterLabel = crumb.label;
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
    state.currentFilterLabel = null;
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

    // Current filter — show label if available, otherwise raw keys
    if (Object.keys(state.filters).length > 0) {
        if (state.breadcrumbs.length > 0) html += '<span class="crumb-sep">\u203a</span>';
        const label = state.currentFilterLabel || Object.entries(state.filters).map(([k,v]) => k + '=' + v).join(', ');
        html += '<span style="color:#4fc3f7">' + dot(label) + esc(label) + '</span>';
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

// -- Transitions DFG (Directly-Follows Graph) ---------------------------------

let transitionsChart = null;

async function refreshTransitions() {
    const container = document.getElementById('table-container');

    let data;
    try {
        data = await send('transitions', {
            from: state.viewFrom,
            to: state.viewTo,
            filters: state.filters,
            num_buckets: 200,
        });
    } catch (e) {
        container.innerHTML = '<p style="color:#888;padding:20px">Transitions error: ' + e.message + '</p>';
        return;
    }

    if (!data || !data.links || data.links.length === 0) {
        container.innerHTML = '<p style="color:#888;padding:20px">No transitions found</p>';
        return;
    }

    const maxEdgeCount = Math.max(...data.links.map(l => l.value));
    const nodeMap = {};
    (data.nodes || []).forEach(n => { nodeMap[n.name] = n; });

    // Build HTML layout
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

        // Build ECharts graph data — grid layout filling the container
        const sortedNodes = [...visibleNodes]
            .map(name => ({ name, ms: (nodeMap[name]?.total_ms || 0) }))
            .sort((a, b) => b.ms - a.ms);

        const n = sortedNodes.length;
        const cols = Math.ceil(Math.sqrt(n * 1.8));  // wider than tall
        const rows = Math.ceil(n / cols);
        const el = document.getElementById('dfg-container');
        const cW = el.clientWidth || 800;
        const cH = el.clientHeight || 550;
        const padX = 80, padY = 60;
        const cellW = (cW - padX * 2) / Math.max(cols - 1, 1);
        const cellH = (cH - padY * 2) / Math.max(rows - 1, 1);

        const ecNodes = sortedNodes.map((nd, i) => {
            const info = nodeMap[nd.name] || {};
            const ms = nd.ms;
            const cls = (info.class || 'unknown').toLowerCase();
            const color = classColor(nd.name) || classColor(cls) || '#888';
            const size = Math.max(15, Math.min(120, 15 + Math.sqrt(ms / maxNodeMs) * 105));
            const timeStr = ms >= 1000 ? (ms/1000).toFixed(1) + 's' : ms.toFixed(0) + 'ms';
            const col = i % cols;
            const row = Math.floor(i / cols);
            return {
                name: nd.name,
                x: padX + col * cellW,
                y: padY + row * cellH,
                symbolSize: size,
                itemStyle: { color: color, borderColor: color, borderWidth: 2 },
                label: {
                    show: true,
                    position: 'bottom',
                    fontSize: 10,
                    color: '#ccc',
                    formatter: (nd.name.indexOf(':') > 0 ? nd.name.substring(nd.name.indexOf(':') + 1) : nd.name) +
                        '\n' + timeStr,
                    lineHeight: 14,
                },
                tooltip: {
                    formatter: `<b>${nd.name}</b><br/>Total time: ${timeStr}`,
                },
                value: ms,
            };
        });

        const ecLinks = visibleLinks.map(l => {
            const w = Math.max(1, Math.min(10, (l.value / maxLinkVal) * 10));
            const pct = (l.value / data.total * 100).toFixed(1);
            const avg = l.value > 0 ? (l.duration_ms / l.value).toFixed(1) : '?';
            return {
                source: l.source,
                target: l.target,
                lineStyle: {
                    width: w,
                    color: '#555',
                    curveness: l.source === l.target ? 0.8 : 0.3,
                    opacity: 0.4 + (l.value / maxLinkVal) * 0.5,
                },
                tooltip: {
                    formatter: `<b>${l.source}</b> → <b>${l.target}</b><br/>` +
                        `Count: ${l.value.toLocaleString()} (${pct}%)<br/>` +
                        `Avg dwell: ${avg} ms`,
                },
                value: l.value,
            };
        });

        if (transitionsChart) transitionsChart.dispose();
        transitionsChart = echarts.init(el, 'dark');

        transitionsChart.setOption({
            backgroundColor: 'transparent',
            tooltip: {
                backgroundColor: '#1e1e3a',
                borderColor: '#333',
                textStyle: { color: '#e0e0e0', fontSize: 12 },
            },
            series: [{
                type: 'graph',
                layout: 'none',
                roam: true,
                draggable: true,
                data: ecNodes,
                links: ecLinks,
                edgeSymbol: ['none', 'arrow'],
                edgeSymbolSize: [0, 8],
                emphasis: {
                    focus: 'adjacency',
                    lineStyle: { width: 4, opacity: 1 },
                },
                lineStyle: { curveness: 0.3 },
            }],
        }, true);
    }

    renderDFG(20);

    const slider = document.getElementById('dfg-slider');
    const sliderVal = document.getElementById('dfg-slider-val');
    slider.addEventListener('input', () => {
        const v = +slider.value;
        sliderVal.textContent = v + '%';
        renderDFG(v);
    });

    // -- Fetch and render variants below the DFG --
    try {
        const vdata = await send('variants', {
            from: state.viewFrom,
            to: state.viewTo,
            filters: state.filters,
            num_buckets: 20,
        });
        if (vdata && vdata.variants && vdata.variants.length > 0) {
            renderVariants(vdata, container);
        }
    } catch (e) { /* ignore */ }
}

function renderVariants(vdata, container) {
    const totalMs = vdata.variants.reduce((s, v) => s + v.total_ms, 0);

    let html = '<div style="padding:10px 20px">' +
        `<h3 style="color:#ccc;margin:10px 0 5px">Execution Flow Patterns ` +
        `<span style="color:#666;font-size:12px">(${vdata.total_executions.toLocaleString()} executions → ` +
        `${vdata.num_variants} patterns)</span></h3>`;

    vdata.variants.forEach((v, idx) => {
        const pctTime = totalMs > 0 ? (v.total_ms / totalMs * 100) : 0;
        const avgStr = v.avg_ms >= 1000 ? (v.avg_ms / 1000).toFixed(1) + 's' :
                       v.avg_ms >= 1 ? v.avg_ms.toFixed(1) + 'ms' :
                       (v.avg_ms * 1000).toFixed(0) + 'μs';
        const p95Str = v.p95_ms >= 1000 ? (v.p95_ms / 1000).toFixed(1) + 's' :
                       v.p95_ms >= 1 ? v.p95_ms.toFixed(1) + 'ms' :
                       (v.p95_ms * 1000).toFixed(0) + 'μs';
        const totalStr = v.total_ms >= 1000 ? (v.total_ms / 1000).toFixed(1) + 's' :
                         v.total_ms.toFixed(0) + 'ms';

        // Flow bar: colored blocks proportional to step avg time
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

        // Step detail text
        let stepsText = v.steps.map(st => {
            const label = st.name.indexOf(':') > 0 ? st.name.substring(st.name.indexOf(':') + 1) : st.name;
            const dur = st.avg_ms >= 1 ? st.avg_ms.toFixed(1) + 'ms' :
                        (st.avg_ms * 1000).toFixed(0) + 'μs';
            if (st.loop) return `[${label}(${dur})…]×N`;
            return `${label}(${dur})`;
        }).join(' → ');

        // Query ID + text preview
        let queryHtml = '';
        if (v.top_query_id) {
            const qid = v.top_query_id;
            const preview = v.query_text
                ? (v.query_text.length > 100 ? v.query_text.substring(0, 100) + '...' : v.query_text)
                : '';
            queryHtml = `<div style="color:#666;font-size:10px;font-family:monospace;margin-top:2px;` +
                `white-space:nowrap;overflow:hidden;text-overflow:ellipsis" ` +
                `title="${esc(v.query_text || '')}">` +
                `<span style="color:#888">${qid}</span>` +
                `${preview ? ' ' + esc(preview) : ''}</div>`;
        }

        html += `<div style="background:#1e1e3a;border:1px solid #2a2a4a;border-radius:6px;padding:10px;margin:6px 0">` +
            `<div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:4px">` +
                `<span style="color:#ccc;font-size:13px;font-weight:500">#${idx + 1}</span>` +
                `<span style="color:#888;font-size:11px">` +
                    `<b style="color:#4fc3f7">${pctTime.toFixed(1)}%</b> of time` +
                    ` · ${v.exec_count.toLocaleString()} exec` +
                    ` · ${v.num_queries} queries` +
                    ` · avg ${avgStr}` +
                    ` · p95 ${p95Str}` +
                    ` · total ${totalStr}` +
                    (v.avg_loop_n > 1.5 ? ` · ~${v.avg_loop_n.toFixed(0)}× loop` : '') +
                `</span>` +
            `</div>` +
            flowHtml +
            `<div style="color:#888;font-size:10px;margin-top:2px">${stepsText}</div>` +
            queryHtml +
        `</div>`;
    });

    html += '</div>';

    // Append to container (after DFG)
    const variantDiv = document.createElement('div');
    variantDiv.innerHTML = html;
    container.appendChild(variantDiv);
}

// -- Start --------------------------------------------------------------------

connect();
