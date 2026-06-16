/* pgwt — thin DOM mount layer for the fidelity-aware panels (Phase B5).
 *
 * The models come from lib/builders/fidelity.js (pure, Node-tested); this file
 * is the ONLY place that touches the DOM for them. Three panels:
 *
 *   - mountUnavailablePanel: the "no full-fidelity data in this window —
 *     escalate to capture" state for EXACT-only views over sampled windows.
 *   - mountEscalateControl: the header "Escalate 60s" button + budget/window
 *     readout, with a de-escalate affordance while active.
 *   - mountMetricsPanel: the collapsible daemon self-metrics panel.
 *
 * All three share an escalate() action wired through lib/control.js. Escalation
 * is operable from the browser over the same WebSocket (pgwt-server proxies the
 * daemon control socket as the `control` command).
 */

import {
    buildUnavailablePanel, buildEscalateControl, buildMetricsPanel,
    buildEscalateResult,
} from './builders/fidelity.js';
import {
    controlEscalate, controlDeescalate, ControlUnavailable,
} from './control.js';
import { esc } from './format.js';

/* Render the unavailable/escalate panel into `el` for an EXACT-only view whose
 * response was {"unavailable": ...}. `data` is that response; ctx supplies the
 * transport (for escalate) + escalation status + a refresh hook. */
export function mountUnavailablePanel(el, data, ctx) {
    const status = ctx.getEscalationStatus ? ctx.getEscalationStatus() : null;
    const model = buildUnavailablePanel(data, {
        escalationSupported: status ? status.escalation_supported : true,
    });
    if (ctx.summaryEl) ctx.summaryEl.innerHTML = '';

    el.innerHTML =
        '<div class="unavailable-panel">' +
        '  <div class="unavailable-icon">◐</div>' +
        '  <div class="unavailable-title">' + esc(model.title) + '</div>' +
        '  <div class="unavailable-hint">' + esc(model.hint) + '</div>' +
        (model.canEscalate
            ? '  <button class="escalate-btn" id="unavail-escalate">Escalate 60s</button>' +
              '  <div class="unavailable-status" id="unavail-status"></div>'
            : '') +
        '</div>';

    if (model.canEscalate) {
        const btn = el.querySelector('#unavail-escalate');
        const statusEl = el.querySelector('#unavail-status');
        btn.addEventListener('click', async () => {
            await doEscalate(ctx, btn, statusEl, /*refreshAfter=*/true);
        });
    }
}

/* Shared escalate action: disables the button, calls control, shows the result,
 * refreshes the daemon status, and (optionally) re-runs the current view so an
 * exact-only view repaints once data is being captured. */
async function doEscalate(ctx, btn, statusEl, refreshAfter) {
    if (btn) { btn.disabled = true; btn.textContent = 'Escalating…'; }
    try {
        const reply = await controlEscalate(ctx.transport, 60, 'manual');
        const res = buildEscalateResult(reply);
        if (statusEl) {
            statusEl.textContent = res.text;
            statusEl.className = 'unavailable-status ' + (res.ok ? 'ok' : 'err');
        }
        if (ctx.refreshEscalationStatus) await ctx.refreshEscalationStatus();
        // Re-render the escalate header control if present.
        if (ctx.onEscalationChanged) ctx.onEscalationChanged();
        // Give the daemon a beat to start writing transition blocks, then
        // refresh the view so it can repaint with exact data.
        if (refreshAfter && ctx.refresh) {
            setTimeout(() => { try { ctx.refresh(); } catch (e) { /* best-effort */ } }, 800);
        }
    } catch (e) {
        if (statusEl) {
            statusEl.className = 'unavailable-status err';
            statusEl.textContent = (e instanceof ControlUnavailable)
                ? 'Escalation unavailable: ' + e.message
                : 'Escalation failed: ' + (e && e.message);
        }
    } finally {
        if (btn) { btn.disabled = false; btn.textContent = 'Escalate 60s'; }
    }
}

/* Render the header escalate control into `el`. Reads the daemon status from
 * ctx (getEscalationStatus) and re-renders itself after each action. */
export function mountEscalateControl(el, ctx) {
    if (!el) return;
    const status = ctx.getEscalationStatus ? ctx.getEscalationStatus() : null;
    const model = buildEscalateControl(status || {});

    if (!model.supported) {
        // Not tiered (or no daemon): hide the control entirely.
        el.innerHTML = '';
        el.style.display = 'none';
        return;
    }
    el.style.display = '';

    const cls = model.escalated ? 'escalate-btn active' : 'escalate-btn';
    el.innerHTML =
        '<button class="' + cls + '" id="hdr-escalate"' +
            (model.canEscalate || model.escalated ? '' : ' disabled') + '>' +
            esc(model.buttonLabel) + '</button>' +
        (model.canDeescalate
            ? '<button class="deescalate-btn" id="hdr-deescalate" title="Stop full-fidelity capture">Stop</button>'
            : '') +
        '<span class="escalate-budget" title="Full-fidelity seconds left this hour">' +
            esc(model.budgetText) + '</span>';

    const escBtn = el.querySelector('#hdr-escalate');
    if (escBtn && !model.escalated) {
        escBtn.addEventListener('click', async () => {
            await doEscalate(ctx, escBtn, null, /*refreshAfter=*/true);
            mountEscalateControl(el, ctx);   // re-render with new status
        });
    }
    const deBtn = el.querySelector('#hdr-deescalate');
    if (deBtn) {
        deBtn.addEventListener('click', async () => {
            deBtn.disabled = true;
            try {
                await controlDeescalate(ctx.transport);
                if (ctx.refreshEscalationStatus) await ctx.refreshEscalationStatus();
                if (ctx.refresh) ctx.refresh();
            } catch (e) { /* best-effort; status poll will correct */ }
            mountEscalateControl(el, ctx);
        });
    }
}

/* Render the daemon self-metrics panel into `el`. `metrics` is the control
 * `metrics` reply; `status` the `status` reply. */
export function mountMetricsPanel(el, metrics, status) {
    if (!el) return;
    if (!metrics) { el.innerHTML = ''; el.style.display = 'none'; return; }
    el.style.display = '';
    const model = buildMetricsPanel(metrics, status);

    let rows = '';
    for (const r of model.rows) {
        rows +=
            '<div class="dm-row' + (r.warn ? ' warn' : '') + '"' +
                (r.hint ? ' title="' + esc(r.hint) + '"' : '') + '>' +
            '<span class="dm-label">' + esc(r.label) + '</span>' +
            '<span class="dm-value">' + esc(String(r.value)) + '</span>' +
            '</div>';
    }
    el.innerHTML =
        '<div class="daemon-metrics-title">Daemon</div>' +
        '<div class="daemon-metrics-grid">' + rows + '</div>';
}
