/* Node unit tests for the pure AAS builder (lib/builders/aas.js).
 *
 * Runs under `node --test` — no framework, no browser, no network. Proves the
 * data -> ECharts option mapping (shape + filter/breakdown correctness) so an
 * off-by-one or a dropped series is caught in milliseconds without Playwright.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildAasOption, aasTooltip } from '../../web/static/lib/builders/aas.js';
import { WAIT_CLASSES, EVENT_PALETTE } from '../../web/static/lib/format.js';

function classBuckets(n) {
    const out = [];
    for (let i = 0; i < n; i++) {
        out.push({ t: 1000 + i, cpu: 1.0, io: 0.5, lock: 0.1, lwlock: 0.2,
            ipc: 0, client: 0, timeout: 0, bufferpin: 0, activity: 0,
            extension: 0, unknown: 0 });
    }
    return out;
}

test('class mode: one series per wait class, x=ts y=aas', () => {
    const data = { bucket_ns: 1, max_aas: 2.0, buckets: classBuckets(3) };
    const { option, seriesNames, seriesColors } = buildAasOption(data, { numCpus: 4 });

    assert.equal(option.series.length, WAIT_CLASSES.length);
    assert.deepEqual(seriesNames, WAIT_CLASSES.map(c => c.label));
    assert.deepEqual(seriesColors, WAIT_CLASSES.map(c => c.color));

    const cpu = option.series.find(s => s.name === 'CPU');
    assert.deepEqual(cpu.data, [[1000, 1], [1001, 1], [1002, 1]]);
    // Stacked area
    assert.equal(cpu.stack, 'aas');
    assert.ok(cpu.areaStyle);
});

test('y max accounts for cpu count and max_aas', () => {
    // numCpus*1.5 dominates when max_aas is small
    let opt = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 8 }).option;
    assert.equal(opt.yAxis.max, 12);  // 8 * 1.5
    // max_aas*1.2 dominates when AAS is high
    opt = buildAasOption({ buckets: classBuckets(1), max_aas: 100, bucket_ns: 1 },
        { numCpus: 4 }).option;
    assert.equal(opt.yAxis.max, 120);  // 100 * 1.2
});

test('cpu markLine present iff numCpus > 0', () => {
    const withCpu = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 4 }).option;
    assert.ok(withCpu.series[0].markLine);
    assert.equal(withCpu.series[0].markLine.data[0].yAxis, 4);

    const noCpu = buildAasOption({ buckets: classBuckets(1), max_aas: 1, bucket_ns: 1 },
        { numCpus: 0 }).option;
    assert.ok(!noCpu.series[0].markLine);
});

test('event breakdown mode: one series per event with palette colors', () => {
    const data = {
        bucket_ns: 1, max_aas: 1.0, breakdown: 'events',
        series: [{ name: 'IO:DataFileRead' }, { name: 'IO:WalSync' }],
        buckets: [
            { t: 10, aas: [0.4, 0.2] },
            { t: 11, aas: [0.5, 0.25] },
        ],
    };
    const { option, seriesNames, seriesColors } = buildAasOption(data, { numCpus: 2 });
    assert.deepEqual(seriesNames, ['IO:DataFileRead', 'IO:WalSync']);
    assert.deepEqual(seriesColors, [EVENT_PALETTE[0], EVENT_PALETTE[1]]);
    assert.deepEqual(option.series[0].data, [[10, 0.4], [11, 0.5]]);
    assert.deepEqual(option.series[1].data, [[10, 0.2], [11, 0.25]]);
});

test('empty data: no crash, hasData false', () => {
    const m = buildAasOption({ buckets: [], max_aas: 0, bucket_ns: 0 }, { numCpus: 4 });
    assert.equal(m.hasData, false);
    assert.equal(m.option.xAxis.min, 0);
    assert.equal(m.option.xAxis.max, 1);
});

test('x axis spans first..last bucket timestamp', () => {
    const opt = buildAasOption({ buckets: classBuckets(5), max_aas: 1, bucket_ns: 1 },
        { numCpus: 1 }).option;
    assert.equal(opt.xAxis.min, 1000);
    assert.equal(opt.xAxis.max, 1004);
});

test('aas values rounded to 4 decimals', () => {
    const data = { bucket_ns: 1, max_aas: 1, buckets: [
        { t: 1, cpu: 0.123456789 }] };
    const cpu = buildAasOption(data, { numCpus: 1 }).option.series.find(s => s.name === 'CPU');
    assert.equal(cpu.data[0][1], 0.1235);
});

test('tooltip totals visible series and orders top-of-stack first', () => {
    const params = [
        { seriesName: 'CPU', value: [1000, 1.0], color: '#1' },
        { seriesName: 'IO', value: [1000, 0.5], color: '#2' },
        { seriesName: 'Idle', value: [1000, 0], color: '#3' },  // dropped (<=0.001)
    ];
    const html = aasTooltip(params, 1000000000);
    assert.ok(html.includes('Total AAS: <b>1.50</b>'));
    // IO is later in params (top of stack) -> appears first after reverse
    assert.ok(html.indexOf('IO') < html.indexOf('CPU'));
    assert.ok(!html.includes('Idle'));
});
