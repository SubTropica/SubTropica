/**
 * Mobile UI smoke test (iPhone viewport):
 *   - Library FAB visible, opens browser
 *   - Overflow (⋯) menu visible, opens menu
 *   - Config opens via overflow menu (bottom sheet)
 *   - Touch tap on canvas adds a vertex
 *   - Two-finger pinch zooms
 * Run from ui/: node test-mobile-smoke.mjs (with python3 -m http.server 8089 running)
 */
import { createRequire } from 'module';
const require = createRequire(import.meta.url);
const pw = await import('/Users/smizera/.npm/_npx/9833c18b2d85bc59/node_modules/playwright/index.mjs');
const { chromium, devices } = pw;

const URL = 'http://localhost:8089/index.html';
const fail = (msg) => { console.error('FAIL:', msg); process.exitCode = 1; };
const pass = (msg) => console.log('PASS:', msg);

async function main() {
  const browser = await chromium.launch({ headless: true });
  const ctx = await browser.newContext({
    ...devices['iPhone 13'],
    hasTouch: true,
  });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', err => errors.push(err.message));
  page.on('console', msg => { if (msg.type() === 'error') errors.push(`console.error: ${msg.text()}`); });
  page.on('response', resp => { if (resp.status() === 404) errors.push(`404: ${resp.url()}`); });

  // Skip onboarding tour so touch interactions aren't intercepted.
  await ctx.addInitScript(() => {
    try { localStorage.setItem('subtropica.tour.seen', '1'); } catch (_) {}
  });
  await page.goto(URL, { waitUntil: 'networkidle' });
  await page.waitForTimeout(500);

  // 1. Mobile tab bar visible; Library FAB hidden at ≤480px (subsumed by
  //    the tab bar's Library tab). FAB stays in DOM for 481–768px.
  const tabbarVisible = await page.evaluate(() => {
    const t = document.getElementById('mobile-tabbar');
    return t && getComputedStyle(t).display !== 'none';
  });
  if (!tabbarVisible) return fail('Mobile tab bar not visible at iPhone 13');
  pass('Mobile tab bar visible');
  const fabHidden = await page.evaluate(() => {
    const f = document.getElementById('library-fab');
    return !f || getComputedStyle(f).display === 'none';
  });
  if (!fabHidden) return fail('Library FAB should be hidden when tab bar is active');
  pass('Library FAB hidden (subsumed by Library tab)');

  // Tap Library tab → opens browser overlay
  await page.tap('[data-tab="library"]');
  await page.waitForTimeout(400);
  const browserOpen = await page.evaluate(() => {
    const ov = document.getElementById('browser-overlay');
    return ov && ov.classList.contains('visible');
  });
  if (!browserOpen) return fail('Library overlay did not open from Library tab');
  pass('Library overlay opens from Library tab');
  // Active-state check
  const libTabActive = await page.evaluate(() =>
    document.querySelector('[data-tab="library"]').classList.contains('mobile-tab-active'));
  if (!libTabActive) return fail('Library tab should be marked active when overlay is open');
  pass('Library tab marked active while overlay is open');
  // Close
  await page.tap('#browser-close');
  await page.waitForTimeout(400);

  // 2. Overflow menu
  const ovBtn = await page.$('#overflow-btn');
  if (!ovBtn || !await ovBtn.isVisible()) return fail('Overflow ⋯ button not visible');
  pass('Overflow ⋯ button visible');

  await ovBtn.tap();
  await page.waitForTimeout(200);
  const menuOpen = await page.evaluate(() => document.getElementById('overflow-dropdown').classList.contains('open'));
  if (!menuOpen) return fail('Overflow menu did not open on tap');
  pass('Overflow menu opens on tap');

  // 3. Tap "Configure" → opens bottom sheet
  await page.tap('[data-action="config"]');
  await page.waitForTimeout(400);
  const cfgOpen = await page.evaluate(() => document.getElementById('config-panel').classList.contains('open'));
  if (!cfgOpen) return fail('Config panel did not open from overflow menu');
  pass('Config bottom sheet opens from overflow → Configure');

  // Verify the panel is positioned as a bottom sheet (top > screen midline)
  const cfgBox = await page.evaluate(() => {
    const el = document.getElementById('config-panel');
    const r = el.getBoundingClientRect();
    return { top: r.top, bottom: r.bottom, height: r.height, viewportH: window.innerHeight };
  });
  if (cfgBox.top < cfgBox.viewportH * 0.1) return fail(`Config panel not bottom-sheet: top=${cfgBox.top}, viewportH=${cfgBox.viewportH}`);
  // On ≤480px the panel now stops above the mobile tab bar (56px); on
  // 481–768px it still reaches the viewport bottom.
  const tabbarH = await page.evaluate(() => {
    const t = document.getElementById('mobile-tabbar');
    return t && getComputedStyle(t).display !== 'none' ? t.getBoundingClientRect().height : 0;
  });
  const expectedPanelBottom = cfgBox.viewportH - tabbarH;
  if (Math.abs(cfgBox.bottom - expectedPanelBottom) > 1) return fail(`Config panel bottom ${cfgBox.bottom} ≠ expected ${expectedPanelBottom} (viewportH ${cfgBox.viewportH} − tabbar ${tabbarH})`);
  pass(`Config panel is a bottom sheet (top=${cfgBox.top.toFixed(0)}, viewportH=${cfgBox.viewportH})`);

  // Tap the close (X) on the sheet
  await page.tap('#config-sheet-close');
  await page.waitForTimeout(400);
  const cfgClosed = await page.evaluate(() => !document.getElementById('config-panel').classList.contains('open'));
  if (!cfgClosed) return fail('Config sheet did not close via X button');
  pass('Config sheet closes via X');

  // 4. Touch-draw a vertex on the canvas
  const before = await page.evaluate(() => window.state ? window.state.vertices.length : -1);
  // window.state may not be exposed; check via SVG vertex count instead.
  const beforeVerts = await page.$$eval('#vertex-layer > *', els => els.length);
  // Tap somewhere in the middle of the canvas
  const cBox = await page.evaluate(() => {
    const c = document.getElementById('draw-canvas');
    const r = c.getBoundingClientRect();
    return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
  });
  await page.touchscreen.tap(cBox.x, cBox.y);
  await page.waitForTimeout(300);
  const afterVerts = await page.$$eval('#vertex-layer > *', els => els.length);
  if (afterVerts <= beforeVerts) return fail(`Touch tap did not add a vertex: before=${beforeVerts}, after=${afterVerts}`);
  pass(`Touch tap added vertex (count ${beforeVerts} → ${afterVerts})`);

  // 5. Two-finger pinch zoom
  const initialZoom = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  // Synthesize a pinch via Page.dispatchEvent. Playwright doesn't have a built-in
  // multi-touch helper; we use CDPSession to dispatch raw touch events.
  const client = await page.context().newCDPSession(page);
  const cx = cBox.x, cy = cBox.y;
  // Two pointers, 100px apart, then move to 200px apart (zoom in)
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [
      { x: cx - 50, y: cy, id: 1 },
      { x: cx + 50, y: cy, id: 2 },
    ],
  });
  await page.waitForTimeout(80);
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchMove',
    touchPoints: [
      { x: cx - 100, y: cy, id: 1 },
      { x: cx + 100, y: cy, id: 2 },
    ],
  });
  await page.waitForTimeout(80);
  await client.send('Input.dispatchTouchEvent', {
    type: 'touchEnd',
    touchPoints: [],
  });
  await page.waitForTimeout(200);
  const afterZoom = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  if (!(afterZoom > initialZoom + 5)) return fail(`Pinch did not increase zoom: ${initialZoom}% → ${afterZoom}%`);
  pass(`Pinch zoomed in (${initialZoom}% → ${afterZoom}%)`);

  // 6. Phase 1 — .notif-stack hidden on mobile; notif-chip + sheet work
  const stackHidden = await page.evaluate(() => {
    const s = document.getElementById('notif-stack');
    return getComputedStyle(s).display === 'none';
  });
  if (!stackHidden) return fail('.notif-stack should be display:none on iPhone 13 viewport');
  pass('.notif-stack hidden on mobile');

  // Load a diagram via Library tab → first card → Load to editor
  await page.tap('[data-tab="library"]');
  await page.waitForTimeout(400);
  const firstCard = await page.$('.browser-toast');
  if (firstCard) {
    await firstCard.tap();
    await page.waitForTimeout(500);
    // Prefer the mobile sticky CTA on phone viewports; fall back to the
    // generic Load button (which is what desktop runs will hit).
    let loadBtn = await page.$('.detail-mobile-cta-btn');
    if (!loadBtn || !await loadBtn.isVisible()) {
      loadBtn = await page.$('.popup-hero-actions .popup-load-btn');
    }
    if (loadBtn && await loadBtn.isVisible()) {
      await loadBtn.tap();
      await page.waitForTimeout(700);
    } else {
      const closeBtn = await page.$('.popup-close');
      if (closeBtn) await closeBtn.tap();
      await page.waitForTimeout(200);
      const browseClose = await page.$('#browser-close');
      if (browseClose) await browseClose.tap();
      await page.waitForTimeout(200);
    }
  }

  const chipInfo = await page.evaluate(() => {
    const c = document.getElementById('notif-chip');
    if (!c) return null;
    const label = document.getElementById('notif-chip-label');
    return { hidden: c.hidden, visible: !c.hidden && getComputedStyle(c).display !== 'none', text: (label ? label.textContent : '').trim() };
  });
  if (!chipInfo) return fail('#notif-chip not in DOM');
  if (!chipInfo.visible) return fail(`#notif-chip not visible after loading diagram (hidden=${chipInfo.hidden})`);
  if (!chipInfo.text) return fail('#notif-chip label is empty');
  pass(`notif-chip visible with label: "${chipInfo.text}"`);

  // Tap chip → sheet opens with toast(s) reparented
  await page.tap('#notif-chip');
  await page.waitForTimeout(400);
  const sheetState = await page.evaluate(() => {
    const s = document.getElementById('notif-sheet');
    const body = document.getElementById('notif-sheet-body');
    return { open: s.classList.contains('open'), toasts: body ? body.querySelectorAll('.notif-toast').length : 0 };
  });
  if (!sheetState.open) return fail('notif-sheet did not open on chip tap');
  if (sheetState.toasts === 0) return fail('notif-sheet opened but has no toasts');
  pass(`notif-sheet opened (${sheetState.toasts} toast(s) reparented)`);

  // Close via close button
  await page.tap('#notif-sheet-close');
  await page.waitForTimeout(400);
  const sheetClosed = await page.evaluate(() => {
    const s = document.getElementById('notif-sheet');
    return !s.classList.contains('open');
  });
  if (!sheetClosed) return fail('notif-sheet did not close via close button');
  pass('notif-sheet closes via ×');

  // 7. Phase 2 — integral card collapsed by default; bottom-bar fully visible
  const cardInfo = await page.evaluate(() => {
    const c = document.getElementById('integral-card');
    const bar = document.getElementById('bottom-bar');
    const cr = c.getBoundingClientRect();
    const br = bar.getBoundingClientRect();
    return {
      collapsed: c.classList.contains('collapsed'),
      cardBottom: cr.bottom, cardTop: cr.top,
      barTop: br.top, barBottom: br.bottom,
    };
  });
  if (!cardInfo.collapsed) return fail('.integral-card should be collapsed by default on mobile');
  if (cardInfo.cardBottom > cardInfo.barTop + 2) return fail(`.integral-card overlaps .bottom-bar (card bottom=${cardInfo.cardBottom.toFixed(0)}, bar top=${cardInfo.barTop.toFixed(0)})`);
  pass(`.integral-card collapsed + above .bottom-bar (gap ${(cardInfo.barTop - cardInfo.cardBottom).toFixed(0)}px)`);

  // Zoom buttons tappable (not hidden behind integral card)
  const zoomBtnVisible = await page.evaluate(() => {
    const btn = document.getElementById('zoom-in-btn');
    if (!btn) return false;
    const r = btn.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    // elementFromPoint at center should hit btn (or a child), not the integral card
    const cx = r.left + r.width / 2, cy = r.top + r.height / 2;
    const hit = document.elementFromPoint(cx, cy);
    return hit === btn || btn.contains(hit);
  });
  if (!zoomBtnVisible) return fail('#zoom-in-btn not tappable (something covers it)');
  pass('#zoom-in-btn tappable (bottom-bar not covered)');

  // Chevron expands the card
  await page.tap('#integral-card-toggle');
  await page.waitForTimeout(400);
  const expanded = await page.evaluate(() => !document.getElementById('integral-card').classList.contains('collapsed'));
  if (!expanded) return fail('Integral card did not expand on chevron tap');
  pass('Integral card expands on chevron tap');
  await page.tap('#integral-card-toggle');
  await page.waitForTimeout(300);

  // Mass legend does not overlap the mobile tab bar
  const barCheck = await page.evaluate(() => {
    const tab = document.getElementById('mobile-tabbar');
    const leg = document.getElementById('mass-legend');
    if (!leg) return { leg: false };
    const l = leg.getBoundingClientRect();
    const t = tab ? tab.getBoundingClientRect() : null;
    return {
      leg: true,
      legBottom: l.bottom,
      tabTop: t ? t.top : null,
      overlap: t ? l.bottom > t.top : false,
    };
  });
  if (barCheck.leg && barCheck.overlap) return fail(`Mass legend overlaps tab bar (legend bottom=${barCheck.legBottom}, tab top=${barCheck.tabTop})`);
  if (barCheck.leg) pass(`Mass legend clear of tab bar (legend bottom=${barCheck.legBottom.toFixed(0)}, tab top=${barCheck.tabTop !== null ? barCheck.tabTop.toFixed(0) : 'n/a'})`);

  // 8. Phase 3 — toolbar slim + Delete mode via overflow menu
  const tbState = await page.evaluate(() => {
    const nickel = document.getElementById('nickel-readout');
    const modeDel = document.getElementById('mode-delete');
    const badge = document.querySelector('.title-badge-mobile');
    const title = document.getElementById('app-title');
    const tb = document.getElementById('toolbar');
    const tbr = tb.getBoundingClientRect();
    const lastChild = tb.querySelector('.toolbar-right').lastElementChild;
    const lcr = lastChild.getBoundingClientRect();
    return {
      nickelHidden: getComputedStyle(nickel).display === 'none',
      modeDelHidden: getComputedStyle(modeDel).display === 'none',
      badgeVisible: badge && getComputedStyle(badge).display !== 'none',
      titleClipped: title.scrollWidth > title.clientWidth + 1,
      toolbarOverflow: lcr.right > tbr.right + 1,
    };
  });
  if (!tbState.nickelHidden) return fail('Nickel readout should be hidden on ≤480px');
  if (!tbState.modeDelHidden) return fail('#mode-delete should be hidden on ≤480px');
  if (!tbState.badgeVisible) return fail('"Mobile" badge should be visible on mobile');
  if (tbState.titleClipped) return fail('App title text is clipped');
  if (tbState.toolbarOverflow) return fail('Toolbar overflows viewport');
  pass('Toolbar slim: Nickel + Delete hidden, "Mobile" badge visible, no clipping');

  // Delete mode via overflow menu
  await page.tap('#overflow-btn');
  await page.waitForTimeout(200);
  await page.tap('[data-action="mode-delete"]');
  await page.waitForTimeout(200);
  const delActive = await page.evaluate(() => {
    const c = document.getElementById('draw-canvas');
    const it = document.querySelector('[data-action="mode-delete"]');
    return {
      canvasMode: c.classList.contains('mode-delete'),
      itemActive: it.classList.contains('overflow-mode-active'),
      itemAria: it.getAttribute('aria-checked') === 'true',
    };
  });
  if (!delActive.canvasMode) return fail('Delete mode did not activate via overflow');
  if (!delActive.itemActive || !delActive.itemAria) return fail('Overflow Delete item not marked active');
  pass('Delete mode reachable via overflow menu');
  // Return to draw mode
  await page.tap('#overflow-btn');
  await page.waitForTimeout(200);
  await page.tap('[data-action="mode-draw"]');
  await page.waitForTimeout(200);

  // 9. Phase 4 — auto-fit loaded diagram on mobile (zoom above 100%, bbox centred)
  const fitInfo = await page.evaluate(() => {
    const z = parseFloat(document.getElementById('zoom-display').textContent);
    const layer = document.getElementById('vertex-layer');
    const kids = layer ? layer.children.length : 0;
    let rect = null;
    if (kids > 0) {
      const r = layer.getBoundingClientRect();
      rect = { w: r.width, h: r.height, vw: window.innerWidth, vh: window.innerHeight };
    }
    return { zoom: z, rect };
  });
  if (!(fitInfo.zoom > 100)) return fail(`Loaded diagram zoom should be >100% on mobile (got ${fitInfo.zoom}%)`);
  pass(`Loaded diagram auto-fits (zoom=${fitInfo.zoom}%)`);

  // 10. Phase 5 — detail popup fullscreen on mobile + sticky CTA
  await page.tap('#notif-chip');
  await page.waitForTimeout(400);
  const firstToast = await page.$('#notif-sheet-body .notif-toast.notif-config');
  if (!firstToast) return fail('No config toast inside sheet to open detail popup from');
  await firstToast.tap();
  await page.waitForTimeout(500);
  const dp = await page.evaluate(() => {
    const p = document.getElementById('detail-panel');
    const cta = document.querySelector('.detail-mobile-cta');
    const ctaBtn = cta ? cta.querySelector('.detail-mobile-cta-btn') : null;
    if (!p) return null;
    const r = p.getBoundingClientRect();
    return {
      open: p.classList.contains('open'),
      width: r.width, height: r.height,
      vw: window.innerWidth, vh: window.innerHeight,
      ctaVisible: cta ? getComputedStyle(cta).display !== 'none' : false,
      ctaBottom: ctaBtn ? ctaBtn.getBoundingClientRect().bottom : null,
    };
  });
  if (!dp || !dp.open) return fail('Detail popup did not open');
  // Full-width; full-height minus the mobile tab bar (Phase 7 — the tab
  // bar is persistent across every overlay).
  const tabH = await page.evaluate(() => {
    const t = document.getElementById('mobile-tabbar');
    return t && getComputedStyle(t).display !== 'none' ? t.getBoundingClientRect().height : 0;
  });
  const expectedH = dp.vh - tabH;
  if (Math.abs(dp.width - dp.vw) > 1 || Math.abs(dp.height - expectedH) > 1) {
    return fail(`Detail popup not fullscreen on mobile: ${dp.width}×${dp.height} vs expected ${dp.vw}×${expectedH} (viewport ${dp.vh}, tab ${tabH})`);
  }
  pass(`Detail popup fullscreen above tab bar (${dp.width}×${dp.height})`);
  if (!dp.ctaVisible) return fail('.detail-mobile-cta should be visible on mobile');
  if (dp.ctaBottom === null || dp.ctaBottom > dp.vh + 1) return fail(`Sticky CTA outside viewport (bottom=${dp.ctaBottom}, vh=${dp.vh})`);
  pass(`Sticky "Load to editor" visible (button bottom=${dp.ctaBottom.toFixed(0)})`);

  // Tap the CTA → popup closes and diagram loads (hub for Phase 4's auto-fit)
  await page.tap('.detail-mobile-cta-btn');
  await page.waitForTimeout(500);
  const afterCta = await page.evaluate(() => ({
    popupOpen: document.getElementById('detail-panel').classList.contains('open'),
  }));
  if (afterCta.popupOpen) return fail('Detail popup did not close after sticky-CTA tap');
  pass('Sticky CTA closes popup + loads diagram');

  // 11. Phase 6 — gesture isolation: pinch inside an open config panel
  // must NOT zoom the canvas.
  await page.tap('#overflow-btn');
  await page.waitForTimeout(200);
  await page.tap('[data-action="config"]');
  await page.waitForTimeout(500);
  const cfgOpen2 = await page.evaluate(() => document.getElementById('config-panel').classList.contains('open'));
  if (!cfgOpen2) return fail('Config panel failed to open for gesture test');
  const zoomBefore = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  const cfgPanelBox = await page.evaluate(() => {
    const p = document.getElementById('config-panel');
    const r = p.getBoundingClientRect();
    return { cx: r.left + r.width / 2, cy: r.top + r.height / 2 };
  });
  const client2 = await page.context().newCDPSession(page);
  await client2.send('Input.dispatchTouchEvent', {
    type: 'touchStart',
    touchPoints: [
      { x: cfgPanelBox.cx - 40, y: cfgPanelBox.cy, id: 10 },
      { x: cfgPanelBox.cx + 40, y: cfgPanelBox.cy, id: 11 },
    ],
  });
  await page.waitForTimeout(50);
  await client2.send('Input.dispatchTouchEvent', {
    type: 'touchMove',
    touchPoints: [
      { x: cfgPanelBox.cx - 100, y: cfgPanelBox.cy, id: 10 },
      { x: cfgPanelBox.cx + 100, y: cfgPanelBox.cy, id: 11 },
    ],
  });
  await page.waitForTimeout(50);
  await client2.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
  await page.waitForTimeout(200);
  const zoomAfter = await page.evaluate(() => parseFloat(document.getElementById('zoom-display').textContent));
  if (Math.abs(zoomAfter - zoomBefore) > 1) {
    return fail(`Pinch inside config panel leaked to canvas zoom (${zoomBefore}% → ${zoomAfter}%)`);
  }
  pass(`Pinch inside config panel does not leak to canvas (zoom stayed ${zoomAfter}%)`);
  // Close config panel
  await page.tap('#config-sheet-close');
  await page.waitForTimeout(300);

  // 12. Phase 7 — tab bar round-trip: Configure + Export tabs
  await page.tap('[data-tab="configure"]');
  await page.waitForTimeout(500);
  const cfgFromTab = await page.evaluate(() => ({
    cfgOpen: document.getElementById('config-panel').classList.contains('open'),
    active: document.querySelector('[data-tab="configure"]').classList.contains('mobile-tab-active'),
  }));
  if (!cfgFromTab.cfgOpen) return fail('Configure tab did not open config panel');
  if (!cfgFromTab.active) return fail('Configure tab not marked active');
  pass('Configure tab opens config panel + marks active');
  // Tap Draw tab — should close config and activate Draw
  await page.tap('[data-tab="draw"]');
  await page.waitForTimeout(400);
  const afterDraw = await page.evaluate(() => ({
    cfgOpen: document.getElementById('config-panel').classList.contains('open'),
    drawActive: document.querySelector('[data-tab="draw"]').classList.contains('mobile-tab-active'),
  }));
  if (afterDraw.cfgOpen) return fail('Tapping Draw tab should close config panel');
  if (!afterDraw.drawActive) return fail('Draw tab should be active after tapping it');
  pass('Draw tab closes config panel and activates');

  // Export tab opens the export dropdown
  await page.tap('[data-tab="export"]');
  await page.waitForTimeout(300);
  const expOpen = await page.evaluate(() => ({
    open: document.getElementById('export-dropdown').classList.contains('visible'),
    active: document.querySelector('[data-tab="export"]').classList.contains('mobile-tab-active'),
  }));
  if (!expOpen.open) return fail('Export tab did not open export dropdown');
  if (!expOpen.active) return fail('Export tab not marked active');
  pass('Export tab opens export dropdown + marks active');
  // Click outside to dismiss (existing document click handler)
  await page.tap('#draw-canvas', { position: { x: 50, y: 200 } });
  await page.waitForTimeout(300);

  // 13. Console errors check
  if (errors.length) {
    console.error('JS errors collected:');
    errors.forEach(e => console.error('  ' + e));
    fail('JS errors during test');
  } else {
    pass('No JS errors');
  }

  await browser.close();
}

main().catch(e => { console.error(e); process.exit(2); });
