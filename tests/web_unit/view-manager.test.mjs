/* Node unit tests for lib/view-manager.js — the response chokepoint.
 *
 * The view-manager is the single place that decides whether a fetched response
 * may render (Trust Milestone T6, UI-7). These tests pin:
 *   - epoch cancellation: a refresh superseded by a newer refresh never paints
 *   - leave-before-enter: switching views disposes the old view fully before
 *     the new one enters, and cancels the old view's channels
 *   - response-to-inactive-view dropped: a slow response addressed to a view
 *     that is no longer active is dropped at the chokepoint
 *   - transport errors surface through ctx.onRequestError (UI-1 degraded
 *     state), while CancelledError stays silent
 */
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { ViewManager } from '../../web/static/lib/view-manager.js';
import { CancelledError, TransportError } from '../../web/static/lib/transport.js';

/* Deferred promise helper. */
function deferred() {
    let resolve, reject;
    const promise = new Promise((res, rej) => { resolve = res; reject = rej; });
    return { promise, resolve, reject };
}

/* A scriptable view: each requests() call takes the next deferred from `gates`
 * (so the test controls exactly when each response "arrives"). */
function makeView(id, log) {
    const gates = [];
    return {
        view: {
            id,
            requests(ctx) {
                const d = deferred();
                gates.push(d);
                log.push(id + '.requests');
                void ctx;
                return d.promise;
            },
            build(data) { log.push(id + '.build'); return { data }; },
            mount(el, model) { log.push(id + '.mount:' + JSON.stringify(model.data)); },
            enter() { log.push(id + '.enter'); },
            leave() { log.push(id + '.leave'); },
        },
        gates,
    };
}

function makeCtx(cancelled, errors) {
    return {
        transport: { cancel: (ch) => cancelled.push(ch) },
        onRequestError: (e) => errors.push(e),
    };
}

function newVM(log, cancelled, errors) {
    const vm = new ViewManager(makeCtx(cancelled, errors));
    vm.setContainer({});
    return vm;
}

// ── Epoch cancellation ───────────────────────────────────────────────────────

test('a refresh superseded by a newer refresh never paints', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    vm.register(a.view);
    vm.active = a.view;

    const r1 = vm.refresh();          // epoch 1, gate 0 pending
    const r2 = vm.refresh();          // epoch 2, gate 1 pending

    // The OLD response arrives after the new refresh began.
    a.gates[0].resolve({ v: 'stale' });
    await r1;
    assert.ok(!log.some(l => l.includes('stale')), 'stale response not mounted');

    a.gates[1].resolve({ v: 'fresh' });
    await r2;
    assert.ok(log.includes('a.mount:{"v":"fresh"}'), 'fresh response mounted');
    assert.equal(log.filter(l => l.startsWith('a.mount')).length, 1,
        'exactly one mount');
});

// ── Leave-before-enter ordering ──────────────────────────────────────────────

test('switchTo leaves the old view (and cancels its channels) before entering the new one', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    const b = makeView('b', log);
    vm.register(a.view).register(b.view);

    vm.active = a.view;
    a.view.enter(vm._viewCtx());
    // Open a channel on view a so the switch has something to cancel.
    vm._viewCtx().channel('table');

    const sw = vm.switchTo('b');
    b.gates[0].resolve({ v: 1 });
    await sw;

    const leaveIdx = log.indexOf('a.leave');
    const enterIdx = log.indexOf('b.enter');
    assert.ok(leaveIdx >= 0 && enterIdx >= 0, 'both hooks ran');
    assert.ok(leaveIdx < enterIdx, 'a.leave strictly before b.enter');
    assert.deepEqual(cancelled, ['a.table'], "old view's channels cancelled");
});

// ── Response-to-inactive-view dropped ────────────────────────────────────────

test('a slow response for a no-longer-active view is dropped at the chokepoint', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    const b = makeView('b', log);
    vm.register(a.view).register(b.view);
    vm.active = a.view;

    const rA = vm.refresh();          // view a fetch in flight
    const sw = vm.switchTo('b');      // user switches away before it lands
    b.gates[0].resolve({ v: 'b-data' });
    await sw;

    // The late a-response arrives AFTER b is active.
    a.gates[0].resolve({ v: 'a-late' });
    await rA;

    assert.ok(!log.some(l => l.startsWith('a.build')), 'inactive view never built');
    assert.ok(!log.some(l => l.includes('a-late')), 'inactive response never mounted');
    assert.ok(log.includes('b.mount:{"v":"b-data"}'), 'active view mounted its own data');
});

test('isActive() reflects the chokepoint for in-view async re-fetches', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    const b = makeView('b', log);
    vm.register(a.view).register(b.view);
    vm.active = a.view;

    const ctxA = vm._viewCtx();
    assert.equal(ctxA.isActive(), true);
    const sw = vm.switchTo('b');
    b.gates[0].resolve({});
    await sw;
    assert.equal(ctxA.isActive(), false, 'stale ctx sees itself inactive');
});

// ── Error propagation (UI-1 degraded state) ──────────────────────────────────

test('a transport-level failure surfaces via ctx.onRequestError', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    vm.register(a.view);
    vm.active = a.view;

    const r = vm.refresh();
    a.gates[0].reject(new TransportError('connection lost', { transport: true }));
    await r;

    assert.equal(errors.length, 1, 'error reported to the app');
    assert.equal(errors[0].message, 'connection lost');
    assert.ok(!log.some(l => l.startsWith('a.build')), 'no paint on error');
});

test('CancelledError (superseded) stays silent — no error report', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    vm.register(a.view);
    vm.active = a.view;

    const r = vm.refresh();
    a.gates[0].reject(new CancelledError('superseded'));
    await r;
    assert.equal(errors.length, 0, 'superseded request is not an error');
});

test('a stale refresh that errors after a switch does not report (its view is gone)', async () => {
    const log = [], cancelled = [], errors = [];
    const vm = newVM(log, cancelled, errors);
    const a = makeView('a', log);
    const b = makeView('b', log);
    vm.register(a.view).register(b.view);
    vm.active = a.view;

    const rA = vm.refresh();
    const sw = vm.switchTo('b');
    b.gates[0].resolve({});
    await sw;
    // The old view's request fails AFTER the switch — must not flip the app
    // into a degraded state for a request nobody is waiting on.
    a.gates[0].reject(new TransportError('timeout', { transport: true }));
    await rA;
    assert.equal(errors.length, 0, 'stale-epoch error not reported');
});
