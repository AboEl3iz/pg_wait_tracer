/* Node unit tests for the shared table builder (lib/table.js) + the migrated
 * table configs (overview, events). Proves sort, cell formatting, row-class and
 * the drill-intent descriptors are correct without a browser.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildTableModel } from '../../web/static/lib/table.js';
import { overviewConfig, eventsConfig } from '../../web/static/lib/builders/table-configs.js';
import { buildSummary } from '../../web/static/views/overview.js';

test('overview: rows keep server order (no sort) + clickable indent-1', () => {
    const rows = [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 1, name: 'CPU*', ms: 4800, pct: 38.4, aas: 1.33 },
        { indent: 2, name: 'IO:DataFileRead', ms: 2100, pct: 16.8, aas: 0.58 },
    ];
    const model = buildTableModel(overviewConfig, rows, null);
    assert.equal(model.rows.length, 3);
    assert.equal(model.headers[0].label, 'Stat Name');
    // order preserved
    assert.ok(model.rows[0].cells[0].html.includes('DB Time'));
    // indent-1 is clickable, indent-2 is not
    assert.ok(model.rows[1].cls.includes('clickable'));
    assert.ok(!model.rows[2].cls.includes('clickable'));
    // DB Time pct shown as plain pct, class rows as bar
    assert.ok(model.rows[0].cells[2].html.includes('100.0%'));
    assert.ok(model.rows[1].cells[2].html.includes('pct-bar'));
});

test('overview onClick: indent-1 -> class drill intent, strips asterisk', () => {
    assert.deepEqual(
        overviewConfig.onClick({ indent: 1, name: 'CPU*' }),
        { filterKey: 'class', filterValue: 'CPU', label: 'CPU' });
    assert.equal(overviewConfig.onClick({ indent: 0, name: 'DB Time' }), null);
    assert.equal(overviewConfig.onClick({ indent: 2, name: 'IO:WalSync' }), null);
});

test('events: descending sort by count', () => {
    const rows = [
        { name: 'A', event_id: 1, count: 10, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
        { name: 'B', event_id: 2, count: 30, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
        { name: 'C', event_id: 3, count: 20, total_ms: 1, avg_us: 1, p50_us: 1,
          p95_us: 1, p99_us: 1, max_us: 1, pct: 1, aas: 0.1 },
    ];
    const model = buildTableModel(eventsConfig, rows, { key: 'count', asc: false });
    const order = model.rows.map(r => r.row.name);
    assert.deepEqual(order, ['B', 'C', 'A']);
    // sort arrow on the count header
    const countHdr = model.headers.find(h => h.key === 'count');
    assert.ok(countHdr.arrow.includes('▼'));
});

test('events: ascending sort flips order', () => {
    const rows = [
        { name: 'A', count: 10 }, { name: 'B', count: 30 }, { name: 'C', count: 20 },
    ].map(r => ({ ...r, event_id: 0, total_ms: 0, avg_us: 0, p50_us: 0, p95_us: 0,
        p99_us: 0, max_us: 0, pct: 0, aas: 0 }));
    const model = buildTableModel(eventsConfig, rows, { key: 'count', asc: true });
    assert.deepEqual(model.rows.map(r => r.row.name), ['A', 'C', 'B']);
});

test('events onClick: event_id drill intent', () => {
    assert.deepEqual(
        eventsConfig.onClick({ event_id: 0x01000015, name: 'IO:DataFileRead' }),
        { filterKey: 'event_id', filterValue: 0x01000015, label: 'IO:DataFileRead' });
});

test('buildSummary: exact metric set + values', () => {
    const data = { wall_ms: 3600000, rows: [
        { indent: 0, name: 'DB Time', ms: 12500, pct: 100, aas: 3.47 },
        { indent: 1, name: 'CPU*', ms: 4800 },
        { indent: 0, name: 'Idle', ms: 45000 },
    ] };
    const s = buildSummary(data, 4);
    assert.deepEqual(s, [
        { label: 'DB Time', value: '12.5s' },
        { label: 'Wall', value: '3600.0s' },
        { label: 'AAS', value: '3.47' },
        { label: 'Idle', value: '45.0s' },
        { label: 'CPUs', value: '4' },
    ]);
});

test('buildSummary: empty data -> empty metrics', () => {
    assert.deepEqual(buildSummary(null, 4), []);
    assert.deepEqual(buildSummary({ rows: [] }, 4), []);
});
