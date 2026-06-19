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

// %DB column is index 8 (name,count,total_ms,avg_us,p50,p95,p99,max,pct,aas).
const PCT_COL = 8;

test('idle event with pct=null renders "—" for %DB, not a bar', () => {
    // Client:ClientRead is idle (excluded from DB Time): server sends
    // pct=null, the cell must show an em-dash rather than a bogus pct-bar.
    const m = buildEventsModel({ rows: [
        ev({ name: 'Client:ClientRead', event_id: 0x06000000,
             count: 100, total_ms: 5000, pct: null }),
    ] }, null);
    const cell = m.table.rows[0].cells[PCT_COL].html;
    assert.ok(cell.includes('—'), `expected em-dash, got: ${cell}`);
    assert.ok(!cell.includes('pct-bar'), `should not render a bar, got: ${cell}`);
    // The row is still visible with its time intact.
    assert.equal(m.table.rows[0].row.total_ms, 5000);
});

test('non-idle event with numeric pct still renders a pct-bar', () => {
    const m = buildEventsModel({ rows: [
        ev({ name: 'IO:DataFileRead', event_id: 0x01000015,
             count: 100, total_ms: 2100, pct: 16.8 }),
    ] }, null);
    const cell = m.table.rows[0].cells[PCT_COL].html;
    assert.ok(cell.includes('pct-bar'), `expected pct-bar, got: ${cell}`);
    assert.ok(cell.includes('16.8%'), `expected 16.8%, got: ${cell}`);
});
