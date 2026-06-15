/* pgwt — "active" view: the Average Active Sessions chart.
 *
 * This is the persistent top chart (Oracle ASH-style AAS). It is the FIRST
 * migrated view per the B3 plan order ("active → overview → ..."): the AAS
 * chart moves from ApexCharts to ECharts here, with the custom drag-select
 * overlay (lib/selection.js) replacing ApexCharts' built-in zoom.
 *
 * Unlike the tab views, this one is persistent — it does not tear down on tab
 * switches (the AAS chart sits above the tabs and reflects the current window
 * for every tab). So app.js drives it directly via build()/mount() on each
 * refresh rather than through the tab-switching view-manager. It still follows
 * the { id, requests, build, mount, enter, leave } contract: it owns its
 * ECharts instance (created in enter, disposed in leave — no module globals)
 * and its builder (lib/builders/aas.js) is pure and Node-tested.
 */

import { buildAasOption } from '../lib/builders/aas.js';
import {
    SAMPLED_BAND_COLOR, MIXED_BAND_COLOR, SAMPLED_BORDER, MIXED_BORDER,
} from '../lib/builders/fidelity.js';
import { attachSelection } from '../lib/selection.js';
import { esc, fmtDuration } from '../lib/format.js';

export function createActiveView() {
    let chart = null;          // ECharts instance — owned here, nowhere else
    let detachSel = null;      // selection-overlay teardown
    let el = null;             // chart container
    let selected = null;       // Set of visible series indices (legend state)

    return {
        id: 'active',

        /* Fetch the AAS data for the current window + filters. Uses a
         * single-flight channel so a new refresh supersedes a pending one. */
        async requests(ctx) {
            const t = ctx.timeRange;
            const params = {
                from: t.from,
                to: t.to,
                buckets: Math.min(Math.floor((el ? el.clientWidth : 800) / 4), 300),
                filters: ctx.filters.snapshot(),
            };
            // Class drill-down (no specific event): break down by events.
            const f = ctx.filters.filters;
            if (f.class && !f.event_id) params.detail = 'events';
            return ctx.transport.request(ctx.channel('aas'), 'aas', params);
        },

        /* PURE: data -> ECharts option + legend metadata. Threads the current
         * window (for full-extent fidelity bands) and the daemon escalation
         * status (for the active-window annotation) through to the builder. */
        build(data, ctx) {
            return buildAasOption(data, {
                numCpus: ctx.server.numCpus,
                win: { from: ctx.timeRange.from, to: ctx.timeRange.to },
                escalationStatus: ctx.getEscalationStatus ? ctx.getEscalationStatus() : null,
            });
        },

        mount(_el, model, ctx) {
            if (!chart || !model.hasData) return;   // disposed / no data
            chart.setOption(model.option, true);
            renderLegend(model, chart, ctx, () => selected, (s) => { selected = s; });
            renderFidelityChip(model, ctx);

            // Status line: window + peak AAS (kept identical to old behavior).
            if (ctx.setStatus) {
                const dur = fmtDuration(ctx.timeRange.span());
                const peak = (model.maxAas || 0).toFixed(1);
                ctx.setStatus(ctx.server.numCpus + ' CPUs · ' + dur +
                    ' window · peak ' + peak + ' AAS', 'connected');
            }
        },

        enter(ctx) {
            el = ctx.chartEl;
            if (!el) return;
            chart = ctx.echarts.init(el, 'dark');
            selected = null;
            // Drag-select overlay → zoom the window.
            detachSel = attachSelection(el, chart, {
                onSelect: (range) => {
                    if (ctx.onZoom) ctx.onZoom(range.from, range.to);
                },
            });
        },

        leave() {
            if (detachSel) { detachSel(); detachSel = null; }
            if (chart) { chart.dispose(); chart = null; }
            const chip = document.getElementById('aas-fidelity-chip');
            if (chip) chip.remove();
            el = null;
            selected = null;
        },

        resize() { if (chart) chart.resize(); },
    };
}

/* Fidelity legend chip: explains the sampled/exact distinction next to the AAS
 * legend. Rendered into a small host inserted after the series legend so the
 * shading band on the chart is never unexplained. Hidden for fully-exact
 * windows (the common case) to avoid noise. */
