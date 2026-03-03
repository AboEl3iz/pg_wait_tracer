/* pgwt — Web Investigation Client */

// -- Wait class colors (Oracle ASH / RDS PI inspired) -------------------------

const WAIT_CLASSES = [
    { key: 'cpu',       label: 'CPU',       color: '#4CAF50' },
    { key: 'io',        label: 'IO',        color: '#2196F3' },
    { key: 'lock',      label: 'Lock',      color: '#F44336' },
    { key: 'lwlock',    label: 'LwLock',    color: '#FF9800' },
    { key: 'ipc',       label: 'IPC',       color: '#9C27B0' },
    { key: 'client',    label: 'Client',    color: '#00BCD4' },
    { key: 'timeout',   label: 'Timeout',   color: '#795548' },
    { key: 'bufferpin', label: 'BufferPin', color: '#FF5722' },
    { key: 'activity',  label: 'Activity',  color: '#9E9E9E' },
    { key: 'extension', label: 'Extension', color: '#607D8B' },
    { key: 'unknown',   label: 'Unknown',   color: '#BDBDBD' },
];

const CLASS_COLOR_MAP = {};
WAIT_CLASSES.forEach(c => {
    CLASS_COLOR_MAP[c.label.toLowerCase()] = c.color;
    CLASS_COLOR_MAP[c.key] = c.color;
});

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
        const ONE_HOUR_NS = 3600 * 1e9;
        state.viewFrom = Math.max(info.from_ns, info.to_ns - ONE_HOUR_NS);
        state.viewTo = info.to_ns;
        setStatus(info.num_cpus + ' CPUs, ~' + fmtCount(info.num_events) + ' events', 'connected');
        updateTimeRange();
        initChart();
        initTabs();
        initTimePicker();
        await refresh();
    } catch (e) {
        setStatus('Error: ' + e.message, 'error');
    }
}

// -- Refresh ------------------------------------------------------------------

async function refresh() {
    await Promise.all([refreshChart(), refreshTable()]);
}

async function refreshChart() {
    try {
        const data = await send('aas', {
            from: state.viewFrom,
            to: state.viewTo,
            buckets: Math.min(Math.floor(chartEl.clientWidth / 4), 300),
            filters: state.filters,
        });
        renderChart(data);
    } catch (e) { /* ignore on disconnect */ }
}

async function refreshTable() {
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

function fmtTime(ns) {
    if (!ns) return '--';
    const d = new Date(ns / 1e6);
    return d.toLocaleTimeString();
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

// -- Tabs ---------------------------------------------------------------------

function initTabs() {
    document.querySelectorAll('.tab').forEach(btn => {
        btn.addEventListener('click', () => {
            switchTab(btn.dataset.tab);
        });
    });
}

function switchTab(tab) {
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
    chart = echarts.init(chartEl, 'dark');
    window.addEventListener('resize', () => chart.resize());

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

        // Minimum drag of 5px to avoid accidental clicks
        if (maxX - minX < 5) return;

        // Convert pixel to data index
        const opt = chart.getOption();
        if (!opt.xAxis || !opt.xAxis[0] || !opt.xAxis[0].data) return;
        const data = opt.xAxis[0].data;
        const startIdx = chart.convertFromPixel({ xAxisIndex: 0 }, minX);
        const endIdx = chart.convertFromPixel({ xAxisIndex: 0 }, maxX);
        if (startIdx >= 0 && endIdx < data.length && startIdx < endIdx) {
            zoomTo(data[startIdx], data[endIdx]);
        }
    });

    // Double-click to zoom out
    chartEl.addEventListener('dblclick', (e) => {
        e.preventDefault();
        zoomOut();
    });
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

    // Quick range buttons
    picker.querySelectorAll('.tp-quick button').forEach(btn => {
        btn.addEventListener('click', () => {
            const secs = parseInt(btn.dataset.range);
            picker.style.display = 'none';
            // Update active class
            picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
            btn.classList.add('active');
            if (secs === 0) {
                zoomTo(state.fromNs, state.toNs);
            } else {
                const from = Math.max(state.fromNs, state.toNs - secs * 1e9);
                zoomTo(from, state.toNs);
            }
        });
    });

    // Custom range apply
    document.getElementById('tp-apply').addEventListener('click', () => {
        const fromStr = document.getElementById('tp-from').value;
        const toStr = document.getElementById('tp-to').value;
        if (!fromStr || !toStr) return;
        const fromNs = new Date(fromStr).getTime() * 1e6;
        const toNs = new Date(toStr).getTime() * 1e6;
        if (fromNs >= toNs) return;
        picker.style.display = 'none';
        picker.querySelectorAll('.tp-quick button').forEach(b => b.classList.remove('active'));
        zoomTo(Math.max(state.fromNs, fromNs), Math.min(state.toNs, toNs));
    });

    // Zoom out button
    zoomOutBtn.addEventListener('click', () => zoomOut());
}

