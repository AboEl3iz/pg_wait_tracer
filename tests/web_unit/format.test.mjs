/* Node unit tests for lib/format.js — the UTC time conversions (UI-11).
 *
 * pgwt displays every time in UTC and defines the custom-range
 * datetime-local inputs as UTC too. These conversions must be independent of
 * the host timezone: the functions use only getUTC accessors and Z-suffixed
 * parsing, so the expected strings hold no matter what TZ the test runs under.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    nsToDatetimeLocalUTC, datetimeLocalUTCToNs, fmtTime,
} from '../../web/static/lib/format.js';

// 2026-03-15 12:34:56 UTC
const NS = Date.UTC(2026, 2, 15, 12, 34, 56) * 1e6;

test('nsToDatetimeLocalUTC renders the UTC wall time regardless of host TZ', () => {
    assert.equal(nsToDatetimeLocalUTC(NS), '2026-03-15T12:34:56');
});

test('datetimeLocalUTCToNs parses the input as UTC (a UTC+3 browser gets no 3h shift)', () => {
    assert.equal(datetimeLocalUTCToNs('2026-03-15T12:34:56'), NS);
    // Without seconds (what browsers emit unless step=1 forces them).
    assert.equal(datetimeLocalUTCToNs('2026-03-15T12:34'),
        Date.UTC(2026, 2, 15, 12, 34, 0) * 1e6);
});

test('round-trip ns -> input string -> ns is exact at second granularity', () => {
    assert.equal(datetimeLocalUTCToNs(nsToDatetimeLocalUTC(NS)), NS);
});

test('datetimeLocalUTCToNs rejects garbage', () => {
    assert.equal(datetimeLocalUTCToNs(''), null);
    assert.equal(datetimeLocalUTCToNs(null), null);
    assert.equal(datetimeLocalUTCToNs('not-a-date'), null);
});

test('fmtTime renders HH:MM:SS in UTC', () => {
    assert.equal(fmtTime(NS), '12:34:56');
});