function renderFidelityChip(model, ctx) {
    const host = ctx.chartEl;
    if (!host || !host.parentNode) return;

    let chip = document.getElementById('aas-fidelity-chip');
    const fid = model.fidelity;
    const showBand = model.shading && model.shading.showLegend;

    if (!showBand) {
        if (chip) chip.remove();
        return;
    }
    if (!chip) {
        chip = document.createElement('div');
        chip.id = 'aas-fidelity-chip';
        chip.style.cssText =
            'display:flex;flex-wrap:wrap;gap:10px;justify-content:center;' +
            'padding:0 20px 8px;background:#1e1e3a;font-size:11px;color:#aaa;';
        const legDiv = document.getElementById('aas-legend');
        if (legDiv && legDiv.parentNode) legDiv.parentNode.insertBefore(chip, legDiv.nextSibling);
        else host.parentNode.insertBefore(chip, host.nextSibling);
    }

    const sampledSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + SAMPLED_BAND_COLOR + ';border:1px dashed ' + SAMPLED_BORDER + '"></span>';
    const mixedSwatch =
        '<span style="width:14px;height:10px;display:inline-block;border-radius:2px;' +
        'background:' + MIXED_BAND_COLOR + ';border:1px dashed ' + MIXED_BORDER + '"></span>';

    let html = '<span>' + esc(model.fidelityLabel) + '</span>';
    if (fid === 'sampled') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'shaded = sampled (estimated, not exact)</span>';
    } else if (fid === 'mixed') {
        html += '<span style="display:inline-flex;align-items:center;gap:5px">' +
            sampledSwatch + 'sampled</span>' +
            '<span style="display:inline-flex;align-items:center;gap:5px">' +
            mixedSwatch + 'mixed window</span>';
    }
    if (model.escalation) {
        html += '<span style="color:' +
            (model.escalation.isAnomaly ? '#E53935' : '#4fc3f7') + '">● ' +
            esc(model.escalation.label) + '</span>';
    }
    chip.innerHTML = html;
}

/* External HTML legend with solo / multi-select / hover — kept out of the chart
 * for full control (matches the old ApexCharts custom legend behavior). Toggles
 * series visibility via ECharts' legend.select API on the chart instance. */
function renderLegend(model, chart, ctx, getSelected, setSelected) {
    const names = model.seriesNames;
    const colors = model.seriesColors;
    const host = ctx.chartEl;
    if (!host) return;

    let sel = getSelected();
    if (!sel) { sel = new Set(names.map((_, i) => i)); setSelected(sel); }

    let legDiv = document.getElementById('aas-legend');
    if (!legDiv) {
        legDiv = document.createElement('div');
        legDiv.id = 'aas-legend';
        legDiv.style.cssText = 'display:flex;flex-wrap:wrap;gap:4px;justify-content:center;padding:8px 20px;background:#1e1e3a;';
        host.parentNode.insertBefore(legDiv, host.nextSibling);
    }

    legDiv.innerHTML = names.map((name, i) =>
        `<span class="aleg" data-i="${i}" style="display:inline-flex;align-items:center;gap:4px;` +
        `cursor:pointer;padding:3px 10px;border-radius:4px;font-size:11px;color:#ccc;` +
        `border:1px solid ${colors[i]};border-left:3px solid ${colors[i]};` +
        `user-select:none;transition:opacity 0.1s">` +
        `<span style="width:8px;height:8px;border-radius:2px;background:${colors[i]}"></span>` +
        `${esc(name)}</span>`
    ).join('');

    function apply(set) {
        names.forEach((n, i) => chart.dispatchAction({
            type: set.has(i) ? 'legendSelect' : 'legendUnSelect', name: n,
        }));
        legDiv.querySelectorAll('.aleg').forEach(elx => {
            elx.style.opacity = set.has(+elx.dataset.i) ? '1' : '0.3';
        });
    }

    function showOnly(idx) {
        names.forEach((n, i) => chart.dispatchAction({
            type: i === idx ? 'legendSelect' : 'legendUnSelect', name: n,
        }));
    }

    legDiv.querySelectorAll('.aleg').forEach(elx => {
        const idx = +elx.dataset.i;
        elx.addEventListener('mouseenter', () => showOnly(idx));
        elx.addEventListener('mouseleave', () => apply(getSelected()));
        elx.addEventListener('click', (e) => {
            let set = getSelected();
            if (e.metaKey || e.ctrlKey) {
                if (set.has(idx)) { if (set.size > 1) set.delete(idx); }
                else set.add(idx);
            } else {
                if (set.size === 1 && set.has(idx)) set = new Set(names.map((_, i) => i));
                else set = new Set([idx]);
            }
            setSelected(set);
            apply(set);
        });
    });

    apply(sel);
}
