/* pgwt — pure builder: AAS data -> ECharts option.
 *
 * The Average Active Sessions chart, migrated from ApexCharts to ECharts. This
 * builder is library-shaped only at the edge (it emits an ECharts option); the
 * data->series mapping is pure and Node-testable. It owns NO chart instance and
 * touches NO DOM. The overview view's mount() feeds the option to its ECharts
 * instance; the selection overlay (lib/selection.js) handles drag-zoom.
 *
 * Stacked area, one series per wait class (or per event in breakdown mode),
 * x = bucket timestamp (ns, numeric), y = AAS. A markLine at numCpus mirrors
 * the old ApexCharts "N CPUs" annotation.
 */

import { WAIT_CLASSES, EVENT_PALETTE, fmtTime } from '../format.js';

/* data: aas response { buckets[], bucket_ns, max_aas, breakdown?, series? }
 * opts: { numCpus }
 * Returns { option, seriesNames, seriesColors } — names/colors drive the
 * external HTML legend (kept out of the chart for full control, as before). */
export function buildAasOption(data, opts) {
    opts = opts || {};
    const numCpus = opts.numCpus || 0;
    const bns = (data && data.bucket_ns) || 0;
    const buckets = (data && data.buckets) || [];

    const isEventBreakdown = data && data.breakdown === 'events' && data.series;

    let seriesDefs, seriesColors;
    if (isEventBreakdown) {
        seriesDefs = data.series.map((s, idx) => ({
            name: s.name,
            data: buckets.map(b => [b.t, +(b.aas[idx] || 0).toFixed(4)]),
        }));
        seriesColors = data.series.map((_, idx) => EVENT_PALETTE[idx % EVENT_PALETTE.length]);
    } else {
        seriesDefs = WAIT_CLASSES.map(wc => ({
            name: wc.label,
            data: buckets.map(b => [b.t, +(b[wc.key] || 0).toFixed(4)]),
        }));
        seriesColors = WAIT_CLASSES.map(c => c.color);
    }

    const maxAas = (data && data.max_aas) || 0;
    const yMax = Math.max(
        maxAas * 1.2,
        numCpus > 0 ? numCpus * 1.5 : 0,
        1
    );

    const xMin = buckets.length ? buckets[0].t : 0;
    const xMax = buckets.length ? buckets[buckets.length - 1].t : 1;

    const series = seriesDefs.map((s, i) => ({
        name: s.name,
        type: 'line',
        stack: 'aas',
        areaStyle: { opacity: 0.85 },
        lineStyle: { width: 1 },
        symbol: 'none',
        emphasis: { disabled: true },
        color: seriesColors[i],
        data: s.data,
    }));

    // CPU reference line as a markLine on the first series.
    if (numCpus > 0 && series.length) {
        series[0].markLine = {
            silent: true,
            symbol: 'none',
            lineStyle: { color: '#E53935', type: 'dashed', width: 1 },
            label: {
                formatter: numCpus + ' CPUs',
                position: 'insideEndTop',
                color: '#fff',
                backgroundColor: '#E53935',
                padding: [2, 5, 2, 5],
                fontSize: 11,
            },
            data: [{ yAxis: numCpus }],
        };
    }

    const option = {
        backgroundColor: 'transparent',
        animation: false,
        // Hidden legend component so the external HTML legend can drive series
        // visibility via legendSelect/legendUnSelect dispatchAction.
        legend: { show: false, data: seriesDefs.map(s => s.name) },
        grid: { left: 55, right: 20, top: 14, bottom: 28 },
        xAxis: {
            type: 'value',
            min: xMin,
            max: xMax,
            axisLabel: {
                color: '#888', fontSize: 10,
                formatter: (v) => fmtTime(v, bns),
                hideOverlap: true,
            },
            axisLine: { lineStyle: { color: '#333' } },
            axisTick: { lineStyle: { color: '#333' } },
            splitLine: { show: false },
        },
        yAxis: {
            type: 'value',
            min: 0,
            max: yMax,
            name: 'Active Sessions',
            nameTextStyle: { color: '#888', fontSize: 11 },
            axisLabel: { color: '#888', fontSize: 10, formatter: (v) => v.toFixed(1) },
            splitLine: { lineStyle: { color: '#2a2a4a' } },
        },
        tooltip: {
            trigger: 'axis',
            backgroundColor: '#1e1e3a',
            borderColor: '#333',
            textStyle: { color: '#e0e0e0', fontSize: 12 },
            axisPointer: { type: 'line', lineStyle: { color: '#666', width: 1, type: 'dashed' } },
            formatter: (params) => aasTooltip(params, bns),
        },
        series,
    };

    return {
        option,
        seriesNames: seriesDefs.map(s => s.name),
        seriesColors,
        maxAas,
        hasData: buckets.length > 0,
    };
}

/* Pure tooltip renderer (exported for testing). `params` is the ECharts axis
 * tooltip param array. */
export function aasTooltip(params, bns) {
    if (!params || !params.length) return '';
    const t = fmtTime(params[0].value[0], bns);
    let total = 0;
    const items = [];
    for (const p of params) {
        const val = (p.value && p.value[1]) || 0;
        if (val > 0.001) {
            items.push({ name: p.seriesName, value: val, color: p.color });
            total += val;
        }
    }
    items.reverse();  // top-of-stack first, matching the old ApexCharts order
    let html = '<div style="padding:4px"><b>' + t + '</b><br>Total AAS: <b>' +
               total.toFixed(2) + '</b><br>';
    for (const it of items) {
        const pct = total > 0 ? (it.value / total * 100).toFixed(0) : '0';
        html += '<span style="color:' + it.color + '">●</span> ' + it.name +
                ': <b>' + it.value.toFixed(2) + '</b> (' + pct + '%)<br>';
    }
    html += '</div>';
    return html;
}
