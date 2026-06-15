/* Node unit tests for the pure events builder (views/events.js) over the shared
 * eventsConfig (lib/builders/table-configs.js). Proves row shape, sort
 * correctness, drill-intent, and edge cases without a browser.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { buildEventsModel } from '../../web/static/views/events.js';
import { eventsConfig } from '../../web/static/lib/builders/table-configs.js';

function ev(over) {
    return Object.assign({
        name: 'X', event_id: 0, count: 0, total_ms: 0, avg_us: 0,
        p50_us: 0, p95_us: 0, p99_us: 0, max_us: 0, pct: 0, aas: 0,
    }, over);
}

test('no sort: keeps server order; every row clickable', () => {
    const data = { rows: [
        ev({ name: 'CPU*', count: 100 }),
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015, count: 50 }),
        ev({ name: 'Lock:relation', event_id: 0x03000000, count: 30 }),
    ] };
    const m = buildEventsModel(data, null);
    assert.equal(m.hasRows, true);
    assert.equal(m.table.rows.length, 3);
    // order preserved
    assert.deepEqual(m.table.rows.map(r => r.row.name),
        ['CPU*', 'IO:DataFileRead', 'Lock:relation']);
    // all clickable for drill-down
    assert.ok(m.table.rows.every(r => r.cls.includes('clickable')));
    // class dot + escaped name in the first cell
    assert.ok(m.table.rows[0].cells[0].html.includes('class-dot'));
    assert.ok(m.table.rows[0].cells[0].html.includes('CPU*'));
});

test('descending sort by count puts largest first + shows ▼ arrow', () => {
    const data = { rows: [
        ev({ name: 'A', count: 10 }), ev({ name: 'B', count: 30 }),
        ev({ name: 'C', count: 20 }),
    ] };
    const m = buildEventsModel(data, { key: 'count', asc: false });
    assert.deepEqual(m.table.rows.map(r => r.row.name), ['B', 'C', 'A']);
    assert.ok(m.table.headers.find(h => h.key === 'count').arrow.includes('▼'));
});

test('ascending sort flips order + shows ▲ arrow', () => {
    const data = { rows: [
        ev({ name: 'A', count: 10 }), ev({ name: 'B', count: 30 }),
        ev({ name: 'C', count: 20 }),
    ] };
    const m = buildEventsModel(data, { key: 'count', asc: true });
    assert.deepEqual(m.table.rows.map(r => r.row.name), ['A', 'C', 'B']);
    assert.ok(m.table.headers.find(h => h.key === 'count').arrow.includes('▲'));
});

test('drill intent targets event_id with the event name as label', () => {
    assert.deepEqual(
        eventsConfig.onClick(ev({ event_id: 0x01000015, name: 'IO:DataFileRead' })),
        { filterKey: 'event_id', filterValue: 0x01000015, label: 'IO:DataFileRead' });
});

test('empty data -> hasRows false, zero rows', () => {
    assert.equal(buildEventsModel(null, null).hasRows, false);
    assert.equal(buildEventsModel({ rows: [] }, null).hasRows, false);
    assert.equal(buildEventsModel({ rows: [] }, null).table.rows.length, 0);
});

test('single row -> stable', () => {
    const m = buildEventsModel({ rows: [ev({ name: 'Solo', count: 1 })] },
        { key: 'count', asc: false });
    assert.equal(m.table.rows.length, 1);
    assert.equal(m.table.rows[0].row.name, 'Solo');
});
