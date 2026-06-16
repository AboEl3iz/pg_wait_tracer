/* pgwt — "concurrency" view: peak concurrent sessions overlay + burst tables.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches concurrency on a single-flight channel; build()
 * is PURE (lib/builders/concurrency.js -> ECharts option + table models);
 * mount() builds the chart + table containers, feeds the option to the
 * view-owned ECharts instance, and paints the tables. The view OWNS its ECharts
 * instance: created in mount (its inner #concurrency-chart node is built there),
 * disposed in leave() — no module-level chart global.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - Empty peaks → "No concurrency data".
 *   - Peak area-line with burst markers + the top-peaks and burst tables.
 *   - Bucket count scales with the AAS chart width (same heuristic as before).
 */

import { buildConcurrencyOption, buildConcurrencyTables } from '../lib/builders/concurrency.js';
import { isUnavailable } from '../lib/builders/fidelity.js';
import { mountUnavailablePanel } from '../lib/panels.js';

export function createConcurrencyView() {
    let chart = null;   // ECharts instance — owned here, nowhere else

    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    return {
        id: 'concurrency',

        async requests(ctx) {
            const chartWidth = (ctx.chartEl && ctx.chartEl.clientWidth) || 800;
            const numBuckets = Math.min(Math.floor(chartWidth / 4), 300);
            return ctx.transport.request(ctx.channel('table'), 'concurrency', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                buckets: numBuckets, filters: ctx.filters.snapshot(),
            });
        },

        build(data) {
            // EXACT-required (A3): a sampled-only window yields the structured
            // "unavailable" marker — surfaced as an explicit escalate panel.
            if (isUnavailable(data)) return { unavailable: data };
            return buildConcurrencyOption(data);
        },

        mount(el, model, ctx) {
            if (model.unavailable) {
                disposeChart();
                mountUnavailablePanel(el, model.unavailable, ctx);
                return;
            }
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            if (!model.hasData) {
                disposeChart();
                el.innerHTML = '<p style="color:#888;padding:20px">No concurrency data</p>';
                return;
            }
            el.innerHTML =
                '<div id="concurrency-chart" style="width:100%;height:350px;"></div>' +
                '<div id="burst-table" style="padding:10px;"></div>';

            const host = document.getElementById('concurrency-chart');
            disposeChart();
            chart = ctx.echarts.init(host, 'dark');
            chart.setOption(model.option, true);

            document.getElementById('burst-table').innerHTML = buildConcurrencyTables(model);
        },

        enter() { /* chart created in mount (its inner node lives there) */ },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
