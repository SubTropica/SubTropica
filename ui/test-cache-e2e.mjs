// End-to-end test of the exact-match library cache short-circuit.
//
// Runs against a live local SubTropica server. Loads a known small
// topology onto the canvas via loadFromNickel(), runs integration twice,
// and asserts that the second run is a pure cache hit — no /api/integrate
// request, status shows "from library".
//
// BASE env var: server URL. Default http://127.0.0.1:60919.

import { chromium } from 'playwright';

const BASE = process.env.BASE || 'http://127.0.0.1:60919';
const URL_WITH_TEST = BASE + '/?test=1';

async function waitForTestHook(page) {
  await page.waitForFunction(() => window.__stTest && window.__stTest.library, { timeout: 30000 });
}

async function waitForResult(page, timeoutMs, label) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    const info = await page.evaluate(() => {
      const status = document.getElementById('integ-status-text');
      return {
        status: status ? (status.textContent || '') : '',
      };
    });
    if (info.status && (
        info.status.includes('from library') ||
        info.status.includes('Complete')
    )) return info.status;
    if (info.status && info.status.includes('Failed')) throw new Error(`${label}: ${info.status}`);
    await page.waitForTimeout(200);
  }
  throw new Error(`${label}: timed out waiting for result`);
}

// (closeIntegrationPanel removed — showIntegrationPanel re-renders the
// integrate-body in place, so Run 2 can fire straight after Run 1.)

