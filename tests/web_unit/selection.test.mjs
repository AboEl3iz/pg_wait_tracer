/* Node unit tests for the pure pixel->time math of the drag-select overlay
 * (lib/selection.js). The pointer-event wiring is browser-only (driven by
 * Playwright), but the math is isolated so an off-by-one in the band->range
 * conversion is caught here in milliseconds.
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { pixelRangeToTime } from '../../web/static/lib/selection.js';

// Fake chart: linear pixel->value map, value = px * 1e6 (so pixels read as ms).
function linearChart(scale = 1e6, offset = 0) {
    return {
        convertFromPixel(_finder, [x]) { return [x * scale + offset]; },
    };
}

test('maps a left-to-right drag to sorted {from,to}', () => {
    const r = pixelRangeToTime(100, 300, linearChart());
    assert.deepEqual(r, { from: 100e6, to: 300e6 });
});

test('drag direction does not matter (always sorted)', () => {
    const r = pixelRangeToTime(300, 100, linearChart());
    assert.deepEqual(r, { from: 100e6, to: 300e6 });
});

test('degenerate (zero-width) range returns null', () => {
    assert.equal(pixelRangeToTime(150, 150, linearChart()), null);
});

test('honors offset (non-zero axis origin)', () => {
    const r = pixelRangeToTime(0, 50, linearChart(1e6, 5_000_000));
    assert.deepEqual(r, { from: 5_000_000, to: 55_000_000 });
});

test('null/NaN conversion -> null (no garbage range)', () => {
    const bad = { convertFromPixel() { return [NaN]; } };
    assert.equal(pixelRangeToTime(10, 90, bad), null);
    const none = { convertFromPixel() { return null; } };
    assert.equal(pixelRangeToTime(10, 90, none), null);
});

test('rounds to integer nanoseconds', () => {
    const frac = { convertFromPixel(_f, [x]) { return [x + 0.4]; } };
    const r = pixelRangeToTime(10, 90, frac);
    assert.deepEqual(r, { from: 10, to: 90 });  // 10.4->10, 90.4->90
});
