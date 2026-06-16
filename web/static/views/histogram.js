/* pgwt — "histogram" view: the latency-over-time heatmap + class/event selectors.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches the event list (for the selectors) and the
 * heatmap (for the current selector/filter state) on single-flight channels;
 * build() is PURE (lib/builders/histogram.js — heatmap option + selector model);
 * mount() paints the selector shell + feeds the option to the view-owned ECharts
 * instance. The view OWNS its ECharts instance: created in enter(), disposed in
 * leave() — no module-level chart global. Window-resize is per-instance.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - Class / Event selectors above a heatmap container.
 *   - Selecting a class repopulates the event list; selecting either refetches
 *     the heatmap with the chosen filter (without a full tab refresh).
 *   - Empty grid → "No data for selected event/range" placeholder.
 */

import { buildHeatmapOption, buildSelectorModel } from '../lib/builders/histogram.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';
import { fmtCount } from '../lib/format.js';

const BUCKETS = () =>
    Math.min(Math.floor((typeof window !== 'undefined' ? window.innerWidth : 1800) / 6), 300);

export function createHistogramView() {
    let chart = null;          // ECharts instance — owned here, nowhere else
    let el = null;             // the view's container
    let events = [];           // cached event list (drives the selectors)
    let ctxRef = null;         // last ctx (for the selector-driven re-fetch)
    let onResize = null;

    function currentFilters() {
        const f = { ...ctxRef.filters.filters };
        const cs = document.getElementById('hm-class');
        const es = document.getElementById('hm-event');
        if (cs && cs.value) f.class = cs.value;
        if (es && es.value) f.event_id = parseInt(es.value);
        return f;
    }

    function fetchHeatmap(filters) {
        return ctxRef.transport.request(ctxRef.channel('heatmap'), 'heatmap', {
            from: ctxRef.timeRange.from, to: ctxRef.timeRange.to,
            buckets: BUCKETS(), filters,
        });
    }

    function renderHeatmap(data) {
        const host = document.getElementById('heatmap-container');
        if (!host) return;
        // EXACT-required (A3): heatmap is unavailable over a sampled-only window.
        if (isUnavailable(data)) {
            if (chart) { chart.dispose(); chart = null; }
            mountUnavailablePanel(host, data, ctxRef);
            return;
        }
        const model = buildHeatmapOption(data);
        if (!model.hasData) {
            if (chart) { chart.dispose(); chart = null; }
            host.innerHTML = '<div class="loading">No data for selected event/range</div>';
            return;
        }
        if (!chart) chart = ctxRef.echarts.init(host, 'dark');
        chart.setOption(model.option, true);
    }

    /* Repopulate the <option>s of the class + event selects from `events`,
     * preserving any active filter / prior selection (matches the old
     * populateSelectors / populateEventSelect). */
    function populateSelectors() {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const model = buildSelectorModel(events, fmtCount);

        const prevClass = classSelect.value;
        classSelect.innerHTML = '<option value="">All</option>';
        for (const cls of model.classes) {
            const opt = document.createElement('option');
            opt.value = cls; opt.textContent = cls; classSelect.appendChild(opt);
        }
        const f = ctxRef.filters.filters;
        if (f.class) classSelect.value = f.class;
        else if (prevClass) classSelect.value = prevClass;
        populateEventSelect();
    }

    function populateEventSelect() {
        const classSelect = document.getElementById('hm-class');
        const eventSelect = document.getElementById('hm-event');
        if (!classSelect || !eventSelect) return;
        const model = buildSelectorModel(events, fmtCount);
        const selClass = classSelect.value;
        const list = selClass ? (model.eventsByClass[selClass] || []) : model.allEvents;

        const prevEvent = eventSelect.value;
        eventSelect.innerHTML =
            '<option value="">All' + (selClass ? ' ' + selClass : '') + '</option>';
        for (const ev of list) {
            const opt = document.createElement('option');
            opt.value = ev.event_id; opt.textContent = ev.label;
            eventSelect.appendChild(opt);
        }
        const f = ctxRef.filters.filters;
        if (f.event_id) eventSelect.value = f.event_id;
        else if (prevEvent) eventSelect.value = prevEvent;
    }

    function ensureShell(container) {
        if (document.getElementById('heatmap-container')) return;
        container.innerHTML =
            '<div class="event-selector">' +
            '  <label>Class</label>' +
            '  <select id="hm-class"><option value="">All</option></select>' +
            '  <label>Event</label>' +
            '  <select id="hm-event"><option value="">All</option></select>' +
            '</div>' +
            '<div id="heatmap-container"></div>';
        document.getElementById('hm-class').addEventListener('change', () => {
            populateEventSelect();
            document.getElementById('hm-event').value = '';
            selectorRefetch();
        });
        document.getElementById('hm-event').addEventListener('change', selectorRefetch);
    }

    /* Selector-driven re-fetch: a single-flight heatmap request that only
     * paints if histogram is still the active view (the chokepoint, applied
     * locally because this fetch is not driven by the view-manager refresh). */
    async function selectorRefetch() {
        let hm;
        try { hm = await fetchHeatmap(currentFilters()); }
        catch (e) { return; }  // superseded / disconnect
        if (ctxRef.isActive && !ctxRef.isActive()) return;
        renderHeatmap(hm);
    }

    return {
        id: 'histogram',

        async requests(ctx) {
            ctxRef = ctx;
            const evData = await ctx.transport.request(ctx.channel('events'),
                'top_events', { from: ctx.timeRange.from, to: ctx.timeRange.to, filters: {} });
            events = (evData && evData.rows) || [];
            const hm = await fetchHeatmap(currentFilters());
            return { events, heatmap: hm };
        },

        build(data) {
            // PURE pre-compute; mount applies it (the DOM selectors still drive
            // a possible re-fetch, but the initial paint is from this model).
            return {
                selectors: buildSelectorModel(data.events, fmtCount),
                heatmap: buildHeatmapOption(data.heatmap),
                raw: data.heatmap,
            };
        },

        mount(container, model, ctx) {
            ctxRef = ctx;
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            el = container;
            ensureShell(container);
            populateSelectors();
            renderHeatmap(model.raw);
        },

        enter(ctx) {
            ctxRef = ctx;   // chart is created lazily in renderHeatmap (needs the
                            // inner #heatmap-container element that mount builds)
        },

        leave() {
            if (chart) { chart.dispose(); chart = null; }
            el = null;
        },

        resize() { if (chart) chart.resize(); },
    };
}