async function main() {
  console.log(`Connecting to ${URL_WITH_TEST}`);
  const browser = await chromium.launch({ headless: true });
  const context = await browser.newContext({ viewport: { width: 1400, height: 900 } });
  const page = await context.newPage();

  page.on('pageerror', err => console.log('  [pageerror]', err.message));
  page.on('console', msg => {
    const t = msg.type();
    if (t === 'error') console.log(`  [console.error]`, msg.text());
  });

  let integrateCount = 0;
  let lockCount = 0;
  let transformCount = 0;
  let lastLockResponse = null;
  page.on('request', req => {
    const u = req.url();
    if (u.includes('/api/integrate')) integrateCount++;
    if (u.includes('/api/lock')) lockCount++;
    if (u.includes('/api/transformResult')) transformCount++;
  });
  page.on('response', async (resp) => {
    if (resp.url().includes('/api/lock')) {
      try { lastLockResponse = await resp.json(); } catch {}
    }
  });

  await page.goto(URL_WITH_TEST, { waitUntil: 'domcontentloaded', timeout: 30000 });
  await page.waitForLoadState('networkidle', { timeout: 30000 }).catch(() => {});
  await waitForTestHook(page);
  const topoInfo = await page.evaluate(() => {
    const lib = window.__stTest.library;
    const out = [];
    for (const k in lib.topologies) {
      const t = lib.topologies[k];
      if (t.loops === 1 && t.legs === 2 && t.props === 2) {
        out.push({ key: k, cfg: Object.keys(t.configs)[0] });
        if (out.length >= 3) break;
      }
    }
    return { nTopos: Object.keys(lib.topologies).length, candidates: out };
  });
  console.log(`  library loaded: ${topoInfo.nTopos} topologies`);
  console.log(`  1L/2P/2E candidates: ${topoInfo.candidates.map(c => c.key + ':' + c.cfg).join(', ')}`);
  if (topoInfo.candidates.length === 0) throw new Error('No 1-loop 2-point 2-prop topology in library');

  const { key: topoKey, cfg: configKey } = topoInfo.candidates[0];
  console.log(`  using: ${topoKey} / ${configKey}`);

  await page.evaluate(({k, c}) => window.__stTest.loadFromNickel(k, c), { k: topoKey, c: configKey });
  await page.waitForTimeout(500);

  const canvasState = await page.evaluate(() => ({
    nVerts: window.__stTest.state.vertices.length,
    nEdges: window.__stTest.state.edges.length,
    nickel: window.__stTest.currentNickel,
  }));
  console.log(`  canvas state: ${canvasState.nVerts} verts, ${canvasState.nEdges} edges, nickel=${canvasState.nickel}`);
  if (canvasState.nEdges === 0) throw new Error('loadFromNickel did not populate canvas');

  await page.evaluate(() => {
    const inp = document.getElementById('ic-name');
    if (inp) inp.value = 'e2e-test';
  });

  // Force a fresh integration by adding a numerator row — the existing
  // bubble entries in the library have numerators:[], so a non-empty
  // numerator row guarantees no cache hit on Run 1. Run 2, reading back
  // the freshly-saved entry, should then hit.
  const cfgCheck = await page.evaluate(() => {
    const cc = window.__stTest.computeConfig;
    // Use l1*p1 — l1 is the loop momentum symbol that SubTropica defines,
    // so the integrand is well-formed (unlike k1*p1 which uses an undefined
    // symbol and produces an infinity result that STSaveResult can't store).
    cc.numeratorRows = [{ expr: 'l1*p1', exp: '-1' }];
    const payload = window.__stTest.collectIntegrationPayload();
    return {
      ccRows: cc.numeratorRows,
      payloadRows: payload.numeratorRows,
    };
  });
  console.log('  forced numerator:', JSON.stringify(cfgCheck));

  // ── Run 1 ──
  console.log('\n## Run 1');
  integrateCount = 0; lockCount = 0; transformCount = 0;
  await page.evaluate(() => window.__stTest.doIntegrate());
  const status1 = await waitForResult(page, 180000, 'Run 1');
  console.log(`  status: ${status1}`);
  console.log(`  lock=${lockCount} integrate=${integrateCount} transform=${transformCount}`);
  const run1Cached = status1.includes('from library');
  console.log(`  cached: ${run1Cached}`);

  if (!run1Cached) {
    console.log('  (fresh run — reloading library for Run 2 to see new entry)');
    const libInfo = await page.evaluate(async () => {
      const resp = await fetch('/api/library');
      if (!resp.ok) return { ok: false };
      const fresh = await resp.json();
      // Wholesale replace the topology map via a helper. Grab the bubble
      // topology entry and count its results before/after.
      const before = Object.keys(window.__stTest.library.topologies['e11|e|']?.configs || {}).length;
      // Replace by clearing and Object.assign
      for (const k of Object.keys(window.__stTest.library.topologies)) {
        delete window.__stTest.library.topologies[k];
      }
      Object.assign(window.__stTest.library.topologies, fresh.topologies);
      const bubble = window.__stTest.library.topologies['e11|e|'];
      const bubbleResults = {};
      if (bubble && bubble.configs) {
        for (const ck in bubble.configs) {
          const rs = bubble.configs[ck].Results || bubble.configs[ck].results || [];
          bubbleResults[ck] = rs.length;
        }
      }
      return { ok: true, beforeConfigs: before, afterConfigs: Object.keys(bubble?.configs || {}).length, bubbleResults };
    });
    console.log(`  library reload: ${JSON.stringify(libInfo)}`);
    await page.evaluate(() => window.__stTest.onGraphChanged());
    await page.waitForTimeout(500);
  }

  // Run 2 will re-render the modal in place (showIntegrationPanel clears
  // integrate-body), so we don't need to close the panel first. But the
  // '★ Result from library' status text is applied by onIntegrationComplete
  // after ~50ms, so we have to give it breathing room.
  //
  // ── Run 2: MUST hit cache ──
  console.log('\n## Run 2 (expect cache hit)');
  integrateCount = 0; lockCount = 0; transformCount = 0;
  lastLockResponse = null;
  // Clear the status text so the waitForResult poll doesn't trip on Run 1's leftover
  await page.evaluate(() => {
    const s = document.getElementById('integ-status-text');
    if (s) { s.textContent = ''; s._done = false; }
  });
  await page.evaluate(() => window.__stTest.doIntegrate());
  const status2 = await waitForResult(page, 30000, 'Run 2');
  console.log(`  status: ${status2}`);
  console.log(`  lock=${lockCount} integrate=${integrateCount} transform=${transformCount}`);
  if (lastLockResponse) {
    console.log(`  lock.topology.nickelIndex: ${JSON.stringify(lastLockResponse.topology?.nickelIndex)}`);
    console.log(`  currentNickel: ${await page.evaluate(() => window.__stTest.currentNickel)}`);
  }
  const run2Cached = status2.includes('from library');
  console.log(`  cached: ${run2Cached}`);

  console.log('\n## Verdict');
  let ok = true;
  if (!run2Cached) { console.log('  ✗ Run 2 did NOT show "from library"'); ok = false; }
  else console.log('  ✓ Run 2 shows "from library"');
  if (integrateCount > 0) { console.log(`  ✗ Run 2 fired ${integrateCount} /api/integrate call(s)`); ok = false; }
  else console.log('  ✓ Run 2 did not call /api/integrate');
  if (transformCount > 0) { console.log(`  ✗ Run 2 fired ${transformCount} /api/transformResult call(s)`); ok = false; }
  else console.log('  ✓ Run 2 did not call /api/transformResult');

  await browser.close();
  if (!ok) process.exit(1);
  console.log('\nPASS');
}

main().catch(e => {
  console.error('FATAL:', e.stack || e.message);
  process.exit(1);
});
