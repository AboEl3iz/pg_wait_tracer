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

export class ControlUnavailable extends Error {
    constructor(msg) { super(msg || 'daemon not running'); this.name = 'ControlUnavailable'; }
}

/* Send one control request; resolve to the inner daemon reply object.
 * Throws ControlUnavailable when no daemon is reachable, or a plain Error on
 * transport failure. `transport` is the shared Transport instance. */
export async function control(transport, request) {
    let env;
    try {
        env = await transport.send('control', { request });
    } catch (e) {
        // Transport-level failure (disconnect/timeout): surface as unavailable
        // so the UI shows the degraded panel rather than a console error.
        throw new ControlUnavailable(e && e.message);
    }
    if (!env) throw new ControlUnavailable('empty response');
    if (env.error) throw new ControlUnavailable(env.error);
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
