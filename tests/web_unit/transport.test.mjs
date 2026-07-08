/* Node unit tests for lib/transport.js — the WebSocket request/response layer.
 *
 * These pin the transport's trust contract (Trust Milestone T6):
 *   - UI-1: an error envelope {"id":N,"error":"..."} REJECTS the request's
 *     promise (with TransportError) — it must never resolve as data. Bridge-
 *     generated envelopes carry "transport":true and are distinguishable from
 *     command-level errors (e.g. "daemon not running").
 *   - single-flight superseding per channel (CancelledError + late-response drop)
 *   - request-id monotonicity (ids never reused within a session)
 *   - cancel/rejectAll abort behavior (nothing left pending, no unhandled
 *     rejections, late responses dropped)
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
    Transport, CancelledError, TransportError,
} from '../../web/static/lib/transport.js';

// Minimal fake WebSocket capturing sent frames (mirrors state.test.mjs).
class FakeWS {
    constructor() { this.sent = []; this.readyState = 1; this._listeners = {}; }
    addEventListener(ev, fn) { (this._listeners[ev] ||= []).push(fn); }
    send(s) { this.sent.push(JSON.parse(s)); }
    deliver(obj) {
        const e = { data: JSON.stringify(obj) };
        (this._listeners.message || []).forEach(fn => fn(e));
    }
}

globalThis.WebSocket = { OPEN: 1 };

function wired() {
    const ws = new FakeWS();
    const tr = new Transport();
    tr.attach(ws);
    return { ws, tr };
}

// ── UI-1: error envelopes must reject, never resolve as data ────────────────

test('error envelope rejects the request (never resolves as data)', async () => {
    const { ws, tr } = wired();
    const p = tr.send('time_model', { from: 1, to: 2 });
    ws.deliver({ id: ws.sent[0].id, error: 'connection lost', transport: true });
    await assert.rejects(p, (e) => {
        assert.ok(e instanceof TransportError, 'rejects with TransportError');
        assert.equal(e.message, 'connection lost');
        assert.equal(e.transport, true, 'bridge envelope flagged as transport-level');
        return true;
    });
    assert.deepEqual(tr.pending, {}, 'nothing left pending');
});

test('command-level error envelope rejects without the transport flag', async () => {
    const { ws, tr } = wired();
    const p = tr.send('control', { request: { cmd: 'status' } });
    ws.deliver({ id: ws.sent[0].id, error: 'daemon not running' });
    await assert.rejects(p, (e) => {
        assert.ok(e instanceof TransportError);
        assert.equal(e.message, 'daemon not running');
        assert.equal(e.transport, false, 'server-side error is NOT transport-level');
        return true;
    });
});

test('error envelope on a channel rejects and frees the channel', async () => {
    const { ws, tr } = wired();
    const p = tr.request('overview.table', 'time_model', {});
    ws.deliver({ id: ws.sent[0].id, error: 'timeout', transport: true });
    await assert.rejects(p, TransportError);
    assert.deepEqual(tr.channelId, {}, 'channel slot freed after error');
    // A follow-up request on the same channel works normally.
    const p2 = tr.request('overview.table', 'time_model', {});
    ws.deliver({ id: ws.sent[1].id, rows: [1] });
    assert.deepEqual((await p2).rows, [1]);
});

test('a response with data and no error resolves (no false rejects)', async () => {
    const { ws, tr } = wired();
    const p = tr.send('info');
    ws.deliver({ id: ws.sent[0].id, from_ns: 1, to_ns: 2 });
    assert.equal((await p).from_ns, 1);
});

// ── Single-flight superseding ────────────────────────────────────────────────

test('request supersedes the pending request on the same channel', async () => {
    const { ws, tr } = wired();
    const p1 = tr.request('overview.table', 'time_model', { from: 1 });
    const p1err = p1.catch(e => e);
    const p2 = tr.request('overview.table', 'time_model', { from: 2 });

    assert.ok((await p1err) instanceof CancelledError, 'first request cancelled');
    assert.equal(ws.sent.length, 2);

    // Late response for the superseded id is dropped; the new one resolves.
    ws.deliver({ id: ws.sent[0].id, value: 'stale' });
    ws.deliver({ id: ws.sent[1].id, value: 'fresh' });
    assert.equal((await p2).value, 'fresh');
});

test('a late ERROR envelope for a superseded id is dropped too', async () => {
    const { ws, tr } = wired();
    const p1 = tr.request('ch', 'cmd', {});
    p1.catch(() => {});
    const p2 = tr.request('ch', 'cmd', {});
    // The stale id errors (e.g. the bridge timed it out) — must not disturb p2.
    ws.deliver({ id: ws.sent[0].id, error: 'timeout', transport: true });
    ws.deliver({ id: ws.sent[1].id, ok: 1 });
    assert.equal((await p2).ok, 1);
});

// ── Request-id monotonicity ──────────────────────────────────────────────────

test('request ids are strictly increasing, never reused', async () => {
    const { ws, tr } = wired();
    const seen = [];
    for (let i = 0; i < 5; i++) {
        tr.send('info').catch(() => {});
        seen.push(ws.sent[ws.sent.length - 1].id);
    }
    // Cancelling / erroring must not recycle ids.
    const p = tr.request('ch', 'cmd', {});
    p.catch(() => {});
    tr.cancel('ch');
    tr.send('info').catch(() => {});
    seen.push(ws.sent[ws.sent.length - 1].id);

    const sorted = [...seen].sort((a, b) => a - b);
    assert.deepEqual(seen, sorted, 'ids in send order are ascending');
    assert.equal(new Set(seen).size, seen.length, 'no id reused');
    tr.rejectAll('cleanup');
});

// ── Abort behavior ───────────────────────────────────────────────────────────

test('cancel rejects the pending request and drops its late response', async () => {
    const { ws, tr } = wired();
    const p = tr.request('view.ch', 'cmd', {});
    const perr = p.catch(e => e);
    tr.cancel('view.ch');
    assert.ok((await perr) instanceof CancelledError);
    assert.deepEqual(tr.pending, {});
    // Delivering the (now unknown) id is a no-op, not a throw.
    ws.deliver({ id: ws.sent[0].id, v: 'late' });
});

test('rejectAll clears everything and rejects with a transport-level error', async () => {
    const { ws, tr } = wired();
    const a = tr.send('info');
    const b = tr.request('ch', 'cmd', {});
    const aerr = a.catch(e => e);
    const berr = b.catch(e => e);
    tr.rejectAll('disconnected');
    const ea = await aerr, eb = await berr;
    assert.equal(ea.message, 'disconnected');
    assert.ok(ea instanceof TransportError, 'disconnect is a TransportError');
    assert.equal(ea.transport, true);
    assert.ok(eb instanceof TransportError);
    assert.deepEqual(tr.pending, {});
    assert.deepEqual(tr.channelId, {});
    void ws;
});

test('send while not connected rejects with a transport-level error', async () => {
    const tr = new Transport();     // no ws attached
    await assert.rejects(tr.send('info'), (e) => {
        assert.ok(e instanceof TransportError);
        assert.equal(e.transport, true);
        return true;
    });
});
