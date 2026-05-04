# LLM prompts for re-driving the demos

The scripted Playwright path (`record-web.sh`) is the canonical way to
reproduce `web.gif`. This file documents the alternative path used to
*originally* author it: an LLM agent driving Chrome via an MCP browser tool
(in this case, [Claude Code](https://claude.com/claude-code) with the
`claude-in-chrome` MCP server).

Use this when you want to record a *new* flow that doesn't match the existing
script — let the model pick coordinates from a screenshot rather than
maintaining brittle selectors.

## Workflow

1. Start `pgwt` locally serving the trace (see `record-web.sh` for the
   wrapper invocation that boots `pgwt` against `demos/local-trace`).
2. Open Chrome to `http://localhost:8384/` and resize to ~1500×900.
3. Hand the model the prompt below.

## Prompt

```
You're going to record a short demo of the pgwt web UI at
http://localhost:8384/. The page is already loaded.

Tools you have:
- mcp__claude-in-chrome__tabs_context_mcp — confirm the tab
- mcp__claude-in-chrome__computer with action=screenshot/left_click/
  left_click_drag/wait/scroll
- mcp__claude-in-chrome__gif_creator with action=start_recording/
  stop_recording/export

Demo flow (~20 seconds):
1. Take a screenshot to confirm the page is rendered (AAS chart visible)
2. Start recording (gif_creator start_recording)
3. Take a screenshot (this becomes the first frame)
4. Drag-zoom the AAS chart into the workload burst — drag from roughly
   (580, 250) to (770, 250). Wait 1.5s. Screenshot.
5. Click the Events tab. Wait 1s. Screenshot.
6. Click the Sessions tab. Wait 1s. Screenshot.
7. Click the Queries tab. Wait 1s. Screenshot.
8. Click the Transitions tab. Wait 1s. Screenshot.
9. Drag the "Simplify" slider from ~20% to 0% (full graph). Wait 1s.
10. Scroll down to bring the transition graph into view. Screenshot.
11. Stop recording.
12. Export with: showClickIndicators=true, showActionLabels=false,
    showProgressBar=false, showWatermark=false, quality=10, download=true.
13. Move ~/Downloads/recording-*.gif to demos/web.gif.

Notes:
- Tab x-coordinates depend on the chart layout. Take a screenshot first
  and read the actual coordinates from there.
- Don't trigger any browser dialogs.
```

## Why keep the LLM path

- New views / new tabs land in the UI faster than scripts get updated.
- For one-off recordings (a bug repro, a feature pitch), authoring a
  Playwright script is overkill.
- The model can adapt to layout shifts that would break a hard-coded
  Playwright script.

The Playwright script is still the authoritative tool for CI / regenerating
the canonical demo gif on a schedule.
