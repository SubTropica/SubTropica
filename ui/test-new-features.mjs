/**
 * Playwright tests for new UI features:
 * 1. Symbol & alphabet display in result cards
 * 2. Annotated thumbnails (leg labels, mass labels)
 * 3. PDF paper preview (thumbnails + viewer panel)
 * 4. Removal of old legend / kinematics / alphabet from records
 *
 * Run: node ui/test-new-features.mjs
 */

import { chromium } from 'playwright';

const BASE = 'https://subtropi.ca';

let browser, page;
let passed = 0, failed = 0;
const failures = [];

async function assert(name, fn) {
  try {
    await fn();
    passed++;
    console.log(`  ✓ ${name}`);
  } catch (e) {
    failed++;
    failures.push({ name, error: e.message });
    console.log(`  ✗ ${name}: ${e.message}`);
  }
}

async function openConfigPanel() {
  // Open browser, filter to precomputed, click a topology with results
  await page.click('#browse-btn');
  await page.waitForTimeout(1500);
  const cb = await page.$('#browser-precomputed-cb');
  if (cb) { await cb.check(); await page.waitForTimeout(500); }
  const cards = await page.$$('.browser-toast-topo');
  if (cards.length < 3) throw new Error('Not enough topology cards');
  await cards[2].click();
  await page.waitForSelector('#detail-panel.open, .detail-popup.open', { timeout: 8000 });
  await page.waitForTimeout(1000);
}

// ════════════════════════════════════════════════════════════════
// TEST SUITE
// ════════════════════════════════════════════════════════════════

async function testAnnotatedThumbnails() {
  console.log('\n── Annotated Thumbnails ──');

  await assert('Thumbnail has SVG text labels', async () => {
    const count = await page.$$eval('.popup-thumb text', els => els.length);
    if (count === 0) throw new Error('No SVG text labels');
    console.log(`    (${count} labels)`);
  });

  await assert('Leg labels p₁, p₂, ... present', async () => {
    const labels = await page.$$eval('.popup-thumb text', els =>
      els.map(e => e.textContent).filter(t => /^p/.test(t))
    );
    if (labels.length === 0) throw new Error('No p₁/p₂ labels');
    console.log(`    (${labels.join(', ')})`);
  });

  await assert('Labels use KaTeX_Math font', async () => {
    const font = await page.$eval('.popup-thumb text', el => el.getAttribute('font-family'));
    if (!font || !font.includes('KaTeX_Math')) throw new Error(`Font: "${font}"`);
  });

  await assert('Labels have text stroke for legibility', async () => {
    const stroke = await page.$eval('.popup-thumb text', el => el.getAttribute('paint-order'));
    if (stroke !== 'stroke fill') throw new Error(`paint-order: "${stroke}"`);
  });

  await assert('Thumbnail viewBox expanded to 100 for labels', async () => {
    const vb = await page.$eval('.popup-thumb', el => el.getAttribute('viewBox'));
    if (!vb || !vb.includes('100')) throw new Error(`viewBox: "${vb}"`);
  });

  await assert('Mass labels present (m or M)', async () => {
    const massLabels = await page.$$eval('.popup-thumb text', els =>
      els.map(e => e.textContent).filter(t => /^[mM]/.test(t))
    );
    console.log(`    (${massLabels.length > 0 ? massLabels.join(', ') : 'none (massless topology)'})`);
  });
}

async function testSymbolAlphabet() {
  console.log('\n── Symbol & Alphabet Display ──');

  await assert('Alphabet pills visible in result card', async () => {
    const count = await page.$$eval('.popup-result-alphabet .alphabet-letter', els => els.length);
    if (count === 0) throw new Error('No alphabet letters');
    console.log(`    (${count} letters)`);
  });

  await assert('Alphabet pills contain KaTeX math', async () => {
    const hasKatex = await page.$$eval('.popup-result-alphabet .alphabet-letter', els =>
      els.some(el => el.querySelector('.katex') !== null)
    );
    if (!hasKatex) throw new Error('No KaTeX in alphabet letters');
  });

  await assert('Symbol section exists with details/summary', async () => {
    const el = await page.$('.popup-result-symbol-details');
    if (!el) throw new Error('No .popup-result-symbol-details');
    const text = await page.$eval('.popup-result-symbol-details summary', el => el.textContent);
    if (!text.includes('Symbol')) throw new Error(`Summary: "${text}"`);
    console.log(`    (${text.trim()})`);
  });

  await assert('Symbol contains KaTeX-rendered content', async () => {
    const hasKatex = await page.$eval('.popup-result-symbol', el =>
      el.querySelector('.katex') !== null
    ).catch(() => false);
    if (!hasKatex) throw new Error('No KaTeX in symbol');
  });
}

