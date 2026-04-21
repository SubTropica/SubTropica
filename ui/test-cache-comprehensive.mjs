/**
 * Comprehensive cache test suite — sequential single-flow.
 *
 * Uses e11|e| (bubble) / 012|0| (2-mass config). This config has a bundled
 * result with subs='{}', norm='Automatic', nums=[], and no library-local
 * copy, making it a clean target for all cache-path tests.
 *
 * Flow:
 *   1. Exact match hit from bundled library
 *   2. Substitutions → soft-transform hit via transformResult
 *   3. Normalization change → soft-transform hit
 *   4. Combined subs + norm → soft-transform hit
 *   5. Numerator blocks soft-transform → fresh integration
 *   6. "Applied:" note visible in UI for soft-transform
 */

import { chromium } from 'playwright';

const BASE = process.env.BASE || 'http://127.0.0.1:63983';
const TOPO = '123|24|e|e|e|';
const CONFIG = '000|00|0|0|0|';

let browser, page;
let passed = 0, failed = 0;
const failures = [];
let apiCalls = {};

function resetApi() { apiCalls = { lock: 0, integrate: 0, transformResult: 0 }; }

async function reset() {
  await page.evaluate(() => {
    const s = document.getElementById('integ-status-text');
    if (s) { s.textContent = ''; s._done = false; }
  });
}

async function waitResult(label, timeoutMs = 60000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const s = await page.evaluate(() => document.getElementById('integ-status-text')?.textContent || '');
    if (s.includes('from library') || s.includes('Complete') || s.includes('substitutions')) return s;
    if (s.includes('Failed')) throw new Error(`${label}: ${s}`);
    await page.waitForTimeout(150);
  }
  throw new Error(`${label}: timed out`);
}

async function test(name, fn) {
  try { await fn(); passed++; console.log(`  ✓ ${name}`); }
  catch (e) { failed++; failures.push({ name, error: e.message }); console.log(`  ✗ ${name}: ${e.message}`); }
}
function assert(c, m) { if (!c) throw new Error(m); }

async function loadDiagram(config = CONFIG) {
  await page.evaluate(async ({t, c}) => {
    // Reload library so we see latest disk state
    const resp = await fetch('/api/library');
    if (resp.ok) {
      const fresh = await resp.json();
      for (const k of Object.keys(window.__stTest.library.topologies)) delete window.__stTest.library.topologies[k];
      Object.assign(window.__stTest.library.topologies, fresh.topologies);
    }
    window.__stTest.loadFromNickel(t, c);
    window.__stTest.computeConfig.numeratorRows = [];
    window.__stTest.computeConfig.substitutions = '';
    window.__stTest.computeConfig.normalization = 'Automatic';
    window.__stTest.onGraphChanged();
  }, { t: TOPO, c: config });
  await page.waitForTimeout(500);
  // Set name + clear DOM config inputs
  await page.evaluate(() => {
    document.getElementById('ic-name').value = 'cache-test';
    const el = document.getElementById('cfg-substitutions');
    if (el) el.value = '';
    const el2 = document.getElementById('cfg-substitutions-momenta');
    if (el2) el2.value = '';
    const na = document.getElementById('cfg-auto-norm');
    if (na) na.checked = true;
  });
  resetApi();
}

async function setSubs(subs) {
  await page.evaluate((s) => {
    window.__stTest.computeConfig.substitutions = s;
    const el = document.getElementById('cfg-substitutions');
    if (el) el.value = s;
    const el2 = document.getElementById('cfg-substitutions-momenta');
    if (el2) el2.value = s;
  }, subs);
}

async function setNorm(norm) {
  await page.evaluate((n) => {
    window.__stTest.computeConfig.normalization = n;
    const na = document.getElementById('cfg-auto-norm');
    if (na) na.checked = (n === 'Automatic');
    const ni = document.getElementById('cfg-normalization');
    if (ni && n !== 'Automatic') ni.value = n;
  }, norm);
}

