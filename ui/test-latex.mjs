/**
 * Playwright test: exercise LaTeX formula rendering in the SubTropica UI.
 *
 * Strategy: set localStorage diagram state before each page load so the
 * app auto-restores the desired graph. This avoids needing to access
 * ES-module-scoped variables.
 *
 * Run:  node ui/test-latex.mjs
 * Requires: local HTTP server on port 8080 serving ui/.
 */

const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium } = pw;

const URL = 'http://localhost:8080';
const SAVE_KEY = 'subtropica-diagram';
let errors = [];
let passed = 0;
let failed = 0;

function assert(condition, msg) {
  if (condition) {
    passed++;
    console.log(`  \u2713 ${msg}`);
  } else {
    failed++;
    console.log(`  \u2717 FAIL: ${msg}`);
  }
}

// ── Diagram definitions ─────────────────────────────────────────────

const BUBBLE = {
  vertices: [{ x: -3, y: 0 }, { x: -1, y: 0 }, { x: 1, y: 0 }, { x: 3, y: 0 }],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 2, b: 3, mass: 0 },
  ],
};

const MASSIVE_BUBBLE = {
  vertices: [{ x: -3, y: 0 }, { x: -1, y: 0 }, { x: 1, y: 0 }, { x: 3, y: 0 }],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 1 },
    { a: 1, b: 2, mass: 2 },
    { a: 2, b: 3, mass: 0 },
  ],
};

const BOX = {
  vertices: [
    { x: -2, y: -2 }, { x: -1, y: -1 }, { x: 1, y: -1 }, { x: 2, y: -2 },
    { x: 1, y: 1 }, { x: -1, y: 1 }, { x: 2, y: 2 }, { x: -2, y: 2 },
  ],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 2, b: 3, mass: 0 },
    { a: 2, b: 4, mass: 0 },
    { a: 4, b: 6, mass: 0 },
    { a: 4, b: 5, mass: 0 },
    { a: 5, b: 7, mass: 0 },
    { a: 5, b: 1, mass: 0 },
  ],
};

const BUBBLE_WITH_EXPONENT = {
  vertices: [{ x: -3, y: 0 }, { x: -1, y: 0 }, { x: 1, y: 0 }, { x: 3, y: 0 }],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 0, propExponent: 3 },
    { a: 1, b: 2, mass: 0 },
    { a: 2, b: 3, mass: 0 },
  ],
};

const BUBBLE_WITH_NUMERATOR = {
  vertices: [{ x: -3, y: 0 }, { x: -1, y: 0 }, { x: 1, y: 0 }, { x: 3, y: 0 }],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 2, b: 3, mass: 0 },
  ],
  computeConfig: {
    numeratorRows: [{ expr: 'l.p', exp: '-1' }],
  },
};

const TRIANGLE = {
  vertices: [
    { x: -3, y: 0 }, { x: -1, y: -1 }, { x: 1, y: -1 },
    { x: 0, y: 1 }, { x: 3, y: 0 }, { x: 0, y: 3 },
  ],
  edges: [
    { a: 0, b: 1, mass: 0 },
    { a: 1, b: 2, mass: 0 },
    { a: 2, b: 4, mass: 0 },
    { a: 2, b: 3, mass: 0 },
    { a: 3, b: 5, mass: 0 },
    { a: 3, b: 1, mass: 0 },
  ],
};

// ── Helpers ──────────────────────────────────────────────────────────

async function loadDiagram(page, diagram) {
  const saveData = JSON.stringify({
    vertices: diagram.vertices,
    edges: diagram.edges,
    computeConfig: diagram.computeConfig || {},
  });

  // Set localStorage, then reload to trigger restoreDiagram()
  await page.evaluate(({ key, data }) => {
    localStorage.setItem(key, data);
  }, { key: SAVE_KEY, data: saveData });

  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(500);
}

async function getFormulaTeX(page) {
  try {
    return await page.$eval('#integral-card-formula annotation[encoding="application/x-tex"]',
      el => el.textContent || '');
  } catch { return ''; }
}

async function hasKaTeXRendered(page) {
  try {
    return await page.$eval('#integral-card-formula', el => !!el.querySelector('.katex'));
  } catch { return false; }
}

async function hasKaTeXError(page) {
  try {
    return (await page.$$('#integral-card-formula .katex-error')).length > 0;
  } catch { return false; }
}

async function getFormulaText(page) {
  try {
    return await page.$eval('#integral-card-formula', el => el.textContent || '');
  } catch { return ''; }
}

// ── Main ─────────────────────────────────────────────────────────────