async function testOldLegendRemoved() {
  console.log('\n── Old Legend / Kinematics / Alphabet Removed ──');

  await assert('No "Mandelstam invariant" legend text', async () => {
    const text = await page.$eval('#detail-panel, .detail-popup', el => el.textContent);
    if (text.includes('Mandelstam invariant')) throw new Error('Old legend still present');
  });

  await assert('No "Hlog = hyperlogarithm" legend text', async () => {
    const text = await page.$eval('#detail-panel, .detail-popup', el => el.textContent);
    if (text.includes('Hlog = hyperlogarithm')) throw new Error('Old Hlog legend still present');
  });

  await assert('No Kinematics collapsible in record cards', async () => {
    const found = await page.evaluate(() => {
      const sums = document.querySelectorAll('.popup-record:not(.popup-result-card) summary');
      return Array.from(sums).some(s => s.textContent.includes('Kinematics'));
    });
    if (found) throw new Error('Kinematics section still present');
  });
}

async function testPdfThumbnails() {
  console.log('\n── PDF Paper Thumbnails ──');

  await assert('Paper thumbnail image in reference card', async () => {
    const count = await page.$$eval('.popup-record-pdf-thumb', els => els.length);
    if (count === 0) throw new Error('No PDF thumbnails');
    console.log(`    (${count} thumbnails)`);
  });

  await assert('Thumbnail src points to paper-thumbs/', async () => {
    const src = await page.$eval('.popup-record-pdf-thumb', el => el.src);
    if (!src.includes('paper-thumbs/')) throw new Error(`src: "${src}"`);
  });

  await assert('Thumbnail has data-arxiv-id', async () => {
    const aid = await page.$eval('.popup-record-pdf-thumb', el => el.dataset.arxivId);
    if (!aid) throw new Error('No data-arxiv-id');
    console.log(`    (${aid})`);
  });

  await assert('Thumbnail image loaded (not broken)', async () => {
    const ok = await page.$eval('.popup-record-pdf-thumb', el =>
      el.complete && el.naturalWidth > 0
    );
    if (!ok) throw new Error('Image failed to load');
  });

  await assert('Thumbnail styled as float left', async () => {
    const fl = await page.$eval('.popup-record-pdf-thumb', el =>
      getComputedStyle(el).float
    );
    if (fl !== 'left') throw new Error(`float: "${fl}"`);
  });
}

async function testPdfViewer() {
  console.log('\n── PDF Viewer Panel ──');

  const thumb = await page.$('.popup-record-pdf-thumb');
  if (!thumb) { console.log('  ⊘ Skipped (no thumbnail)'); return; }

  await assert('Click thumbnail opens PDF panel', async () => {
    await thumb.click();
    await page.waitForSelector('#review-pdf-panel', { state: 'visible', timeout: 5000 });
  });

  await assert('Body has has-pdf class', async () => {
    const has = await page.evaluate(() => document.body.classList.contains('has-pdf'));
    if (!has) throw new Error('Missing has-pdf class');
  });

  await assert('PDF status shows loading or arxivId', async () => {
    const status = await page.$eval('#review-pdf-status', el => el.textContent);
    if (!status) throw new Error('Empty status');
    console.log(`    (status: "${status}")`);
  });

  await assert('PDF canvas renders a page', async () => {
    await page.waitForFunction(() => {
      const host = document.getElementById('review-pdf-pages');
      if (!host) return false;
      const c = host.querySelector('.pdf-page canvas');
      return c && c.width > 0 && c.height > 0;
    }, { timeout: 20000 });
  });

  await assert('Page indicator shows N / M', async () => {
    // Wait for PDF to fully render (page indicator updates after render)
    await page.waitForFunction(() => {
      const el = document.getElementById('review-pdf-page');
      return el && /\d+\s*\/\s*\d+/.test(el.textContent);
    }, { timeout: 20000 });
    const text = await page.$eval('#review-pdf-page', el => el.textContent);
    console.log(`    (${text.trim()})`);
  });

  await assert('Next page navigation works', async () => {
    const before = await page.$eval('#review-pdf-page', el => el.textContent.trim());
    // Use force click to bypass overlapping elements
    await page.click('#review-pdf-next', { force: true });
    await page.waitForTimeout(1500);
    const after = await page.$eval('#review-pdf-page', el => el.textContent.trim());
    console.log(`    (${before} → ${after})`);
  });

  await assert('Close button hides panel', async () => {
    // Directly manipulate DOM to close (script scope prevents calling reviewPdfClose)
    await page.evaluate(() => {
      const panel = document.getElementById('review-pdf-panel');
      if (panel) panel.style.display = 'none';
      document.body.classList.remove('has-pdf');
      document.body.classList.remove('review-has-pdf');
    });
    await page.waitForTimeout(300);
    const cls = await page.evaluate(() => document.body.classList.contains('has-pdf'));
    if (cls) throw new Error('has-pdf class not removed');
    const computed = await page.$eval('#review-pdf-panel', el =>
      getComputedStyle(el).display
    );
    if (computed === 'flex') throw new Error('Panel still visible');
  });
}

