const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium } = pw;
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage({ viewport: { width: 1400, height: 900 } });
const errors = [];
page.on('pageerror', err => errors.push(err.message));
await page.goto('http://localhost:8080', { waitUntil: 'networkidle' });
await page.waitForTimeout(2000);

// Open browser, switch to diagrams
await page.locator('text=Library').first().click({ timeout: 5000 });
await page.waitForTimeout(1000);
await page.locator('#browser-tab-diagrams').click({ timeout: 3000 });
await page.waitForTimeout(500);

// Enable "precomputed only" filter to show only starred entries
await page.evaluate(() => {
  const cb = document.getElementById('browser-precomputed-cb');
  if (cb && !cb.checked) { cb.checked = true; cb.dispatchEvent(new Event('change')); }
});
await page.waitForTimeout(500);

const cards = await page.$$('.browser-toast');
console.log(`Found ${cards.length} starred cards`);

for (let i = 0; i < cards.length; i++) {
  const allCards = await page.$$('.browser-toast');
  if (i >= allCards.length) break;
  
  const name = await allCards[i].evaluate(el => el.textContent.trim().slice(0, 60));
  
  await allCards[i].scrollIntoViewIfNeeded({ timeout: 2000 });
  await allCards[i].click({ timeout: 5000 });
  await page.waitForTimeout(600);
  
  // Take screenshot of the detail panel
  const panel = await page.$('#detail-panel');
  if (panel) {
    const fname = `/tmp/subtropica-audit/${String(i).padStart(2,'0')}_${name.replace(/[^a-zA-Z0-9]/g, '_').slice(0, 40)}.png`;
    await panel.screenshot({ path: fname });
    console.log(`  [${i}] ${name}`);
  } else {
    console.log(`  [${i}] PANEL NOT OPEN: ${name}`);
  }
  
  await page.keyboard.press('Escape');
  await page.waitForTimeout(200);
}

const jsErrs = errors.filter(e => !e.includes('404'));
if (jsErrs.length > 0) {
  console.log(`\nJS errors: ${jsErrs.length}`);
  [...new Set(jsErrs)].forEach(e => console.log(`  ${e.slice(0,200)}`));
}
await browser.close();
