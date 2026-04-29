#!/usr/bin/env node
// record-web.mjs — Headed Playwright recording of the pgwt web UI.
//
// Drives the demo flow: drag-zoom the AAS chart into the workload burst →
// Events → Sessions → Queries → Transitions (with simplify slider at 0%).
// Records video; ffmpeg post-processes to gif.
//
// Prerequisites:
//   npm install  (in demos/ — installs Playwright)
//   ffmpeg on PATH
//   pgwt server already serving on http://localhost:8384/  (see record-web.sh)
//
// Output: demos/web.gif

import { chromium } from 'playwright';
import { spawnSync } from 'node:child_process';
import { mkdirSync, rmSync, readdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const URL = process.env.PGWT_URL || 'http://localhost:8384/';
const VIDEO_DIR = join(HERE, '.video');
const OUTPUT = join(HERE, 'web.gif');

rmSync(VIDEO_DIR, { recursive: true, force: true });
mkdirSync(VIDEO_DIR, { recursive: true });

const browser = await chromium.launch({ headless: false });
const context = await browser.newContext({
    viewport: { width: 1500, height: 810 },
    deviceScaleFactor: 1,
    recordVideo: { dir: VIDEO_DIR, size: { width: 1500, height: 810 } },
});
const page = await context.newPage();

console.log(`==> Loading ${URL}`);
await page.goto(URL);
await page.waitForSelector('.echarts-for-react, canvas, #aas-chart, [class*="aas"]', { timeout: 10000 }).catch(() => {});
await page.waitForTimeout(2500);  // chart render

console.log('==> Drag-zoom into the workload burst');
// ECharts brush-zoom on the AAS chart. The chart sits roughly y=70..360.
// Burst is centered around the middle on a 15-minute view.
await page.mouse.move(580, 250);
await page.mouse.down();
await page.mouse.move(770, 250, { steps: 12 });
await page.mouse.up();
await page.waitForTimeout(1500);

const tabs = ['Events', 'Sessions', 'Queries', 'Transitions'];
for (const tab of tabs) {
    console.log(`==> Click tab: ${tab}`);
    await page.getByRole('tab', { name: tab }).or(page.getByText(tab, { exact: true })).first().click();
    await page.waitForTimeout(1500);
    if (tab === 'Transitions') {
        // Drag simplify slider from ~20% down to ~0% to reveal full graph
        await page.waitForTimeout(500);
        const slider = await page.$('input[type="range"]');
        if (slider) {
            const box = await slider.boundingBox();
            if (box) {
                await page.mouse.move(box.x + box.width * 0.2, box.y + box.height / 2);
                await page.mouse.down();
                await page.mouse.move(box.x + 4, box.y + box.height / 2, { steps: 8 });
                await page.mouse.up();
                await page.waitForTimeout(1200);
                // Scroll slightly to bring graph into view
                await page.mouse.wheel(0, 200);
                await page.waitForTimeout(1200);
            }
        }
    }
}

await page.waitForTimeout(1000);
console.log('==> Closing browser ...');
await context.close();
await browser.close();

// Find the video file (Playwright names it with a hash)
const files = readdirSync(VIDEO_DIR).filter(f => f.endsWith('.webm'));
if (!files.length) {
    console.error('FATAL: no video file produced');
    process.exit(1);
}
const video = join(VIDEO_DIR, files[0]);

console.log(`==> Converting ${video} -> ${OUTPUT}`);
// Two-pass palette for compact, high-quality gif
const PALETTE = join(VIDEO_DIR, 'palette.png');
const fps = process.env.GIF_FPS || '8';
const r1 = spawnSync('ffmpeg', ['-y', '-i', video, '-vf',
    `fps=${fps},scale=1500:-1:flags=lanczos,palettegen=stats_mode=diff`,
    PALETTE], { stdio: 'inherit' });
if (r1.status !== 0) process.exit(r1.status || 1);

const r2 = spawnSync('ffmpeg', ['-y', '-i', video, '-i', PALETTE, '-lavfi',
    `fps=${fps},scale=1500:-1:flags=lanczos [x]; [x][1:v] paletteuse=dither=bayer:bayer_scale=4`,
    OUTPUT], { stdio: 'inherit' });
if (r2.status !== 0) process.exit(r2.status || 1);

console.log(`==> ${OUTPUT} ready`);