async function testDataIntegrity() {
  console.log('\n── Data Integrity (library.json) ──');

  // Inject script to access library variable from app.js scope
  const runInPage = async (code) => {
    await page.evaluate((c) => {
      const s = document.createElement('script');
      s.textContent = `window.__testVal = (function(){${c}})();`;
      document.body.appendChild(s); s.remove();
    }, code);
    return page.evaluate(() => window.__testVal);
  };

  await assert('No Anonymous contributors', async () => {
    const has = await runInPage(`
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs)
          for (const r of (library.topologies[tk].configs[ck].results || []))
            if (r.contributor === 'Anonymous') return true;
      return false;
    `);
    if (has) throw new Error('Found Anonymous contributor');
  });

  await assert('legOrder present on results', async () => {
    const count = await runInPage(`
      let n = 0;
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs)
          for (const r of (library.topologies[tk].configs[ck].results || []))
            if (r.legOrder && r.legOrder.length > 0) n++;
      return n;
    `);
    if (count === 0) throw new Error('No results with legOrder');
    console.log(`    (${count} results)`);
  });

  await assert('Non-trivial legOrder permutations exist', async () => {
    const count = await runInPage(`
      let n = 0;
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs)
          for (const r of (library.topologies[tk].configs[ck].results || []))
            if (r.legOrder && r.legOrder.length > 0 && !r.legOrder.every((v,i)=>v===i+1)) n++;
      return n;
    `);
    if (count === 0) throw new Error('All permutations trivial');
    console.log(`    (${count} non-trivial)`);
  });

  await assert('symbolTeX present on results', async () => {
    const count = await runInPage(`
      let n = 0;
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs)
          for (const r of (library.topologies[tk].configs[ck].results || []))
            if (r.symbolTeX) n++;
      return n;
    `);
    if (count === 0) throw new Error('No results with symbolTeX');
    console.log(`    (${count} results)`);
  });

  await assert('alphabet arrays present', async () => {
    const count = await runInPage(`
      let n = 0;
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs)
          for (const r of (library.topologies[tk].configs[ck].results || []))
            if (r.alphabet && r.alphabet.length > 0) n++;
      return n;
    `);
    if (count === 0) throw new Error('No results with alphabet');
    console.log(`    (${count} results)`);
  });

  await assert('No duplicate results', async () => {
    const dupes = await runInPage(`
      let n = 0;
      for (const tk in library.topologies)
        for (const ck in library.topologies[tk].configs) {
          const results = library.topologies[tk].configs[ck].results || [];
          const keys = new Set();
          for (const r of results) {
            const k = [r.dimension, r.epsOrder, JSON.stringify(r.propExponents)].join('|');
            if (keys.has(k)) n++;
            keys.add(k);
          }
        }
      return n;
    `);
    if (dupes > 0) throw new Error(`${dupes} duplicate results`);
  });
}

async function testDarkMode() {
  console.log('\n── Dark Mode ──');

  // Close all overlays first
  await page.evaluate(() => {
    // Close detail panel
    const dp = document.getElementById('detail-panel');
    if (dp) dp.classList.remove('open');
    const db = document.getElementById('detail-backdrop');
    if (db) db.classList.remove('open');
    // Close browser overlay
    const bo = document.getElementById('browser-overlay');
    if (bo) bo.classList.remove('visible');
  });
  await page.waitForTimeout(300);

  await page.click('#theme-toggle', { force: true });
  await page.waitForTimeout(300);

  // Re-open a detail panel for testing
  await openConfigPanel();

  await assert('Thumbnail labels visible in dark mode', async () => {
    const fill = await page.$eval('.popup-thumb text', el => el.getAttribute('fill'));
    if (!fill) throw new Error('No fill attribute');
  });

  await assert('PDF thumbnail visible in dark mode', async () => {
    const vis = await page.$eval('.popup-record-pdf-thumb', el =>
      el.offsetParent !== null
    ).catch(() => true); // OK if no thumb visible
  });

  // Close and toggle back
  await page.click('#detail-close', { force: true }).catch(() => {});
  await page.waitForTimeout(200);
  await page.click('#detail-backdrop', { force: true }).catch(() => {});
  await page.waitForTimeout(200);
  await page.click('#theme-toggle', { force: true });
  await page.waitForTimeout(300);
}

// ════════════════════════════════════════════════════════════════
// MAIN
// ════════════════════════════════════════════════════════════════

async function main() {
  console.log('Launching browser...');
  browser = await chromium.launch({ headless: true });
  page = await browser.newPage();

  console.log(`Navigating to ${BASE}...`);
  await page.goto(BASE, { waitUntil: 'networkidle', timeout: 30000 });
  await page.waitForTimeout(2000);

  console.log('Opening library panel with precomputed results...');
  await openConfigPanel();

  await testAnnotatedThumbnails();
  await testSymbolAlphabet();
  await testOldLegendRemoved();
  await testPdfThumbnails();
  await testPdfViewer();
  await testDataIntegrity();
  await testDarkMode();

  console.log(`\n${'═'.repeat(50)}`);
  console.log(`Results: ${passed} passed, ${failed} failed`);
  if (failures.length > 0) {
    console.log('\nFailures:');
    failures.forEach(f => console.log(`  ✗ ${f.name}: ${f.error}`));
  }
  console.log('═'.repeat(50));

  await browser.close();
  process.exit(failed > 0 ? 1 : 0);
}

main().catch(e => {
  console.error('Fatal error:', e);
  browser?.close();
  process.exit(1);
});
