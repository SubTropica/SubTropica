/**
 * Playwright test: click library entries and check for JS errors.
 * Run: npx playwright test test-clicks.mjs
 * Or: node test-clicks.mjs (uses playwright API directly)
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
// Resolve from npx cache
const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium } = pw;

const URL = 'http://localhost:8080';

async function main() {
  const browser = await chromium.launch({ headless: true });
  const page = await browser.newPage();

  // Collect JS errors
  const errors = [];
  page.on('pageerror', err => errors.push(err.message));
  page.on('console', msg => {
    if (msg.type() === 'error') errors.push(`console.error: ${msg.text()}`);
  });

  console.log('Loading page...');
  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(1000);

  // Open the library browser
  console.log('Opening library browser...');
  const browseBtn = await page.$('#browse-btn') || await page.$('button:has-text("Browse")');
  if (!browseBtn) {
    // Try the library icon/button
    const allBtns = await page.$$('button');
    for (const btn of allBtns) {
      const text = await btn.textContent();
      if (text.includes('Browse') || text.includes('Library') || text.includes('library')) {
        await btn.click();
        break;
      }
    }
  } else {
    await browseBtn.click();
  }
  await page.waitForTimeout(500);

  // Switch to Diagrams tab if available
  const diagTab = await page.$('#browser-tab-diagrams');
  if (diagTab) {
    await diagTab.click();
    await page.waitForTimeout(500);
  }

  // Get all diagram cards in the browser list
  let cards = await page.$$('.browser-toast');
  console.log(`Found ${cards.length} diagram cards`);

  if (cards.length === 0) {
    // Try topologies tab
    const topoTab = await page.$('#browser-tab-topos');
    if (topoTab) {
      await topoTab.click();
      await page.waitForTimeout(500);
    }
    cards = await page.$$('.browser-toast');
    console.log(`Found ${cards.length} topology cards after switching tabs`);
  }

  // Click first 20 entries and check for errors
  const maxTest = Math.min(cards.length, 30);
  let clickErrors = 0;

  for (let i = 0; i < maxTest; i++) {
    // Re-query cards since DOM may have changed
    cards = await page.$$('.browser-toast');
    if (i >= cards.length) break;

    const card = cards[i];
    const cardText = (await card.textContent()).slice(0, 60).trim();
    const errorsBefore = errors.length;

    try {
      await card.click();
      await page.waitForTimeout(300);

      // Check if detail panel opened
      const panel = await page.$('#detail-panel.open');
      const panelVisible = panel !== null;

      if (errors.length > errorsBefore) {
        const newErrors = errors.slice(errorsBefore);
        console.log(`  [${i}] CLICK ERROR on "${cardText}"`);
        for (const e of newErrors) console.log(`    -> ${e.slice(0, 200)}`);
        clickErrors++;
      } else if (!panelVisible) {
        console.log(`  [${i}] PANEL DID NOT OPEN for "${cardText}"`);
        clickErrors++;
      } else {
        console.log(`  [${i}] OK: "${cardText}"`);
      }

      // Close panel
      const closeBtn = await page.$('#detail-panel .popup-close') || await page.$('#detail-panel [class*="close"]');
      if (closeBtn) {
        await closeBtn.click();
        await page.waitForTimeout(200);
      } else {
        // Click outside to close
        await page.click('body', { position: { x: 10, y: 10 } });
        await page.waitForTimeout(200);
      }
    } catch (e) {
      console.log(`  [${i}] EXCEPTION on "${cardText}": ${e.message.slice(0, 200)}`);
      clickErrors++;
    }
  }

  console.log(`\n=== Summary ===`);
  console.log(`Tested: ${maxTest} entries`);
  console.log(`Click errors: ${clickErrors}`);
  console.log(`Total JS errors: ${errors.length}`);
  if (errors.length > 0) {
    console.log('\nAll JS errors:');
    // Deduplicate
    const unique = [...new Set(errors)];
    for (const e of unique) console.log(`  ${e.slice(0, 300)}`);
  }

  await browser.close();
  process.exit(clickErrors > 0 ? 1 : 0);
}

main().catch(e => { console.error(e); process.exit(1); });