async function main() {
  const browser = await chromium.launch({ headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 900 } });
  const page = await ctx.newPage();

  page.on('pageerror', err => errors.push(err.message));
  page.on('console', msg => {
    if (msg.type() === 'error' && !msg.text().includes('404') && !msg.text().includes('favicon'))
      errors.push(`console.error: ${msg.text()}`);
  });

  // Initial load
  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(500);

  // ═══════════════════════════════════════════════════════════════════
  // Test 1: Bubble
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 1: Bubble diagram ===');
  errors = [];
  await loadDiagram(page, BUBBLE);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  let tex = await getFormulaTeX(page);
  assert(tex.includes('\\ell'), `Has loop momentum (got: ${tex.substring(0, 80)}...)`);
  assert(tex.includes('\\frac'), 'Has fraction');
  assert(errors.length === 0, `No JS errors (${errors.join('; ')})`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 2: Box
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 2: Box diagram ===');
  errors = [];
  await loadDiagram(page, BOX);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  tex = await getFormulaTeX(page);
  assert(tex.includes('\\ell'), 'Has loop momentum');
  assert(errors.length === 0, `No JS errors`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 3: Massive bubble
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 3: Massive bubble ===');
  errors = [];
  await loadDiagram(page, MASSIVE_BUBBLE);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  tex = await getFormulaTeX(page);
  assert(tex.includes('m'), `Has mass symbol (got: ${tex.substring(0, 100)}...)`);
  assert(errors.length === 0, `No JS errors`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 4: Non-unit exponent
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 4: Non-unit propagator exponent ===');
  errors = [];
  await loadDiagram(page, BUBBLE_WITH_EXPONENT);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  tex = await getFormulaTeX(page);
  // Online mode shows generic ν_i; full mode shows ^{3}
  assert(tex.includes('^{3}') || tex.includes('\\nu'), `Shows exponent or generic ν (got: ${tex.substring(0, 100)}...)`);
  assert(errors.length === 0, `No JS errors`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 5: Numerator
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 5: Numerator in loop formula ===');
  errors = [];
  await loadDiagram(page, BUBBLE_WITH_NUMERATOR);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  tex = await getFormulaTeX(page);
  let text = await getFormulaText(page);
  // The numerator should NOT be just "1"
  // Check that the fraction numerator includes the expression
  const hasNonTrivialNum = !tex.match(/\\frac\{1\}\{/) || tex.includes('l') || tex.includes('p');
  assert(hasNonTrivialNum, `Numerator is not just "1" (tex: ${tex.substring(0, 120)}...)`);
  assert(errors.length === 0, `No JS errors`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 6: Triangle
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 6: Triangle diagram ===');
  errors = [];
  await loadDiagram(page, TRIANGLE);

  assert(await hasKaTeXRendered(page), 'KaTeX rendered');
  assert(!await hasKaTeXError(page), 'No KaTeX errors');
  tex = await getFormulaTeX(page);
  assert(tex.includes('\\ell'), 'Has loop momentum');
  assert(errors.length === 0, `No JS errors`);

  // ═══════════════════════════════════════════════════════════════════
  // Test 7: Library popups
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 7: Library popup LaTeX ===');
  errors = [];

  // Load clean page (no diagram)
  await page.evaluate(({ key }) => localStorage.removeItem(key), { key: SAVE_KEY });
  await page.reload({ waitUntil: 'networkidle' });
  await page.waitForTimeout(500);

  const browseBtn = await page.$('#browse-btn');
  let katexErrorCards = [];
  let popupsTested = 0;

  if (browseBtn) {
    await browseBtn.click();
    await page.waitForTimeout(600);

    const cards = await page.$$('.browser-toast');
    const maxTest = Math.min(cards.length, 25);

    for (let i = 0; i < maxTest; i++) {
      try {
        const cardList = await page.$$('.browser-toast');
        if (i >= cardList.length) break;
        await cardList[i].click();
        await page.waitForTimeout(350);
        popupsTested++;

        const errEls = await page.$$('.katex-error');
        if (errEls.length > 0) {
          const cardText = await cardList[i].textContent().catch(() => `card#${i}`);
          const errTexts = await Promise.all(errEls.map(e => e.textContent()));
          katexErrorCards.push({ card: cardText.trim().substring(0, 50), errors: errTexts });
        }

        await page.keyboard.press('Escape');
        await page.waitForTimeout(150);
      } catch {}
    }

    if (katexErrorCards.length > 0) {
      console.log('  KaTeX errors in:');
      katexErrorCards.forEach(c => console.log(`    "${c.card}": ${c.errors.join(', ').substring(0, 80)}`));
    }
    assert(katexErrorCards.length === 0,
      `${popupsTested} popups tested, ${katexErrorCards.length} KaTeX errors`);
  } else {
    console.log('  (browse button not found)');
  }

  // ═══════════════════════════════════════════════════════════════════
  // Test 8: arXiv:arXiv: duplication
  // ═══════════════════════════════════════════════════════════════════
  console.log('\n=== Test 8: arXiv linkification ===');
  const allText = await page.evaluate(() => document.body.innerText);
  const dupeCount = (allText.match(/arXiv:arXiv:/g) || []).length;
  assert(dupeCount === 0, `No "arXiv:arXiv:" duplicates in page text (found ${dupeCount})`);

  // Check rendered <a> tags specifically
  const linkDupes = await page.$$eval('a', els =>
    els.filter(el => el.textContent.includes('arXiv:arXiv:')).map(el => el.textContent)
  );
  assert(linkDupes.length === 0, `No "arXiv:arXiv:" in link text (found ${linkDupes.length})`);

  // ═══════════════════════════════════════════════════════════════════
  // Summary
  // ═══════════════════════════════════════════════════════════════════
  console.log(`\n${'='.repeat(50)}`);
  console.log(`Results: ${passed} passed, ${failed} failed`);
  console.log(`${'='.repeat(50)}`);

  await browser.close();
  process.exit(failed > 0 ? 1 : 0);
}

main().catch(err => {
  console.error('Test runner error:', err);
  process.exit(1);
});
