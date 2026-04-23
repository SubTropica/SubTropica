/**
 * Mobile onboarding-tour smoke test (iPhone 13 viewport).
 *
 * Walks the full onboarding tour on ≤480 px and asserts that each step's
 * spotlight lands on a visible element inside the viewport — the original
 * desktop tour pointed at `#nickel-readout`, `#export-menu-btn`,
 * `#browse-btn`, `#notif-stack .notif-toast.notif-config`, all of which
 * are hidden / non-existent on mobile. The mobile overrides in
 * TOUR_STEPS[i].mobile replace the selectors with elements that actually
 * live on phones (#overflow-btn, the tab-bar tabs, #notif-chip, etc.).
 *
 * Run from ui/: node test-mobile-tour.mjs (with python3 -m http.server 8089).
 */
const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium, devices } = pw;
const URL = 'http://localhost:8089/index.html';

const fail = (m) => { console.error('FAIL:', m); process.exitCode = 1; };
const pass = (m) => console.log('PASS:', m);

async function main() {
  const browser = await chromium.launch({ headless: true });
  const ctx = await browser.newContext({ ...devices['iPhone 13'], hasTouch: true });
  const page = await ctx.newPage();
  const jsErrs = [];
  page.on('pageerror', e => jsErrs.push('pageerror: ' + e.message));
  page.on('console', m => { if (m.type() === 'error' && !/api\/(library|ping)/.test(m.text())) jsErrs.push('console.error: ' + m.text()); });

  // Make sure the tour fires on load (default behaviour for first visit).
  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1500);  // tour kicks off 1200ms after load

  // Tour card visible?
  const card = await page.$('.tour-card');
  if (!card) return fail('Tour card did not appear on first visit');
  const visible = await card.isVisible();
  if (!visible) return fail('Tour card in DOM but not visible');
  pass('Tour auto-starts on first mobile visit');

  // Walk through each step, asserting the spotlight target is inside the
  // viewport AND visible. Mobile-specific steps skip cleanly via `mobile.skip`
  // so we only see steps that have a real mobile target.
  const totalSteps = await page.evaluate(() => {
    // TOUR_STEPS is a const inside the module; count the "N / M" progress.
    const el = document.querySelector('.tour-card-progress');
    if (!el) return 0;
    const m = /\/\s*(\d+)/.exec(el.textContent || '');
    return m ? parseInt(m[1], 10) : 0;
  });
  if (!totalSteps) return fail('Could not read tour progress total');
  pass(`Tour length reported: ${totalSteps} steps`);

  const nextBtn = '.tour-next';
  const vw = 390, vh = 664;
  let stepsSeen = 0;
  const maxSteps = 25;  // safety bound in case Finish isn't reached
  for (let i = 0; i < maxSteps; i++) {
    // Read current step's progress index + title
    const meta = await page.evaluate(() => {
      const title = document.querySelector('.tour-card-title')?.textContent || '';
      const prog  = document.querySelector('.tour-card-progress')?.textContent || '';
      const nextText = document.querySelector('.tour-next')?.textContent || '';
      const spot = document.querySelector('.tour-spot');
      let spotRect = null;
      if (spot && getComputedStyle(spot).visibility !== 'hidden' && spot.classList.contains('visible')) {
        const r = spot.getBoundingClientRect();
        spotRect = { top: r.top, left: r.left, width: r.width, height: r.height };
      }
      return { title: title.trim(), prog: prog.trim(), nextText: nextText.trim(), spotRect };
    });
    stepsSeen++;
    if (meta.spotRect) {
      const r = meta.spotRect;
      const inside = r.top >= -8 && r.left >= -8 && r.top + r.height <= vh + 8 && r.left + r.width <= vw + 8;
      if (!inside) {
        return fail(`Step "${meta.title}" (${meta.prog}): spotlight outside viewport — rect=${JSON.stringify(r)}`);
      }
      pass(`Step "${meta.title}" (${meta.prog}): spot in viewport [${r.left.toFixed(0)}, ${r.top.toFixed(0)}, ${r.width.toFixed(0)}×${r.height.toFixed(0)}]`);
    } else {
      pass(`Step "${meta.title}" (${meta.prog}): centered (no spotlight)`);
    }
    if (/finish/i.test(meta.nextText)) {
      // Final step — advance once more to close, then exit the loop.
      await page.tap(nextBtn);
      await page.waitForTimeout(400);
      break;
    }
    await page.tap(nextBtn);
    // Generous wait for prepare() (some steps animate the canvas / open the
    // library overlay).
    await page.waitForTimeout(1200);
  }

  if (stepsSeen === 0) return fail('Walked zero steps');
  if (stepsSeen >= maxSteps) return fail('Tour did not reach Finish within ' + maxSteps + ' steps');
  pass(`Walked ${stepsSeen} steps to Finish`);

  // Tour should be closed now
  const tourOpen = await page.evaluate(() => {
    const ov = document.querySelector('.tour-overlay');
    return ov && ov.classList.contains('visible');
  });
  if (tourOpen) return fail('Tour overlay still open after Finish');
  pass('Tour closes on Finish');

  if (jsErrs.length) {
    console.error('JS errors during tour:');
    jsErrs.forEach(e => console.error('  ' + e));
    fail('JS errors during tour walk');
  } else {
    pass('No JS errors during tour');
  }

  await browser.close();
}

main().catch(e => { console.error(e); process.exit(2); });
