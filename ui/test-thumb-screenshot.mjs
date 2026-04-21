// Smoke test for Q2: open the integration modal on a loaded diagram,
// take a screenshot of just the diagram preview, and assert the rendered
// SVG contains momentum labels (p₁, p₂, …).

import { chromium } from 'playwright';

const BASE = process.env.BASE || 'http://127.0.0.1:61147';

async function main() {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
  page.on('pageerror', e => console.log('[pageerror]', e.message));

  await page.goto(BASE + '/?test=1', { waitUntil: 'domcontentloaded' });
  await page.waitForFunction(() => window.__stTest && window.__stTest.library, { timeout: 20000 });

  // Load bubble with one massive internal edge so there's a mass label
  // (entries like 011|0| have one internal edge with a mass scale)
  const topoKey = 'e11|e|';
  const configKey = '011|0|';
  await page.evaluate(({k, c}) => window.__stTest.loadFromNickel(k, c), { k: topoKey, c: configKey });
  await page.waitForTimeout(500);

  await page.evaluate(() => {
    const inp = document.getElementById('ic-name');
    if (inp) inp.value = 'thumb-test';
  });

  // Open integration modal via doIntegrate
  await page.evaluate(() => window.__stTest.doIntegrate());
  // Give the panel a second to render
  await page.waitForTimeout(800);

  // Inspect the thumbnail container
  const result = await page.evaluate(() => {
    const diag = document.getElementById('integ-diagram');
    if (!diag) return { error: 'no integ-diagram' };
    const svg = diag.querySelector('svg');
    if (!svg) return { error: 'no svg in integ-diagram', html: diag.innerHTML.slice(0, 300) };
    const texts = Array.from(svg.querySelectorAll('text')).map(t => t.textContent);
    const hasLegLabel = texts.some(t => /^p.$/u.test(t) || /^p[\u2080-\u2089]$/u.test(t));
    const hasMassLabel = texts.some(t => t === 'm' || t === 'M' || /^m[\u2080-\u2089]$/u.test(t) || /^M[\u2080-\u2089]$/u.test(t));
    return {
      svgPresent: true,
      textNodes: texts,
      hasLegLabel,
      hasMassLabel,
      nEdges: svg.querySelectorAll('line, path').length,
      nCircles: svg.querySelectorAll('circle').length,
    };
  });
  console.log(JSON.stringify(result, null, 2));

  await page.screenshot({ path: '/tmp/integ_modal.png', fullPage: false });
  console.log('screenshot → /tmp/integ_modal.png');

  await browser.close();

  // Assert
  if (result.error) { console.error('FAIL:', result.error); process.exit(1); }
  if (!result.svgPresent) { console.error('FAIL: no SVG'); process.exit(1); }
  if (!result.hasLegLabel) { console.error('FAIL: no leg labels on diagram'); process.exit(1); }
  console.log('\nPASS — diagram preview has labels');
}

main().catch(e => { console.error(e); process.exit(1); });
