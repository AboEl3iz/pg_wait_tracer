/* pgwt — "transitions" view: the directly-follows graph (DFG) + flow variants.
 *
 * Migrated to the { id, requests, build, mount, enter, leave } contract (B3
 * part 3 — the heaviest view, done last). requests() fetches transitions and
 * (optionally) variants on single-flight channels; build() is PURE
 * (lib/builders/transitions.js -> ECharts graph option for the default
 * threshold + the variants HTML); mount() builds the slider + DFG container +
 * variants section, feeds the option to the view-owned ECharts instance, and
 * wires the slider to re-render at a new threshold. The view OWNS its ECharts
 * instance: created in mount (its inner #dfg-container node is built there),
 * disposed in leave() — no module-level chart global.
 *
 * Behavior is identical to the old legacy adapter in app.js:
 *   - No links → "No transitions found".
 *   - Simplify slider (0-100%, default 20) hides edges below that fraction of
 *     the busiest edge and re-lays-out the graph.
 *   - Variants ("Execution"/"Planning" flow patterns) render below the DFG.
 */

import {
    buildTransitionsOption, buildVariantsHtml,
} from '../lib/builders/transitions.js';
import { esc } from '../lib/format.js';

const DEFAULT_THRESHOLD = 20;

export function createTransitionsView() {
    let chart = null;     // ECharts instance — owned here, nowhere else
    let dataRef = null;   // current transitions payload (for slider re-render)

    function disposeChart() { if (chart) { chart.dispose(); chart = null; } }

    function dfgDims() {
        const el = document.getElementById('dfg-container');
        return { width: (el && el.clientWidth) || 800, height: (el && el.clientHeight) || 550 };
    }

    function renderDFG(ctx, threshold) {
        const el = document.getElementById('dfg-container');
        if (!el) return;
        const { option, visibleCount } = buildTransitionsOption(dataRef, threshold, dfgDims());
        if (visibleCount === 0) {
            disposeChart();
            el.innerHTML =
                '<p style="color:#666;padding:40px;text-align:center">No transitions above threshold</p>';
            return;
        }
        disposeChart();
        chart = ctx.echarts.init(el, 'dark');
        chart.setOption(option, true);
    }

    return {
        id: 'transitions',

        async requests(ctx) {
            const data = await ctx.transport.request(ctx.channel('table'), 'transitions', {
                from: ctx.timeRange.from, to: ctx.timeRange.to,
                filters: ctx.filters.snapshot(), num_buckets: 200,
            });
            let variants = null;
            try {
                variants = await ctx.transport.request(ctx.channel('variants'), 'variants', {
                    from: ctx.timeRange.from, to: ctx.timeRange.to,
                    filters: ctx.filters.snapshot(), num_buckets: 20,
                });
            } catch (e) { /* variants optional */ }
            return { transitions: data, variants };
        },

        build(data) {
            const t = data.transitions;
            const hasLinks = !!(t && t.links && t.links.length > 0);
            return {
                transitions: t,
                hasLinks,
                total: (t && t.total) || 0,
                variantsHtml: data.variants ? buildVariantsHtml(data.variants, esc) : '',
            };
        },

        mount(el, model, ctx) {
            if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';
            disposeChart();

            if (!model.hasLinks) {
                el.innerHTML = '<p style="color:#888;padding:20px">No transitions found</p>';
                return;
            }
            dataRef = model.transitions;

            el.innerHTML =
                '<div style="padding:10px 20px;display:flex;align-items:center;gap:12px">' +
                    '<span style="color:#888;font-size:12px">Simplify:</span>' +
                    '<input type="range" id="dfg-slider" min="0" max="100" value="' +
                        DEFAULT_THRESHOLD + '" style="width:200px;accent-color:#4fc3f7">' +
                    '<span id="dfg-slider-val" style="color:#888;font-size:12px">' +
                        DEFAULT_THRESHOLD + '%</span>' +
                    '<span style="color:#666;font-size:11px;margin-left:8px">' +
                        Number(model.total).toLocaleString() + ' transitions</span>' +
                '</div>' +
                '<div id="dfg-container" style="width:100%;height:550px;background:#1a1a2e"></div>';

            renderDFG(ctx, DEFAULT_THRESHOLD);

            const slider = document.getElementById('dfg-slider');
            const sliderVal = document.getElementById('dfg-slider-val');
            slider.addEventListener('input', () => {
                sliderVal.textContent = slider.value + '%';
                renderDFG(ctx, +slider.value);
            });

            if (model.variantsHtml) {
                const section = document.createElement('div');
                section.innerHTML = model.variantsHtml;
                el.appendChild(section);
            }
        },

        enter() { /* chart created in mount (its inner node lives there) */ },
        leave() { disposeChart(); dataRef = null; },
        resize() { if (chart) chart.resize(); },
    };
}
