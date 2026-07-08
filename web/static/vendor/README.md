# Vendored third-party assets

## echarts.min.js

| | |
|---|---|
| Library | [Apache ECharts](https://echarts.apache.org/) |
| Version | **5.5.1** (bundles zrender 5.6.0; embedded `version="5.5.1"` string) |
| Source | https://cdn.jsdelivr.net/npm/echarts@5.5.1/dist/echarts.min.js (npm package `echarts@5.5.1`, file `dist/echarts.min.js`) — the committed file is **byte-identical** to that URL (verified 2026-07-07) |
| License | Apache-2.0 |
| SHA-256 | `e84270bd0cd5bdf60fefc26d00c2a391cb2e81f4d26a7a9ee16185a54773a3cf` |

Vendored (rather than loaded from a CDN) deliberately: deterministic builds,
airgapped-friendly `go:embed` packaging, stable visual snapshots, and no
supply-chain exposure from a floating CDN tag. See the comment in
`web/static/index.html`.

Verify the committed file at any time:

```sh
sha256sum web/static/vendor/echarts.min.js
# must print e84270bd0cd5bdf60fefc26d00c2a391cb2e81f4d26a7a9ee16185a54773a3cf
```

## Update procedure

1. Download the new pinned version (never "latest"):
   `curl -LO https://cdn.jsdelivr.net/npm/echarts@<X.Y.Z>/dist/echarts.min.js`
2. Cross-check the bytes against a second source (e.g. `npm pack echarts@<X.Y.Z>`
   and compare `package/dist/echarts.min.js`) before committing.
3. Replace `web/static/vendor/echarts.min.js`, update the version and SHA-256
   in **this file** in the same commit.
4. Run the local suites: `node --test 'tests/web_unit/*.test.mjs'`,
   `python3 tests/test_web_ui.py`, `python3 tests/test_web_ui_chaos.py`.
5. **Rebaseline the visual snapshots** — an ECharts upgrade almost always
   moves pixels. Baselines are generated only in CI: trigger the `CI`
   workflow via `workflow_dispatch` with `update_snapshots: true`, download
   the `snapshot-baselines` artifact, commit the PNGs under
   `tests/web_snapshots/`, and push them with the upgrade PR (see
   `.github/workflows/ci.yml` and `tests/web_snapshots/README.md`).
6. Note the upgrade + new hash in the PR body.
