/* pgwt — WebSocket transport with request ids and single-flight per channel.
 *
 * Replaces app.js's ad-hoc `_refreshGen` generation counters. The old code
 * guarded against stale responses by stamping a global generation number on
 * every user action and re-checking it before each renderer ran — fragile,
 * because every async branch had to remember to re-check, and several did not
 * (the histogram/timeline/transitions branches), which is exactly where the
 * chaos races lived.
 *
 * Here, superseding is structural:
 *
 *   - Every request carries a unique id and belongs to a named *channel*
 *     (e.g. "overview.table", "overview.chart"). A channel models "the one
 *     in-flight request whose answer this slot cares about".
 *   - request(channel, cmd, params) cancels any pending request on the same
 *     channel before issuing the new one. The cancelled request's promise
 *     rejects with a CancelledError and its eventual response is dropped when
 *     it arrives (its id is no longer the channel's current id).
 *   - send(cmd, params) is the channel-less escape hatch (e.g. one-off `info`
 *     polls) — no superseding, just a plain request id.
 *
 * The view-manager layers a second, coarser guard on top (drop responses for a
 * view that is no longer active); transport handles the fine-grained
 * "newest-request-on-this-channel-wins".
 */

export class CancelledError extends Error {
    constructor(msg) { super(msg || 'cancelled'); this.name = 'CancelledError'; }
}

export class Transport {
    constructor() {
        this.ws = null;
        this.nextId = 1;
        this.pending = {};        // id -> { resolve, reject, timer, channel }
        this.channelId = {};      // channel -> current (latest) request id
        this.timeoutMs = 30000;
    }

    /* Wire up an open WebSocket. Caller owns open/close/reconnect lifecycle and
     * calls attach() on each fresh connection. */
    attach(ws) {
        this.ws = ws;
        ws.addEventListener('message', (e) => this._onMessage(e));
    }

    _onMessage(e) {
        let data;
        try { data = JSON.parse(e.data); } catch { return; }
        const p = this.pending[data.id];
        if (!p) return;             // unknown / already-superseded id: drop
        clearTimeout(p.timer);
        delete this.pending[data.id];
        if (p.channel && this.channelId[p.channel] === data.id) {
            delete this.channelId[p.channel];
        }
        p.resolve(data);
    }

    isOpen() {
        return this.ws && this.ws.readyState === WebSocket.OPEN;
    }

    /* Low-level request without a channel — no superseding. */
    send(cmd, params) {
        return this._issue(cmd, params, null);
    }

    /* Single-flight request on `channel`: supersedes any pending request on the
     * same channel (its promise rejects with CancelledError; its late response
     * is dropped on arrival). */
    request(channel, cmd, params) {
        this.cancel(channel);
        return this._issue(cmd, params, channel);
    }

    /* Cancel the pending request (if any) on `channel`. */
    cancel(channel) {
        const id = this.channelId[channel];
        if (id == null) return;
        const p = this.pending[id];
        delete this.channelId[channel];
        if (p) {
            clearTimeout(p.timer);
            delete this.pending[id];
            p.reject(new CancelledError('superseded: ' + channel));
        }
    }

    _issue(cmd, params, channel) {
        if (!this.isOpen()) {
            return Promise.reject(new Error('not connected'));
        }
        const id = this.nextId++;
        const msg = JSON.stringify({ id, cmd, ...params });
        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                delete this.pending[id];
                if (channel && this.channelId[channel] === id) {
                    delete this.channelId[channel];
                }
                reject(new Error('timeout'));
            }, this.timeoutMs);
            this.pending[id] = { resolve, reject, timer, channel };
            if (channel) this.channelId[channel] = id;
            this.ws.send(msg);
        });
    }

    /* Reject everything in flight (called on disconnect). */
    rejectAll(reason) {
        for (const id of Object.keys(this.pending)) {
            const p = this.pending[id];
            clearTimeout(p.timer);
            p.reject(new Error(reason));
        }
        this.pending = {};
        this.channelId = {};
    }
}
