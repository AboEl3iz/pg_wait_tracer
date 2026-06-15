/* pgwt — "timeline" view: the per-session wait timeline (Gantt-style bars).
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3). requests() fetches session_timeline (only when a pid/query filter is
 * set — otherwise returns a prompt sentinel) on a single-flight channel;
 * build() is PURE (lib/builders/timeline.js -> ECharts custom-series option);
 * mount() paints the optional truncation banner + feeds the option to the
 * view-owned ECharts instance. The view OWNS its ECharts instance: created in
 * mount (its inner #timeline-chart node is built there), disposed in leave() —
 * no module-level chart global.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - No pid/query filter → "Select a session (PID) or query" prompt.
 *   - Empty result → "No events for selected session/range".
 *   - Truncated result → a yellow "Showing N of M events" banner above the chart.
 */

import { buildTimelineOption } from '../lib/builders/timeline.js';

export function createTimelineView() {
    let chart = null;   // ECharts instance — owned here, nowhere else

    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    return {
        id: 'timeline',

        async requests(ctx) {
            const f = ctx.filters.filters;
            if (!f.pid && !f.query_id) return { prompt: true };
            return ctx.transport.request(ctx.channel('table'), 'session_timeline', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(),
            });
        },

        build(data, ctx) {
            if (data && data.prompt) return { prompt: true };
            return buildTimelineOption(data, { from: ctx.timeRange.from, to: ctx.timeRange.to });
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';

            if (model.prompt) {
                disposeChart();
                el.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                    'Select a session (PID) or query to view timeline</div>';
                return;
            }
            if (!model.hasData) {
                disposeChart();
                el.innerHTML = '<div style="padding:40px;text-align:center;color:#888">' +
                    'No events for selected session/range</div>';
                return;
            }

            let html = '';
            if (model.truncated) {
                html += '<div style="padding:8px 20px;font-size:12px;color:#ffd700;background:#3d3200;' +
                    'border-bottom:1px solid #555">Showing ' + model.count + ' of ' +
                    model.total_count + ' events. Zoom in for more detail.</div>';
            }
            html += '<div id="timeline-chart" style="height:' + model.chartHeight +
                'px;padding:10px 20px"></div>';
            el.innerHTML = html;

            const host = document.getElementById('timeline-chart');
            disposeChart();
            chart = ctx.echarts.init(host, 'dark');
            chart.setOption(model.option, true);
        },

        enter() { /* chart created in mount (its inner node lives there) */ },
        leave() { disposeChart(); },
        resize() { if (chart) chart.resize(); },
    };
}
