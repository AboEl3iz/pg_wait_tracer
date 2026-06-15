/* Node unit tests for the pure concurrency builders (lib/builders/concurrency.js).
 *
 * Runs under `node --test`. Proves the concurrency data -> ECharts line option +
 * burst markers, and the top-peaks / burst HTML tables: peak series mapping,
 * burst marker placement (first bucket >= timestamp), top-10-by-max ordering,
 * and empty-input handling.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    buildConcurrencyOption, buildConcurrencyTables,
} from '../../web/static/lib/builders/concurrency.js';

function data() {
    return {
        bucket_ns: 60_000_000_000,
        peaks: [
            { t: 1000, t_ms: 1, max: 2, event: 'A' },
            { t: 2000, t_ms: 2, max: 8, event: 'B' },
            { t: 3000, t_ms: 3, max: 5, event: 'C' },
        ],
        bursts: [
            { timestamp_ns: 2500, timestamp_ms: 2, event: 'B', sessions: 8,
              pids: [1, 2, 3, 4, 5, 6, 7, 8, 9, 10] },
        ],
    };
}

test('line series maps per-bucket peak max; area + symbol-none', () => {
    const { option, hasData } = buildConcurrencyOption(data());
    assert.equal(hasData, true);
    assert.equal(option.series[0].type, 'line');
    assert.deepEqual(option.series[0].data, [2, 8, 5]);
    assert.ok(option.series[0].areaStyle);
    assert.equal(option.series[0].symbol, 'none');
    assert.deepEqual(option.xAxis.data, [1000, 2000, 3000]);
});

test('burst marker placed at first bucket whose t >= burst timestamp', () => {
    const { option } = buildConcurrencyOption(data());
    const mp = option.series[0].markPoint.data;
    assert.equal(mp.length, 1);
    // timestamp_ns 2500 -> first peak with t>=2500 is index 2 (t=3000)
    assert.equal(mp[0].coord[0], 2);
    assert.equal(mp[0].value, 8);
    assert.equal(mp[0].symbol, 'triangle');
});

test('no bursts -> markPoint omitted', () => {
    const { option } = buildConcurrencyOption({ ...data(), bursts: [] });
    assert.equal(option.series[0].markPoint, undefined);
});

test('empty peaks -> hasData false, empty tables/topPeaks', () => {
    const m = buildConcurrencyOption({ peaks: [] });
    assert.equal(m.hasData, false);
    assert.equal(m.option, null);
    assert.deepEqual(m.topPeaks, []);
});

test('topPeaks: only max>1, sorted desc, capped at 10', () => {
    const peaks = [{ t: 0, t_ms: 0, max: 1, event: 'x' }]   // dropped (max=1)
        .concat(Array.from({ length: 15 }, (_, i) =>
            ({ t: i + 1, t_ms: i + 1, max: i + 2, event: 'e' + i })));
    const m = buildConcurrencyOption({ peaks, bursts: [], bucket_ns: 1 });
    assert.equal(m.topPeaks.length, 10);
    assert.equal(m.topPeaks[0].max, 16);                 // largest first
    assert.ok(m.topPeaks.every(p => p.max > 1));
});

test('tables HTML: top-peaks + burst sections, PID truncation at 8', () => {
    const m = buildConcurrencyOption(data());
    const html = buildConcurrencyTables(m);
    assert.ok(html.includes('Top Peak Moments'));
    assert.ok(html.includes('Burst Events'));
    assert.ok(html.includes('<b>8</b>'));               // burst sessions
    // 10 pids -> show first 8 then ellipsis
    assert.ok(html.includes('1, 2, 3, 4, 5, 6, 7, 8...'));
});

test('tables HTML: no bursts -> explicit "No burst events" line', () => {
    const m = buildConcurrencyOption({ ...data(), bursts: [] });
    const html = buildConcurrencyTables(m);
    assert.ok(html.includes('No burst events detected'));
});