function nsToDatetimeLocal(ns) {
    const d = new Date(ns / 1e6);
    // Format as YYYY-MM-DDTHH:MM:SS for datetime-local input
    const pad = (n) => String(n).padStart(2, '0');
    return d.getFullYear() + '-' + pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
        'T' + pad(d.getHours()) + ':' + pad(d.getMinutes()) + ':' + pad(d.getSeconds());
}

function renderChart(data) {
    if (!chart || !data.buckets || data.buckets.length === 0) return;

    const times = data.buckets.map(b => b.t);

    const series = WAIT_CLASSES.map(wc => ({
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
                let t = fmtTime(params[0].axisValue);
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
            data: WAIT_CLASSES.map(c => c.label),
            bottom: 0,
            textStyle: { color: '#888', fontSize: 11 },
            itemWidth: 12,
            itemHeight: 8,
        },
        grid: {
            left: 50, right: 20, top: 20, bottom: 40,
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
                formatter: function(v) { return fmtTime(v); },
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
            { key: 'db_time_ms', label: 'DB Time', cls: 'num', format: (r) => fmtMs(r.db_time_ms) },
            { key: 'cpu_pct', label: 'CPU%', cls: 'num', format: (r) => pctBar(r.cpu_pct, '#4CAF50') },
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
            { key: 'query_id', label: 'Query', format: (r) => {
                const id = '<span class="query-id">' + r.query_id + '</span>';
                if (r.text) return dot(r.top_wait) + esc(r.text.substring(0, 80)) +
                    (r.text.length > 80 ? '...' : '');
                return dot(r.top_wait) + id;
            }},
            { key: 'total_ms', label: 'Time', cls: 'num', format: (r) => fmtMs(r.total_ms) },
            { key: 'pct', label: '%DB', cls: 'num', format: (r) => fmtPct(r.pct) },
            { key: 'classes', label: 'Wait Profile', format: (r) =>
                stackedBar(r.classes, r.total_ms) },
            { key: 'count', label: 'Calls', cls: 'num', format: (r) => fmtCount(r.count) },
            { key: 'avg_us', label: 'Avg', cls: 'num', format: (r) => fmtUs(r.avg_us) },
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
}

// -- Drill-down ---------------------------------------------------------------

function drillDown(filterKey, filterValue, label) {
    state.breadcrumbs.push({
        label: label,
        filters: { ...state.filters },
        tab: state.activeTab,
    });
    state.filters[filterKey] = filterValue;

    // Auto-pivot
    const pivotMap = { class: 'events', event_id: 'sessions', pid: 'queries' };
    if (pivotMap[filterKey]) {
        switchTab(pivotMap[filterKey]);
    }

    updateBreadcrumb();
    refresh();
}

function drillUp(index) {
    const crumb = state.breadcrumbs[index];
    state.filters = { ...crumb.filters };
    state.breadcrumbs = state.breadcrumbs.slice(0, index);
    switchTab(crumb.tab);
    updateBreadcrumb();
    refresh();
}

function clearFilters() {
    state.filters = {};
    state.breadcrumbs = [];
    updateBreadcrumb();
    switchTab('overview');
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

// -- Start --------------------------------------------------------------------

connect();
