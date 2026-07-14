/* pgwt — explicit UI state.
 *
 * Replaces the old grab-bag global `state` object that was mutated mid-flight
 * by every code path. State here is split into three concerns, each with a
 * small explicit API:
 *
 *   - TimeRange      : the view window (from/to ns) + server clock + live span.
 *   - FilterStack    : current drill-down filters + breadcrumb history.
 *   - ServerInfo     : static server facts (cpus, trace bounds).
 *
 * Views receive exactly what they need (a snapshot) rather than reaching into
 * a shared mutable object. Nothing here touches the DOM or the network — it is
 * pure data, so it is Node-testable.
 */

const FIFTEEN_MIN_NS = 900 * 1e9;

/* Static server facts, set once on connect and refreshed on info ticks. */
export class ServerInfo {
    constructor() {
        this.fromNs = 0;      // earliest ts in the trace
        this.toNs = 0;        // latest ts in the trace
        this.nowNs = 0;       // server wall clock (live anchor)
        this.numCpus = 0;
        this.numEvents = 0;
        // Version handshake (T7 / TST-11): the server reports these in `info`.
        this.serverVersion = null;
        this.protocol = null;
    }

    update(info) {
        if (!info) return;
        if (info.from_ns) this.fromNs = info.from_ns;
        if (info.to_ns) this.toNs = info.to_ns;
        this.nowNs = info.now_ns || info.to_ns || this.nowNs;
        if (info.num_cpus != null) this.numCpus = info.num_cpus;
        if (info.num_events != null) this.numEvents = info.num_events;
        if (info.server_version != null) this.serverVersion = info.server_version;
        if (info.protocol != null) this.protocol = info.protocol;
    }
}

/* The view window. Single source of truth for from/to (ns). Zoom history and
 * live-span tracking live here too, but no timers (those belong to the
 * view-manager refresh loop). */
export class TimeRange {
    constructor(server) {
        this.server = server;
        this.from = 0;
        this.to = 0;
        this.zoomHistory = [];
        this.liveRangeSecs = 900;   // span used when live mode is on
    }

    /* Initialize to the default "last 15 min ending NOW" window. */
    initDefault() {
        const now = this.server.nowNs || this.server.toNs;
        this.from = now - FIFTEEN_MIN_NS;
        this.to = now;
        this.liveRangeSecs = 900;
    }

    set(from, to) {
        this.from = from;
        this.to = to;
    }

    /* Push the current window onto zoom history, then move to [from,to]. */
    zoomTo(from, to) {
        this.zoomHistory.push({ from: this.from, to: this.to });
        if (this.zoomHistory.length > 10) this.zoomHistory.shift();
        this.from = from;
        this.to = to;
    }

    /* Pop zoom history, or widen the window 2x clamped to trace bounds. */
    zoomOut() {
        if (this.zoomHistory.length > 0) {
            const prev = this.zoomHistory.pop();
            this.from = prev.from;
            this.to = prev.to;
            return;
        }
        const mid = (this.from + this.to) / 2;
        const span = this.to - this.from;
        this.from = Math.max(this.server.fromNs, mid - span);
        this.to = Math.min(this.server.toNs, mid + span);
    }

    /* Re-anchor a live window so it always ends at the latest server clock.
     * "Last N min always means NOW" — never anchor to stale data. */
    anchorLive(rangeSecs) {
        const end = this.server.nowNs || this.server.toNs;
        this.liveRangeSecs = rangeSecs;
        this.from = end - rangeSecs * 1e9;
        this.to = end;
    }

    span() { return this.to - this.from; }
}

/* Filters + breadcrumb stack for drill-down. Pure data transitions; callers
 * decide which view to show. */
export class FilterStack {
    constructor() {
        this.filters = {};
        this.breadcrumbs = [];     // [{ label, filters, view }]
        this.currentLabel = null;
    }

    snapshot() { return { ...this.filters }; }

    /* Drill into filterKey=filterValue. `fromView` is the view we drilled from
     * (recorded in the breadcrumb so drillUp can restore it). Returns the new
     * filters object. */
    drill(filterKey, filterValue, label, fromView) {
        this.breadcrumbs.push({
            label: this.currentLabel || '',
            filters: { ...this.filters },
            view: fromView,
        });
        this.filters = { ...this.filters, [filterKey]: filterValue };
        this.currentLabel = label;
        return this.filters;
    }

    /* Restore the breadcrumb at `index`; returns its recorded view id. */
    drillUp(index) {
        const crumb = this.breadcrumbs[index];
        if (!crumb) return null;
        this.filters = { ...crumb.filters };
        this.currentLabel = crumb.label;
        this.breadcrumbs = this.breadcrumbs.slice(0, index);
        return crumb.view;
    }

    clear() {
        this.filters = {};
        this.breadcrumbs = [];
        this.currentLabel = null;
    }

    isEmpty() {
        return this.breadcrumbs.length === 0 &&
               Object.keys(this.filters).length === 0;
    }
}

export { FIFTEEN_MIN_NS };
