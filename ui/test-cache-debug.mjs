// Debug variant: load the UI, load the topology, reload library, and
// directly invoke lookupLibraryResult with the same payload/lockResult
// structure doIntegrate would pass. Print the intermediate state so we
// can see WHICH field comparison fails.

import { chromium } from 'playwright';
const BASE = process.env.BASE || 'http://127.0.0.1:60919';

async function main() {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();
  page.on('pageerror', e => console.log('[pageerror]', e.message));

  await page.goto(BASE + '/?test=1', { waitUntil: 'networkidle', timeout: 30000 });
  await page.waitForFunction(() => window.__stTest && window.__stTest.library, { timeout: 20000 });

  // Load e11|e| / 000|0|
  await page.evaluate(() => window.__stTest.loadFromNickel('e11|e|', '000|0|'));
  await page.waitForTimeout(500);

  // Set a name so collectIntegrationPayload doesn't bail
  await page.evaluate(() => {
    const inp = document.getElementById('ic-name');
    if (inp) inp.value = 'e2e-debug';
  });

  // Trigger a live match update so currentMatches is populated
  await page.evaluate(() => window.__stTest.onGraphChanged());
  await page.waitForTimeout(300);

  const debug = await page.evaluate(async () => {
    const T = window.__stTest;
    const payload = T.collectIntegrationPayload();

    // Build a synthetic lockResult matching what /api/lock would return.
    // For the lookup we only need lockResult.topology.nickelIndex.
    const lockResult = { topology: { nickelIndex: T.currentNickel } };

    // Peek at the library entry we care about
    const topo = T.library.topologies[T.currentNickel];
    const cfgs = topo ? topo.configs : null;
    const cfgKeys = cfgs ? Object.keys(cfgs) : [];
    const peeked = {};
    if (cfgs) {
      for (const ck of cfgKeys) {
        const results = cfgs[ck].Results || cfgs[ck].results || [];
        peeked[ck] = results.map(r => ({
          dimension: r.dimension,
          epsOrder: r.epsOrder,
          propExponents: r.propExponents,
          numerators: r.numerators,
          internalMasses: r.internalMasses,
          externalMasses: r.externalMasses,
          substitutions: r.substitutions,
          normalization: r.normalization,
          stVersion: r.stVersion,
        }));
      }
    }

    // Peek currentMatches
    const matchInfo = T.currentMatches.find(m => m.topoKey === T.currentNickel);

    // Actually run the lookup
    const hit = T.lookupLibraryResult(lockResult, payload);

    return {
      nickel: T.currentNickel,
      payload: {
        dimension: payload.dimension,
        epsOrder: payload.epsOrder,
        propExponents: payload.propExponents,
        numeratorRows: payload.numeratorRows,
        internalMasses: payload.internalMasses,
        externalMasses: payload.externalMasses,
        substitutions: payload.substitutions,
        normalization: payload.normalization,
      },
      storedResults: peeked,
      configMatchInfo: matchInfo ? {
        topoKey: matchInfo.topoKey,
        configMatches: matchInfo.configMatches,
      } : null,
      cacheHit: hit ? { _cached: true, cacheSource: hit._cacheSource } : null,
    };
  });

  console.log(JSON.stringify(debug, null, 2));
  await browser.close();
}
main().catch(e => { console.error(e); process.exit(1); });
