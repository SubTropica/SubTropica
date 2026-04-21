import { createRequire } from 'module';
const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium } = pw;
const browser = await chromium.launch({ headless: true });
const page = await browser.newPage();
const errors = [];
page.on('pageerror', err => errors.push(err.message));
await page.goto('http://localhost:8080', { waitUntil: 'networkidle' });
await page.waitForTimeout(1000);
// Open browser, switch to diagrams
const allBtns = await page.$$('button');
for (const btn of allBtns) { if ((await btn.textContent()).includes('Browse')) { await btn.click(); break; } }
await page.waitForTimeout(500);
const dt = await page.$('#browser-tab-diagrams'); if (dt) { await dt.click(); await page.waitForTimeout(500); }
const total = (await page.$$('.browser-toast')).length;
let fails = 0;
// Test every 10th entry to cover the range
for (let i = 0; i < total; i += 10) {
  const cards = await page.$$('.browser-toast');
  if (i >= cards.length) break;
  const errBefore = errors.length;
  try {
    await cards[i].click(); await page.waitForTimeout(300);
    const panel = await page.$('#detail-panel.open');
    if (errors.length > errBefore) { fails++; console.log(`FAIL [${i}]: ${errors.slice(errBefore).join('; ').slice(0,150)}`); }
    else if (!panel) { fails++; console.log(`FAIL [${i}]: panel not open`); }
    const cb = await page.$('#detail-panel .popup-close') || await page.$('#detail-panel [class*="close"]');
    if (cb) await cb.click(); else await page.click('body', { position: { x:10, y:10 } });
    await page.waitForTimeout(150);
  } catch(e) { fails++; console.log(`FAIL [${i}]: ${e.message.slice(0,150)}`); }
}
console.log(`Tested ${Math.ceil(total/10)} of ${total} entries, ${fails} failures, ${errors.filter(e=>!e.includes('404')).length} JS errors`);
await browser.close();
process.exit(fails > 0 ? 1 : 0);
