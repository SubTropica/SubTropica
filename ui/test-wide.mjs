const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium } = pw;
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
const errors = [];
page.on('pageerror', err => errors.push(err.message));
await page.goto('http://localhost:8080', { waitUntil: 'networkidle' });
await page.waitForTimeout(2000);

// Click Library button
await page.locator('text=Library').first().click({ timeout: 5000 });
await page.waitForTimeout(1000);

// Screenshot after opening
await page.screenshot({ path: '/tmp/subtropica-library.png', fullPage: true });

// Switch to diagrams tab
await page.locator('#browser-tab-diagrams').click({ timeout: 3000 }).catch(() => {});
await page.waitForTimeout(500);

let cardCount = (await page.$$('.browser-toast')).length;
console.log(`Found ${cardCount} cards`);

let fails = 0, tested = 0;
const step = Math.max(1, Math.floor(cardCount / 50));

for (let i = 0; i < cardCount; i += step) {
  const cards = await page.$$('.browser-toast');
  if (i >= cards.length) break;
  const errBefore = errors.length;
  
  try {
    await cards[i].scrollIntoViewIfNeeded({ timeout: 2000 });
    await cards[i].click({ timeout: 3000 });
    await page.waitForTimeout(350);
  } catch(e) {
    tested++; continue;
  }
  
  const newErrs = errors.slice(errBefore).filter(e => !e.includes('404'));
  if (newErrs.length > 0) {
    const name = await cards[i].textContent().catch(() => '?');
    fails++;
    console.log(`FAIL [${i}] "${(name||'').trim().slice(0,50)}": ${newErrs[0].slice(0,200)}`);
  }
  tested++;
  await page.keyboard.press('Escape');
  await page.waitForTimeout(150);
}

const jsErrs = [...new Set(errors.filter(e => !e.includes('404')))];
console.log(`\nTested ${tested}/${cardCount}, ${fails} failures, ${jsErrs.length} unique JS errors`);
jsErrs.forEach(e => console.log(`  ${e.slice(0,250)}`));
await browser.close();
process.exit(fails > 0 ? 1 : 0);
