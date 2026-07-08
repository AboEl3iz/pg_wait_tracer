/* pgwt — daemon control-plane client (Phase B5).
 *
 * pgwt-server proxies the daemon's unix control socket as a single `control`
 * command over the same WebSocket:
 *
 *   → {"id":N,"cmd":"control","request":{"cmd":"status"}}
 *   ← {"id":N,"response":<daemon reply>}   (or {"id":N,"error":"daemon not running"})
 *
 * This thin wrapper unwraps that envelope so callers get the inner daemon reply
 * directly (or a thrown Error). It uses the transport's channel-less send()
 * (control calls are not view refreshes and must not be superseded by a tab
 * switch), and it tolerates the "no daemon" case — when the server is replaying
 * a static trace there is no daemon, so the UI degrades to "escalation
 * unavailable" rather than erroring.
 */

import { TransportError } from './transport.js';

export class ControlUnavailable extends Error {
    constructor(msg) { super(msg || 'daemon not running'); this.name = 'ControlUnavailable'; }
}

/* Send one control request; resolve to the inner daemon reply object.
 * Throws ControlUnavailable when the SERVER answered "no daemon here" (static
 * replay) — that is a stable fact about the deployment. A transport-level
 * failure (disconnect / bridge lost the pipe / timeout) rethrows as-is: the
 * daemon may be fine, we just can't reach anything right now, so callers must
 * NOT latch "no daemon" off it (UI-1/UI-9). */
export async function control(transport, request) {
    let env;
    try {
        env = await transport.send('control', { request });
    } catch (e) {
        if (e instanceof TransportError && !e.transport) {
            // Command-level error from a live server: no daemon control socket.
            throw new ControlUnavailable(e.message);
        }
        throw e;   // transport degraded — caller keeps last-known daemon state
    }
    if (!env) throw new ControlUnavailable('empty response');
    return env.response || {};
}

export function controlStatus(transport)  { return control(transport, { cmd: 'status' }); }
export function controlMetrics(transport) { return control(transport, { cmd: 'metrics' }); }
export function controlEscalate(transport, durationS, reason) {
    return control(transport, {
        cmd: 'escalate',
        duration_s: durationS || 60,
        reason: reason || 'manual',
    });
}
export function controlDeescalate(transport) {
    return control(transport, { cmd: 'deescalate' });
}
