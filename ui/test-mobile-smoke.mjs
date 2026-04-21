/**
 * Mobile UI smoke test (iPhone viewport):
 *   - Library FAB visible, opens browser
 *   - Overflow (⋯) menu visible, opens menu
 *   - Config opens via overflow menu (bottom sheet)
 *   - Touch tap on canvas adds a vertex
 *   - Two-finger pinch zooms
 * Run from ui/: node test-mobile-smoke.mjs (with python3 -m http.server 8089 running)
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium, devices } = pw;

const URL = 'http://localhost:8089/index.html';
const fail = (msg) => { console.error('FAIL:', msg); process.exitCode = 1; };
const pass = (msg) => console.log('PASS:', msg);

async function main() {
  const browser = await chromium.launch({ headless: true });
  const ctx = await browser.newContext({
    ...devices['iPhone 13'],
    hasTouch: true,
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', err => errors.push(err.message));
  page.on('console', msg => { if (msg.type() === 'error') errors.push(`console.error: ${msg.text()}`); });
  page.on('response', resp => { if (resp.status() === 404) errors.push(`404: ${resp.url()}`); });

  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(500);

  // 1. Library FAB visible
  const fab = await page.$('#library-fab');
  if (!fab) return fail('Library FAB not found in DOM');
  const fabVisible = await fab.isVisible();
  if (!fabVisible) return fail('Library FAB not visible at iPhone viewport');
  pass('Library FAB visible');

  // Tap library FAB → opens browser overlay
  await fab.tap();
  await page.waitForTimeout(400);
  const browserOpen = await page.evaluate(() => {
    const ov = document.getElementById('browser-overlay');
    return ov && ov.classList.contains('visible');
  });
  if (!browserOpen) return fail('Library overlay did not open from FAB');
  pass('Library overlay opens from FAB');
  // Close
  await page.tap('#browser-close');
  await page.waitForTimeout(300);

  // 2. Overflow menu
  const ovBtn = await page.$('#overflow-btn');
  if (!ovBtn || !await ovBtn.isVisible()) return fail('Overflow ⋯ button not visible');
  pass('Overflow ⋯ button visible');

  await ovBtn.tap();
  await page.waitForTimeout(200);
  const menuOpen = await page.evaluate(() => document.getElementById('overflow-dropdown').classList.contains('open'));
  if (!menuOpen) return fail('Overflow menu did not open on tap');
  pass('Overflow menu opens on tap');

  // 3. Tap "Configure" → opens bottom sheet
  await page.tap('[data-action="config"]');
  await page.waitForTimeout(400);
  const cfgOpen = await page.evaluate(() => document.getElementById('config-panel').classList.contains('open'));
  if (!cfgOpen) return fail('Config panel did not open from overflow menu');
  pass('Config bottom sheet opens from overflow → Configure');

  // Verify the panel is positioned as a bottom sheet (top > screen midline)
  const cfgBox = await page.evaluate(() => {
    const el = document.getElementById('config-panel');
    const r = el.getBoundingClientRect();
    return { top: r.top, bottom: r.bottom, height: r.height, viewportH: window.innerHeight };
  });
  if (cfgBox.top < cfgBox.viewportH * 0.1) return fail(`Config panel not bottom-sheet: top=${cfgBox.top}, viewportH=${cfgBox.viewportH}`);
  if (Math.abs(cfgBox.bottom - cfgBox.viewportH) > 1) return fail(`Config panel does not reach bottom: bottom=${cfgBox.bottom}, viewportH=${cfgBox.viewportH}`);
  pass(`Config panel is a bottom sheet (top=${cfgBox.top.toFixed(0)}, viewportH=${cfgBox.viewportH})`);

  // Tap the close (X) on the sheet
  await page.tap('#config-sheet-close');
  await page.waitForTimeout(400);
  const cfgClosed = await page.evaluate(() => !document.getElementById('config-panel').classList.contains('open'));
  if (!cfgClosed) return fail('Config sheet did not close via X button');
  pass('Config sheet closes via X');

  // 4. Touch-draw a vertex on the canvas
  const before = await page.evaluate(() => window.state ? window.state.vertices.length : -1);
  // window.state may not be exposed; check via SVG vertex count instead.
  const beforeVerts = await page.$$eval('#vertex-layer > *', els => els.length);
  // Tap somewhere in the middle of the canvas
  const cBox = await page.evaluate(() => {
    const c = document.getElementById('draw-canvas');
    const r = c.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  });
  await page.touchscreen.tap(cBox.x, cBox.y);
  await page.waitForTimeout(300);
  const afterVerts = await page.$$eval('#vertex-layer > *', els => els.length);
  if (afterVerts <= beforeVerts) return fail(`Touch tap did not add a vertex: before=${beforeVerts}, after=${afterVerts}`);
  pass(`Touch tap added vertex (count ${beforeVerts} → ${afterVerts})`);

  // 5. Two-finger pinch zoom
  const initialZoom = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  // Synthesize a pinch via Page.dispatchEvent. Playwright doesn't have a built-in
  // multi-touch helper; we use CDPSession to dispatch raw touch events.
  const client = await page.context().newCDPSession(page);
  const cx = cBox.x, cy = cBox.y;
  // Two pointers, 100px apart, then move to 200px apart (zoom in)
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [
      { x: cx - 50, y: cy, id: 1 },
      { x: cx + 50, y: cy, id: 2 },
    ],
  });
  await page.waitForTimeout(80);
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchMove',
    touchPoints: [
      { x: cx - 100, y: cy, id: 1 },
      { x: cx + 100, y: cy, id: 2 },
    ],
  });
  await page.waitForTimeout(80);
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchEnd',
    touchPoints: [],
  });
  await page.waitForTimeout(200);
  const afterZoom = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  if (!(afterZoom > initialZoom + 5)) return fail(`Pinch did not increase zoom: ${initialZoom}% → ${afterZoom}%`);
  pass(`Pinch zoomed in (${initialZoom}% → ${afterZoom}%)`);

  // 6. Console errors check
  if (errors.length) {
    console.error('JS errors collected:');
    errors.forEach(e => console.error('  ' + e));
    fail('JS errors during test');
  } else {
    pass('No JS errors');
  }

  await browser.close();
}

main().catch(e => { console.error(e); process.exit(2); });