async function main() {
  console.log(`\nCache test suite — ${BASE}/?test=1`);
  console.log(`Target: ${TOPO} / ${CONFIG}\n`);

  browser = await chromium.launch({ headless: true });
  const ctx = await browser.newContext({ viewport: { width: 1400, height: 900 } });
  page = await ctx.newPage();

  page.on('pageerror', e => console.log('  [pageerror]', e.message));
  page.on('request', req => {
    const u = req.url();
    if (u.includes('/api/lock')) apiCalls.lock++;
    if (u.includes('/api/integrate')) apiCalls.integrate++;
    if (u.includes('/api/transformResult')) apiCalls.transformResult++;
  });

  await page.goto(BASE + '/?test=1', { waitUntil: 'domcontentloaded', timeout: 30000 });
  await page.waitForLoadState('networkidle', { timeout: 15000 }).catch(() => {});
  await page.waitForFunction(
    () => window.__stTest && window.__stTest.library && Object.keys(window.__stTest.library.topologies || {}).length > 50,
    { timeout: 30000 });
  console.log('Library loaded.\n');

  // ═══════════════════════════════════════════════════════════════════
  console.log('## 1. Exact match (bundled result, all defaults)');
  await test('Exact match hit', async () => {
    await loadDiagram();
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('exact');
    assert(s.includes('from library'), `expected "from library", got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
    assert(apiCalls.transformResult === 0, `transformResult=${apiCalls.transformResult}`);
  });

  // ═══════════════════════════════════════════════════════════════════
  console.log('\n## 2. Substitutions → soft-transform');
  await test('Subs trigger transformResult, no integrate', async () => {
    await loadDiagram();
    await setSubs('{s12 -> 5}');
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('subs');
    assert(s.includes('from library'), `expected cache hit, got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
    assert(apiCalls.transformResult > 0, `transformResult=${apiCalls.transformResult}`);
  });

  // ═══════════════════════════════════════════════════════════════════
  console.log('\n## 3. Normalization → soft-transform');
  await test('Norm change triggers transformResult', async () => {
    await loadDiagram();
    await setNorm('1');
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('norm');
    assert(s.includes('from library'), `expected cache hit, got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
    assert(apiCalls.transformResult > 0, `transformResult=${apiCalls.transformResult}`);
  });

  // ═══════════════════════════════════════════════════════════════════
  console.log('\n## 4. Combined subs + norm → single soft-transform');
  await test('Both subs + norm in one transform call', async () => {
    await loadDiagram();
    await setSubs('{s12 -> -2, s23 -> 3}');
    await setNorm('1');
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('combined');
    assert(s.includes('from library'), `expected cache hit, got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
    assert(apiCalls.transformResult > 0, `transformResult=${apiCalls.transformResult}`);
  });

  // ═══════════════════════════════════════════════════════════════════
  console.log('\n## 5. Numerator blocks soft-transform → fresh integration');
  await test('Numerator forces fresh run', async () => {
    await loadDiagram();
    await setSubs('{s12 -> 5}');
    await page.evaluate(() => {
      window.__stTest.computeConfig.numeratorRows = [{ expr: 'l1*p1', exp: '-1' }];
    });
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('num-block', 180000);
    assert(!s.includes('from library'), `expected fresh, got "${s}"`);
    assert(apiCalls.integrate > 0, 'should call integrate');
    assert(apiCalls.transformResult === 0, 'should NOT call transformResult');
  });

  // ═══════════════════════════════════════════════════════════════════
  console.log('\n## 6. UI shows "Applied:" note');
  await test('Applied note visible', async () => {
    await loadDiagram();
    await setSubs('{s12 -> 99}');
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    await waitResult('ui-note');
    await page.waitForTimeout(200);
    const note = await page.evaluate(() => {
      const params = document.getElementById('integ-params');
      if (!params) return '';
      return Array.from(params.querySelectorAll('div[style*="italic"]')).map(n => n.textContent).join(' ');
    });
    assert(note.includes('Applied:'), `expected "Applied:", got "${note}"`);
  });

  // ═══════════════════════════════════════════════════════════════════
  // Advanced scenarios
  // ═══════════════════════════════════════════════════════════════════

  console.log('\n## 7. Re-run exact match after soft-transform (no state leakage)');
  await test('Exact match still works after a soft-transform run', async () => {
    await loadDiagram();
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('re-exact');
    assert(s.includes('from library'), `expected exact hit, got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
    assert(apiCalls.transformResult === 0, `no transform needed`);
  });

  console.log('\n## 8. Different topology → no cross-contamination');
  await test('Different topo does not false-hit', async () => {
    // Use bubble (e11|e|) — completely different topology
    await page.evaluate(async () => {
      const resp = await fetch('/api/library');
      if (resp.ok) {
        const fresh = await resp.json();
        for (const k of Object.keys(window.__stTest.library.topologies)) delete window.__stTest.library.topologies[k];
        Object.assign(window.__stTest.library.topologies, fresh.topologies);
      }
      window.__stTest.loadFromNickel('e11|e|', '000|0|');
      window.__stTest.computeConfig.numeratorRows = [{ expr: 'unique_' + Date.now(), exp: '-1' }];
      window.__stTest.onGraphChanged();
    });
    await page.waitForTimeout(500);
    await page.evaluate(() => { document.getElementById('ic-name').value = 'cross-test'; });
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('cross-topo', 180000);
    assert(!s.includes('from library') || apiCalls.integrate > 0,
      'unique numerator on different topo should not match original topo');
  });

  console.log('\n## 9. Eps order: cached >= requested → hit');
  await test('Higher cached eps order still hits', async () => {
    await loadDiagram();
    // Default eps order is 0; stored entry has eps 0. Request eps -1 → cached >= requested.
    await page.evaluate(() => {
      window.__stTest.computeConfig.epsOrder = '-1';
      const el = document.getElementById('ic-eps-order');
      if (el) el.value = '-1';
    });
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('eps-higher');
    assert(s.includes('from library'), `expected hit, got "${s}"`);
    assert(apiCalls.integrate === 0, `integrate=${apiCalls.integrate}`);
  });

  console.log('\n## 10. Eps order: requested > cached → miss');
  await test('Higher requested eps order misses', async () => {
    await loadDiagram();
    await page.evaluate(() => {
      window.__stTest.computeConfig.epsOrder = '5';
      const el = document.getElementById('ic-eps-order');
      if (el) el.value = '5';
    });
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('eps-miss', 180000);
    assert(!s.includes('from library'), `expected miss, got "${s}"`);
    assert(apiCalls.integrate > 0, 'should call integrate');
  });

  console.log('\n## 11. Dimension change → miss (no soft-transform for dim)');
  await test('Dimension change forces fresh', async () => {
    await loadDiagram();
    await page.evaluate(() => {
      window.__stTest.computeConfig.dimension = '6 - 2*eps';
      const el = document.getElementById('ic-dimension');
      if (el) el.value = '6 - 2*eps';
    });
    await reset(); resetApi();
    await page.evaluate(() => window.__stTest.doIntegrate());
    const s = await waitResult('dim-miss', 180000);
    assert(!s.includes('from library'), `expected miss, got "${s}"`);
    assert(apiCalls.integrate > 0, 'should call integrate');
  });

  // ── Summary ──
  console.log(`\n${'═'.repeat(50)}`);
  console.log(`${passed}/${passed + failed} passed`);
  if (failures.length > 0) {
    console.log('\nFailures:');
    failures.forEach(f => console.log(`  - ${f.name}: ${f.error}`));
  }
  await browser.close();
  if (failed > 0) process.exit(1);
  console.log('\nPASS');
}

main().catch(e => { console.error('FATAL:', e); process.exit(1); });
