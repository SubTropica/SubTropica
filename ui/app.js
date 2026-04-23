/**
 * app.js — SubTropica
 *
 * Unified Feynman diagram editor with live reactive topology matching.
 * Runs in two modes:
 *   - Online mode (standalone): library lookup, diagram drawing, export
 *   - Full mode (Mathematica kernel): online + configuration + integration
 */

import { Nickel, canonicalize, LEG, isConnected } from './nickel.js';
import { renderNotebook } from './notebook/render.mjs';
import { wlToNb } from './notebook/wl-to-nb.mjs';

// Starter-notebook template: fetched lazily on the first download. Cached so
// subsequent downloads within the same session are instant.
let _starterTemplatePromise = null;
function getStarterTemplate() {
  if (!_starterTemplatePromise) {
    _starterTemplatePromise = fetch('notebook/template.wl', { cache: 'force-cache' })
      .then(r => { if (!r.ok) throw new Error('template fetch failed'); return r.text(); });
  }
  return _starterTemplatePromise;
}

// ─── Hover tooltip system ──────────────────────────────────────────
// Elements opt in via `data-tip="plain text"` or `data-tip-html="<rich>"`.
// Works for dynamically-built DOM via event delegation — sprinkle the attr
// wherever, no registration needed. Show delay keeps tips unobtrusive.
const TIP_DELAY_MS = 500;
let _tipEl = null, _tipTimer = null, _tipTarget = null;
function _ensureTipEl() {
  if (_tipEl) return _tipEl;
  _tipEl = document.createElement('div');
  _tipEl.className = 'tip-popup';
  _tipEl.setAttribute('role', 'tooltip');
  document.body.appendChild(_tipEl);
  return _tipEl;
}
function _positionTip(target) {
  const el = _ensureTipEl();
  const tr = target.getBoundingClientRect();
  const er = el.getBoundingClientRect();
  const margin = 8;
  let top = tr.top - er.height - margin;
  if (top < margin) top = tr.bottom + margin;
  let left = tr.left + tr.width / 2 - er.width / 2;
  left = Math.max(margin, Math.min(left, window.innerWidth - er.width - margin));
  el.style.top = top + 'px';
  el.style.left = left + 'px';
}
function _showTip(target) {
  const el = _ensureTipEl();
  const text = target.dataset.tip;
  const html = target.dataset.tipHtml;
  if (!text && !html) return;
  if (html) el.innerHTML = html; else el.textContent = text;
  _tipTarget = target;
  requestAnimationFrame(() => { _positionTip(target); el.classList.add('visible'); });
}
function _hideTip() {
  if (_tipEl) _tipEl.classList.remove('visible');
  _tipTarget = null;
}
document.addEventListener('mouseover', evt => {
  const target = evt.target.closest && evt.target.closest('[data-tip], [data-tip-html]');
  if (!target || target === _tipTarget) return;
  clearTimeout(_tipTimer);
  _tipTimer = setTimeout(() => _showTip(target), TIP_DELAY_MS);
});
document.addEventListener('mouseout', evt => {
  const target = evt.target.closest && evt.target.closest('[data-tip], [data-tip-html]');
  if (!target) return;
  if (evt.relatedTarget && target.contains(evt.relatedTarget)) return;
  clearTimeout(_tipTimer);
  _hideTip();
});
document.addEventListener('scroll', _hideTip, true);
document.addEventListener('click', _hideTip, true);

// ─── Onboarding tour ──────────────────────────────────────────────────
// First-visit guided walkthrough covering the non-obvious affordances:
// how to draw, the edge-midpoint bubble, pan/zoom, Library/Integrate
// entry points. `localStorage.subtropica.tour.seen === '1'` suppresses.
// Always available via window.__st.startTour() for manual replay.
const TOUR_STORAGE_KEY = 'subtropica.tour.seen';

let _tourDrawPromise = null;
// Abort flag checked between awaits in animateDoubleBox — without it, Skip
// only hides the overlay while the demo keeps pushing vertices/edges into
// state for the remaining ~6 s of scripted gestures. _tourDrewDemo records
// whether the tour actually drew (vs. finding the canvas already populated),
// so close knows whether it is safe to wipe the canvas.
let _tourAborted = false;
let _tourDrewDemo = false;
// Edge index chosen by the "Edit an edge" step — the follow-up "Set the
// mass" step reads this so the two phases target the same leg without
// repeating the position search.
let _tourMassEdgeIdx = -1;
function _tourSleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// Tour layout pivots on mobile vs desktop: several elements the desktop
// tour points at (Nickel readout, top-toolbar Export button, Library FAB,
// right-side toast stack) are hidden on phones; others (chip, tab bar,
// overflow menu) take their place. Each step may declare a `mobile` object
// that overrides { selector, title, body, placement, prepare, skip } when
// _isMobileTour() is true; _tourStepProp reads through the override.
function _isMobileTour() {
  return typeof matchMedia === 'function' && matchMedia('(max-width: 768px)').matches;
}
function _tourStepProp(step, name) {
  if (_isMobileTour() && step.mobile && step.mobile[name] !== undefined) return step.mobile[name];
  return step[name];
}

// ── Ghost cursor ──────────────────────────────────────────────────────
// A small marker that tweens across the canvas during the honest-draw demo,
// so users see the double-box emerge as if someone were using the app. Lives
// in the DOM layer above the spotlight (z-index 10002).
let _tourGhostEl = null;
function _tourMakeGhost() {
  if (_tourGhostEl) return _tourGhostEl;
  const g = document.createElement('div');
  g.className = 'tour-ghost';
  document.body.appendChild(g);
  _tourGhostEl = g;
  return g;
}
function _tourRemoveGhost() {
  if (_tourGhostEl && _tourGhostEl.parentNode) _tourGhostEl.parentNode.removeChild(_tourGhostEl);
  _tourGhostEl = null;
}
function _svgToScreen(pt) {
  const sp = canvas.createSVGPoint();
  sp.x = pt.x; sp.y = pt.y;
  const m = canvas.getScreenCTM();
  if (!m) return { x: 0, y: 0 };
  const r = sp.matrixTransform(m);
  return { x: r.x, y: r.y };
}
function _tourEase(t) { return 1 - Math.pow(1 - t, 3); }
function _tourGhostSet(svgPt) {
  const g = _tourGhostEl; if (!g) return;
  const s = _svgToScreen(svgPt);
  g.style.left = s.x + 'px';
  g.style.top = s.y + 'px';
}
function _tourGhostTo(svgPt, dur = 320) {
  return new Promise(resolve => {
    const g = _tourGhostEl; if (!g) { resolve(); return; }
    const target = _svgToScreen(svgPt);
    const fromX = parseFloat(g.style.left) || target.x;
    const fromY = parseFloat(g.style.top) || target.y;
    const t0 = performance.now();
    const tick = now => {
      const t = Math.min((now - t0) / dur, 1);
      const e = _tourEase(t);
      g.style.left = (fromX + (target.x - fromX) * e) + 'px';
      g.style.top  = (fromY + (target.y - fromY) * e) + 'px';
      if (t < 1) requestAnimationFrame(tick);
      else resolve();
    };
    requestAnimationFrame(tick);
  });
}
// Screen-space tween for HTML targets (mass-picker buttons, panel rows) —
// the SVG→screen conversion in _tourGhostTo is wrong for these since they
// don't live in the canvas coordinate system.
function _tourGhostToScreen(screenPt, dur = 320) {
  return new Promise(resolve => {
    const g = _tourGhostEl; if (!g) { resolve(); return; }
    const fX = parseFloat(g.style.left);
    const fY = parseFloat(g.style.top);
    const fromX = Number.isFinite(fX) ? fX : screenPt.x;
    const fromY = Number.isFinite(fY) ? fY : screenPt.y;
    const t0 = performance.now();
    const tick = now => {
      const t = Math.min((now - t0) / dur, 1);
      const e = _tourEase(t);
      g.style.left = (fromX + (screenPt.x - fromX) * e) + 'px';
      g.style.top  = (fromY + (screenPt.y - fromY) * e) + 'px';
      if (t < 1) requestAnimationFrame(tick);
      else resolve();
    };
    requestAnimationFrame(tick);
  });
}
function _tourGhostPress(on) {
  const g = _tourGhostEl; if (!g) return;
  g.classList.toggle('pressed', !!on);
}
// Drag tween: interpolates in SVG coords and updates BOTH the ghost's
// screen position AND state.edgeDragPos every frame, so renderEdgePreview
// draws the preview line tracking the cursor — exactly like a real drag.
function _tourDragTween(svgFrom, svgTo, dur = 220) {
  return new Promise(resolve => {
    const g = _tourGhostEl; if (!g) { resolve(); return; }
    const t0 = performance.now();
    const tick = now => {
      const t = Math.min((now - t0) / dur, 1);
      const e = _tourEase(t);
      const cur = {
        x: svgFrom.x + (svgTo.x - svgFrom.x) * e,
        y: svgFrom.y + (svgTo.y - svgFrom.y) * e,
      };
      const s = _svgToScreen(cur);
      g.style.left = s.x + 'px';
      g.style.top  = s.y + 'px';
      state.edgeDragPos = cur;
      renderEdgePreview();
      if (t < 1) requestAnimationFrame(tick);
      else resolve();
    };
    requestAnimationFrame(tick);
  });
}

// Honest double-box animation: every gesture adds at most one vertex plus
// one edge, starting from an existing vertex, so the diagram is connected
// at every intermediate frame (matches how a real user would draw it). A
// ghost cursor traces each drag between action points.
//
// Canvas viewBox `-6 -4 12 8` (x: -6..6, y: -4..4, y-down).
async function animateDoubleBox() {
  if (!state || !Array.isArray(state.vertices)) return;
  if (state.vertices.length > 0) return; // don't stomp on user's work
  _tourDrewDemo = true;

  const V = {
    TL: { x: -2.4, y: -1.2 }, TM: { x: 0.0, y: -1.2 }, TR: { x: 2.4, y: -1.2 },
    BL: { x: -2.4, y:  1.2 }, BM: { x: 0.0, y:  1.2 }, BR: { x: 2.4, y:  1.2 },
    extTL: { x: -4.5, y: -2.8 }, extTR: { x: 4.5, y: -2.8 },
    extBL: { x: -4.5, y:  2.8 }, extBR: { x: 4.5, y:  2.8 },
  };

  // Minimal edge record — include all optional string fields so downstream
  // renderers that call .match on them don't choke on undefined.
  const newEdge = (a, b) => ({
    a, b, mass: 0, style: 'solid',
    massLabel: '', edgeLabel: '', extMomLabel: '', propExponent: 1,
  });

  const ghost = _tourMakeGhost();
  // Start the ghost just outside the canvas so it feels like it flies in.
  _tourGhostSet({ x: -5.5, y: -3.5 });
  requestAnimationFrame(() => ghost.classList.add('visible'));
  await _tourSleep(180);

  // Place the first vertex with a single click.
  const placeFirst = async () => {
    if (_tourAborted) return;
    await _tourGhostTo(V.TL, 360);
    if (_tourAborted) return;
    _tourGhostPress(true); await _tourSleep(70);
    if (_tourAborted) return;
    state.vertices.push(V.TL);
    state.newVertexIdx = 0;
    render();
    _tourGhostPress(false); await _tourSleep(110);
  };

  // Drag from an existing vertex (by index) to either a new point (spawns
  // a vertex) or another existing vertex (edge only, no new vertex). The
  // edge preview line tracks the ghost cursor in real time, mirroring how
  // a real drag looks (uses the same renderEdgePreview as the user does).
  const drag = async (fromIdx, opts) => {
    if (_tourAborted) return;
    const fromPos = state.vertices[fromIdx];
    if (!fromPos) return; // state cleared by abort between awaits
    await _tourGhostTo(fromPos, 170);
    if (_tourAborted) return;
    _tourGhostPress(true); await _tourSleep(50);
    if (_tourAborted) return;

    // Engage the live edge preview (renders to previewLayer each frame).
    state.edgeDragFrom = fromIdx;
    state.edgeDragPos = { x: fromPos.x, y: fromPos.y };
    renderEdgePreview();

    const toSvg = opts.toNew || state.vertices[opts.toExisting];
    await _tourDragTween(fromPos, toSvg, 210);
    if (_tourAborted) return;

    // Commit: tear down the preview, then push the real edge / vertex.
    state.edgeDragFrom = null;
    state.edgeDragPos = null;
    if (opts.toNew) {
      state.vertices.push({ x: opts.toNew.x, y: opts.toNew.y });
      const newIdx = state.vertices.length - 1;
      state.newVertexIdx = newIdx;
      state.edges.push(newEdge(fromIdx, newIdx));
      state.newEdgeIdx = state.edges.length - 1;
    } else {
      state.edges.push(newEdge(fromIdx, opts.toExisting));
      state.newEdgeIdx = state.edges.length - 1;
      state.newVertexIdx = -1;
    }
    renderEdgePreview(); // clears the preview line now that edgeDragFrom is null
    render();

    _tourGhostPress(false); await _tourSleep(85);
  };

  await placeFirst();
  // Trace the outer box as a single path: TL→TM→TR→BR→BM→BL, then close.
  await drag(0, { toNew: V.TM });       // v1
  await drag(1, { toNew: V.TR });       // v2
  await drag(2, { toNew: V.BR });       // v3
  await drag(3, { toNew: V.BM });       // v4
  await drag(4, { toNew: V.BL });       // v5
  await drag(5, { toExisting: 0 });     // close outer box
  // Internal rung: v1 (top-mid) ↔ v4 (bot-mid).
  await drag(1, { toExisting: 4 });
  // External legs: place the external vertex first (click in empty space
  // outside the diagram), then drag FROM it TO the internal vertex.
  // This gives the visual impression of drawing outside-to-inside —
  // matching how one draws an external leg in practice.
  const placeExt = async (pos) => {
    if (_tourAborted) return;
    await _tourGhostTo(pos, 320);
    if (_tourAborted) return;
    _tourGhostPress(true); await _tourSleep(70);
    if (_tourAborted) return;
    state.vertices.push({ x: pos.x, y: pos.y });
    state.newVertexIdx = state.vertices.length - 1;
    render();
    _tourGhostPress(false); await _tourSleep(110);
  };
  await placeExt(V.extTL); await drag(6, { toExisting: 0 });  // ext→TL
  await placeExt(V.extTR); await drag(7, { toExisting: 2 });  // ext→TR
  await placeExt(V.extBL); await drag(8, { toExisting: 5 });  // ext→BL
  await placeExt(V.extBR); await drag(9, { toExisting: 3 });  // ext→BR

  state.newVertexIdx = -1;
  state.newEdgeIdx = -1;
  if (typeof onGraphChanged === 'function') onGraphChanged();
  render();

  // Hide ghost with a tiny delay so the final drop settles first.
  await _tourSleep(200);
  if (_tourGhostEl) _tourGhostEl.classList.remove('visible');
  setTimeout(_tourRemoveGhost, 350);
}

// Each step can opt-in to a `prepare` hook that prepares the UI so the
// target is actually visible & meaningful (e.g. draw a demo diagram before
// pointing at the Nickel readout, or expand the collapsed integral card).
// Prepare hooks may be async — the spotlight/card placement waits for them.
// Steps follow the visual layout so the spotlight travels in one direction
// rather than hopping around: canvas → top-left toolbar → top-right toolbar
// → bottom bar → bottom-right integral card → finish. Each step can opt in
// to a `prepare` hook that sets the UI up before the spotlight measures its
// target (e.g. draw the demo, expand the collapsed integral card).
const TOUR_STEPS = [
  {
    selector: null, placement: 'center',
    title: 'Welcome to SubTropica',
    body: () => backendMode === 'full'
      ? 'Connected to your Mathematica kernel. A quick tour of the non-obvious bits \u2014 skip any time with <code>Esc</code>.'
      : 'A quick tour of the non-obvious bits. You can skip any time with <code>Esc</code>.',
  },
  {
    // Kick off the honest double-box animation. The drawing takes ~6s; it
    // runs in the background while the user reads this card (awaitPrepare
    // is false so the card appears immediately).
    prepare: () => {
      if (!_tourDrawPromise) _tourDrawPromise = animateDoubleBox();
      return _tourDrawPromise;
    },
    awaitPrepare: false,
    selector: '#draw-canvas', placement: 'below-left',
    title: 'Draw a diagram',
    body: 'Watch \u2014 I\u2019m tracing out a <strong>double-box diagram</strong>. In your own work you\u2019d <strong>click</strong> in empty space to place a vertex and <strong>drag</strong> from a vertex to extend an edge. External legs come from dragging a vertex into empty space.',
  },
  {
    // Phase 1 of the mass-picker demo: ghost-click the top-right external
    // leg's bubble so the popup appears. The card then explains what the
    // picker is. The actual M-option click happens in the NEXT step so the
    // user has time to read before the mass is applied.
    prepare: async () => {
      await _tourDrawPromise;
      if (_tourAborted) return;

      // Stash the chosen edge index on the state so the follow-up step can
      // find it without repeating the position search.
      const near = (v, x, y) => v && Math.abs(v.x - x) < 0.4 && Math.abs(v.y - y) < 0.4;
      const edgeIdx = state.edges.findIndex(e => {
        const va = state.vertices[e.a], vb = state.vertices[e.b];
        return (near(va, 4.5, -2.8) && near(vb, 2.4, -1.2)) ||
               (near(vb, 4.5, -2.8) && near(va, 2.4, -1.2));
      });
      if (edgeIdx < 0) return;
      _tourMassEdgeIdx = edgeIdx;
      const ed = state.edges[edgeIdx];
      const va = state.vertices[ed.a], vb = state.vertices[ed.b];
      const mid = { x: (va.x + vb.x) / 2, y: (va.y + vb.y) / 2 };

      // If the picker is already visible (e.g., user is navigating Back
      // from the next step), skip the ghost animation — it would feel
      // redundant to replay the bubble-click tween.
      const picker = document.getElementById('mass-picker');
      if (picker && picker.classList.contains('visible')) return;

      _tourMakeGhost();
      if (!_tourGhostEl.style.left) _tourGhostSet({ x: 4.5, y: -2.8 });
      _tourGhostEl.classList.add('visible');
      await _tourSleep(80);
      if (_tourAborted) return;

      await _tourGhostTo(mid, 500);
      if (_tourAborted) return;
      _tourGhostPress(true); await _tourSleep(90);
      _tourGhostPress(false);

      openMassPicker(edgeIdx, mid.x, mid.y);
      // Let the picker render so its buttons have layout before we measure.
      await _tourSleep(260);
      if (_tourAborted) { closeMassPicker(); return; }

      if (_tourGhostEl) _tourGhostEl.classList.remove('visible');
    },
    awaitPrepare: true,
    selector: '#mass-picker',
    title: 'Edit an edge',
    body: 'Every edge has a small circle at its midpoint — <strong>click it</strong> to open this popup. From here you set the edge’s <strong>mass</strong>, <strong>style</strong>, <strong>propagator exponent</strong>, <strong>momentum label</strong>, or reverse the arrow on an external leg. External legs default to the <strong>M</strong> family; internal edges to <strong>m</strong>.',
  },
  {
    // Phase 2: ghost-click the "new M" option. This commits the mass and
    // makes the diagram a 1-mass double box — downstream steps (toast
    // matching, detail popup, first-paper preview) all resolve against
    // that config.
    prepare: async () => {
      if (_tourAborted) return;
      const edgeIdx = _tourMassEdgeIdx;
      if (edgeIdx == null || edgeIdx < 0) return;

      // Ensure the picker is open — if the user navigated back from a later
      // step, _tourShow's transition rule closed it; reopen without the
      // ghost bubble tween so the flow still makes sense.
      let picker = document.getElementById('mass-picker');
      if (!picker || !picker.classList.contains('visible')) {
        const ed = state.edges[edgeIdx];
        if (!ed) return;
        const va = state.vertices[ed.a], vb = state.vertices[ed.b];
        const mid = { x: (va.x + vb.x) / 2, y: (va.y + vb.y) / 2 };
        openMassPicker(edgeIdx, mid.x, mid.y);
        await _tourSleep(260);
        if (_tourAborted) { closeMassPicker(); return; }
        picker = document.getElementById('mass-picker');
      }

      // On an external leg, kind 'M' is primary and the first
      // `.mass-option-new` button is the new-M slot we want.
      const mBtn = picker && picker.querySelector('.mass-option-new');
      if (!mBtn) { setEdgeMass(edgeIdx, 1, 'M'); return; }

      _tourMakeGhost();
      const r = mBtn.getBoundingClientRect();
      const targetScreen = { x: r.left + r.width / 2, y: r.top + r.height / 2 };
      // If the ghost was hidden/removed, seed its position near the picker
      // so the inbound tween is visible rather than materialising on top
      // of the target.
      if (!_tourGhostEl.style.left) {
        _tourGhostEl.style.left = (targetScreen.x - 80) + 'px';
        _tourGhostEl.style.top  = (targetScreen.y - 40) + 'px';
      }
      _tourGhostEl.classList.add('visible');
      await _tourSleep(80);
      if (_tourAborted) { closeMassPicker(); return; }

      await _tourGhostToScreen(targetScreen, 420);
      if (_tourAborted) { closeMassPicker(); return; }
      _tourGhostPress(true); await _tourSleep(90);
      _tourGhostPress(false);
      mBtn.click();
      await _tourSleep(180);

      if (_tourGhostEl) _tourGhostEl.classList.remove('visible');
    },
    awaitPrepare: true,
    selector: '#mass-picker',
    title: 'Set the mass',
    body: 'Watch — I’m clicking the <strong>M</strong> option. For an external leg this means <em>p² = M²</em>, turning the diagram into a <strong>1-mass double box</strong>. A mass name input appears below once a slot is selected, so you can rename <code>M</code> to, e.g., <code>M_H</code>.',
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#mode-segment', placement: 'below',
    title: 'Draw vs delete',
    body: 'Toggle between draw and delete modes here. In delete mode, clicking a vertex or an edge removes it (the app keeps the graph connected).',
    mobile: {
      // On phones the mode toggle lives inside the overflow (\u22ef) menu along
      // with rotate / flip / auto-arrange / clear. Spotlight the \u22ef button
      // as the common entry point. Skip the draw-promise await (no reason
      // to wait ~6s for the demo animation to finish on mobile \u2014 the \u22ef
      // button exists from page load).
      prepare: () => {}, awaitPrepare: false,
      selector: '#overflow-btn', placement: 'below',
      title: 'Actions menu',
      body: 'Tap <strong>\u22ef</strong> for everything that doesn\u2019t fit in a four-tab nav: <strong>Draw / Delete mode</strong>, <strong>rotate</strong>, <strong>flip</strong>, <strong>auto-arrange</strong>, <strong>clear canvas</strong>.',
    },
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#rebalance-btn', placement: 'above',
    title: 'Auto-arrange',
    body: 'Force-directed re-layout \u2014 handy when your diagram gets tangled after editing. The icons to the left rotate and flip the drawing; hit <code>L</code> as a keyboard shortcut.',
    // Covered by the Actions-menu step on mobile; skip the dedicated
    // rebalance spotlight.
    mobile: { skip: true },
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#zoom-display', placement: 'above',
    title: 'Zoom',
    body: 'Zoom in and out of the canvas. The scroll wheel works too, and you can pan by dragging empty space.',
    mobile: {
      // No need to wait for the draw animation; zoom-display is always there.
      prepare: () => {}, awaitPrepare: false,
      body: 'Zoom in and out; <strong>pinch</strong> on the canvas does the same, and you can <strong>pan</strong> by dragging an empty area with one finger.',
    },
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#nickel-readout', placement: 'below',
    title: 'Topology fingerprint',
    body: 'Whenever your drawing is connected, its canonical <strong>Nickel index</strong> appears here. Hover it for the full definition \u2014 this is how SubTropica matches your diagram against the library.',
    // Nickel readout is hidden on \u2264480px; it still appears inside the
    // library detail popup. Not worth a dedicated step on phones.
    mobile: { skip: true },
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#export-menu-btn', placement: 'below',
    title: 'Export',
    body: 'Save the drawing as <strong>SVG, PNG, PDF, or TikZ</strong>, or copy a shareable link that encodes the diagram in the URL.',
    mobile: {
      // Export lives in the bottom tab bar on phones. Skip the draw-promise
      // await — the tab is present from page load.
      prepare: () => {}, awaitPrepare: false,
      selector: '[data-tab="export"]', placement: 'above',
      title: 'Export',
      body: 'Tap <strong>Export</strong> in the bottom bar to save as <strong>SVG, PNG, PDF, or TikZ</strong>, or copy a shareable link that encodes the diagram in the URL.',
    },
  },
  {
    prepare: async () => {
      await _tourDrawPromise;
      try { closeBrowser(); } catch {}
    },
    awaitPrepare: true,
    selector: '#browse-btn', placement: 'below',
    title: 'Browse the library',
    body: 'Several hundred topologies with masses and precomputed results. If your current diagram matches one, it lights up in the list.',
    mobile: {
      prepare: () => { try { closeBrowser(); } catch {} }, awaitPrepare: false,
      selector: '[data-tab="library"]', placement: 'above',
      title: 'Library tab',
      body: 'Tap <strong>Library</strong> in the bottom bar to browse several hundred topologies with masses and precomputed results. If your current diagram matches one, it lights up in the list.',
    },
  },
  {
    // Open the library overlay so users can see what they\u2019re browsing
    // through. We close it again on the next step so the toasts stay visible.
    prepare: async () => {
      await _tourDrawPromise;
      closeConfigPanel();
      closeDetailPanel();
      try { openBrowser(); } catch {}
      // Wait for the overlay to slide in AND the panel to finish its
      // CSS transition. getBoundingClientRect mid-transition returns a
      // partial rect, causing the spotlight to cover only part of the
      // panel.
      await new Promise(r => {
        const start = performance.now();
        let lastH = 0;
        const tick = () => {
          const panel = document.querySelector('.browser-panel');
          const ov = document.getElementById('browser-overlay');
          if (!ov || !ov.classList.contains('visible')) {
            if (performance.now() - start > 2000) return r();
            return requestAnimationFrame(tick);
          }
          // Wait until the panel's height stabilises (transition done).
          const h = panel ? panel.getBoundingClientRect().height : 0;
          if (h > 100 && Math.abs(h - lastH) < 1) return r();
          lastH = h;
          if (performance.now() - start > 2500) return r();
          requestAnimationFrame(tick);
        };
        tick();
      });
    },
    awaitPrepare: true,
    selector: '.browser-panel',
    title: 'Inside the library',
    body: 'Filter by loop count, leg count, mass scales, or whether a result has been computed. Switch to the <strong>Diagrams</strong> tab to see specific mass configurations grouped by topology, with thumbnails and \u03B5-orders.',
  },
  {
    // Close the library overlay and spotlight the matched config toast.
    // The first toast is the topology header; the second is the first
    // matching mass configuration — that\u2019s the one we want.
    prepare: async () => {
      await _tourDrawPromise;
      try { closeBrowser(); } catch {}
      closeDetailPanel();
      await new Promise(r => {
        const start = performance.now();
        const tick = () => {
          const toasts = document.querySelectorAll('#notif-stack .notif-toast');
          if (toasts.length >= 2) return r();
          if (performance.now() - start > 3000) return r();
          requestAnimationFrame(tick);
        };
        tick();
      });
    },
    awaitPrepare: true,
    selector: '#notif-stack .notif-toast.notif-config',
    title: 'Library matches',
    body: 'After every edit, SubTropica matches your drawing against the library. The cards on the right are the hits \u2014 each one is a specific mass configuration of the matching topology, with badges for \u03B5-orders, mass scales, and reference count.',
    mobile: {
      // Mobile collapses the toast stack to a single chip at top-center.
      prepare: async () => {
        await _tourDrawPromise;
        try { closeBrowser(); } catch {}
        closeDetailPanel();
        await new Promise(r => {
          const start = performance.now();
          const tick = () => {
            const chip = document.getElementById('notif-chip');
            if (chip && !chip.hidden) return r();
            if (performance.now() - start > 3000) return r();
            requestAnimationFrame(tick);
          };
          tick();
        });
      },
      selector: '#notif-chip', placement: 'below',
      title: 'Library matches',
      body: 'After every edit, SubTropica matches your drawing against the library. The chip at the top shows the matched <strong>topology</strong> and best <strong>mass configuration</strong>. Tap it to see every match.',
    },
  },
  {
    // Now actually click that toast and spotlight the detail popup that opens.
    prepare: async () => {
      await _tourDrawPromise;
      const toasts = document.querySelectorAll('#notif-stack .notif-toast');
      const target = toasts[1] || toasts[0];
      if (target) target.click();
      await new Promise(r => {
        const start = performance.now();
        const tick = () => {
          const dp = document.getElementById('detail-panel');
          if (dp && dp.classList.contains('open')) return r();
          if (performance.now() - start > 1500) return r();
          requestAnimationFrame(tick);
        };
        tick();
      });
    },
    awaitPrepare: true,
    selector: '#detail-panel',
    title: 'Library entry details',
    body: '<strong>Clicking a card</strong> \u2014 like this \u2014 opens the full entry: every cited paper, mass-scale information, precomputed \u03B5-expansions, and a paper preview link for each reference.',
    mobile: {
      // Two-tap path on mobile: chip opens the sheet, then the first config
      // toast inside opens the (fullscreen) detail popup.
      prepare: async () => {
        await _tourDrawPromise;
        const chip = document.getElementById('notif-chip');
        if (chip && !chip.hidden) chip.click();
        await new Promise(r => setTimeout(r, 350));
        const cfgToast = document.querySelector('#notif-sheet-body .notif-toast.notif-config')
                      || document.querySelector('#notif-sheet-body .notif-toast');
        if (cfgToast) cfgToast.click();
        await new Promise(r => {
          const start = performance.now();
          const tick = () => {
            const dp = document.getElementById('detail-panel');
            if (dp && dp.classList.contains('open')) return r();
            if (performance.now() - start > 1500) return r();
            requestAnimationFrame(tick);
          };
          tick();
        });
      },
      title: 'Library entry details',
      body: 'A match opens the full entry fullscreen: every cited paper, mass-scale info, precomputed \u03B5-expansions, a PDF preview for each reference. The <strong>Load to editor</strong> bar at the bottom drops the diagram onto the canvas.',
    },
  },
  {
    // Click the first paper reference in the detail popup — the app's own
    // thumbnail-click handler calls openPdfPanel with whatever arXiv ID
    // that row is wired to, so the tour self-corrects if the library's
    // reference ordering changes. Close the detail popup afterwards so the
    // PDF preview has the stage to itself.
    prepare: async () => {
      await _tourDrawPromise;
      const firstThumb = document.querySelector('#detail-panel .popup-record-pdf-thumb');
      if (firstThumb) firstThumb.click();
      closeDetailPanel();
      // Allow the slide-in transition + first PDF page to start rendering.
      await new Promise(r => setTimeout(r, 450));
    },
    awaitPrepare: true,
    selector: '#config-panel',
    title: 'Paper preview',
    body: 'When a library entry cites a paper, you can browse the original PDF right here \u2014 jump straight to the figures and formulas the entry was extracted from.',
  },
  {
    // Open the config panel to the Kinematics tab. Full-mode only — the
    // online demo has no kernel to run Integrate, so the kinematics panel
    // isn't useful to introduce there.
    includeIf: () => backendMode === 'full',
    prepare: async () => {
      await _tourDrawPromise;
      closeDetailPanel();
      try { closeBrowser(); } catch {}
      openConfigPanel();
      // Switch to the Kinematics tab
      const kinTab = document.querySelector('.config-tab[data-tab="cfg-momenta"]');
      if (kinTab) kinTab.click();
      await new Promise(r => setTimeout(r, 400));
    },
    awaitPrepare: true,
    selector: '#cfg-momenta',
    title: 'Kinematics panel',
    body: 'Configure <strong>external momenta</strong>, <strong>internal masses</strong>, <strong>propagator exponents</strong>, and <strong>numerators</strong> here. These feed directly into <code>STIntegrate</code> when you hit Integrate. Press <code>C</code> to toggle this panel any time.',
  },
  {
    prepare: async () => {
      await _tourDrawPromise;
      closeConfigPanel();
      closeDetailPanel();
      try { closeBrowser(); } catch {}
      // Expand the integral card so the preview cell is actually visible.
      const card = document.getElementById('integral-card');
      if (card && card.classList.contains('collapsed')) card.classList.remove('collapsed');
    },
    awaitPrepare: true,
    selector: '#integral-card-preview', placement: 'above',
    title: 'Live integral preview',
    body: 'The integral is re-rendered in LaTeX as you draw, with one-click copy as LaTeX or Mathematica syntax.',
  },
  {
    prepare: () => _tourDrawPromise, awaitPrepare: true,
    selector: '#integrate-fab', placement: 'above-left',
    title: 'Integrate',
    body: () => backendMode === 'full'
      ? 'Opens a panel with the integral and any precomputed library result for this exact mass configuration. Hit <strong>Integrate</strong> to compute it directly in your running Mathematica kernel.'
      : 'Opens a panel with the integral, any precomputed library result for this exact mass configuration, and a <strong>Download notebook</strong> button that hands you a Mathematica <code>.nb</code> pre-wired to <code>STIntegrate</code>.',
  },
  {
    selector: null, placement: 'center',
    title: 'That\u2019s it',
    body: () => backendMode === 'full'
      ? 'Hover any term with a dashed underline or a small icon for more. You can replay this tour from the <code>?</code> modal. Happy integrating!'
      : 'Hover any term with a dashed underline or a small icon for more. You can replay this tour from the <code>?</code> modal.',
  },
];

let _tourIdx = 0;
let _tourEls = null;

function _tourBuild() {
  const overlay = document.createElement('div');
  overlay.className = 'tour-overlay';
  const spot = document.createElement('div');
  spot.className = 'tour-spot';
  // Hidden by default via CSS opacity:0. The .visible class fades it in; no
  // display:none so fades actually animate (display changes can't be
  // transitioned in CSS).
  const card = document.createElement('div');
  card.className = 'tour-card';
  card.innerHTML =
    '<h3 class="tour-card-title"></h3>' +
    '<div class="tour-card-body"></div>' +
    '<div class="tour-card-footer">' +
    '  <button class="tour-card-skip">Skip</button>' +
    '  <span class="tour-card-progress"></span>' +
    '  <button class="tour-card-btn tour-prev">Back</button>' +
    '  <button class="tour-card-btn tour-card-btn-primary tour-next">Next</button>' +
    '</div>';
  document.body.appendChild(overlay);
  document.body.appendChild(spot);
  document.body.appendChild(card);
  card.querySelector('.tour-next').addEventListener('click', _tourNext);
  card.querySelector('.tour-prev').addEventListener('click', () => {
    // On step 0 the Back button reads "Skip" and closes the tour;
    // on all other steps it goes back normally.
    if (_tourIdx === 0) _tourClose(); else _tourPrev();
  });
  card.querySelector('.tour-card-skip').addEventListener('click', _tourClose);
  overlay.addEventListener('click', _tourClose);
  document.addEventListener('keydown', _tourKey);
  _tourEls = { overlay, spot, card };
}

function _tourKey(e) {
  if (!_tourEls || !_tourEls.overlay.classList.contains('visible')) return;
  if (e.key === 'Escape') { e.preventDefault(); _tourClose(); }
  else if (e.key === 'ArrowRight' || e.key === 'Enter') { e.preventDefault(); _tourNext(); }
  else if (e.key === 'ArrowLeft') { e.preventDefault(); _tourPrev(); }
}

function _visibleRect(el) {
  if (!el) return null;
  const r = el.getBoundingClientRect();
  if (r.width < 2 || r.height < 2) return null;
  const cs = getComputedStyle(el);
  if (cs.display === 'none' || cs.visibility === 'hidden' || parseFloat(cs.opacity) < 0.05) return null;
  return r;
}

function _tourShow(idx) {
  if (!_tourEls) _tourBuild();
  if (idx < 0 || idx >= TOUR_STEPS.length) { _tourClose(); return; }
  // Skip steps gated by includeIf() (used to hide full-mode-only steps in
  // the online demo) and by mobile.skip (desktop-only steps on mobile).
  // Direction of travel is inferred from the current idx.
  const dir = idx >= _tourIdx ? 1 : -1;
  while (idx >= 0 && idx < TOUR_STEPS.length) {
    const s = TOUR_STEPS[idx];
    if (typeof s.includeIf === 'function' && !s.includeIf()) { idx += dir; continue; }
    if (_isMobileTour() && s.mobile && s.mobile.skip) { idx += dir; continue; }
    break;
  }
  if (idx < 0 || idx >= TOUR_STEPS.length) { _tourClose(); return; }
  // Close the mass picker on step transitions, UNLESS the incoming step is
  // one of the mass-picker steps — the two phases of the mass demo both
  // want the popup to stay open, and reopening it between them would make
  // the picker flash (openMassPicker re-renders the body from scratch).
  if (_tourStepProp(TOUR_STEPS[idx], 'selector') !== '#mass-picker') {
    try { closeMassPicker(); } catch {}
  }
  _tourIdx = idx;
  const step = TOUR_STEPS[idx];
  const { overlay, spot, card } = _tourEls;
  overlay.classList.add('visible');

  // Run the step's prepare hook. `awaitPrepare` (default true) blocks the
  // spotlight placement until prepare resolves — use when the target doesn't
  // exist yet (e.g. an edge bubble that only appears after the demo draws).
  // Set to false for fire-and-forget preparations (like kicking off the
  // double-box animation itself: we want the card visible immediately while
  // the drawing unfolds behind it).
  const preparePromise = (() => {
    const pFn = _tourStepProp(step, 'prepare');
    if (typeof pFn !== 'function') return Promise.resolve();
    try { return Promise.resolve(pFn()); }
    catch (_) { return Promise.resolve(); }
  })();
  const waitForPrepare = _tourStepProp(step, 'awaitPrepare') !== false;

  // Cross-fade: slide the old card out, swap contents, slide the new in.
  const fill = () => {
    const titleVal = _tourStepProp(step, 'title');
    const bodyVal  = _tourStepProp(step, 'body');
    card.querySelector('.tour-card-title').innerHTML = typeof titleVal === 'function' ? titleVal() : titleVal;
    card.querySelector('.tour-card-body').innerHTML  = typeof bodyVal  === 'function' ? bodyVal()  : bodyVal;
    card.querySelector('.tour-card-progress').textContent = (idx + 1) + ' / ' + TOUR_STEPS.length;
    const prevBtn = card.querySelector('.tour-prev');
    prevBtn.textContent = idx === 0 ? 'Skip' : 'Back';
    prevBtn.disabled = false;
    prevBtn.style.opacity = '';
    card.querySelector('.tour-next').textContent = idx === TOUR_STEPS.length - 1 ? 'Finish' : 'Next';
  };
  const place = () => {
    const selVal = _tourStepProp(step, 'selector');
    const target = selVal ? document.querySelector(selVal) : null;
    const r = _visibleRect(target);
    if (!r) {
      // Target not found or not visible — fall back to a centered card and
      // dim via the overlay (no spotlight to do it).
      spot.classList.remove('visible');
      overlay.classList.add('dim');
      card.style.transform = 'translate(-50%, -50%)';
      card.style.top = '50%'; card.style.left = '50%';
      return;
    }
    const pad = 6;
    spot.style.top = (r.top - pad) + 'px';
    spot.style.left = (r.left - pad) + 'px';
    spot.style.width = (r.width + 2 * pad) + 'px';
    spot.style.height = (r.height + 2 * pad) + 'px';
    spot.classList.add('visible');
    // Spot dims via its own outer box-shadow; turn off overlay dim so we
    // don't paint on top of the spotlight outline.
    overlay.classList.remove('dim');

    // Re-measure the card so placement accounts for the new body length.
    card.style.transform = '';
    const cr = card.getBoundingClientRect();
    const cardW = cr.width || 340, cardH = cr.height || 180;
    let top, left;
    const gap = 16;
    switch (_tourStepProp(step, 'placement')) {
      case 'below':
        top = r.bottom + gap;
        left = Math.max(12, Math.min(r.left + r.width / 2 - cardW / 2, window.innerWidth - cardW - 12));
        break;
      case 'above':
        top = Math.max(12, r.top - cardH - gap);
        left = Math.max(12, Math.min(r.left + r.width / 2 - cardW / 2, window.innerWidth - cardW - 12));
        break;
      case 'above-left':
        top = Math.max(12, r.top - cardH - gap);
        left = Math.max(12, r.right - cardW);
        break;
      case 'below-left':
        top = r.top + 12;
        left = Math.max(12, r.left + 12);
        break;
      case 'center-over':
      default: {
        // Place next to the target rather than on top so the card doesn't
        // cover the thing it's describing.
        const rightSpace  = window.innerWidth  - r.right;
        const leftSpace   = r.left;
        const belowSpace  = window.innerHeight - r.bottom;
        const aboveSpace  = r.top;
        if (rightSpace  > cardW + gap) { top = r.top; left = r.right + gap; }
        else if (leftSpace   > cardW + gap) { top = r.top; left = r.left - cardW - gap; }
        else if (belowSpace  > cardH + gap) { top = r.bottom + gap; left = r.left; }
        else if (aboveSpace  > cardH + gap) { top = r.top - cardH - gap; left = r.left; }
        else { top = Math.max(12, r.top + r.height / 2 - cardH / 2); left = 20; }
        top  = Math.max(12, Math.min(top,  window.innerHeight - cardH - 12));
        left = Math.max(12, Math.min(left, window.innerWidth  - cardW - 12));
      }
    }
    card.style.top = top + 'px';
    card.style.left = left + 'px';
  };

  const afterPrepare = () => {
    // Re-measure once the DOM has painted the prepared state.
    requestAnimationFrame(() => place());
  };

  // If this is the first show, no previous card to fade out.
  const firstShow = !card.classList.contains('visible');
  if (firstShow) {
    fill();
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        place();
        card.classList.add('visible');
      });
    });
    if (waitForPrepare) preparePromise.then(afterPrepare);
    return;
  }

  // Step-to-step transition. Sequence the user should perceive:
  //   (a) card slides from the previous target to the new one (visible,
  //       carries the OLD content during most of the slide);
  //   (b) near the END of the slide, the content cross-fades and refreshes.
  // The prior ordering (fade-out → swap + reposition → fade-in) instead made
  // the card become fully visible *with the new content* while still
  // animating to the new position, so users saw "refresh, then move".
  //
  // CSS timing: .tour-card transitions top/left over ~420 ms; .swap-out
  // drops opacity to 0 over 180 ms. We kick off the move right away, then
  // start the content cross-fade at ~260 ms (most of the slide is visible).
  if (!waitForPrepare) {
    place();
    requestAnimationFrame(() => place());
  } else {
    // Async prepare may change layout; we defer placement to after it
    // resolves. Card stays at previous spot until then.
    preparePromise.then(() => {
      setTimeout(() => requestAnimationFrame(() => place()), 400);
    });
  }

  setTimeout(() => {
    card.classList.add('swap-out');
    setTimeout(() => {
      fill();
      card.classList.remove('swap-out');
    }, 180);
  }, 260);
}

function _tourNext() { _tourShow(_tourIdx + 1); }
function _tourPrev() { _tourShow(_tourIdx - 1); }
function _tourClose() {
  if (!_tourEls) return;
  const { overlay, spot, card } = _tourEls;
  _tourAborted = true;

  // Close any side panels/overlays a step may have opened. The tour drives
  // real UI surfaces (browser, detail popup, config panel, mass picker), so
  // Skip must undo them or the user is left staring at leftover state they
  // never opened themselves.
  try { closeBrowser(); } catch {}
  try { closeConfigPanel(); } catch {}
  try { closeDetailPanel(); } catch {}
  try { closeMassPicker(); } catch {}

  // Wipe the demo double-box if (and only if) the tour drew it — never touch
  // a drawing the user started before the tour ran.
  if (_tourDrewDemo && state) {
    state.vertices = [];
    state.edges = [];
    state.newVertexIdx = -1;
    state.newEdgeIdx = -1;
    state.edgeDragFrom = null;
    state.edgeDragPos = null;
    try { if (typeof renderEdgePreview === 'function') renderEdgePreview(); } catch {}
    try { if (typeof render === 'function') render(); } catch {}
    try { if (typeof onGraphChanged === 'function') onGraphChanged(); } catch {}
  }

  _tourRemoveGhost();
  document.removeEventListener('keydown', _tourKey);

  // Detach the tour DOM entirely — fading to opacity:0 leaves three z-index
  // 9999+ nodes parked in document.body, which is the literal "ghost" users
  // were reporting.
  try { overlay.remove(); } catch {}
  try { spot.remove(); } catch {}
  try { card.remove(); } catch {}

  _tourEls = null;
  _tourIdx = 0;
  _tourDrawPromise = null;
  _tourDrewDemo = false;
  _tourMassEdgeIdx = -1;

  try { localStorage.setItem(TOUR_STORAGE_KEY, '1'); } catch {}
}

function startTour() {
  // Manual replay from a finished state: reset the card so first-show
  // treatment applies (fade-in instead of swap-in), clear the demo drawing
  // promise, and wipe the canvas so the double-box animation replays.
  if (_tourEls) _tourEls.card.classList.remove('visible');
  _tourDrawPromise = null;
  _tourAborted = false;
  _tourDrewDemo = false;
  _tourMassEdgeIdx = -1;
  _tourRemoveGhost();
  if (state) {
    state.vertices = [];
    state.edges = [];
    state.newVertexIdx = -1;
    state.newEdgeIdx = -1;
    if (typeof render === 'function') render();
    if (typeof onGraphChanged === 'function') onGraphChanged();
  }
  _tourShow(0);
}

// Expose a manual-replay handle for the help icon / console, and kick off
// the tour on first visit (after the UI has had a moment to settle).
window.__st = Object.assign(window.__st || {}, { startTour });
window.addEventListener('load', () => {
  try {
    if (localStorage.getItem(TOUR_STORAGE_KEY) === '1') return;
  } catch {}
  // Delay so sizes/positions are stable before we measure.
  setTimeout(startTour, 1200);
});

const ST_VERSION = '1.0.445';

// ─── Library ─────────────────────────────────────────────────────────

let library = null;
// Waitlist: set of "topoKey\x00configKey" strings for entries that should
// be hidden from the public browser and highlighted (blue) in review mode.
// Loaded from ui/waitlist.json; the file is generated by
// scripts/mark_new_records.py — default content is every library entry
// untracked in git (i.e. new extraction-pipeline output not yet promoted).
let _waitlistConfigSet = new Set();
let _waitlistTopoSet = new Set();

function waitlistKey(topoKey, configKey) { return topoKey + '\x00' + configKey; }

async function loadWaitlist() {
  try {
    const resp = await fetch('waitlist.json', { cache: 'no-store' });
    if (!resp.ok) return;
    const data = await resp.json();
    (data.configs || []).forEach(c => {
      if (c.topoKey && c.configKey) {
        _waitlistConfigSet.add(waitlistKey(c.topoKey, c.configKey));
        _waitlistTopoSet.add(c.topoKey);
      }
    });
  } catch (e) {
    // File absent — no waitlist, that's fine.
  }
}

async function loadLibrary() {
  try {
    // Full mode: Python server builds library from library-bundled/ + library-local/ on the fly
    // Online mode: fall back to static library.json
    const resp = await fetch('/api/library');
    if (resp.ok) {
      library = await resp.json();
    } else {
      // Fallback for online/static hosting
      const resp2 = await fetch('library.json');
      library = await resp2.json();
    }
  } catch (e) {
    try {
      const resp2 = await fetch('library.json');
      library = await resp2.json();
    } catch (e2) {
      console.error('Failed to load library:', e2);
      library = { topologies: {} };
    }
  }
  await loadWaitlist();
  // Initial render once library is ready
  render();
  onGraphChanged();
}

// ─── Backend Mode ───────────────────────────────────────────────────

let backendMode = 'lite';  // 'lite' or 'full'
let backendDeps = {};      // dependency status from kernel: {pySecDec: "ok", FIESTA: "missing", ...}
const _sessionSalt = 1 + Math.floor(Math.random() * 999);

const kernel = {
  async ping() {
    try {
      const ctrl = new AbortController();
      setTimeout(() => ctrl.abort(), 1000);
      const r = await fetch('/api/ping', { signal: ctrl.signal });
      const d = await r.json();
      return d.status === 'alive' ? d : false;
    } catch { return false; }
  },
  async post(action, body = {}) {
    const r = await fetch('/api/' + action, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(body),
    });
    return r.json();
  },
  async get(endpoint) {
    const r = await fetch('/api/' + endpoint);
    return r.json();
  },
};

async function detectBackendMode() {
  const pingData = await kernel.ping();
  if (pingData) {
    backendMode = 'full';
    document.body.classList.add('mode-full');
    const titleBadge = document.querySelector('.title-badge');
    if (titleBadge) titleBadge.style.display = 'none';
    if (pingData.deps) backendDeps = pingData.deps;
  } else {
    // Lite mode: default to Paper tab in config panel
    const paperTab = document.querySelector('.config-tab[data-tab="cfg-paper"]');
    if (paperTab) switchConfigTab(paperTab);
  }
}

// ─── Constants ───────────────────────────────────────────────────────

// Cloudflare Worker base URL — shared by the submission / correction /
// PDF-proxy routes.  Hardcoded (not an env var) because the UI is a
// static bundle deployed without a build step.
const SUBMIT_WORKER_BASE = 'https://subtropica-submit.subtropica.workers.dev';

const SVG_NS = 'http://www.w3.org/2000/svg';
const VERTEX_RADIUS = 0.07;
const HIT_RADIUS = 0.25;
const SNAP_STEP = 0.25;
const BASE_VIEWBOX = { x: -6, y: -4, w: 12, h: 8 };
const MAX_HISTORY = 50;

// ─── Mass palette (from SubTropica) ─────────────────────────────────

const MASS_PALETTE = [
  '#A84545',  // m₁ maroon
  '#3A7040',  // m₂ tropical leaf
  '#4a6fa5',  // m₃ steel blue
  '#d4940a',  // m₄ golden amber
  '#7b3f7f',  // m₅ plum purple
  '#c45050',  // m₆ brick red
  '#1a8a7a',  // m₇ sea green
  '#8b5c2a',  // m₈ driftwood brown
];

// Color lookup keyed on the (kind, mass) slot, not on `mass` alone — the
// same integer is reused across kinds (internal m[1] and external M[1]
// are distinct symbols), so indexing only on the integer would paint them
// with the same swatch and silently tell the user "these are the same
// mass". We shift M-kind by half the palette so m- and M-slots never
// collide until more than PALETTE_LEN/2 slots of one kind exist.
const MASS_KIND_OFFSET = Math.floor(MASS_PALETTE.length / 2);
function slotPaletteIndex(mass, kind) {
  const off = kind === 'M' ? MASS_KIND_OFFSET : 0;
  return (mass - 1 + off) % MASS_PALETTE.length;
}
function massColor(mass, kind) {
  if (!mass || mass === 0) return null;  // massless → use --edge-color
  return MASS_PALETTE[slotPaletteIndex(mass, kind || 'm')];
}

const LINE_STYLES = [
  { id: 'solid',    label: 'Fermion' },
  { id: 'dashed',   label: 'Higgs' },
  { id: 'wavy',     label: 'Photon' },
  { id: 'dblwavy',  label: 'Graviton' },
  { id: 'gluon',    label: 'Gluon' },
];

// ─── State ───────────────────────────────────────────────────────────

const state = {
  mode: 'draw',
  vertices: [],
  edges: [],
  selectedVertex: null,
  dragging: null,
  edgeDragFrom: null,
  edgeDragPos: null,
  newVertexIdx: -1,
  newEdgeIdx: -1,
  wobbleVerts: [],
  splitEdgeIndices: [],
  _undoPushedOnDown: false,
};

// ─── Computation Config (persisted) ─────────────────────────────────

const COMPUTE_CONFIG_DEFAULTS = {
  // Basic
  dimension: '4 - 2*eps',
  epsOrder: '0',
  diagramName: '',
  autoName: true,

  // Numerators
  numeratorRows: [],  // [{expr: '', exp: '-1'}, ...]

  // Advanced: Core
  representation: 'Schwinger',
  normalization: 'Automatic',
  substitutions: '',
  simplifyOutput: 'Simplify',

  // Advanced: Output
  cleanOutput: true,
  contourHandling: 'Abort',
  verbose: false,
  showTimings: true,
  showIntegrands: false,
  saveSlowest: false,
  saveAll: false,

  // Advanced: Gauge
  gauge: 'Automatic',
  includeGauges: 'All',

  // Advanced: Pipeline
  heuristic: 'LeafCountLinear',
  scanScoreInterval: '{1, 3}',
  scoreInParallel: 'All',
  timeUpperBound: '10^17',
  memoryCutOff: 'None',
  scoringMemFrac: '0.5',
  parallelization: 'All',
  kernels: '',
  clearCaches: false,
  reuseResults: true,
  selectFaces: 'All',
  stopAt: 'Automatic',
  startAt: 'None',
  setupInParallel: 'Automatic',
  findRoots: false,
  methodLR: 'Espresso',
};

const computeConfig = { ...COMPUTE_CONFIG_DEFAULTS };

let SNAP_ON = false;
let zoomLevel = 1.0;
let panOffset = { x: 0, y: 0 };
let _panning = null;  // { startX, startY, origPanX, origPanY }
let undoStack = [];
let redoStack = [];
let currentNickel = null;
let currentMatches = [];  // array of {topoKey, topo} for current graph
let currentSubtopoMatches = []; // array of {topoKey, topo, pinched, collapsed}
let hintTimeout = null;
let massPickerEdge = null;   // index of edge whose mass picker is open

// ─── DOM refs ────────────────────────────────────────────────────────

const $ = id => document.getElementById(id);
const canvas = $('draw-canvas');
const gridLayer = $('grid-layer');
const edgeLayer = $('edge-layer');
const previewLayer = $('preview-layer');
const vertexLayer = $('vertex-layer');
const selectionLayer = $('selection-layer');

// ─── SVG helpers ─────────────────────────────────────────────────────

function svgPoint(evt) {
  const pt = canvas.createSVGPoint();
  pt.x = evt.clientX; pt.y = evt.clientY;
  const svgP = pt.matrixTransform(canvas.getScreenCTM().inverse());
  return { x: svgP.x, y: svgP.y };
}

function dist(a, b) {
  return Math.sqrt((a.x - b.x) ** 2 + (a.y - b.y) ** 2);
}

function snap(val) {
  return SNAP_ON ? Math.round(val / SNAP_STEP) * SNAP_STEP : val;
}

function nearestVertex(p) {
  let best = -1, bestD = Infinity;
  for (let i = 0; i < state.vertices.length; i++) {
    const d = dist(p, state.vertices[i]);
    if (d < bestD) { bestD = d; best = i; }
  }
  return bestD < HIT_RADIUS ? best : -1;
}

function nearestEdge(p) {
  let best = -1, bestD = Infinity;
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const a = state.vertices[e.a], b = state.vertices[e.b];
    const mid = { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
    if (dist(p, mid) < bestD) { bestD = dist(p, mid); best = i; }
  }
  return bestD < 0.4 ? best : -1;
}

function nearestEdgeSegment(p) {
  let best = -1, bestD = Infinity, bestPt = null;
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const a = state.vertices[e.a], b = state.vertices[e.b];
    const dx = b.x - a.x, dy = b.y - a.y;
    const len2 = dx * dx + dy * dy;
    if (len2 < 1e-12) continue;
    let t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
    t = Math.max(0.05, Math.min(0.95, t));
    const proj = { x: a.x + t * dx, y: a.y + t * dy };
    const d = dist(p, proj);
    if (d < bestD) { bestD = d; best = i; bestPt = proj; }
  }
  return bestD < 0.2 ? { index: best, point: bestPt } : { index: -1, point: null };
}

// ─── Undo / Redo ─────────────────────────────────────────────────────

function updateUndoRedoBtns() {
  const u = undoStack.length, r = redoStack.length;
  $('undo-btn').textContent = u > 0 ? `\u21A9 Undo (${u})` : '\u21A9 Undo';
  $('redo-btn').textContent = r > 0 ? `\u21AA Redo (${r})` : '\u21AA Redo';
  $('undo-btn').disabled = u === 0;
  $('redo-btn').disabled = r === 0;
}

function copyEdge(e) {
  return {
    a: e.a, b: e.b, mass: e.mass || 0, style: e.style || 'solid',
    massLabel: e.massLabel || '', edgeLabel: e.edgeLabel || '',
    extMomLabel: e.extMomLabel || '', propExponent: e.propExponent ?? 1,
  };
}

function snapshotGraph() {
  return {
    vertices: state.vertices.map(v => ({ x: v.x, y: v.y })),
    edges: state.edges.map(copyEdge),
  };
}

function pushUndoState() {
  undoStack.push(snapshotGraph());
  if (undoStack.length > MAX_HISTORY) undoStack.shift();
  redoStack = [];
  updateUndoRedoBtns();
}

function undoAction() {
  if (undoStack.length === 0) return;
  redoStack.push(snapshotGraph());
  const prev = undoStack.pop();
  state.vertices = prev.vertices;
  state.edges = prev.edges;
  state.selectedVertex = null;
  updateUndoRedoBtns();
  render();
  onGraphChanged();
}

function redoAction() {
  if (redoStack.length === 0) return;
  undoStack.push(snapshotGraph());
  const next = redoStack.pop();
  state.vertices = next.vertices;
  state.edges = next.edges;
  state.selectedVertex = null;
  updateUndoRedoBtns();
  render();
  onGraphChanged();
}

function clearAll() {
  if (state.vertices.length === 0 && state.edges.length === 0) return;
  pushUndoState();
  state.vertices = [];
  state.edges = [];
  state.selectedVertex = null;
  // Reset sticky name so auto-name resumes
  if (typeof _nameEditedByUser !== 'undefined') _nameEditedByUser = false;
  if ($('ic-name')) $('ic-name').value = '';
  render();
  onGraphChanged();
  showUndoToast('Cleared');
}

function showUndoToast(msg) {
  const existing = $('undo-toast-el');
  if (existing) existing.remove();
  const toast = document.createElement('div');
  toast.id = 'undo-toast-el';
  toast.className = 'undo-toast';
  toast.innerHTML = msg + ' <button>Undo</button>';
  toast.querySelector('button').addEventListener('click', () => { undoAction(); toast.remove(); });
  document.body.appendChild(toast);
  setTimeout(() => { if (toast.parentElement) toast.remove(); }, 5000);
}

function showWarningToast(msg, isWarning) {
  const existing = $('undo-toast-el');
  if (existing) existing.remove();
  const toast = document.createElement('div');
  toast.id = 'undo-toast-el';
  toast.className = 'undo-toast' + (isWarning ? ' warning-toast' : '');
  toast.textContent = msg;
  document.body.appendChild(toast);
  setTimeout(() => { if (toast.parentElement) toast.remove(); }, 3000);
}

// ─── Zoom ────────────────────────────────────────────────────────────

function zoomCanvas(dir) {
  if (dir > 0) zoomLevel = Math.min(6.0, zoomLevel * 1.1);
  else zoomLevel = Math.max(0.15, zoomLevel / 1.1);
  applyZoom();
}

function applyZoom() {
  const w = BASE_VIEWBOX.w / zoomLevel;
  const h = BASE_VIEWBOX.h / zoomLevel;
  const cx = BASE_VIEWBOX.x + BASE_VIEWBOX.w / 2 + panOffset.x;
  const cy = BASE_VIEWBOX.y + BASE_VIEWBOX.h / 2 + panOffset.y;
  canvas.setAttribute('viewBox', `${cx - w/2} ${cy - h/2} ${w} ${h}`);
  $('zoom-display').textContent = Math.round(zoomLevel * 100) + '%';
  render();
}

// ─── Layout transforms ──────────────────────────────────────────────

function rotateLayout(degrees) {
  if (state.vertices.length < 2) return;
  pushUndoState();
  const rad = degrees * Math.PI / 180;
  const cosA = Math.cos(rad), sinA = Math.sin(rad);
  let cx = 0, cy = 0;
  state.vertices.forEach(v => { cx += v.x; cy += v.y; });
  cx /= state.vertices.length; cy /= state.vertices.length;
  const to = state.vertices.map(v => {
    const dx = v.x - cx, dy = v.y - cy;
    return { x: cx + dx * cosA - dy * sinA, y: cy + dx * sinA + dy * cosA };
  });
  animateVertices(to, 350);
}

function flipLayout(axis) {
  if (state.vertices.length < 2) return;
  pushUndoState();
  let cx = 0, cy = 0;
  state.vertices.forEach(v => { cx += v.x; cy += v.y; });
  cx /= state.vertices.length; cy /= state.vertices.length;
  const to = state.vertices.map(v => {
    if (axis === 'h') return { x: cx - (v.x - cx), y: v.y };
    else return { x: v.x, y: cy - (v.y - cy) };
  });
  animateVertices(to, 350);
}

function computeForceLayout(vertices, edges) {
  if (vertices.length < 2 || edges.length < 1)
    return vertices.map(v => ({ x: v.x, y: v.y }));

  const degree = {};
  edges.forEach(e => {
    degree[e.a] = (degree[e.a] || 0) + 1;
    degree[e.b] = (degree[e.b] || 0) + 1;
  });
  const intVerts = [], extVerts = [];
  for (let i = 0; i < vertices.length; i++) {
    if ((degree[i] || 0) === 1) extVerts.push(i);
    else intVerts.push(i);
  }
  if (intVerts.length === 0) return vertices.map(v => ({ x: v.x, y: v.y }));

  // Add random jitter proportional to layout scale to break symmetry
  let maxCoord = 0;
  vertices.forEach(v => { maxCoord = Math.max(maxCoord, Math.abs(v.x), Math.abs(v.y)); });
  const jitter = Math.max(0.1, maxCoord * 0.15);
  const pos = vertices.map(v => ({
    x: v.x + (Math.random() - 0.5) * jitter,
    y: v.y + (Math.random() - 0.5) * jitter
  }));
  const nInt = intVerts.length;
  const k = Math.sqrt(nInt * 1.5 / nInt);
  const kRep = k * k * 2, kAtt = 0.3;
  let temp = k * 2;
  const cooling = temp / 201;

  // Helper: do segments (p1,p2) and (p3,p4) cross?
  function segmentsCross(p1, p2, p3, p4) {
    const d1x = p2.x - p1.x, d1y = p2.y - p1.y;
    const d2x = p4.x - p3.x, d2y = p4.y - p3.y;
    const denom = d1x * d2y - d1y * d2x;
    if (Math.abs(denom) < 1e-12) return false;
    const t = ((p3.x - p1.x) * d2y - (p3.y - p1.y) * d2x) / denom;
    const u = ((p3.x - p1.x) * d1y - (p3.y - p1.y) * d1x) / denom;
    return t > 0.05 && t < 0.95 && u > 0.05 && u < 0.95;
  }

  // Build internal edge list for crossing detection
  const intEdges = edges.filter(e => intVerts.indexOf(e.a) >= 0 && intVerts.indexOf(e.b) >= 0);

  for (let iter = 0; iter < 200; iter++) {
    const fx = {}, fy = {};
    intVerts.forEach(i => { fx[i] = 0; fy[i] = 0; });
    // Node-node repulsion
    for (let a = 0; a < intVerts.length; a++) {
      for (let b = a + 1; b < intVerts.length; b++) {
        const vi = intVerts[a], vj = intVerts[b];
        let dx = pos[vi].x - pos[vj].x, dy = pos[vi].y - pos[vj].y;
        let d2 = dx * dx + dy * dy;
        if (d2 < 0.001) { dx = 0.05 * (Math.random() - 0.5); dy = 0.05 * (Math.random() - 0.5); d2 = dx*dx + dy*dy; }
        const d = Math.sqrt(d2), f = kRep / d2;
        fx[vi] += dx/d*f; fy[vi] += dy/d*f;
        fx[vj] -= dx/d*f; fy[vj] -= dy/d*f;
      }
    }
    // Edge attraction — count multiplicity so multi-edges don't over-attract
    const pairMult = {};
    edges.forEach(e => {
      const pk = Math.min(e.a, e.b) + '_' + Math.max(e.a, e.b);
      pairMult[pk] = (pairMult[pk] || 0) + 1;
    });
    const pairDone = {};
    edges.forEach(e => {
      if (intVerts.indexOf(e.a) < 0 || intVerts.indexOf(e.b) < 0) return;
      const pk = Math.min(e.a, e.b) + '_' + Math.max(e.a, e.b);
      if (pairDone[pk]) return; // apply attraction once per vertex pair
      pairDone[pk] = true;
      const mult = pairMult[pk];
      const dx = pos[e.b].x - pos[e.a].x, dy = pos[e.b].y - pos[e.a].y;
      const d = Math.sqrt(dx*dx + dy*dy) || 0.01, f = d * kAtt;
      fx[e.a] += dx/d*f; fy[e.a] += dy/d*f;
      fx[e.b] -= dx/d*f; fy[e.b] -= dy/d*f;
      // Multi-edge minimum distance: push apart if too close
      if (mult > 1 && d < k * 1.2) {
        const repF = kRep * mult / (d * d + 0.01);
        fx[e.a] -= dx/d*repF; fy[e.a] -= dy/d*repF;
        fx[e.b] += dx/d*repF; fy[e.b] += dy/d*repF;
      }
    });
    // Edge crossing repulsion: push vertices of crossing edges apart
    const crossForce = k * 0.5;
    for (let i = 0; i < intEdges.length; i++) {
      for (let j = i + 1; j < intEdges.length; j++) {
        const e1 = intEdges[i], e2 = intEdges[j];
        // Skip edges sharing a vertex
        if (e1.a === e2.a || e1.a === e2.b || e1.b === e2.a || e1.b === e2.b) continue;
        if (segmentsCross(pos[e1.a], pos[e1.b], pos[e2.a], pos[e2.b])) {
          // Push midpoints of crossing edges apart
          const m1x = (pos[e1.a].x + pos[e1.b].x) / 2;
          const m1y = (pos[e1.a].y + pos[e1.b].y) / 2;
          const m2x = (pos[e2.a].x + pos[e2.b].x) / 2;
          const m2y = (pos[e2.a].y + pos[e2.b].y) / 2;
          let dx = m1x - m2x, dy = m1y - m2y;
          const dl = Math.sqrt(dx*dx + dy*dy) || 0.01;
          dx /= dl; dy /= dl;
          // Perpendicular push on the edge vertices
          const nx = -dy, ny = dx;
          fx[e1.a] += nx * crossForce; fy[e1.a] += ny * crossForce;
          fx[e1.b] += nx * crossForce; fy[e1.b] += ny * crossForce;
          fx[e2.a] -= nx * crossForce; fy[e2.a] -= ny * crossForce;
          fx[e2.b] -= nx * crossForce; fy[e2.b] -= ny * crossForce;
        }
      }
    }
    intVerts.forEach(vi => {
      const d = Math.sqrt(fx[vi]**2 + fy[vi]**2) || 0.01;
      const c = Math.min(d, temp) / d;
      pos[vi].x += fx[vi]*c; pos[vi].y += fy[vi]*c;
    });
    temp = Math.max(0.01, temp - cooling);
  }

  let cx = 0, cy = 0;
  intVerts.forEach(i => { cx += pos[i].x; cy += pos[i].y; });
  cx /= nInt; cy /= nInt;

  extVerts.forEach(vi => {
    let nb = -1;
    edges.forEach(e => {
      if (e.a === vi && intVerts.indexOf(e.b) >= 0) nb = e.b;
      if (e.b === vi && intVerts.indexOf(e.a) >= 0) nb = e.a;
    });
    if (nb < 0) return;
    let dx = pos[nb].x - cx, dy = pos[nb].y - cy;
    const dl = Math.sqrt(dx*dx + dy*dy) || 1;
    dx /= dl; dy /= dl;
    const sibs = extVerts.filter(ev => edges.some(e => (e.a===ev&&e.b===nb)||(e.b===ev&&e.a===nb)));
    const si = sibs.indexOf(vi), ns = sibs.length;
    if (ns > 1) {
      const spread = Math.min(0.8, 0.4 * ns);
      const ang = Math.atan2(dy, dx) + (si - (ns-1)/2) * spread / Math.max(ns-1, 1);
      dx = Math.cos(ang); dy = Math.sin(ang);
    }
    pos[vi] = { x: pos[nb].x + dx * k * 0.8, y: pos[nb].y + dy * k * 0.8 };
  });
  return pos;
}

let _layoutAnim = null;

function easeBounce(t) {
  const c4 = (2 * Math.PI) / 4.5;
  if (t <= 0) return 0;
  if (t >= 1) return 1;
  return 1 + Math.pow(2, -8 * t) * Math.sin((t * 8 - 0.75) * c4);
}

/** Animate vertices from current positions to target positions. */
function animateVertices(to, duration = 400) {
  const from = state.vertices.map(v => ({ x: v.x, y: v.y }));
  if (_layoutAnim) cancelAnimationFrame(_layoutAnim);
  const t0 = performance.now();

  function tick(now) {
    const elapsed = now - t0;
    const t = Math.min(elapsed / duration, 1);
    const e = easeBounce(t);

    state.vertices = from.map((f, i) => ({
      x: f.x + (to[i].x - f.x) * e,
      y: f.y + (to[i].y - f.y) * e,
    }));
    render();

    if (t < 1) {
      _layoutAnim = requestAnimationFrame(tick);
    } else {
      _layoutAnim = null;
      onGraphChanged();
    }
  }

  _layoutAnim = requestAnimationFrame(tick);
}

function rebalanceLayout() {
  if (state.vertices.length < 2 || state.edges.length < 1) return;
  pushUndoState();
  const to = computeForceLayout(state.vertices, state.edges);
  animateVertices(to, 600);
}

// ─── Mode switching ──────────────────────────────────────────────────

// ─── Label mode ─────────────────────────────────────────────────

const LABEL_MODES = ['none', 'numbers', 'momenta']; // kept for save/restore compat
let labelMode = 'momenta';
let showArrows = true;
let lineScale = 1.0;   // multiplier for edge stroke width
let labelScale = 1.0;  // multiplier for label font size

function toggleLabelMode() {
  // Cycle: momenta → none → momenta (skip numbers)
  labelMode = labelMode === 'momenta' ? 'none' : 'momenta';
  $('momenta-toggle').checked = labelMode === 'momenta';
  render();
}

function syncLabelToggles() {
  labelMode = $('momenta-toggle').checked ? 'momenta' : 'none';
  showArrows = $('arrows-toggle').checked;
  render();
}

function setMode(m) {
  state.mode = m;
  state.selectedVertex = null;
  state.edgeDragFrom = null;
  state.edgeDragPos = null;
  state.dragging = null;
  clearEdgePreview();

  const drawBtn = $('mode-draw'), delBtn = $('mode-delete');
  drawBtn.className = 'mode-btn' + (m === 'draw' ? ' active-draw' : '');
  delBtn.className = 'mode-btn' + (m === 'delete' ? ' active-delete' : '');
  canvas.setAttribute('class', m === 'delete' ? 'mode-delete' : '');
  // Mirror the active mode on the overflow-menu items so the mobile path
  // reflects the current mode (the toolbar buttons may be hidden on phones).
  document.querySelectorAll('.overflow-mode-item').forEach(el => {
    const on = el.dataset.action === ('mode-' + m);
    el.setAttribute('aria-checked', on ? 'true' : 'false');
    el.classList.toggle('overflow-mode-active', on);
  });
  render();
}

function countLoops() {
  if (state.edges.length === 0) return 0;
  const adj = {};
  state.edges.forEach(e => {
    if (!adj[e.a]) adj[e.a] = [];
    if (!adj[e.b]) adj[e.b] = [];
    adj[e.a].push(e.b);
    adj[e.b].push(e.a);
  });
  const visited = {};
  let components = 0;
  const verts = Object.keys(adj).map(Number);
  verts.forEach(v => {
    if (!visited[v]) {
      components++;
      const q = [v]; visited[v] = true;
      while (q.length) {
        const cur = q.shift();
        (adj[cur] || []).forEach(nb => { if (!visited[nb]) { visited[nb] = true; q.push(nb); } });
      }
    }
  });
  return state.edges.length - verts.length + components;
}

// ─── Render ──────────────────────────────────────────────────────────

let _offscreenToastShown = false;

// ─── Label placement system ─────────────────────────────────────────

/**
 * Estimate label bounding box in SVG units from LaTeX source.
 * Returns { w, h } — approximate width and height.
 */
function estimateLabelSize(latex) {
  // Count main-size characters (Greek letters, normal chars)
  let mainChars = 0;
  let subChars = 0;
  let temp = latex;
  // Count subscript/superscript content separately (rendered smaller)
  const subMatches = temp.match(/[_^]\{([^}]*)\}/g) || [];
  for (const m of subMatches) subChars += m.length - 3; // subtract _{ and }
  temp = temp.replace(/[_^]\{[^}]*\}/g, '');
  // Greek/command letters → 1 char each
  temp = temp.replace(/\\(ell|gamma|alpha|beta|mu|nu|pi|sigma|eta|varepsilon)\b/g, 'X');
  temp = temp.replace(/\\(left|right|mathrm|text|mathbf|mathit|operatorname)\b/g, '');
  temp = temp.replace(/\\[a-zA-Z]+/g, 'X');
  temp = temp.replace(/[{}\\]/g, '').replace(/\s+/g, '');
  mainChars = temp.length;
  // At LABEL_SCALE=0.01, 10px font ≈ 6px per char → 0.06 SVG units
  const opCount = (latex.match(/[+\-]/g) || []).length;
  const w = Math.max(mainChars * 0.065 + subChars * 0.04 + opCount * 0.05 + 0.06, 0.10);
  const h = subChars > 0 ? 0.16 : 0.13;
  return { w, h };
}

/**
 * Collect obstacle rectangles from edges, vertices, and bubbles.
 * Each obstacle: { x, y, w, h } where (x,y) is center.
 */
function collectObstacles() {
  const obs = [];
  // Vertex circles
  for (let i = 0; i < state.vertices.length; i++) {
    const v = state.vertices[i];
    const r = VERTEX_RADIUS;
    obs.push({ x: v.x, y: v.y, w: r * 4, h: r * 4, type: 'vertex' });
  }
  // Edge segments (sampled at 3 points along each edge)
  const counts = {};
  for (let i = 0; i < state.edges.length; i++) {
    const key = Math.min(state.edges[i].a, state.edges[i].b) + '-' + Math.max(state.edges[i].a, state.edges[i].b);
    counts[key] = (counts[key] || 0) + 1;
  }
  const copyIdx = [];
  const counts2 = {};
  for (let i = 0; i < state.edges.length; i++) {
    const key = Math.min(state.edges[i].a, state.edges[i].b) + '-' + Math.max(state.edges[i].a, state.edges[i].b);
    counts2[key] = (counts2[key] || 0) + 1;
    copyIdx.push(counts2[key]);
  }
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a >= state.vertices.length || e.b >= state.vertices.length) continue;
    const a = state.vertices[e.a], b = state.vertices[e.b];
    const key = Math.min(e.a, e.b) + '-' + Math.max(e.a, e.b);
    const n = counts[key], j = copyIdx[i];
    // Self-loop: sample points along the loop circle
    if (e.a === e.b) {
      let gcx = 0, gcy = 0;
      state.vertices.forEach(v => { gcx += v.x; gcy += v.y; });
      gcx /= state.vertices.length || 1; gcy /= state.vertices.length || 1;
      let dx = a.x - gcx, dy = a.y - gcy;
      const dl = Math.sqrt(dx*dx + dy*dy);
      if (dl < 0.01) { dx = 0; dy = -1; } else { dx /= dl; dy /= dl; }
      const baseAng = Math.atan2(dy, dx);
      const spread = n > 1 ? 0.6 : 0;
      const ang = baseAng + (j - (n+1)/2) * spread;
      const loopR = 0.28;
      const cx = a.x + Math.cos(ang) * loopR, cy = a.y + Math.sin(ang) * loopR;
      for (const t of [0.25, 0.5, 0.75]) {
        const ta = ang + t * 2 * Math.PI;
        obs.push({ x: cx + Math.cos(ta) * loopR, y: cy + Math.sin(ta) * loopR, w: 0.16, h: 0.16, type: 'edge' });
      }
      continue;
    }
    for (const t of [0.25, 0.5, 0.75]) {
      let px, py;
      if (n > 1) {
        const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
        const dx = hi.x - lo.x, dy = hi.y - lo.y, len = Math.sqrt(dx*dx+dy*dy) || 1;
        const nx = -dy/len, ny = dx/len;
        const off = (j - (n+1)/2) * 0.5;
        const ls = e.style || 'solid';
        if (ls === 'solid' || ls === 'dashed') {
          const cx = (a.x+b.x)/2 + off*nx, cy = (a.y+b.y)/2 + off*ny;
          const t1 = 1 - t;
          px = t1*t1*a.x + 2*t*t1*cx + t*t*b.x;
          py = t1*t1*a.y + 2*t*t1*cy + t*t*b.y;
        } else {
          px = a.x + (b.x-a.x)*t + off*nx;
          py = a.y + (b.y-a.y)*t + off*ny;
        }
      } else {
        px = a.x + (b.x - a.x) * t;
        py = a.y + (b.y - a.y) * t;
      }
      obs.push({ x: px, y: py, w: 0.16, h: 0.16, type: 'edge' });
    }
  }
  return obs;
}

/** Check if two rectangles overlap. Each: { x, y, w, h } with (x,y) = center. */
function rectsOverlap(a, b) {
  return Math.abs(a.x - b.x) < (a.w + b.w) / 2 &&
         Math.abs(a.y - b.y) < (a.h + b.h) / 2;
}

/**
 * Resolve label collisions via greedy displacement.
 * Modifies label.x and label.y in place.
 */
function resolveCollisions(labels, obstacles) {
  const placed = [];
  for (const L of labels) {
    if (L.skipCollision) { placed.push(L); continue; }
    for (let iter = 0; iter < 24; iter++) {
      let totalDx = 0, totalDy = 0;
      const lr = { x: L.x, y: L.y, w: L.w, h: L.h };
      // Accumulate push from all overlapping placed labels
      for (const P of placed) {
        const pr = { x: P.x, y: P.y, w: P.w, h: P.h };
        if (rectsOverlap(lr, pr)) {
          const ox = (lr.w + pr.w) / 2 - Math.abs(lr.x - pr.x);
          const oy = (lr.h + pr.h) / 2 - Math.abs(lr.y - pr.y);
          totalDx += (lr.x >= pr.x ? ox : -ox);
          totalDy += (lr.y >= pr.y ? oy : -oy);
        }
      }
      // Accumulate push from obstacles
      for (const O of obstacles) {
        if (rectsOverlap(lr, O)) {
          const ox = (lr.w + O.w) / 2 - Math.abs(lr.x - O.x);
          const oy = (lr.h + O.h) / 2 - Math.abs(lr.y - O.y);
          totalDx += (lr.x >= O.x ? ox : -ox);
          totalDy += (lr.y >= O.y ? oy : -oy);
        }
      }
      if (Math.abs(totalDx) < 0.001 && Math.abs(totalDy) < 0.001) break;
      // Apply full separation
      L.x += totalDx * 0.65;
      L.y += totalDy * 0.65;
      // Leash constraint
      const ldx = L.x - L.anchorX, ldy = L.y - L.anchorY;
      const ld = Math.sqrt(ldx*ldx + ldy*ldy);
      if (ld > L.maxLeash) {
        L.x = L.anchorX + ldx / ld * L.maxLeash;
        L.y = L.anchorY + ldy / ld * L.maxLeash;
      }
    }
    placed.push(L);
  }
}

/**
 * Emit all label foreignObject elements into the SVG.
 */
// Scale factor: CSS pixels → SVG units
// At default viewBox (12 units wide), on a ~1200px screen, 1 SVG unit ≈ 100px.
// We render KaTeX at 10px CSS and scale by 0.01 to get ~0.1 SVG units.
const LABEL_SCALE = 0.01;

/**
 * Convert a LaTeX momentum/label string to plain Unicode for SVG <text>.
 * Handles common physics notation: \ell_{i} → ℓᵢ, p_{i} → pᵢ, etc.
 */
function latexToUnicode(latex) {
  if (!latex) return '';
  let s = latex;
  // Subscript digits: _{1} → ₁, etc.
  const subDigits = {'0':'₀','1':'₁','2':'₂','3':'₃','4':'₄','5':'₅','6':'₆','7':'₇','8':'₈','9':'₉'};
  // Superscript digits
  const supDigits = {'0':'⁰','1':'¹','2':'²','3':'³','4':'⁴','5':'⁵','6':'⁶','7':'⁷','8':'⁸','9':'⁹'};
  // Greek letters
  s = s.replace(/\\ell/g, 'ℓ');
  s = s.replace(/\\alpha/g, 'α').replace(/\\beta/g, 'β').replace(/\\gamma/g, 'γ');
  s = s.replace(/\\delta/g, 'δ').replace(/\\mu/g, 'μ').replace(/\\nu/g, 'ν');
  s = s.replace(/\\pi/g, 'π').replace(/\\sigma/g, 'σ').replace(/\\tau/g, 'τ');
  s = s.replace(/\\varepsilon/g, 'ε').replace(/\\epsilon/g, 'ε');
  s = s.replace(/\\omega/g, 'ω').replace(/\\lambda/g, 'λ').replace(/\\rho/g, 'ρ');
  s = s.replace(/\\Gamma/g, 'Γ').replace(/\\Delta/g, 'Δ').replace(/\\Sigma/g, 'Σ');
  // Subscripts: _{...} or _x (single char)
  s = s.replace(/_\{([^}]*)\}/g, (_, content) =>
    content.replace(/./g, ch => subDigits[ch] || ch));
  s = s.replace(/_(\d)/g, (_, d) => subDigits[d] || d);
  // Superscripts: ^{...} or ^x
  s = s.replace(/\^\{([^}]*)\}/g, (_, content) =>
    content.replace(/./g, ch => supDigits[ch] || ch));
  s = s.replace(/\^(\d)/g, (_, d) => supDigits[d] || d);
  // Clean up remaining LaTeX commands
  s = s.replace(/\\mathrm\{([^}]*)\}/g, '$1');
  s = s.replace(/\\text\{([^}]*)\}/g, '$1');
  s = s.replace(/\\left\(/g, '(').replace(/\\right\)/g, ')');
  s = s.replace(/\\,/g, ' ').replace(/\\;/g, ' ').replace(/\\quad/g, '  ');
  s = s.replace(/\\/g, '');  // remove any remaining backslashes
  s = s.replace(/[{}]/g, ''); // remove remaining braces
  // Use proper Unicode minus sign (U+2212) instead of hyphen-minus
  s = s.replace(/-/g, '\u2212');
  return s.trim();
}

function emitLabels(labels) {
  for (const L of labels) {
    const txt = document.createElementNS(SVG_NS, 'text');
    txt.setAttribute('x', L.x);
    txt.setAttribute('y', L.y);
    txt.setAttribute('text-anchor', 'middle');
    txt.setAttribute('dominant-baseline', 'central');
    txt.setAttribute('class', 'edge-label-svg ' + (L.cssClass || ''));
    txt.style.pointerEvents = 'none';
    const baseSize = (L.cssClass || '').includes('vertex-label') ? 0.10 : 0.14;
    txt.style.fontSize = (baseSize * labelScale) + 'px';
    txt.textContent = latexToUnicode(L.latex);
    L.parent.appendChild(txt);
  }
}

/**
 * Collect edge labels into the labels array.
 * Reads edge geometry from state and computes preferred positions.
 */
function collectEdgeLabels(labels, edgeDeg) {
  const counts = {}, copyIdx = [];
  for (let i = 0; i < state.edges.length; i++) {
    const key = Math.min(state.edges[i].a, state.edges[i].b) + '-' + Math.max(state.edges[i].a, state.edges[i].b);
    counts[key] = (counts[key] || 0) + 1;
    copyIdx.push(counts[key]);
  }

  // Graph centroid (for choosing label side on single edges)
  let gcx = 0, gcy = 0;
  const nvt = state.vertices.length || 1;
  state.vertices.forEach(v => { gcx += v.x; gcy += v.y; });
  gcx /= nvt; gcy /= nvt;

  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a >= state.vertices.length || e.b >= state.vertices.length) continue;
    const a = state.vertices[e.a], b = state.vertices[e.b];
    const key = Math.min(e.a, e.b) + '-' + Math.max(e.a, e.b);
    const n = counts[key], j = copyIdx[i];

    // Self-loop: place label at the far point of the loop
    if (e.a === e.b) {
      // Use stored loop geometry from renderEdges
      const lCx = e._loopCx ?? a.x;
      const lCy = e._loopCy ?? a.y - 0.28;
      const lR = e._loopR ?? 0.28;
      const lAng = e._loopAng ?? -Math.PI / 2;
      // Label at the tip of the teardrop, offset outward
      const farX = a.x + Math.cos(lAng) * 0.55;
      const farY = a.y + Math.sin(lAng) * 0.55;

      const bubbleG = edgeLayer.querySelector(`.edge-bubble[data-edge="${i}"]`);
      const parent = bubbleG || edgeLayer;

      if (shouldShowLabels()) {
        let latex;
        if (labelMode === 'momenta') {
          const mLabels = getMomentumLabels();
          latex = mLabels ? mLabels[i] : '?';
        } else {
          latex = (i + 1).toString();
        }
        const sz = estimateLabelSize(latex);
        labels.push({
          x: farX, y: farY, anchorX: farX, anchorY: farY,
          w: sz.w, h: sz.h, maxLeash: 0.65,
          latex, cssClass: 'edge-label-tex',
          parent, type: 'edge-primary', edgeIdx: i, priority: 1,
        });
      }
      const hasCustomLabel = e.edgeLabel && e.edgeLabel.trim();
      if (hasCustomLabel && labelMode !== 'none') {
        const sz = estimateLabelSize(e.edgeLabel.trim());
        const sideX = lCx + Math.cos(lAng + 0.6) * (lR + 0.12);
        const sideY = lCy + Math.sin(lAng + 0.6) * (lR + 0.12);
        labels.push({
          x: sideX, y: sideY, anchorX: sideX, anchorY: sideY,
          w: sz.w, h: sz.h, maxLeash: 0.65,
          latex: e.edgeLabel.trim(), cssClass: 'edge-label-tex',
          parent, type: 'edge-custom', edgeIdx: i, priority: 1,
        });
      }
      continue;
    }

    const degA = edgeDeg[e.a] || 0, degB = edgeDeg[e.b] || 0;
    const isExtLeg = degA === 1 || degB === 1;
    // Use canonical direction (min→max index) for consistent normal across multi-edges
    const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
    const edx = hi.x - lo.x, edy = hi.y - lo.y;
    const edgeLen = Math.sqrt(edx*edx + edy*edy) || 1;
    const enx = -edy / edgeLen, eny = edx / edgeLen;  // normal

    // Compute curve midpoint (anchor for label)
    let emx, emy;
    if (n > 1) {
      const px = enx, py = eny;
      const off = (j - (n+1)/2) * 0.5;
      const ls = e.style || 'solid';
      if (ls === 'solid' || ls === 'dashed') {
        const cx = (a.x+b.x)/2 + off*px, cy = (a.y+b.y)/2 + off*py;
        emx = (a.x + 2*cx + b.x) / 4;
        emy = (a.y + 2*cy + b.y) / 4;
      } else {
        emx = (a.x+b.x)/2 + off*px;
        emy = (a.y+b.y)/2 + off*py;
      }
    } else {
      emx = (a.x+b.x)/2;
      emy = (a.y+b.y)/2;
    }

    // Choose label offset direction
    let labelNormSign;
    if (n > 1) {
      const off = (j - (n+1)/2) * 0.5;
      labelNormSign = off >= 0 ? 1 : -1;
    } else {
      const toCenter = (gcx - emx) * enx + (gcy - emy) * eny;
      labelNormSign = toCenter > 0 ? -1 : 1;
    }

    // Find the bubble group to attach labels to
    const bubbleG = edgeLayer.querySelector(`.edge-bubble[data-edge="${i}"]`);
    const parent = bubbleG || edgeLayer;

    function addLabel(latex, side, type) {
      const sz = estimateLabelSize(latex);
      let lx, ly;
      if (isExtLeg) {
        const extV = degA === 1 ? a : b;
        const intV = degA === 1 ? b : a;
        const dx = extV.x - intV.x, dy = extV.y - intV.y;
        const len = Math.sqrt(dx*dx + dy*dy) || 1;
        lx = extV.x + (dx / len) * 0.22;
        ly = extV.y + (dy / len) * 0.22;
        // Offset perpendicular slightly so label isn't on the edge line
        const perpSign = side > 0 ? labelNormSign : -labelNormSign;
        lx += perpSign * enx * 0.22;
        ly += perpSign * eny * 0.22;
      } else {
        const offset = 0.34;
        const dir = side > 0 ? labelNormSign : -labelNormSign;
        lx = emx + dir * enx * offset;
        ly = emy + dir * eny * offset;
      }
      labels.push({
        x: lx, y: ly,
        anchorX: lx, anchorY: ly,
        w: sz.w, h: sz.h,
        maxLeash: isExtLeg ? 0.80 : 0.65,
        latex, cssClass: 'edge-label-tex',
        parent, type, edgeIdx: i,
        priority: isExtLeg ? 0 : 1,
      });
    }

    // Primary label: momenta or numbers
    if (shouldShowLabels()) {
      let latex;
      if (labelMode === 'momenta') {
        const mLabels = getMomentumLabels();
        latex = mLabels ? mLabels[i] : '?';
        // Skip trivial "0" momenta (e.g., 1-leg external)
        if (latex === '0') latex = null;
      } else {
        latex = (i + 1).toString();
      }
      if (latex) addLabel(latex, +1, 'edge-primary');
    }

    // Custom particle label (on opposite side)
    const hasCustomLabel = e.edgeLabel && e.edgeLabel.trim();
    if (hasCustomLabel && labelMode !== 'none') {
      addLabel(e.edgeLabel.trim(), shouldShowLabels() ? -1 : +1, 'edge-custom');
    }
  }
}

/**
 * Collect vertex labels into the labels array.
 * Places vertex numbers in the widest angular gap between incident edges.
 */
function collectVertexLabels(labels, edgeDeg) {
  // Vertex numbers only show in numbers mode, centered inside the vertex
  if (labelMode !== 'numbers') return;

  for (let i = 0; i < state.vertices.length; i++) {
    const d = edgeDeg[i] || 0;
    if (d <= 1) continue;  // skip external vertices
    const v = state.vertices[i];
    const latex = (i + 1).toString();
    const sz = estimateLabelSize(latex);

    const wrap = vertexLayer.querySelector(`g[data-vertex="${i}"]`) || vertexLayer;

    labels.push({
      x: v.x, y: v.y,
      anchorX: v.x, anchorY: v.y,
      w: sz.w, h: sz.h,
      maxLeash: 0,  // don't move — stays centered on vertex
      latex, cssClass: 'vertex-label-tex',
      parent: wrap, type: 'vertex',
      vertexIdx: i,
      priority: 0,  // highest priority — never displaced
      skipCollision: true,  // don't push other labels away from this
    });
  }
}

function render() {
  renderGrid();
  renderEdges();
  renderVertices();

  // ── Label placement pipeline ──
  const edgeDeg = getVertexDegrees();
  const labels = [];
  const obstacles = collectObstacles();

  collectEdgeLabels(labels, edgeDeg);
  collectVertexLabels(labels, edgeDeg);

  // Sort by priority so higher-priority labels get placed first (less likely to move)
  labels.sort((a, b) => a.priority - b.priority);

  resolveCollisions(labels, obstacles);
  emitLabels(labels);

  renderSelection();
  checkOffscreenVertices();
}

function checkOffscreenVertices() {
  if (state.vertices.length === 0 || _offscreenToastShown) return;
  const vb = canvas.getAttribute('viewBox').split(' ').map(Number);
  const [vx, vy, vw, vh] = vb;
  for (const v of state.vertices) {
    if (v.x < vx || v.x > vx + vw || v.y < vy || v.y > vy + vh) {
      _offscreenToastShown = true;
      showWarningToast('Ctrl + drag to pan the canvas');
      // Reset after a while so it can show again if user forgets
      setTimeout(() => { _offscreenToastShown = false; }, 15000);
      return;
    }
  }
}

function renderGrid() {
  gridLayer.innerHTML = '';
  const vb = canvas.getAttribute('viewBox').split(' ').map(Number);
  const step = SNAP_STEP;
  const xMin = Math.floor(vb[0] / step) * step;
  const xMax = Math.ceil((vb[0] + vb[2]) / step) * step;
  const yMin = Math.floor(vb[1] / step) * step;
  const yMax = Math.ceil((vb[1] + vb[3]) / step) * step;
  for (let x = xMin; x <= xMax; x += step) {
    for (let y = yMin; y <= yMax; y += step) {
      const dot = document.createElementNS(SVG_NS, 'circle');
      dot.setAttribute('cx', x); dot.setAttribute('cy', y);
      dot.setAttribute('r', 0.018 / Math.sqrt(zoomLevel));
      dot.setAttribute('fill', 'var(--grid-dot)');
      dot.setAttribute('opacity', SNAP_ON ? '0.5' : '0.25');
      gridLayer.appendChild(dot);
    }
  }
}

function edgeMidpoint(i) {
  const e = state.edges[i];
  const a = state.vertices[e.a], b = state.vertices[e.b];
  const counts = {};
  let copyJ = 0;
  for (let k = 0; k <= i; k++) {
    const key = Math.min(state.edges[k].a, state.edges[k].b) + '-' + Math.max(state.edges[k].a, state.edges[k].b);
    counts[key] = (counts[key] || 0) + 1;
    if (k === i) copyJ = counts[key];
  }
  const normKey = Math.min(e.a, e.b) + '-' + Math.max(e.a, e.b);
  let total = 0;
  state.edges.forEach(ee => {
    const ek = Math.min(ee.a, ee.b) + '-' + Math.max(ee.a, ee.b);
    if (ek === normKey) total++;
  });
  if (total > 1) {
    const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
    const dx = hi.x - lo.x, dy = hi.y - lo.y, len = Math.sqrt(dx * dx + dy * dy) || 1;
    const px = -dy / len, py = dx / len;
    const off = (copyJ - (total + 1) / 2) * 0.5;
    const lineStyle = e.style || 'solid';
    if (lineStyle === 'solid' || lineStyle === 'dashed') {
      // Quadratic Bézier midpoint at t=0.5: (A + 2C + B)/4
      const cx = (a.x + b.x) / 2 + off * px, cy = (a.y + b.y) / 2 + off * py;
      return { x: (a.x + 2 * cx + b.x) / 4, y: (a.y + 2 * cy + b.y) / 4 };
    } else {
      return { x: (a.x + b.x) / 2 + off * px, y: (a.y + b.y) / 2 + off * py };
    }
  }
  return { x: (a.x + b.x) / 2, y: (a.y + b.y) / 2 };
}

function renderEdges() {
  edgeLayer.innerHTML = '';
  const counts = {}, copyIdx = [];
  for (let i = 0; i < state.edges.length; i++) {
    const key = Math.min(state.edges[i].a, state.edges[i].b) + '-' + Math.max(state.edges[i].a, state.edges[i].b);
    counts[key] = (counts[key] || 0) + 1;
    copyIdx.push(counts[key]);
  }
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a >= state.vertices.length || e.b >= state.vertices.length) continue;
    const a = state.vertices[e.a], b = state.vertices[e.b];
    const key = Math.min(e.a, e.b) + '-' + Math.max(e.a, e.b);
    const n = counts[key], j = copyIdx[i];
    const mc = massColor(e.mass || 0, getEdgeMassKind(e));
    const edgeCol = mc || 'var(--edge-color)';
    const sw = (mc ? 0.065 : 0.05) * lineScale;
    const lineStyle = e.style || 'solid';

    let el;
    // ── Self-loop (tadpole) — teardrop shape with edge avoidance ──
    if (e.a === e.b) {
      // Collect angles of all other edges at this vertex for avoidance
      const edgeAngles = [];
      for (let k = 0; k < state.edges.length; k++) {
        if (k === i) continue;
        const ek = state.edges[k];
        if (ek.a === e.a && ek.b === e.a) {
          // Another tadpole — will be fanned, skip for avoidance
          continue;
        }
        if (ek.a === e.a || ek.b === e.a) {
          const other = ek.a === e.a ? ek.b : ek.a;
          if (other < state.vertices.length) {
            const ov = state.vertices[other];
            edgeAngles.push(Math.atan2(ov.y - a.y, ov.x - a.x));
          }
        }
      }

      // Choose base angle: find the largest angular gap between edges
      let baseAng;
      if (edgeAngles.length === 0) {
        // No other edges: point away from centroid (or up)
        let gcx = 0, gcy = 0;
        state.vertices.forEach(v => { gcx += v.x; gcy += v.y; });
        gcx /= state.vertices.length || 1; gcy /= state.vertices.length || 1;
        let dx = a.x - gcx, dy = a.y - gcy;
        const dl = Math.sqrt(dx*dx + dy*dy);
        if (dl < 0.01) baseAng = -Math.PI / 2;
        else baseAng = Math.atan2(dy, dx);
      } else {
        edgeAngles.sort((x, y) => x - y);
        let bestGap = 0, bestMid = -Math.PI / 2;
        for (let k = 0; k < edgeAngles.length; k++) {
          const next = k + 1 < edgeAngles.length ? edgeAngles[k+1] : edgeAngles[0] + 2 * Math.PI;
          const gap = next - edgeAngles[k];
          if (gap > bestGap) { bestGap = gap; bestMid = edgeAngles[k] + gap / 2; }
        }
        baseAng = bestMid;
      }

      // Fan multiple tadpoles on same vertex
      const spread = n > 1 ? 0.55 : 0;
      const ang = baseAng + (j - (n+1)/2) * spread;
      const loopH = 0.45;  // height of teardrop from vertex
      const loopW = 0.16;  // half-width at the widest point
      const cos = Math.cos(ang), sin = Math.sin(ang);

      // Teardrop path: vertex → bulge → vertex via two cubic Beziers
      // Control points in local frame (along ang direction):
      //   cp1: (loopH*0.5, +loopW*1.4)  cp2: (loopH, +loopW*0.4)  tip: (loopH, 0)
      //   cp3: (loopH, -loopW*0.4)  cp4: (loopH*0.5, -loopW*1.4)  back to vertex
      const cp1x = a.x + cos * loopH * 0.4 - sin * loopW * 1.5;
      const cp1y = a.y + sin * loopH * 0.4 + cos * loopW * 1.5;
      const cp2x = a.x + cos * loopH * 0.95 - sin * loopW * 0.5;
      const cp2y = a.y + sin * loopH * 0.95 + cos * loopW * 0.5;
      const tipx = a.x + cos * loopH;
      const tipy = a.y + sin * loopH;
      const cp3x = a.x + cos * loopH * 0.95 + sin * loopW * 0.5;
      const cp3y = a.y + sin * loopH * 0.95 - cos * loopW * 0.5;
      const cp4x = a.x + cos * loopH * 0.4 + sin * loopW * 1.5;
      const cp4y = a.y + sin * loopH * 0.4 - cos * loopW * 1.5;

      el = document.createElementNS(SVG_NS, 'path');
      el.setAttribute('d',
        `M${a.x},${a.y} C${cp1x},${cp1y} ${cp2x},${cp2y} ${tipx},${tipy} C${cp3x},${cp3y} ${cp4x},${cp4y} ${a.x},${a.y}`
      );
      el.setAttribute('fill', 'none');
      el.setAttribute('stroke', edgeCol);
      el.setAttribute('stroke-width', sw);
      el.setAttribute('stroke-linecap', 'round');
      if (lineStyle === 'dashed') el.setAttribute('stroke-dasharray', `${sw * 3} ${sw * 2}`);
      // Store loop geometry for bubble/label placement (center of teardrop)
      e._loopCx = a.x + cos * loopH * 0.55;
      e._loopCy = a.y + sin * loopH * 0.55;
      e._loopR = loopH * 0.35;
      e._loopAng = ang;
    } else if (n === 1) {
      el = createStyledLine(a.x, a.y, b.x, b.y, lineStyle, edgeCol, sw);
    } else {
      const mx = (a.x+b.x)/2, my = (a.y+b.y)/2;
      // Use canonical direction (min→max vertex index) so perpendicular is
      // consistent across all edges in the same multi-edge group.
      const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
      const dx = hi.x-lo.x, dy = hi.y-lo.y;
      const len = Math.sqrt(dx*dx+dy*dy)||1;
      const px = -dy/len, py = dx/len;
      const off = (j-(n+1)/2)*0.5;
      // For multi-edges with non-solid styles, use endpoints offset by the curve
      if (lineStyle === 'solid' || lineStyle === 'dashed') {
        el = document.createElementNS(SVG_NS, 'path');
        el.setAttribute('d', `M${a.x},${a.y} Q${mx+off*px},${my+off*py} ${b.x},${b.y}`);
        el.setAttribute('fill', 'none');
        el.setAttribute('stroke', edgeCol);
        el.setAttribute('stroke-width', sw);
        el.setAttribute('stroke-linecap', 'round');
        if (lineStyle === 'dashed') el.setAttribute('stroke-dasharray', `${sw * 3} ${sw * 2}`);
      } else {
        // For wavy/gluon multi-edges, offset the straight endpoints
        el = createStyledLine(
          a.x + off * px, a.y + off * py,
          b.x + off * px, b.y + off * py,
          lineStyle, edgeCol, sw
        );
      }
    }
    if (el) {
      if (i === state.newEdgeIdx) {
        el.style.strokeDasharray = '100'; el.style.strokeDashoffset = '100';
        el.style.animation = 'edgeGrow 0.3s ease-out forwards';
      } else if (state.splitEdgeIndices.indexOf(i) >= 0) {
        el.style.animation = 'edgeSplit 0.45s ease-out forwards';
      }
      el.dataset.edgeIdx = i;
      edgeLayer.appendChild(el);

      // Wider invisible hit overlay so hovering anywhere near the edge
      // (not only the tiny mass-picker circle) brings up the tooltip.
      // Without this, the edge is ~6 px thick and the affordance is
      // undiscoverable unless the user knows to hunt for the dot.
      const hitPath = document.createElementNS(SVG_NS, 'path');
      if (e.a === e.b) {
        // Self-loop: reuse the same cubic Bezier geometry.
        hitPath.setAttribute('d', el.getAttribute('d') || '');
      } else if (n > 1) {
        const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
        const lo = state.vertices[Math.min(e.a, e.b)];
        const hi = state.vertices[Math.max(e.a, e.b)];
        const dx = hi.x - lo.x, dy = hi.y - lo.y;
        const len = Math.sqrt(dx * dx + dy * dy) || 1;
        const px = -dy / len, py = dx / len;
        const off = (j - (n + 1) / 2) * 0.5;
        hitPath.setAttribute('d',
          `M${a.x},${a.y} Q${mx + off * px},${my + off * py} ${b.x},${b.y}`);
      } else {
        hitPath.setAttribute('d', `M${a.x},${a.y} L${b.x},${b.y}`);
      }
      hitPath.setAttribute('fill', 'none');
      hitPath.setAttribute('stroke', 'transparent');
      hitPath.setAttribute('stroke-width', '0.22');
      hitPath.setAttribute('stroke-linecap', 'round');
      hitPath.style.cursor = 'pointer';
      hitPath.style.pointerEvents = 'stroke';
      hitPath.dataset.edgeHitIdx = i;
      const maybeShowTip = (evt) => {
        if (massPickerEdge !== null) return;
        // Don't flash the tooltip while the user is mid-drag (edge preview,
        // vertex drag, panning) — it would fight the drag indicator.
        if (state.edgeDragFrom !== null || state.dragging !== null) return;
        showEdgeTooltip(i, evt);
      };
      hitPath.addEventListener('mouseenter', maybeShowTip);
      hitPath.addEventListener('mousemove', maybeShowTip);
      hitPath.addEventListener('mouseleave', () => hideEdgeTooltip());
      edgeLayer.appendChild(hitPath);
    }

    // Momentum arrow (shown in momenta label mode, skip for self-loops)
    // Arrow always points from e.a → e.b, matching the solver's sign convention.
    // Reversing an edge (swapping a,b) flips the arrow and negates the momentum label.
    if (showArrows && e.a !== e.b) {
      const dirX = b.x - a.x;
      const dirY = b.y - a.y;
      const dlen = Math.sqrt(dirX*dirX + dirY*dirY) || 1;
      const ux = dirX/dlen, uy = dirY/dlen;

      // Arrow position: at t≈0.35 along the edge (offset from midpoint to avoid bubble)
      const tArrow = 0.35;
      let arrowX, arrowY;
      if (n > 1) {
        const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
        const dx = hi.x-lo.x, dy = hi.y-lo.y, len = Math.sqrt(dx*dx+dy*dy)||1;
        const px = -dy/len, py = dx/len;
        const off = (j-(n+1)/2)*0.5;
        if (lineStyle === 'solid' || lineStyle === 'dashed') {
          // Quadratic Bézier at parameter t: (1-t)²A + 2t(1-t)C + t²B
          const ctrlX = (a.x+b.x)/2 + off*px, ctrlY = (a.y+b.y)/2 + off*py;
          const t1 = 1 - tArrow;
          arrowX = t1*t1*a.x + 2*tArrow*t1*ctrlX + tArrow*tArrow*b.x;
          arrowY = t1*t1*a.y + 2*tArrow*t1*ctrlY + tArrow*tArrow*b.y;
        } else {
          arrowX = a.x + (b.x-a.x)*tArrow + off*px;
          arrowY = a.y + (b.y-a.y)*tArrow + off*py;
        }
      } else {
        arrowX = a.x + (b.x - a.x) * tArrow;
        arrowY = a.y + (b.y - a.y) * tArrow;
      }

      const arrLen = 0.13;   // length of arrowhead along edge
      const arrW = 0.07;    // half-width of arrowhead base
      const tipX = arrowX + ux * arrLen * 0.5;
      const tipY = arrowY + uy * arrLen * 0.5;
      const baseX = arrowX - ux * arrLen * 0.5;
      const baseY = arrowY - uy * arrLen * 0.5;
      const anx = -uy, any_ = ux;
      const resolvedCol = mc || getComputedStyle(document.documentElement).getPropertyValue('--edge-color').trim();
      // Chevron (>) shape — open polyline, no fill
      const arrow = document.createElementNS(SVG_NS, 'polyline');
      arrow.setAttribute('points',
        `${baseX + anx*arrW},${baseY + any_*arrW} ${tipX},${tipY} ${baseX - anx*arrW},${baseY - any_*arrW}`
      );
      arrow.setAttribute('fill', 'none');
      arrow.setAttribute('stroke', resolvedCol);
      arrow.setAttribute('stroke-width', sw);
      arrow.setAttribute('stroke-linecap', 'round');
      arrow.setAttribute('stroke-linejoin', 'round');
      arrow.setAttribute('opacity', '1');
      edgeLayer.appendChild(arrow);
    }

    // Mid-edge bubble — placed at the actual midpoint of the rendered curve
    let emx, emy;
    if (e.a === e.b && e._loopCx !== undefined) {
      // Self-loop: bubble at the far point of the loop circle
      emx = e._loopCx + Math.cos(e._loopAng) * e._loopR;
      emy = e._loopCy + Math.sin(e._loopAng) * e._loopR;
    } else if (n > 1) {
      const lo = state.vertices[Math.min(e.a, e.b)], hi = state.vertices[Math.max(e.a, e.b)];
      const dx = hi.x-lo.x, dy = hi.y-lo.y, len = Math.sqrt(dx*dx+dy*dy)||1;
      const px = -dy/len, py = dx/len;
      const off = (j-(n+1)/2)*0.5;
      if (lineStyle === 'solid' || lineStyle === 'dashed') {
        // Quadratic Bézier: midpoint at t=0.5 is (A + 2C + B)/4
        const cx = (a.x+b.x)/2 + off*px, cy = (a.y+b.y)/2 + off*py;
        emx = (a.x + 2*cx + b.x) / 4;
        emy = (a.y + 2*cy + b.y) / 4;
      } else {
        // Offset straight line: midpoint of the offset segment
        emx = (a.x + b.x)/2 + off*px;
        emy = (a.y + b.y)/2 + off*py;
      }
    } else {
      emx = (a.x+b.x)/2;
      emy = (a.y+b.y)/2;
    }
    const bubbleG = document.createElementNS(SVG_NS, 'g');
    bubbleG.classList.add('edge-bubble');
    if (!window._bubbleIntroShown && state.edges.length <= 3) {
      bubbleG.classList.add('bubble-intro');
      window._bubbleIntroShown = true;
    }
    bubbleG.setAttribute('data-edge', i);
    // Invisible larger hit target
    const hit = document.createElementNS(SVG_NS, 'circle');
    hit.setAttribute('cx', emx); hit.setAttribute('cy', emy);
    hit.setAttribute('r', '0.15');
    hit.setAttribute('fill', 'transparent');
    hit.setAttribute('stroke', 'none');
    bubbleG.appendChild(hit);
    const bg = document.createElementNS(SVG_NS, 'circle');
    bg.setAttribute('cx', emx); bg.setAttribute('cy', emy);
    bg.setAttribute('r', '0.075');
    bg.setAttribute('fill', 'var(--bg)');
    bg.setAttribute('stroke', 'var(--border)');
    bg.setAttribute('stroke-width', '0.012');
    bg.setAttribute('opacity', '0.85');
    bubbleG.appendChild(bg);

    // Spinner ring if this edge is being edited in the mass picker
    if (massPickerEdge === i) {
      // Static filled circle underneath
      const bgFill = document.createElementNS(SVG_NS, 'circle');
      bgFill.setAttribute('cx', emx); bgFill.setAttribute('cy', emy);
      bgFill.setAttribute('r', '0.075');
      bgFill.setAttribute('fill', 'var(--bg)');
      bgFill.setAttribute('stroke', 'none');
      bgFill.setAttribute('opacity', '0.85');
      bubbleG.insertBefore(bgFill, bg);
      // The bg circle becomes the spinning ring (no fill)
      bg.setAttribute('fill', 'none');
      bg.style.transformOrigin = `${emx}px ${emy}px`;
      bg.classList.add('edge-bubble-active');
    }

    bubbleG.style.cursor = state.mode === 'delete' ? 'not-allowed' : 'pointer';
    // Intercept pointer events (the canvas uses pointerdown for vertex/edge
    // creation; without handling that here, clicking this bubble fires the
    // canvas handler first and subdivides the edge into a new vertex).
    bubbleG.addEventListener('pointerdown', (evt) => { evt.stopPropagation(); });
    bubbleG.addEventListener('mousedown', (evt) => {
      evt.stopPropagation();
      evt.preventDefault();
      if (state.mode === 'delete') {
        // Delete this edge — allow if it's a leaf edge (external)
        const edge = state.edges[i];
        const deg = getVertexDegrees();
        const degA = deg[edge.a] || 0;
        const degB = deg[edge.b] || 0;
        const orphans = [];
        if (degA === 1) orphans.push(edge.a);
        if (degB === 1) orphans.push(edge.b);

        if (orphans.length === 0 && wouldEdgeRemovalDisconnect(i)) {
          showConnectedWarning();
        } else {
          pushUndoState();
          state.edges.splice(i, 1);
          // Remove orphaned leaf vertices (highest index first)
          orphans.sort((a, b) => b - a).forEach(oi => {
            state.vertices.splice(oi, 1);
            state.edges = state.edges.map(e => ({
              ...e,
              a: e.a > oi ? e.a - 1 : e.a,
              b: e.b > oi ? e.b - 1 : e.b,
            }));
          });
          render();
          onGraphChanged();
        }
      } else {
        openMassPicker(i, emx, emy);
      }
    });
    bubbleG.addEventListener('mouseup', (evt) => { evt.stopPropagation(); });
    bubbleG.addEventListener('click', (evt) => { evt.stopPropagation(); });
    bubbleG.addEventListener('mouseenter', (evt) => {
      if (massPickerEdge !== null) return; // don't show tooltip while picker is open
      showEdgeTooltip(i, evt);
    });
    bubbleG.addEventListener('mouseleave', () => { hideEdgeTooltip(); });
    edgeLayer.appendChild(bubbleG);
  }
  renderMassLegend();
}

// ─── KaTeX helper ───────────────────────────────────────────────────

// Render a string that may contain inline $...$ math into safe HTML.
// Non-math text is HTML-escaped; math chunks are rendered with KaTeX and
// gracefully fall back to a literal "$expr$" if parsing fails or KaTeX
// hasn't loaded yet. Unpaired dollars pass through escaped.
function renderInlineMathString(s) {
  if (typeof s !== 'string' || !s) return '';
  // Allow escaping a literal dollar as "\$" — stash to a sentinel.
  const DOLLAR = '\uE000';
  const input = s.replace(/\\\$/g, DOLLAR);
  const parts = input.split('$');
  if (parts.length < 3) {
    // No pair of dollars → plain text.
    return escapeHtml(input.replace(new RegExp(DOLLAR, 'g'), '$'));
  }
  let out = '';
  for (let i = 0; i < parts.length; i++) {
    const chunk = parts[i].replace(new RegExp(DOLLAR, 'g'), '$');
    if (i % 2 === 0) {
      out += escapeHtml(chunk);
    } else if (i === parts.length - 1) {
      // Dangling odd → treat the opening $ as literal.
      out += escapeHtml('$' + chunk);
    } else if (typeof katex !== 'undefined') {
      try {
        out += katex.renderToString(chunk, { throwOnError: true, displayMode: false });
      } catch {
        out += escapeHtml('$' + chunk + '$');
      }
    } else {
      out += escapeHtml('$' + chunk + '$');
    }
  }
  return out;
}

function renderTeX(latex, el) {
  if (typeof katex !== 'undefined') {
    try {
      // Try strict render first to detect parse errors
      katex.render(latex, el, { throwOnError: true, displayMode: false });
    } catch {
      // Incomplete or invalid LaTeX (e.g. user mid-typing "m_")
      // Show the raw text in muted style instead of a red error
      el.textContent = latex;
      el.style.color = 'var(--text-muted)';
      el.style.fontStyle = 'italic';
      return;
    }
    el.style.color = '';
    el.style.fontStyle = '';
  } else {
    el.textContent = latex;
  }
}

// ─── Edge hover tooltip ─────────────────────────────────────────────

function showEdgeTooltip(edgeIdx, evt) {
  const e = state.edges[edgeIdx];
  const tip = $('edge-tooltip');
  const deg = getVertexDegrees();
  const isExt = (deg[e.a] || 0) === 1 || (deg[e.b] || 0) === 1;

  let rows = '';
  // Edge number
  rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Edge</span><span class="edge-tooltip-val">${edgeIdx + 1}${isExt ? ' (external)' : ''}</span></div>`;
  // Momentum
  const momLabels = getMomentumLabels();
  if (momLabels && momLabels[edgeIdx] && momLabels[edgeIdx] !== '0') {
    rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Mom.</span><span class="edge-tooltip-val" id="edge-tip-mom"></span></div>`;
  }
  // Mass
  const mass = e.mass || 0;
  const mc = massColor(mass, getEdgeMassKind(e));
  const massLabel = getMassDisplayLabel(mass, e);
  const swatchHTML = mc ? `<span class="edge-tooltip-swatch" style="background:${mc}"></span> ` : '';
  rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Mass</span><span class="edge-tooltip-val">${swatchHTML}<span id="edge-tip-mass"></span></span></div>`;
  // Style
  const style = e.style || 'solid';
  if (style !== 'solid') {
    rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Style</span><span class="edge-tooltip-val">${style}</span></div>`;
  }
  // Propagator exponent
  const exp = e.propExponent ?? 1;
  if (exp !== 1 && !isExt) {
    rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Exponent</span><span class="edge-tooltip-val">\u03BD = ${exp}</span></div>`;
  }
  // Particle label
  if (e.edgeLabel && e.edgeLabel.trim()) {
    rows += `<div class="edge-tooltip-row"><span class="edge-tooltip-label">Label</span><span class="edge-tooltip-val" id="edge-tip-label"></span></div>`;
  }

  tip.innerHTML = rows;

  // Render KaTeX into the placeholder spans
  const momEl = tip.querySelector('#edge-tip-mom');
  if (momEl && momLabels && momLabels[edgeIdx] && momLabels[edgeIdx] !== '0') {
    renderTeX(momLabels[edgeIdx], momEl);
  }
  const massEl = tip.querySelector('#edge-tip-mass');
  if (massEl) renderTeX(massLabel, massEl);
  const labelEl = tip.querySelector('#edge-tip-label');
  if (labelEl && e.edgeLabel) renderTeX(e.edgeLabel.trim(), labelEl);

  // Position above the cursor
  tip.classList.add('visible');
  const tipRect = tip.getBoundingClientRect();
  let left = evt.clientX;
  let top = evt.clientY - tipRect.height - 12;
  if (top < 48) top = evt.clientY + 16; // flip below if near toolbar
  if (left + tipRect.width / 2 > window.innerWidth - 8) left = window.innerWidth - tipRect.width / 2 - 8;
  if (left - tipRect.width / 2 < 8) left = tipRect.width / 2 + 8;
  tip.style.left = left + 'px';
  tip.style.top = top + 'px';
}

function hideEdgeTooltip() {
  $('edge-tooltip').classList.remove('visible');
}

// ─── Mass picker tooltip ────────────────────────────────────────────

let _massPickerJustOpened = false;

/**
 * Get the LaTeX display label for a mass value.
 * User-typed labels like m_H become m_{H}, M_W becomes M_{W}, etc.
 */
// Kind of a mass slot: 'm' for internal-propagator masses (default on
// interior edges) or 'M' for external-leg masses (square roots of squared
// external momenta). Read from edge.massKind if the user set it explicitly;
// otherwise inferred from edge topology — a degree-1-endpoint edge is an
// external leg and defaults to 'M'. Returns null for the massless case.
function getEdgeMassKind(edge) {
  if (!edge || !edge.mass || edge.mass === 0) return null;
  if (edge.massKind === 'M' || edge.massKind === 'm') return edge.massKind;
  const deg = getVertexDegrees();
  const isExtLeg = (deg[edge.a] || 0) === 1 || (deg[edge.b] || 0) === 1;
  return isExtLeg ? 'M' : 'm';
}

// Count distinct (kind, mass) slots in use. Used by the label renderer to
// decide 'm' vs 'm_{1}' etc.: when only one m-kind slot is live, it renders
// as bare 'm'; two or more, they split into 'm_{1}, m_{2}'. Same for 'M'.
function getDistinctMassSlots(kind) {
  const distinct = new Set();
  for (const e of state.edges) {
    if (e.mass && e.mass > 0 && getEdgeMassKind(e) === kind) distinct.add(e.mass);
  }
  return distinct;
}

function getMassDisplayLabel(mass, edge) {
  if (edge && edge.massLabel) return massLabelToTeX(edge.massLabel);
  if (!mass || mass === 0) return '0';
  const kind = getEdgeMassKind(edge) || 'm';
  const distinct = getDistinctMassSlots(kind);
  return distinct.size <= 1 ? kind : `${kind}_{${mass}}`;
}

/**
 * Convert a user-typed mass label to proper LaTeX.
 * e.g. "m_H" → "m_{H}", "m_top" → "m_{top}", "M_W" → "M_{W}"
 * Labels without underscores pass through unchanged.
 */
function massLabelToTeX(label) {
  if (!label) return label;
  // If user typed something like m_H or M_top, wrap subscript in braces
  return label.replace(/_([^{].*?)(?=$|\s|\^)/g, '_{$1}');
}

// Centralized edge-to-Mma emit, so every downstream path (STIntegrate
// command builder, notebook download, Export tab, kinematics panel) uses
// the same kind-aware convention:
//
//   - custom label (user-typed in the picker): sanitized to atomic via
//     massLabelToMma, matching the "m_H -> mH" rule.
//   - auto-numbered internal masses: `m` when there's only one internal
//     slot on the canvas, else `Subscript[m, N]` — this is the legacy
//     kernel-side convention that library-stored results were built
//     against, so we preserve it to avoid breaking library matching.
//   - auto-numbered external-leg masses: same story with `M` /
//     `Subscript[M, N]`. Honors edge.massKind (or topology as fallback).
//
// Returns the literal Mma string; the "0" case for massless edges is
// handled at the call site because some callers want to skip the edge
// rather than emit "0".
function edgeMassToMma(edge) {
  if (!edge || !edge.mass || edge.mass === 0) return '0';
  if (edge.massLabel) return massLabelToMma(edge.massLabel);
  const kind = getEdgeMassKind(edge) || 'm';
  const count = getDistinctMassSlots(kind).size;
  return count <= 1 ? kind : `Subscript[${kind}, ${edge.mass}]`;
}

// Map a small set of common unicode symbols to Mma-legal ASCII.
// Extend as needed; anything not in the map is kept verbatim during sanitize,
// and only ASCII-alphanumeric survives the final pass.
const UNICODE_TO_ASCII = {
  'μ': 'mu', 'ν': 'nu', 'ξ': 'xi', 'π': 'pi', 'ρ': 'rho', 'σ': 'sigma',
  'τ': 'tau', 'φ': 'phi', 'χ': 'chi', 'ψ': 'psi', 'ω': 'omega',
  'α': 'alpha', 'β': 'beta', 'γ': 'gamma', 'δ': 'delta', 'ε': 'epsilon',
  'ζ': 'zeta', 'η': 'eta', 'θ': 'theta', 'ι': 'iota', 'κ': 'kappa',
  'λ': 'lambda',
};

/**
 * Convert a user-typed mass label to an atomic Mathematica symbol.
 * User types display-style labels like "m_H", "M_W", "μ" for readability in
 * the canvas/popup; STIntegrate needs a plain atomic symbol (no underscore,
 * which is pattern syntax; no unicode, which breaks many downstream
 * Variables[]/OptionValue[] lookups). We strip underscores, braces, and
 * backslashes, fold common Greek letters to their ASCII names, and drop any
 * other non-alphanumeric character.
 *
 *   "m_H"   → "mH"            "M_W"   → "MW"
 *   "m_top" → "mtop"          "μ"     → "mu"
 *   "m1"    → "m1"            "m"     → "m"        (unchanged)
 *
 * Auto-numbered masses from the popup's "new" slot go through a different
 * path (buildGraphArgJS emits m[1], m[2], M[1], ... directly). This function
 * only handles user-typed custom labels stored in edge.massLabel.
 */
function massLabelToMma(label) {
  if (!label) return label;
  let s = String(label);
  // Fold unicode greek to ASCII.
  s = s.replace(/./gu, ch => UNICODE_TO_ASCII[ch] ?? ch);
  // Drop brace/backslash TeX artifacts the user might have typed.
  s = s.replace(/[{}\\]/g, '');
  // Collapse _XYZ into the bare XYZ (atomic symbol: m_H -> mH).
  s = s.replace(/_+/g, '');
  // Strip any remaining non-alphanumeric character so the result is a
  // legal atomic Mma symbol.
  s = s.replace(/[^A-Za-z0-9]/g, '');
  // Mma symbols must start with a letter; prepend 'm' if caller typed a
  // label that reduced to digits only (defensive — unlikely in practice).
  if (/^[0-9]/.test(s)) s = 'm' + s;
  return s || label;  // fall back to original if sanitizing ate everything
}

function openMassPicker(edgeIdx, svgX, svgY) {
  hideEdgeTooltip();
  closeMassPicker();
  massPickerEdge = edgeIdx;
  _massPickerJustOpened = true;
  setTimeout(() => { _massPickerJustOpened = false; }, 300);
  render();
  const picker = $('mass-picker');
  const body = $('mass-picker-body');
  body.innerHTML = '';

  // Close button
  const closeBtn = document.createElement('button');
  closeBtn.className = 'picker-close';
  closeBtn.innerHTML = '&times;';
  closeBtn.addEventListener('click', (evt) => { evt.stopPropagation(); closeMassPicker(); });
  body.appendChild(closeBtn);

  const columns = document.createElement('div');
  columns.className = 'picker-columns';

  // ── Column 1: Mass ──
  const massCol = document.createElement('div');
  massCol.className = 'picker-col';
  const massTitle = document.createElement('div');
  massTitle.className = 'picker-col-title';
  massTitle.textContent = 'Mass';
  massCol.appendChild(massTitle);

  // Determine whether the edge being picked is an external leg (degree-1
  // endpoint) or interior. External defaults new masses to M-family,
  // interior to m-family.
  const degMap = getVertexDegrees();
  const thisEdge = state.edges[edgeIdx];
  const thisIsExt = (degMap[thisEdge.a] || 0) === 1 || (degMap[thisEdge.b] || 0) === 1;
  const primaryKind = thisIsExt ? 'M' : 'm';
  const secondaryKind = thisIsExt ? 'm' : 'M';

  // Walk the current graph to collect the distinct mass slots in use,
  // bucketed by kind. Each bucket is keyed by the mass integer; the first
  // edge we see with that kind determines the color/swatch.
  const slotsByKind = { m: new Map(), M: new Map() };
  for (const e of state.edges) {
    if (!e.mass || e.mass === 0) continue;
    const k = getEdgeMassKind(e);
    if (!k) continue;
    if (!slotsByKind[k].has(e.mass)) {
      slotsByKind[k].set(e.mass, {
        mass: e.mass,
        kind: k,
        massLabel: e.massLabel || '',
      });
    }
  }

  // Render a label for a mass slot: custom label wins; else follow the
  // bare-'m' vs 'm_{N}' rule based on how many distinct slots of this kind
  // are in use. Preview labels for the "new" slot reuse the same logic.
  function labelForSlot(kind, mass, customLabel) {
    if (customLabel) return massLabelToTeX(customLabel);
    const count = slotsByKind[kind].size;
    return count <= 1 ? kind : `${kind}_{${mass}}`;
  }

  // Next unused integer in a given kind bucket — used as the mass index the
  // "new (kind)" button would mint. Starts at 1 if no slots of this kind
  // exist yet.
  function nextSlotIndex(kind) {
    const inUse = slotsByKind[kind];
    let i = 1;
    while (inUse.has(i)) i++;
    return i;
  }

  function makeMassBtn({ kind, mass, customLabel, isNewSlot = false }) {
    const thisMass = state.edges[edgeIdx].mass || 0;
    const thisKind = getEdgeMassKind(state.edges[edgeIdx]);
    const isActive = !isNewSlot && thisMass === mass && thisKind === kind;
    const btn = document.createElement('button');
    btn.className = 'mass-option' + (isActive ? ' active' : '') +
      (isNewSlot ? ' mass-option-new' : '');
    const swatch = document.createElement('span');
    swatch.className = 'mass-swatch';
    swatch.style.background = MASS_PALETTE[slotPaletteIndex(mass, kind)];
    btn.appendChild(swatch);
    const label = document.createElement('span');
    label.className = 'mass-tex';
    // Preview for "new" slots shows what the label WOULD be if picked —
    // i.e. compute with slotsByKind[kind].size + 1 so the bare-'m' rule
    // flips to 'm_{1}' / 'm_{2}' when the second slot of this kind is
    // about to appear.
    let displayLabel;
    if (isNewSlot) {
      const futureCount = slotsByKind[kind].size + 1;
      displayLabel = futureCount <= 1 ? kind : `${kind}_{${mass}}`;
    } else {
      displayLabel = labelForSlot(kind, mass, customLabel);
    }
    renderTeX(displayLabel, label);
    btn.appendChild(label);
    btn.addEventListener('click', () => { setEdgeMass(edgeIdx, mass, kind); });
    return btn;
  }

  // 0 (massless) — always offered.
  const zeroBtn = document.createElement('button');
  const thisEdgeMass = thisEdge.mass || 0;
  zeroBtn.className = 'mass-option' + (thisEdgeMass === 0 ? ' active' : '');
  const zSw = document.createElement('span');
  zSw.className = 'mass-swatch';
  zSw.style.background = 'var(--edge-color)';
  zeroBtn.appendChild(zSw);
  const zL = document.createElement('span');
  zL.className = 'mass-tex';
  renderTeX('0', zL);
  zeroBtn.appendChild(zL);
  zeroBtn.addEventListener('click', () => { setEdgeMass(edgeIdx, 0); });
  massCol.appendChild(zeroBtn);

  // Existing same-kind slots (primary). Sorted by mass index so the canvas
  // labels ('m_{1}, m_{2}, ...') line up with the popup ordering.
  const primarySlots = [...slotsByKind[primaryKind].values()]
    .sort((a, b) => a.mass - b.mass);
  for (const slot of primarySlots) {
    massCol.appendChild(makeMassBtn({
      kind: slot.kind, mass: slot.mass, customLabel: slot.massLabel,
    }));
  }

  // One "new" slot for the primary kind — this is the user's "m_{2}" option
  // when there's already an 'm' on the canvas, or 'm' if none exists yet.
  massCol.appendChild(makeMassBtn({
    kind: primaryKind, mass: nextSlotIndex(primaryKind), isNewSlot: true,
  }));

  // Existing cross-kind slots + one "new" cross-kind — present so a user can
  // tie (e.g.) an external leg to an internal mass 'm' (on-shell at m), or
  // start a fresh 'M' on an interior edge. Rendered in the same column for
  // now; a future polish pass can visually separate.
  const secondarySlots = [...slotsByKind[secondaryKind].values()]
    .sort((a, b) => a.mass - b.mass);
  for (const slot of secondarySlots) {
    massCol.appendChild(makeMassBtn({
      kind: slot.kind, mass: slot.mass, customLabel: slot.massLabel,
    }));
  }
  massCol.appendChild(makeMassBtn({
    kind: secondaryKind, mass: nextSlotIndex(secondaryKind), isNewSlot: true,
  }));

  // Custom mass name input (e.g., m_W, M_H)
  const currentMass = state.edges[edgeIdx].mass || 0;
  if (currentMass > 0) {
    const massNameTitle = document.createElement('div');
    massNameTitle.className = 'picker-col-title';
    massNameTitle.style.marginTop = '6px';
    massNameTitle.textContent = 'Mass name';
    massCol.appendChild(massNameTitle);

    const massNameInput = document.createElement('input');
    massNameInput.className = 'picker-label-input';
    massNameInput.type = 'text';
    massNameInput.placeholder = (() => {
      const activeKind = getEdgeMassKind(state.edges[edgeIdx]) || 'm';
      const count = getDistinctMassSlots(activeKind).size;
      return count <= 1 ? activeKind : `${activeKind}_{${currentMass}}`;
    })();
    massNameInput.value = state.edges[edgeIdx].massLabel || '';
    massNameInput.addEventListener('input', () => {
      state.edges[edgeIdx].massLabel = massNameInput.value;
      // 2-leg: sync label to paired leg
      const extE = getExternalEdges();
      if (extE.length === 2 && extE.includes(edgeIdx)) {
        const otherIdx = extE[0] === edgeIdx ? extE[1] : extE[0];
        state.edges[otherIdx].massLabel = massNameInput.value;
      }
      render();
      renderMassLegend();
    });
    massNameInput.addEventListener('keydown', (evt) => {
      if (evt.key === 'Enter') closeMassPicker();
      evt.stopPropagation();
    });
    massNameInput.addEventListener('click', (evt) => evt.stopPropagation());
    massCol.appendChild(massNameInput);
  }

  // ── Column 2: Type ──
  const styleCol = document.createElement('div');
  styleCol.className = 'picker-col picker-col-wide';
  const styleTitle = document.createElement('div');
  styleTitle.className = 'picker-col-title';
  styleTitle.textContent = 'Type';
  styleCol.appendChild(styleTitle);

  const currentStyle = state.edges[edgeIdx].style || 'solid';
  const STYLE_TIPS = {
    solid:   'Solid line — fermion (quark, lepton). Default; the visual style does not affect the physics.',
    dashed:  'Dashed — Higgs or scalar. Cosmetic; propagator comes from the mass.',
    wavy:    'Wavy — photon or W/Z. Cosmetic; propagator comes from the mass.',
    dblwavy: 'Double-wavy — graviton. Cosmetic.',
    gluon:   'Coil — gluon. Cosmetic; the style does not change the integral.',
  };
  LINE_STYLES.forEach(ls => {
    const btn = document.createElement('button');
    btn.className = 'style-option' + (currentStyle === ls.id ? ' active' : '');
    if (STYLE_TIPS[ls.id]) btn.dataset.tip = STYLE_TIPS[ls.id];
    const preview = document.createElementNS(SVG_NS, 'svg');
    preview.setAttribute('viewBox', '0 0 32 8');
    preview.setAttribute('width', '32');
    preview.setAttribute('height', '8');
    preview.classList.add('style-preview');
    const previewLine = createStyledLine(0, 4, 32, 4, ls.id, 'var(--text)', 1.5);
    if (previewLine) preview.appendChild(previewLine);
    btn.appendChild(preview);
    const label = document.createElement('span');
    label.className = 'style-label';
    label.textContent = ls.label;
    btn.appendChild(label);
    btn.addEventListener('click', () => { setEdgeStyle(edgeIdx, ls.id); });
    styleCol.appendChild(btn);
  });

  // ── Column 3: Label ──
  const labelCol = document.createElement('div');
  labelCol.className = 'picker-col';
  const labelTitle = document.createElement('div');
  labelTitle.className = 'picker-col-title';
  labelTitle.textContent = 'Label';
  labelCol.appendChild(labelTitle);

  const labelInput = document.createElement('input');
  labelInput.className = 'picker-label-input';
  labelInput.type = 'text';
  labelInput.placeholder = 'e.g. H, \\gamma, g';
  labelInput.value = state.edges[edgeIdx].edgeLabel || '';
  labelInput.addEventListener('input', () => {
    state.edges[edgeIdx].edgeLabel = labelInput.value;
    render();
  });
  labelInput.addEventListener('keydown', (evt) => {
    if (evt.key === 'Enter') closeMassPicker();
    evt.stopPropagation();
  });
  labelInput.addEventListener('click', (evt) => evt.stopPropagation());
  labelCol.appendChild(labelInput);

  // Preview of the label
  const labelPreview = document.createElement('div');
  labelPreview.className = 'picker-label-preview';
  const updateLabelPreview = () => {
    const val = labelInput.value.trim();
    if (val) renderTeX(val, labelPreview);
    else labelPreview.textContent = '';
  };
  updateLabelPreview();
  labelInput.addEventListener('input', updateLabelPreview);
  labelCol.appendChild(labelPreview);

  // Momentum label input for external legs
  const deg = getVertexDegrees();
  const isExtLeg = (deg[state.edges[edgeIdx].a] || 0) === 1 || (deg[state.edges[edgeIdx].b] || 0) === 1;
  if (isExtLeg) {
    const momTitle = document.createElement('div');
    momTitle.className = 'picker-col-title';
    momTitle.style.marginTop = '6px';
    momTitle.textContent = 'Momentum';
    labelCol.appendChild(momTitle);

    const momInput = document.createElement('input');
    momInput.className = 'picker-label-input';
    momInput.type = 'text';
    momInput.placeholder = 'e.g. k, q_{1}';
    momInput.value = state.edges[edgeIdx].extMomLabel || '';
    momInput.addEventListener('input', () => {
      state.edges[edgeIdx].extMomLabel = momInput.value;
      _momentumLabels = null;  // invalidate cache
      render();
    });
    momInput.addEventListener('keydown', (evt) => {
      if (evt.key === 'Enter') closeMassPicker();
      evt.stopPropagation();
    });
    momInput.addEventListener('click', (evt) => evt.stopPropagation());
    labelCol.appendChild(momInput);

    const momPreview = document.createElement('div');
    momPreview.className = 'picker-label-preview';
    const updateMomPreview = () => {
      const val = momInput.value.trim();
      if (val) renderTeX(val, momPreview);
      else momPreview.textContent = '';
    };
    updateMomPreview();
    momInput.addEventListener('input', updateMomPreview);
    labelCol.appendChild(momPreview);

    // "Swap with …" chip row. Lets the user transpose this leg with any
    // other external leg from within the edge popup — complements the
    // drag-to-reorder grip in the External masses panel. Each chip
    // is labeled p_N (canonical index), click → swapExtLegs() + close.
    const extEdgesList = [];
    for (let i = 0; i < state.edges.length; i++) {
      const ee = state.edges[i];
      if (ee.a === ee.b) continue;
      const eA = (deg[ee.a] || 0) <= 1;
      const eB = (deg[ee.b] || 0) <= 1;
      if (!eA && !eB) continue;
      extEdgesList.push(i);
    }
    const thisExtIdx = extEdgesList.indexOf(edgeIdx);
    if (thisExtIdx >= 0 && extEdgesList.length >= 2) {
      const SUB_DIGITS = '₀₁₂₃₄₅₆₇₈₉';
      const subscript = (n) => String(n).split('').map(d => SUB_DIGITS[+d] || d).join('');

      const swapLabel = document.createElement('div');
      swapLabel.className = 'picker-col-title';
      swapLabel.style.marginTop = '6px';
      swapLabel.textContent = 'Swap with';
      labelCol.appendChild(swapLabel);

      const swapRow = document.createElement('div');
      swapRow.className = 'picker-swap-row';
      extEdgesList.forEach((_, otherExtIdx) => {
        if (otherExtIdx === thisExtIdx) return;
        const chip = document.createElement('button');
        chip.type = 'button';
        chip.className = 'picker-swap-chip';
        chip.textContent = 'p' + subscript(otherExtIdx + 1);
        chip.title = `Swap this leg (p${subscript(thisExtIdx + 1)}) with p${subscript(otherExtIdx + 1)}`;
        chip.addEventListener('click', (evt) => {
          evt.stopPropagation();
          if (swapExtLegs(thisExtIdx, otherExtIdx)) closeMassPicker();
        });
        swapRow.appendChild(chip);
      });
      labelCol.appendChild(swapRow);
    }
  }

  // Propagator exponent input (for internal edges, full mode only)
  if (!isExtLeg && backendMode === 'full') {
    const expTitle = document.createElement('div');
    expTitle.className = 'picker-col-title';
    expTitle.style.marginTop = '6px';
    expTitle.textContent = 'Exponent (\u03BD)';
    expTitle.dataset.tipHtml = '<strong>Propagator exponent &nu;</strong> — raises the denominator to this power: the edge contributes <code>1 / (q<sup>2</sup> - m<sup>2</sup>)<sup>&nu;</sup></code>. Default <code>1</code>. Values &ge; 2 are dotted propagators common in IBP reductions; <code>0</code> removes the propagator.';
    labelCol.appendChild(expTitle);

    const expInput = document.createElement('input');
    expInput.className = 'picker-label-input';
    expInput.type = 'number';
    expInput.step = '1';
    expInput.min = '0';
    expInput.value = state.edges[edgeIdx].propExponent ?? 1;
    expInput.style.width = '60px';
    expInput.dataset.tipHtml = '<strong>Propagator exponent &nu;</strong> — raises the denominator to this power: the edge contributes <code>1 / (q<sup>2</sup> - m<sup>2</sup>)<sup>&nu;</sup></code>. Default <code>1</code>. Values &ge; 2 are dotted propagators common in IBP reductions; <code>0</code> removes the propagator.';
    expInput.addEventListener('input', () => {
      const val = parseInt(expInput.value);
      if (!isNaN(val) && val >= 0) {
        state.edges[edgeIdx].propExponent = val;
        saveDiagram();
      }
    });
    expInput.addEventListener('keydown', (evt) => {
      if (evt.key === 'Enter') closeMassPicker();
      evt.stopPropagation();
    });
    expInput.addEventListener('click', (evt) => evt.stopPropagation());
    labelCol.appendChild(expInput);
  }

  // Action buttons in the label column
  const reverseBtn = document.createElement('button');
  reverseBtn.className = 'picker-reverse-btn';
  reverseBtn.innerHTML = '<svg viewBox="0 0 16 16" width="12" height="12" style="vertical-align:-1px;fill:none;stroke:currentColor;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round"><path d="M1 5h12M10 2l3 3-3 3"/><path d="M15 11H3M6 8l-3 3 3 3"/></svg> Reverse';
  reverseBtn.addEventListener('click', (evt) => {
    evt.stopPropagation();
    pushUndoState();
    const edge = state.edges[edgeIdx];
    const tmp = edge.a;
    edge.a = edge.b;
    edge.b = tmp;
    // Mark the edge as explicitly reversed by the user so the external-leg
    // normalizer in onGraphChanged() doesn't flip it back on the next mutation.
    edge.arrowReversed = !edge.arrowReversed;
    _momentumLabels = null;
    closeMassPicker();
    render();
    onGraphChanged();
  });
  labelCol.appendChild(reverseBtn);

  const deleteBtn = document.createElement('button');
  deleteBtn.className = 'picker-reverse-btn picker-delete-btn';
  deleteBtn.innerHTML = '<svg viewBox="0 0 16 16" width="12" height="12" style="vertical-align:-1px;fill:none;stroke:currentColor;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round"><line x1="2" y1="2" x2="14" y2="14"/><line x1="14" y1="2" x2="2" y2="14"/></svg> Delete';
  deleteBtn.addEventListener('click', (evt) => {
    evt.stopPropagation();
    if (wouldEdgeRemovalDisconnect(edgeIdx)) {
      showConnectedWarning();
      return;
    }
    pushUndoState();
    const edge = state.edges[edgeIdx];
    const degA = state.edges.filter(e => e.a === edge.a || e.b === edge.a).length;
    const degB = state.edges.filter(e => e.a === edge.b || e.b === edge.b).length;
    const orphans = [];
    if (degA === 1) orphans.push(edge.a);
    if (degB === 1) orphans.push(edge.b);
    state.edges.splice(edgeIdx, 1);
    orphans.sort((a, b) => b - a).forEach(oi => {
      state.vertices.splice(oi, 1);
      state.edges = state.edges.map(e => {
        const c = copyEdge(e);
        if (c.a > oi) c.a--;
        if (c.b > oi) c.b--;
        return c;
      });
    });
    closeMassPicker();
    render();
    onGraphChanged();
  });
  labelCol.appendChild(deleteBtn);

  columns.appendChild(massCol);
  columns.appendChild(styleCol);
  columns.appendChild(labelCol);
  body.appendChild(columns);

  // Position picker with viewport clipping prevention
  const pt = canvas.createSVGPoint();
  pt.x = svgX; pt.y = svgY;
  const screenPt = pt.matrixTransform(canvas.getScreenCTM());
  picker.classList.add('visible');
  const pRect = picker.getBoundingClientRect();
  let left = screenPt.x;
  let top = screenPt.y + 8;
  // Prevent right edge clipping
  if (left + pRect.width / 2 > window.innerWidth - 8) {
    left = window.innerWidth - pRect.width / 2 - 8;
  }
  // Prevent left edge clipping
  if (left - pRect.width / 2 < 8) {
    left = pRect.width / 2 + 8;
  }
  // If picker would go below viewport, show above the point instead
  if (top + pRect.height > window.innerHeight - 8) {
    top = screenPt.y - pRect.height - 8;
  }
  picker.style.left = left + 'px';
  picker.style.top = top + 'px';
}

function startMassLabelEdit(edgeIdx, labelEl) {
  const edge = state.edges[edgeIdx];
  const input = document.createElement('input');
  input.className = 'mass-label-edit';
  input.type = 'text';
  input.value = edge.massLabel || '';
  input.placeholder = `m_{${edge.mass}}`;
  labelEl.textContent = '';
  labelEl.appendChild(input);
  input.focus();
  input.select();
  const finish = () => {
    const val = input.value.trim();
    edge.massLabel = val;
    labelEl.textContent = '';
    renderTeX(getMassDisplayLabel(edge.mass, edge), labelEl);
    render();
    renderMassLegend();
  };
  input.addEventListener('blur', finish);
  input.addEventListener('keydown', (evt) => {
    if (evt.key === 'Enter') input.blur();
    if (evt.key === 'Escape') { input.value = ''; input.blur(); }
    evt.stopPropagation();
  });
  input.addEventListener('click', (evt) => evt.stopPropagation());
}

function closeMassPicker() {
  massPickerEdge = null;
  $('mass-picker').classList.remove('visible');
  render();
}

/**
 * Parse a mass value typed into a config panel input field.
 * Returns { mass: <int>, label: <string> }.
 *   '0' or '' → massless (mass=0, label='')
 *   'm3' or '3' → mass scale 3 with default label
 *   'M_W' or 'm_t' → next available mass scale with custom label
 */
function parseConfigMassInput(raw, edgeIdx) {
  const s = raw.trim();
  if (s === '' || s === '0') return { mass: 0, label: '' };

  // Match 'm<N>' or just '<N>' (integer mass scale) — lowercase m only
  // Uppercase 'M' is treated as a custom label (external mass convention)
  const mNum = s.match(/^m?(\d+)$/);
  if (mNum) return { mass: parseInt(mNum[1]), label: '' };

  // Custom label — find if any edge already uses this label, reuse its mass number
  for (const e of state.edges) {
    if (e.massLabel === s && e.mass > 0) return { mass: e.mass, label: s };
  }

  // Assign next available mass number
  const used = new Set(state.edges.map(e => e.mass).filter(m => m > 0));
  let next = 1;
  while (used.has(next)) next++;
  return { mass: next, label: s };
}

function getExternalEdges() {
  const deg = getVertexDegrees();
  const ext = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a === e.b) continue;
    if ((deg[e.a] || 0) <= 1 || (deg[e.b] || 0) <= 1) ext.push(i);
  }
  return ext;
}

function setEdgeMass(edgeIdx, mass, kind) {
  const extEdges = getExternalEdges();
  const nLegs = extEdges.length;
  const isExt = extEdges.includes(edgeIdx);

  // 1-leg: block mass changes on the single external leg
  if (isExt && nLegs === 1) return;

  pushUndoState();
  state.edges[edgeIdx].mass = mass;
  // Persist the kind choice (m-family vs M-family) on the edge so later
  // popup renders / label lookups honor the user's pick even if the edge
  // topology would default to the other family.
  if (mass === 0) {
    delete state.edges[edgeIdx].massKind;
  } else if (kind === 'm' || kind === 'M') {
    state.edges[edgeIdx].massKind = kind;
  }

  // 2-leg: mirror mass + kind to the paired external leg
  if (isExt && nLegs === 2) {
    const otherIdx = extEdges[0] === edgeIdx ? extEdges[1] : extEdges[0];
    state.edges[otherIdx].mass = mass;
    state.edges[otherIdx].massLabel = state.edges[edgeIdx].massLabel;
    if (mass === 0) {
      delete state.edges[otherIdx].massKind;
    } else if (kind === 'm' || kind === 'M') {
      state.edges[otherIdx].massKind = kind;
    }
  }

  closeMassPicker();
  render();
  renderMassLegend();
  onGraphChanged();
  // Refresh config panel if open
  if ($('config-panel').classList.contains('open')) {
    populateExtMasses();
    populateEdgeTable();
  }
}

function setEdgeStyle(edgeIdx, style) {
  pushUndoState();
  state.edges[edgeIdx].style = style;
  closeMassPicker();
  render();
}

/**
 * Create an SVG element for a styled line segment.
 * Returns a <line>, <path>, or <g> element depending on style.
 * For 'wavy'/'dblwavy'/'zigzag', generates a path along the line.
 */
function createStyledLine(x1, y1, x2, y2, style, color, strokeWidth) {
  const dx = x2 - x1, dy = y2 - y1;
  const len = Math.sqrt(dx * dx + dy * dy);
  if (len < 0.001) return null;

  if (style === 'dashed') {
    const line = document.createElementNS(SVG_NS, 'line');
    line.setAttribute('x1', x1); line.setAttribute('y1', y1);
    line.setAttribute('x2', x2); line.setAttribute('y2', y2);
    line.setAttribute('stroke', color);
    line.setAttribute('stroke-width', strokeWidth);
    line.setAttribute('stroke-linecap', 'round');
    line.setAttribute('stroke-dasharray', `${strokeWidth * 3} ${strokeWidth * 2}`);
    return line;
  }

  if (style === 'wavy' || style === 'dblwavy') {
    // Generate sinusoidal path along the line
    const amp = strokeWidth * 2;
    const freq = len / (strokeWidth * 4); // number of half-waves
    const steps = Math.max(Math.round(freq * 8), 16);
    const nx = -dy / len, ny = dx / len;  // normal

    const makeWavePath = (offset) => {
      let d = `M${x1 + nx * offset},${y1 + ny * offset}`;
      for (let i = 1; i <= steps; i++) {
        const t = i / steps;
        const px = x1 + dx * t + nx * offset;
        const py = y1 + dy * t + ny * offset;
        const wave = Math.sin(t * freq * Math.PI) * amp;
        d += ` L${px + nx * wave},${py + ny * wave}`;
      }
      return d;
    };

    if (style === 'wavy') {
      const path = document.createElementNS(SVG_NS, 'path');
      path.setAttribute('d', makeWavePath(0));
      path.setAttribute('fill', 'none');
      path.setAttribute('stroke', color);
      path.setAttribute('stroke-width', strokeWidth);
      path.setAttribute('stroke-linecap', 'round');
      return path;
    } else {
      const g = document.createElementNS(SVG_NS, 'g');
      const sep = strokeWidth * 1.5;
      [sep / 2, -sep / 2].forEach(off => {
        const path = document.createElementNS(SVG_NS, 'path');
        path.setAttribute('d', makeWavePath(off));
        path.setAttribute('fill', 'none');
        path.setAttribute('stroke', color);
        path.setAttribute('stroke-width', strokeWidth);
        path.setAttribute('stroke-linecap', 'round');
        g.appendChild(path);
      });
      return g;
    }
  }

  if (style === 'gluon' || style === 'zigzag') {
    // Canonical gluon: a helical coil drawn as a chain of cubic Béziers whose
    // control points pull backward past each segment's endpoints, so each
    // segment self-intersects and the run reads as overlapping spring loops.
    // ('zigzag' kept as an alias so pre-existing library entries still render.)
    const coilDiameter = strokeWidth * 3;         // width of each loop along the axis
    const amp = strokeWidth * 2.2;                // loop height (perpendicular)
    const nLoops = Math.max(Math.round(len / coilDiameter), 2);
    const segLen = len / nLoops;
    const pull = segLen * 0.75;                   // backward overshoot on each control point
    const tx = dx / len, ty = dy / len;           // unit tangent
    const nx = -ty,        ny = tx;               // unit normal

    let d = `M${x1},${y1}`;
    for (let i = 0; i < nLoops; i++) {
      const t0 = i / nLoops, t1 = (i + 1) / nLoops;
      const sx = x1 + dx * t0, sy = y1 + dy * t0;
      const ex = x1 + dx * t1, ey = y1 + dy * t1;
      const cp1x = sx - pull * tx + amp * nx;
      const cp1y = sy - pull * ty + amp * ny;
      const cp2x = ex + pull * tx + amp * nx;
      const cp2y = ey + pull * ty + amp * ny;
      d += ` C${cp1x},${cp1y} ${cp2x},${cp2y} ${ex},${ey}`;
    }
    const path = document.createElementNS(SVG_NS, 'path');
    path.setAttribute('d', d);
    path.setAttribute('fill', 'none');
    path.setAttribute('stroke', color);
    path.setAttribute('stroke-width', strokeWidth);
    path.setAttribute('stroke-linecap', 'round');
    return path;
  }

  // Default: solid
  const line = document.createElementNS(SVG_NS, 'line');
  line.setAttribute('x1', x1); line.setAttribute('y1', y1);
  line.setAttribute('x2', x2); line.setAttribute('y2', y2);
  line.setAttribute('stroke', color);
  line.setAttribute('stroke-width', strokeWidth);
  line.setAttribute('stroke-linecap', 'round');
  return line;
}

// ─── Mass legend ────────────────────────────────────────────────────

// Briefly highlight (flash) an edge on the canvas
// Find the SVG element for a given edge index
function findEdgeElement(edgeIdx) {
  return edgeLayer.querySelector(`[data-edge-idx="${edgeIdx}"]`) || null;
}

let _pulsingEdge = null;  // { edgeIdx, animId }

// Start a gentle continuous pulse on an edge (called on input focus)
function flashEdge(edgeIdx) {
  stopEdgePulse();  // stop any existing pulse
  const start = performance.now();
  _pulsingEdge = { edgeIdx, animId: 0 };

  function pulseTick(now) {
    if (!_pulsingEdge || _pulsingEdge.edgeIdx !== edgeIdx) return;
    // Re-find the element each frame (survives render() rebuilds)
    const target = findEdgeElement(edgeIdx);
    if (!target) { _pulsingEdge.animId = requestAnimationFrame(pulseTick); return; }
    const origSW = 0.05;
    const elapsed = now - start;
    // Initial wobble (first 600ms), then settle into gentle breathing
    let sw;
    if (elapsed < 600) {
      const t = elapsed / 600;
      if (t < 0.2) sw = origSW + (origSW * 1.5) * (t / 0.2);
      else if (t < 0.5) sw = origSW * 2.5 - (origSW * 1.8) * ((t - 0.2) / 0.3);
      else sw = origSW * 0.7 + (origSW * 0.7) * ((t - 0.5) / 0.5);
    } else {
      const breath = Math.sin((elapsed - 600) * Math.PI * 2 / 1800) * 0.25 + 1.25;
      sw = origSW * breath;
    }
    target.setAttribute('stroke-width', sw);
    _pulsingEdge.animId = requestAnimationFrame(pulseTick);
  }
  _pulsingEdge.animId = requestAnimationFrame(pulseTick);
}

// Stop the continuous pulse and restore original stroke width
function stopEdgePulse() {
  if (!_pulsingEdge) return;
  const target = findEdgeElement(_pulsingEdge.edgeIdx);
  cancelAnimationFrame(_pulsingEdge.animId);
  if (target) target.setAttribute('stroke-width', 0.05);
  _pulsingEdge = null;
}

function renderMassLegend() {
  const legend = $('mass-legend');
  // Enumerate distinct (kind, mass) slots — every entry in this list gets
  // its own palette color via slotPaletteIndex, so internal m[1] and
  // external M[1] never share the same swatch.
  const slots = [];      // {kind, mass, massLabel}
  const seen = new Set();
  for (const e of state.edges) {
    if (!e.mass || e.mass === 0) continue;
    const kind = getEdgeMassKind(e) || 'm';
    const key = `${kind}:${e.mass}`;
    if (seen.has(key)) continue;
    seen.add(key);
    slots.push({ kind, mass: e.mass, massLabel: e.massLabel || '' });
  }
  if (slots.length === 0) {
    legend.style.display = 'none';
    return;
  }
  legend.style.display = '';
  legend.innerHTML = '';

  // Heading
  const heading = document.createElement('div');
  heading.className = 'legend-heading';
  heading.textContent = 'Masses';
  legend.appendChild(heading);

  // Massless entry
  const ml = document.createElement('div');
  ml.className = 'legend-entry';
  const mlLine = document.createElement('span');
  mlLine.className = 'legend-line';
  mlLine.style.background = 'var(--edge-color)';
  ml.appendChild(mlLine);
  const mlText = document.createElement('span');
  mlText.className = 'legend-text';
  renderTeX('0', mlText);
  ml.appendChild(mlText);
  legend.appendChild(ml);

  // Slot entries, grouped by kind. Uses getDistinctMassSlots for the
  // "bare m vs m_{N}" decision so the legend labels exactly match the
  // canvas labels.
  slots.sort((a, b) => a.kind === b.kind ? a.mass - b.mass : (a.kind === 'm' ? -1 : 1));
  slots.forEach(slot => {
    const col = MASS_PALETTE[slotPaletteIndex(slot.mass, slot.kind)];
    const entry = document.createElement('div');
    entry.className = 'legend-entry';
    const line = document.createElement('span');
    line.className = 'legend-line legend-line-thick';
    line.style.background = col;
    entry.appendChild(line);
    const text = document.createElement('span');
    text.className = 'legend-text';
    const sameKindCount = getDistinctMassSlots(slot.kind).size;
    const displayLabel = slot.massLabel
      ? slot.massLabel
      : (sameKindCount === 1 ? slot.kind : `${slot.kind}_{${slot.mass}}`);
    renderTeX(displayLabel, text);
    entry.appendChild(text);
    legend.appendChild(entry);
  });
}

function shouldShowLabels() {
  return labelMode === 'numbers' || labelMode === 'momenta';
}

// ─── Momentum conservation solver ───────────────────────────────────

// Track last auto-selected chords for initializing userChordSet
let _lastAutoChords = null;

/**
 * Solve momentum conservation for the current diagram.
 * Returns an array of momentum label strings, one per edge,
 * or null if the diagram is incomplete.
 *
 * Algorithm:
 *   1. Classify edges as external legs (one endpoint has degree 1) or internal.
 *   2. Assign external momenta p₁, p₂, ... to legs.
 *   3. BFS to find a spanning tree among internal edges (or use userChordSet).
 *   4. Chords (non-tree internal edges) get loop momenta l₁, l₂, ...
 *   5. Solve momentum conservation at each internal vertex for tree-edge momenta.
 *
 * Momenta are represented as coefficient vectors over {l₁,...,l_L, p₁,...,p_E}.
 */
/**
 * Core momentum-routing solver.  Returns raw coefficient vectors (not
 * formatted strings) so callers can render either LaTeX (solveMomenta) or
 * Mathematica syntax (buildPropsArgJS).  The three-component shape matches
 * the kernel's autoRouteMomenta output order: loop momenta first
 * (l[1]..l[L]), then external momenta (p[1]..p[E]).
 *
 * Returns null when the diagram is incomplete, otherwise:
 *   {
 *     momenta:        Array<null | number[]>,   // per-edge coefficient vectors
 *     nLoops, nExt, basisLen,
 *     isExternal:     boolean[],                // one entry per edge
 *     extLegVertex:   number[],                 // for ext edges: internal vertex
 *     internalEdges:  number[],                 // indices of internal edges
 *     edgeExtLabel:   number[]                  // 0-based ext-leg index per edge
 *   }
 */
function solveMomentaRaw() {
  if (state.vertices.length < 2 || state.edges.length < 1) return null;

  const deg = getVertexDegrees();
  const nEdges = state.edges.length;

  // Classify edges
  const isExternal = [];     // true if this edge is an external leg
  const extLegVertex = [];   // for external edges: which internal vertex it connects to
  const internalEdges = [];  // indices of internal edges
  let extCount = 0;
  const edgeExtLabel = new Array(nEdges).fill(-1); // ext momentum index (0-based)

  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    const dA = deg[e.a] || 0, dB = deg[e.b] || 0;
    if (dA === 1 || dB === 1) {
      isExternal.push(true);
      const intV = dA === 1 ? e.b : e.a;
      extLegVertex.push(intV);
      edgeExtLabel[i] = extCount++;
    } else {
      isExternal.push(false);
      extLegVertex.push(-1);
      internalEdges.push(i);
    }
  }

  const nExt = extCount;

  // BFS spanning tree among internal edges
  // Build adjacency for internal vertices via internal edges
  const intVerts = new Set();
  internalEdges.forEach(ei => {
    intVerts.add(state.edges[ei].a);
    intVerts.add(state.edges[ei].b);
  });
  // Also add internal vertices that only have external legs
  for (let i = 0; i < state.vertices.length; i++) {
    if ((deg[i] || 0) > 1) intVerts.add(i);
  }

  const adj = {};
  intVerts.forEach(v => { adj[v] = []; });
  internalEdges.forEach(ei => {
    const e = state.edges[ei];
    if (adj[e.a]) adj[e.a].push({ edge: ei, to: e.b });
    if (adj[e.b]) adj[e.b].push({ edge: ei, to: e.a });
  });

  let chordEdges, treeEdges, nLoops;
  const visited = new Set();

  if (userChordSet && userChordSet.size > 0) {
    // User-selected chords: use those instead of BFS
    chordEdges = internalEdges.filter(ei => userChordSet.has(ei));
    treeEdges = internalEdges.filter(ei => !userChordSet.has(ei));
    nLoops = chordEdges.length;
    // Still need visited order for tree-edge solving
    const treeEdgeSet = new Set(treeEdges);
    if (intVerts.size > 0) {
      const start = intVerts.values().next().value;
      const queue = [start];
      visited.add(start);
      while (queue.length > 0) {
        const cur = queue.shift();
        for (const { edge, to } of (adj[cur] || [])) {
          if (!visited.has(to) && treeEdgeSet.has(edge)) {
            visited.add(to);
            queue.push(to);
          }
        }
      }
      // Also visit via chord edges to cover disconnected tree components
      for (const { edge, to } of Object.values(adj).flat()) {
        if (!visited.has(to)) {
          visited.add(to);
          const queue2 = [to];
          while (queue2.length > 0) {
            const cur2 = queue2.shift();
            for (const nb of (adj[cur2] || [])) {
              if (!visited.has(nb.to)) {
                visited.add(nb.to);
                queue2.push(nb.to);
              }
            }
          }
        }
      }
    }
  } else {
    // Auto BFS spanning tree
    const treeEdgeSet = new Set();
    if (intVerts.size > 0) {
      const start = intVerts.values().next().value;
      const queue = [start];
      visited.add(start);
      while (queue.length > 0) {
        const cur = queue.shift();
        for (const { edge, to } of (adj[cur] || [])) {
          if (!visited.has(to)) {
            visited.add(to);
            treeEdgeSet.add(edge);
            queue.push(to);
          }
        }
      }
    }
    chordEdges = internalEdges.filter(ei => !treeEdgeSet.has(ei));
    treeEdges = internalEdges.filter(ei => treeEdgeSet.has(ei));
    nLoops = chordEdges.length;
    _lastAutoChords = chordEdges.slice();
  }

  // Basis: [l₁, ..., l_L, p₁, ..., p_E]
  const basisLen = nLoops + nExt;
  const ZERO = () => new Array(basisLen).fill(0);

  // Assign momenta to edges
  // Momentum[i] = coefficient vector; sign convention: momentum flows from e.a → e.b
  const momenta = new Array(nEdges).fill(null);

  // External legs: p_k
  for (let i = 0; i < nEdges; i++) {
    if (isExternal[i]) {
      const vec = ZERO();
      const pIdx = nLoops + edgeExtLabel[i];
      // Convention: momentum flows INTO the internal vertex
      const e = state.edges[i];
      const intV = extLegVertex[i];
      if (e.b === intV) vec[pIdx] = 1;   // flows a→b into intV
      else vec[pIdx] = -1;               // flows a→b away from intV, negate
      momenta[i] = vec;
    }
  }

  // Chords: l_k
  chordEdges.forEach((ei, k) => {
    const vec = ZERO();
    vec[k] = 1;  // l_{k+1} flows from a→b
    momenta[ei] = vec;
  });

  // Tree edges: solve from momentum conservation
  // At each internal vertex v: Σ (momentum out of v) = 0
  // i.e., Σ_{edges incident to v} ±momentum[e] + Σ_{ext legs at v} ±p_k = 0
  // We solve for tree-edge unknowns using back-substitution on the tree.

  // Process tree edges leaf-to-root via reverse BFS order
  // For each tree edge, one side is "deeper" — solve from leaves inward
  const treeEdgeVecs = {};
  treeEdges.forEach(ei => { treeEdgeVecs[ei] = null; });

  // Iterative solve: repeat until all tree edges are determined
  // (For a tree, processing leaves first guarantees convergence in one pass
  //  if we go in reverse BFS order)
  const bfsOrder = [...visited]; // BFS order of internal vertices
  for (let pass = bfsOrder.length - 1; pass >= 0; pass--) {
    const v = bfsOrder[pass];

    // Find the one unknown tree edge at this vertex (if any)
    let unknownEdge = -1;
    let unknownCount = 0;
    const incidentEdges = [];

    for (let i = 0; i < nEdges; i++) {
      const e = state.edges[i];
      if (e.a === v || e.b === v) {
        incidentEdges.push(i);
        if (momenta[i] === null) {
          unknownEdge = i;
          unknownCount++;
        }
      }
    }

    if (unknownCount !== 1) continue; // skip if 0 or >1 unknowns

    // Sum all known momenta flowing out of v
    const sum = ZERO();
    for (const ei of incidentEdges) {
      if (ei === unknownEdge) continue;
      const e = state.edges[ei];
      if (e.a === e.b) continue; // Self-loop: +ℓ and −ℓ cancel, net zero
      const vec = momenta[ei];
      if (!vec) continue;
      // If edge flows a→b and v=a, momentum leaves v: +vec
      // If edge flows a→b and v=b, momentum enters v: -vec
      const sign = (e.a === v) ? 1 : -1;
      for (let j = 0; j < basisLen; j++) sum[j] += sign * vec[j];
    }

    // Unknown edge must cancel the sum: sign * unknown + sum = 0
    // → unknown = -sum / sign
    const ue = state.edges[unknownEdge];
    const sign = (ue.a === v) ? 1 : -1;
    const vec = ZERO();
    for (let j = 0; j < basisLen; j++) vec[j] = -sum[j] / sign;
    momenta[unknownEdge] = vec;
  }

  // ── Momentum conservation constraints for 1-leg and 2-leg diagrams ──
  // 1-leg: p₁ = 0  →  zero out all p₁ coefficients
  if (nExt === 1) {
    const p1Idx = nLoops; // index of p₁ in basis
    for (let i = 0; i < nEdges; i++) {
      if (momenta[i]) momenta[i][p1Idx] = 0;
    }
  }
  // 2-leg: p₂ = −p₁  →  fold p₂ into p₁ (add −p₂ coeff to p₁ coeff, zero p₂)
  if (nExt === 2) {
    const p1Idx = nLoops;
    const p2Idx = nLoops + 1;
    for (let i = 0; i < nEdges; i++) {
      if (momenta[i]) {
        momenta[i][p1Idx] -= momenta[i][p2Idx];
        momenta[i][p2Idx] = 0;
      }
    }
  }

  return {
    momenta, nLoops, nExt, basisLen,
    isExternal, extLegVertex, internalEdges, edgeExtLabel
  };
}

/**
 * Solve momentum conservation and return an array of LaTeX-formatted
 * per-edge momentum labels (one string per edge in state.edges order).
 * Thin wrapper around solveMomentaRaw + formatMomentum.
 */
function solveMomenta() {
  const r = solveMomentaRaw();
  if (!r) return null;
  const { momenta, nLoops, nExt, isExternal } = r;

  // Build custom external momentum names from edge extMomLabel
  const extNames = [];
  for (let i = 0; i < state.edges.length; i++) {
    if (isExternal[i]) {
      const label = state.edges[i].extMomLabel;
      extNames.push(label && label.trim() ? label.trim() : null);
    }
  }

  // Build custom loop momentum names
  const loopNames = [];
  for (let i = 0; i < nLoops; i++) {
    loopNames.push(customLoopLabels[i] || null);
  }

  // Format momentum vectors as strings
  return momenta.map(vec => {
    if (!vec) return '?';
    return formatMomentum(vec, nLoops, extNames, loopNames);
  });
}

/**
 * Format a momentum coefficient vector as a readable string.
 * Basis: [l₁,...,l_L, p₁,...,p_E]
 */
/**
 * Format a momentum coefficient vector as a LaTeX string.
 * Uses \ell_i for loop momenta and p_i for external momenta.
 */
/** Check if a LaTeX momentum name is compound (contains + or - operators) */
function isCompoundMom(name) {
  // Strip LaTeX commands and subscripts, then check for +/- not inside braces
  const stripped = name.replace(/\\[a-zA-Z]+/g, '').replace(/[_^]\{[^}]*\}/g, '').replace(/[{}]/g, '');
  return /[+\-]/.test(stripped);
}

function formatMomentum(vec, nLoops, extNames, loopNames) {
  const nExt = vec.length - nLoops;
  const terms = [];
  for (let i = 0; i < vec.length; i++) {
    const c = vec[i];
    if (Math.abs(c) < 1e-10) continue;
    let name;
    let compound = false; // whether name contains +/- and needs parenthesizing
    if (i < nLoops) {
      const custom = loopNames && loopNames[i];
      if (custom) {
        name = custom;
        compound = isCompoundMom(custom);
      } else {
        // Drop subscript when there's only one loop momentum
        name = nLoops === 1 ? '\\ell' : `\\ell_{${i + 1}}`;
      }
    } else {
      const extIdx = i - nLoops;
      const custom = (extNames && extNames[extIdx]) ? extNames[extIdx] : null;
      if (custom) {
        name = custom;
        compound = isCompoundMom(custom);
      } else {
        // Drop subscript when there's only one independent external momentum
        // (nExt ≤ 2 covers both 1-leg (p=0, not shown) and 2-leg (p₂ folded into p₁))
        name = nExt <= 2 ? 'p' : `p_{${extIdx + 1}}`;
      }
    }

    // Wrap compound names in parentheses when used in multi-term expressions
    const displayName = compound ? `(${name})` : name;

    if (Math.abs(c - 1) < 1e-10) {
      terms.push({ sign: '+', body: displayName });
    } else if (Math.abs(c + 1) < 1e-10) {
      terms.push({ sign: '-', body: displayName });
    } else {
      const ac = Math.abs(c);
      terms.push({ sign: c > 0 ? '+' : '-', body: `${ac}${displayName}` });
    }
  }
  if (terms.length === 0) return '0';

  // If there's only one term with coefficient ±1 and a compound name,
  // unwrap the parentheses (they're not needed for a standalone term)
  if (terms.length === 1) {
    let body = terms[0].body;
    if (body.startsWith('(') && body.endsWith(')')) {
      body = body.slice(1, -1);
    }
    return terms[0].sign === '-' ? `-${body}` : body;
  }

  let result = '';
  terms.forEach((t, idx) => {
    if (idx === 0) {
      result += t.sign === '-' ? `-${t.body}` : t.body;
    } else {
      result += t.sign === '-' ? ` - ${t.body}` : ` + ${t.body}`;
    }
  });
  return result;
}

// Cache for momentum labels (recomputed on graph change)
let _momentumLabels = null;

// User-selected chord edges (indices into state.edges), or null for auto BFS
let userChordSet = null;
// Custom loop momentum labels (e.g. ['\\ell_{1}', 'k'])
let customLoopLabels = [];

function getMomentumLabels() {
  if (_momentumLabels === null) {
    _momentumLabels = solveMomenta();
  }
  return _momentumLabels;
}

/** Reset chord selection (called when topology changes) */
function resetChordSelection() {
  userChordSet = null;
  customLoopLabels = [];
}

/** Toggle an edge as a chord (loop momentum carrier) */
function toggleChord(edgeIdx) {
  const deg = getVertexDegrees();
  const e = state.edges[edgeIdx];
  // Only internal edges can be chords
  if ((deg[e.a] || 0) <= 1 || (deg[e.b] || 0) <= 1) return;
  if (e.a === e.b) return; // tadpole self-loops are always chords automatically

  // Count loops
  const intVerts = new Set();
  const intEdges = [];
  for (let i = 0; i < state.edges.length; i++) {
    const ed = state.edges[i];
    const dA = deg[ed.a] || 0, dB = deg[ed.b] || 0;
    if (dA > 1 && dB > 1) {
      intEdges.push(i);
      intVerts.add(ed.a);
      intVerts.add(ed.b);
    }
  }
  const nLoops = intEdges.length - intVerts.size + 1;
  if (nLoops < 1) return;

  // Initialize userChordSet from current auto if needed
  if (!userChordSet) {
    // Run solveMomenta to get the auto chords, then extract them
    const saved = _momentumLabels;
    _momentumLabels = null;
    getMomentumLabels(); // force recompute
    userChordSet = new Set(_lastAutoChords || []);
    customLoopLabels = [];
    _momentumLabels = null; // will recompute with userChordSet
  }

  if (userChordSet.has(edgeIdx)) {
    // Find the index of this chord in the ordered chord list before removing
    const orderedChords = intEdges.filter(i => userChordSet.has(i));
    const removeIdx = orderedChords.indexOf(edgeIdx);
    userChordSet.delete(edgeIdx);
    // Remove the corresponding custom label
    if (removeIdx >= 0 && removeIdx < customLoopLabels.length) {
      customLoopLabels.splice(removeIdx, 1);
    }
    // Renumber default labels for remaining chords
    customLoopLabels = customLoopLabels.map((l, i) => l || `\\ell_{${i + 1}}`);
  } else {
    if (userChordSet.size >= nLoops) {
      showWarningToast(`Already ${nLoops} loop momenta selected (L=${nLoops}). Deselect one first.`);
      return;
    }
    userChordSet.add(edgeIdx);
    customLoopLabels.push(`\\ell_{${userChordSet.size}}`);
  }

  _momentumLabels = null;
  render();
  // Re-populate edge table if config panel is open
  const container = $('cfg-edge-table-container');
  if (container && container.innerHTML) populateEdgeTable();
  updateIntegralCard();
}

/** Auto-select chords via BFS (reset to automatic) */
function autoSelectChords() {
  userChordSet = null;
  customLoopLabels = [];
  _momentumLabels = null;
  render();
  const container = $('cfg-edge-table-container');
  if (container && container.innerHTML) populateEdgeTable();
  updateIntegralCard();
}

/** Clear all chord selections */
function clearChordSelection() {
  userChordSet = new Set();
  customLoopLabels = [];
  _momentumLabels = null;
  render();
  const container = $('cfg-edge-table-container');
  if (container && container.innerHTML) populateEdgeTable();
  updateIntegralCard();
}

/** Rename a loop momentum label */
function renameLoopMom(chordIndex, newLabel) {
  customLoopLabels[chordIndex] = newLabel;
  _momentumLabels = null;
  render();
}

function getVertexDegrees() {
  const deg = {};
  for (let i = 0; i < state.vertices.length; i++) deg[i] = 0;
  state.edges.forEach(e => { deg[e.a] = (deg[e.a] || 0) + 1; deg[e.b] = (deg[e.b] || 0) + 1; });
  return deg;
}

function renderVertices() {
  vertexLayer.innerHTML = '';
  const deg = getVertexDegrees();

  for (let i = 0; i < state.vertices.length; i++) {
    const v = state.vertices[i];
    const isNew = (i === state.newVertexIdx);
    const isWobble = state.wobbleVerts.indexOf(i) >= 0;
    const isExternal = (deg[i] || 0) <= 1;

    const wrap = document.createElementNS(SVG_NS, 'g');
    wrap.setAttribute('data-vertex', i);
    if (isExternal) wrap.classList.add('vertex-external');
    if (isNew) {
      wrap.style.transformOrigin = v.x + 'px ' + v.y + 'px';
      wrap.style.animation = 'vertexDrop 0.35s cubic-bezier(0.34,1.56,0.64,1) forwards';
    } else if (isWobble) {
      wrap.style.transformOrigin = v.x + 'px ' + v.y + 'px';
      wrap.style.animation = 'vertexWobble 0.4s ease-out';
    }

    const c = document.createElementNS(SVG_NS, 'circle');
    c.setAttribute('cx', v.x); c.setAttribute('cy', v.y);
    c.setAttribute('r', VERTEX_RADIUS);
    c.setAttribute('fill', 'var(--node-fill)');
    c.setAttribute('stroke', 'var(--node-stroke)');
    c.setAttribute('stroke-width', '0.03');
    wrap.appendChild(c);

    vertexLayer.appendChild(wrap);
  }
}

function renderSelection() {
  selectionLayer.innerHTML = '';
  if (state.selectedVertex !== null && state.selectedVertex < state.vertices.length) {
    const v = state.vertices[state.selectedVertex];
    // Soft radial glow
    const defs = document.createElementNS(SVG_NS, 'defs');
    const grad = document.createElementNS(SVG_NS, 'radialGradient');
    grad.id = 'sel-glow';
    const s0 = document.createElementNS(SVG_NS, 'stop');
    s0.setAttribute('offset', '0%'); s0.setAttribute('stop-color', 'var(--accent)'); s0.setAttribute('stop-opacity', '0.2');
    const s1 = document.createElementNS(SVG_NS, 'stop');
    s1.setAttribute('offset', '100%'); s1.setAttribute('stop-color', 'var(--accent)'); s1.setAttribute('stop-opacity', '0');
    grad.appendChild(s0); grad.appendChild(s1);
    defs.appendChild(grad);
    selectionLayer.appendChild(defs);
    const c = document.createElementNS(SVG_NS, 'circle');
    c.setAttribute('cx', v.x); c.setAttribute('cy', v.y);
    c.setAttribute('r', VERTEX_RADIUS + 0.14);
    c.setAttribute('fill', 'url(#sel-glow)');
    c.setAttribute('stroke', 'none');
    selectionLayer.appendChild(c);
  }
}

function renderEdgePreview() {
  previewLayer.innerHTML = '';
  if (state.edgeDragFrom === null || !state.edgeDragPos) return;
  const from = state.vertices[state.edgeDragFrom];
  const to = state.edgeDragPos;
  const line = document.createElementNS(SVG_NS, 'line');
  line.setAttribute('x1', from.x); line.setAttribute('y1', from.y);
  line.setAttribute('x2', to.x); line.setAttribute('y2', to.y);
  line.setAttribute('stroke', 'var(--accent)');
  line.setAttribute('stroke-width', '0.04');
  line.setAttribute('stroke-opacity', '0.5');
  line.setAttribute('stroke-dasharray', '0.06 0.04');
  line.setAttribute('stroke-linecap', 'round');
  previewLayer.appendChild(line);

  const nearest = nearestVertex(to);
  if (nearest >= 0 && nearest !== state.edgeDragFrom) {
    const v = state.vertices[nearest];
    const defs = document.createElementNS(SVG_NS, 'defs');
    const grad = document.createElementNS(SVG_NS, 'radialGradient');
    grad.id = 'target-glow';
    const s0 = document.createElementNS(SVG_NS, 'stop');
    s0.setAttribute('offset', '0%'); s0.setAttribute('stop-color', 'var(--accent)'); s0.setAttribute('stop-opacity', '0.25');
    const s1 = document.createElementNS(SVG_NS, 'stop');
    s1.setAttribute('offset', '100%'); s1.setAttribute('stop-color', 'var(--accent)'); s1.setAttribute('stop-opacity', '0');
    grad.appendChild(s0); grad.appendChild(s1);
    defs.appendChild(grad);
    previewLayer.appendChild(defs);
    const glow = document.createElementNS(SVG_NS, 'circle');
    glow.setAttribute('cx', v.x); glow.setAttribute('cy', v.y);
    glow.setAttribute('r', VERTEX_RADIUS + 0.14);
    glow.setAttribute('fill', 'url(#target-glow)');
    glow.setAttribute('stroke', 'none');
    previewLayer.appendChild(glow);
  }
}

// Edge drawing trail — fading ghost lines
const _edgeTrails = [];
const TRAIL_MAX = 6;

function _spawnTrail(x1, y1, x2, y2) {
  const g = document.createElementNS(SVG_NS, 'line');
  g.setAttribute('x1', x1); g.setAttribute('y1', y1);
  g.setAttribute('x2', x2); g.setAttribute('y2', y2);
  g.setAttribute('stroke', 'var(--accent)');
  g.setAttribute('stroke-width', '0.03');
  g.setAttribute('stroke-linecap', 'round');
  g.setAttribute('stroke-opacity', '0.12');
  previewLayer.insertBefore(g, previewLayer.firstChild);
  _edgeTrails.push(g);
  // Fade: reduce opacity over time via direct manipulation
  requestAnimationFrame(() => {
    g.style.transition = 'opacity 0.4s ease-out';
    g.style.opacity = '0';
  });
  // Prune old trails
  while (_edgeTrails.length > TRAIL_MAX) {
    const old = _edgeTrails.shift();
    if (old.parentNode) old.parentNode.removeChild(old);
  }
}

function clearEdgePreview() {
  // Remove preview elements but keep fading trails
  const children = Array.from(previewLayer.children);
  children.forEach(c => {
    if (!_edgeTrails.includes(c)) previewLayer.removeChild(c);
  });
}

function clearEdgeTrails() {
  _edgeTrails.forEach(g => { if (g.parentNode) g.parentNode.removeChild(g); });
  _edgeTrails.length = 0;
  previewLayer.innerHTML = '';
}

// ─── Pointer events (mouse, pen, touch) + multi-touch pinch/pan ─────

const _activePointers = new Map();   // pointerId → { x, y } in client coords
let _gesture = null;                 // pinch/pan: { startDist, startSvgCentroid, startZoom }

// Revert any in-progress single-pointer action when a 2nd pointer arrives.
function cancelSinglePointerGesture() {
  if (state._undoPushedOnDown || state.dragging) {
    try { undoAction(); redoStack.pop(); } catch (e) {}
  }
  _panning = null;
  state.edgeDragFrom = null; state.edgeDragPos = null;
  state.selectedVertex = null; state.dragging = null;
  state._undoPushedOnDown = false;
  state._deleteDownPos = null;
  state._edgeDragMaxDist = 0;
  canvas.style.cursor = '';
  clearEdgePreview();
  clearEdgeTrails();
  render();
}

function beginPinchGesture() {
  const pts = [..._activePointers.values()];
  const a = pts[0], b = pts[1];
  const cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
  _gesture = {
    startDist: Math.hypot(a.x - b.x, a.y - b.y) || 1,
    startSvgCentroid: svgPoint({ clientX: cx, clientY: cy }),
    startZoom: zoomLevel,
  };
}

function updatePinchGesture() {
  if (!_gesture || _activePointers.size !== 2) return;
  const pts = [..._activePointers.values()];
  const a = pts[0], b = pts[1];
  const curDist = Math.hypot(a.x - b.x, a.y - b.y) || 1;
  const curCx = (a.x + b.x) / 2, curCy = (a.y + b.y) / 2;
  const newZoom = Math.max(0.15, Math.min(6.0, _gesture.startZoom * (curDist / _gesture.startDist)));
  const rect = canvas.getBoundingClientRect();
  if (rect.width === 0 || rect.height === 0) return;
  const newW = BASE_VIEWBOX.w / newZoom;
  const newH = BASE_VIEWBOX.h / newZoom;
  // Anchor: the SVG point under the gesture-start centroid stays under the
  // current screen centroid (so users perceive zoom around their fingers).
  const cxSvg = _gesture.startSvgCentroid.x - (curCx - rect.left - rect.width  / 2) * newW / rect.width;
  const cySvg = _gesture.startSvgCentroid.y - (curCy - rect.top  - rect.height / 2) * newH / rect.height;
  zoomLevel = newZoom;
  panOffset.x = cxSvg - BASE_VIEWBOX.x - BASE_VIEWBOX.w / 2;
  panOffset.y = cySvg - BASE_VIEWBOX.y - BASE_VIEWBOX.h / 2;
  applyZoom();
}

canvas.addEventListener('pointerdown', function(evt) {
  if (evt.button !== 0) return;
  // Belt-and-suspenders guard: if the pointer originates inside an open
  // overlay sheet (config panel, library browser, detail popup, notif sheet,
  // or expanded integral card), don't let it start a canvas gesture. Those
  // surfaces stack above the canvas at higher z-indices, but if the native
  // scroll-forward behaviour leaks through we'd begin a phantom pan/zoom.
  if (evt.target && evt.target.closest &&
      evt.target.closest('#config-panel.open, .browser-overlay.visible, #detail-panel.open, #notif-sheet.open, .integral-card.mobile-expanded, .detail-backdrop.open, .notif-sheet-backdrop.open')) {
    return;
  }
  _activePointers.set(evt.pointerId, { x: evt.clientX, y: evt.clientY });

  // 2+ pointers → multi-touch pinch/pan; revert any in-progress single-pointer action
  if (_activePointers.size >= 2) {
    cancelSinglePointerGesture();
    if (_activePointers.size === 2) beginPinchGesture();
    return;
  }

  evt.preventDefault();
  hideHint();
  try { canvas.setPointerCapture(evt.pointerId); } catch (e) {}

  // Ctrl/Cmd+drag → pan (mouse/pen; touch users use two fingers instead)
  if (evt.ctrlKey || evt.metaKey) {
    _panning = { startX: evt.clientX, startY: evt.clientY, origPanX: panOffset.x, origPanY: panOffset.y };
    canvas.style.cursor = 'grabbing';
    return;
  }

  const p = svgPoint(evt);
  const vi = nearestVertex(p);

  if (state.mode === 'draw') {
    if (vi >= 0 && evt.shiftKey) {
      pushUndoState();
      state.dragging = { index: vi };
    } else if (vi >= 0) {
      state.edgeDragFrom = vi;
      state.edgeDragPos = state.vertices[vi];
      state._edgeDragMaxDist = 0;
      state.selectedVertex = vi;
      render();
    } else {
      const eHit = nearestEdgeSegment(p);
      if (eHit.index >= 0) {
        pushUndoState(); state._undoPushedOnDown = true;
        const old = state.edges[eHit.index];
        const ea = old.a, eb = old.b, om = old.mass || 0;
        // Place vertex at the actual click point (not the straight-line projection),
        // so subdividing curved multi-edges doesn't unbend them.
        state.vertices.push({ x: snap(p.x), y: snap(p.y) });
        const si = state.vertices.length - 1;
        state.edges.splice(eHit.index, 1);
        state.edges.push({ a: Math.min(ea,si), b: Math.max(ea,si), mass: om });
        state.edges.push({ a: Math.min(eb,si), b: Math.max(eb,si), mass: om });
        state.newVertexIdx = si;
        state.splitEdgeIndices = [state.edges.length-2, state.edges.length-1];
        state.wobbleVerts = [ea, eb];
        state.edgeDragFrom = si;
        state.edgeDragPos = state.vertices[si];
        state.selectedVertex = si;
        render(); onGraphChanged();
        setTimeout(() => { state.newVertexIdx=-1; state.splitEdgeIndices=[]; state.wobbleVerts=[]; render(); }, 500);
        return;
      }
      pushUndoState(); state._undoPushedOnDown = true;
      state.vertices.push({ x: snap(p.x), y: snap(p.y) });
      const ni = state.vertices.length - 1;
      state.newVertexIdx = ni;
      state.edgeDragFrom = ni;
      state.edgeDragPos = state.vertices[ni];
      state.selectedVertex = ni;
      render(); onGraphChanged();
      setTimeout(() => { state.newVertexIdx = -1; }, 400);
    }
  } else if (state.mode === 'delete') {
    state._deleteDownPos = { x: p.x, y: p.y };
    if (vi >= 0) {
      if (wouldVertexRemovalDisconnect(vi)) { showConnectedWarning(); return; }
      pushUndoState();
      state.edges = state.edges.filter(e => e.a !== vi && e.b !== vi);
      state.vertices.splice(vi, 1);
      state.edges = state.edges.map(e => ({
        a: e.a > vi ? e.a-1 : e.a,
        b: e.b > vi ? e.b-1 : e.b,
        mass: e.mass || 0,
        style: e.style || 'solid',
        massLabel: e.massLabel || '',
        edgeLabel: e.edgeLabel || '', extMomLabel: e.extMomLabel || '',
      }));
    } else {
      const ei = nearestEdge(p);
      if (ei >= 0) {
        const edge = state.edges[ei];
        // Find vertices that will become isolated after edge removal
        const degA = state.edges.filter(e => e.a === edge.a || e.b === edge.a).length;
        const degB = state.edges.filter(e => e.a === edge.b || e.b === edge.b).length;
        const orphans = [];
        if (degA === 1) orphans.push(edge.a);
        if (degB === 1) orphans.push(edge.b);

        // Check connectivity excluding orphan removal
        // (removing a leaf edge + its leaf vertex never disconnects)
        if (orphans.length === 0 && wouldEdgeRemovalDisconnect(ei)) {
          showConnectedWarning(); return;
        }

        pushUndoState();
        state.edges.splice(ei, 1);

        // Remove orphaned vertices (highest index first to avoid shifting issues)
        orphans.sort((a, b) => b - a).forEach(oi => {
          state.vertices.splice(oi, 1);
          state.edges = state.edges.map(e => ({
            a: e.a > oi ? e.a - 1 : e.a,
            b: e.b > oi ? e.b - 1 : e.b,
            mass: e.mass || 0, style: e.style || 'solid', massLabel: e.massLabel || '', edgeLabel: e.edgeLabel || '', extMomLabel: e.extMomLabel || '',
          }));
        });
      }
    }
    render(); onGraphChanged();
  }
});

canvas.addEventListener('pointermove', function(evt) {
  // Track the moving pointer so multi-touch math stays in sync
  const tracked = _activePointers.get(evt.pointerId);
  if (tracked) { tracked.x = evt.clientX; tracked.y = evt.clientY; }

  // Multi-touch gesture takes priority over single-pointer logic
  if (_gesture) { updatePinchGesture(); return; }

  // Panning
  if (_panning) {
    const dx = (evt.clientX - _panning.startX);
    const dy = (evt.clientY - _panning.startY);
    // Convert pixel delta to SVG units using zoom-derived dimensions (not DOM readback)
    const vbW = BASE_VIEWBOX.w / zoomLevel;
    const vbH = BASE_VIEWBOX.h / zoomLevel;
    const rect = canvas.getBoundingClientRect();
    panOffset.x = _panning.origPanX - dx * (vbW / rect.width);
    panOffset.y = _panning.origPanY - dy * (vbH / rect.height);
    applyZoom();
    return;
  }

  const p = svgPoint(evt);
  if (state.mode === 'draw' && state.dragging !== null) {
    state.vertices[state.dragging.index] = { x: snap(p.x), y: snap(p.y) };
    render();
  }
  if (state.mode === 'draw' && state.edgeDragFrom !== null) {
    const prevPos = state.edgeDragPos;
    state.edgeDragPos = p;
    // Track max distance from origin vertex for tadpole detection
    const from = state.vertices[state.edgeDragFrom];
    const d = dist(p, from);
    if (d > (state._edgeDragMaxDist || 0)) state._edgeDragMaxDist = d;
    // Spawn ghost trail from previous position
    if (prevPos && d > 0.05) _spawnTrail(from.x, from.y, prevPos.x, prevPos.y);
    renderEdgePreview();
  }
});

canvas.addEventListener('pointerup', function(evt) {
  _activePointers.delete(evt.pointerId);
  try { canvas.releasePointerCapture(evt.pointerId); } catch (e) {}

  // If we were in a multi-touch gesture, swallow this event; finger lifts
  // shouldn't trigger draw-mode edge release. Reset gesture once all gone.
  if (_gesture) {
    if (_activePointers.size === 0) _gesture = null;
    return;
  }

  if (_panning) {
    _panning = null;
    canvas.style.cursor = '';
    return;
  }

  const p = svgPoint(evt);
  if (state.mode === 'draw' && state.edgeDragFrom !== null) {
    const vi = nearestVertex(p);
    const from = state.vertices[state.edgeDragFrom];
    const moved = dist(p, from) >= HIT_RADIUS;
    // For tadpole: check if the drag EVER went far enough from the vertex,
    // not just the final position (which may return to the vertex center)
    const draggedAway = (state._edgeDragMaxDist || 0) >= HIT_RADIUS;

    if (vi >= 0 && vi === state.edgeDragFrom && draggedAway) {
      // Drag-and-return to same vertex → create tadpole (self-loop)
      if (!state._undoPushedOnDown) pushUndoState();
      state.edges.push({ a: vi, b: vi });
      state.newEdgeIdx = state.edges.length - 1;
      state.wobbleVerts = [vi];
      onGraphChanged();
      setTimeout(() => { state.newEdgeIdx=-1; state.wobbleVerts=[]; render(); }, 450);
    } else if (vi >= 0 && vi !== state.edgeDragFrom) {
      if (!state._undoPushedOnDown) pushUndoState();
      const a = Math.min(state.edgeDragFrom, vi), b = Math.max(state.edgeDragFrom, vi);
      state.edges.push({ a, b });
      state.newEdgeIdx = state.edges.length - 1;
      state.wobbleVerts = [a, b];
      onGraphChanged();
      setTimeout(() => { state.newEdgeIdx=-1; state.wobbleVerts=[]; render(); }, 450);
    } else if (moved && vi !== state.edgeDragFrom) {
      // Check if releasing on an existing edge — subdivide it
      const edgeHit = nearestEdgeSegment(p);
      if (edgeHit.index >= 0) {
        if (!state._undoPushedOnDown) pushUndoState();
        const splitEdge = state.edges[edgeHit.index];
        const splitA = splitEdge.a, splitB = splitEdge.b;
        const splitMass = splitEdge.mass || 0;
        const splitStyle = splitEdge.style || 'solid';
        const splitMassLabel = splitEdge.massLabel || '';
        const splitEdgeLabel = splitEdge.edgeLabel || '';
        // Insert new vertex at the nearest point on the edge
        const sp = edgeHit.point;
        state.vertices.push({ x: sp.x, y: sp.y });
        const ni = state.vertices.length - 1;
        // Remove the old edge and replace with two halves
        state.edges.splice(edgeHit.index, 1);
        state.edges.push({ a: Math.min(splitA, ni), b: Math.max(splitA, ni), mass: splitMass, style: splitStyle, massLabel: splitMassLabel, edgeLabel: splitEdgeLabel });
        state.edges.push({ a: Math.min(splitB, ni), b: Math.max(splitB, ni), mass: splitMass, style: splitStyle, massLabel: splitMassLabel, edgeLabel: splitEdgeLabel });
        // Connect the dragged edge to the new vertex
        const ea = Math.min(state.edgeDragFrom, ni), eb = Math.max(state.edgeDragFrom, ni);
        state.edges.push({ a: ea, b: eb });
        state.newVertexIdx = ni;
        state.newEdgeIdx = state.edges.length - 1;
        state.splitEdgeIndices = [state.edges.length - 3, state.edges.length - 2];
        state.wobbleVerts = [state.edgeDragFrom, ni];
        onGraphChanged();
        setTimeout(() => { state.newVertexIdx=-1; state.newEdgeIdx=-1; state.splitEdgeIndices=[]; state.wobbleVerts=[]; render(); }, 450);
      } else {
        if (!state._undoPushedOnDown) pushUndoState();
        state.vertices.push({ x: snap(p.x), y: snap(p.y) });
        const ni = state.vertices.length - 1;
        // Orient: new vertex (external) → drag origin (internal), so arrow points inward
        state.edges.push({ a: ni, b: state.edgeDragFrom });
        state.newVertexIdx = ni; state.newEdgeIdx = state.edges.length-1;
        state.wobbleVerts = [state.edgeDragFrom, ni];
        onGraphChanged();
        setTimeout(() => { state.newVertexIdx=-1; state.newEdgeIdx=-1; state.wobbleVerts=[]; render(); }, 450);
      }
    }
    // If the graph became disconnected, revert and warn.
    if (state._undoPushedOnDown && !isGraphConnected(state.vertices.length, state.edges)) {
      undoAction();
      redoStack.pop();
      showConnectedWarning();
    }
    state.edgeDragFrom = null; state.edgeDragPos = null;
    state.selectedVertex = null; state._undoPushedOnDown = false;
    clearEdgePreview();
    // Let trails fade, then remove
    setTimeout(clearEdgeTrails, 500);
    render();
  }
  if (state.dragging) {
    state.dragging = null;
    onGraphChanged();
  }
  // Detect drag attempt in delete mode → suggest switching to draw mode
  if (state.mode === 'delete' && state._deleteDownPos) {
    const d = dist(p, state._deleteDownPos);
    if (d >= HIT_RADIUS) {
      showWarningToast('Switch to Draw mode to create edges [1]');
    }
    state._deleteDownPos = null;
  }
});

canvas.addEventListener('pointercancel', function(evt) {
  _activePointers.delete(evt.pointerId);
  try { canvas.releasePointerCapture(evt.pointerId); } catch (e) {}
  if (_activePointers.size === 0) _gesture = null;
  if (state.edgeDragFrom !== null || state.dragging !== null || _panning) {
    cancelSinglePointerGesture();
  }
});

canvas.addEventListener('pointerleave', function() {
  // Fires only when pointer leaves without capture (hover/no-button mouse).
  // Captured pointers (active drags) won't fire this until release.
  if (state.edgeDragFrom !== null) {
    state.edgeDragFrom = null; state.edgeDragPos = null;
    state.selectedVertex = null; clearEdgePreview(); render();
  }
  state.dragging = null;
});

// Ctrl/Cmd+scroll to zoom — must capture to prevent browser native zoom
document.addEventListener('wheel', function(evt) {
  if (!evt.ctrlKey && !evt.metaKey) return;
  // Only zoom when mouse is over the canvas
  if (!canvas.contains(evt.target) && evt.target !== canvas) return;
  evt.preventDefault();
  evt.stopPropagation();
  zoomCanvas(evt.deltaY < 0 ? 1 : -1);
}, { passive: false, capture: true });

// ─── Hint ────────────────────────────────────────────────────────────

function hideHint() {
  if (state.vertices.length === 0) return; // keep hint visible until first vertex
  const h = $('draw-hint');
  if (h && !h.classList.contains('hidden')) {
    h.classList.add('hidden');
  }
}

// ─── Connectivity checks ────────────────────────────────────────────

/** Check if the graph given by vertex count + edge list is connected (ignoring isolated vertices). */
function isGraphConnected(numVerts, edges) {
  if (numVerts === 0) return true;
  if (edges.length === 0) return numVerts <= 1;
  // BFS from the first vertex that appears in any edge
  const adj = {};
  for (let i = 0; i < numVerts; i++) adj[i] = [];
  edges.forEach(e => { adj[e.a].push(e.b); adj[e.b].push(e.a); });
  // Find all vertices with at least one edge
  const hasEdge = new Set();
  edges.forEach(e => { hasEdge.add(e.a); hasEdge.add(e.b); });
  if (hasEdge.size === 0) return numVerts <= 1;
  const start = hasEdge.values().next().value;
  const visited = new Set([start]);
  const queue = [start];
  while (queue.length) {
    const cur = queue.shift();
    for (const nb of adj[cur]) {
      if (!visited.has(nb)) { visited.add(nb); queue.push(nb); }
    }
  }
  // All vertices that participate in edges must be reachable
  for (const v of hasEdge) {
    if (!visited.has(v)) return false;
  }
  // Also check for isolated vertices (those with no edges)
  for (let i = 0; i < numVerts; i++) {
    if (!hasEdge.has(i) && hasEdge.size > 0) return false;
  }
  return true;
}

/** Would removing edge at index `ei` disconnect the graph? */
function wouldEdgeRemovalDisconnect(ei) {
  const remaining = state.edges.filter((_, i) => i !== ei);
  return !isGraphConnected(state.vertices.length, remaining);
}

/** Would removing vertex `vi` (and its edges) disconnect the graph? */
function wouldVertexRemovalDisconnect(vi) {
  const remaining = state.edges
    .filter(e => e.a !== vi && e.b !== vi)
    .map(e => ({
      a: e.a > vi ? e.a - 1 : e.a,
      b: e.b > vi ? e.b - 1 : e.b,
      mass: e.mass || 0,
    }));
  return !isGraphConnected(state.vertices.length - 1, remaining);
}

function showConnectedWarning() {
  showWarningToast('Cannot delete \u2014 would disconnect diagram', true);
}

// ─── Graph → Nickel edge list ────────────────────────────────────────

function buildEdgeData() {
  if (state.vertices.length < 2 || state.edges.length < 1) return null;
  const degree = {};
  state.edges.forEach(e => {
    degree[e.a] = (degree[e.a]||0)+1;
    degree[e.b] = (degree[e.b]||0)+1;
  });
  const vertexToNode = {};
  let next = 0;
  for (let i = 0; i < state.vertices.length; i++) {
    vertexToNode[i] = (degree[i] || 0) <= 1 ? LEG : next++;
  }
  const edges = [];
  const masses = [];
  for (const e of state.edges) {
    edges.push([vertexToNode[e.a], vertexToNode[e.b]]);
    masses.push(e.mass || 0);
  }
  return { edges, masses };
}

/**
 * Build edge data with multiple external legs at the same internal vertex
 * collapsed into a single massive leg. Used for library matching so that
 * e.g. a triangle with 2 legs at one vertex matches the standard 3-point triangle.
 *
 * Rule: if N > 1 external legs attach to the same internal vertex,
 * they are replaced by a single LEG edge. Its mass is set to the max
 * of the individual leg masses, or 1 if all are massless (since the
 * combined momentum (p₁+p₂+...)² is generically non-zero).
 */
function buildCollapsedEdgeData() {
  if (state.vertices.length < 2 || state.edges.length < 1) return null;
  const degree = {};
  state.edges.forEach(e => {
    degree[e.a] = (degree[e.a]||0)+1;
    degree[e.b] = (degree[e.b]||0)+1;
  });

  // Map vertices to internal nodes or LEG
  const vertexToNode = {};
  let next = 0;
  for (let i = 0; i < state.vertices.length; i++) {
    vertexToNode[i] = (degree[i] || 0) <= 1 ? LEG : next++;
  }

  // Group external legs by their internal vertex
  const legsByIntNode = {};  // internalNode → [{edgeIdx, mass}]
  const internalEdges = [];  // indices of non-external edges

  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const na = vertexToNode[e.a], nb = vertexToNode[e.b];
    if (na === LEG && nb !== LEG) {
      if (!legsByIntNode[nb]) legsByIntNode[nb] = [];
      legsByIntNode[nb].push({ idx: i, mass: e.mass || 0 });
    } else if (nb === LEG && na !== LEG) {
      if (!legsByIntNode[na]) legsByIntNode[na] = [];
      legsByIntNode[na].push({ idx: i, mass: e.mass || 0 });
    } else {
      internalEdges.push(i);
    }
  }

  const edges = [];
  const masses = [];

  // Add internal edges as-is
  for (const i of internalEdges) {
    const e = state.edges[i];
    edges.push([vertexToNode[e.a], vertexToNode[e.b]]);
    masses.push(e.mass || 0);
  }

  // Add one collapsed leg per internal node that has external legs
  for (const nodeStr in legsByIntNode) {
    const node = parseInt(nodeStr);
    const legs = legsByIntNode[node];
    const maxMass = Math.max(...legs.map(l => l.mass));
    // If multiple legs and all massless, the combined momentum is massive
    const collapsedMass = legs.length > 1 && maxMass === 0 ? 1 : maxMass;
    edges.push([node, LEG]);
    masses.push(collapsedMass);
  }

  return { edges, masses };
}

// ─── Mass-aware configuration matching ──────────────────────────────

/**
 * Build canvas mass array aligned with Nickel traversal order.
 * Returns one mass value per edge, in the same order as characters
 * in the Nickel string (skipping '|' separators).
 */
function getCanvasMassArray(nickelList, nodeMap, edges, masses) {
  // Invert nodeMap: canonical label → original internal node
  const invMap = new Map();
  for (const [orig, canon] of nodeMap) {
    invMap.set(canon, orig);
  }

  // Build edge pool keyed by sorted (origA, origB).
  // For multi-edges, masses are sorted so consumption order is canonical.
  const pool = {};
  for (let k = 0; k < edges.length; k++) {
    const a = edges[k][0], b = edges[k][1];
    const key = Math.min(a, b) + ',' + Math.max(a, b);
    if (!pool[key]) pool[key] = [];
    pool[key].push(masses[k]);
  }
  for (const key in pool) pool[key].sort((a, b) => a - b);

  // Track consumption index per pool key
  const consumed = {};

  // Traverse Nickel list in canonical order
  const result = [];
  for (let i = 0; i < nickelList.length; i++) {
    for (const j of nickelList[i]) {
      const origI = invMap.get(i);
      const origJ = j === LEG ? LEG : invMap.get(j);
      const a = origI, b = (origJ === undefined ? LEG : origJ);
      const key = Math.min(a, b) + ',' + Math.max(a, b);
      if (!consumed[key]) consumed[key] = 0;
      const idx = consumed[key]++;
      result.push(pool[key] ? (pool[key][idx] ?? 0) : 0);
    }
  }
  return result;
}

/**
 * Parse a library config key (e.g. "n00|n|") into an array of label characters,
 * one per edge in Nickel traversal order (skipping '|' separators).
 */
function parseConfigColoring(configKey) {
  const labels = [];
  for (const ch of configKey) {
    if (ch !== '|') labels.push(ch);
  }
  return labels;
}

/**
 * Translate library config labels from the library's (Python) canonical bare
 * order to the JS canonical bare order used by the canvas matcher.
 *
 * The library stores entries keyed by Python-canonical bare + color labels,
 * which are aligned 1:1 with that bare's character order. The canvas
 * canonicalizes via ui/nickel.js (GraphState convention) which picks a
 * different representative for the same equivalence class — so labels
 * read straight from the library config won't line up positionally with
 * canvasMasses. This helper re-canonicalizes the library's bare through
 * JS and reorders the labels into JS-canonical traversal order.
 *
 * Returns an array of label characters in JS canonical order.
 */
function libraryConfigLabelsInJSOrder(libBare, configKey) {
  const rawLabels = parseConfigColoring(configKey);
  // Parse the library bare and walk it in the same position order as rawLabels
  const n = Nickel.fromString(libBare);
  const libNickelList = n.nickel;
  // Build edge list and per-edge labels in library's Python-canonical order.
  // Order matches the iteration in edgesFromNickel in nickel.js.
  const edgeList = [];
  const labelPerEdge = [];
  let idx = 0;
  for (let i = 0; i < libNickelList.length; i++) {
    for (const j of libNickelList[i]) {
      const lo = Math.min(i, j), hi = Math.max(i, j);
      edgeList.push([lo, hi]);
      labelPerEdge.push(rawLabels[idx] ?? '0');
      idx++;
    }
  }
  // Canonicalize via JS and remap labels into JS canonical order.
  const c = canonicalize(edgeList);
  const jsNickel = c.nickel;
  const nodeMap = c.nodeMaps[0];
  const invMap = new Map();
  for (const [orig, canon] of nodeMap) invMap.set(canon, orig);

  // Build a pool of labels keyed by sorted (origA, origB) vertex pair.
  // Within a multi-edge group we must pick a canonical ordering that matches
  // what the canvas matcher will produce. We sort by (label, then insertion
  // index) as a string-safe tiebreak — this agrees with JS numeric sort on
  // digit labels while also being well-defined for wildcards ('n','z','s').
  const pool = {};
  for (let k = 0; k < edgeList.length; k++) {
    const [a, b] = edgeList[k];
    const key = a + ',' + b;
    if (!pool[key]) pool[key] = [];
    pool[key].push(labelPerEdge[k]);
  }
  for (const k in pool) pool[k].sort();  // default string sort

  const consumed = {};
  const out = [];
  for (let i2 = 0; i2 < jsNickel.length; i2++) {
    for (const j2 of jsNickel[i2]) {
      const origI = invMap.get(i2);
      const origJ = j2 === LEG ? LEG : invMap.get(j2);
      const lo = Math.min(origI, origJ), hi = Math.max(origI, origJ);
      const key = lo + ',' + hi;
      if (!consumed[key]) consumed[key] = 0;
      const idx2 = consumed[key]++;
      out.push(pool[key] ? (pool[key][idx2] ?? '0') : '0');
    }
  }
  return out;
}

/**
 * Classify a library config's compatibility with canvas masses.
 *
 * Returns:
 *   'exact'      — partition structure matches: library 0↔canvas 0,
 *                   definite labels map to distinct non-zero canvas masses,
 *                   wildcards (n/z/s) correspond to non-zero canvas masses.
 *   'compatible' — library can be specialized to canvas (setting masses
 *                   equal or to zero), but it's not an exact match.
 *   'none'       — incompatible (library has 0 where canvas has mass).
 */
function classifyConfigMatch(configLabels, canvasMasses) {
  if (configLabels.length !== canvasMasses.length) return 'none';

  const mapping = {};  // definite label → canvas mass value

  for (let i = 0; i < configLabels.length; i++) {
    const label = configLabels[i];
    const mass = canvasMasses[i];

    if (label === '0') {
      // Library says massless — canvas must also be massless
      if (mass !== 0) return 'none';
    } else if (label === 'n' || label === 'z' || label === 's') {
      // Wildcard — any canvas mass is fine (independent)
    } else {
      // Definite label (1, 2, ..., a, b, ...) — must be consistent
      if (label in mapping) {
        if (mapping[label] !== mass) return 'none';
      } else {
        mapping[label] = mass;
      }
    }
  }

  // Compatible. Now check if it's exact:
  // 1) All definite labels map to distinct non-zero masses
  const vals = Object.values(mapping);
  const allNonZero = vals.every(v => v !== 0);
  const allDistinct = new Set(vals).size === vals.length;

  // 2) Wildcards correspond to non-zero canvas masses
  let wildcardsNonZero = true;
  for (let i = 0; i < configLabels.length; i++) {
    const label = configLabels[i];
    if ((label === 'n' || label === 'z' || label === 's') && canvasMasses[i] === 0) {
      wildcardsNonZero = false;
      break;
    }
  }

  if (allNonZero && allDistinct && wildcardsNonZero) return 'exact';
  return 'compatible';
}

// ─── SVG thumbnail generator ────────────────────────────────────────

/**
 * Generate a small SVG diagram for a topology + mass configuration.
 * Returns an SVG element suitable for use as a thumbnail.
 */
function generateThumbnail(topoNickel, configKey, options) {
  const showLabels = options && options.labels;
  const legOrder = (options && options.legOrder) || null;  // canonical leg → original leg mapping
  // Direct mass override — used by the integration modal to render a
  // canvas drawing whose masses don't round-trip through a library
  // configKey string. When supplied, it's an array aligned with the
  // Nickel traversal order (one entry per edge, 0 = massless).
  const edgeMassesOverride = options && options.edgeMasses;
  // Use nickelFull (with legs) if available in library, else fall back to vacuum Nickel
  const topo = library && library.topologies ? library.topologies[topoNickel] : null;
  const nickelStr = (topo && topo.nickelFull) ? topo.nickelFull : topoNickel;
  const n = Nickel.fromString(nickelStr);
  const edgeList = n.edges;
  const nickelList = n.nickel;

  // For vacuum topologies without nickelFull, add synthetic legs from legVertices
  if (topo && !topo.nickelFull && topo.legVertices && topo.legVertices.length > 0) {
    for (const v of topo.legVertices) {
      edgeList.push([LEG, v]);
    }
  }

  // Assign mass colors from config coloring
  const configLabels = configKey ? parseConfigColoring(configKey) : [];
  // Map config labels → visual mass indices.
  // Labels are aligned 1:1 with the FULL bare Nickel traversal, including
  // LEG positions ('e' in the bare ↔ leg mass digit in the color).
  const labelToMass = {};
  let nextMass = 1;
  let edgeMasses;
  if (edgeMassesOverride) {
    edgeMasses = edgeMassesOverride;
  } else {
    edgeMasses = [];
    let labelIdx = 0;
    for (let i = 0; i < nickelList.length; i++) {
      for (const j of nickelList[i]) {
        const label = configLabels[labelIdx++] || '0';
        let mass = 0;
        if (label === '0') {
          mass = 0;  // massless
        } else {
          // Same label = same mass scale (unified digit scheme)
          if (!(label in labelToMass)) labelToMass[label] = nextMass++;
          mass = labelToMass[label];
        }
        edgeMasses.push(mass);
      }
    }
  }

  // Build vertex/edge arrays and run force layout (same as loadFromNickel)
  const internalNodes = new Set();
  for (const [a, b] of edgeList) {
    if (a >= 0) internalNodes.add(a);
    if (b >= 0) internalNodes.add(b);
  }
  const sortedInternal = [...internalNodes].sort((a, b) => a - b);
  const numInt = sortedInternal.length;
  const R = 28;

  // Initial circular layout
  const initVerts = [];
  for (let i = 0; i <= Math.max(...sortedInternal, 0); i++) {
    const ang = (2 * Math.PI * i) / numInt - Math.PI / 2;
    // Add jitter proportional to R to break symmetry
    initVerts.push({
      x: R * Math.cos(ang) + (Math.random() - 0.5) * R * 0.3,
      y: R * Math.sin(ang) + (Math.random() - 0.5) * R * 0.3
    });
  }

  // Add external leg vertices
  const layoutEdges = [];
  let nv = initVerts.length;
  edgeList.forEach(([a, b]) => {
    let la = a, lb = b;
    if (la < 0) { la = nv; initVerts.push({ x: initVerts[lb].x * 1.5, y: initVerts[lb].y * 1.5 }); nv++; }
    if (lb < 0) { lb = nv; initVerts.push({ x: initVerts[la].x * 1.5, y: initVerts[la].y * 1.5 }); nv++; }
    layoutEdges.push({ a: Math.min(la, lb), b: Math.max(la, lb) });
  });

  // Run force layout
  const laid = computeForceLayout(initVerts, layoutEdges);

  // Normalize to fit thumbnail: center and scale to [-35, 35]
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  laid.forEach(v => { minX = Math.min(minX, v.x); maxX = Math.max(maxX, v.x); minY = Math.min(minY, v.y); maxY = Math.max(maxY, v.y); });
  const rangeX = maxX - minX || 1, rangeY = maxY - minY || 1;
  const scale = Math.min(60 / rangeX, 60 / rangeY);
  const midX = (minX + maxX) / 2, midY = (minY + maxY) / 2;
  laid.forEach(v => { v.x = (v.x - midX) * scale; v.y = (v.y - midY) * scale; });

  // Separate internal node positions and external leg stubs
  const pos = {};
  sortedInternal.forEach(nd => { pos[nd] = laid[nd]; });

  // Map external legs: find which laid vertex each leg corresponds to
  const legStubs = [];
  const legsByNode = {};
  edgeList.forEach(([a, b]) => {
    if (a < 0 && b >= 0) {
      // Find the laid vertex for this leg (order matches the layoutEdges construction)
      if (!legsByNode[b]) legsByNode[b] = [];
      // The leg vertex was added in the same order as edgeList traversal
      legsByNode[b].push(legStubs.length);
      legStubs.push({ parent: b });
    }
    if (b < 0 && a >= 0) {
      if (!legsByNode[a]) legsByNode[a] = [];
      legsByNode[a].push(legStubs.length);
      legStubs.push({ parent: a });
    }
  });
  // Assign leg stub positions from laid vertices
  let legVertIdx = numInt; // external verts start after internal ones in laid array
  edgeList.forEach(([a, b]) => {
    if (a < 0 || b < 0) {
      const stubI = legStubs.findIndex(s => s.x === undefined);
      if (stubI >= 0) {
        legStubs[stubI].x = laid[legVertIdx].x;
        legStubs[stubI].y = laid[legVertIdx].y;
      }
      legVertIdx++;
    }
  });

  let cx = 0, cy = 0;
  sortedInternal.forEach(nd => { cx += pos[nd].x; cy += pos[nd].y; });
  cx /= numInt || 1; cy /= numInt || 1;

  // Build SVG
  const size = showLabels ? 100 : 80;
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('viewBox', `-${size/2} -${size/2} ${size} ${size}`);
  svg.setAttribute('width', '100%');
  svg.setAttribute('height', '100%');
  svg.style.display = 'block';

  // Collect drawable edges with colors
  // Nickel traversal order matches edgeMasses order, but edgeList order may differ.
  // Re-derive edges from Nickel traversal to stay aligned with edgeMasses.
  let massIdx = 0;
  let legIdx = 0;
  const drawEdges = [];
  for (let i = 0; i < nickelList.length; i++) {
    for (const j of nickelList[i]) {
      const mass = edgeMasses[massIdx++];
      const isLeg = j < 0;
      drawEdges.push({ from: i, to: j, mass, isLeg, legIndex: isLeg ? ++legIdx : 0 });
    }
  }
  // Add synthetic legs (from legVertices) that aren't in nickelList
  if (topo && !topo.nickelFull && topo.legVertices && topo.legVertices.length > 0) {
    for (const v of topo.legVertices) {
      drawEdges.push({ from: v, to: LEG, mass: 0, isLeg: true, legIndex: ++legIdx });
    }
  }

  // Classify mass labels for display (only when labels enabled)
  // Convention matching parseCNickelToGraph:
  //   - digit on both internal + external → "m" (shared mass, use internal name)
  //   - digit only on external legs → "M" (external-only mass)
  //   - digit only on internal edges → "m" (internal-only mass)
  //   - subscript only when multiple distinct scales of the same type
  if (showLabels) {
    const internalDigits = new Set();
    const externalOnlyDigits = new Set();
    const sharedDigits = new Set();
    const allDigitsOnInternal = new Set();
    const allDigitsOnExternal = new Set();
    drawEdges.forEach(e => {
      if (e.mass > 0) {
        if (e.isLeg) allDigitsOnExternal.add(e.mass);
        else allDigitsOnInternal.add(e.mass);
      }
    });
    allDigitsOnInternal.forEach(d => internalDigits.add(d));
    allDigitsOnExternal.forEach(d => {
      if (allDigitsOnInternal.has(d)) sharedDigits.add(d);
      else externalOnlyDigits.add(d);
    });
    // Count distinct "m-type" scales (internal + shared) and "M-type" scales (external-only)
    const mScales = new Set([...internalDigits, ...sharedDigits]);
    const bigMScales = externalOnlyDigits;

    const SUB = ['\u2080','\u2081','\u2082','\u2083','\u2084','\u2085','\u2086','\u2087','\u2088','\u2089'];
    drawEdges.forEach(e => {
      if (e.mass > 0) {
        if (e.isLeg && externalOnlyDigits.has(e.mass)) {
          // External-only mass → "M" (single) or "Mᵢ" where i = leg position
          // Convention: M[i] in the entry, where i is the boundary-ordered leg index
          e.massLabel = bigMScales.size === 1 ? 'M' : 'M' + (SUB[e.legIndex] || SUB[0]);
        } else {
          // Internal or shared mass → "m" or "mₐ" (subscript = mass class digit)
          e.massLabel = mScales.size === 1 ? 'm' : 'm' + (SUB[e.mass] || SUB[0]);
        }
      }
    });
  }

  // Track multi-edges between same pair for offset curves
  const pairCount = {};
  const pairSeen = {};
  drawEdges.forEach(e => {
    const key = e.to < 0 ? `leg_${e.from}_${Math.random()}` : `${Math.min(e.from, e.to)}_${Math.max(e.from, e.to)}`;
    e._pairKey = key;
    pairCount[key] = (pairCount[key] || 0) + 1;
  });

  // Leg stub counter per internal node
  const legCounter = {};
  const labelData = []; // Collect labels to draw after edges

  drawEdges.forEach(e => {
    const mc = massColor(e.mass, getEdgeMassKind(e));
    const color = mc || getComputedStyle(document.documentElement).getPropertyValue('--text-muted').trim();
    const sw = e.mass > 0 ? 2.5 : 1.5;

    if (e.to < 0) {
      // External leg
      if (!legCounter[e.from]) legCounter[e.from] = 0;
      const nodeLegs = legsByNode[e.from] || [];
      const stubIdx = nodeLegs[legCounter[e.from]++];
      if (stubIdx === undefined) return;
      const stub = legStubs[stubIdx];
      const line = document.createElementNS(SVG_NS, 'line');
      line.setAttribute('x1', pos[e.from].x);
      line.setAttribute('y1', pos[e.from].y);
      line.setAttribute('x2', stub.x);
      line.setAttribute('y2', stub.y);
      line.setAttribute('stroke', color);
      line.setAttribute('stroke-width', sw);
      line.setAttribute('stroke-linecap', 'round');
      svg.appendChild(line);

      if (showLabels) {
        // Leg tip label: p₁, p₂, ... (use legOrder to map canonical → original leg index)
        const dx = stub.x - pos[e.from].x, dy = stub.y - pos[e.from].y;
        const dl = Math.sqrt(dx * dx + dy * dy) || 1;
        const SUB_DIGITS = '\u2080\u2081\u2082\u2083\u2084\u2085\u2086\u2087\u2088\u2089';
        const origLeg = legOrder ? (legOrder[e.legIndex - 1] || e.legIndex) : e.legIndex;
        const legLabel = 'p' + (SUB_DIGITS[origLeg] || origLeg);
        labelData.push({
          x: stub.x + (dx / dl) * 7,
          y: stub.y + (dy / dl) * 7,
          text: legLabel,
          color: e.mass > 0 ? color : null,
          size: 8.5,
          bold: true
        });
        // Mass label on massive legs
        if (e.massLabel) {
          const mx = (pos[e.from].x + stub.x) / 2;
          const my = (pos[e.from].y + stub.y) / 2;
          const nx = -dy / dl, ny = dx / dl;
          // Offset perpendicular, away from centroid
          const dotProd = nx * (mx - cx) + ny * (my - cy);
          const sign = dotProd >= 0 ? 1 : -1;
          labelData.push({
            x: mx + nx * sign * 6,
            y: my + ny * sign * 6,
            text: e.massLabel,
            color: color,
            size: 7,
            bold: false
          });
        }
      }
    } else if (e.from === e.to) {
      // Self-loop
      const p = pos[e.from];
      const dx = p.x - cx, dy = p.y - cy;
      const ang = Math.atan2(dy, dx);
      const loopR = 8;
      const loopCx = p.x + Math.cos(ang) * loopR;
      const loopCy = p.y + Math.sin(ang) * loopR;
      const circle = document.createElementNS(SVG_NS, 'circle');
      circle.setAttribute('cx', loopCx);
      circle.setAttribute('cy', loopCy);
      circle.setAttribute('r', loopR);
      circle.setAttribute('fill', 'none');
      circle.setAttribute('stroke', color);
      circle.setAttribute('stroke-width', sw);
      svg.appendChild(circle);

      if (showLabels && e.massLabel) {
        // Mass label at far point of self-loop
        labelData.push({
          x: loopCx + Math.cos(ang) * (loopR + 5),
          y: loopCy + Math.sin(ang) * (loopR + 5),
          text: e.massLabel,
          color: color,
          size: 5.5,
          bold: false
        });
      }
    } else {
      // Internal edge — curve if multi-edge
      const key = e._pairKey;
      if (!pairSeen[key]) pairSeen[key] = 0;
      const idx = pairSeen[key]++;
      const total = pairCount[key];
      const p1 = pos[e.from], p2 = pos[e.to];
      if (!p1 || !p2) return;  // skip edges with missing vertex positions

      let edgeMx, edgeMy; // midpoint for mass label

      if (total === 1) {
        const line = document.createElementNS(SVG_NS, 'line');
        line.setAttribute('x1', p1.x); line.setAttribute('y1', p1.y);
        line.setAttribute('x2', p2.x); line.setAttribute('y2', p2.y);
        line.setAttribute('stroke', color);
        line.setAttribute('stroke-width', sw);
        line.setAttribute('stroke-linecap', 'round');
        svg.appendChild(line);
        edgeMx = (p1.x + p2.x) / 2;
        edgeMy = (p1.y + p2.y) / 2;
      } else {
        // Curved multi-edge
        const mx = (p1.x + p2.x) / 2, my = (p1.y + p2.y) / 2;
        const dx = p2.x - p1.x, dy = p2.y - p1.y;
        const nx = -dy, ny = dx;
        const nl = Math.sqrt(nx * nx + ny * ny) || 1;
        const offset = (idx - (total - 1) / 2) * 12;
        const cpx = mx + (nx / nl) * offset;
        const cpy = my + (ny / nl) * offset;
        const path = document.createElementNS(SVG_NS, 'path');
        path.setAttribute('d', `M${p1.x},${p1.y} Q${cpx},${cpy} ${p2.x},${p2.y}`);
        path.setAttribute('fill', 'none');
        path.setAttribute('stroke', color);
        path.setAttribute('stroke-width', sw);
        path.setAttribute('stroke-linecap', 'round');
        svg.appendChild(path);
        // Bézier midpoint at t=0.5: (p1 + 2*cp + p2) / 4
        edgeMx = (p1.x + 2 * cpx + p2.x) / 4;
        edgeMy = (p1.y + 2 * cpy + p2.y) / 4;
      }

      if (showLabels && e.massLabel) {
        const dx = p2.x - p1.x, dy = p2.y - p1.y;
        const dl = Math.sqrt(dx * dx + dy * dy) || 1;
        const nx = -dy / dl, ny = dx / dl;
        const dotProd = nx * (edgeMx - cx) + ny * (edgeMy - cy);
        const sign = dotProd >= 0 ? 1 : -1;
        labelData.push({
          x: edgeMx + nx * sign * 6,
          y: edgeMy + ny * sign * 6,
          text: e.massLabel,
          color: color,
          size: 5.5,
          bold: false
        });
      }
    }
  });

  // Draw internal node dots
  sortedInternal.forEach(nd => {
    const dot = document.createElementNS(SVG_NS, 'circle');
    dot.setAttribute('cx', pos[nd].x);
    dot.setAttribute('cy', pos[nd].y);
    dot.setAttribute('r', 2.5);
    dot.setAttribute('fill', getComputedStyle(document.documentElement).getPropertyValue('--node-fill').trim());
    svg.appendChild(dot);
  });

  // Draw labels on top of everything (KaTeX math font, matching canvas labels)
  if (showLabels) {
    const cs = getComputedStyle(document.documentElement);
    const textColor = cs.getPropertyValue('--text').trim();
    const bgColor = cs.getPropertyValue('--bg').trim();
    labelData.forEach(lbl => {
      const txt = document.createElementNS(SVG_NS, 'text');
      txt.setAttribute('x', lbl.x);
      txt.setAttribute('y', lbl.y);
      txt.setAttribute('text-anchor', 'middle');
      txt.setAttribute('dominant-baseline', 'central');
      txt.setAttribute('font-size', lbl.size);
      txt.setAttribute('fill', lbl.color || textColor);
      txt.setAttribute('pointer-events', 'none');
      txt.setAttribute('font-family', "'KaTeX_Math', 'Latin Modern Math', 'STIX Two Math', 'Cambria Math', 'Times New Roman', serif");
      txt.setAttribute('font-style', 'italic');
      // Text stroke for legibility against edges
      txt.setAttribute('paint-order', 'stroke fill');
      txt.setAttribute('stroke', bgColor);
      txt.setAttribute('stroke-width', lbl.size * 0.15);
      txt.setAttribute('stroke-linejoin', 'round');
      if (lbl.bold) txt.setAttribute('font-weight', '600');
      txt.textContent = lbl.text;
      svg.appendChild(txt);
    });
  }

  return svg;
}

// ─── Live reactive matching ──────────────────────────────────────────

let _matchDebounce = null;

let _lastTopoSig = '';

function onGraphChanged() {
  _momentumLabels = null;  // invalidate momentum cache
  // Reset chord selection if topology (edge connectivity) changed
  const topoSig = state.edges.map(e => `${e.a}-${e.b}`).join(',') + '|' + state.vertices.length;
  if (topoSig !== _lastTopoSig) {
    _lastTopoSig = topoSig;
    resetChordSelection();
  }
  saveDiagram();
  updateUrlHash();
  // Review-mode dirty detection: any canvas mutation after the initial load
  // sets the dirty flag (unless we're in the middle of loading an entry).
  if (reviewMode && !_reviewLoadingEntry) reviewSetDirty(true);
  // Normalize external legs: ensure a→b points inward (external→internal).
  // Skip edges the user explicitly reversed via the mass picker — otherwise
  // this loop immediately undoes their reversal on the next mutation.
  const deg = getVertexDegrees();
  for (const e of state.edges) {
    if (e.arrowReversed) continue;
    const dA = deg[e.a] || 0, dB = deg[e.b] || 0;
    // If b is external (degree 1) and a is internal, flip so external is a
    if (dB === 1 && dA > 1) {
      const tmp = e.a; e.a = e.b; e.b = tmp;
    }
  }
  // If the config panel is already open, refresh its body so the Kinematics
  // sections don't show "No edges yet" / "No external legs" when the user
  // opened the panel before drawing.
  const cfgPanel = $('config-panel');
  if (cfgPanel && cfgPanel.classList.contains('open')) {
    populateExtMasses();
    populateEdgeTable();
    populateKinematicsDisplay();
  }
  clearTimeout(_matchDebounce);
  _matchDebounce = setTimeout(doLiveMatch, 80);
}

function doLiveMatch() {
  updateIntegralCard();
  const edgeData = buildEdgeData();

  // Update Nickel readout
  if (!edgeData || edgeData.edges.length === 0) {
    currentNickel = null;
    currentMatches = [];
    $('nickel-readout').style.display = 'none';
    clearNotifications();
    return;
  }

  const { edges, masses } = edgeData;

  // Check connectivity
  try {
    if (!isConnected(edges)) {
      currentNickel = null;
      currentMatches = [];
      $('nickel-readout').style.display = 'none';
      clearNotifications();
      return;
    }
  } catch(e) {
    return;
  }

  try {
    const result = canonicalize(edges);
    const nickelStr = result.string;
    currentNickel = nickelStr;

    // Show Nickel readout
    $('nickel-readout').style.display = 'flex';
    $('nickel-display').textContent = nickelStr;

    // Search library
    if (!library || !library.topologies) return;

    // Try matching with both full and collapsed topologies.
    // For each, compute canvasMasses under ALL graph automorphisms (nodeMaps)
    // so that symmetry-equivalent mass configurations are found. E.g. a double
    // box with one off-shell leg should match regardless of which corner it's at.
    const matchSets = [];

    // 1) Full topology match
    const allCanvasMasses = result.nodeMaps.map(nm =>
      getCanvasMassArray(result.nickel, nm, edges, masses)
    );
    matchSets.push({ nickel: nickelStr, allCanvasMasses });

    // 2) Collapsed topology match (merge co-vertex external legs)
    try {
      const collapsed = buildCollapsedEdgeData();
      if (collapsed && collapsed.edges.length > 0 && isConnected(collapsed.edges)) {
        const cResult = canonicalize(collapsed.edges);
        const cNickel = cResult.string;
        if (cNickel !== nickelStr) {
          const allCMasses = cResult.nodeMaps.map(nm =>
            getCanvasMassArray(cResult.nickel, nm, collapsed.edges, collapsed.masses)
          );
          matchSets.push({ nickel: cNickel, allCanvasMasses: allCMasses });
        }
      }
    } catch (_) { /* collapsed topology may be degenerate — skip silently */ }

    // Build canonical Nickel index: re-canonicalize each library topology's Nickel
    // using the same canonicalize() function the canvas uses, so both forms match.
    // Also precompute JS-canonical-ordered labels for each config, so matching
    // can compare labels 1:1 with canvasMasses even when Python and JS
    // canonicalizers pick different representatives for the same graph.
    if (!library._canonIndex) {
      library._canonIndex = {};  // canonicalNickel -> [libraryKey, ...]
      for (const key in library.topologies) {
        try {
          const n = Nickel.fromString(key);
          const c = canonicalize(n.edges);
          const canon = c.string;
          if (!library._canonIndex[canon]) library._canonIndex[canon] = [];
          library._canonIndex[canon].push(key);
          // Precompute JS-canonical labels for each config under this topology.
          const topoObj = library.topologies[key];
          const cfgs = topoObj.configs || {};
          for (const ck in cfgs) {
            try {
              cfgs[ck]._jsLabels = libraryConfigLabelsInJSOrder(key, ck);
            } catch (_) {
              cfgs[ck]._jsLabels = null;
            }
          }
        } catch(_) {}
      }
    }

    const matches = [];
    const seenTopoKeys = new Set();
    for (const { nickel, allCanvasMasses } of matchSets) {
      // Look up by canonical form (handles different Nickel encodings of same graph)
      const candidateKeys = library._canonIndex[nickel] || [];
      for (const key of candidateKeys) {
        if (seenTopoKeys.has(key)) continue;
        seenTopoKeys.add(key);
        const topo = library.topologies[key];

        const configMatches = {};
        const configs = topo.configs || {};
        for (const ck in configs) {
          const labels = configs[ck]._jsLabels || parseConfigColoring(ck);
          // Try all graph automorphisms — pick the best match.
          // This ensures symmetry-equivalent mass configs are found.
          let best = 'none';
          for (const cm of allCanvasMasses) {
            const r = classifyConfigMatch(labels, cm);
            if (r === 'exact') { best = 'exact'; break; }
            if (r === 'compatible' && best !== 'exact') best = 'compatible';
          }
          configMatches[ck] = best;
        }

        matches.push({ topoKey: key, topo, configMatches });
      }
    }

    // ── Subtopology matching ──
    const subtopoMatches = [];
    if (library.subtopologies) {
      const seenSubParents = new Set(matches.map(m => m.topoKey)); // skip direct matches
      for (const { nickel } of matchSets) {
        const bareNickel = nickel.replace(/\|+$/, '|');
        const entries = library.subtopologies[nickel] || library.subtopologies[bareNickel] || [];
        for (const entry of entries) {
          if (seenSubParents.has(entry.p)) continue;
          seenSubParents.add(entry.p);
          const parentTopo = library.topologies[entry.p];
          if (!parentTopo) continue;
          subtopoMatches.push({
            topoKey: entry.p,
            topo: parentTopo,
            pinched: entry.e,
            collapsed: !!entry.c,
          });
        }
      }
    }
    // Sort: fewer pinched edges first (closer match), then by loop count
    subtopoMatches.sort((a, b) => (a.pinched.length - b.pinched.length) || ((a.topo.loops||0) - (b.topo.loops||0)));

    // Did matches change? (include config match info in comparison)
    const matchSig = matches.map(m =>
      m.topoKey + ':' + Object.entries(m.configMatches).sort().map(([k,v]) => k+'='+v).join(';')
    ).sort().join(',');
    const subSig = subtopoMatches.map(m => m.topoKey + ':' + m.pinched.join(',')).sort().join(';');
    const prevSig = currentMatches.map(m =>
      m.topoKey + ':' + Object.entries(m.configMatches || {}).sort().map(([k,v]) => k+'='+v).join(';')
    ).sort().join(',');
    const prevSubSig = currentSubtopoMatches.map(m => m.topoKey + ':' + m.pinched.join(',')).sort().join(';');
    if (matchSig !== prevSig || subSig !== prevSubSig) {
      currentMatches = matches;
      currentSubtopoMatches = subtopoMatches;
      updateNotifications();
      // If the Paper tab is showing the matched-papers list (no PDF loaded),
      // refresh it so the list tracks the current drawing.
      if (!_pdfDoc) _populateMatchedPapersList();
    }
  } catch (e) {
    // Canonicalization can fail on degenerate graphs — that's fine
    console.warn('doLiveMatch error:', e);
    currentNickel = null;
    $('nickel-readout').style.display = 'none';
    currentMatches = [];
    currentSubtopoMatches = [];
    clearNotifications();
  }
}

// ─── Notifications ───────────────────────────────────────────────────

function clearNotifications() {
  const stack = $('notif-stack');
  const body = document.getElementById('notif-sheet-body');
  [stack, body].forEach(root => {
    if (!root) return;
    root.querySelectorAll('.notif-toast').forEach(el => {
      el.classList.add('removing');
      setTimeout(() => el.remove(), 300);
    });
  });
  if (typeof syncMobileNotifChip === 'function') {
    setTimeout(syncMobileNotifChip, 320);
  }
}

function buildToastKey(topoKey, configKey) {
  return configKey ? `${topoKey}::${configKey}` : `topo::${topoKey}`;
}

function createTopoToast(topoKey, topo, cm) {
  const toast = document.createElement('div');
  toast.className = 'notif-toast notif-topo';
  toast.dataset.key = buildToastKey(topoKey, null);

  const name = topo.primaryName || topo.name || topo.Name || topoKey;
  const loops = topo.loops ?? topo.L ?? '?';
  const legs = topo.legs ?? '?';
  const props = topo.props ?? '?';

  // Check if any config toast will be shown (exact or compatible)
  const hasConfigMatch = Object.values(cm).some(v => v === 'exact' || v === 'compatible');

  const thumb = generateThumbnail(topoKey, null);
  thumb.classList.add('notif-thumb');
  const body = document.createElement('div');
  body.className = 'notif-body';
  const noMatchBadge = hasConfigMatch ? '' : '<span class="badge badge-muted" style="margin-left:6px">No exact match</span>';
  body.innerHTML = `
    <div class="notif-title">${name} topology${noMatchBadge}</div>
    <div class="notif-stats">
      <span><span class="notif-stat-val">${loops}</span> loop${loops !== 1 ? 's' : ''}</span>
      <span><span class="notif-stat-val">${legs}</span> leg${legs !== 1 ? 's' : ''}</span>
      <span><span class="notif-stat-val">${props}</span> prop${props !== 1 ? 's' : ''}</span>
    </div>
  `;
  toast.appendChild(thumb);
  toast.appendChild(body);
  toast.addEventListener('click', () => openDetailPanel(topoKey, topo, cm, null));
  return toast;
}

function createSubtopoToast(topoKey, topo, pinched, collapsed) {
  const toast = document.createElement('div');
  toast.className = 'notif-toast notif-subtopo';
  toast.dataset.key = `subtopo::${topoKey}`;

  const name = topo.primaryName || topo.name || topo.Name || topoKey;
  const loops = topo.loops ?? topo.L ?? '?';
  const legs = topo.legs ?? '?';
  const nContracted = pinched.length;

  const thumb = generateThumbnail(topoKey, null);
  thumb.classList.add('notif-thumb');
  const body = document.createElement('div');
  body.className = 'notif-body';
  let badges = `<span class="badge badge-gold">${nContracted} contracted</span>`;
  if (collapsed) badges += '<span class="badge badge-muted">legs collapsed</span>';
  body.innerHTML = `
    <div class="notif-cfg-header">
      <div class="notif-cfg-name">Subtopology of ${name}</div>
      <div class="notif-cfg-badges">${badges}</div>
    </div>
    <div class="notif-stats">
      <span><span class="notif-stat-val">${loops}</span> loop${loops !== 1 ? 's' : ''}</span>
      <span><span class="notif-stat-val">${legs}</span> leg${legs !== 1 ? 's' : ''}</span>
    </div>
  `;
  toast.appendChild(thumb);
  toast.appendChild(body);
  toast.addEventListener('click', () => openDetailPanel(topoKey, topo, {}, null));
  return toast;
}

function createConfigToast(topoKey, topo, cm, ck) {
  const configs = topo.configs || {};
  const cfg = configs[ck];
  if (!cfg) return null;

  const toast = document.createElement('div');
  toast.className = 'notif-toast notif-config';
  toast.dataset.key = buildToastKey(topoKey, ck);

  const match = cm[ck];
  const cfgNames = cfg.Names || cfg.names || [];
  const canonical = cfg.canonicalName || cfg.CanonicalName || '';
  const label = canonical || (cfgNames.length > 0
    ? (Array.isArray(cfgNames) ? cfgNames[0] : cfgNames)
    : ck);
  let badges = '';
  if (match === 'exact') badges += '<span class="badge badge-exact">Exact match</span>';
  const ms = cfg.MassScales ?? cfg.massScales;
  if (ms !== undefined && ms !== null) badges += `<span class="badge badge-accent">${ms} scale${ms !== 1 ? 's' : ''}</span>`;
  const fc = cfg.FunctionClass || cfg.functionClass;
  if (fc && fc !== 'None' && fc !== 'Unknown' && fc !== 'unknown') badges += functionBadge(fc);
  const epsLabel = summarizeEpsilonOrders(cfg);
  if (epsLabel) badges += `<span class="badge badge-gold">\u03B5: ${epsLabel}</span>`;
  badges += refCountBadge(cfg);

  const thumb = generateThumbnail(topoKey, ck);
  thumb.classList.add('notif-thumb');
  const cfgHasResults = (cfg.results||cfg.Results||[]).length > 0;
  const starPrefix = cfgHasResults ? '<span class="result-star" title="Result computed">\u2605</span> ' : '';
  const body = document.createElement('div');
  body.className = 'notif-body';
  body.innerHTML = `
    <div class="notif-cfg-header">
      <div class="notif-cfg-name">${starPrefix}${label}</div>
      <div class="notif-cfg-badges">${badges}</div>
    </div>
  `;
  toast.appendChild(thumb);
  toast.appendChild(body);
  toast.addEventListener('click', () => openDetailPanel(topoKey, topo, cm, ck));
  return toast;
}

function updateNotifications() {
  const stack = $('notif-stack');

  // Build the new set of toast keys + elements
  const newToasts = [];  // { key, element }
  currentMatches.forEach(({ topoKey, topo, configMatches }) => {
    const cm = configMatches || {};

    newToasts.push({
      key: buildToastKey(topoKey, null),
      create: () => createTopoToast(topoKey, topo, cm),
    });

    const matchedKeys = Object.keys(cm)
      .filter(k => cm[k] === 'exact' || cm[k] === 'compatible')
      .sort((a, b) => {
        if (cm[a] === cm[b]) return 0;
        return cm[a] === 'exact' ? -1 : 1;
      });

    matchedKeys.forEach(ck => {
      newToasts.push({
        key: buildToastKey(topoKey, ck),
        create: () => createConfigToast(topoKey, topo, cm, ck),
      });
    });
  });

  // Subtopology matches (shown after direct matches, limited to top 8)
  currentSubtopoMatches.slice(0, 8).forEach(({ topoKey, topo, pinched, collapsed }) => {
    newToasts.push({
      key: `subtopo::${topoKey}`,
      create: () => createSubtopoToast(topoKey, topo, pinched, collapsed),
    });
  });

  const newKeys = new Set(newToasts.map(t => t.key));

  // Collect existing toasts by key
  const existingByKey = {};
  stack.querySelectorAll('.notif-toast').forEach(el => {
    const k = el.dataset.key;
    if (k) existingByKey[k] = el;
  });
  const existingKeys = new Set(Object.keys(existingByKey));

  // Determine removed, kept, added
  const removedKeys = [...existingKeys].filter(k => !newKeys.has(k));
  const addedToasts = newToasts.filter(t => !existingKeys.has(t.key));

  // Slide out removed toasts
  removedKeys.forEach(k => {
    const el = existingByKey[k];
    el.classList.add('removing');
    setTimeout(() => el.remove(), 300);
  });

  // Update kept toasts (refresh click handlers for new cm)
  // Just leave them in place — topology doesn't change, position stays

  // Insert new toasts in correct order after exit animation settles
  const insertDelay = removedKeys.length > 0 ? 320 : 0;
  if (addedToasts.length > 0) {
    setTimeout(() => {
      // Rebuild order: place new toasts in correct position
      const allCurrentEls = {};
      stack.querySelectorAll('.notif-toast:not(.removing)').forEach(el => {
        if (el.dataset.key) allCurrentEls[el.dataset.key] = el;
      });

      let staggerIdx = 0;
      newToasts.forEach(({ key, create }) => {
        if (allCurrentEls[key]) return; // already in DOM
        const el = create();
        if (!el) return;
        el.style.animationDelay = `${staggerIdx * 60}ms`;
        stack.appendChild(el);
        staggerIdx++;
      });

      // Ensure subtopology toasts are always at the bottom
      stack.querySelectorAll('.notif-subtopo:not(.removing)').forEach(el => {
        stack.appendChild(el);
      });
      syncMobileNotifChip();
    }, insertDelay);
  } else {
    syncMobileNotifChip();
  }
}

// ─── Mobile notification chip + bottom sheet ─────────────────────────
// On ≤768px the right-side .notif-stack is hidden. We surface a single
// chip at top-center summarizing the current topology + config matches.
// Tap → open a bottom sheet that reparents the .notif-toast elements
// from #notif-stack. Closing the sheet moves them back.

function _isMobileNotifMode() {
  return typeof matchMedia === 'function' && matchMedia('(max-width: 768px)').matches;
}

function _notifChipPrimary(stack) {
  // Prefer active > topo > config > subtopo; fall back to first child
  return stack.querySelector('.notif-toast.notif-active')
      || stack.querySelector('.notif-toast.notif-topo')
      || stack.querySelector('.notif-toast.notif-config')
      || stack.querySelector('.notif-toast');
}

function _notifChipLabelFor(toast) {
  if (!toast) return '';
  const titleEl = toast.querySelector('.notif-title, .notif-cfg-name');
  if (!titleEl) return '';
  // Strip any trailing "No exact match" badge text etc.; take the first
  // visible text node chunk only.
  const clone = titleEl.cloneNode(true);
  clone.querySelectorAll('.badge').forEach(b => b.remove());
  return (clone.textContent || '').trim();
}

function syncMobileNotifChip() {
  const chip = document.getElementById('notif-chip');
  if (!chip) return;
  const stack = document.getElementById('notif-stack');
  const label = document.getElementById('notif-chip-label');
  const count = document.getElementById('notif-chip-count');
  const sheetOpen = document.getElementById('notif-sheet')?.classList.contains('open');

  // If the sheet is open, the toasts live inside the sheet — count from there.
  const source = sheetOpen ? document.getElementById('notif-sheet-body') : stack;
  const toasts = source ? source.querySelectorAll('.notif-toast:not(.removing)') : [];

  if (!toasts.length) {
    chip.hidden = true;
    chip.classList.remove('has-config');
    if (sheetOpen) closeNotifSheet();
    return;
  }

  // Prefer topology + first config match for a "Topo · Config" label.
  const topo = [...toasts].find(t => t.classList.contains('notif-topo'));
  const cfg  = [...toasts].find(t => t.classList.contains('notif-config'));
  const primary = topo || _notifChipPrimary({ querySelector: s => source.querySelector(s) });
  const topoLabel = topo ? _notifChipLabelFor(topo) : '';
  const cfgLabel  = cfg  ? _notifChipLabelFor(cfg)  : '';
  let text = topoLabel && cfgLabel ? `${topoLabel} · ${cfgLabel}`
           : topoLabel || cfgLabel || _notifChipLabelFor(primary);
  // Strip trailing " topology" from the topo title for a tighter chip
  text = text.replace(/\s+topology(\s+·\s+|$)/, '$1');
  if (label) label.textContent = text;

  chip.classList.toggle('has-config', !!cfg && !topo);
  if (count) {
    if (toasts.length > 1) { count.hidden = false; count.textContent = String(toasts.length); }
    else                   { count.hidden = true;  count.textContent = ''; }
  }
  chip.hidden = false;
}

function openNotifSheet() {
  if (!_isMobileNotifMode()) return;
  const stack = document.getElementById('notif-stack');
  const sheet = document.getElementById('notif-sheet');
  const body  = document.getElementById('notif-sheet-body');
  const backdrop = document.getElementById('notif-sheet-backdrop');
  const chip = document.getElementById('notif-chip');
  if (!stack || !sheet || !body || !backdrop || !chip) return;
  // Move toasts into the sheet body (preserves click handlers)
  [...stack.querySelectorAll('.notif-toast:not(.removing)')].forEach(el => body.appendChild(el));
  backdrop.hidden = false;
  sheet.hidden = false;
  sheet.setAttribute('aria-hidden', 'false');
  // Force reflow so the transition fires
  void sheet.offsetWidth;
  backdrop.classList.add('open');
  sheet.classList.add('open');
  chip.setAttribute('aria-expanded', 'true');
}

function closeNotifSheet() {
  const stack = document.getElementById('notif-stack');
  const sheet = document.getElementById('notif-sheet');
  const body  = document.getElementById('notif-sheet-body');
  const backdrop = document.getElementById('notif-sheet-backdrop');
  const chip = document.getElementById('notif-chip');
  if (!sheet || !sheet.classList.contains('open')) return;
  sheet.classList.remove('open');
  if (backdrop) backdrop.classList.remove('open');
  sheet.setAttribute('aria-hidden', 'true');
  if (chip) chip.setAttribute('aria-expanded', 'false');
  // After the slide-down animation, move toasts back and hide sheet
  setTimeout(() => {
    if (stack && body) {
      [...body.querySelectorAll('.notif-toast:not(.removing)')].forEach(el => stack.appendChild(el));
    }
    if (sheet) sheet.hidden = true;
    if (backdrop) backdrop.hidden = true;
    // Subtopology toasts go to the bottom of the stack
    if (stack) stack.querySelectorAll('.notif-subtopo:not(.removing)').forEach(el => stack.appendChild(el));
  }, 300);
}

(function _wireMobileNotifChip() {
  if (typeof document === 'undefined') return;
  const ready = () => {
    const chip = document.getElementById('notif-chip');
    const sheet = document.getElementById('notif-sheet');
    const closeBtn = document.getElementById('notif-sheet-close');
    const backdrop = document.getElementById('notif-sheet-backdrop');
    if (chip) chip.addEventListener('click', () => {
      if (sheet && sheet.classList.contains('open')) closeNotifSheet();
      else                                            openNotifSheet();
    });
    if (closeBtn) closeBtn.addEventListener('click', closeNotifSheet);
    if (backdrop) backdrop.addEventListener('click', closeNotifSheet);
    // Close sheet if viewport grows past the mobile breakpoint
    if (typeof matchMedia === 'function') {
      matchMedia('(max-width: 768px)').addEventListener('change', (e) => {
        if (!e.matches) closeNotifSheet();
        syncMobileNotifChip();
      });
    }
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', ready);
  else ready();
})();

// ─── INSPIRE metadata cache ─────────────────────────────────────────

const _inspireCache = {}; // arxivId → { citations, authors, title } or 'pending'
const _inspirePendingCallbacks = {}; // arxivId → [onResult, onResult, ...] queued while 'pending'

// Convert "Last, First Middle" → "First Middle Last". Passes through if no
// comma (already in First Last form).
function _firstLast(name) {
  if (typeof name !== 'string' || !name.trim()) return name || '';
  const s = name.trim();
  if (!s.includes(',')) return s;
  const [last, rest] = s.split(/,\s*/, 2);
  return rest ? `${rest} ${last}` : s;
}

// Resolve the durable cache's author list into display-ready objects by
// cross-referencing the durable author profile index (for preferred_name).
function resolveAuthors(authors, authorIndex) {
  if (!Array.isArray(authors)) return [];
  authorIndex = authorIndex || {};
  return authors.map(a => {
    if (typeof a === 'string') return { full_name: a, display_name: _firstLast(a) };
    const iid = a.inspire_id;
    const profile = iid != null ? authorIndex[String(iid)] : null;
    const pref = profile && profile.preferred_name;
    return {
      full_name: a.full_name || '',
      inspire_id: iid || null,
      display_name: pref || _firstLast(a.full_name || ''),
    };
  });
}

// Durable cache baked at scripts/refresh_inspire_metadata.py — loaded lazily
// the first time fetchInspireData is called. Contains title/authors/journal/
// volume/year/pages/doi/texkey per arxiv_id but deliberately NOT citations
// (those are always fetched live, see below).
let _inspireDurableCache = null;
let _inspireDurableAuthors = null;
let _inspireDurableCachePromise = null;
function _loadDurableInspireCache() {
  if (_inspireDurableCache !== null) return Promise.resolve({ arxiv: _inspireDurableCache, authors: _inspireDurableAuthors });
  if (_inspireDurableCachePromise) return _inspireDurableCachePromise;
  _inspireDurableCachePromise = fetch('inspire_cache.json')
    .then(r => r.ok ? r.json() : { arxiv_ids: {}, authors: {} })
    .then(d => {
      _inspireDurableCache = d.arxiv_ids || {};
      _inspireDurableAuthors = d.authors || {};
      return { arxiv: _inspireDurableCache, authors: _inspireDurableAuthors };
    })
    .catch(() => {
      _inspireDurableCache = {};
      _inspireDurableAuthors = {};
      return { arxiv: _inspireDurableCache, authors: _inspireDurableAuthors };
    });
  return _inspireDurableCachePromise;
}

// Kick off the durable cache fetch eagerly on load so the first topology
// the user opens can pull titles synchronously instead of waiting a round trip.
_loadDurableInspireCache();

/** Synchronous peek at the durable INSPIRE cache. Returns the cached record
 *  for an arXiv ID (with .title, .authors, etc.), or null if the cache
 *  hasn't loaded yet or there is no hit. Use this at render time so the
 *  title fills in without waiting for a microtask. */
function getDurableInspireHit(arxivId) {
  if (_inspireDurableCache === null || !arxivId) return null;
  return _inspireDurableCache[arxivId] || null;
}

/**
 * Extract arXiv ID from a reference string.
 * Returns the ID (e.g., '1704.05465' or 'hep-ph/9409388') or null.
 */
function extractArxivId(ref) {
  const m = ref.match(/(\d{4}\.\d{4,5})/);
  if (m) return m[1];
  const old = ref.match(/((?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})/);
  if (old) return old[1];
  return null;
}

/**
 * Fetch INSPIRE metadata for an arXiv ID.
 * Updates the cache and calls onResult(data) when done.
 */
function fetchInspireData(idOrTexkey, onResult) {
  if (_inspireCache[idOrTexkey]) {
    if (_inspireCache[idOrTexkey] === 'pending') {
      // Fetch is in flight from an earlier call — queue this callback so
      // it fires alongside the original requester. Without this queue,
      // every subsequent caller during the pending window silently got no
      // data, which is why many records' authors stayed blank even though
      // INSPIRE had them.
      (_inspirePendingCallbacks[idOrTexkey] ||= []).push(onResult);
    } else {
      onResult(_inspireCache[idOrTexkey]);
    }
    return;
  }
  _inspireCache[idOrTexkey] = 'pending';
  _inspirePendingCallbacks[idOrTexkey] = [];

  // Consult the durable inspire_cache.json first. If we have a hit for the
  // arxiv ID (not a texkey — cache is keyed by arxiv_id), use the stored
  // fields immediately and then hit INSPIRE live only for the citation count.
  const isTexkey = idOrTexkey.includes(':');
  _loadDurableInspireCache().then(({ arxiv, authors: authorIndex }) => {
    const dHit = !isTexkey ? arxiv[idOrTexkey] : null;
    if (dHit && dHit.title) {
      // Serve cached fields right away so the UI renders without waiting.
      const baseResult = {
        citations: null,    // filled in by the live fetch below
        // Preserve the structured form so the UI can build direct author
        // links (inspire_id) and prefer preferred_name where available.
        authors: resolveAuthors(dHit.authors, authorIndex).slice(0, 10),
        title: dHit.title,
        journal: dHit.journal,
        volume: dHit.volume,
        pages: dHit.pages,
        year: dHit.year,
        doi: dHit.doi,
        texkey: dHit.texkey,
        inspire_id: dHit.inspire_id,
        published: !!dHit.published,
      };
      _inspireCache[idOrTexkey] = baseResult;
      const queued = _inspirePendingCallbacks[idOrTexkey] || [];
      delete _inspirePendingCallbacks[idOrTexkey];
      onResult(baseResult);
      for (const cb of queued) { try { cb(baseResult); } catch (e) { console.error(e); } }

      // Fetch citation count live (always up-to-date). If the paper is not
      // yet published, also re-pull publication_info so we pick up newly
      // assigned journal/volume.
      const fields = dHit.published
        ? 'citation_count'
        : 'citation_count,publication_info.journal_title,publication_info.journal_volume,publication_info.page_start,publication_info.year,dois.value';
      const url = `https://inspirehep.net/api/literature?q=eprint+${encodeURIComponent(idOrTexkey)}&fields=${fields}`;
      fetch(url).then(r => r.json()).then(data => {
        const hit = (data.hits && data.hits.hits && data.hits.hits[0]) ? data.hits.hits[0].metadata : null;
        if (!hit) return;
        const updated = Object.assign({}, baseResult);
        updated.citations = hit.citation_count ?? null;
        if (!dHit.published && hit.publication_info && hit.publication_info[0]) {
          const pub = hit.publication_info[0];
          if (pub.journal_title) {
            updated.journal = pub.journal_title;
            updated.volume = pub.journal_volume;
            updated.pages = pub.page_start;
            updated.year = pub.year;
            updated.published = true;
          }
        }
        if (hit.dois && hit.dois.length && !updated.doi) updated.doi = hit.dois[0].value;
        _inspireCache[idOrTexkey] = updated;
        onResult(updated);
      }).catch(() => { /* keep base result */ });
      return;
    }

    // No durable cache hit — fall through to the original live fetch.
    const q = isTexkey
      ? `texkey+${encodeURIComponent(idOrTexkey)}`
      : `eprint+${encodeURIComponent(idOrTexkey)}`;
    const url = `https://inspirehep.net/api/literature?q=${q}&fields=citation_count,authors.full_name,authors.record,titles.title,publication_info.journal_title,publication_info.journal_volume,publication_info.page_start,publication_info.year,dois.value`;
    fetch(url).then(r => r.json()).then(data => {
      const hit = (data.hits && data.hits.hits && data.hits.hits[0]) ? data.hits.hits[0].metadata : null;
      if (hit) {
        const pub = (hit.publication_info || [{}])[0];
        const result = {
          citations: hit.citation_count ?? null,
          authors: (hit.authors || []).map(a => {
            const ref = (a.record && a.record.$ref) || '';
            const m = ref.match(/\/authors\/(\d+)/);
            return { full_name: a.full_name || '', inspire_id: m ? parseInt(m[1], 10) : null,
                     display_name: _firstLast(a.full_name || '') };
          }).slice(0, 10),
          title: (hit.titles && hit.titles[0]) ? hit.titles[0].title : null,
          journal: pub.journal_title,
          volume: pub.journal_volume,
          pages: pub.page_start,
          year: pub.year,
          doi: (hit.dois && hit.dois[0]) ? hit.dois[0].value : null,
          published: !!pub.journal_title,
        };
        _inspireCache[idOrTexkey] = result;
        const queued = _inspirePendingCallbacks[idOrTexkey] || [];
        delete _inspirePendingCallbacks[idOrTexkey];
        onResult(result);
        for (const cb of queued) { try { cb(result); } catch (e) { console.error(e); } }
      } else {
        const empty = { citations: null, authors: [], title: null };
        _inspireCache[idOrTexkey] = empty;
        const queued = _inspirePendingCallbacks[idOrTexkey] || [];
        delete _inspirePendingCallbacks[idOrTexkey];
        for (const cb of queued) { try { cb(empty); } catch (e) { console.error(e); } }
      }
    }).catch(() => {
      const empty = { citations: null, authors: [], title: null };
      _inspireCache[idOrTexkey] = empty;
      const queued = _inspirePendingCallbacks[idOrTexkey] || [];
      delete _inspirePendingCallbacks[idOrTexkey];
      for (const cb of queued) { try { cb(empty); } catch (e) { console.error(e); } }
    });
  });
}

// ─── Detail Panel ────────────────────────────────────────────────────

/** Clean Mathematica escapes left over from library import. */
function cleanMmaEscapes(s) {
  if (!s || typeof s !== 'string') return typeof s === 'string' ? s : '';
  return s
    .replace(/\\221/g, '\u2018')   // left single quote
    .replace(/\\222/g, '\u2019')   // right single quote
    .replace(/\\226/g, '\u2013')   // en-dash
    .replace(/\\:0304RightArrow/g, '\u2192')  // Mathematica \[RightArrow]
    .replace(/RightArrow/g, '\u2192')          // bare RightArrow
    .replace(/\\:03B5/g, '\u03B5')             // epsilon
    .replace(/\\:03B1/g, '\u03B1')             // alpha
    .replace(/\\:03B2/g, '\u03B2')             // beta
    .replace(/\\:03B3/g, '\u03B3')             // gamma
    .replace(/\\:03BC/g, '\u03BC')             // mu
    .replace(/\\:03C0/g, '\u03C0')             // pi
    .replace(/\\:221E/g, '\u221E')             // infinity
    .replace(/\\:\w{4}/g, '');                 // strip remaining Mathematica escapes
}

/** Map function class string to a short label + colored badge.
 *  Keyword matching handles verbose arXiv descriptions gracefully. */
function functionBadge(fc) {
  // Priority order: higher complexity wins
  const KEYWORDS = [
    { re: /Calabi[–-]?Yau/i,   label: 'Calabi\u2013Yau', cls: 'badge-royalblue' },
    { re: /elliptic/i,          label: 'elliptic',         cls: 'badge-purple' },
    { re: /MZV/,                label: 'MZV',              cls: 'badge-blue' },
    { re: /MPL/,                label: 'MPL',              cls: 'badge-blue' },
    { re: /mixed/i,             label: 'mixed',            cls: 'badge-purple' },
    { re: /logarithm/i,        label: 'MPL',              cls: 'badge-blue' },
    { re: /rational|Gamma/i,   label: 'rational',         cls: 'badge-muted' },
  ];
  for (const { re, label, cls } of KEYWORDS) {
    if (re.test(fc)) {
      const title = fc !== label ? ` title="${fc.replace(/"/g, '&quot;')}"` : '';
      return `<span class="badge ${cls}"${title}>${label}</span>`;
    }
  }
  // Fallback: truncate long descriptions
  const short = fc.length > 20 ? fc.slice(0, 18) + '\u2026' : fc;
  const title = fc.length > 20 ? ` title="${fc.replace(/"/g, '&quot;')}"` : '';
  return `<span class="badge badge-muted"${title}>${short}</span>`;
}

/** Generate source badge(s). Supports Source as string or array. */
function sourceBadge(cfg) {
  const raw = cfg.source || cfg.Source || '';
  let srcs = Array.isArray(raw) ? raw : (raw ? [raw] : []);
  // If arXiv is present, suppress Loopedia badge (arXiv subsumes it)
  if (srcs.includes('arXiv')) srcs = srcs.filter(s => s !== 'Loopedia');
  const badgeMap = {
    'arXiv': '<span class="badge badge-royalblue">arXiv</span>',
    'Loopedia': '<span class="badge badge-green">Loopedia</span>',
    'QCDLoop': '<span class="badge badge-teal">QCDLoop</span>',
    'SubTropica': '<span class="badge badge-accent">SubTropica</span>',
  };
  return srcs.map(s => badgeMap[s] || '').join('');
}

/** Check if a config has a given source (works with string or array). */
function hasSource(cfg, name) {
  const raw = cfg.source || cfg.Source || '';
  return Array.isArray(raw) ? raw.includes(name) : raw === name;
}

/** Generate a reference count badge if there are multiple refs. */
function refCountBadge(cfg) {
  const refs = cfg.references || cfg.References || [];
  if (refs.length > 1) return `<span class="badge badge-muted">${refs.length} refs</span>`;
  return '';
}

/**
 * Summarize epsilon orders across all records of a config.
 * Returns a compact label string, or null if no info.
 */
function summarizeEpsilonOrders(cfg) {
  const records = cfg.Records || cfg.records || [];
  let hasAll = false;
  let maxBound = -Infinity;
  const epsSet = new Set();
  records.forEach(rec => {
    // Some library records store epsOrders as a number or array instead of a
    // string; coerce so the regex .match doesn't throw.
    const rawEo = rec.EpsOrders ?? rec.epsOrders ?? '';
    const eo = Array.isArray(rawEo) ? rawEo.join(',') : String(rawEo);
    if (!eo || eo === 'None') return;
    if (eo === 'all') { hasAll = true; return; }
    const leMatch = eo.match(/LessEqual\s+(-?\d+)/);
    if (leMatch) { maxBound = Math.max(maxBound, parseInt(leMatch[1])); return; }
    const uwMatch = eo.match(/Upto\s+Weight\s+(\d+)/i);
    if (uwMatch) { maxBound = Math.max(maxBound, parseInt(uwMatch[1])); return; }
    eo.split(/[;,]/).map(s => s.trim()).filter(Boolean).forEach(o => {
      const n = parseInt(o);
      if (!isNaN(n)) epsSet.add(n);
    });
  });
  if (hasAll) return 'all orders';
  if (maxBound > -Infinity) {
    const minKnown = epsSet.size > 0 ? Math.min(...epsSet) : 0;
    for (let i = minKnown; i <= maxBound; i++) epsSet.add(i);
  }
  if (epsSet.size === 0) return null;
  const sorted = [...epsSet].sort((a, b) => a - b);
  const isConsecutive = sorted.length >= 3 &&
    sorted.every((v, i) => i === 0 || v === sorted[i - 1] + 1);
  return isConsecutive
    ? `${sorted[0]}, \u2026, ${sorted[sorted.length - 1]}`
    : sorted.join(', ');
}

/**
 * Turn arXiv IDs and DOIs in a reference string into clickable links.
 * Handles: "arXiv:1705.06483", "hep-ph/0611236", "(doi: 10.1103/...)"
 */
function linkifyRef(ref) {
  let s = cleanMmaEscapes(ref);
  // DOI inside parentheses: (doi: 10.xxxx/yyyy)
  s = s.replace(
    /\(doi:\s*(10\.[^\s)<>]+)\)/g,
    (_, doi) => `(<a href="https://doi.org/${doi}" target="_blank" rel="noopener">doi:${doi}</a>)`
  );
  // Standalone DOI: doi:10.xxxx/yyyy (not already in an <a> tag)
  s = s.replace(
    /(?<!href=")(?<!">)\bdoi:\s*(10\.\d{4,}\/[^\s)<>]+)/gi,
    (_, doi) => `<a href="https://doi.org/${doi}" target="_blank" rel="noopener">doi:${doi}</a>`
  );
  // arXiv:XXXX.XXXXX (new-style)
  s = s.replace(
    /arXiv:(\d{4}\.\d{4,5})/g,
    (_, id) => `<a href="https://arxiv.org/abs/${id}" target="_blank" rel="noopener">arXiv:${id}</a>`
  );
  // arXiv:hep-ph/XXXXXXX (old-style with explicit arXiv: prefix)
  s = s.replace(
    /arXiv:((?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})\b/g,
    (_, id) => `<a href="https://arxiv.org/abs/${id}" target="_blank" rel="noopener">arXiv:${id}</a>`
  );
  // bare hep-ph/XXXXXXX (old-style without arXiv: prefix, not already linkified)
  s = s.replace(
    /(?<![:\/])\b((?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})\b/g,
    (_, id) => `<a href="https://arxiv.org/abs/${id}" target="_blank" rel="noopener">arXiv:${id}</a>`
  );
  // Journal references: e.g. "Nucl. Phys. B412 (1994) 523–552"
  // Link to INSPIRE search if not already inside an <a> tag
  s = s.replace(
    /\b((?:Nucl\. Phys\.|Phys\. Lett\.|Phys\. Rev\.|JHEP|Commun\. Math\. Phys\.|Eur\. Phys\. J\.|J\. Math\. Phys\.|Ann\. Phys\.|Prog\. Theor\. Phys\.|Nuovo Cim\.|Adv\. Math\.)[\s]*[A-Z]?\d+[\s]*\(\d{4}\)[\s]*\d+[–\-\u2013]?\d*)/g,
    (match) => {
      const query = encodeURIComponent(match.replace(/[–\u2013]/g, '-'));
      return `<a href="https://inspirehep.net/search?p=${query}" target="_blank" rel="noopener">${match}</a>`;
    }
  );
  return s;
}

function openDetailPanel(topoKey, topo, configMatches, configKey, opts) {
  const fromBrowser = opts && opts.fromBrowser;
  // Highlight the corresponding toast and keep stack browsable
  const activeKey = buildToastKey(topoKey, configKey);
  const notifStack = $('notif-stack');
  const notifSheetBody = document.getElementById('notif-sheet-body');
  [notifStack, notifSheetBody].forEach(root => {
    if (!root) return;
    root.querySelectorAll('.notif-toast').forEach(el => {
      el.classList.toggle('notif-active', el.dataset.key === activeKey);
    });
  });
  notifStack.classList.toggle('notif-browsing', !fromBrowser);
  // On mobile, the detail popup should be frontmost: close the notif sheet.
  if (typeof closeNotifSheet === 'function' && typeof _isMobileNotifMode === 'function' && _isMobileNotifMode()) {
    closeNotifSheet();
  }

  const panel = $('detail-panel');
  const cm = configMatches || {};
  const configs = topo.configs || {};
  const cfg = configKey ? configs[configKey] : null;
  const match = configKey ? (cm[configKey] || 'none') : null;

  // ── Build popup content ──
  const content = $('detail-configs');
  content.innerHTML = '';

  // Header: thumbnail + title block
  const header = document.createElement('div');
  header.className = 'popup-hero';

  // Extract legOrder from first result that has one
  let legOrder = null;
  if (cfg) {
    const cfgResults = cfg.results || cfg.Results || [];
    for (const r of cfgResults) {
      if (r.legOrder && r.legOrder.length > 0) { legOrder = r.legOrder; break; }
    }
  }
  const thumb = generateThumbnail(topoKey, configKey, { labels: true, legOrder });
  thumb.classList.add('popup-thumb');
  header.appendChild(thumb);

  const titleBlock = document.createElement('div');
  titleBlock.className = 'popup-title-block';

  const topoName = topo.primaryName || topo.name || topo.Name || topoKey;
  const loops = topo.loops ?? topo.L ?? '?';
  const legs = topo.legs ?? '?';
  const props = topo.props ?? '?';
  const cfgNames = cfg ? (cfg.Names || cfg.names || []) : [];
  const canonical = cfg ? (cfg.canonicalName || cfg.CanonicalName || '') : '';

  if (cfg) {
    // Config popup: canonical name as title, topology as context line, full CNickel.
    // Aliases = the legacy free-form Names from records (shown in tooltip).
    const primaryName = canonical
      || (cfgNames.length > 0
        ? (Array.isArray(cfgNames) ? cfgNames[0] : cfgNames)
        : topoName);
    const aliases = Array.isArray(cfgNames) ? cfgNames.filter(n => n && n !== primaryName) : [];
    const titleAttr = aliases.length > 0 ? ` title="Also known as: ${aliases.join(', ')}"` : '';
    const cNickel = cfg.CNickelIndex || cfg.CNickel || cfg.nickel || `${topoKey}:${configKey}`;
    titleBlock.innerHTML = `<div class="popup-name"${titleAttr}>${primaryName}</div>`;
    titleBlock.innerHTML += `<div class="popup-topo-line">${topoName} topology</div>`;
    titleBlock.innerHTML += `<div class="popup-nickel">${cNickel}</div>`;
    $('detail-header-title').textContent = primaryName;
  } else {
    // Topology-only popup
    titleBlock.innerHTML = `<div class="popup-name">${topoName}</div>`;
    titleBlock.innerHTML += `<div class="popup-nickel">${topoKey}</div>`;
    $('detail-header-title').textContent = topoName;
  }

  // Badges
  let badgeHTML = '';
  badgeHTML += `<span class="badge badge-muted">${loops} loop${loops !== 1 ? 's' : ''}</span>`;
  badgeHTML += `<span class="badge badge-muted">${legs} leg${legs !== 1 ? 's' : ''}</span>`;
  badgeHTML += `<span class="badge badge-muted">${props} propagator${props !== 1 ? 's' : ''}</span>`;
  if (match === 'exact') badgeHTML += '<span class="badge badge-exact">Exact match</span>';
  if (cfg) {
    const ms = cfg.MassScales ?? cfg.massScales;
    if (ms !== undefined && ms !== null) badgeHTML += `<span class="badge badge-accent">${ms} scale${ms !== 1 ? 's' : ''}</span>`;
    const fc = cfg.FunctionClass || cfg.functionClass;
    if (fc && fc !== 'None' && fc !== 'Unknown' && fc !== 'unknown') badgeHTML += functionBadge(fc);
    const epsLabel = summarizeEpsilonOrders(cfg);
    if (epsLabel) badgeHTML += `<span class="badge badge-gold">\u03B5: ${epsLabel}</span>`;
    badgeHTML += refCountBadge(cfg);
    // Verified badge removed from diagram-level header — it now appears
    // per-result in the result card badges (see popup-result-card).
  }
  titleBlock.innerHTML += `<div class="popup-badges">${badgeHTML}</div>`;

  header.appendChild(titleBlock);

  // Actions cluster — top-right of the hero.  Holds the diagram-level
  // external links (Loopedia, QCDLoop) and the "Load to editor" button.
  // These all describe the diagram as a whole, so they live in the hero
  // rather than duplicated on every record card.  Shown regardless of
  // how the popup was opened (library browser, toast, etc.).
  if (cfg) {
    const actions = document.createElement('div');
    actions.className = 'popup-hero-actions';

    const linkSvg = '<svg viewBox="0 0 24 24"><path d="M18 13v6a2 2 0 01-2 2H5a2 2 0 01-2-2V8a2 2 0 012-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>';

    if (hasSource(cfg, 'Loopedia')) {
      const loopediaUrl = `https://loopedia.mpp.mpg.de/?graph=${encodeURIComponent(topoKey)}`;
      actions.insertAdjacentHTML('beforeend',
        `<a class="detail-link detail-link-muted" href="${loopediaUrl}" target="_blank" rel="noopener">Loopedia ${linkSvg}</a>`);
    }
    const qcdloopUrl = cfg.QCDLoopURL || cfg.qcdloopURL || '';
    if (qcdloopUrl) {
      actions.insertAdjacentHTML('beforeend',
        `<a class="detail-link detail-link-muted" href="${qcdloopUrl}" target="_blank" rel="noopener">QCDLoop ${linkSvg}</a>`);
    }

    // "Load to editor" — always available for config popups, including
    // toast-opened ones.  closeBrowser() is a no-op when the library
    // browser isn't open, so it's safe to call unconditionally.
    const _loadClickHandler = () => {
      closeDetailPanel();
      closeBrowser();
      loadFromNickel(topoKey, configKey);
    };
    const loadBtn = document.createElement('button');
    loadBtn.className = 'popup-load-btn';
    loadBtn.textContent = 'Load to editor';
    loadBtn.addEventListener('click', _loadClickHandler);
    actions.appendChild(loadBtn);

    if (actions.children.length > 0) header.appendChild(actions);

    // Stash the handler so the mobile sticky-CTA below can re-use it without
    // duplicating the loadFromNickel call shape.
    panel.dataset.hasLoadCta = '1';
    panel._loadCtaHandler = _loadClickHandler;
  } else {
    panel.dataset.hasLoadCta = '';
    panel._loadCtaHandler = null;
  }

  content.appendChild(header);

  // ── Content body ──
  if (cfg) {
    // Config-specific popup: show references
    const records = cfg.Records || cfg.records || [];
    if (records.length > 0) {
      const refsSection = document.createElement('div');
      refsSection.className = 'popup-section';
      refsSection.innerHTML = `<div class="popup-section-title">References</div>`;

      records.forEach(rec => {
        const card = document.createElement('div');
        card.className = 'popup-record';

        const ref = cleanMmaEscapes(rec.Reference || rec.reference || '');
        const authors = cleanMmaEscapes(rec.Authors || rec.authors || '');
        const desc = cleanMmaEscapes(rec.Description || rec.description || '');
        const epsOrd = rec.EpsOrders || rec.epsOrders || '';

        let html = '';

        // Per-record external link buttons — arXiv and INSPIRE are tied
        // to the specific paper a record cites; diagram-level links
        // (Loopedia, QCDLoop) have moved to the hero actions cluster.
        const linkBtns = [];
        const linkSvg = '<svg viewBox="0 0 24 24"><path d="M18 13v6a2 2 0 01-2 2H5a2 2 0 01-2-2V8a2 2 0 012-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>';
        const arxivMatch = ref.match(/arXiv:(\d{4}\.\d{4,5})/);
        const oldArxivMatch = ref.match(/((?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})/);
        const arxivId = arxivMatch ? arxivMatch[1] : (oldArxivMatch ? oldArxivMatch[1] : null);
        if (arxivId) {
          linkBtns.push(`<a class="detail-link detail-link-muted" href="https://arxiv.org/abs/${arxivId}" target="_blank" rel="noopener">arXiv ${linkSvg}</a>`);
          linkBtns.push(`<a class="detail-link detail-link-muted" href="https://inspirehep.net/arxiv/${arxivId}" target="_blank" rel="noopener">INSPIRE ${linkSvg}</a>`);
        }
        // "Report issue" trigger.  Shown on every record so a reader who
        // spots a misidentification can flag it against a specific paper
        // × topology × config triple.  Data attributes carry the context
        // the modal needs; the listener is attached after card.innerHTML
        // below.
        const recId = rec.recordId || '';
        if (recId) {
          linkBtns.push(
            `<button type="button" class="report-issue-link"
                data-record-id="${escapeHtml(recId)}"
                data-arxiv-id="${escapeHtml(arxivId || '')}"
                title="Flag a problem with this entry">&#x26A0; Report</button>`
          );
        }
        if (linkBtns.length > 0) {
          html += `<div class="popup-record-links">${linkBtns.join(' ')}</div>`;
        }

        // Paper thumbnail (page 1 preview, float left)
        if (arxivId) {
          const safeId = arxivId.replace(/\//g, '_');
          html += `<img class="popup-record-pdf-thumb" src="paper-thumbs/${safeId}.jpg" alt="" data-arxiv-id="${escapeHtml(arxivId)}" loading="lazy" onerror="this.style.display='none'">`;
        }

        // Title slot (above authors). We look up the durable INSPIRE cache
        // synchronously; if the cache is already loaded (kicked off at page
        // load), the real paper title shows immediately with no flicker.
        // The async fetchInspireData below still fires — it updates the
        // element if the cache wasn't ready in time and adds the citation
        // badge once the live fetch completes.
        const cachedHit = getDurableInspireHit(arxivId);
        const initialTitle = cachedHit && cachedHit.title ? cachedHit.title : '';
        html += `<div class="popup-record-title">${renderInlineMathString(initialTitle)}</div>`;
        if (authors) {
          // Structured authors (list of {full_name, inspire_id}) or plain string
          const authorsIsArray = Array.isArray(authors);
          let formatted;
          let hadEtAl = false;
          if (authorsIsArray) {
            formatted = authors.map(a => {
              if (typeof a === 'string') {
                const display = _firstLast(a);
                const url = `https://inspirehep.net/authors?q=${encodeURIComponent(display)}`;
                return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(display)}</a>`;
              }
              const display = a.display_name || _firstLast(a.full_name || '');
              const iid = a.inspire_id;
              const url = iid ? `https://inspirehep.net/authors/${iid}` : `https://inspirehep.net/authors?q=${encodeURIComponent(display)}`;
              return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(display)}</a>`;
            });
          } else {
            let authStr = authors.replace(/,?\s*et\s+al\.?\s*$/i, '').trim();
            hadEtAl = /et\s+al/i.test(authors);
            const authorList = authStr.split(',').map(a => a.trim()).filter(Boolean);
            const isLastFirst = authorList.length >= 2 && !/\s/.test(authorList[0]) && !/\s/.test(authorList[1]);
            let names;
            if (isLastFirst) {
              names = [];
              for (let ai = 0; ai < authorList.length - 1; ai += 2) {
                names.push(authorList[ai + 1] + ' ' + authorList[ai]);
              }
              if (authorList.length % 2 === 1) names.push(authorList[authorList.length - 1]);
            } else {
              names = authorList;
            }
            formatted = names.map(name => {
              const url = `https://inspirehep.net/authors?q=${encodeURIComponent(name)}`;
              return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(name)}</a>`;
            });
          }
          let shortAuthors = formatted.length > 10
            ? formatted.slice(0, 10).join(', ') + ' et al.'
            : formatted.join(', ') + (hadEtAl ? ' et al.' : '');
          html += `<div class="popup-record-authors">${shortAuthors}</div>`;
        } else {
          html += `<div class="popup-record-authors"></div>`;
        }
        if (ref) {
          // Location info (Figure / Eq. / Section) is often inaccurate — hide it.
          html += `<div class="popup-record-ref">${linkifyRef(ref)}</div>`;
        }

        // Verified flag is stored in the library but not shown in the public UI

        // Per-record badges
        const recBadges = [];
        if (epsOrd) recBadges.push(`<span class="badge badge-gold">\u03B5: ${epsOrd}</span>`);
        const dimSchemeRaw = rec.dimScheme || rec.dim_scheme;
        if (dimSchemeRaw) {
          let ds = String(dimSchemeRaw).replace(/\bCDR\b/g, 'd=4-2*eps').replace(/\bD=/g, 'd=').replace(/2eps\b/g, '2*eps');
          recBadges.push(`<span class="badge badge-muted">${escapeHtml(ds)}</span>`);
        }
        const compLevel = rec.computationLevel || rec.computation_level;
        if (compLevel && compLevel !== 'full' && compLevel !== 'analytic') {
          const clCls = compLevel === 'IBP_only' ? 'badge-gold'
                      : compLevel === 'numeric' ? 'badge-muted'
                      : 'badge-muted';
          recBadges.push(`<span class="badge ${clCls}">${compLevel.replace(/_/g, ' ')}</span>`);
        }
        const miCount = rec.masterCount || rec.master_integral_count;
        if (miCount) recBadges.push(`<span class="badge badge-muted">${miCount} MI${miCount !== 1 ? 's' : ''}</span>`);
        if (recBadges.length > 0) {
          html += `<div class="popup-record-badges">${recBadges.join(' ')}</div>`;
        }

        // Description, shown beneath the badges so it reads as an
        // annotation rather than as the paper's own title.
        if (desc) {
          // Route through the inline-math renderer so any $...$ spans are
          // KaTeX-rendered. Non-math text is still HTML-escaped internally.
          html += `<div class="popup-record-desc">${renderInlineMathString(desc)}</div>`;
        }

        // Related papers — hidden for now (some data may be unreliable)
        // const relPapers = rec.relatedPapers || rec.related_papers;


        // Ancillary files (clickable links)
        if (rec.ancillaryFiles || (rec.ancillaryPaths && rec.ancillaryPaths.length > 0)) {
          const paths = rec.ancillaryPaths || [];
          const ancArxivId = extractArxivId(ref);
          html += '<div style="font-size:11px;color:var(--text-muted);margin-top:4px">';
          html += '\u{1F4CE} Ancillary files';
          if (paths.length > 0) {
            html += ': ' + paths.map(p => {
              let url;
              if (p.startsWith('http')) {
                url = p;
              } else if (ancArxivId) {
                const ancPath = p.startsWith('anc/') ? p : 'anc/' + p;
                url = `https://arxiv.org/src/${ancArxivId}/${ancPath}`;
              }
              return url
                ? `<a href="${url}" target="_blank" rel="noopener" style="color:var(--link)"><code style="font-size:10px">${escapeHtml(p)}</code></a>`
                : `<code style="font-size:10px">${escapeHtml(p)}</code>`;
            }).join(', ');
          }
          html += '</div>';
        }

        card.innerHTML = html;
        refsSection.appendChild(card);

        // Click thumbnail → open PDF viewer
        card.querySelector('.popup-record-pdf-thumb')?.addEventListener('click', (e) => {
          e.stopPropagation();
          const aid = e.target.dataset.arxivId;
          if (aid) openPdfPanel(aid, rec);
        });

        // Click "Report" → open correction modal with record context.
        // The closure captures topoKey / configKey / cfg / rec from the
        // enclosing openDetailPanel call, so the modal always knows which
        // (paper × topology × config) triple the user is flagging.
        card.querySelector('.report-issue-link')?.addEventListener('click', (e) => {
          e.stopPropagation();
          const recCanonical = cfg.canonicalName || cfg.CanonicalName || '';
          const recCNickel = cfg.CNickelIndex || cfg.CNickel || cfg.nickel || `${topoKey}:${configKey}`;
          openCorrectionModal({
            cnickelIndex: recCNickel,
            recordId: rec.recordId || '',
            arxivId: arxivId || '',
            canonicalName: recCanonical,
            record: rec,
          });
        });

        // Fetch INSPIRE metadata if we have an arXiv ID or texkey. No
        // open-panel guard: when the cache is already warm the callback
        // fires synchronously during this forEach, before panel.add('open')
        // runs a few lines below. Writing to the captured `card` is safe
        // even if the panel is closed later — the node is either live or
        // detached, both harmless.
        const inspireId = extractArxivId(ref) || rec.texkey || '';
        if (inspireId) {
          fetchInspireData(inspireId, (data) => {

            // Replace fallback description with real paper title
            const titleEl = card.querySelector('.popup-record-title');
            if (data.title && titleEl) {
              titleEl.innerHTML = renderInlineMathString(data.title);
            }

            // Always overwrite the authors line with INSPIRE's structured
            // data when present — the record's string is freeform "Last,
            // First X., Last, First" which can't reliably be re-ordered to
            // First Last, whereas INSPIRE ships a per-author display_name
            // derived from the preferred-name index.
            const authorsEl = card.querySelector('.popup-record-authors');
            if (data.authors.length > 0 && authorsEl) {
              const linked = data.authors.map(a => {
                if (typeof a === 'string') {
                  const display = _firstLast(a);
                  const url = `https://inspirehep.net/authors?q=${encodeURIComponent(display)}`;
                  return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(display)}</a>`;
                }
                const display = a.display_name || _firstLast(a.full_name || '');
                const url = a.inspire_id ? `https://inspirehep.net/authors/${a.inspire_id}`
                                            : `https://inspirehep.net/authors?q=${encodeURIComponent(display)}`;
                return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(display)}</a>`;
              });
              authorsEl.innerHTML = linked.length > 10
                ? linked.slice(0, 10).join(', ') + ' et al.'
                : linked.join(', ');
            }

            // Add citation badge
            if (data.citations != null) {
              const citBadge = document.createElement('span');
              citBadge.className = 'badge badge-muted';
              citBadge.textContent = `${data.citations} citation${data.citations !== 1 ? 's' : ''}`;
              const badgesEl = card.querySelector('.popup-record-badges');
              if (badgesEl) {
                badgesEl.appendChild(citBadge);
              } else {
                const newBadges = document.createElement('div');
                newBadges.className = 'popup-record-badges';
                newBadges.appendChild(citBadge);
                const descEl = card.querySelector('.popup-record-desc');
                if (descEl) descEl.before(newBadges);
                else card.appendChild(newBadges);
              }
            }
          });
        }
      });

      if (records.some(r => !r.verified)) {
        const note = document.createElement('div');
        note.style.cssText = 'font-size:10px;color:var(--text-muted);padding:4px 0 8px;line-height:1.4;font-style:italic';
        note.textContent = 'Data collection was AI-assisted and might contain errors.';
        refsSection.appendChild(note);
      }

      content.appendChild(refsSection);
      // Review-mode decorator (no-op when not in review mode)
      reviewDecorateRecords(refsSection, cfg, topoKey, configKey);
    }

    // ── Fallback references (when no Records but References list exists) ──
    if (records.length === 0) {
      const refs = cfg.References || cfg.references || [];
      const qcdUrl = cfg.QCDLoopURL || cfg.qcdloopURL || '';
      if (refs.length > 0 || qcdUrl) {
        const fallbackSection = document.createElement('div');
        fallbackSection.className = 'popup-section';
        fallbackSection.innerHTML = `<div class="popup-section-title">References</div>`;
        const linkSvg2 = '<svg viewBox="0 0 24 24"><path d="M18 13v6a2 2 0 01-2 2H5a2 2 0 01-2-2V8a2 2 0 012-2h6"/><polyline points="15 3 21 3 21 9"/><line x1="10" y1="14" x2="21" y2="3"/></svg>';
        refs.forEach(refStr => {
          const card = document.createElement('div');
          card.className = 'popup-record';
          let html = '';
          const fbLinks = [];
          const aid = extractArxivId(refStr);
          if (aid) {
            fbLinks.push(`<a class="detail-link detail-link-muted" href="https://arxiv.org/abs/${aid}" target="_blank" rel="noopener">arXiv ${linkSvg2}</a>`);
            fbLinks.push(`<a class="detail-link detail-link-muted" href="https://inspirehep.net/arxiv/${aid}" target="_blank" rel="noopener">INSPIRE ${linkSvg2}</a>`);
          }
          if (qcdUrl) fbLinks.push(`<a class="detail-link detail-link-muted" href="${qcdUrl}" target="_blank" rel="noopener">QCDLoop ${linkSvg2}</a>`);
          if (fbLinks.length) html += `<div class="popup-record-links">${fbLinks.join(' ')}</div>`;
          const fbHit = aid ? getDurableInspireHit(aid) : null;
          const fbInitialTitle = fbHit && fbHit.title ? fbHit.title : '';
          const fbTitleStyle = fbInitialTitle ? '' : ' style="display:none"';
          html += `<div class="popup-record-title"${fbTitleStyle}>${renderInlineMathString(fbInitialTitle)}</div>`;
          html += `<div class="popup-record-authors"></div>`;
          html += `<div class="popup-record-ref">${linkifyRef(refStr)}</div>`;
          card.innerHTML = html;
          fallbackSection.appendChild(card);
          if (aid) {
            fetchInspireData(aid, (data) => {
              const titleEl = card.querySelector('.popup-record-title');
              if (data.title && titleEl) { titleEl.innerHTML = renderInlineMathString(data.title); titleEl.style.display = ''; }
              const authorsEl = card.querySelector('.popup-record-authors');
              if (data.authors.length > 0 && authorsEl && !authorsEl.textContent.trim()) {
                const linked = data.authors.map(name => {
                  const url = `https://inspirehep.net/authors?q=${encodeURIComponent(name)}`;
                  return `<a href="${url}" target="_blank" rel="noopener" style="color:var(--text)">${escapeHtml(name)}</a>`;
                });
                authorsEl.innerHTML = linked.length > 10
                  ? linked.slice(0, 10).join(', ') + ' et al.'
                  : linked.join(', ');
              }
              if (data.citations != null) {
                const badge = document.createElement('span');
                badge.className = 'badge badge-muted';
                badge.textContent = `${data.citations} citation${data.citations !== 1 ? 's' : ''}`;
                card.appendChild(badge);
              }
            });
          }
        });
        content.appendChild(fallbackSection);
      }
    }

    // ── Computed Results section ──
    let results = cfg.results || cfg.Results || [];
    if (results.length > 0) {
      // Sort newest first
      results = results.slice().sort((a, b) =>
        (b.timestamp || '').localeCompare(a.timestamp || ''));

      // Detect disagreement: group by (dimension, epsOrder) and compare normalized TeX
      const normalizeTeX = (s) => (s || '').replace(/\s+/g, '').replace(/\\,|\\;|\\!/g, '');
      const groups = {};
      results.forEach(r => {
        const k = `${r.dimension || ''}|${r.epsOrder ?? ''}`;
        (groups[k] = groups[k] || []).push(r);
      });
      const disagreeingKeys = new Set();
      for (const k in groups) {
        const g = groups[k];
        if (g.length < 2) continue;
        const ref = normalizeTeX(g[0].resultTeX || g[0].result || '');
        if (g.some(r => normalizeTeX(r.resultTeX || r.result || '') !== ref)) {
          disagreeingKeys.add(k);
        }
      }

      const resultsSection = document.createElement('div');
      resultsSection.className = 'popup-section';
      resultsSection.innerHTML = `<div class="popup-section-title"><span class="result-star">\u2605</span> Computed Results</div>`;

      results.forEach(r => {
        const groupKey = `${r.dimension || ''}|${r.epsOrder ?? ''}`;
        const inDisagreement = disagreeingKeys.has(groupKey);
        const card = document.createElement('div');
        card.className = 'popup-record popup-result-card';

        let html = '';

        // Badge row: clean, all-badge layout
        const badges = [];

        // Dimension: prettify 4 - 2*eps → D = 4−2ε
        if (r.dimension) {
          const prettyDim = String(r.dimension)
            .replace(/\s*\*\s*eps\b/g, '\u03B5')
            .replace(/\s*-\s*/g, '\u2212')
            .replace(/\s*\+\s*/g, '+');
          badges.push(`<span class="badge badge-green">D = ${prettyDim}</span>`);
        }

        // Epsilon order
        if (r.epsOrder !== undefined && r.epsOrder !== null) {
          badges.push(`<span class="badge badge-gold">\u03B5 \u2264 ${r.epsOrder}</span>`);
        }

        // Non-trivial propagator exponents
        if (r.propExponents && Array.isArray(r.propExponents) && r.propExponents.some(e => e !== 1)) {
          badges.push(`<span class="badge badge-muted">\u03BD = [${r.propExponents.join(',')}]</span>`);
        }

        // Human-readable date (with "NEW" pill for < 7 days)
        if (r.timestamp) {
          const d = new Date(r.timestamp);
          if (!isNaN(d)) {
            const now = new Date();
            const diffMs = now - d;
            const diffDays = Math.floor(diffMs / (1000 * 60 * 60 * 24));
            const diffHrs = Math.floor(diffMs / (1000 * 60 * 60));
            let dateText;
            if (diffHrs < 24) dateText = diffHrs <= 1 ? 'today' : `${diffHrs}h ago`;
            else if (diffDays < 7) dateText = `${diffDays}d ago`;
            else dateText = d.toLocaleDateString('en-US', { month: 'short', day: 'numeric', year: 'numeric' });
            const fullTs = d.toLocaleString();
            badges.push(`<span class="badge badge-accent" title="${fullTs}">${dateText}</span>`);
            if (diffDays < 7) {
              badges.push(`<span class="badge badge-new">NEW</span>`);
            }
          }
        }

        // Contributor badge: initials, hover reveals full info
        if (r.contributor || r.stVersion) {
          const name = r.contributor || 'Anonymous';
          const initials = name === 'Anonymous' ? '?'
            : name.split(/\s+/).map(w => w[0]).filter(Boolean).slice(0, 2).join('').toUpperCase();
          const tooltip = [
            name !== 'Anonymous' ? `Contributed by ${name}` : 'Anonymous submission',
            r.stVersion ? `SubTropica ${r.stVersion.replace(/^SubTropica\s*v?/i, 'v')}` : '',
          ].filter(Boolean).join(' \u00B7 ');
          badges.push(`<span class="badge badge-accent" title="${tooltip}">${initials}</span>`);
        }

        // Numerically verified badge (per-result, not per-diagram)
        if (r.verified) {
          const method = r.method || '';
          const tip = r.verifiedAt
            ? `Numerically verified${method ? ' via ' + method : ''} on ${r.verifiedAt}`
            : `Numerically verified${method ? ' via ' + method : ''}`;
          badges.push(`<span class="badge badge-green" title="${tip}">\u2713 Verified</span>`);
        }

        // Disagreement warning: when results for the same (D, ε) from different contributors differ
        if (inDisagreement) {
          badges.push(`<span class="badge badge-warn" title="Multiple contributors submitted different results for this (D, ε). Values do not agree — treat with care.">\u26A0 differs</span>`);
        }

        if (badges.length > 0) {
          html += `<div class="popup-record-badges">${badges.join(' ')}</div>`;
        }

        // Result preview: TeX div (populated on-demand if only compressed is available)
        html += `<div class="popup-result-tex" id="result-tex-${Math.random().toString(36).substr(2,6)}"></div>`;

        // Alphabet (always visible as inline pills)
        if (r.wDefinitions && r.wDefinitions.length > 0) {
          // Normalized alphabet: show W_i = definition pills
          html += '<div style="margin-top:8px;display:flex;align-items:baseline;gap:6px;flex-wrap:wrap">';
          html += '<span style="font-size:11px;color:var(--text-muted);font-weight:500;white-space:nowrap">Alphabet</span>';
          html += '<div class="popup-result-alphabet popup-result-alphabet-w" style="display:flex;flex-wrap:wrap;gap:3px;line-height:1"></div>';
          html += '</div>';
        } else if (r.alphabet && r.alphabet.length > 0) {
          html += '<div style="margin-top:8px;display:flex;align-items:baseline;gap:6px;flex-wrap:wrap">';
          html += '<span style="font-size:11px;color:var(--text-muted);font-weight:500;white-space:nowrap">Alphabet</span>';
          html += '<div class="popup-result-alphabet" style="display:flex;flex-wrap:wrap;gap:3px;line-height:1"></div>';
          html += '</div>';
        }

        // Symbol (collapsible, with weight and term count in summary)
        {
          const symTex = r.normalizedSymbolTeX || r.symbolTeX;
          if (symTex) {
            const wt = r.symbolWeight || 0;
            const origNt = r.symbolTerms || 0;
            const normNt = r.normalizedSymbolTerms || 0;
            const symMeta = [];
            if (wt > 0) symMeta.push('weight ' + wt);
            if (normNt > 0 && normNt !== origNt) symMeta.push(origNt + '\u2009\u2192\u2009' + normNt + ' terms');
            else if (origNt > 0) symMeta.push(origNt + ' term' + (origNt !== 1 ? 's' : ''));
            const symLabel = 'Symbol' + (symMeta.length > 0 ? ' (' + symMeta.join(', ') + ')' : '');
            html += '<details class="popup-result-symbol-details" style="margin-top:6px">';
            html += '<summary style="cursor:pointer;font-size:12px;color:var(--text-muted);font-weight:500">' + symLabel + '</summary>';
            html += '<div class="popup-result-symbol" style="overflow-x:auto;padding:4px 0;margin-top:4px"></div>';
            html += '</details>';
          }
        }

        // Action buttons
        html += '<div class="popup-result-actions">';
        if (r.resultTeX || r.resultCompressed) html += `<button class="modal-btn-tiny popup-copy-tex">Copy LaTeX</button>`;
        if (r.normalizedSymbolTeX || r.symbolTeX) html += `<button class="modal-btn-tiny popup-copy-symbol-tex">Copy Symbol</button>`;
        if (r.resultCompressed && backendMode !== 'full') html += `<button class="modal-btn-tiny popup-download-nb" title="Download Mathematica notebook with full result">Download .nb</button>`;
        if (backendMode === 'full') html += `<button class="modal-btn-tiny popup-delete-result" style="margin-left:auto;color:var(--red)">\u2715 Delete</button>`;
        html += '</div>';

        card.innerHTML = html;

        // Render TeX preview: use existing resultTeX, or decode from compressed on demand
        const texEl = card.querySelector('.popup-result-tex');
        if (r.resultTeX && texEl) {
          // Legacy path: resultTeX already available
          try {
            const cleaned = cleanTeX(r.resultTeX);
            katex.render('\\displaystyle ' + cleaned, texEl, {
              throwOnError: false, displayMode: true, maxSize: Infinity, maxExpand: 10000, trust: true
            });
          } catch {
            // KaTeX internal error — try again in non-display mode (more lenient)
            try {
              katex.render(cleanTeX(r.resultTeX), texEl, {
                throwOnError: false, displayMode: false, maxSize: Infinity, trust: true
              });
            } catch {
              // Final fallback: formatted code block
              texEl.innerHTML = '<pre class="result-tex-fallback">' +
                r.resultTeX.replace(/&/g,'&amp;').replace(/</g,'&lt;') + '</pre>';
            }
          }
          texEl._texStr = r.resultTeX;
        } else if (r.resultCompressed && texEl && backendMode === 'full') {
          // On-demand decode from binary via kernel
          texEl.innerHTML = '<span style="color:var(--text-muted);font-size:11px">Decoding result\u2026</span>';
          kernel.post('decodeResult', { resultCompressed: r.resultCompressed }).then(dec => {
            if (dec.status === 'ok' && dec.resultTeX) {
              const cleaned = cleanTeX(dec.resultTeX);
              const label = dec.truncated ? ' + \\cdots' : '';
              try {
                katex.render('\\displaystyle ' + cleaned + label, texEl, {
                  throwOnError: false, displayMode: true, maxSize: 500
                });
              } catch { texEl.textContent = dec.resultTeX; }
              texEl._texStr = dec.resultTeX;

              // Render decoded symbol/alphabet if available
              const symEl2 = card.querySelector('.popup-result-symbol');
              if (symEl2 && dec.symbolTeX && typeof katex !== 'undefined') {
                card.querySelector('.popup-result-symbol-details')?.removeAttribute('hidden');
                try {
                  katex.render('\\displaystyle ' + dec.symbolTeX, symEl2, {
                    throwOnError: false, displayMode: true, maxSize: 500
                  });
                } catch { symEl2.textContent = dec.symbolTeX; }
              }
            } else {
              texEl.innerHTML = '<span style="color:var(--text-muted);font-style:italic">Could not decode result</span>';
            }
          }).catch(() => {
            texEl.innerHTML = '<span style="color:var(--text-muted);font-style:italic">Result stored (binary)</span>';
          });
        } else if (r.resultCompressed && texEl) {
          // Online mode: no kernel — show what we can
          texEl.innerHTML = '<span style="color:var(--text-muted);font-style:italic">Result stored as binary. Download .nb or open in Mathematica.</span>';
        } else if (texEl) {
          texEl.style.display = 'none';
        }

        // Render symbol TeX (prefer normalized)
        const symbolEl = card.querySelector('.popup-result-symbol');
        {
          const symTex = cleanSymbolTeX(r.normalizedSymbolTeX || r.symbolTeX);
          if (symbolEl && symTex && typeof katex !== 'undefined') {
            try {
              katex.render('\\displaystyle ' + symTex, symbolEl, {
                throwOnError: false, displayMode: true, maxSize: 500
              });
            } catch { symbolEl.textContent = symTex; }
          }
        }

        // Render alphabet pills (prefer W definitions; algebraic pairs fuse)
        const alphEl = card.querySelector('.popup-result-alphabet');
        renderAlphabetPills(alphEl, r.wDefinitions, r.alphabet);

        // Copy LaTeX handler
        card.querySelector('.popup-copy-tex')?.addEventListener('click', (e) => {
          e.stopPropagation();
          const tex = card.querySelector('.popup-result-tex')?._texStr;
          if (tex) {
            navigator.clipboard.writeText(tex).then(() => showWarningToast('LaTeX copied'));
          } else {
            showWarningToast('No LaTeX available');
          }
        });

        // Copy Symbol LaTeX handler (prefer normalized)
        card.querySelector('.popup-copy-symbol-tex')?.addEventListener('click', (e) => {
          e.stopPropagation();
          const symTex = cleanSymbolTeX(r.normalizedSymbolTeX || r.symbolTeX);
          if (symTex) {
            navigator.clipboard.writeText(symTex).then(() => showWarningToast('Symbol LaTeX copied'));
          }
        });

        // Download .nb handler
        card.querySelector('.popup-download-nb')?.addEventListener('click', (e) => {
          e.stopPropagation();
          downloadResultNotebook(r);
        });

        card.querySelector('.popup-delete-result')?.addEventListener('click', (e) => {
          e.stopPropagation();
          const btn = e.currentTarget;
          if (btn.dataset.confirming) {
            // Second click = confirmed
            deleteLibraryResult(topoKey, configKey, results.indexOf(r));
            card.style.transition = 'opacity 0.3s, max-height 0.3s';
            card.style.opacity = '0';
            card.style.maxHeight = card.offsetHeight + 'px';
            setTimeout(() => { card.style.maxHeight = '0'; card.style.overflow = 'hidden'; card.style.padding = '0'; card.style.margin = '0'; }, 50);
            setTimeout(() => card.remove(), 350);
          } else {
            // First click = ask for confirmation
            btn.dataset.confirming = '1';
            btn.textContent = 'Confirm delete?';
            btn.style.fontWeight = '600';
            setTimeout(() => {
              if (btn.dataset.confirming) {
                delete btn.dataset.confirming;
                btn.textContent = '\u2715 Delete';
                btn.style.fontWeight = '';
              }
            }, 3000);
          }
        });

        resultsSection.appendChild(card);
      });

      content.appendChild(resultsSection);
    }
  } else {
    // Topology-only popup: show gallery of all diagrams (mass configurations)
    const configKeys = Object.keys(configs);
    if (configKeys.length > 0) {
      const galSection = document.createElement('div');
      galSection.className = 'popup-section';
      galSection.innerHTML = `<div class="popup-section-title">Diagrams (${configKeys.length})</div>`;

      const grid = document.createElement('div');
      grid.className = 'popup-gallery';

      // Sort: exact matches first, then compatible, then the rest
      const sorted = configKeys.slice().sort((a, b) => {
        const rank = { exact: 0, compatible: 1, none: 2 };
        return (rank[cm[a]] ?? 2) - (rank[cm[b]] ?? 2);
      });

      sorted.forEach(ck => {
        const c = configs[ck];
        const cfgNames = c.Names || c.names || [];
        const canonical = c.canonicalName || c.CanonicalName || '';
        const label = canonical || (cfgNames.length > 0
          ? (Array.isArray(cfgNames) ? cfgNames[0] : cfgNames)
          : ck);
        const matchType = cm[ck] || 'none';

        const card = document.createElement('div');
        card.className = 'popup-gallery-card' + (matchType === 'exact' ? ' popup-gallery-exact' : matchType === 'compatible' ? ' popup-gallery-compat' : '');

        const cardThumb = generateThumbnail(topoKey, ck);
        cardThumb.classList.add('popup-gallery-thumb');
        card.appendChild(cardThumb);

        const cardInfo = document.createElement('div');
        cardInfo.className = 'popup-gallery-info';
        let cardBadges = '';
        const fc = c.FunctionClass || c.functionClass;
        if (fc && fc !== 'None' && fc !== 'Unknown' && fc !== 'unknown') cardBadges += functionBadge(fc);
        const epsLabel = summarizeEpsilonOrders(c);
        if (epsLabel) cardBadges += `<span class="badge badge-gold">\u03B5: ${epsLabel}</span>`;
        if (matchType === 'exact') cardBadges += '<span class="badge badge-exact">Exact</span>';
        cardBadges += refCountBadge(c);
        cardInfo.innerHTML = `<div class="popup-gallery-name">${label}</div>` +
          (cardBadges ? `<div class="popup-gallery-badges">${cardBadges}</div>` : '');
        card.appendChild(cardInfo);

        card.addEventListener('click', () => {
          openDetailPanel(topoKey, topo, cm, ck, fromBrowser ? { fromBrowser: true } : undefined);
        });
        grid.appendChild(card);
      });

      galSection.appendChild(grid);
      content.appendChild(galSection);
    }
  }

  // Mobile sticky-CTA: on ≤480px the popup becomes a fullscreen sheet and the
  // hero's top-right "Load to editor" button is far from the thumb. Append a
  // second CTA that position:sticky's to the bottom of the scroll area so it
  // stays reachable regardless of scroll position.
  if (panel._loadCtaHandler) {
    const mCta = document.createElement('div');
    mCta.className = 'detail-mobile-cta';
    const mBtn = document.createElement('button');
    mBtn.className = 'popup-load-btn detail-mobile-cta-btn';
    mBtn.textContent = 'Load to editor';
    mBtn.addEventListener('click', panel._loadCtaHandler);
    mCta.appendChild(mBtn);
    content.appendChild(mCta);
  }

  // Raise above browser overlay when opened from browser
  if (fromBrowser) {
    panel.classList.add('above-browser');
    $('detail-backdrop').classList.add('above-browser');
  } else {
    panel.classList.remove('above-browser');
    $('detail-backdrop').classList.remove('above-browser');
  }

  // Position: center by default, shift left if notifications would overlap
  panel.style.left = '';  // reset to CSS default (50%)
  if (!fromBrowser) {
    const notifs = $('notif-stack');
    const hasNotifs = notifs && notifs.children.length > 0;
    if (hasNotifs) {
      // Notification stack: right:16px, width:340px → occupies right 370px
      // Popup uses transform: translate(-50%), so right edge = left + width/2
      // We need popup right edge < viewport - 370px (with 16px gap)
      const vw = window.innerWidth;
      const notifLeft = vw - 370;
      // Estimate popup width (use min 500px as typical)
      const popupW = Math.min(Math.max(panel.scrollWidth || 500, 280), vw - 48);
      const centeredRight = vw / 2 + popupW / 2;
      if (centeredRight > notifLeft - 16) {
        // Shift left so right edge clears notification zone
        const newCenter = notifLeft - 16 - popupW / 2;
        panel.style.left = Math.max(popupW / 2 + 16, newCenter) + 'px';
      }
    }
  }

  panel.classList.add('open');
  $('detail-backdrop').classList.add('open');
  updateCanvasDepth();
}

function closeDetailPanel() {
  $('detail-panel').classList.remove('open');
  $('detail-panel').classList.remove('above-browser');
  $('detail-panel').style.left = '';  // reset position
  $('detail-backdrop').classList.remove('open');
  $('detail-backdrop').classList.remove('above-browser');
  // Remove toast highlight and browsing mode — clear on both stack and sheet body
  $('notif-stack').classList.remove('notif-browsing');
  const _sheetBody = document.getElementById('notif-sheet-body');
  [$('notif-stack'), _sheetBody].forEach(root => {
    if (!root) return;
    root.querySelectorAll('.notif-active').forEach(el => el.classList.remove('notif-active'));
  });
  updateCanvasDepth();
}

// ─── Export ──────────────────────────────────────────────────────────

function generateTikZRaw() {
  const deg = getVertexDegrees();
  const momLabels = getMomentumLabels();
  const showLabels = shouldShowLabels();

  let tikz = '\\begin{tikzpicture}\n';

  // Coordinates
  state.vertices.forEach((v, i) => {
    tikz += `  \\coordinate (v${i+1}) at (${v.x.toFixed(3)}, ${(-v.y).toFixed(3)});\n`;
  });
  tikz += '\n';

  // Edges with styles and colors
  state.edges.forEach((e, i) => {
    const mass = e.mass || 0;
    // Thicken massive edges so they visually pop against massless ones,
    // matching the convention of printed diagrams in the literature.
    let opts = [mass > 0 ? 'ultra thick' : 'thick'];
    // Mid-edge momentum arrow, matching the "Arrows" toggle in the UI.
    // TikZ's `postaction` places an arrow head at ~55 % along the edge
    // without breaking the line, so the result mirrors what the canvas
    // draws.
    if (showArrows && e.a !== e.b) {
      opts.push('postaction={decorate, decoration={markings, mark=at position 0.55 with {\\arrow{Latex[length=2mm]}}}}');
    }
    if (mass > 0) {
      const colors = ['Maroon', 'OliveGreen', 'RoyalBlue', 'Goldenrod', 'Plum', 'BrickRed', 'Teal', 'Brown'];
      opts.push(colors[(mass - 1) % colors.length]);
    }
    if (e.style === 'dashed') opts.push('dashed');
    else if (e.style === 'wavy') opts.push('decorate', 'decoration={snake, amplitude=1.5pt, segment length=5pt}');
    else if (e.style === 'dblwavy') opts.push('double', 'decorate', 'decoration={snake, amplitude=1.5pt, segment length=5pt}');
    else if (e.style === 'gluon' || e.style === 'zigzag') opts.push('decorate', 'decoration={coil, aspect=0.5, segment length=4pt, amplitude=3pt}');

    let edgeLine = `  \\draw[${opts.join(', ')}] (v${e.a+1}) -- (v${e.b+1})`;

    // Edge label (particle label or momentum/number)
    if (e.edgeLabel) {
      edgeLine += ` node[midway, auto] {$${e.edgeLabel}$}`;
    } else if (showLabels && momLabels) {
      edgeLine += ` node[midway, auto, font=\\footnotesize] {$${momLabels[i]}$}`;
    } else if (showLabels) {
      edgeLine += ` node[midway, auto, font=\\footnotesize] {${i + 1}}`;
    }

    const exp = e.propExponent ?? 1;
    if (exp !== 1) edgeLine += ` %% nu=${exp}`;

    tikz += edgeLine + ';\n';
  });
  tikz += '\n';

  // Vertices (internal only get filled dots)
  state.vertices.forEach((_, i) => {
    const isExternal = (deg[i] || 0) <= 1;
    if (!isExternal) {
      tikz += `  \\fill (v${i+1}) circle (2pt);\n`;
    }
  });

  tikz += '\\end{tikzpicture}';
  return tikz;
}

/** Resolve all CSS var() references in an element tree to computed values. */
function resolveVarsInSVG(el) {
  const style = getComputedStyle(document.documentElement);
  const resolve = (val) => {
    if (!val || !val.includes('var(')) return val;
    return val.replace(/var\(--([^)]+)\)/g, (_, name) => {
      return style.getPropertyValue('--' + name).trim() || '';
    });
  };
  // Resolve attributes that commonly use CSS vars
  const attrs = ['fill', 'stroke', 'stop-color', 'flood-color'];
  const walk = (node) => {
    if (node.nodeType !== 1) return;
    attrs.forEach(attr => {
      const v = node.getAttribute(attr);
      if (v && v.includes('var(')) node.setAttribute(attr, resolve(v));
    });
    // Also resolve inline style
    if (node.style) {
      for (let i = 0; i < node.style.length; i++) {
        const prop = node.style[i];
        const v = node.style.getPropertyValue(prop);
        if (v && v.includes('var(')) node.style.setProperty(prop, resolve(v));
      }
    }
    for (const child of node.children) walk(child);
  };
  walk(el);
}

/** Inline computed styles onto SVG <text> labels for standalone rendering.
 *  Needed when SVG is used as data URI <img> (no external CSS). */
function inlineLabelStyles(svgEl) {
  const style = getComputedStyle(document.documentElement);
  const textColor = style.getPropertyValue('--text').trim() || '#3D3428';
  const bgColor = style.getPropertyValue('--bg').trim() || '#FDF9F4';
  svgEl.querySelectorAll('.edge-label-svg').forEach(txt => {
    txt.setAttribute('fill', textColor);
    txt.setAttribute('stroke', bgColor);
    txt.setAttribute('stroke-width', '0.03');
    txt.setAttribute('stroke-linejoin', 'round');
    txt.setAttribute('paint-order', 'stroke fill');
    txt.setAttribute('font-family', "'KaTeX_Math', 'Times New Roman', serif");
    txt.setAttribute('font-style', 'italic');
    txt.setAttribute('font-size', '0.14px');
  });
}

/** Create a clean SVG for export (no grid, transparent background). */
function generateExportSVG() {
  const clone = canvas.cloneNode(true);
  // Remove grid, selection, preview layers
  const grid = clone.querySelector('#grid-layer');
  if (grid) grid.innerHTML = '';
  const sel = clone.querySelector('#selection-layer');
  if (sel) sel.innerHTML = '';
  const prev = clone.querySelector('#preview-layer');
  if (prev) prev.innerHTML = '';
  // Remove edge bubble hit targets and background circles
  clone.querySelectorAll('.edge-bubble circle[fill="transparent"]').forEach(el => el.remove());
  clone.querySelectorAll('.edge-bubble circle').forEach(el => {
    const f = el.getAttribute('fill');
    if (f && f.includes('--bg')) el.remove();
  });
  // Remove edge-label background rects
  clone.querySelectorAll('.edge-label-bg').forEach(el => el.remove());
  // External vertex markers are invisible in the live canvas (CSS
  // opacity:0) — strip them from the clone so they don't appear as
  // dots in the exported PNG/PDF/SVG/TikZ-via-SVG output.
  clone.querySelectorAll('.vertex-external').forEach(el => el.remove());
  // Resolve CSS variables so the SVG renders standalone
  resolveVarsInSVG(clone);
  inlineLabelStyles(clone);
  // Transparent background
  clone.style.background = 'transparent';
  clone.removeAttribute('style');
  // Tight bounding box
  if (state.vertices.length > 0) {
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    state.vertices.forEach(v => {
      minX = Math.min(minX, v.x); maxX = Math.max(maxX, v.x);
      minY = Math.min(minY, v.y); maxY = Math.max(maxY, v.y);
    });
    const pad = 0.8;
    clone.setAttribute('viewBox', `${minX - pad} ${minY - pad} ${maxX - minX + 2*pad} ${maxY - minY + 2*pad}`);
  }
  return new XMLSerializer().serializeToString(clone);
}

/** Generate a clean diagram preview SVG for the integration panel.
 *  Similar to generateExportSVG but also hides external vertex dots
 *  and uses tighter cropping based only on internal vertices. */
function generateDiagramPreview() {
  const clone = canvas.cloneNode(true);
  // Remove grid, selection, preview
  ['#grid-layer', '#selection-layer', '#preview-layer'].forEach(sel => {
    const el = clone.querySelector(sel);
    if (el) el.innerHTML = '';
  });
  // Remove edge bubble hit targets and bg circles
  clone.querySelectorAll('.edge-bubble circle[fill="transparent"]').forEach(el => el.remove());
  clone.querySelectorAll('.edge-bubble circle').forEach(el => {
    const f = el.getAttribute('fill');
    if (f && f.includes('--bg')) el.remove();
  });
  clone.querySelectorAll('.edge-label-bg').forEach(el => el.remove());
  // Remove external vertex dots
  clone.querySelectorAll('.vertex-external').forEach(el => el.remove());

  resolveVarsInSVG(clone);
  inlineLabelStyles(clone);
  clone.style.background = 'transparent';
  clone.removeAttribute('style');

  // Tight bounding box: use only internal vertices for centering,
  // but include external vertex positions for full extent
  if (state.vertices.length > 0) {
    const deg = getVertexDegrees();
    let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
    state.vertices.forEach((v, i) => {
      // Include all vertices for bounding, but weight internal ones
      minX = Math.min(minX, v.x); maxX = Math.max(maxX, v.x);
      minY = Math.min(minY, v.y); maxY = Math.max(maxY, v.y);
    });
    const pad = 0.6;
    clone.setAttribute('viewBox', `${minX - pad} ${minY - pad} ${maxX - minX + 2*pad} ${maxY - minY + 2*pad}`);
  }
  return new XMLSerializer().serializeToString(clone);
}

function exportPNG() {
  const svgStr = generateExportSVG();
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = 800; c.height = 600;
    const ctx = c.getContext('2d');
    // Transparent background — no fillRect
    ctx.drawImage(img, 0, 0, c.width, c.height);
    c.toBlob(blob => {
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a'); a.href = url; a.download = 'diagram.png'; a.click();
      URL.revokeObjectURL(url);
    });
  };
  img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svgStr);
}

async function exportPDF() {
  // Minimal single-page PDF with the diagram as a high-res embedded image
  const svgStr = generateExportSVG();
  const img = await new Promise(resolve => {
    const i = new Image();
    i.onload = () => resolve(i);
    i.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svgStr);
  });
  {
    const w = 2400, h = 1600;
    const c = document.createElement('canvas');
    c.width = w; c.height = h;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0, w, h);

    // Extract raw RGBA pixel data for PNG-style embedding in PDF
    const rawData = ctx.getImageData(0, 0, w, h).data;
    // Separate RGB and alpha channels for PDF (SMask for transparency)
    const rgbData = new Uint8Array(w * h * 3);
    const alphaData = new Uint8Array(w * h);
    let hasAlpha = false;
    for (let i = 0, j = 0, k = 0; i < rawData.length; i += 4) {
      rgbData[j++] = rawData[i];
      rgbData[j++] = rawData[i+1];
      rgbData[j++] = rawData[i+2];
      alphaData[k] = rawData[i+3];
      if (alphaData[k] < 255) hasAlpha = true;
      k++;
    }

    // Use DeflateRaw (pako-free: use canvas JPEG as fallback if no compression API)
    async function deflate(data) {
      if (typeof CompressionStream !== 'undefined') {
        const cs = new CompressionStream('deflate');
        const writer = cs.writable.getWriter();
        writer.write(data);
        writer.close();
        const chunks = [];
        const reader = cs.readable.getReader();
        while (true) {
          const { done, value } = await reader.read();
          if (done) break;
          chunks.push(value);
        }
        const total = chunks.reduce((s, c) => s + c.length, 0);
        const result = new Uint8Array(total);
        let offset = 0;
        for (const chunk of chunks) { result.set(chunk, offset); offset += chunk.length; }
        return result;
      }
      return null;  // fallback: use uncompressed
    }

    const rgbCompressed = await deflate(rgbData);
    const alphaCompressed = hasAlpha ? await deflate(alphaData) : null;

    // PDF structure
    const pW = 612, pH = 408; // points (8.5 x 5.67 in, ~3:2 aspect)
    const parts = [];  // binary-safe parts
    // Required PDF header. The second line is a 4-byte binary marker that
    // signals "this file contains binary data" to transport tools, per
    // PDF 32000-1:2008 §7.5.2 — without it some readers treat the file as
    // text, which breaks image streams.
    parts.push(new Uint8Array([0x25, 0x50, 0x44, 0x46, 0x2d, 0x31, 0x2e, 0x34, 0x0a,
                                 0x25, 0xe2, 0xe3, 0xcf, 0xd3, 0x0a]));
    let objCount = 0;
    const xref = [];

    function addObjBin(headerStr, streamData) {
      const n = ++objCount;
      const totalLen = parts.reduce((s, p) => s + p.length, 0);
      xref.push(totalLen);
      const header = new TextEncoder().encode(`${n} 0 obj\n${headerStr}\nstream\n`);
      const footer = new TextEncoder().encode(`\nendstream\nendobj\n`);
      parts.push(header, streamData, footer);
      return n;
    }
    function addObj(content) {
      const n = ++objCount;
      const totalLen = parts.reduce((s, p) => s + p.length, 0);
      xref.push(totalLen);
      parts.push(new TextEncoder().encode(`${n} 0 obj\n${content}\nendobj\n`));
      return n;
    }

    // Fixed object layout (keeps the Pages /Kids reference to `3 0 R`
    // stable regardless of whether the image carries an SMask alpha
    // channel):
    //   1 Catalog   2 Pages   3 Page   4 Image   5 Content   6 SMask (optional)
    const pageNum = 3, imageNum = 4, contentNum = 5;
    const smaskNum = (hasAlpha && alphaCompressed) ? 6 : null;

    addObj(`<< /Type /Catalog /Pages 2 0 R >>`);
    addObj(`<< /Type /Pages /Kids [${pageNum} 0 R] /Count 1 >>`);
    addObj(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${pW} ${pH}] /Contents ${contentNum} 0 R /Resources << /XObject << /Img ${imageNum} 0 R >> >> >>`);

    const imgFilter = rgbCompressed ? '/FlateDecode' : '';
    const imgStreamData = rgbCompressed || rgbData;
    const smaskRef = smaskNum ? ` /SMask ${smaskNum} 0 R` : '';
    addObjBin(
      `<< /Type /XObject /Subtype /Image /Width ${w} /Height ${h} /ColorSpace /DeviceRGB /BitsPerComponent 8${imgFilter ? ' /Filter ' + imgFilter : ''} /Length ${imgStreamData.length}${smaskRef} >>`,
      imgStreamData
    );

    // Content stream: draw the image scaled to page
    const drawCmd = `q ${pW} 0 0 ${pH} 0 0 cm /Img Do Q`;
    addObj(`<< /Length ${drawCmd.length} >>\nstream\n${drawCmd}\nendstream`);

    if (smaskNum !== null) {
      addObjBin(
        `<< /Type /XObject /Subtype /Image /Width ${w} /Height ${h} /ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode /Length ${alphaCompressed.length} >>`,
        alphaCompressed
      );
    }

    // Build xref and trailer
    const enc = new TextEncoder();
    const xrefOffset = parts.reduce((s, p) => s + p.length, 0);
    let xrefStr = `xref\n0 ${objCount + 1}\n0000000000 65535 f \n`;
    xref.forEach(off => { xrefStr += off.toString().padStart(10, '0') + ' 00000 n \n'; });
    xrefStr += `trailer\n<< /Size ${objCount + 1} /Root 1 0 R >>\nstartxref\n${xrefOffset}\n%%EOF`;
    parts.push(enc.encode(xrefStr));

    const blob = new Blob(parts, { type: 'application/pdf' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a'); a.href = url; a.download = 'diagram.pdf'; a.click();
    URL.revokeObjectURL(url);
  }
}

function exportTikZ() {
  const tikz = generateTikZRaw();
  const latex = `\\documentclass[border=5pt]{standalone}
\\usepackage{tikz}
\\usepackage[dvipsnames]{xcolor}
\\usetikzlibrary{arrows.meta, decorations.markings, decorations.pathmorphing}

\\begin{document}
${tikz}
\\end{document}`;
  const blob = new Blob([latex], { type: 'application/x-tex' });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a'); a.href = url; a.download = 'diagram.tex'; a.click();
  URL.revokeObjectURL(url);
}

// ─── Browser ─────────────────────────────────────────────────────────

// ─── About ──────────────────────────────────────────────────────────

function openAbout() {
  const body = $('about-body');
  // Compute live stats from library
  let nTopos = 0, nConfigs = 0, nRecords = 0, nRefs = 0, nLocal = 0, nResults = 0;
  const refSet = new Set();
  const scatterPoints = [];  // {loops, legs} for each config
  if (library && library.topologies) {
    const topos = library.topologies;
    for (const key in topos) {
      nTopos++;
      const loops = topos[key].loops ?? topos[key].L ?? 0;
      const legs = topos[key].legs ?? 0;
      const configs = topos[key].configs || {};
      for (const ck in configs) {
        nConfigs++;
        const resultsList = configs[ck].results||configs[ck].Results||[];
        const hasResults = resultsList.length > 0;
        nResults += resultsList.length;
        scatterPoints.push({ loops, legs, computed: hasResults });
        const src = configs[ck].source || configs[ck].Source || '';
        if (src === 'SubTropica') nLocal++;
        const recs = configs[ck].Records || configs[ck].records || [];
        nRecords += recs.length;
        recs.forEach(r => {
          const ref = r.Reference || r.reference || '';
          if (ref) refSet.add(ref);
        });
      }
    }
  }
  // Read papersScanned from _meta (populated from _stats.json via library.json builder)
  nRefs = (library && library._meta && library._meta.papersScanned) || 770;
  const coconuts = parseInt(localStorage.getItem('subtropica-coconuts') || '0');

  const isFull = backendMode === 'full';
  const lastSync = isFull ? (localStorage.getItem('subtropica-last-sync') || 'never') : '';

  // Match the top-toolbar badge: "Mobile" on phone widths, "Online" elsewhere;
  // no badge in Full (Mathematica-backed) mode.
  const isMobileBadge = typeof matchMedia === 'function' && matchMedia('(max-width: 768px)').matches;
  const titleBadge = isFull ? '' : ` <span class="title-badge">${isMobileBadge ? 'Mobile' : 'Online'}</span>`;
  const subtitle = isFull
    ? `v${ST_VERSION}`
    : `v${ST_VERSION} &mdash; Feynman diagram editor &amp; integral database`;
  const desc = isFull
    ? '<div class="about-desc">A Mathematica package for evaluating Euler integrals via tropical geometry.</div>'
    : '';

  // Stats: shown in all modes
  let statsHTML = `
    <div class="about-stat"><div class="about-stat-val">${nTopos}</div><div class="about-stat-label">topologies</div></div>
    <div class="about-stat"><div class="about-stat-val">${nConfigs}</div><div class="about-stat-label">diagrams</div></div>
    <div class="about-stat"><div class="about-stat-val">${nRefs}</div><div class="about-stat-label">papers scanned</div></div>
    <div class="about-stat"><div class="about-stat-val">${nRecords}</div><div class="about-stat-label">records</div></div>
    <div class="about-stat"><div class="about-stat-val">${nResults}</div><div class="about-stat-label">results</div></div>
    <div class="about-stat"><div class="about-stat-val">${isFull ? coconuts : _sessionSalt}</div><div class="about-stat-label"><span class="mascot-emoji">${mascotEmoji()}</span> earned</div></div>
  `;

  let footerHTML = 'Data sourced from <a href="https://arxiv.org" target="_blank" rel="noopener">arXiv</a>, <a href="https://inspirehep.net" target="_blank" rel="noopener">INSPIRE-HEP</a>, <a href="https://loopedia.mpp.mpg.de" target="_blank" rel="noopener">Loopedia</a>, and <a href="https://qcdloop.fnal.gov" target="_blank" rel="noopener">QCDLoop</a>.<br/>Library data licensed under <a href="https://creativecommons.org/licenses/by-nc-sa/4.0/" target="_blank" rel="noopener">CC BY-NC-SA 4.0</a>.';
  if (isFull) footerHTML = `Last sync: ${lastSync}<br/>` + footerHTML;

  body.innerHTML = `
    <div class="about-coconut"><span class="mascot-emoji">${mascotEmoji()}</span></div>
    <div class="about-title"><span class="title-accent">Sub</span>Tropica${titleBadge}</div>
    <div class="about-subtitle">${subtitle}</div>
    ${desc}
    <div class="about-authors">Mathieu Giroux, Sebastian Mizera, Giulio Salvatori</div>
    <div class="about-stats">${statsHTML}</div>
    <div class="about-scatter-wrap">
      <div class="about-scatter-title">Library coverage</div>
      <canvas id="about-scatter" width="360" height="200"></canvas>
    </div>
    <div class="about-footer">${footerHTML}</div>
  `;
  requestAnimationFrame(() => drawScatterPlot('about-scatter', scatterPoints));
  $('about-overlay').classList.add('visible');
}

function drawScatterPlot(canvasId, points) {
  const canvas = $(canvasId);
  if (!canvas) return;
  const ctx = canvas.getContext('2d');
  const W = canvas.width, H = canvas.height;
  const pad = { top: 10, right: 15, bottom: 28, left: 32 };

  // Compute axis ranges
  const maxLoops = Math.max(1, ...points.map(p => p.loops));
  const maxLegs = Math.max(2, ...points.map(p => p.legs));

  const plotW = W - pad.left - pad.right;
  const plotH = H - pad.top - pad.bottom;

  function toX(legs) { return pad.left + (legs - 0) / (maxLegs + 1) * plotW; }
  function toY(loops) { return pad.top + plotH - (loops - 0) / (maxLoops + 1) * plotH; }

  // Count total and computed points at each (loops, legs)
  const counts = {};
  const computed = {};
  for (const p of points) {
    const k = p.loops + ',' + p.legs;
    counts[k] = (counts[k] || 0) + 1;
    if (p.computed) computed[k] = (computed[k] || 0) + 1;
  }

  // Background
  ctx.clearRect(0, 0, W, H);

  // Grid lines
  ctx.strokeStyle = getComputedStyle(document.body).getPropertyValue('--border') || '#e0d8cf';
  ctx.lineWidth = 0.5;
  for (let l = 0; l <= maxLoops; l++) {
    ctx.beginPath(); ctx.moveTo(pad.left, toY(l)); ctx.lineTo(W - pad.right, toY(l)); ctx.stroke();
  }
  for (let g = 0; g <= maxLegs; g++) {
    ctx.beginPath(); ctx.moveTo(toX(g), pad.top); ctx.lineTo(toX(g), H - pad.bottom); ctx.stroke();
  }

  // Axis labels
  const textColor = getComputedStyle(document.body).getPropertyValue('--text-muted') || '#999';
  ctx.fillStyle = textColor;
  ctx.font = '9px system-ui, sans-serif';
  ctx.textAlign = 'center';
  for (let g = 0; g <= maxLegs; g++) {
    ctx.fillText(g, toX(g), H - pad.bottom + 14);
  }
  ctx.fillText('legs', pad.left + plotW / 2, H - 2);
  ctx.textAlign = 'right';
  for (let l = 0; l <= maxLoops; l++) {
    ctx.fillText(l, pad.left - 5, toY(l) + 3);
  }
  ctx.save();
  ctx.translate(8, pad.top + plotH / 2);
  ctx.rotate(-Math.PI / 2);
  ctx.textAlign = 'center';
  ctx.fillText('loops', 0, 0);
  ctx.restore();

  // Draw bubbles (circles first, then labels on top so numbers aren't occluded)
  const accent = getComputedStyle(document.body).getPropertyValue('--accent') || '#8b5c2a';
  for (const k in counts) {
    const [l, g] = k.split(',').map(Number);
    const n = counts[k];
    const r = Math.min(2 + Math.sqrt(n) * 2.5, 14);
    ctx.beginPath();
    ctx.arc(toX(g), toY(l), r, 0, 2 * Math.PI);
    ctx.fillStyle = accent;
    ctx.globalAlpha = 0.55;
    ctx.fill();
    ctx.globalAlpha = 1;
    ctx.strokeStyle = accent;
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  // Overlay: computed entries as smaller maroon filled circles
  const maroon = getComputedStyle(document.body).getPropertyValue('--accent') || '#A84545';
  for (const k in computed) {
    const [l, g] = k.split(',').map(Number);
    const r = Math.min(2 + Math.sqrt(computed[k]) * 2, 10);
    ctx.beginPath();
    ctx.arc(toX(g), toY(l), r, 0, 2 * Math.PI);
    ctx.fillStyle = maroon;
    ctx.globalAlpha = 0.9;
    ctx.fill();
    ctx.globalAlpha = 1;
  }

  // Draw count labels last (on top of all circles)
  ctx.fillStyle = '#fff';
  ctx.font = 'bold 8px system-ui, sans-serif';
  ctx.textAlign = 'center';
  for (const k in counts) {
    const [l, g] = k.split(',').map(Number);
    const n = counts[k];
    const r = Math.min(2 + Math.sqrt(n) * 2.5, 14);
    if (n > 1 && r > 5) {
      ctx.fillText(n, toX(g), toY(l) + 3);
    }
  }
}

function closeAbout() {
  $('about-overlay').classList.remove('visible');
}

// ─── Correction-report modal ────────────────────────────────────────
//
// The modal is a pure-client form that POSTs to the Cloudflare Worker's
// /correction route.  The Worker triggers correction.yml, which opens a
// GitHub issue on SubTropica/SubTropica.  No kernel required — works in
// both the deployed static UI and in the local Mathematica-backed UI.

const CORRECTION_CONTRIB_KEY = 'subtropica-correction-contributor';

// Gag template for the "Citation request" category.  Pre-filled into
// the comment textarea when the user selects that category; cleared
// again if they switch to another category without editing.
const CITATION_REQUEST_TEMPLATE =
`Dear authors,

I've been reading your paper with great interest. However, I can't help but notice that you missed to include citations to my seminal papers:
[List at least 5 of your papers here]
[Preferably, with BibTeX entries ready to copy-paste]

It would be wonderful if you could update your paper with these citations at your earliest opportunity.

Best regards,
[Your name]
`;

let _correctionCtx = null;  // { cnickelIndex, recordId, arxivId, canonicalName, record }

function openCorrectionModal(ctx) {
  _correctionCtx = ctx || {};
  const overlay = $('correction-overlay');
  if (!overlay) return;

  // Populate the read-only target block
  const target = $('correction-target');
  const bits = [];
  if (ctx.canonicalName) {
    bits.push(`<div class="correction-target-row"><b>Name:</b> ${escapeHtml(ctx.canonicalName)}</div>`);
  }
  bits.push(`<div class="correction-target-row"><b>CNI:</b> <code>${escapeHtml(ctx.cnickelIndex || '—')}</code></div>`);
  if (ctx.recordId) {
    bits.push(`<div class="correction-target-row"><b>Record:</b> <code>${escapeHtml(ctx.recordId)}</code></div>`);
  }
  if (ctx.arxivId) {
    bits.push(`<div class="correction-target-row"><b>arXiv:</b> <a href="https://arxiv.org/abs/${encodeURIComponent(ctx.arxivId)}" target="_blank" rel="noopener">${escapeHtml(ctx.arxivId)}</a></div>`);
  }
  target.innerHTML = bits.join('');

  // Reset form fields
  $('correction-category').value = 'misidentified-diagram';
  $('correction-comment').value = '';
  $('correction-proposed').value = '';
  $('correction-comment-count').textContent = '0';
  $('correction-proposed-count').textContent = '0';
  $('correction-status').textContent = '';
  $('correction-status').className = 'correction-status';
  $('correction-submit').disabled = true;
  $('correction-submit').textContent = 'Submit';

  // Contributor: restore from localStorage.  Default is anonymous.
  let storedName = '';
  let storedAnon = true;
  try {
    const raw = localStorage.getItem(CORRECTION_CONTRIB_KEY);
    if (raw) {
      const parsed = JSON.parse(raw);
      storedName = parsed.name || '';
      storedAnon = parsed.anonymous !== false;
    }
  } catch {}
  const nameInput = $('correction-contributor');
  const anonCb = $('correction-anon');
  nameInput.value = storedName;
  anonCb.checked = storedAnon;
  nameInput.disabled = storedAnon;
  nameInput.placeholder = storedAnon ? 'Anonymous' : 'Your name';

  overlay.classList.add('visible');
  setTimeout(() => $('correction-comment').focus(), 50);
}

function closeCorrectionModal() {
  $('correction-overlay').classList.remove('visible');
  _correctionCtx = null;
}

function updateCorrectionSubmitState() {
  const comment = $('correction-comment').value.trim();
  $('correction-submit').disabled = comment.length < 10;
}

async function submitCorrection() {
  if (!_correctionCtx) return;
  const btn = $('correction-submit');
  const status = $('correction-status');
  const category = $('correction-category').value;
  const comment = $('correction-comment').value.trim();
  const proposed = $('correction-proposed').value.trim();
  const anon = $('correction-anon').checked;
  const name = $('correction-contributor').value.trim();
  const contributor = anon || !name ? 'anonymous' : name;

  // Persist the contributor choice so the user doesn't re-type on the
  // next report.  Anonymous mode still remembers the name, so unchecking
  // the box restores it.
  try {
    localStorage.setItem(CORRECTION_CONTRIB_KEY, JSON.stringify({
      name,
      anonymous: anon,
    }));
  } catch {}

  const payload = {
    cnickelIndex:  _correctionCtx.cnickelIndex,
    recordId:      _correctionCtx.recordId,
    arxivId:       _correctionCtx.arxivId || '',
    canonicalName: _correctionCtx.canonicalName || '',
    category,
    comment,
    proposedValue: proposed,
    contributor,
    uiVersion:     (typeof ST_VERSION !== 'undefined' ? ST_VERSION : ''),
    timestamp:     new Date().toISOString(),
  };

  btn.disabled = true;
  btn.textContent = 'Submitting…';
  status.textContent = '';
  status.className = 'correction-status';

  try {
    const r = await fetch(SUBMIT_WORKER_BASE + '/correction', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
    });
    const data = await r.json();
    if (r.ok && data.status === 'ok') {
      status.textContent = 'Thanks — we’ll review shortly.';
      status.className = 'correction-status ok';
      btn.textContent = '✓ Submitted';
      setTimeout(closeCorrectionModal, 1400);
    } else if (data.status === 'duplicate') {
      status.textContent = 'An identical report was already submitted.';
      status.className = 'correction-status ok';
      btn.textContent = 'Already reported';
      setTimeout(closeCorrectionModal, 1600);
    } else {
      status.textContent = 'Error: ' + (data.error || ('HTTP ' + r.status));
      status.className = 'correction-status error';
      btn.disabled = false;
      btn.textContent = 'Submit';
    }
  } catch (e) {
    status.textContent = 'Network error — please try again.';
    status.className = 'correction-status error';
    btn.disabled = false;
    btn.textContent = 'Submit';
  }
}

// ─── Browser ─────────────────────────────────────────────────────────

let _browserTab = localStorage.getItem('subtropica-browser-tab') || 'topos';

function openBrowser() {
  if (!library || !library.topologies) return;
  // Show sync button only in full (Mathematica) mode
  const syncBtn = $('browser-sync-btn');
  if (syncBtn) syncBtn.style.display = backendMode === 'full' ? '' : 'none';
  // Restore saved tab
  $('browser-tab-topos').classList.toggle('active', _browserTab === 'topos');
  $('browser-tab-diagrams').classList.toggle('active', _browserTab === 'diagrams');
  $('browser-overlay').classList.add('visible');
  populateBrowser();
}

function closeBrowser() {
  $('browser-overlay').classList.remove('visible');
}

function switchBrowserTab(tab) {
  _browserTab = tab;
  localStorage.setItem('subtropica-browser-tab', tab);
  $('browser-tab-topos').classList.toggle('active', tab === 'topos');
  $('browser-tab-diagrams').classList.toggle('active', tab === 'diagrams');
  populateBrowser();
}

function populateBrowser() {
  const body = $('browser-body');

  // A topology is waitlisted iff ALL of its configs are waitlisted — if any
  // config is public, the topology is too (the waitlisted configs will be
  // filtered out individually when rendered in the diagrams tab).
  const topoIsFullyWaitlisted = (key, t) => {
    const cfgKeys = Object.keys(t.configs || {});
    if (cfgKeys.length === 0) return false;
    return cfgKeys.every(ck => _waitlistConfigSet.has(waitlistKey(key, ck)));
  };

  // Build topology list
  const topos = [];
  for (const key in library.topologies) {
    const t = library.topologies[key];
    const hasResults = Object.values(t.configs||{}).some(c => (c.results||c.Results||[]).length > 0);
    const isLocal = Object.values(t.configs||{}).some(c => (c.source||c.Source||'') === 'SubTropica');
    // Collect all diagram names/aliases under this topology for search
    const _diagramNames = [];
    for (const ck in (t.configs||{})) {
      const cn = t.configs[ck].Names || t.configs[ck].names || [];
      (Array.isArray(cn) ? cn : [cn]).forEach(n => { if (n) _diagramNames.push(n.toLowerCase()); });
      const cName = t.configs[ck].canonicalName || t.configs[ck].CanonicalName;
      if (cName) _diagramNames.push(cName.toLowerCase());
    }
    const isWaitlisted = topoIsFullyWaitlisted(key, t);
    topos.push({ key, name: t.primaryName||t.name||t.Name||key, loops: t.loops??t.L??0, legs: t.legs??0, props: t.props??0, topo: t, configs: Object.keys(t.configs||{}).length, hasResults, isLocal, isWaitlisted, _diagramNames });
  }

  // Classify function class into canonical categories
  function classifyFC(fc) {
    if (!fc || fc === 'None' || fc === 'unknown' || fc === 'Unknown') return null;
    const KEYWORDS = [
      [/Calabi[–\-]?Yau/i, 'Calabi-Yau'],
      [/elliptic/i, 'elliptic'],
      [/hypergeometric/i, 'hypergeometric'],
      [/MZV/, 'MPL'], [/MPL/, 'MPL'], [/logarithm/i, 'MPL'],
      [/mixed/i, 'mixed'],
      [/rational|Gamma/i, 'rational'],
    ];
    for (const [re, label] of KEYWORDS) { if (re.test(fc)) return label; }
    return 'other';
  }

  // Build diagram (config) list
  const diagrams = [];
  for (const key in library.topologies) {
    const t = library.topologies[key];
    const configs = t.configs || {};
    for (const ck in configs) {
      const cfg = configs[ck];
      const cfgNames = cfg.Names || cfg.names || [];
      const canonical = cfg.canonicalName || cfg.CanonicalName || '';
      const name = canonical || (cfgNames.length > 0 ? (Array.isArray(cfgNames) ? cfgNames[0] : cfgNames) : ck);
      const source = cfg.source || cfg.Source || '';
      const fc = cfg.FunctionClass || cfg.functionClass || '';
      diagrams.push({
        topoKey: key, configKey: ck, topo: t, cfg,
        name, topoName: t.primaryName||t.name||t.Name||key,
        loops: t.loops??t.L??0, legs: t.legs??0, props: t.props??0,
        source,
        isLocal: hasSource(cfg, 'SubTropica'),
        isWaitlisted: _waitlistConfigSet.has(waitlistKey(key, ck)),
        massScales: cfg.MassScales ?? cfg.massScales ?? null,
        fcClass: classifyFC(fc),
        hasResults: (cfg.results||cfg.Results||[]).length > 0,
        compLevel: (() => { const recs = cfg.Records||cfg.records||[]; for (const r of recs) { const cl = r.computationLevel||r.computation_level; if (cl) return cl.replace(/_/g,' '); } return null; })(),
      });
    }
  }

  const items = _browserTab === 'topos' ? topos : diagrams;

  // ── Unified chip filter builder ──
  // Each filter: { groupEl, activeSet, values[], formatter? }
  const filters = {};

  function buildChipGroup(id, values, formatter) {
    const group = $(id);
    const active = new Set();
    const storageKey = 'subtropica-filter-' + id;
    group.innerHTML = '';
    if (values.length === 0) { group.style.display = 'none'; return { active, group }; }
    group.style.display = '';

    // Restore saved selection
    try {
      const saved = JSON.parse(localStorage.getItem(storageKey) || '[]');
      saved.forEach(v => { if (values.includes(v)) active.add(v); });
    } catch(e) {}

    function saveActive() { localStorage.setItem(storageKey, JSON.stringify([...active])); }

    // "All" chip
    const allChip = document.createElement('span');
    allChip.className = 'browser-chip' + (active.size === 0 ? ' active' : '');
    allChip.textContent = 'All';
    allChip.addEventListener('click', () => {
      active.clear();
      saveActive();
      group.querySelectorAll('.browser-chip').forEach(c => c.classList.remove('active'));
      allChip.classList.add('active');
      // Clear any stuck height animation before re-rendering
      body.style.maxHeight = '';
      body.style.overflow = '';
      body.style.transition = '';
      renderList();
    });
    group.appendChild(allChip);
    // Value chips
    values.forEach(v => {
      const chip = document.createElement('span');
      chip.className = 'browser-chip' + (active.has(v) ? ' active' : '');
      chip.textContent = formatter ? formatter(v) : String(v);
      chip.addEventListener('click', () => {
        if (active.has(v)) { active.delete(v); chip.classList.remove('active'); }
        else { active.add(v); chip.classList.add('active'); }
        saveActive();
        // Toggle "All" chip
        allChip.classList.toggle('active', active.size === 0);
        renderList();
      });
      group.appendChild(chip);
    });
    return { active, group };
  }

  const loopVals = [...new Set(items.map(t=>t.loops))].sort((a,b)=>a-b);
  const legVals = [...new Set(items.map(t=>t.legs))].sort((a,b)=>a-b);
  filters.loops = buildChipGroup('browser-filter-loops', loopVals);
  filters.legs = buildChipGroup('browser-filter-legs', legVals);

  // Precomputed toggle — restore from localStorage, default on for full mode
  const precomputedCb = $('browser-precomputed-cb');
  if (precomputedCb) {
    const savedPre = localStorage.getItem('subtropica-filter-precomputed');
    precomputedCb.checked = savedPre !== null ? savedPre === '1' : (backendMode === 'full');
    precomputedCb.onchange = () => {
      localStorage.setItem('subtropica-filter-precomputed', precomputedCb.checked ? '1' : '0');
      renderList();
    };
  }

  if (_browserTab === 'diagrams') {
    const fcVals = [...new Set(diagrams.map(d=>d.fcClass).filter(Boolean))].sort();
    const msVals = [...new Set(diagrams.map(d=>d.massScales).filter(v=>v!=null))].sort((a,b)=>a-b);
    filters.fc = buildChipGroup('browser-filter-fc', fcVals);
    filters.ms = buildChipGroup('browser-filter-ms', msVals, v => `${v}`);
    $('browser-filter-fc').style.display = fcVals.length > 0 ? '' : 'none';
    $('browser-filter-ms').style.display = msVals.length > 0 ? '' : 'none';
  } else {
    $('browser-filter-fc').style.display = 'none';
    $('browser-filter-ms').style.display = 'none';
  }
  // Legacy "Verified:" chip group placeholder — superseded by the waitlist
  // visibility model. Hide unconditionally.
  $('browser-filter-verified').style.display = 'none';

  // "Local only" toggle — visible only in full (Mathematica) mode
  const localFilter = $('browser-filter-local');
  if (localFilter) {
    localFilter.style.display = backendMode === 'full' ? '' : 'none';
    const localCb = $('browser-local-cb');
    if (localCb) {
      const savedLocal = localStorage.getItem('subtropica-filter-local');
      if (savedLocal !== null) localCb.checked = savedLocal === '1';
      localCb.onchange = () => {
        localStorage.setItem('subtropica-filter-local', localCb.checked ? '1' : '0');
        renderList();
      };
    }
  }

  const search = $('browser-search');
  const searchClear = $('browser-search-clear');
  // Pre-fill from a ?q=... URL param (e.g. notebooks deep-linking by
  // CNickelIndex). URL param wins over the sticky localStorage value;
  // clear the param so it doesn't leak into subsequent session state.
  const urlQ = new URLSearchParams(window.location.search).get('q');
  if (urlQ) {
    search.value = urlQ;
    const u = new URL(window.location.href);
    u.searchParams.delete('q');
    history.replaceState(null, '', u.toString());
  } else {
    search.value = localStorage.getItem('subtropica-filter-search') || '';
  }
  if (searchClear) searchClear.style.display = search.value ? '' : 'none';
  search.addEventListener('input', () => {
    localStorage.setItem('subtropica-filter-search', search.value);
    if (searchClear) searchClear.style.display = search.value ? '' : 'none';
  });
  if (searchClear) searchClear.addEventListener('click', () => {
    search.value = '';
    localStorage.setItem('subtropica-filter-search', '');
    searchClear.style.display = 'none';
    search.dispatchEvent(new Event('input'));
    search.focus();
  });

  function renderList() {
    // Animate height change
    const oldH = body.scrollHeight;

    _renderListInner();

    // After DOM update, animate from old height to new
    requestAnimationFrame(() => {
      const newH = body.scrollHeight;
      if (oldH !== newH && oldH > 0) {
        body.style.transition = 'none';
        body.style.maxHeight = oldH + 'px';
        body.style.overflow = 'hidden';
        requestAnimationFrame(() => {
          body.style.transition = 'max-height 0.5s cubic-bezier(0.4, 0, 0.2, 1)';
          body.style.maxHeight = newH + 'px';
          setTimeout(() => { body.style.maxHeight = ''; body.style.overflow = ''; body.style.transition = ''; }, 550);
        });
      }
    });
  }

  function _renderListInner() {
    const sf = search.value.toLowerCase().trim();
    const fLoops = filters.loops.active;
    const fLegs = filters.legs.active;
    const fFC = filters.fc ? filters.fc.active : new Set();
    const fMS = filters.ms ? filters.ms.active : new Set();
    const precompOnly = $('browser-precomputed-cb')?.checked || false;
    const localOnly = $('browser-local-cb')?.checked || false;
    // Review mode (?review=1) shows waitlisted entries (highlighted blue);
    // public mode hides them entirely. No user toggle for this — it's a
    // tier boundary, not a preference.
    const isReviewMode = new URLSearchParams(window.location.search).get('review') === '1';

    if (_browserTab === 'topos') {
      const filtered = topos.filter(t => {
        if (precompOnly && !t.hasResults) return false;
        if (localOnly && !t.isLocal) return false;
        if (!isReviewMode && t.isWaitlisted) return false;
        if (fLoops.size > 0 && !fLoops.has(t.loops)) return false;
        if (fLegs.size > 0 && !fLegs.has(t.legs)) return false;
        if (sf && !t.name.toLowerCase().includes(sf) && !t.key.toLowerCase().includes(sf) && !(t._diagramNames && t._diagramNames.some(n => n.includes(sf)))) return false;
        return true;
      });
      filtered.sort((a, b) => (a.loops - b.loops) || (a.legs - b.legs) || (a.props - b.props));
      $('browser-count').textContent = filtered.length + ' topolog' + (filtered.length!==1?'ies':'y');
      body.innerHTML = '';
      if (filtered.length === 0) {
        body.innerHTML = '<div style="padding:32px;text-align:center;color:var(--text-muted)">No matches.</div>';
        return;
      }
      const list = document.createElement('div'); list.className = 'browser-list';
      filtered.forEach(t => {
        try {
        const card = document.createElement('div');
        card.className = 'browser-toast browser-toast-topo' + (t.isWaitlisted ? ' browser-toast-waitlisted' : '');
        const thumb = generateThumbnail(t.key, null);
        thumb.classList.add('notif-thumb');
        const cardBody = document.createElement('div');
        cardBody.className = 'notif-body';
        const resultStar = t.hasResults ? '<span class="result-star" title="Result computed">\u2605</span> ' : '';
        cardBody.innerHTML = `
          <div class="notif-title">${resultStar}${t.name}</div>
          <div class="notif-stats">
            <span><span class="notif-stat-val">${t.loops}</span> loop${t.loops !== 1 ? 's' : ''}</span>
            <span><span class="notif-stat-val">${t.legs}</span> leg${t.legs !== 1 ? 's' : ''}</span>
            <span><span class="notif-stat-val">${t.props}</span> prop${t.props !== 1 ? 's' : ''}</span>
          </div>
        `;
        card.appendChild(thumb);
        card.appendChild(cardBody);
        card.addEventListener('click', () => {
          // If topology has exactly one config, open it directly (like diagram click)
          const cfgKeys = Object.keys(t.topo.configs || {});
          if (cfgKeys.length === 1) {
            openDetailPanel(t.key, t.topo, {[cfgKeys[0]]: 'compatible'}, cfgKeys[0], { fromBrowser: true });
          } else {
            openDetailPanel(t.key, t.topo, {}, null, { fromBrowser: true });
          }
        });
        list.appendChild(card);
        } catch (e) { console.warn('Thumbnail failed for', t.key, e); }
      });
      body.appendChild(list);
    } else {
      // Diagrams tab
      const filtered = diagrams.filter(d => {
        if (precompOnly && !d.hasResults) return false;
        if (localOnly && !d.isLocal) return false;
        if (!isReviewMode && d.isWaitlisted) return false;
        if (fLoops.size > 0 && !fLoops.has(d.loops)) return false;
        if (fLegs.size > 0 && !fLegs.has(d.legs)) return false;
        if (fFC.size > 0 && !fFC.has(d.fcClass)) return false;
        if (fMS.size > 0 && !fMS.has(d.massScales)) return false;
        if (sf && !d.name.toLowerCase().includes(sf) && !d.topoName.toLowerCase().includes(sf) && !d.topoKey.toLowerCase().includes(sf) && !d.configKey.toLowerCase().includes(sf)) return false;
        return true;
      });
      filtered.sort((a, b) => (a.loops - b.loops) || (a.legs - b.legs) || (a.props - b.props) || a.name.localeCompare(b.name));
      $('browser-count').textContent = filtered.length + ' diagram' + (filtered.length!==1?'s':'');
      body.innerHTML = '';
      if (filtered.length === 0) {
        body.innerHTML = '<div style="padding:32px;text-align:center;color:var(--text-muted)">No matches.</div>';
        return;
      }
      const list = document.createElement('div'); list.className = 'browser-list';
      filtered.forEach(d => {
        try {
        const card = document.createElement('div');
        card.className = 'browser-toast browser-toast-diagram' + (d.isWaitlisted ? ' browser-toast-waitlisted' : '');
        const thumb = generateThumbnail(d.topoKey, d.configKey);
        thumb.classList.add('notif-thumb');
        const cardBody = document.createElement('div');
        cardBody.className = 'notif-body';
        let badges = '';
        const ms = d.massScales;
        if (ms !== undefined && ms !== null) badges += `<span class="badge badge-accent">${ms} scale${ms !== 1 ? 's' : ''}</span>`;
        const fc = d.cfg.FunctionClass || d.cfg.functionClass;
        if (fc && fc !== 'None' && fc !== 'Unknown' && fc !== 'unknown') badges += functionBadge(fc);
        badges += refCountBadge(d.cfg);
        // Verified badge removed from toast — now per-result only.
        const dResultStar = d.hasResults ? '<span class="result-star" title="Result computed">\u2605</span> ' : '';
        cardBody.innerHTML = `
          <div class="notif-title">${dResultStar}${d.name}${badges ? ' ' + badges : ''}</div>
          <div class="notif-stats">
            <span><span class="notif-stat-val">${d.loops}</span> loop${d.loops !== 1 ? 's' : ''}</span>
            <span><span class="notif-stat-val">${d.legs}</span> leg${d.legs !== 1 ? 's' : ''}</span>
            <span><span class="notif-stat-val">${d.props}</span> prop${d.props !== 1 ? 's' : ''}</span>
          </div>
        `;
        card.appendChild(thumb);
        card.appendChild(cardBody);
        card.addEventListener('click', () => {
          openDetailPanel(d.topoKey, d.topo, {[d.configKey]: 'compatible'}, d.configKey, { fromBrowser: true });
        });
        list.appendChild(card);
        } catch (e) { console.warn('Thumbnail failed for', d.topoKey, d.configKey, e); }
      });
      body.appendChild(list);
    }
  }

  search.oninput = renderList;
  renderList();
}

// Viewport-aware auto-fit after loading a diagram. On phones the default
// zoom=1.0 leaves the graph tiny (BASE_VIEWBOX is 12×8 SVG units mapped to
// ~390px wide); scale and centre on the bbox so the diagram fills ~70% of
// the viewport. Desktop keeps the default behaviour.
function _fitLoadedGraphToViewport(vertices) {
  if (!vertices || vertices.length === 0) return false;
  const isMobile = typeof matchMedia === 'function' && matchMedia('(max-width: 768px)').matches;
  if (!isMobile) return false;
  let minX = Infinity, maxX = -Infinity, minY = Infinity, maxY = -Infinity;
  for (const v of vertices) {
    if (v.x < minX) minX = v.x;
    if (v.x > maxX) maxX = v.x;
    if (v.y < minY) minY = v.y;
    if (v.y > maxY) maxY = v.y;
  }
  const bbW = Math.max(maxX - minX, 0.5);  // floor avoids div-by-zero for tadpoles
  const bbH = Math.max(maxY - minY, 0.5);
  const PAD = 1.6;  // 60% headroom around the bbox for vertex radii + leg momentum labels
  const zByW = BASE_VIEWBOX.w / (bbW * PAD);
  const zByH = BASE_VIEWBOX.h / (bbH * PAD);
  const fit  = Math.min(zByW, zByH);
  zoomLevel = Math.max(0.3, Math.min(fit, 4.0));
  panOffset = { x: (minX + maxX) / 2, y: (minY + maxY) / 2 };
  return true;
}

function loadFromNickel(nickelStr, configKey) {
  try {
    // Use nickelFull (with legs) if available, else fall back to vacuum Nickel + legVertices
    const topo = library && library.topologies ? library.topologies[nickelStr] : null;
    const parseStr = (topo && topo.nickelFull) ? topo.nickelFull : nickelStr;
    const n = Nickel.fromString(parseStr);
    const edgeList = n.edges;
    const nickelList = n.nickel;

    // Add synthetic legs from legVertices if no nickelFull
    if (topo && !topo.nickelFull && topo.legVertices && topo.legVertices.length > 0) {
      for (const v of topo.legVertices) {
        edgeList.push([LEG, v]);
      }
    }

    // Parse config mass coloring. Labels are aligned 1:1 with the FULL bare
    // Nickel traversal — one label per char in the bare string, including
    // LEG positions ('e' in the bare ↔ leg mass digit in the color).
    const configLabels = configKey ? parseConfigColoring(configKey) : [];
    const edgeMasses = [];
    let labelIdx = 0;
    const labelToMass = {};
    let nextMass = 1;
    for (let i = 0; i < nickelList.length; i++) {
      for (const j of nickelList[i]) {
        const label = configLabels[labelIdx++] || '0';
        let mass = 0;
        if (label !== '0') {
          if (label === 'z') { mass = nextMass++; }
          else { if (!(label in labelToMass)) labelToMass[label] = nextMass++; mass = labelToMass[label]; }
        }
        edgeMasses.push(mass);
      }
    }

    const allNodes = new Set();
    edgeList.forEach(e => { if (e[0]>=0) allNodes.add(e[0]); if (e[1]>=0) allNodes.add(e[1]); });
    const maxNode = Math.max(...allNodes, 0);
    const verts = [];
    for (let i = 0; i <= maxNode; i++) {
      const ang = (2*Math.PI*i)/(maxNode+1);
      verts.push({ x: Math.cos(ang)*1.5, y: Math.sin(ang)*1.5 });
    }
    const canvasEdges = [];
    let nv = verts.length;
    let edgeIdx = 0;
    edgeList.forEach(e => {
      let a = e[0], b = e[1];
      let isLegA = false, isLegB = false;
      if (a === LEG) { isLegA = true; a = nv; verts.push({ x: verts[b].x*1.5, y: verts[b].y*1.5 }); nv++; }
      if (b === LEG) { isLegB = true; b = nv; verts.push({ x: verts[a].x*1.5, y: verts[a].y*1.5 }); nv++; }
      const mass = edgeMasses[edgeIdx] || 0;
      if (isLegA) {
        canvasEdges.push({ a, b, mass });
      } else if (isLegB) {
        canvasEdges.push({ a: b, b: a, mass });
      } else {
        canvasEdges.push({ a: Math.min(a,b), b: Math.max(a,b), mass });
      }
      edgeIdx++;
    });
    const laid = computeForceLayout(verts, canvasEdges);
    pushUndoState();
    state.vertices = laid;
    state.edges = canvasEdges;
    _momentumLabels = null;  // invalidate before render
    zoomLevel = 1.0;
    panOffset = { x: 0, y: 0 };
    _fitLoadedGraphToViewport(laid);  // mobile-only: scale + centre the bbox
    applyZoom();
    onGraphChanged();
    if (configKey) renderMassLegend();
  } catch (e) { console.error('Load failed:', e); }
}

function importEdgeList(input) {
  let str = input.trim().replace(/\{/g,'[').replace(/\}/g,']');
  try {
    const parsed = JSON.parse(str);
    const nodes = new Set();
    parsed.forEach(e => { nodes.add(e[0]); nodes.add(e[1]); });
    const nl = [...nodes].sort((a,b)=>a-b);
    const nm = {}; const verts = [];
    nl.forEach((n,i) => { nm[n]=i; verts.push({ x: Math.cos(2*Math.PI*i/nl.length)*1.5, y: Math.sin(2*Math.PI*i/nl.length)*1.5 }); });
    const ce = parsed.map(e => ({ a: Math.min(nm[e[0]],nm[e[1]]), b: Math.max(nm[e[0]],nm[e[1]]) }));
    const laid = computeForceLayout(verts, ce);
    pushUndoState();
    state.vertices = laid; state.edges = ce;
    _momentumLabels = null;
    zoomLevel = 1.0;
    panOffset = { x: 0, y: 0 };
    _fitLoadedGraphToViewport(laid);
    applyZoom(); onGraphChanged();
    closeBrowser();
  } catch(e) { showWarningToast('Could not parse edge list', true); }
}

// ─── Keyboard shortcuts ──────────────────────────────────────────────

document.addEventListener('keydown', function(evt) {
  if ((evt.ctrlKey||evt.metaKey) && ['c','a','v','x'].includes(evt.key)) return;
  const tag = (evt.target.tagName||'').toLowerCase();
  if (tag==='input'||tag==='textarea'||tag==='select') return;

  // Review mode keystrokes — handled before normal shortcuts so j/k/v don't
  // collide with anything. No-op when reviewMode is off.
  if (reviewHandleKey(evt)) return;

  // PDF page navigation — arrow keys when the PDF panel is visible.
  // Applies in two cases: (a) config-panel Paper tab is the active tab,
  // (b) review-mode and the left PDF panel is open.
  if (_pdfDoc && (evt.key === 'ArrowLeft' || evt.key === 'ArrowRight')) {
    const cfgOpen = $('config-panel').classList.contains('open');
    const paperActive = document.getElementById('cfg-paper')?.classList.contains('active');
    const reviewPdfOpen = document.body.classList.contains('review-has-pdf');
    if ((cfgOpen && paperActive) || reviewPdfOpen) {
      if (evt.key === 'ArrowLeft') reviewPdfPrev();
      else reviewPdfNext();
      evt.preventDefault();
      return;
    }
  }

  if (evt.key==='1'||evt.key==='d') { setMode('draw'); evt.preventDefault(); }
  if (evt.key==='2'||evt.key==='x') { setMode('delete'); evt.preventDefault(); }
  if (evt.key==='s' && !evt.ctrlKey && !evt.metaKey) { SNAP_ON=!SNAP_ON; $('snap-toggle').checked=SNAP_ON; render(); evt.preventDefault(); }
  if (evt.key==='='||evt.key==='+') { zoomCanvas(1); evt.preventDefault(); }
  if (evt.key==='-') { zoomCanvas(-1); evt.preventDefault(); }
  if (evt.key==='0') { zoomLevel=1.0; applyZoom(); evt.preventDefault(); }
  if (evt.key==='l'||evt.key==='L') { rebalanceLayout(); evt.preventDefault(); }
  if (evt.key==='n'||evt.key==='N') { toggleLabelMode(); evt.preventDefault(); }
  if (evt.key==='c'||evt.key==='C') { if ($('config-panel').classList.contains('open')) closeConfigPanel(); else openConfigPanel(); evt.preventDefault(); }
  if (evt.key==='?' || evt.key==='/') { $('help-overlay').classList.add('visible'); evt.preventDefault(); return; }
  if (evt.key==='Escape') {
    if ($('correction-overlay') && $('correction-overlay').classList.contains('visible')) { closeCorrectionModal(); evt.preventDefault(); return; }
    if ($('config-panel').classList.contains('open')) { closeConfigPanel(); evt.preventDefault(); return; }
    if ($('help-overlay').classList.contains('visible')) { $('help-overlay').classList.remove('visible'); evt.preventDefault(); return; }
    if ($('about-overlay').classList.contains('visible')) { closeAbout(); evt.preventDefault(); return; }
    if ($('integrate-overlay').classList.contains('visible')) { $('integrate-overlay').classList.remove('visible'); evt.preventDefault(); return; }
    if ($('detail-panel').classList.contains('open')) { closeDetailPanel(); evt.preventDefault(); return; }
    if ($('browser-overlay').classList.contains('visible')) { closeBrowser(); evt.preventDefault(); return; }
    if (state.edgeDragFrom!==null) { state.edgeDragFrom=null; state.edgeDragPos=null; clearEdgeTrails(); render(); }
    else if (state.selectedVertex!==null) { state.selectedVertex=null; render(); }
    evt.preventDefault();
  }
  if (evt.key==='Tab' && state.vertices.length>0) {
    const cur = state.selectedVertex;
    state.selectedVertex = cur===null ? 0 : evt.shiftKey ? (cur-1+state.vertices.length)%state.vertices.length : (cur+1)%state.vertices.length;
    render(); evt.preventDefault();
  }
  if ((evt.ctrlKey||evt.metaKey) && evt.key==='z' && !evt.shiftKey) { undoAction(); evt.preventDefault(); }
  if ((evt.ctrlKey||evt.metaKey) && (evt.key==='y'||(evt.key==='z'&&evt.shiftKey))) { redoAction(); evt.preventDefault(); }
});

// ─── Button bindings ─────────────────────────────────────────────────

$('mode-draw').addEventListener('click', () => setMode('draw'));
$('mode-delete').addEventListener('click', () => setMode('delete'));
$('undo-btn').addEventListener('click', undoAction);
$('redo-btn').addEventListener('click', redoAction);
$('rotate-cw-btn').addEventListener('click', () => rotateLayout(45));
$('rotate-ccw-btn').addEventListener('click', () => rotateLayout(-45));
$('flip-h-btn').addEventListener('click', () => flipLayout('h'));
$('flip-v-btn').addEventListener('click', () => flipLayout('v'));
$('rebalance-btn').addEventListener('click', rebalanceLayout);
$('momenta-toggle').addEventListener('change', syncLabelToggles);
$('arrows-toggle').addEventListener('change', syncLabelToggles);
$('line-scale').addEventListener('input', function() { lineScale = parseFloat(this.value); render(); });
$('label-scale').addEventListener('input', function() { labelScale = parseFloat(this.value); render(); });
// Position the Display dropdown menu below the button (fixed positioning)
{
  const dd = document.querySelector('.display-dropdown');
  const positionMenu = () => {
    const btn = dd?.querySelector('.display-dropdown-btn');
    const menu = dd?.querySelector('.display-dropdown-menu');
    if (btn && menu) {
      const r = btn.getBoundingClientRect();
      menu.style.top = r.bottom + 'px';
      menu.style.left = r.left + 'px';
    }
  };
  dd?.addEventListener('mouseenter', positionMenu);
  // Touch path: tapping the display button toggles .open (no hover on touch devices)
  $('display-btn')?.addEventListener('click', (e) => {
    e.stopPropagation();
    positionMenu();
    dd?.classList.toggle('open');
  });
  document.addEventListener('click', (e) => {
    if (dd && !dd.contains(e.target)) dd.classList.remove('open');
  });
}

// Overflow (⋯) menu — surfaces toolbar/bottom-bar actions on mobile
{
  const dd = $('overflow-dropdown');
  const btn = $('overflow-btn');
  const menu = $('overflow-menu');
  if (dd && btn && menu) {
    const _syncOverflowBodyClass = () => {
      document.body.classList.toggle('overflow-open', dd.classList.contains('open'));
    };
    btn.addEventListener('click', (e) => {
      e.stopPropagation();
      dd.classList.toggle('open');
      _syncOverflowBodyClass();
      // Close the notif sheet when the overflow menu opens, so the menu isn't
      // visually occluded and the user isn't juggling two open surfaces.
      if (dd.classList.contains('open') && typeof closeNotifSheet === 'function') {
        closeNotifSheet();
      }
    });
    document.addEventListener('click', (e) => {
      if (!dd.contains(e.target)) {
        dd.classList.remove('open');
        _syncOverflowBodyClass();
      }
    });
    menu.addEventListener('click', (e) => {
      const item = e.target.closest('[data-action]');
      if (!item) return;
      dd.classList.remove('open');
      _syncOverflowBodyClass();
      const action = item.dataset.action;
      if (action === 'config') {
        if ($('config-panel').classList.contains('open')) closeConfigPanel();
        else openConfigPanel();
        return;
      }
      if (action === 'mode-draw')   { setMode('draw');   return; }
      if (action === 'mode-delete') { setMode('delete'); return; }
      if (action === 'safe-exit') {
        // Safe exit: tell the kernel to normalize $STQuadruple / $STGraph /
        // friends, then shut down.  We do NOT close the window here — the
        // kernel kills the server + native viewer as soon as it processes
        // the request.  Requested by Giulio, 2026-04-21.
        (async () => {
          try {
            await kernel.post('safeExit', {});
          } catch (err) {
            // Request may never return (server dies mid-response); silent ok.
          }
        })();
        return;
      }
      // All other actions forward to the existing toolbar/bottom-bar buttons
      const target = {
        'redo': 'redo-btn',
        'clear': 'clear-btn',
        'rotate-cw': 'rotate-cw-btn',
        'rotate-ccw': 'rotate-ccw-btn',
        'flip-h': 'flip-h-btn',
        'flip-v': 'flip-v-btn',
        'rebalance': 'rebalance-btn',
      }[action];
      if (target && $(target)) $(target).click();
    });
  }
}
$('zoom-out-btn').addEventListener('click', () => zoomCanvas(-1));
$('zoom-in-btn').addEventListener('click', () => zoomCanvas(1));
$('snap-toggle').addEventListener('change', function() { SNAP_ON=this.checked; render(); });
$('clear-btn').addEventListener('click', clearAll);
$('app-title').addEventListener('click', openAbout);
$('help-btn').addEventListener('click', () => { $('help-overlay').classList.add('visible'); });
$('help-close').addEventListener('click', () => { $('help-overlay').classList.remove('visible'); });
$('help-overlay').addEventListener('click', function(evt) { if (evt.target === this) this.classList.remove('visible'); });
$('help-tour-btn').addEventListener('click', () => {
  $('help-overlay').classList.remove('visible');
  // Defer so the modal's close animation doesn't wrestle with the tour's
  // position measurements.
  setTimeout(() => startTour(), 250);
});
$('about-close').addEventListener('click', closeAbout);
$('about-overlay').addEventListener('click', function(evt) { if (evt.target===this) closeAbout(); });
if ($('correction-overlay')) {
  $('correction-close').addEventListener('click', closeCorrectionModal);
  $('correction-cancel').addEventListener('click', closeCorrectionModal);
  $('correction-overlay').addEventListener('click', function(evt) {
    if (evt.target === this) closeCorrectionModal();
  });
  $('correction-submit').addEventListener('click', submitCorrection);
  $('correction-comment').addEventListener('input', function() {
    $('correction-comment-count').textContent = String(this.value.length);
    updateCorrectionSubmitState();
  });
  $('correction-proposed').addEventListener('input', function() {
    $('correction-proposed-count').textContent = String(this.value.length);
  });
  $('correction-anon').addEventListener('change', function() {
    const nameInput = $('correction-contributor');
    nameInput.disabled = this.checked;
    nameInput.placeholder = this.checked ? 'Anonymous' : 'Your name';
    if (this.checked) nameInput.blur();
    else nameInput.focus();
  });
  $('correction-category').addEventListener('change', function() {
    const textarea = $('correction-comment');
    const current = textarea.value;
    if (this.value === 'citation-request') {
      // Pre-fill the gag template, but don't clobber a real message the
      // user has already started writing.
      if (!current.trim() || current === CITATION_REQUEST_TEMPLATE) {
        textarea.value = CITATION_REQUEST_TEMPLATE;
      }
    } else if (current === CITATION_REQUEST_TEMPLATE) {
      // Switched away from citation-request without editing the template
      // → clear it so the user doesn't accidentally submit the gag under
      // a different category.
      textarea.value = '';
    }
    $('correction-comment-count').textContent = String(textarea.value.length);
    updateCorrectionSubmitState();
  });
}
$('browse-btn').addEventListener('click', openBrowser);
if ($('library-fab')) $('library-fab').addEventListener('click', openBrowser);
$('browser-close').addEventListener('click', closeBrowser);

// ─── Mobile tab bar (≤480px) ─────────────────────────────────────────
// Persistent bottom nav: Draw · Library · Configure · Export. Taps route
// to existing handlers. Active state reflects which surface is foreground.
(function _wireMobileTabbar() {
  const tabbar = document.getElementById('mobile-tabbar');
  if (!tabbar) return;
  const tabs = tabbar.querySelectorAll('.mobile-tab');
  function setActive(name) {
    tabs.forEach(el => el.classList.toggle('mobile-tab-active', el.dataset.tab === name));
  }
  function syncActiveTab() {
    const cfgOpen = document.getElementById('config-panel')?.classList.contains('open');
    const libOpen = document.getElementById('browser-overlay')?.classList.contains('visible');
    const expOpen = document.getElementById('export-dropdown')?.classList.contains('visible');
    if      (libOpen) setActive('library');
    else if (cfgOpen) setActive('configure');
    else if (expOpen) setActive('export');
    else              setActive('draw');
  }
  tabbar.addEventListener('click', (evt) => {
    const btn = evt.target.closest('.mobile-tab');
    if (!btn) return;
    const which = btn.dataset.tab;
    // Close any surfaces that don't match the tap.
    const cfgPanel = document.getElementById('config-panel');
    const expDD    = document.getElementById('export-dropdown');
    if (which !== 'library') closeBrowser();
    if (which !== 'configure' && cfgPanel && cfgPanel.classList.contains('open')) closeConfigPanel();
    if (which !== 'export' && expDD && expDD.classList.contains('visible')) expDD.classList.remove('visible');

    if (which === 'draw') {
      setMode('draw');
    } else if (which === 'library') {
      openBrowser();
    } else if (which === 'configure') {
      if (cfgPanel && cfgPanel.classList.contains('open')) closeConfigPanel();
      else openConfigPanel();
    } else if (which === 'export') {
      if (expDD) expDD.classList.toggle('visible');
      evt.stopPropagation();  // prevent document listener from immediately closing the dropdown
    }
    syncActiveTab();
  });
  // Keep the highlight in sync with dismissals (tap backdrop, swipe to
  // close, keyboard dismiss). A 250 ms poll is cheap and avoids threading
  // explicit sync calls through every open/close site.
  setInterval(syncActiveTab, 250);
  syncActiveTab();
})();
$('browser-overlay').addEventListener('click', function(evt) { if (evt.target===this) closeBrowser(); });
$('browser-tab-topos').addEventListener('click', () => switchBrowserTab('topos'));
$('browser-tab-diagrams').addEventListener('click', () => switchBrowserTab('diagrams'));
$('browser-sync-btn').addEventListener('click', async () => {
  const btn = $('browser-sync-btn');
  btn.disabled = true;
  btn.textContent = 'Syncing\u2026';
  try {
    const res = await kernel.post('syncLibrary', '{}');
    if (res.status === 'ok') {
      localStorage.setItem('subtropica-last-sync', new Date().toLocaleDateString('en-US', { year: 'numeric', month: 'short', day: 'numeric' }));
      showWarningToast(`Synced: ${res.downloaded || 0} new entries`);
      // Reload library.json and refresh the browser
      try {
        const libRes = await fetch('/api/library');
        if (libRes.ok) {
          library = await libRes.json();
          populateBrowser();
        }
      } catch {}
    } else {
      showWarningToast('Sync failed: ' + (res.error || 'unknown'));
    }
  } catch { showWarningToast('Sync failed: network error'); }
  btn.disabled = false;
  btn.textContent = 'Sync';
});
// Import button (wired up when UI elements are added to the browser panel)
if ($('browser-import-btn')) {
  $('browser-import-btn').addEventListener('click', () => { const v=$('browser-edge-input').value; if (v) importEdgeList(v); });
}
$('detail-close').addEventListener('click', closeDetailPanel);
$('detail-backdrop').addEventListener('click', closeDetailPanel);

// Click-to-focus: detail popup or config panel comes to front on mousedown
$('detail-panel').addEventListener('mousedown', () => focusPanel('detail'));
$('config-panel').addEventListener('mousedown', () => focusPanel('config'));

// Export dropdown
$('export-menu-btn').addEventListener('click', (evt) => {
  evt.stopPropagation();
  $('export-dropdown').classList.toggle('visible');
});
document.addEventListener('click', () => {
  $('export-dropdown').classList.remove('visible');
  if (!_massPickerJustOpened) closeMassPicker();
});
$('export-pdf').addEventListener('click', exportPDF);
$('export-png').addEventListener('click', exportPNG);
$('export-tikz').addEventListener('click', exportTikZ);
$('export-copy-svg').addEventListener('click', () => {
  const svgStr = generateExportSVG();
  navigator.clipboard.writeText(svgStr).then(() => showWarningToast('SVG copied to clipboard'));
});
$('export-copy-png').addEventListener('click', () => {
  const svgStr = generateExportSVG();
  const img = new Image();
  img.onload = () => {
    const c = document.createElement('canvas');
    c.width = 800; c.height = 600;
    const ctx = c.getContext('2d');
    ctx.drawImage(img, 0, 0, c.width, c.height);
    c.toBlob(blob => {
      navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]).then(
        () => showWarningToast('PNG copied to clipboard'),
        () => showWarningToast('Clipboard access denied', true)
      );
    });
  };
  img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svgStr);
});
$('export-copy-link').addEventListener('click', () => {
  updateUrlHash();
  navigator.clipboard.writeText(window.location.href).then(
    () => showWarningToast('Shareable link copied'),
    () => showWarningToast('Clipboard access denied', true)
  );
});

// Dark mode
function mascotEmoji() { return document.body.classList.contains('dark') ? '\u{1F427}' : '\u{1F965}'; }

function updateMascot() {
  const m = mascotEmoji();
  document.querySelectorAll('.mascot-emoji').forEach(el => { el.textContent = m; });
  // Update favicon
  const favicon = document.querySelector('link[rel="icon"]');
  if (favicon) favicon.href = `data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>${m}</text></svg>`;
}

function toggleDarkMode() {
  document.body.classList.toggle('dark');
  localStorage.setItem('subtropica-dark', document.body.classList.contains('dark') ? '1' : '0');
  updateMascot();
  render();
}
$('theme-toggle').addEventListener('click', toggleDarkMode);

// Prevent clicks inside mass picker from closing it
$('mass-picker').addEventListener('click', (evt) => evt.stopPropagation());
$('mass-picker').addEventListener('mousedown', (evt) => evt.stopPropagation());


// ─── Feynman integral representation ────────────────────────────────

/**
 * Build a symbol dictionary from the current diagram state.
 * Each entry: { input: string, latex: string, mma: string }
 * Sorted by descending length so longest-match-first tokenization works.
 */
function buildSymbolDict() {
  const dict = [];
  const deg = getVertexDegrees();

  // Count loops and external legs for subscript simplification
  const intEdges = [], extEdges = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a === e.b) { intEdges.push(i); continue; }
    if ((deg[e.a]||0) <= 1 || (deg[e.b]||0) <= 1) extEdges.push(i);
    else intEdges.push(i);
  }
  const intVerts = new Set();
  for (let i = 0; i < state.vertices.length; i++) if ((deg[i]||0) > 1) intVerts.add(i);
  const nLoops = Math.max(0, intEdges.length - intVerts.size + 1);
  const nExt = extEdges.length;

  // Loop momenta
  for (let i = 0; i < nLoops; i++) {
    const custom = customLoopLabels[i];
    const idx = i + 1;
    const sub = nLoops === 1 ? '' : `_{${idx}}`;
    const mma = `l[${idx}]`;
    if (custom) {
      // Custom label — user might type it as-is
      const plain = custom.replace(/\\/g, '').replace(/[{}]/g, '');
      dict.push({ input: plain, latex: custom, mma });
    }
    // Default names: l, l1, l2, ell, ell1, \ell, \ell_1
    if (nLoops === 1) {
      dict.push({ input: 'l', latex: `\\ell${sub}`, mma });
    }
    // Indexed forms: ell1, l1, \ell_{1}, \ell_1, l[1]
    dict.push({ input: `ell${idx}`, latex: `\\ell_{${idx}}`, mma });
    dict.push({ input: `l${idx}`, latex: `\\ell_{${idx}}`, mma });
    dict.push({ input: `\\ell_{${idx}}`, latex: `\\ell_{${idx}}`, mma });
    dict.push({ input: `\\ell_${idx}`, latex: `\\ell_{${idx}}`, mma });
    dict.push({ input: `l[${idx}]`, latex: `\\ell_{${idx}}`, mma });
  }
  // Unsubscripted forms (only when nLoops >= 1, map to first loop momentum)
  if (nLoops >= 1) {
    const mma1 = 'l[1]';
    const sub1 = nLoops === 1 ? '' : '_{1}';
    dict.push({ input: 'ell', latex: `\\ell${sub1}`, mma: mma1 });
    dict.push({ input: '\\ell', latex: `\\ell${sub1}`, mma: mma1 });
  }

  // External momenta
  for (let i = 0; i < nExt; i++) {
    const e = state.edges[extEdges[i]];
    const idx = i + 1;
    const sub = nExt <= 2 ? '' : `_{${idx}}`;
    const mma = `p[${idx}]`;
    const customLabel = e.extMomLabel;
    if (customLabel && customLabel.trim()) {
      const plain = customLabel.replace(/\\/g, '').replace(/[{}]/g, '');
      const latex = massLabelToTeX(customLabel);
      dict.push({ input: plain, latex, mma });
    }
    if (nExt <= 2) {
      dict.push({ input: 'p', latex: `p${sub}`, mma });
    }
    dict.push({ input: `p${idx}`, latex: `p_{${idx}}`, mma });
    dict.push({ input: `p[${idx}]`, latex: `p_{${idx}}`, mma });
  }

  // Mass symbols
  const massEntries = new Map(); // mass value → {input, latex, mma}
  for (const e of state.edges) {
    if (!e.mass || e.mass <= 0) continue;
    if (massEntries.has(e.mass)) continue;
    const distinctMasses = new Set(state.edges.filter(x => x.mass > 0).map(x => x.mass));
    const sub = distinctMasses.size === 1 ? '' : `${e.mass}`;
    if (e.massLabel) {
      const plain = e.massLabel.replace(/\\/g, '').replace(/[{}]/g, '');
      const latex = massLabelToTeX(e.massLabel);
      const mma = massLabelToMma(e.massLabel);
      dict.push({ input: plain, latex, mma });
      massEntries.set(e.mass, true);
    } else {
      dict.push({ input: `m${sub || e.mass}`, latex: sub ? `m` : `m_{${e.mass}}`, mma: `m[${e.mass}]` });
      massEntries.set(e.mass, true);
    }
  }

  // Common constants/variables
  dict.push({ input: 'eps', latex: '\\varepsilon', mma: 'eps' });
  dict.push({ input: 'pi', latex: '\\pi', mma: 'Pi' });

  // Sort by descending input length for longest-match-first
  dict.sort((a, b) => b.input.length - a.input.length);
  return dict;
}

/**
 * Parse a physics expression into { latex, mma } using the diagram's symbol table.
 * Handles: arithmetic (+, -, *, ^), dot products (.), parentheses, numbers.
 * Unrecognized identifiers render as \texttt{} in LaTeX and pass through in Mma.
 */
function parsePhysicsExpr(raw) {
  if (!raw || !raw.trim()) return { latex: '', mma: '' };
  const dict = buildSymbolDict();
  let s = raw.trim();
  let latex = '', mma = '';
  let prevWasSymbol = false;

  while (s.length > 0) {
    // Skip whitespace
    if (/^\s/.test(s)) { s = s.replace(/^\s+/, ''); latex += ' '; mma += ' '; continue; }

    // Try matching a known symbol (longest first)
    let matched = false;
    for (const entry of dict) {
      // Word boundary check: next char after match must not be alphanumeric
      if (s.startsWith(entry.input)) {
        const after = s[entry.input.length];
        if (after && /[a-zA-Z0-9]/.test(after) && !/[\[\(]/.test(after)) continue;
        // Dot product: symbol.symbol or symbol * symbol
        if (prevWasSymbol && !latex.endsWith('\\cdot ') && !latex.endsWith('{+}') && !latex.endsWith('{-}') && !latex.endsWith('(')) {
          latex += ' \\cdot ';
          mma += '*';
        }
        latex += entry.latex;
        mma += entry.mma;
        s = s.slice(entry.input.length);
        matched = true;
        prevWasSymbol = true;
        break;
      }
    }
    if (matched) continue;

    // Operators and punctuation
    const ch = s[0];
    if (ch === '+') { latex += '{+}'; mma += '+'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '-') { latex += '{-}'; mma += '-'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '*') { latex += '\\,'; mma += '*'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '.') { latex += '\\cdot '; mma += '\x00'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '^') { latex += '^'; mma += '^'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '(') { latex += '('; mma += '('; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === ')') { latex += ')'; mma += ')'; s = s.slice(1); prevWasSymbol = true; continue; }
    if (ch === '{') { latex += '\\{'; mma += '{'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === '}') { latex += '\\}'; mma += '}'; s = s.slice(1); prevWasSymbol = false; continue; }
    if (ch === ',') { latex += ',\\,'; mma += ','; s = s.slice(1); prevWasSymbol = false; continue; }

    // Arrow (-> or =) in substitutions
    if (s.startsWith('->')) { latex += '\\to '; mma += '->'; s = s.slice(2); prevWasSymbol = false; continue; }
    if (ch === '=' && !s.startsWith('==')) { latex += '\\to '; mma += '->'; s = s.slice(1); prevWasSymbol = false; continue; }

    // Numbers (including decimals and fractions like 1/2)
    const numMatch = s.match(/^(\d+(?:\.\d+)?(?:\/\d+)?)/);
    if (numMatch) {
      const num = numMatch[1];
      if (num.includes('/')) {
        const [n, d] = num.split('/');
        latex += `\\frac{${n}}{${d}}`;
      } else {
        latex += num;
      }
      mma += num;
      s = s.slice(num.length);
      prevWasSymbol = false;
      continue;
    }

    // Unrecognized identifier: collect alphanumeric + underscore
    const idMatch = s.match(/^([a-zA-Z_][a-zA-Z0-9_]*)/);
    if (idMatch) {
      const id = idMatch[1];
      // Mandelstam-like variables: s12, s123, t12, u12
      if (/^[stu]\d+$/.test(id)) {
        latex += `${id[0]}_{${id.slice(1)}}`;
        mma += id;
      }
      // Single Greek letter names
      else if (/^(alpha|beta|gamma|delta|epsilon|zeta|eta|theta|lambda|mu|nu|rho|sigma|tau|phi|chi|psi|omega|Gamma|Delta|Sigma|Pi|Omega)$/.test(id)) {
        latex += `\\${id}`;
        mma += id;
      }
      // Common functions
      else if (/^(Log|Sin|Cos|Exp|Sqrt)$/.test(id)) {
        latex += `\\${id.toLowerCase()}`;
        mma += id;
      }
      // Variable with trailing digits: x1 → x_{1}, M2 → M_{2}
      else if (/^([a-zA-Z])(\d+)$/.test(id)) {
        const [, letter, digits] = id.match(/^([a-zA-Z])(\d+)$/);
        latex += `${letter}_{${digits}}`;
        mma += id;
      }
      // Single letter: just use it as-is (italic in math mode)
      else if (id.length === 1) {
        latex += id;
        mma += id;
      }
      // Multi-letter unknown → \mathrm{} (cleaner than \texttt{})
      else {
        latex += `\\mathrm{${id}}`;
        mma += id;
      }
      s = s.slice(id.length);
      prevWasSymbol = true;
      continue;
    }

    // Fallback: emit character as-is
    latex += ch;
    mma += ch;
    s = s.slice(1);
    prevWasSymbol = false;
  }

  // Post-process: convert dot products (marked with \0) to CenterDot[a, b].
  // Pattern: the operand before \0 is the last token, the operand after is the next token.
  if (mma.includes('\0')) {
    // Split by \0 and rebuild with CenterDot wrapping
    mma = mma.replace(
      /([a-zA-Z_][a-zA-Z0-9_\[\]]*)\x00([a-zA-Z_][a-zA-Z0-9_\[\]]*)/g,
      'CenterDot[$1,$2]'
    );
    // Clean up any remaining \0 (shouldn't happen, but safety)
    mma = mma.replace(/\x00/g, '*');
  }

  return { latex, mma };
}

// Backward-compatible wrapper (used in buildFeynmanIntegralLaTeX)
function momToTeX(raw) {
  return parsePhysicsExpr(raw).latex;
}

function buildFeynmanIntegralLaTeX() {
  const deg = getVertexDegrees();
  const nEdges = state.edges.length;
  if (nEdges < 1) return null;

  // Classify edges
  const isExt = [];
  const intEdges = [];
  let extCount = 0;
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    if ((deg[e.a] || 0) === 1 || (deg[e.b] || 0) === 1) {
      isExt.push(true);
      extCount++;
    } else {
      isExt.push(false);
      intEdges.push(i);
    }
  }

  // No internal edges → no formula to display
  if (intEdges.length === 0) return null;

  // Count loops
  const intVerts = new Set();
  for (let i = 0; i < state.vertices.length; i++) {
    if ((deg[i] || 0) > 1) intVerts.add(i);
  }
  const V = intVerts.size;
  const E = intEdges.length;
  const L = E - V + 1;

  const momLabels = getMomentumLabels();

  // Build measure (SubTropica style) — empty for tree-level
  let measure = '';
  if (L >= 1) {
    const measures = [];
    for (let i = 1; i <= L; i++) {
      const ellLabel = L === 1 ? '\\ell' : `\\ell_{${i}}`;
      measures.push(`\\frac{\\mathrm{d}^{D}${ellLabel}}{i\\pi^{D/2}}`);
    }
    measure = '\\int ' + measures.join('\\,');
  }

  // Build propagator factors with actual exponents
  const denParts = [];
  let propIdx = 1;
  intEdges.forEach(ei => {
    const e = state.edges[ei];
    const exp = e.propExponent ?? 1;
    const mom = momLabels ? momLabels[ei] : `k_{${propIdx}}`;
    const massVal = e.mass || 0;
    const massTeX = massVal > 0 ? getMassDisplayLabel(massVal, e) : null;
    const isSimple = !mom.includes('+') && !mom.includes('-');
    const momSq = isSimple ? `${mom}^2` : `\\left(${mom}\\right)^2`;
    let prop = momSq;
    if (massTeX) prop += ` - ${massTeX}^2`;
    // Bracket/exponent rules:
    //   massless + default exp → bare (q^2)
    //   massive + default exp  → [q^2 - m^2]
    //   any non-default exp    → [q^2 - m^2]^{ν}
    if (backendMode === 'full') {
      const isDefaultExp = (exp === 1 || exp === '1');
      if (isDefaultExp && !massTeX) {
        denParts.push(prop);
      } else if (isDefaultExp && massTeX) {
        denParts.push(`[${prop}]`);
      } else {
        denParts.push(`[${prop}]^{${exp}}`);
      }
    } else {
      // Online mode: always show generic ν_i
      if (!massTeX) {
        denParts.push(`[${prop}]^{\\nu_{${propIdx}}}`);
      } else {
        denParts.push(`[${prop}]^{\\nu_{${propIdx}}}`);
      }
    }
    propIdx++;
  });

  // Normalization prefactor
  let normPrefix = '';
  const norm = computeConfig.normalization;
  if (norm === 'Automatic' || !norm) {
    // Default SubTropica normalization: e^{L ε γ_E} (skip for tree-level)
    if (L === 1) {
      normPrefix = '\\mathrm{e}^{\\varepsilon \\gamma_E} \\,';
    } else if (L >= 2) {
      normPrefix = `\\mathrm{e}^{${L}\\varepsilon \\gamma_E} \\,`;
    }
  } else if (norm !== '1') {
    normPrefix = `${norm} \\,`;
  }

  // Build numerator from numerator rows
  const numRows = computeConfig.numeratorRows.filter(r => r.expr);
  let numerator = '1';
  if (numRows.length > 0) {
    const numParts = numRows.map(r => {
      const expr = momToTeX(r.expr || '1');
      const exp = parseInt(r.exp) || -1;
      if (exp === -1) return `(${expr})`;
      return `(${expr})^{${-exp}}`;
    });
    numerator = numParts.join('\\,');
  }

  let latex;
  if (L >= 1) {
    latex = `${normPrefix}${measure} \\; \\frac{${numerator}}{${denParts.join('\\,')}}`;
  } else if (denParts.length > 0) {
    // Tree-level: no integral sign, just propagator product
    latex = `\\frac{${numerator}}{${denParts.join('\\,')}}`;
  } else {
    return null;  // no internal edges at all
  }
  return { latex, loops: L, extLegs: extCount, intProps: E };
}

function buildFeynmanIntegralMathematica() {
  const deg = getVertexDegrees();
  const nEdges = state.edges.length;
  if (nEdges < 1) return null;

  const intEdges = [];
  let extCount = 0;
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    if ((deg[e.a] || 0) === 1 || (deg[e.b] || 0) === 1) extCount++;
    else intEdges.push(i);
  }

  const intVerts = new Set();
  for (let i = 0; i < state.vertices.length; i++) {
    if ((deg[i] || 0) > 1) intVerts.add(i);
  }
  const L = intEdges.length - intVerts.size + 1;
  if (intEdges.length === 0) return null;

  const momLabels = getMomentumLabels();

  // Build propagator list with exponents nu[i]
  const props = [];
  let propIdx = 1;
  intEdges.forEach(ei => {
    const e = state.edges[ei];
    let mom = momLabels ? momLabels[ei] : `k[${propIdx}]`;
    // Convert LaTeX momentum to Mathematica: \ell_{1} -> l[1], p_{1} -> p[1]
    // Handle both subscripted and unsubscripted forms (from subscript simplification)
    mom = mom.replace(/\\ell_\{(\d+)\}/g, 'l[$1]');
    mom = mom.replace(/\\ell\b/g, 'l[1]');  // unsubscripted \ell -> l[1]
    mom = mom.replace(/p_\{(\d+)\}/g, 'p[$1]');
    mom = mom.replace(/\bp\b(?!\[)/g, 'p[1]');  // unsubscripted p -> p[1]
    mom = mom.replace(/ \+ /g, '+').replace(/ - /g, '-');
    const massVal = e.mass || 0;
    const massLabel = e.massLabel ? massLabelToMma(e.massLabel) : `m[${massVal}]`;
    const massM = massVal > 0 ? massLabel : '0';
    props.push(`FAD[{${mom}, ${massM}}, nu[${propIdx}]]`);
    propIdx++;
  });

  return props.join(' ');
}

// ─── Panel focus (click-to-front between config panel and detail popup) ───

function focusPanel(which) {
  // 'config' → config panel in front; 'detail' → detail popup in front (default stacking)
  $('config-panel').classList.toggle('panel-front', which === 'config');
}

function updateCanvasDepth() {
  const anyOpen = $('config-panel').classList.contains('open') ||
                  $('detail-panel').classList.contains('open');
  document.body.classList.toggle('canvas-recessed', anyOpen);
}

// ─── Configuration panel ─────────────────────────────────────────────

function openConfigPanel() {
  $('config-panel').classList.add('open');
  $('config-backdrop').classList.add('open');
  populateConfigPanel();
  updateCanvasDepth();
  // If the panel opens on the Kinematics tab (sticky selection), keep
  // the canvas sharp — see switchConfigTab for the full explanation.
  const activeTab = document.querySelector('.config-tab.active');
  document.body.classList.toggle('canvas-on-kinematics',
    activeTab && activeTab.dataset.tab === 'cfg-momenta');
  // Slide mass legend out of the way
  const legend = $('mass-legend');
  if (legend) legend.classList.add('config-shifted');
  // Keep integral card visible above config panel so user can click Integrate
  if (_integralCard) _integralCard.style.zIndex = '200';
}

function closeConfigPanel() {
  $('config-panel').classList.remove('open', 'panel-front');
  $('config-backdrop').classList.remove('open');
  syncConfigToState();
  updateCanvasDepth();
  document.body.classList.remove('canvas-on-kinematics');
  // Slide mass legend back
  const legend = $('mass-legend');
  if (legend) legend.classList.remove('config-shifted');
  if (_integralCard) { _integralCard.style.zIndex = ''; updateIntegralCard(); }
}

function switchConfigTab(btn) {
  const tabId = btn.dataset.tab;
  document.querySelectorAll('.config-tab').forEach(t => t.classList.toggle('active', t === btn));
  document.querySelectorAll('.config-tab-content').forEach(c => c.classList.toggle('active', c.id === tabId));
  // Keep the canvas sharp on the Kinematics tab: the user is picking loop
  // momenta / assigning external masses and needs to read edge labels
  // clearly, so the "panel over canvas" blur that applies elsewhere would
  // get in the way. Tracked via body class; a CSS override in style.css
  // removes the blur filter when this class is present.
  document.body.classList.toggle('canvas-on-kinematics', tabId === 'cfg-momenta');
  // Re-sync on tab change so destination content reflects the latest state.
  // Without this, toggling an Advanced-tab option then switching to Export
  // shows a stale command (the Export tab was last rendered at panel-open
  // time). Cheap operations; safe to run on every switch.
  syncConfigToState();
  if (tabId === 'cfg-export') {
    populateExportTab();
  } else if (tabId === 'cfg-momenta') {
    populateExtMasses();
    populateEdgeTable();
    populateKinematicsDisplay();
    populateNumeratorRows();
  } else if (tabId === 'cfg-advanced') {
    syncAdvancedToForm();
  } else if (tabId === 'cfg-paper') {
    // Refresh the matched-papers list so it reflects whatever's on the
    // canvas right now (and the current match results).
    if (!_pdfDoc) _populateMatchedPapersList();
  }
}

function populateConfigPanel() {
  // Settings are now in the integral card — sync them
  syncIntegralCardSettings();

  // Edge table (Tab 2)
  populateEdgeTable();

  // External masses (Tab 2)
  populateExtMasses();

  // Kinematic substitutions display (Tab 2)
  populateKinematicsDisplay();

  // Numerator rows (Tab 3)
  populateNumeratorRows();

  // Advanced options — sync from computeConfig
  syncAdvancedToForm();

  // Export tab
  populateExportTab();

  // Paper tab: list of matched papers when no PDF is currently loaded
  if (!_pdfDoc) _populateMatchedPapersList();

  // Footer
  populateConfigFooter();

  // Disable autocomplete/autocorrect/autocapitalize on all config inputs
  $('config-panel').querySelectorAll('input').forEach(inp => {
    inp.setAttribute('autocomplete', 'off');
    inp.setAttribute('autocorrect', 'off');
    inp.setAttribute('autocapitalize', 'off');
    inp.setAttribute('spellcheck', 'false');
  });
}

function populateEdgeTable() {
  const container = $('cfg-edge-table-container');
  container.innerHTML = '';

  const deg = getVertexDegrees();
  const nEdges = state.edges.length;
  if (nEdges === 0) {
    container.innerHTML = '<div style="font-size:12px;color:var(--text-muted);padding:8px">No edges yet.</div>';
    return;
  }

  // Classify edges
  const intEdges = [];
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    const extA = (deg[e.a] || 0) <= 1, extB = (deg[e.b] || 0) <= 1;
    if (!extA && !extB) intEdges.push(i);
    else if (e.a === e.b) intEdges.push(i); // tadpole
  }

  if (intEdges.length === 0) {
    container.innerHTML = '<div style="font-size:12px;color:var(--text-muted);padding:8px">No internal edges.</div>';
    return;
  }

  const momLabels = getMomentumLabels();

  // Determine which edges are chords
  const chordSet = userChordSet || new Set(_lastAutoChords || []);
  const nLoops = intEdges.length - new Set(intEdges.flatMap(ei => [state.edges[ei].a, state.edges[ei].b])).size + 1;
  const nChords = intEdges.filter(ei => chordSet.has(ei)).length;

  // Header: loop count and chord status
  const header = document.createElement('div');
  header.className = 'cfg-section-title';
  header.style.cssText = 'display:flex;align-items:center;gap:8px';
  header.innerHTML = `<span>Propagators</span>`;
  if (nChords < nLoops) {
    header.innerHTML += `<span style="color:var(--text-muted)">\u00B7</span>
      <span style="font-size:11px;font-weight:400;color:var(--text-mid)">${nChords}/${nLoops} loop momenta</span>
      <span style="font-size:10px;color:var(--text-muted)">\u2190 click \u2113 to select</span>`;
  }
  container.appendChild(header);

  const table = document.createElement('table');
  table.className = 'cfg-edge-table';
  table.innerHTML = `<thead><tr>
    <th style="width:30px;text-align:center" title="Click to assign loop momentum">\u2113</th>
    <th style="width:36px">Edge</th>
    <th>Momentum</th>
    <th style="width:50px;text-align:center">Mass</th>
    <th style="width:50px;text-align:center">Exp</th>
  </tr></thead>`;

  // Map chord edges to their index in the chord list (for custom labels)
  const chordIndexMap = {};
  let chordCounter = 0;
  intEdges.forEach(ei => {
    if (chordSet.has(ei)) chordIndexMap[ei] = chordCounter++;
  });

  const tbody = document.createElement('tbody');
  intEdges.forEach((ei, idx) => {
    const e = state.edges[ei];
    const mom = momLabels ? momLabels[ei] : '';
    const isChord = chordSet.has(ei);
    const tr = document.createElement('tr');
    if (isChord) tr.className = 'chord-row';

    // Chord toggle — clickable
    const tdChord = document.createElement('td');
    tdChord.style.textAlign = 'center';
    const toggle = document.createElement('div');
    toggle.className = 'chord-toggle' + (isChord ? ' active' : '');
    toggle.textContent = isChord ? '\u2113' : '';
    toggle.title = isChord ? 'Remove loop momentum' : 'Assign loop momentum';
    toggle.style.cursor = 'pointer';
    toggle.addEventListener('click', () => toggleChord(ei));
    tdChord.appendChild(toggle);
    tr.appendChild(tdChord);

    // Edge label
    const tdEdge = document.createElement('td');
    tdEdge.innerHTML = `<span style="font-size:11px;font-weight:600;color:var(--text-muted)">${idx+1}</span>`;
    tr.appendChild(tdEdge);

    // Momentum
    const tdMom = document.createElement('td');
    const momInput = document.createElement('input');
    momInput.value = mom || '';
    momInput.addEventListener('focus', () => flashEdge(ei));
    momInput.addEventListener('blur', stopEdgePulse);
    if (isChord) {
      // Chord: editable loop momentum label
      momInput.title = 'Rename loop momentum symbol';
      momInput.addEventListener('change', function() {
        const ci = chordIndexMap[ei];
        if (ci !== undefined) {
          renameLoopMom(ci, this.value);
          populateEdgeTable();
          updateIntegralCard();
        }
      });
    } else {
      // Tree edge: read-only (solved from conservation)
      momInput.readOnly = true;
      momInput.style.color = 'var(--text-muted)';
      momInput.title = 'Solved from momentum conservation';
    }
    tdMom.appendChild(momInput);
    tr.appendChild(tdMom);

    // Mass — use bare 'm' for single-mass diagrams, 'm1' etc. for multi
    const tdMass = document.createElement('td');
    const massInput = document.createElement('input');
    // Display the same canonical label the canvas/notebook would emit.
    // For 'm1' / 'm_{1}' we use the unbracketed form 'm1' as the input
    // value (user-editable); edgeMassToMma turns that into Subscript[m,1]
    // downstream when building the STIntegrate command.
    if (e.mass > 0) {
      if (e.massLabel) {
        massInput.value = e.massLabel;
      } else {
        const kind = getEdgeMassKind(e) || 'm';
        const count = getDistinctMassSlots(kind).size;
        massInput.value = count <= 1 ? kind : `${kind}${e.mass}`;
      }
    } else {
      massInput.value = '0';
    }
    massInput.style.textAlign = 'center';
    massInput.dataset.edgeIdx = ei;
    massInput.addEventListener('focus', () => flashEdge(ei));
    massInput.addEventListener('blur', stopEdgePulse);
    massInput.addEventListener('change', function() {
      const parsed = parseConfigMassInput(this.value, ei);
      state.edges[ei].mass = parsed.mass;
      state.edges[ei].massLabel = parsed.label;
      this.value = parsed.mass > 0 ? (parsed.label || `m${parsed.mass}`) : '0';
      render();
      renderMassLegend();
      onGraphChanged();
    });
    tdMass.appendChild(massInput);
    tr.appendChild(tdMass);

    // Propagator exponent
    const tdExp = document.createElement('td');
    const expInput = document.createElement('input');
    expInput.type = 'number';
    expInput.min = '0';
    expInput.value = e.propExponent ?? 1;
    expInput.style.textAlign = 'center';
    expInput.dataset.edgeIdx = ei;
    expInput.addEventListener('focus', () => flashEdge(ei));
    expInput.addEventListener('blur', stopEdgePulse);
    expInput.addEventListener('change', function() {
      const val = parseInt(this.value);
      if (!isNaN(val) && val >= 0) {
        state.edges[ei].propExponent = val;
        saveDiagram();
        updateIntegralCard();
      } else {
        this.value = state.edges[ei].propExponent ?? 1;
      }
    });
    tdExp.appendChild(expInput);
    tr.appendChild(tdExp);

    tbody.appendChild(tr);
  });
  table.appendChild(tbody);
  container.appendChild(table);

  // Auto / Clear buttons
  const btns = document.createElement('div');
  btns.style.cssText = 'display:flex;gap:6px;margin-top:8px;align-items:center';
  const autoBtn = document.createElement('button');
  autoBtn.className = 'btn btn-sm btn-secondary';
  autoBtn.textContent = 'Auto';
  autoBtn.title = 'Auto-select loop momenta via BFS spanning tree';
  autoBtn.addEventListener('click', autoSelectChords);
  btns.appendChild(autoBtn);
  const clearBtn = document.createElement('button');
  clearBtn.className = 'btn btn-sm btn-secondary';
  clearBtn.textContent = 'Clear';
  clearBtn.title = 'Clear all momentum assignments';
  clearBtn.addEventListener('click', clearChordSelection);
  btns.appendChild(clearBtn);
  container.appendChild(btns);
}

function populateExtMasses() {
  const container = $('cfg-ext-masses');
  container.innerHTML = '';

  const deg = getVertexDegrees();
  const extEdges = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    if (!extA && !extB) continue;
    if (e.a === e.b) continue;
    extEdges.push(i);
  }

  const nLegs = extEdges.length;

  if (nLegs === 0) {
    container.innerHTML = '<div style="font-size:12px;color:var(--text-muted)">No external legs.</div>';
    return;
  }

  // 1-leg: momentum = 0, mass = 0, not editable
  if (nLegs === 1) {
    const edgeIdx = extEdges[0];
    state.edges[edgeIdx].mass = 0;
    state.edges[edgeIdx].massLabel = '';
    const table = document.createElement('table');
    table.className = 'cfg-edge-table';
    table.innerHTML = `<thead><tr><th>Leg</th><th>Mom.</th><th style="width:50px;text-align:center">Mass</th></tr></thead>
      <tbody><tr>
        <td><span style="font-size:11px;font-weight:600;color:var(--text-muted)">p<sub>1</sub></span></td>
        <td><input value="0" disabled title="Fixed by momentum conservation (1-leg)"/></td>
        <td><input value="0" disabled style="text-align:center" title="Fixed (1-leg)"/></td>
      </tr></tbody>`;
    container.appendChild(table);
    return;
  }

  // Table with drag grip + momentum name + mass columns
  const table = document.createElement('table');
  table.className = 'cfg-edge-table';
  table.innerHTML = `<thead><tr><th style="width:20px"></th><th>Leg</th><th>Custom name</th><th style="width:50px;text-align:center">Mass</th></tr></thead>`;
  const tbody = document.createElement('tbody');
  tbody.id = 'ext-mass-tbody';

  function applyMassChange(edgeIdx, input) {
    const parsed = parseConfigMassInput(input.value, edgeIdx);
    state.edges[edgeIdx].mass = parsed.mass;
    state.edges[edgeIdx].massLabel = parsed.label;
    input.value = parsed.mass > 0 ? (parsed.label || `m${parsed.mass}`) : '0';
    render(); renderMassLegend(); onGraphChanged();
    populateKinematicsDisplay();
  }

  const massInputs = [];

  extEdges.forEach((edgeIdx, idx) => {
    const legNum = idx + 1;
    const e = state.edges[edgeIdx];
    const tr = document.createElement('tr');
    tr.className = 'ext-drag-row';
    tr.dataset.extIdx = idx;

    // Drag grip
    const tdGrip = document.createElement('td');
    tdGrip.className = 'ext-drag-grip';
    tdGrip.textContent = '\u2261';  // ≡
    tdGrip.title = 'Drag to reorder this leg';
    tdGrip.addEventListener('pointerdown', (evt) => extGripDown(evt, idx, extEdges, tbody));
    tr.appendChild(tdGrip);

    // Leg label
    const tdLeg = document.createElement('td');
    tdLeg.innerHTML = `<span style="font-size:11px;font-weight:600;color:var(--text-muted)">p<sub>${legNum}</sub></span>`;
    tr.appendChild(tdLeg);

    // Momentum label (editable custom name) — also accepts a reorder hint:
    // typing a bare integer N or "pN" / "p_N" / "p_{N}" in range [1, nLegs]
    // moves this leg to position N (same as drag-to-reorder). Anything else
    // is stored as a custom display name (e.g. k, q_1).
    const tdMom = document.createElement('td');
    const momInput = document.createElement('input');
    momInput.value = e.extMomLabel || '';
    momInput.placeholder = `p_{${legNum}}`;
    momInput.title = 'Rename (e.g. k, q_1), or type a number to move this leg to that position';
    momInput.addEventListener('change', function() {
      const raw = this.value;
      const trimmed = raw.trim();
      const m = trimmed.match(/^\s*p?_?\{?(\d+)\}?\s*$/i);
      if (m) {
        const targetPos = parseInt(m[1], 10);
        if (targetPos >= 1 && targetPos <= nLegs) {
          state.edges[edgeIdx].extMomLabel = '';
          if (targetPos === legNum) {
            this.value = '';
            render(); onGraphChanged();
            return;
          }
          reorderExtLeg(legNum - 1, targetPos - 1);
          const rows = document.querySelectorAll('#ext-mass-tbody .ext-drag-row');
          if (rows[targetPos - 1]) {
            rows[targetPos - 1].classList.add('drop-landed');
            setTimeout(() => rows[targetPos - 1]?.classList.remove('drop-landed'), 800);
          }
          return;
        }
      }
      state.edges[edgeIdx].extMomLabel = raw;
      render(); onGraphChanged();
    });
    momInput.addEventListener('focus', () => flashEdge(edgeIdx));
    momInput.addEventListener('blur', stopEdgePulse);
    tdMom.appendChild(momInput);
    tr.appendChild(tdMom);

    // Mass — show the canonical kind-aware label ('m' / 'M' for single,
    // 'm2' / 'M2' when multiple slots of that kind are on the canvas).
    const tdMass = document.createElement('td');
    const massInput = document.createElement('input');
    if (e.mass > 0) {
      if (e.massLabel) {
        massInput.value = e.massLabel;
      } else {
        const kind = getEdgeMassKind(e) || 'M';  // ext-masses panel defaults M
        const count = getDistinctMassSlots(kind).size;
        massInput.value = count <= 1 ? kind : `${kind}${e.mass}`;
      }
    } else {
      massInput.value = '0';
    }
    massInput.placeholder = '0';
    massInput.style.textAlign = 'center';
    massInput.dataset.extMass = '1';  // marker for querySelectorAll

    if (nLegs === 2) {
      const otherIdx = extEdges[1 - idx];
      massInput.addEventListener('change', function() {
        applyMassChange(edgeIdx, this);
        state.edges[otherIdx].mass = state.edges[edgeIdx].mass;
        state.edges[otherIdx].massLabel = state.edges[edgeIdx].massLabel;
        const otherInput = massInputs[1 - idx];
        if (otherInput) otherInput.value = this.value;
        render(); renderMassLegend(); onGraphChanged();
      });
      if (idx === 1) massInput.title = 'Tied to M\u2081 (2-leg)';
    } else {
      massInput.addEventListener('change', function() { applyMassChange(edgeIdx, this); });
    }
    massInput.addEventListener('focus', () => flashEdge(edgeIdx));
    massInput.addEventListener('blur', stopEdgePulse);
    tdMass.appendChild(massInput);
    tr.appendChild(tdMass);

    tbody.appendChild(tr);
    massInputs.push(massInput);
  });

  table.appendChild(tbody);
  container.appendChild(table);

  // Hint: drag reorders, typed name renames, typed number reorders.
  const hint = document.createElement('div');
  hint.className = 'ext-mass-hint';
  hint.innerHTML =
    'Drag <span class="ext-mass-hint-grip">≡</span> to reorder. ' +
    'Type a name (<code>k</code>, <code>q</code>) to rename, ' +
    'or <code>1</code>–<code>' + nLegs + '</code> (or <code>p<sub>N</sub></code>) ' +
    'to move this leg.';
  container.appendChild(hint);

  // 2-leg: enforce M₂ = M₁ on initial populate
  if (nLegs === 2) {
    const e0 = state.edges[extEdges[0]];
    const e1 = state.edges[extEdges[1]];
    if (e0.mass !== e1.mass || e0.massLabel !== e1.massLabel) {
      state.edges[extEdges[1]].mass = e0.mass;
      state.edges[extEdges[1]].massLabel = e0.massLabel;
      if (massInputs[1]) massInputs[1].value = massInputs[0].value;
    }
  }
}

// ── External momenta drag-to-reorder ─────────────────────────────────

// Move one external leg from position `fromIdx` to `toIdx` (0-based within
// the external-edge list). Mutates state.edges in place (the data at each
// external position is permuted — endpoints are preserved) and runs the full
// update cascade so the Export tab's propagator / quadruple strings refresh.
// Shared by the drag handler and the typed-reorder path in the label input.
function reorderExtLeg(fromIdx, toIdx) {
  const deg = getVertexDegrees();
  const extEdges = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    if (!extA && !extB) continue;
    if (e.a === e.b) continue;
    extEdges.push(i);
  }
  if (fromIdx === toIdx) return false;
  if (fromIdx < 0 || toIdx < 0 ||
      fromIdx >= extEdges.length || toIdx >= extEdges.length) return false;

  const reordered = extEdges.slice();
  const item = reordered.splice(fromIdx, 1)[0];
  reordered.splice(toIdx, 0, item);

  const extPositions = extEdges.slice().sort((a, b) => a - b);
  const reorderedEdges = reordered.map(ei => ({ ...state.edges[ei] }));
  extPositions.forEach((pos, i) => {
    state.edges[pos] = reorderedEdges[i];
  });

  render();
  renderMassLegend();
  onGraphChanged();
  populateExtMasses();
  populateKinematicsDisplay();
  return true;
}

// Transpose two external legs (0-based indices within the external-edge list).
// Unlike reorderExtLeg, this is a direct swap — only these two slots change,
// every other leg stays at its position. Used by the edge-popup "Swap with"
// chips, where the user is focused on one leg and wants it to trade places
// with a specific other leg.
function swapExtLegs(i, j) {
  if (i === j) return false;
  const deg = getVertexDegrees();
  const extEdges = [];
  for (let k = 0; k < state.edges.length; k++) {
    const e = state.edges[k];
    if (e.a === e.b) continue;
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    if (!extA && !extB) continue;
    extEdges.push(k);
  }
  if (i < 0 || j < 0 || i >= extEdges.length || j >= extEdges.length) return false;

  const posI = extEdges[i], posJ = extEdges[j];
  const tmp = state.edges[posI];
  state.edges[posI] = state.edges[posJ];
  state.edges[posJ] = tmp;

  render();
  renderMassLegend();
  onGraphChanged();
  populateExtMasses();
  populateKinematicsDisplay();
  return true;
}

let _extDrag = null;

function extGripDown(evt, idx, extEdges, tbody) {
  if (evt.button !== 0) return;
  evt.preventDefault();
  evt.stopPropagation();

  const rows = Array.from(tbody.querySelectorAll('.ext-drag-row'));
  if (rows.length < 2) return;
  const rowH = rows[0].getBoundingClientRect().height;

  // Mark source row
  rows[idx].classList.add('drag-source');

  // Create ghost
  const ghost = document.createElement('div');
  ghost.className = 'ext-drag-ghost';
  ghost.textContent = `p${idx + 1}`;
  ghost.style.left = evt.clientX + 'px';
  ghost.style.top = evt.clientY + 'px';
  document.body.appendChild(ghost);

  _extDrag = { idx, extEdges: extEdges.slice(), tbody, ghost, rowH, startY: evt.clientY, target: idx };

  document.addEventListener('pointermove', extGripMove);
  document.addEventListener('pointerup', extGripUp);
  document.addEventListener('pointercancel', extGripCancel);
}

function extGripMove(evt) {
  if (!_extDrag) return;
  const { idx, tbody, ghost, rowH, startY } = _extDrag;

  ghost.style.left = evt.clientX + 'px';
  ghost.style.top = evt.clientY + 'px';

  const delta = evt.clientY - startY;
  const rows = Array.from(tbody.querySelectorAll('.ext-drag-row'));
  let target = idx + Math.round(delta / rowH);
  target = Math.max(0, Math.min(target, rows.length - 1));

  if (target !== _extDrag.target) {
    _extDrag.target = target;
    rows.forEach((r, i) => {
      r.classList.remove('drag-shift-up', 'drag-shift-down');
      if (i === idx) return;
      if (target > idx && i > idx && i <= target) r.classList.add('drag-shift-up');
      if (target < idx && i < idx && i >= target) r.classList.add('drag-shift-down');
    });
  }
}

function extGripUp() {
  if (!_extDrag) return;
  const { idx, target } = _extDrag;
  extGripCleanup();

  if (!reorderExtLeg(idx, target)) return;

  // Flash the landed row
  const newRows = document.querySelectorAll('#ext-mass-tbody .ext-drag-row');
  if (newRows[target]) {
    newRows[target].classList.add('drop-landed');
    setTimeout(() => newRows[target]?.classList.remove('drop-landed'), 800);
  }
}

function extGripCancel() { extGripCleanup(); }

function extGripCleanup() {
  if (!_extDrag) return;
  const { ghost, tbody } = _extDrag;
  ghost?.remove();
  tbody?.querySelectorAll('.ext-drag-row').forEach(r => {
    r.classList.remove('drag-source', 'drag-shift-up', 'drag-shift-down');
  });
  document.removeEventListener('pointermove', extGripMove);
  document.removeEventListener('pointerup', extGripUp);
  document.removeEventListener('pointercancel', extGripCancel);
  _extDrag = null;
}

/**
 * Populate the kinematic substitutions display in the Momenta tab.
 * Fetches p_i . p_j rules from the kernel (via /api/kinematics)
 * and renders them in LaTeX via KaTeX.
 */
function populateKinematicsDisplay() {
  const section = $('cfg-kinematics-section');
  const container = $('cfg-kinematics-display');
  if (!section || !container) return;
  container.innerHTML = '';

  // Count external legs
  const extMassInputs = document.querySelectorAll('#cfg-ext-masses [data-ext-mass]');
  const n = extMassInputs.length;
  if (n < 2) { section.style.display = 'none'; return; }
  section.style.display = '';

  // In full mode, fetch from kernel; in lite mode, show nothing (no kernel)
  if (backendMode !== 'full') {
    container.innerHTML = '<div style="font-size:12px;color:var(--text-muted)">Connect to kernel to see kinematic rules.</div>';
    return;
  }

  container.innerHTML = '<div style="font-size:12px;color:var(--text-muted)">Loading...</div>';

  // Count distinct mass scales for single-mass simplification
  const deg = getVertexDegrees();
  const distinctMassScales = new Set();
  for (const e of state.edges) {
    if (e.mass && e.mass > 0) distinctMassScales.add(e.mass);
  }
  const singleMassKin = distinctMassScales.size === 1;

  // Collect current mass values — use same single-mass logic as integration payload
  const masses = Array.from(extMassInputs).map(inp => {
    const v = inp.value.trim();
    if (!v || v === '0') return '0';
    // For single-mass diagrams without custom labels, use bare 'm'
    if (singleMassKin && /^m\d*$/.test(v)) return 'm';
    return massLabelToMma(v);
  });
  const subsInput = $('cfg-substitutions-momenta');
  const substitutions = subsInput ? subsInput.value.trim() || '{}' : '{}';

  // Collect internal (propagator) mass values
  const intMasses = state.edges
    .filter(e => (deg[e.a]||0) > 1 && (deg[e.b]||0) > 1)
    .map(e => e.mass || 0);

  kernel.post('kinematics', { nLegs: n, masses, intMasses, substitutions }).then(data => {
    container.innerHTML = '';
    if (!data || !data.rules) {
      section.style.display = 'none';
      return;
    }

    const katexOpts = { throwOnError: false, displayMode: false, trust: true };

    // Helper: render a row into a grid table
    function addRow(table, lhsTex, rhsTex) {
      const lhsCell = document.createElement('div');
      lhsCell.style.textAlign = 'right';
      if (typeof katex !== 'undefined') {
        katex.render(`${lhsTex} =`, lhsCell, katexOpts);
      } else lhsCell.textContent = lhsTex + ' =';
      table.appendChild(lhsCell);

      const rhsCell = document.createElement('div');
      if (typeof katex !== 'undefined') {
        katex.render(rhsTex, rhsCell, katexOpts);
      } else rhsCell.textContent = rhsTex;
      table.appendChild(rhsCell);
    }

    const table = document.createElement('div');
    table.style.cssText = 'display:grid; grid-template-columns:auto 1fr; gap:1px 12px; align-items:baseline';

    let hasContent = false;

    // External mass rules: p_i^2 = ... (skip massless)
    const extRules = (data.extMassRules || []).filter(r => !r.zero);
    for (const r of extRules) {
      addRow(table, r.lhs, r.rhs);
      hasContent = true;
    }

    // Mandelstam rules (off-diagonal p_i . p_j)
    const offDiag = data.rules.filter(r => !r.diagonal);
    for (const r of offDiag) {
      addRow(table, r.lhs, r.rhs);
      hasContent = true;
    }

    // Internal mass rules: m_i^2 = mm_i (skip massless)
    const intRules = (data.intMassRules || []).filter(r => !r.zero);
    for (const r of intRules) {
      addRow(table, r.lhs, r.rhs);
      hasContent = true;
    }

    if (hasContent) {
      container.appendChild(table);
    } else {
      section.style.display = 'none';
    }
  }).catch(() => {
    container.innerHTML = '<div style="font-size:12px;color:var(--text-muted)">Could not load kinematic rules.</div>';
  });

  // Add change listener for refetch (only once)
  const subsMomenta = $('cfg-substitutions-momenta');
  if (subsMomenta && !subsMomenta._kinListener) {
    subsMomenta._kinListener = true;
    // Sync initial value from computeConfig
    subsMomenta.value = computeConfig.substitutions || '{}';
    subsMomenta.addEventListener('change', () => {
      computeConfig.substitutions = subsMomenta.value;
      const subsAdv = $('cfg-substitutions');
      if (subsAdv) subsAdv.value = subsMomenta.value;
      populateKinematicsDisplay();
      // Refresh the Export tab so the Substitutions option appears in the
      // printed STIntegrate command immediately, not only after panel reopen.
      updateIntegralCard();
    });
  }
}

function populateNumeratorRows() {
  const container = $('cfg-numerator-rows');
  container.innerHTML = '';
  computeConfig.numeratorRows.forEach((nr, i) => {
    const row = document.createElement('div');
    row.className = 'cfg-numerator-row';
    row.innerHTML = `
      <span class="cfg-num-label">${i + 1}.</span>
      <input class="cfg-input" placeholder="e.g. l.p or (l+p)^2" value="${nr.expr || ''}" data-idx="${i}" data-field="expr" autocomplete="off"/>
      <input class="cfg-input cfg-input-exp" type="number" max="0" step="1" placeholder="-1" value="${nr.exp || '-1'}" data-idx="${i}" data-field="exp" autocomplete="off"/>
    `;
    row.querySelectorAll('input').forEach(inp => {
      inp.addEventListener('input', function() {
        const idx = parseInt(this.dataset.idx);
        if (this.dataset.field === 'exp') {
          const val = parseInt(this.value);
          if (!isNaN(val) && val > 0) { this.value = 0; }
          computeConfig.numeratorRows[idx].exp = this.value;
        } else {
          computeConfig.numeratorRows[idx][this.dataset.field] = this.value;
        }
        updateIntegralCard();
      });
    });

    container.appendChild(row);
  });
}

function addNumeratorRow() {
  computeConfig.numeratorRows.push({ expr: '', exp: '-1' });
  populateNumeratorRows();
}

function removeNumeratorRow() {
  if (computeConfig.numeratorRows.length > 0) {
    computeConfig.numeratorRows.pop();
    populateNumeratorRows();
  }
}

function syncAdvancedToForm() {
  const c = computeConfig;
  $('cfg-representation').value = c.representation;
  $('cfg-auto-norm').checked = c.normalization === 'Automatic';
  $('cfg-normalization').value = c.normalization === 'Automatic' ? '1' : c.normalization;
  $('cfg-normalization').disabled = c.normalization === 'Automatic';
  $('cfg-substitutions').value = c.substitutions;
  if ($('cfg-substitutions-momenta')) $('cfg-substitutions-momenta').value = c.substitutions;
  $('cfg-simplify').value = c.simplifyOutput;
  $('cfg-clean-output').checked = c.cleanOutput;
  $('cfg-contour').value = c.contourHandling;
  $('cfg-verbose').checked = c.verbose;
  $('cfg-show-timings').checked = c.showTimings;
  $('cfg-show-integrands').checked = c.showIntegrands;
  $('cfg-save-slowest').checked = c.saveSlowest;
  $('cfg-save-all').checked = c.saveAll;
  $('cfg-auto-gauge').checked = c.gauge === 'Automatic';
  $('cfg-gauge').value = c.gauge === 'Automatic' ? '{x1 -> 1}' : c.gauge;
  $('cfg-gauge').disabled = c.gauge === 'Automatic';
  $('cfg-include-gauges').value = c.includeGauges;
  $('cfg-heuristic').value = c.heuristic;
  $('cfg-score-interval').value = c.scanScoreInterval;
  $('cfg-score-parallel').value = c.scoreInParallel;
  $('cfg-time-bound').value = c.timeUpperBound;
  $('cfg-memory-cutoff').value = c.memoryCutOff;
  $('cfg-scoring-mem').value = c.scoringMemFrac;
  $('cfg-parallelization').value = c.parallelization;
  $('cfg-kernels').value = c.kernels;
  $('cfg-setup-parallel').value = c.setupInParallel;
  $('cfg-clear-caches').checked = c.clearCaches;
  $('cfg-reuse-results').checked = c.reuseResults;
  $('cfg-find-roots').checked = c.findRoots;
  $('cfg-method-lr').value = c.methodLR;
  $('cfg-select-faces').value = c.selectFaces;
  $('cfg-stop-at').value = c.stopAt;
  $('cfg-start-at').value = c.startAt;
}

function syncConfigToState() {
  // Sync settings from integral card
  const c = computeConfig;
  c.diagramName = $('ic-name') ? $('ic-name').value : c.diagramName;
  c.autoName = !_nameEditedByUser;
  c.dimension = $('ic-dimension') ? $('ic-dimension').value : c.dimension;
  c.epsOrder = $('ic-eps-order') ? $('ic-eps-order').value : c.epsOrder;

  // Config panel may not have been opened yet
  if (!$('cfg-representation')) return;

  c.representation = $('cfg-representation').value;
  c.normalization = $('cfg-auto-norm').checked ? 'Automatic' : $('cfg-normalization').value;
  // Sync substitutions: prefer Momenta tab input, fall back to Advanced
  const subsMom = $('cfg-substitutions-momenta');
  const subsAdv = $('cfg-substitutions');
  if (subsMom && subsMom.value !== c.substitutions) {
    c.substitutions = subsMom.value;
    if (subsAdv) subsAdv.value = subsMom.value;
  } else if (subsAdv) {
    c.substitutions = subsAdv.value;
    if (subsMom) subsMom.value = subsAdv.value;
  }
  c.simplifyOutput = $('cfg-simplify').value;
  c.cleanOutput = $('cfg-clean-output').checked;
  c.contourHandling = $('cfg-contour').value;
  c.verbose = $('cfg-verbose').checked;
  c.showTimings = $('cfg-show-timings').checked;
  c.showIntegrands = $('cfg-show-integrands').checked;
  c.saveSlowest = $('cfg-save-slowest').checked;
  c.saveAll = $('cfg-save-all').checked;
  c.gauge = $('cfg-auto-gauge').checked ? 'Automatic' : $('cfg-gauge').value;
  c.includeGauges = $('cfg-include-gauges').value;
  c.heuristic = $('cfg-heuristic').value;
  c.scanScoreInterval = $('cfg-score-interval').value;
  c.scoreInParallel = $('cfg-score-parallel').value;
  c.timeUpperBound = $('cfg-time-bound').value;
  c.memoryCutOff = $('cfg-memory-cutoff').value;
  c.scoringMemFrac = $('cfg-scoring-mem').value;
  c.parallelization = $('cfg-parallelization').value;
  c.kernels = $('cfg-kernels').value;
  c.setupInParallel = $('cfg-setup-parallel').value;
  c.clearCaches = $('cfg-clear-caches').checked;
  c.reuseResults = $('cfg-reuse-results').checked;
  c.findRoots = $('cfg-find-roots').checked;
  c.methodLR = $('cfg-method-lr').value;
  c.selectFaces = $('cfg-select-faces').value;
  c.stopAt = $('cfg-stop-at').value;
  c.startAt = $('cfg-start-at').value;

  saveDiagram();
}

// ─── Export tab ─────────────────────────────────────────────────────

// Kernel emits CenterDot as the U+00B7 glyph (·), which can arrive as "Â·"
// mojibake when the transport mis-decodes UTF-8 as Latin-1. Normalize both
// forms to the ASCII-safe `\[CenterDot]` escape, which round-trips cleanly
// through copy-paste into Mathematica.
function fixCenterDot(s) {
  return s ? s.replace(/\u00c2\u00b7|\u00b7/g, '\\[CenterDot]') : s;
}

function buildSTIntegrateOpts() {
  syncConfigToState();
  const c = computeConfig;
  const d = COMPUTE_CONFIG_DEFAULTS;
  const opts = [];

  // Helper: emit option only if value differs from default
  const str = (key, optName, def) => { if (c[key] && c[key] !== def) opts.push(`"${optName}" -> "${c[key]}"`); };
  const val = (key, optName, def) => { if (c[key] && c[key] !== def) opts.push(`"${optName}" -> ${c[key]}`); };
  const sym = (key, optName, def) => {
    if (c[key] && c[key] !== def) {
      const v = c[key];
      // Capitalize Mathematica symbols: All, None, Automatic, True, False
      const mma = v === 'All' || v === 'None' || v === 'Automatic' || v === 'True' || v === 'False'
        || v === 'Simplify' || v === 'FullSimplify' || v === 'Identity' ? v : `"${v}"`;
      opts.push(`"${optName}" -> ${mma}`);
    }
  };
  const bool = (key, optName, def) => {
    if (c[key] !== def) opts.push(`"${optName}" -> ${c[key] ? 'True' : 'False'}`);
  };

  // Basic
  val('dimension', 'Dimension', d.dimension);
  val('epsOrder', 'Order', d.epsOrder);

  // Core
  str('representation', 'Representation', d.representation);
  if (c.normalization && c.normalization !== d.normalization) opts.push(`"Normalization" -> ${c.normalization}`);
  if (c.substitutions && c.substitutions !== d.substitutions) opts.push(`"Substitutions" -> ${c.substitutions}`);

  // Output
  sym('simplifyOutput', 'SimplifyOutput', d.simplifyOutput);
  bool('cleanOutput', 'CleanOutput', d.cleanOutput);
  str('contourHandling', 'ContourHandling', d.contourHandling);
  bool('verbose', 'Verbose', d.verbose);
  bool('showTimings', 'ShowTimings', d.showTimings);
  bool('showIntegrands', 'ShowIntegrands', d.showIntegrands);
  bool('saveSlowest', 'SaveSlowestIntegrand', d.saveSlowest);
  bool('saveAll', 'SaveAllIntegrands', d.saveAll);

  // Gauge & LR
  if (c.gauge && c.gauge !== d.gauge) opts.push(`"Gauge" -> ${c.gauge}`);
  sym('includeGauges', 'IncludeGauges', d.includeGauges);
  str('methodLR', 'MethodLR', d.methodLR);
  bool('findRoots', 'FindRoots', d.findRoots);

  // Scoring
  str('heuristic', 'Heuristic', d.heuristic);
  val('scanScoreInterval', 'ScanScoreInterval', d.scanScoreInterval);
  sym('scoreInParallel', 'ScoreInParallel', d.scoreInParallel);
  val('timeUpperBound', 'TimeUpperBound', d.timeUpperBound);
  sym('memoryCutOff', 'MemoryPercentCutOff', d.memoryCutOff);
  val('scoringMemFrac', 'ScoringMemoryFraction', d.scoringMemFrac);

  // Parallelization
  sym('parallelization', 'Parallelization', d.parallelization);
  if (c.kernels && c.kernels !== d.kernels) opts.push(`"KernelsAvailable" -> ${c.kernels}`);
  sym('setupInParallel', 'SetupInParallel', d.setupInParallel);

  // Execution
  str('stopAt', 'StopAt', d.stopAt);
  sym('startAt', 'StartAt', d.startAt);
  sym('selectFaces', 'SelectFaces', d.selectFaces);
  bool('reuseResults', 'ReuseExistingResults', d.reuseResults);
  bool('clearCaches', 'ClearCachesPerIntegrand', d.clearCaches);

  return opts;
}

/**
 * Format a momentum coefficient vector as a Mathematica InputForm string.
 * Basis: [l[1],...,l[L], p[1],...,p[E]].  Used by buildPropsInfoJS to
 * emit inverse propagators that match the kernel's autoRouteMomenta +
 * ToString[..., InputForm] path on $STPropagators.
 *
 * Examples:
 *   [1,0,-1,0]  (L=2, E=2)   -> "l[1] - p[1]"
 *   [2,0,0,0]                -> "2*l[1]"
 *   [0,0,0,0]                -> "0"
 *   [-1,0,0,0]               -> "-l[1]"
 */
function formatMomentumMma(vec, nLoops) {
  const terms = [];
  for (let i = 0; i < vec.length; i++) {
    const c = vec[i];
    if (Math.abs(c) < 1e-10) continue;
    const name = i < nLoops ? `l[${i + 1}]` : `p[${i - nLoops + 1}]`;
    if (Math.abs(c - 1) < 1e-10) terms.push({ sign: 1, body: name });
    else if (Math.abs(c + 1) < 1e-10) terms.push({ sign: -1, body: name });
    else {
      const ac = Math.abs(c);
      // Integer coefficients render without trailing ".0"
      const coeffStr = Number.isInteger(ac) ? String(ac) : String(ac);
      terms.push({ sign: c > 0 ? 1 : -1, body: `${coeffStr}*${name}` });
    }
  }
  if (terms.length === 0) return '0';
  let out = '';
  for (let i = 0; i < terms.length; i++) {
    const t = terms[i];
    if (i === 0) out += (t.sign < 0 ? '-' : '') + t.body;
    else out += (t.sign < 0 ? ' - ' : ' + ') + t.body;
  }
  return out;
}

/**
 * Format an inverse propagator k^2 - m^2 as Mathematica InputForm, matching
 * the kernel's ToString[k^2 - m^2, InputForm] rendering.  Rules:
 *   - k = 0:            massless -> "0";  massive -> "-<mass>^2"
 *   - k = single term:  squaring eats the sign (Mathematica auto-simplifies
 *                       Power[Times[-1, x], 2] -> Power[x, 2]);  emit "x^2".
 *   - k = multi-term:   wrap in parens, emit "(<k>)^2".
 * Mass symbol is appended only when non-zero; matches edgeMassToMma's "0"
 * sentinel for massless edges.
 */
function formatPropMma(vec, nLoops, massSymbol) {
  const isZero = vec.every(c => Math.abs(c) < 1e-10);
  if (isZero) return massSymbol === '0' ? '0' : `-${massSymbol}^2`;
  const kStr = formatMomentumMma(vec, nLoops);
  const hasMultipleTerms = /\s[+\-]\s/.test(kStr);
  const leadingMinus = kStr.startsWith('-');
  let k2;
  if (hasMultipleTerms) k2 = `(${kStr})^2`;
  else if (leadingMinus) k2 = kStr.slice(1) + '^2';
  else k2 = kStr + '^2';
  return massSymbol === '0' ? k2 : `${k2} - ${massSymbol}^2`;
}

/**
 * Client-side propagator list + exponent list builder.  Mirrors the kernel
 * path (handleEstimate, SubTropica.wl:~19189-19232): same momentum routing
 * via solveMomentaRaw, same l[i]/p[i] labels, same (k^2 - m^2) form, same
 * numerator-row append behaviour.  Used to default the Export panel to
 * kernel-free output (works in STBrowser[] mode).  Returns null when the
 * diagram can't be routed; populateExportTab falls back to the kernel's
 * propsStr in that case.
 *
 * Returned shape:
 *   { propsArg: "{(l[1])^2, (l[1] - p[1])^2 - m^2, ...}",
 *     exponentsStr: "{1, 1, -1}" or "" when all unit }
 */
function buildPropsInfoJS() {
  const r = solveMomentaRaw();
  if (!r) return null;
  const { momenta, nLoops, isExternal } = r;

  const propStrs = [];
  const exps = [];
  for (let i = 0; i < state.edges.length; i++) {
    if (isExternal[i]) continue;
    const vec = momenta[i];
    if (!vec) return null;  // incomplete routing -> fall back to kernel
    const massSym = edgeMassToMma(state.edges[i]);
    propStrs.push(formatPropMma(vec, nLoops, massSym));
    exps.push(state.edges[i].propExponent ?? 1);
  }

  // Append numerator rows in the order they appear in the UI.  Matches
  // handleEstimate's AppendTo on $STPropagators / fullExpsEst (SubTropica.wl
  // ~19217-19221).  Numerator exponent defaults to -1 per computeConfig
  // defaults (app.js:1144).
  const numRows = (computeConfig.numeratorRows || []).filter(r => r && r.expr && r.expr.trim());
  for (const nr of numRows) {
    const mmaExpr = parsePhysicsExpr(nr.expr.trim()).mma;
    propStrs.push(mmaExpr);
    const expNum = Number(nr.exp);
    exps.push(Number.isFinite(expNum) ? expNum : -1);
  }

  if (propStrs.length === 0) return null;
  const allUnit = exps.every(e => e === 1);
  return {
    propsArg: `{${propStrs.join(', ')}}`,
    exponentsStr: allUnit ? '' : `{${exps.join(', ')}}`,
  };
}

// Build M[i] -> <external mass symbol> substitutions, one per external leg,
// in canvas state.edges order (filtered to external, matching buildGraphArgJS
// node ordering which the kernel's props-form path also uses).  Always
// emitted for the props form regardless of the mass value, since propagators
// only carry the symbolic p[i] and evaluating the kinematics requires the
// M[i] -> ... mapping.  Returns an array of rule strings like
// ["M[1] -> 0", "M[2] -> m", ...].
function buildPropsExtMassSubsJS() {
  const deg = getVertexDegrees();
  const rules = [];
  let legIdx = 0;
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a === e.b) continue;  // self-loop can't be external
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    if (!(extA || extB)) continue;
    legIdx += 1;
    const label = edgeMassToMma(e);
    rules.push(`M[${legIdx}] -> ${label}`);
  }
  return rules;
}

// Build the graph-form argument string from canvas state (no kernel needed)
function buildGraphArgJS() {
  const deg = getVertexDegrees();

  // Canonicalize vertex IDs to contiguous 1..N in first-appearance
  // order. The raw canvas indices can start at any value and have gaps
  // (e.g. after a delete-vertex edit that leaves the remaining indices
  // non-contiguous), which leaked into the emitted STIntegrate command
  // as surprises like {{4,5},0} when the user expected {{1,2},0}. The
  // kernel renumbers internally in feynmanAnalyzeTopology, so this
  // is purely cosmetic but matches what the kernel actually runs.
  const canon = new Map();
  let nextId = 0;
  const getId = raw => {
    let id = canon.get(raw);
    if (id === undefined) { id = ++nextId; canon.set(raw, id); }
    return id;
  };

  const edges = [];
  const nodes = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const extA = (e.a !== e.b) && (deg[e.a] || 0) <= 1;
    const extB = (e.a !== e.b) && (deg[e.b] || 0) <= 1;
    const mLabel = edgeMassToMma(e);
    if (extA || extB) {
      const intV = extA ? e.b : e.a;
      nodes.push(`{${getId(intV)}, ${mLabel}}`);
    } else {
      // Normalize endpoint order so the emitted pair is always sorted.
      // state.edges can hold unsorted (a,b) for edges created via the
      // drag-to-empty-space path (L3051 stores {a: newVertex, b: dragOrigin}
      // to encode arrow direction for external legs). For internal edges
      // that aren't external, endpoint order carries no physical meaning
      // and downstream tooling expects {min, max}.
      const ia = getId(e.a);
      const ib = getId(e.b);
      const la = Math.min(ia, ib);
      const lb = Math.max(ia, ib);
      edges.push(`{{${la}, ${lb}}, ${mLabel}}`);
    }
  }
  if (edges.length === 0) return null;
  return `{\n  {${edges.join(', ')}},\n  {${nodes.join(', ')}}}`;
}

function populateExportTab() {
  const stats = getTopologyStats();
  const hasGraph = stats.nIntEdges > 0;
  const opts = buildSTIntegrateOpts();
  const optStr = opts.length > 0 ? ',\n  ' + opts.join(',\n  ') : '';

  // Variable chips (full mode only — section hidden by CSS in lite mode)
  const varsContainer = $('export-vars');
  if (varsContainer) {
    const vars = [
      { name: '$STGraph', desc: '{edges, nodes}' },
      { name: '$STQuadruple', desc: '{pref, integrand, vars, coeffs}' },
      { name: '$STPropagators', desc: 'inverse propagators' },
      { name: '$STEdges', desc: 'edge list' },
      { name: '$STNodes', desc: 'node list' },
      { name: '$STPrefactor', desc: 'prefactor' },
      { name: '$STIntegrand', desc: 'integrand' },
      { name: '$STVariables', desc: 'Schwinger vars' },
      { name: '$STCoefficients', desc: 'coefficients' },
    ];
    varsContainer.innerHTML = vars.map(v =>
      `<span class="export-var" title="${v.desc} — click to copy" data-copy="${v.name}">` +
      `<span class="export-var-icon">\u2398</span>${v.name}</span>`
    ).join('');
  }

  // STIntegrate commands
  // Graph form: always available (built client-side), refined by Tier 2
  // Propagator + Quadruple: only available from Tier 2 (require kernel)
  const codeGraph = $('export-code-graph');
  const codeProps = $('export-code-props');
  const codeQuad = $('export-code-quad');

  // Prefer full Tier-2 data (richer), fall back to the light
  // integrand-only cache populated by the Export-tab background prefetch.
  const t2 = _cachedTier2 && _cachedTier2.result;
  const ib = _cachedIntegrand && _cachedIntegrand.result;
  const srcProps = t2 || ib;  // whichever is available for props/quad
  const estimatePending = hasGraph && backendMode === 'full' &&
    (_estimateTimer || _estimateAbort || _integrandAbort);
  const graphArg = (srcProps && srcProps.graphStr) || (hasGraph ? buildGraphArgJS() : null);
  // JS-side props builder: default source so the Export panel works in
  // STBrowser[] mode (no kernel) and stays in sync with the canvas state
  // without a round-trip to Mathematica.  Kernel's propsStr used as fallback
  // when JS can't route (incomplete diagram).  On divergence, console-warn
  // so drift from the kernel path surfaces early.
  const jsProps = hasGraph ? buildPropsInfoJS() : null;
  const kernelPropsArg = (srcProps && srcProps.propsStr) || null;
  const propsArg = (jsProps && jsProps.propsArg) || kernelPropsArg;
  if (jsProps && kernelPropsArg && jsProps.propsArg !== kernelPropsArg) {
    const normalize = s => String(s).replace(/\s+/g, ' ').trim();
    if (normalize(jsProps.propsArg) !== normalize(kernelPropsArg)) {
      // eslint-disable-next-line no-console
      console.warn('[Export] JS props differ from kernel propsStr',
        { js: jsProps.propsArg, kernel: kernelPropsArg });
    }
  }
  const quadArg  = (srcProps && srcProps.quadStr)  || null;
  // Tier 2 returns exponentsStr (e.g. "{1, 1, -1}") whenever the propagator
  // list has non-unit exponents — either from user-set propExponents or from
  // numerator rows, which handleEstimate appends to $STPropagators.  Both
  // the propagator form and the graph form need an explicit "Exponents"
  // option so copy-paste gives the correct integral.  (Graph form is hidden
  // entirely when a numerator row is present — see hasNumerator branch
  // below — so its exponents list only carries the user-set propExponents.)
  // Prefer JS-computed exponents so no-kernel mode still picks up user-set
  // propExponents; fall back to kernel's exponentsStr / graphExponentsStr.
  const jsExponentsStr = (jsProps && jsProps.exponentsStr) || '';
  const exponentsStr = jsExponentsStr || (srcProps && srcProps.exponentsStr) || '';
  const graphExponentsStr = jsExponentsStr
    || (srcProps && srcProps.graphExponentsStr)
    || (srcProps && srcProps.exponentsStr)
    || '';

  // Props-form "Substitutions": always prepend M[i] -> <label> for every
  // external leg.  User-typed subs from the config panel are appended AFTER
  // the ext-mass rules so they override on duplicate LHS (Mathematica's /.
  // applies rules in sequence, later wins).  Graph and quad forms are
  // unaffected: graph encodes external masses on the node list, quad
  // form's Euler integrand uses MM/mm[i] placeholders, not M[i].
  const extMassRules = buildPropsExtMassSubsJS();
  const userSubsRaw = (computeConfig.substitutions || '').trim();
  const userSubsBody = (userSubsRaw.startsWith('{') && userSubsRaw.endsWith('}'))
    ? userSubsRaw.slice(1, -1).trim()
    : userSubsRaw;
  let propsSubsListStr = '';
  if (extMassRules.length > 0 && userSubsBody) {
    propsSubsListStr = `{${extMassRules.join(', ')}, ${userSubsBody}}`;
  } else if (extMassRules.length > 0) {
    propsSubsListStr = `{${extMassRules.join(', ')}}`;
  } else if (userSubsBody) {
    propsSubsListStr = `{${userSubsBody}}`;
  }
  const optsForProps = opts.filter(s => !s.startsWith('"Substitutions"'));
  if (propsSubsListStr) optsForProps.push(`"Substitutions" -> ${propsSubsListStr}`);
  const propsBaseOptStr = optsForProps.length > 0
    ? ',\n  ' + optsForProps.join(',\n  ')
    : '';

  const propsOptStr = exponentsStr
    ? propsBaseOptStr + ',\n  "Exponents" -> ' + exponentsStr
    : propsBaseOptStr;
  const graphOptStr = graphExponentsStr
    ? optStr + ',\n  "Exponents" -> ' + graphExponentsStr
    : optStr;

  // Graph form {edges, nodes} carries endpoints and masses only — it has
  // no slot for a numerator. As soon as the user adds a numerator row, the
  // graph-form command is strictly incomplete, so hide it and show a note
  // pointing the user to the propagator form instead.
  const hasNumerator = (computeConfig.numeratorRows || []).some(r => r && r.expr && r.expr.trim());
  const graphCmdEl = $('export-cmd-graph');
  const graphNoteEl = (() => {
    let n = document.getElementById('export-cmd-graph-note');
    if (!n && graphCmdEl) {
      n = document.createElement('div');
      n.id = 'export-cmd-graph-note';
      n.className = 'export-cmd-note';
      n.style.cssText = 'font-size:11px;color:var(--text-muted);padding:10px 12px;line-height:1.45;border:1px dashed var(--border);border-radius:6px;background:var(--panel-alt,rgba(127,127,127,0.05))';
      graphCmdEl.appendChild(n);
    }
    return n;
  })();

  if (hasGraph) {
    if (codeGraph && codeProps && codeQuad) {
      if (hasNumerator) {
        // Hide the graph-form command entirely; surface the explanation.
        codeGraph.textContent = '';
        codeGraph.style.display = 'none';
        if (graphNoteEl) {
          graphNoteEl.style.display = '';
          graphNoteEl.innerHTML =
            'Graph form <code>{{edges}, {nodes}}</code> cannot encode ' +
            'numerators \u2014 the schema only carries endpoints and masses. ' +
            'Use the <b>Propagator form</b> below instead; it lists the ' +
            'numerator as an extra propagator and carries the corresponding ' +
            '<code>"Exponents"</code> option.';
        }
      } else {
        codeGraph.style.display = '';
        if (graphNoteEl) graphNoteEl.style.display = 'none';
        codeGraph.textContent = graphArg ? `STIntegrate[${graphArg}${graphOptStr}]` : '';
      }
    }
    // Prop / Quad forms come from either a full Tier-2 run or the
    // lightweight background prefetch (skipExpansion). The empty-before-
    // prefetch window is short (sub-second for simple diagrams) so we
    // just show a "Generating..." placeholder instead of the old
    // "press Check complexity" stub that required a manual click.
    const generatingStub = '(generating\u2026)';
    if (codeProps) {
      if (propsArg) { codeProps.textContent = fixCenterDot(`STIntegrate[${propsArg}${propsOptStr}]`); delete codeProps.dataset.loading; delete codeProps.dataset.stub; }
      else if (estimatePending) { codeProps.textContent = ''; codeProps.dataset.loading = '1'; delete codeProps.dataset.stub; }
      else { codeProps.textContent = generatingStub; delete codeProps.dataset.loading; codeProps.dataset.stub = '1'; }
    }
    if (codeQuad) {
      if (quadArg) { codeQuad.textContent = fixCenterDot(`STIntegrate[${quadArg}${optStr}]`); delete codeQuad.dataset.loading; delete codeQuad.dataset.stub; }
      else if (estimatePending) { codeQuad.textContent = ''; codeQuad.dataset.loading = '1'; delete codeQuad.dataset.stub; }
      else { codeQuad.textContent = generatingStub; delete codeQuad.dataset.loading; codeQuad.dataset.stub = '1'; }
    }
  } else {
    if (codeGraph) { codeGraph.textContent = ''; codeGraph.style.display = ''; }
    if (graphNoteEl) graphNoteEl.style.display = 'none';
    if (codeProps) { codeProps.textContent = ''; delete codeProps.dataset.loading; }
    if (codeQuad) { codeQuad.textContent = ''; delete codeQuad.dataset.loading; }
  }
}

// Copy handlers for export tab
document.addEventListener('click', (evt) => {
  // Variable chips
  const varChip = evt.target.closest('.export-var');
  if (varChip) {
    const text = varChip.dataset.copy;
    if (text) navigator.clipboard.writeText(text).then(() => showWarningToast('Copied ' + text));
    return;
  }
  // Command copy buttons
  const copyBtn = evt.target.closest('.export-copy');
  if (copyBtn) {
    const targetId = copyBtn.dataset.target;
    const pre = document.querySelector(`#${targetId} .export-code`);
    if (pre && pre.textContent) {
      navigator.clipboard.writeText(pre.textContent).then(() => showWarningToast('Copied to clipboard'));
    }
  }
});

// ─────────────────────────────────────────────────────────────────────

function populateConfigFooter() {
  const footer = $('config-footer');
  footer.innerHTML = '';
  if (backendMode !== 'full') {
    const link = document.createElement('a');
    link.className = 'btn btn-sm btn-primary';
    link.href = 'https://github.com/SubTropica/SubTropica';
    link.target = '_blank';
    link.rel = 'noopener';
    link.innerHTML = '<svg class="ico" viewBox="0 0 24 24" style="stroke:#fff"><path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg> Get SubTropica';
    footer.appendChild(link);
  }
  // Hide footer when empty to avoid stray border
  footer.style.display = footer.children.length > 0 ? '' : 'none';
}

function collectIntegrationPayload() {
  syncConfigToState();
  const deg = getVertexDegrees();
  const nEdges = state.edges.length;

  // Count distinct mass scales (for label simplification)
  const distinctMasses = new Set();
  for (const e of state.edges) {
    if (e.mass && e.mass > 0) distinctMasses.add(e.mass);
  }
  const singleMass = distinctMasses.size === 1;

  // Build edge pairs for the kernel (1-indexed vertex labels)
  const edgePairs = [];
  const internalMasses = [];
  const propExponents = [];
  const intEdgeIndices = [];
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    // Include all edges (kernel distinguishes internal/external)
    edgePairs.push([e.a, e.b]);
    // Kind-aware emit via edgeMassToMma: bare 'm'/'M' for single slot,
    // Subscript[m,N] / Subscript[M,N] for multi-slot. Sanitizes user-typed
    // custom labels to atomic. See edgeMassToMma definition for details.
    const massLabel = edgeMassToMma(e);
    internalMasses.push(massLabel);
    propExponents.push(e.propExponent ?? 1);
  }

  // External masses: prefer config panel inputs, fall back to edge state
  const extMassInputs = document.querySelectorAll('#cfg-ext-masses [data-ext-mass]');
  let externalMasses;
  if (extMassInputs.length > 0) {
    externalMasses = Array.from(extMassInputs).map(inp => {
      const v = inp.value.trim();
      if (!v || v === '0') return '0';
      return massLabelToMma(v);
    });
  } else {
    // Config panel not opened — read directly from edge state
    const extEdgeList = [];
    for (let i = 0; i < nEdges; i++) {
      const e = state.edges[i];
      if (e.a === e.b) continue;
      if ((deg[e.a]||0) <= 1 || (deg[e.b]||0) <= 1) extEdgeList.push(i);
    }
    externalMasses = extEdgeList.map(i => edgeMassToMma(state.edges[i]));
  }

  // Compute JS chord positions for kernel sync.
  // Ensures the kernel uses the same chord edges as the UI display.
  getMomentumLabels(); // force momentum solver to run (populates _lastAutoChords)
  const intEdgesList = [];
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    if ((deg[e.a] || 0) > 1 && (deg[e.b] || 0) > 1) intEdgesList.push(i);
  }
  const chordSet = userChordSet || new Set(_lastAutoChords || []);
  // 1-indexed positions in the internal-edge list = edgelistfinal indices in Mathematica
  const jsChordPositions = [];
  intEdgesList.forEach((ei, idx) => {
    if (chordSet.has(ei)) jsChordPositions.push(idx + 1);
  });

  // Collect external momentum labels (in external-edge order)
  const extEdgeList = [];
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    if (e.a === e.b) continue;
    if ((deg[e.a]||0) <= 1 || (deg[e.b]||0) <= 1) extEdgeList.push(i);
  }
  const extMomLabels = extEdgeList.map(i => {
    const label = state.edges[i].extMomLabel;
    return (label && label.trim()) ? parsePhysicsExpr(label.trim()).mma : '';
  });
  const hasCustomExtMom = extMomLabels.some(l => l !== '');

  const c = computeConfig;
  return {
    edgePairs,
    internalMasses,
    externalMasses,
    propExponents,
    jsChordPositions,
    jsNickel: currentNickel || '',  // JS-computed canonical Nickel
    numeratorRows: c.numeratorRows.filter(r => r.expr).map(r => ({
      expr: parsePhysicsExpr(r.expr).mma, exp: r.exp
    })),
    dimension: c.dimension,
    epsOrder: c.epsOrder,
    autoName: c.autoName,
    diagramName: c.diagramName,
    autoExtMom: !hasCustomExtMom,
    autoLoopMom: true,
    extMomLabels,
    loopMomLabels: [],
    representation: c.representation,
    normalization: c.normalization,
    substitutions: c.substitutions ? parsePhysicsExpr(c.substitutions).mma : '{}',
    simplifyOutput: c.simplifyOutput,
    cleanOutput: c.cleanOutput,
    contourHandling: c.contourHandling,
    verbose: c.verbose,
    showTimings: c.showTimings,
    showIntegrands: c.showIntegrands,
    saveSlowest: c.saveSlowest,
    saveAll: c.saveAll,
    gauge: c.gauge,
    includeGauges: c.includeGauges,
    heuristic: c.heuristic,
    scanScoreInterval: c.scanScoreInterval,
    scoreInParallel: c.scoreInParallel,
    timeUpperBound: c.timeUpperBound,
    memoryCutOff: c.memoryCutOff,
    scoringMemFrac: c.scoringMemFrac,
    parallelization: c.parallelization,
    kernels: c.kernels,
    clearCaches: c.clearCaches,
    reuseResults: c.reuseResults,
    selectFaces: c.selectFaces,
    stopAt: c.stopAt,
    startAt: c.startAt,
    setupInParallel: c.setupInParallel,
    findRoots: c.findRoots,
    methodLR: c.methodLR,
  };
}

let _integrationPolling = null;
let _integrationDone = false;
let _progressAnimFrame = null;
let _currentProgress = 0;
let _targetProgress = 0;
let _integStartTime = 0;
let _cachedSymanzikTeX = null;
let _lastIntegrationData = null;

// Timing accumulator, fed by `[timing]` lines in the structured log
// (kernel-side EchoTiming → CellPrint → cellToText prepends "[timing] ").
// renderTimingTable() builds the Timings panel from _kernelTimings.
// Dropped the old stage-index-transition derivation: the log-side
// timings are second-accurate (no 800 ms poll slop) and emitted by
// the same EchoTiming calls that structured the old progress log.
let _kernelTimings = [];     // [{label:string, seconds:number}, …]

// ── Non-default options display ──

function buildNonDefOpts(config) {
  const opts = [];
  if (config.representation && config.representation !== 'Schwinger')
    opts.push('Representation: ' + config.representation);
  if (config.verbose) opts.push('Verbose: True');
  if (config.stopAt && config.stopAt !== 'Automatic')
    opts.push('StopAt: ' + config.stopAt);
  if (config.numeratorRows && config.numeratorRows.length > 0) {
    const numStr = config.numeratorRows.map(r =>
      r.expr + (r.exp && r.exp !== '1' ? ' [\u03BD=' + r.exp + ']' : '')
    ).join(', ');
    opts.push('Numerator: ' + numStr);
  }
  if (config.propExponents && config.propExponents.length > 0) {
    if (config.propExponents.some(p => p !== '1' && p !== 1))
      opts.push('Prop. exp: [' + config.propExponents.join(', ') + ']');
  }
  if (config.substitutions && config.substitutions.length > 0 && config.substitutions !== '{}')
    opts.push('Substitutions: ' + config.substitutions);
  if (config.simplifyOutput && config.simplifyOutput !== 'Simplify')
    opts.push('SimplifyOutput: ' + config.simplifyOutput);
  if (config.contourHandling && config.contourHandling !== 'Abort')
    opts.push('ContourHandling: ' + config.contourHandling);
  if (config.gauge && config.gauge !== 'Automatic')
    opts.push('Gauge: ' + config.gauge);
  if (config.normalization && config.normalization !== 'Automatic')
    opts.push('Normalization: ' + config.normalization);
  if (config.cleanOutput === false) opts.push('CleanOutput: False');
  if (!config.showTimings) opts.push('ShowTimings: False');
  if (config.showIntegrands) opts.push('ShowIntegrands: True');
  if (config.saveSlowest) opts.push('SaveSlowest: True');
  if (config.saveAll) opts.push('SaveAll: True');
  if (config.heuristic && config.heuristic !== 'LeafCountLinear')
    opts.push('Heuristic: ' + config.heuristic);
  if (config.kernels && config.kernels !== '')
    opts.push('Kernels: ' + config.kernels);
  if (config.includeGauges && config.includeGauges !== 'All')
    opts.push('IncludeGauges: ' + config.includeGauges);
  if (config.selectFaces && config.selectFaces !== 'All')
    opts.push('SelectFaces: ' + config.selectFaces);
  if (config.clearCaches) opts.push('ClearCaches: True');
  if (config.startAt && config.startAt !== 'None')
    opts.push('StartAt: ' + config.startAt);
  if (config.parallelization && config.parallelization !== 'All')
    opts.push('Parallelization: ' + config.parallelization);
  if (config.scanScoreInterval && config.scanScoreInterval !== '{1, 3}')
    opts.push('ScanScoreInterval: ' + config.scanScoreInterval);
  if (config.scoreInParallel && config.scoreInParallel !== 'All')
    opts.push('ScoreInParallel: ' + config.scoreInParallel);
  if (config.timeUpperBound && config.timeUpperBound !== '10^17')
    opts.push('TimeUpperBound: ' + config.timeUpperBound);
  if (config.memoryCutOff && config.memoryCutOff !== 'None')
    opts.push('MemoryCutOff: ' + config.memoryCutOff);
  if (config.scoringMemFrac && config.scoringMemFrac !== '0.5')
    opts.push('ScoringMemFraction: ' + config.scoringMemFrac);
  if (config.reuseResults === false) opts.push('ReuseResults: False');
  return opts;
}

// ── Integration panel builder ──

// Assemble a best-effort client-side STIntegrate[…] command string for
// the SubTropica view of the Integral box. The kernel emits its own
// authoritative `stCommand` in the result payload (see handleIntegrate);
// onIntegrationComplete upgrades the stashed string when that arrives.
function buildSTIntegrateCommandJS(payload) {
  const graphArg = buildGraphArgJS();
  if (!graphArg) return '';
  const opts = buildSTIntegrateOpts();
  const optStr = opts.length ? ',\n  ' + opts.join(',\n  ') : '';
  return `STIntegrate[${graphArg}${optStr}]`;
}

function showIntegrationPanel(payload) {
  _integrationDone = false;
  const body = $('integrate-body');
  body.innerHTML = '';

  // Widen the modal for integration view
  const panel = body.closest('.modal-panel');
  if (panel) panel.classList.add('integ-mode');

  // ── Summary card ──
  const card = document.createElement('div');
  card.className = 'integ-summary-card';
  card.id = 'integ-summary-card';
  const cardFlex = document.createElement('div');
  cardFlex.className = 'integ-summary-flex';

  // Left: diagram preview — render via generateThumbnail() so the integration
  // modal gets the same nicely-labeled diagram as the library detail popup
  // (leg momentum labels, mass labels, mass colors), rather than a stripped
  // clone of the live canvas SVG. We derive the Nickel and canvas-mass array
  // on the fly so novel drawings that aren't yet in the library still render.
  const diagDiv = document.createElement('div');
  diagDiv.className = 'integ-diagram';
  diagDiv.id = 'integ-diagram';
  (function renderIntegDiagram() {
    const edgeData = buildEdgeData();
    if (!edgeData || edgeData.edges.length === 0) return;
    try {
      const canon = canonicalize(edgeData.edges);
      const nodeMap = canon.nodeMaps[0];  // any automorphism works for display
      const canvasMasses = getCanvasMassArray(
        canon.nickel, nodeMap, edgeData.edges, edgeData.masses);
      // Use canon.string (not canon.nickel — which is the internal
      // array, not the Nickel-string format generateThumbnail expects).
      // canon.string carries LEG markers from buildEdgeData AND shares
      // its edge order with canvasMasses. Other candidates (vacuum
      // currentNickel, library-indexed nickelFull) would either drop
      // legs or shuffle the mass array out of alignment with the edges.
      const thumb = generateThumbnail(canon.string, null, {
        labels: true,
        edgeMasses: canvasMasses,
      });
      if (thumb) {
        // Explicit pixel dimensions instead of width/height:auto so the
        // SVG renders in Mathematica's embedded WebView too -- its older
        // WebKit collapses SVGs to 0×0 inside flex containers when the
        // intrinsic size isn't set. Matches the .popup-thumb pattern
        // already used for the library popup. Max-width keeps the
        // diagram from overflowing on narrow modal widths.
        thumb.style.cssText = 'width:200px;height:200px;max-width:100%;flex-shrink:0';
        diagDiv.appendChild(thumb);
      }
    } catch (e) {
      console.warn('[SubTropica] Thumbnail generation failed:', e.message || e);
      // Fall back to the stripped-canvas SVG if anything throws
      // (disconnected graph, unusual canonicalization state, etc.)
      const svgStr = generateDiagramPreview();
      if (svgStr) {
        const img = document.createElement('img');
        img.src = 'data:image/svg+xml;charset=utf-8,' + encodeURIComponent(svgStr);
        img.alt = 'Feynman diagram';
        img.style.cssText = 'width:200px;height:200px;max-width:100%;object-fit:contain;flex-shrink:0';
        diagDiv.appendChild(img);
      }
    }
    // If both thumbnail methods failed, clone the live canvas as a last resort
    if (!diagDiv.hasChildNodes()) {
      try {
        const liveCanvas = document.getElementById('draw-canvas');
        if (liveCanvas) {
          const clone = liveCanvas.cloneNode(true);
          clone.style.cssText = 'width:200px;height:200px;max-width:100%;flex-shrink:0';
          clone.removeAttribute('id');
          diagDiv.appendChild(clone);
        }
      } catch {}
    }
  })();
  cardFlex.appendChild(diagDiv);

  // Right: parameters
  const params = document.createElement('div');
  params.className = 'integ-params';
  params.id = 'integ-params';
  const addParam = (label, value) => {
    const l = document.createElement('div');
    l.className = 'integ-param-label';
    l.textContent = label;
    params.appendChild(l);
    const v = document.createElement('div');
    v.className = 'integ-param-value';
    v.textContent = value;
    params.appendChild(v);
  };
  addParam('NAME', payload.diagramName || currentNickel || '(unnamed)');
  addParam('DIMENSION', payload.dimension || '4 - 2\u03B5');
  addParam('\u03B5 ORDER', String(payload.epsOrder ?? 0));
  const opts = buildNonDefOpts(payload);
  if (opts.length) addParam('OPTIONS', opts.join('\n'));
  else addParam('OPTIONS', 'All default');
  cardFlex.appendChild(params);
  card.appendChild(cardFlex);

  body.appendChild(card);

  // ── Input formula ──
  // Layout:
  //   [Integral label]                                    [difficulty stars]
  //   <rendered integral / Symanzik / STIntegrate command>
  //                                [Loop] [Symanzik] [SubTropica] [Copy LaTeX]
  const formulaFrame = document.createElement('div');
  formulaFrame.className = 'integ-formula-frame';

  const formulaHeader = document.createElement('div');
  formulaHeader.className = 'integ-formula-header';
  const formulaLabel = document.createElement('span');
  formulaLabel.className = 'integ-param-label';
  formulaLabel.style.margin = '0';
  formulaLabel.textContent = 'Integral';
  formulaHeader.appendChild(formulaLabel);

  // Difficulty stars pinned top-right of the Integral box. Renders into a
  // local container so the pulse bookkeeping on the main #difficulty-stars
  // doesn't fight this view.
  const starsHolder = document.createElement('div');
  starsHolder.className = 'difficulty-stars integ-difficulty-stars';
  // Stable id so refreshIntegDifficultyStars() can find it when a later
  // runTier2Check() / onIntegrationComplete fills the cache.
  starsHolder.id = 'integ-difficulty-stars';
  formulaHeader.appendChild(starsHolder);
  formulaFrame.appendChild(formulaHeader);

  refreshIntegDifficultyStars();

  const integral = buildFeynmanIntegralLaTeX();
  const formulaView = document.createElement('div');
  formulaView.className = 'integ-formula-view';
  formulaView.id = 'integ-formula-view';
  if (integral && typeof katex !== 'undefined') {
    formulaView._latex = integral.latex;
    katex.render('\\displaystyle ' + integral.latex, formulaView, { throwOnError: false, displayMode: true });
  } else if (integral) {
    formulaView.textContent = integral.latex;
    formulaView._latex = integral.latex;
  }
  // Stash a client-side-built STIntegrate command for the SubTropica view.
  // Updated in onIntegrationComplete when the kernel's authoritative
  // `stCommand` field arrives.
  formulaView._stCmd = buildSTIntegrateCommandJS(payload);
  formulaFrame.appendChild(formulaView);

  // Footer row with formula-mode toggles + copy button (bottom-right).
  const formulaFooter = document.createElement('div');
  formulaFooter.className = 'integ-formula-footer';

  const btnRow = document.createElement('div');
  btnRow.className = 'modal-btn-row';
  const loopBtn = document.createElement('button');
  loopBtn.className = 'integ-formula-btn active';
  loopBtn.id = 'integ-formula-loop-btn';
  loopBtn.textContent = 'Loop';
  loopBtn.addEventListener('click', () => switchIntegFormula('loop'));
  btnRow.appendChild(loopBtn);
  const symBtn = document.createElement('button');
  symBtn.className = 'integ-formula-btn';
  symBtn.id = 'integ-formula-sym-btn';
  symBtn.textContent = 'Symanzik';
  symBtn.addEventListener('click', () => switchIntegFormula('symanzik'));
  btnRow.appendChild(symBtn);
  const stBtn = document.createElement('button');
  stBtn.className = 'integ-formula-btn';
  stBtn.id = 'integ-formula-st-btn';
  stBtn.textContent = 'SubTropica';
  stBtn.title = 'Show the STIntegrate[] command for this run';
  stBtn.addEventListener('click', () => switchIntegFormula('subtropica'));
  btnRow.appendChild(stBtn);

  if (integral) {
    const copyBtn = document.createElement('button');
    copyBtn.className = 'integ-formula-btn';
    // Single "Copy" button — the payload depends on view.dataset.mode:
    // LaTeX for Loop/Symanzik, the STIntegrate[] command for SubTropica.
    // Matches the Export tab's single-button convention.
    copyBtn.textContent = 'Copy';
    copyBtn.addEventListener('click', () => {
      const view = $('integ-formula-view');
      if (view && view.dataset.mode === 'subtropica') {
        navigator.clipboard.writeText(view._stCmd || '').then(() => showWarningToast('Command copied'));
      } else {
        navigator.clipboard.writeText(view?._latex || integral.latex).then(() => showWarningToast('LaTeX copied'));
      }
    });
    btnRow.appendChild(copyBtn);
  }
  formulaFooter.appendChild(btnRow);
  formulaFrame.appendChild(formulaFooter);

  body.appendChild(formulaFrame);

  // ── Result box with progress border ──
  const resultOuter = document.createElement('div');
  resultOuter.className = 'result-box-outer';
  resultOuter.id = 'integ-result-box';

  // Verified badge: absolutely-positioned chip in the top-right of the
  // result box, hidden until numerical verification succeeds. Lives on
  // the outer box (not the inner content) so it overlays the border and
  // isn't pushed around by the result scroll container.
  const verifiedBadge = document.createElement('div');
  verifiedBadge.id = 'integ-result-verified-badge';
  verifiedBadge.className = 'integ-result-verified-badge';
  verifiedBadge.style.display = 'none';
  resultOuter.appendChild(verifiedBadge);
  const resultInner = document.createElement('div');
  resultInner.className = 'result-box-inner';

  // Loading state
  const loadingRow = document.createElement('div');
  loadingRow.className = 'integ-loading-row';
  loadingRow.id = 'integ-loading-state';
  loadingRow.innerHTML = `
    <div class="coconut-spinner">
      <div class="spinner-track"></div>
      <div class="spinner-arc"></div>
      <span class="coconut-emoji mascot-emoji">${mascotEmoji()}</span>
    </div>
    <div class="integ-loading-text">
      <div class="integ-status-label" id="integ-status-text">Integrating\u2026</div>
      <div class="integ-stage-label" id="integ-stage-name">Initializing</div>
      <div class="integ-sub-label" id="integ-sub-progress"></div>
    </div>`;
  // Abort button (visible during integration)
  const abortBtn = document.createElement('button');
  abortBtn.className = 'modal-btn-tiny';
  abortBtn.id = 'integ-abort-btn';
  abortBtn.style.cssText = 'color:var(--red);margin-top:8px';
  abortBtn.textContent = '\u2715 Abort';
  abortBtn.addEventListener('click', () => {
    if (abortBtn.dataset.confirming) {
      cancelIntegration();
    } else {
      abortBtn.dataset.confirming = '1';
      abortBtn.textContent = 'Abort integration?';
      abortBtn.style.fontWeight = '600';
      setTimeout(() => {
        if (abortBtn.dataset.confirming) {
          delete abortBtn.dataset.confirming;
          abortBtn.textContent = '\u2715 Abort';
          abortBtn.style.fontWeight = '';
        }
      }, 3000);
    }
  });
  loadingRow.appendChild(abortBtn);

  resultInner.appendChild(loadingRow);

  // Result reveal (hidden until complete)
  const resultReveal = document.createElement('div');
  resultReveal.className = 'result-reveal';
  resultReveal.id = 'integ-result-content';
  resultInner.appendChild(resultReveal);
  resultOuter.appendChild(resultInner);
  body.appendChild(resultOuter);

  // ── Pro-tools tabs (Setup · Log · Timings).
  // One collapsible box containing three mutually-exclusive panels.
  // Tab bar is always visible (with live counters / totals); clicking
  // a tab expands the box and switches to that panel. Clicking the
  // active tab collapses the box. Pros only pay the vertical real-estate
  // for the tab they're reading. *)
  const toolsWrapper = document.createElement('div');
  toolsWrapper.className = 'integ-tools-wrapper';
  toolsWrapper.id = 'integ-tools-wrapper';

  const toolsTabs = document.createElement('div');
  toolsTabs.className = 'integ-tools-tabs';
  toolsTabs.innerHTML =
    '<button class="integ-tools-tab" data-tab="setup" type="button">Setup</button>' +
    '<button class="integ-tools-tab" data-tab="log" type="button">' +
      'Log<span class="integ-log-pills" id="integ-log-pills"></span>' +
    '</button>' +
    '<button class="integ-tools-tab" data-tab="timings" type="button">' +
      'Timings<span class="integ-tools-total" id="integ-timings-total"></span>' +
    '</button>';
  toolsWrapper.appendChild(toolsTabs);

  const toolsBody = document.createElement('div');
  toolsBody.className = 'integ-tools-body';
  toolsWrapper.appendChild(toolsBody);

  // Panel: Setup. renderSetupPanel() writes into this element when the
  // kind="setup" JSONL record arrives.
  const setupPanel = document.createElement('div');
  setupPanel.className = 'integ-tools-panel';
  setupPanel.dataset.panel = 'setup';
  setupPanel.id = 'integ-setup-wrapper';
  toolsBody.appendChild(setupPanel);

  // Panel: Log (toolbar + structured viewer + raw fallback).
  const logPanel = document.createElement('div');
  logPanel.className = 'integ-tools-panel';
  logPanel.dataset.panel = 'log';
  logPanel.id = 'integ-log-body';  // re-used by toggle / auto-expand code

  const logToolbar = document.createElement('div');
  logToolbar.className = 'integ-log-toolbar';
  logToolbar.id = 'integ-log-toolbar';
  logPanel.appendChild(logToolbar);

  const logViewer = document.createElement('div');
  logViewer.className = 'integ-log integ-log-viewer';
  logViewer.id = 'integ-log-viewer';
  logViewer.style.display = 'none';
  logPanel.appendChild(logViewer);

  const logPre = document.createElement('div');
  logPre.className = 'integ-log integ-log-raw';
  logPre.id = 'integ-log';
  logPre.textContent = 'Waiting for kernel output\u2026';
  logPanel.appendChild(logPre);
  toolsBody.appendChild(logPanel);

  // Panel: Timings. renderTimingTable() writes into this element.
  const timingPanel = document.createElement('div');
  timingPanel.className = 'integ-tools-panel';
  timingPanel.dataset.panel = 'timings';
  timingPanel.id = 'integ-timing-wrapper';
  toolsBody.appendChild(timingPanel);

  body.appendChild(toolsWrapper);

  // Wire the tab buttons: click switches + expands; click active tab
  // collapses. Pills inside the Log tab stop propagation so pill-click
  // keeps its jump-to-first-error semantics.
  Array.from(toolsTabs.querySelectorAll('.integ-tools-tab')).forEach(btn => {
    btn.addEventListener('click', ev => {
      if (ev.target && ev.target.closest('.integ-log-pills')) return;
      switchToolsTab(btn.dataset.tab);
    });
  });
  renderLogToolbar();

  // Reset progress state
  _currentProgress = 0;
  _targetProgress = 0;
  _integStartTime = Date.now();
  _cachedSymanzikTeX = null;
  _logRecordCounts = { info: 0, warn: 0, error: 0 };
  _logRecordsSeen = 0;
  _setupRecord = null;
  _viewerActive = false;
  _currentFoldBody = null;
  _currentFoldCount = 0;
  _firstErrorRow = null;
  _firstWarnRow = null;
  _errorAutoExpanded = false;
  _rawJsonlText = '';
  _jsonlByteOffset = 0;
  _decisionSummaries = [];
  const decCard = document.getElementById('integ-decisions-wrapper');
  if (decCard) decCard.remove();
  _logFilters = { showInfo: true, showWarn: true, showError: true, search: '' };
  renderLogPills();
  _kernelTimings = [];

  // Open overlay
  $('integrate-overlay').classList.add('visible');
  startProgressAnimation();
}

function startProgressAnimation() {
  if (_progressAnimFrame) cancelAnimationFrame(_progressAnimFrame);
  function tick() {
    if (_currentProgress < _targetProgress) {
      _currentProgress += (_targetProgress - _currentProgress) * 0.08 + 0.1;
      if (_currentProgress > _targetProgress) _currentProgress = _targetProgress;
      const box = $('integ-result-box');
      if (box) box.style.setProperty('--progress', _currentProgress.toFixed(1) + '%');
    }
    // Update elapsed time
    const elapsed = ((Date.now() - _integStartTime) / 1000).toFixed(0);
    const statusEl = $('integ-status-text');
    if (statusEl && !statusEl._done) {
      statusEl.textContent = 'Integrating\u2026 ' + formatElapsed(elapsed);
    }
    _progressAnimFrame = requestAnimationFrame(tick);
  }
  _progressAnimFrame = requestAnimationFrame(tick);
}

function formatElapsed(sec) {
  sec = parseInt(sec);
  if (sec < 60) return sec + 's';
  return Math.floor(sec / 60) + 'm ' + (sec % 60) + 's';
}

function stopProgressAnimation() {
  if (_progressAnimFrame) { cancelAnimationFrame(_progressAnimFrame); _progressAnimFrame = null; }
}

// ── Coconut celebration ─────────────────────────────────────────────
function coconutCelebration() {
  const N = 30 + Math.floor(Math.random() * 31); // 30–60

  // Increment coconut counter by number of coconuts in the confetti
  const prev = parseInt(localStorage.getItem('subtropica-coconuts') || '0');
  localStorage.setItem('subtropica-coconuts', String(prev + N));
  const GRAVITY = 1800;  // px/s²
  const container = document.createElement('div');
  container.style.cssText = 'position:fixed;inset:0;pointer-events:none;z-index:9999;overflow:hidden';
  document.body.appendChild(container);

  const coconuts = [];
  for (let i = 0; i < N; i++) {
    const el = document.createElement('div');
    const size = 20 + Math.random() * 30;
    el.textContent = mascotEmoji();
    el.style.cssText = `position:absolute;font-size:${size}px;line-height:1;will-change:transform;`;
    container.appendChild(el);

    // Launch from bottom of screen with upward velocity
    const W = window.innerWidth, H = window.innerHeight;
    let x = Math.random() * W;
    let y = H + 20 + Math.random() * 40;
    let vx = (Math.random() - 0.5) * 600;
    let vy = -(1200 + Math.random() * 800);

    const spin = (Math.random() - 0.5) * 720; // deg/s
    coconuts.push({ el, x, y, vx, vy, spin, angle: Math.random() * 360 });
  }

  let last = performance.now();
  function tick(now) {
    const dt = Math.min((now - last) / 1000, 0.05);
    last = now;
    let anyVisible = false;
    for (const c of coconuts) {
      c.vy += GRAVITY * dt;
      c.x += c.vx * dt;
      c.y += c.vy * dt;
      c.angle += c.spin * dt;
      c.el.style.transform = `translate(${c.x}px,${c.y}px) rotate(${c.angle}deg)`;
      if (c.y < window.innerHeight + 100) anyVisible = true;
    }
    if (anyVisible) requestAnimationFrame(tick);
    else container.remove();
  }
  requestAnimationFrame(tick);
}

// Pro-tools tab switcher. Clicking an inactive tab activates it and
// expands the panel; clicking the active tab collapses. Centralised
// so auto-expand-on-error (openLogTab) and pill jump-to-first can
// reuse the same state machine.
function switchToolsTab(name) {
  const wrapper = $('integ-tools-wrapper');
  if (!wrapper) return;
  const tabs = wrapper.querySelectorAll('.integ-tools-tab');
  const panels = wrapper.querySelectorAll('.integ-tools-panel');
  const activeBtn = wrapper.querySelector('.integ-tools-tab.active');
  if (activeBtn && activeBtn.dataset.tab === name) {
    // Collapse.
    wrapper.classList.remove('expanded');
    activeBtn.classList.remove('active');
    panels.forEach(p => p.classList.remove('active'));
    return;
  }
  wrapper.classList.add('expanded');
  tabs.forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  panels.forEach(p => p.classList.toggle('active', p.dataset.panel === name));
}

// Open the Log tab unconditionally (used by pill-jump and auto-expand
// on first error). Idempotent.
function openLogTab() {
  const wrapper = $('integ-tools-wrapper');
  if (!wrapper) return;
  const activeBtn = wrapper.querySelector('.integ-tools-tab.active');
  if (activeBtn && activeBtn.dataset.tab === 'log') return;
  switchToolsTab('log');
}

function switchIntegFormula(mode) {
  const loopBtn = $('integ-formula-loop-btn');
  const symBtn = $('integ-formula-sym-btn');
  const stBtn = $('integ-formula-st-btn');
  const view = $('integ-formula-view');
  if (!view) return;
  const setActive = which => {
    if (loopBtn) loopBtn.classList.toggle('active', which === 'loop');
    if (symBtn)  symBtn.classList.toggle('active',  which === 'symanzik');
    if (stBtn)   stBtn.classList.toggle('active',   which === 'subtropica');
    view.dataset.mode = which;
  };

  if (mode === 'loop') {
    setActive('loop');
    const integral = buildFeynmanIntegralLaTeX();
    if (integral && typeof katex !== 'undefined') {
      view._latex = integral.latex;
      katex.render('\\displaystyle ' + integral.latex, view, { throwOnError: false, displayMode: true });
    }
  } else if (mode === 'subtropica') {
    setActive('subtropica');
    const cmd = view._stCmd || '(command not available yet)';
    // Monospace, left-aligned, wrap long lines. No KaTeX.
    view.innerHTML = '';
    const pre = document.createElement('pre');
    pre.className = 'integ-formula-stcmd';
    pre.textContent = cmd;
    view.appendChild(pre);
  } else {
    setActive('symanzik');
    if (_cachedSymanzikTeX) {
      renderSymanzikFormula(view, _cachedSymanzikTeX);
    } else {
      view.textContent = 'Loading Symanzik\u2026';
      kernel.get('symanzik_tex').then(data => {
        if (data && data.tex) {
          _cachedSymanzikTeX = data;
          renderSymanzikFormula(view, data);
        } else {
          view.textContent = 'Symanzik not available yet.';
        }
      }).catch(() => { view.textContent = 'Symanzik not available.'; });
    }
  }
}

function renderSymanzikFormula(view, data) {
  if (typeof katex !== 'undefined' && data.tex) {
    var cleaned = cleanTeX(data.tex);
    var wrapped = wrapSymanzikIntegral(cleaned, data.nvars || 0);
    view._latex = wrapped;
    katex.render('\\displaystyle ' + wrapped, view, { throwOnError: false, displayMode: true, trust: true, maxSize: 500, maxExpand: 10000 });
  }
}

const SCAN_LABELS = ['Initialization','Building integrands','Tropical subtractions','Expanding the integrand','Setting up the directories','Scoring gauges','Finding linear orders','Setting up the kernels','Integrating','Collecting the results'];
const NOSCAN_LABELS = ['Initialization','Expanding the integrand','Setting up the directories','Finding linear orders','Setting up the kernels','Integrating','Collecting the results'];

function updateProgressFromLog(logText) {
  // Color-code and update log display
  const logEl = $('integ-log');
  if (logEl) {
    logEl.innerHTML = colorCodeLog(logText);
    logEl.scrollTop = logEl.scrollHeight;
  }

  // Fallback: estimate progress from log keywords (used when /api/stage unavailable)
  if (logText.includes('Integrating face') || logText.includes('Combining results')) _targetProgress = Math.max(_targetProgress, 80);
  else if (logText.includes('Scoring') || logText.includes('scoring')) _targetProgress = Math.max(_targetProgress, 65);
  else if (logText.includes('linear order') || logText.includes('Linear order')) _targetProgress = Math.max(_targetProgress, 50);
  else if (logText.includes('Expanding') || logText.includes('expanding')) _targetProgress = Math.max(_targetProgress, 30);
  else if (logText.includes('Building integrand') || logText.includes('Parametric integrand')) _targetProgress = Math.max(_targetProgress, 15);
  else if (logText.includes('Starting integration')) _targetProgress = Math.max(_targetProgress, 5);
}

// ── Structured kernel log (progress.jsonl) ──────────────────────────
// Populated by /api/progress_jsonl alongside the existing /api/progress
// firehose. Drives the info/warn/error summary pills, the "Full setup"
// panel (kind=="setup" records), the Reproducibility footer
// (kind=="repro" records), and the structured viewer (message records).
let _logRecordCounts = { info: 0, warn: 0, error: 0 };
let _logRecordsSeen = 0;  // number of parsed records so far; lines past
                          // this index are new appends on the next poll.
let _setupRecord = null;  // last observed kind="setup" record (run start)

// Structured viewer state (axes 6, 8, 10).
let _viewerActive = false;     // flipped to true on first message record
let _currentFoldBody = null;   // open <details> body for a contiguous
                               // run of non-wrapper records, or null
let _currentFoldCount = 0;     // running line count inside the open fold
let _firstErrorRow = null;     // first DOM row with data-level="error"
let _firstWarnRow = null;      // first DOM row with data-level="warn"
let _logFilters = { showInfo: true, showWarn: true, showError: true, search: '' };
let _errorAutoExpanded = false;  // guards one-shot auto-expand on error
let _rawJsonlText = '';        // cached raw JSONL blob for Export
let _jsonlByteOffset = 0;      // byte cursor for /api/progress_jsonl?since=
                               // tailing (axis 11). Reset on new run / truncation.

function renderLogPills() {
  const el = document.getElementById('integ-log-pills');
  if (!el) return;
  const c = _logRecordCounts;
  const mk = (cls, label, n, lvl) =>
    `<span class="log-pill ${cls}" data-jump-level="${lvl}" title="jump to first ${lvl}">` +
    `${label} ${n}</span>`;
  const parts = [];
  // Always show info so the user sees that the structured stream is live.
  parts.push(mk('log-pill-info',  'info',  c.info,  'info'));
  if (c.warn > 0)  parts.push(mk('log-pill-warn',  'warn',  c.warn,  'warn'));
  if (c.error > 0) parts.push(mk('log-pill-error', 'error', c.error, 'error'));
  el.innerHTML = parts.join('');
  // Pill click → scroll-to-first-matching-level (axis 8). Stop
  // propagation so the surrounding toggle doesn't collapse the log.
  Array.from(el.querySelectorAll('[data-jump-level]')).forEach(p => {
    p.addEventListener('click', ev => {
      ev.stopPropagation();
      jumpToFirst(p.dataset.jumpLevel);
    });
  });
}

function jumpToFirst(level) {
  const target = level === 'error' ? _firstErrorRow :
                 level === 'warn'  ? _firstWarnRow  : null;
  if (!target) return;
  openLogTab();
  // Open any containing <details> fold
  let node = target.parentElement;
  while (node) {
    if (node.tagName === 'DETAILS') node.open = true;
    node = node.parentElement;
  }
  target.scrollIntoView({ block: 'center', behavior: 'smooth' });
  target.classList.add('log-highlight');
  setTimeout(() => target.classList.remove('log-highlight'), 1600);
}

// Reset all JSONL-viewer state (used on file truncation / new run).
function resetJsonlState() {
  _logRecordCounts = { info: 0, warn: 0, error: 0 };
  _logRecordsSeen = 0;
  _setupRecord = null;
  _rawJsonlText = '';
  _jsonlByteOffset = 0;
  const viewer = document.getElementById('integ-log-viewer');
  if (viewer) viewer.innerHTML = '';
  _viewerActive = false;
  _currentFoldBody = null;
  _currentFoldCount = 0;
  _firstErrorRow = null;
  _firstWarnRow = null;
  _errorAutoExpanded = false;
  _decisionSummaries = [];
  const decCard = document.getElementById('integ-decisions-wrapper');
  if (decCard) decCard.remove();
  renderLogPills();
}

// Append one parsed record to the viewer state. Returns an object
// flagging whether a setup/repro record was observed, so callers can
// decide whether to re-render those panels once per batch.
function ingestJsonlRecord(rec, viewer) {
  const lvl = rec.level;
  if (lvl === 'info' || lvl === 'warn' || lvl === 'error') {
    _logRecordCounts[lvl]++;
  }
  if (rec.kind === 'setup') { _setupRecord = rec; return { setup: true }; }
  if (rec.kind === 'decision' && viewer) appendDecisionRow(viewer, rec);
  if (rec.kind === 'message'  && viewer) appendLogRow(viewer, rec);
  return {};
}

// Axis 11 entry point. Receives only the *new* tail past the last
// observed byte offset. No need to re-parse records seen on earlier
// polls — the byte cursor guarantees each line is processed once.
function ingestJsonlTail(tailText) {
  if (!tailText) return;
  _rawJsonlText += tailText;
  const lines = tailText.split('\n').filter(l => l.length > 0);
  const viewer = document.getElementById('integ-log-viewer');
  let newSetup = false;
  for (const line of lines) {
    let rec;
    try { rec = JSON.parse(line); } catch { continue; }
    const r = ingestJsonlRecord(rec, viewer);
    if (r.setup) newSetup = true;
  }
  _logRecordsSeen += lines.length;
  renderLogPills();
  if (newSetup) renderSetupPanel();
  if (_viewerActive) {
    const rawEl = document.getElementById('integ-log');
    if (rawEl && !rawEl.dataset.rawOverride) rawEl.style.display = 'none';
    const vEl = document.getElementById('integ-log-viewer');
    if (vEl) vEl.style.display = '';
  }
}

// Backwards-compat wrapper: older call sites that have the full
// jsonlText (legacy fallback, never hit in the tailing path) can still
// call ingestJsonlText and it re-derives the tail.
function ingestJsonlText(jsonlText) {
  if (!jsonlText) return;
  const fullLen = (jsonlText || '').length;
  if (fullLen < _rawJsonlText.length) resetJsonlState();
  const tail = jsonlText.slice(_rawJsonlText.length);
  ingestJsonlTail(tail);
}

// Axis 11 poll: fetch /api/progress_jsonl?since=<offset> and feed the
// tail into ingestJsonlTail. Handles both server-side responses with
// the tailing fields (status/jsonl/size/since) and older servers that
// return only {status, jsonl} — in that case we fall back to the
// length-compare path via ingestJsonlText.
async function pollJsonlTail() {
  const r = await fetch('/api/progress_jsonl?since=' + _jsonlByteOffset);
  if (!r.ok) return;
  const jl = await r.json();
  if (!jl || jl.status !== 'ok') return;
  if (typeof jl.size === 'number') {
    // Tailing-capable server. Detect truncation and reset.
    if (jl.size < _jsonlByteOffset) resetJsonlState();
    if (typeof jl.jsonl === 'string' && jl.jsonl) ingestJsonlTail(jl.jsonl);
    _jsonlByteOffset = jl.size;
  } else if (typeof jl.jsonl === 'string') {
    // Older server: full-file response. Legacy path.
    ingestJsonlText(jl.jsonl);
  }
}

// Append one message record to the structured viewer as a <div> row.
// Consecutive non-wrapper records (stPrint/stEcho/stCellPrint/stdout)
// are grouped under a <details> fold; wrapper records close the open
// fold and render in-line.
function appendLogRow(viewer, rec) {
  const src = rec.source || 'unknown';
  const lvl = rec.level  || 'info';
  const text = rec.text != null ? String(rec.text) : '';
  const isWrapper = src === 'wrapper';

  // Timings source: every "[timing] …" line is captured into the
  // Timings panel before it's rendered inline in the log.
  maybeIngestTimingLine(text);

  // Toggle fold membership based on wrapper-vs-not.
  if (!isWrapper) {
    if (!_currentFoldBody) {
      const details = document.createElement('details');
      details.className = 'log-fold';
      const summary = document.createElement('summary');
      summary.className = 'log-fold-summary';
      summary.innerHTML =
        '<span class="log-fold-chevron">\u25B6</span>' +
        '<span class="log-fold-title">STEvaluate output</span>' +
        '<span class="log-fold-count"></span>';
      details.appendChild(summary);
      const foldBody = document.createElement('div');
      foldBody.className = 'log-fold-body';
      details.appendChild(foldBody);
      viewer.appendChild(details);
      _currentFoldBody = foldBody;
      _currentFoldCount = 0;
      _currentFoldBody._countEl = summary.querySelector('.log-fold-count');
    }
  } else if (_currentFoldBody) {
    _currentFoldBody = null;
    _currentFoldCount = 0;
  }

  const row = document.createElement('div');
  row.className = 'log-row log-row-' + lvl +
    (isWrapper ? ' log-row-wrapper' : ' log-row-passthrough');
  row.dataset.level = lvl;
  row.dataset.source = src;
  // Preserve multiline messages' whitespace but collapse runs of 3+
  // spaces (boxToText in the kernel leaves some stringy gaps).
  row.textContent = text;

  if (isWrapper) {
    viewer.appendChild(row);
  } else {
    _currentFoldBody.appendChild(row);
    _currentFoldCount++;
    if (_currentFoldBody._countEl) {
      _currentFoldBody._countEl.textContent =
        '\u00B7 ' + _currentFoldCount + ' line' +
        (_currentFoldCount === 1 ? '' : 's');
    }
  }

  // Remember first warn / error rows for jump-to (axis 8).
  if (lvl === 'warn'  && !_firstWarnRow)  _firstWarnRow  = row;
  if (lvl === 'error' && !_firstErrorRow) _firstErrorRow = row;

  // One-shot: auto-expand the Log tab + jump to the first error record.
  if (lvl === 'error' && !_errorAutoExpanded) {
    _errorAutoExpanded = true;
    openLogTab();
    // Open the containing fold, if any
    let node = row.parentElement;
    while (node) {
      if (node.tagName === 'DETAILS') node.open = true;
      node = node.parentElement;
    }
    setTimeout(() => {
      row.scrollIntoView({ block: 'center', behavior: 'smooth' });
      row.classList.add('log-highlight');
      setTimeout(() => row.classList.remove('log-highlight'), 1800);
    }, 50);
  }

  if (!_viewerActive) _viewerActive = true;

  // Re-apply search filter to the new row (level filters use CSS, no JS).
  if (_logFilters.search) applySearchToRow(row, _logFilters.search);
}

// Axis 5: render a kind="decision" JSONL record as a distinctive row
// in the log viewer, and push it into the "Kernel decisions" summary
// card above the log. Decisions break any open pass-through fold
// (they're significant enough to deserve top-level visibility).
function appendDecisionRow(viewer, rec) {
  // Close open pass-through fold so the decision isn't buried.
  if (_currentFoldBody) {
    _currentFoldBody = null;
    _currentFoldCount = 0;
  }

  const sub = rec.subKind || 'decision';
  const summary = formatDecisionSummary(rec);

  const row = document.createElement('div');
  row.className = 'log-row log-row-decision';
  row.dataset.level = rec.level || 'info';
  row.dataset.source = 'decision';
  row.innerHTML =
    '<span class="log-decision-tag">DECISION · ' + escapeHtml(sub) + '</span>' +
    '<span class="log-decision-summary">' + escapeHtml(summary) + '</span>';
  viewer.appendChild(row);

  // Also push to the top-of-log "Kernel decisions" summary card.
  pushDecisionSummary(rec, summary);

  if (!_viewerActive) _viewerActive = true;
  if (_logFilters.search) applySearchToRow(row, _logFilters.search);
}

// Render a one-line human summary for each known subKind. Unknown
// subKinds fall back to a JSON dump of their fields so nothing is
// silently swallowed.
function formatDecisionSummary(rec) {
  switch (rec.subKind) {
    case 'gaugeSelected': {
      const best = rec.bestGauge != null ? 'x' + rec.bestGauge : '?';
      const cands = Array.isArray(rec.candidates)
        ? rec.candidates.map(g => 'x' + g).join(', ') : '?';
      const score = rec.bestScore != null ? String(rec.bestScore) : '?';
      return `best gauge ${best}  (score ${score})  from ${cands}`;
    }
    default: {
      const { t, level, kind, subKind, source, ...rest } = rec;
      return JSON.stringify(rest);
    }
  }
}

// "Kernel decisions" summary card: a compact header above the log
// body that lists every decision observed this run, so pros see all
// of them at a glance without scrolling the fold column. Rendered
// lazily on first decision.
let _decisionSummaries = [];  // [{subKind, summary, rec}]
function pushDecisionSummary(rec, summary) {
  _decisionSummaries.push({ subKind: rec.subKind, summary, rec });
  renderDecisionCard();
}

function renderDecisionCard() {
  let card = document.getElementById('integ-decisions-wrapper');
  if (!card) {
    // Inserted just above the pro-tools tabs so decisions stay visible
    // without clicking into the Log tab. Few and high-signal by design.
    const tools = document.getElementById('integ-tools-wrapper');
    if (!tools || !tools.parentNode) return;
    card = document.createElement('div');
    card.className = 'integ-decisions-wrapper';
    card.id = 'integ-decisions-wrapper';
    tools.parentNode.insertBefore(card, tools);
  }
  const rows = _decisionSummaries.map(d =>
    `<div class="integ-decision-row">` +
      `<span class="integ-decision-tag">${escapeHtml(d.subKind || '?')}</span>` +
      `<span class="integ-decision-summary">${escapeHtml(d.summary)}</span>` +
    `</div>`
  ).join('');
  card.innerHTML =
    '<div class="integ-decisions-header">Kernel decisions</div>' +
    '<div class="integ-decisions-body">' + rows + '</div>';
  card.style.display = '';
}

function applySearchToRow(row, q) {
  const hit = !q || row.textContent.toLowerCase().includes(q.toLowerCase());
  row.classList.toggle('log-row-hidden-search', !hit);
}

function applySearchToAll(q) {
  const viewer = document.getElementById('integ-log-viewer');
  if (!viewer) return;
  Array.from(viewer.querySelectorAll('.log-row')).forEach(r =>
    applySearchToRow(r, q));
}

function applyLevelFilters() {
  const viewer = document.getElementById('integ-log-viewer');
  if (!viewer) return;
  viewer.classList.toggle('hide-info',  !_logFilters.showInfo);
  viewer.classList.toggle('hide-warn',  !_logFilters.showWarn);
  viewer.classList.toggle('hide-error', !_logFilters.showError);
}

// ── Log toolbar (axis 10) ──────────────────────────────────────────
// Level toggles, text filter, and copy/export buttons. Rendered once
// per showIntegrationPanel; state lives in _logFilters and is reset
// alongside the viewer.
function renderLogToolbar() {
  const el = document.getElementById('integ-log-toolbar');
  if (!el) return;
  el.innerHTML =
    '<label class="log-toolbar-toggle"><input type="checkbox" id="log-flt-info" checked>info</label>' +
    '<label class="log-toolbar-toggle"><input type="checkbox" id="log-flt-warn" checked>warn</label>' +
    '<label class="log-toolbar-toggle"><input type="checkbox" id="log-flt-error" checked>error</label>' +
    '<input type="search" class="log-toolbar-search" id="log-search" ' +
      'placeholder="filter lines\u2026">' +
    '<button class="modal-btn-tiny" id="log-btn-copy" title="Copy visible log as plain text">Copy log</button>' +
    '<button class="modal-btn-tiny" id="log-btn-export" title="Download progress.jsonl">Export JSONL</button>' +
    '<button class="modal-btn-tiny" id="log-btn-failure" title="Copy a bundle of setup + repro + warnings/errors" style="display:none">Copy failure report</button>' +
    '<button class="modal-btn-tiny" id="log-btn-raw" title="Toggle the raw progress.log fallback view">Raw</button>';

  const bind = (id, evt, fn) => {
    const n = document.getElementById(id);
    if (n) n.addEventListener(evt, fn);
  };
  bind('log-flt-info',  'change', ev => {
    _logFilters.showInfo = ev.target.checked; applyLevelFilters();
  });
  bind('log-flt-warn',  'change', ev => {
    _logFilters.showWarn = ev.target.checked; applyLevelFilters();
  });
  bind('log-flt-error', 'change', ev => {
    _logFilters.showError = ev.target.checked; applyLevelFilters();
  });
  bind('log-search', 'input', ev => {
    _logFilters.search = ev.target.value || '';
    applySearchToAll(_logFilters.search);
  });
  bind('log-btn-copy', 'click', () => {
    const viewer = document.getElementById('integ-log-viewer');
    const rawEl  = document.getElementById('integ-log');
    let text;
    if (_viewerActive && viewer) {
      // Walk visible (non-filtered) rows and emit one line each. <details>
      // folds contribute their rows too, regardless of open state.
      const rows = viewer.querySelectorAll('.log-row');
      text = Array.from(rows)
        .filter(r => !r.classList.contains('log-row-hidden-search'))
        .filter(r => {
          const lvl = r.dataset.level;
          if (lvl === 'info'  && !_logFilters.showInfo)  return false;
          if (lvl === 'warn'  && !_logFilters.showWarn)  return false;
          if (lvl === 'error' && !_logFilters.showError) return false;
          return true;
        })
        .map(r => r.textContent).join('\n');
    } else if (rawEl) {
      text = rawEl.textContent;
    } else {
      text = '';
    }
    navigator.clipboard.writeText(text).then(
      () => showWarningToast('Log copied'));
  });
  bind('log-btn-export', 'click', () => {
    if (!_rawJsonlText) { showWarningToast('No JSONL yet'); return; }
    const blob = new Blob([_rawJsonlText], { type: 'application/x-ndjson' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    const stamp = new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19);
    a.href = url;
    a.download = 'subtropica-progress-' + stamp + '.jsonl';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
  });
  bind('log-btn-failure', 'click', () => {
    const lines = [];
    lines.push('=== SubTropica failure report ===');
    if (_setupRecord) {
      lines.push('--- Setup ---');
      lines.push('Nickel: ' + (_setupRecord.nickel || ''));
      lines.push('Dimension: ' + (_setupRecord.dimension || ''));
      lines.push('Int masses: ' + (_setupRecord.internalMasses || ''));
      lines.push('Ext masses: ' + (_setupRecord.externalMasses || ''));
      lines.push('Prop exponents: ' + (_setupRecord.propExponents || ''));
      lines.push('');
    }
    const viewer = document.getElementById('integ-log-viewer');
    if (viewer) {
      const bad = Array.from(viewer.querySelectorAll(
        '.log-row-warn, .log-row-error'));
      if (bad.length) {
        lines.push('--- Warnings / errors ---');
        bad.forEach(r => lines.push(
          '[' + (r.dataset.level || '?') + '] ' + r.textContent));
        lines.push('');
      }
    }
    if (_setupRecord && _setupRecord.stCommand) {
      lines.push('--- STEvaluate command ---');
      lines.push(_setupRecord.stCommand);
    }
    navigator.clipboard.writeText(lines.join('\n')).then(
      () => showWarningToast('Failure report copied'));
  });
  bind('log-btn-raw', 'click', () => {
    const rawEl = document.getElementById('integ-log');
    const vEl   = document.getElementById('integ-log-viewer');
    if (!rawEl || !vEl) return;
    const showingRaw = rawEl.style.display !== 'none';
    if (showingRaw) {
      rawEl.style.display = 'none';
      rawEl.dataset.rawOverride = '';
      vEl.style.display = '';
    } else {
      rawEl.style.display = '';
      rawEl.dataset.rawOverride = '1';
      vEl.style.display = 'none';
    }
  });
}

async function pollStageProgress() {
  try {
    const stg = await kernel.get('stage');
    if (!stg || !stg.Stage) return;

    const idx = stg.StageIndex || 0;
    const count = stg.StageCount || 10;
    const isDone = stg.Stage === 'Done';
    const isFailed = stg.Stage === 'Failed';
    const labels = (count <= 7) ? NOSCAN_LABELS : SCAN_LABELS;
    const labelFor = i => (i > 0 && i <= labels.length) ? labels[i - 1] : String(i);

    // Update progress border target
    _targetProgress = isDone ? 100 : Math.min(95, (idx / count) * 100);

    // Update stage name label
    const stageEl = $('integ-stage-name');
    if (stageEl) {
      let stageName = labelFor(idx);
      if (isDone) stageName = 'Done';
      if (isFailed) stageName = 'Failed';
      stageEl.textContent = stageName;
    }

    // Update sub-progress (face/job tracking)
    const subEl = $('integ-sub-progress');
    if (subEl) {
      let subText = '';
      if (stg.FaceCurrent && stg.FaceTotal) {
        subText = 'Face ' + stg.FaceCurrent + ' / ' + stg.FaceTotal;
        if (stg.FaceName) subText += '  \u00B7  ' + stg.FaceName;
      }
      if (stg.JobsTotal && stg.JobsTotal > 0) {
        const completed = stg.JobsCompleted || 0;
        subText = 'Integrands: ' + completed + ' / ' + stg.JobsTotal;
      }
      subEl.textContent = subText;
    }
  } catch {}
}

// Format a duration for the timing table. Under a minute: "12.3 s".
// Between 1 and 60 minutes: "4m 32s". Above an hour: "1h 23m".
function formatStageDuration(ms) {
  const s = ms / 1000;
  if (s < 60) return s.toFixed(1) + ' s';
  if (s < 3600) return Math.floor(s / 60) + 'm ' + Math.round(s % 60) + 's';
  return Math.floor(s / 3600) + 'h ' + Math.round((s % 3600) / 60) + 'm';
}

// Parse a "[timing]" line into {label, seconds}. EchoTiming output via
// cellToText looks like "[timing] <seconds> <label>" after the boxed
// expression is rendered, but we stay tolerant: pull the first float
// we find and call the remainder the label. Returns null on no match.
const TIMING_LINE_RE = /\[timing\]\s*([-+]?\d+(?:\.\d+)?)\s*(.*)$/;
function parseTimingLine(text) {
  if (!text || text.indexOf('[timing]') < 0) return null;
  const m = text.match(TIMING_LINE_RE);
  if (!m) return null;
  const seconds = parseFloat(m[1]);
  if (!isFinite(seconds)) return null;
  let label = (m[2] || '').trim();
  // Strip boxed residue and collapsed whitespace.
  label = label.replace(/\s{2,}/g, ' ').replace(/^[,:\s]+/, '');
  if (!label) label = '(unlabeled)';
  return { label, seconds };
}

// Ingest a candidate log-line text: if it carries a "[timing]" prefix,
// push a timing row and schedule a panel re-render + tab-button total
// update. Called from appendLogRow at each message record.
function maybeIngestTimingLine(text) {
  const t = parseTimingLine(text);
  if (!t) return;
  // Skip the outermost "Overall timing" wrapper — it's the sum of
  // everything below it (redundant with the Total chip) and would
  // dominate the bar chart.
  if (/^Overall\s+timing/i.test(t.label)) return;
  _kernelTimings.push(t);
  renderTimingTable();  // idempotent incremental rebuild
  updateTimingsTabTotal();
}

// Write the running total into the Timings tab button so pros see the
// elapsed wall-clock without expanding the panel.
function updateTimingsTabTotal() {
  const el = document.getElementById('integ-timings-total');
  if (!el) return;
  if (_kernelTimings.length === 0) { el.textContent = ''; return; }
  const totalMs = _kernelTimings.reduce((a, t) => a + t.seconds * 1000, 0);
  el.textContent = ' · ' + formatStageDuration(totalMs);
}

// Render the Timings panel from _kernelTimings. Called whenever a new
// "[timing]" line is seen; safe to call repeatedly.
function renderTimingTable() {
  const wrapper = $('integ-timing-wrapper');
  if (!wrapper) return;
  if (_kernelTimings.length === 0) { wrapper.innerHTML = ''; return; }

  const totalMs = _kernelTimings.reduce((a, t) => a + t.seconds * 1000, 0);
  const maxMs   = _kernelTimings.reduce(
    (a, t) => Math.max(a, t.seconds * 1000), 0);

  const toolbar = document.createElement('div');
  toolbar.className = 'integ-timing-toolbar';
  const total = document.createElement('span');
  total.className = 'integ-timing-total';
  total.textContent = 'Total ' + formatStageDuration(totalMs);
  toolbar.appendChild(total);
  const copyBtn = document.createElement('button');
  copyBtn.className = 'modal-btn-tiny';
  copyBtn.textContent = 'Copy';
  copyBtn.addEventListener('click', ev => {
    ev.stopPropagation();
    const lines = _kernelTimings.map(t => {
      const pct = totalMs > 0 ? ((t.seconds * 1000 / totalMs) * 100).toFixed(1) : '0.0';
      return t.label.padEnd(36).slice(0, 36) +
             formatStageDuration(t.seconds * 1000).padStart(10) +
             '   ' + pct.padStart(5) + '%';
    });
    lines.push('');
    lines.push('Total'.padEnd(36) + formatStageDuration(totalMs).padStart(10));
    navigator.clipboard.writeText(lines.join('\n')).then(
      () => showWarningToast('Timings copied'));
  });
  toolbar.appendChild(copyBtn);

  const body = document.createElement('div');
  body.className = 'integ-timing-body integ-timing-body-flat';
  body.id = 'integ-timing-body';
  _kernelTimings.forEach(t => {
    const row = document.createElement('div');
    row.className = 'integ-timing-row';
    const pct = totalMs > 0 ? (t.seconds * 1000 / totalMs) * 100 : 0;
    const rel = maxMs   > 0 ? (t.seconds * 1000 / maxMs)   * 100 : 0;
    row.innerHTML =
      '<span class="integ-timing-name">' + escapeHtml(t.label) + '</span>' +
      '<span class="integ-timing-bar"><span style="width:' + rel.toFixed(1) +
        '%"></span></span>' +
      '<span class="integ-timing-dt">' +
        formatStageDuration(t.seconds * 1000) + '</span>' +
      '<span class="integ-timing-pct">' + pct.toFixed(1) + '%</span>';
    body.appendChild(row);
  });

  wrapper.innerHTML = '';
  wrapper.appendChild(toolbar);
  wrapper.appendChild(body);
}

// ── Setup panel (axis 3) ───────────────────────────────────────────
// Populated by the kind="setup" JSONL record. Rendered as the content
// of the Setup tab (headerless — the tab button is the header). Pros
// inspect the authoritative post-parse snapshot: masses, propagator
// exponents, numerator rows, topology counters, Nickel, and the
// fully-resolved STIntegrate[...] command.
function renderSetupPanel() {
  const wrapper = $('integ-setup-wrapper');
  if (!wrapper || !_setupRecord) return;
  const rec = _setupRecord;

  // Pairs rendered in order. Null/empty values are filtered out.
  const rows = [
    ['Nickel index',    rec.nickel],
    ['Topology',        `${rec.loops ?? '?'} loops · ${rec.numEdges ?? '?'} edges · ${rec.numLegs ?? '?'} legs`],
    ['Dimension',       rec.dimension],
    ['ε order',         String(rec.epsOrder ?? 0)],
    ['Internal masses', rec.internalMasses],
    ['External masses', rec.externalMasses],
    ['Prop exponents',  rec.propExponents],
    ['Numerator rows',  String(rec.numeratorRows ?? 0)],
  ].filter(([_, v]) => v != null && v !== '');

  // Toolbar row with a Copy STIntegrate button on the right. The tab
  // button is the panel's "header", so this row is just the action.
  const toolbar = document.createElement('div');
  toolbar.className = 'integ-setup-toolbar';
  if (rec.stCommand) {
    const copyBtn = document.createElement('button');
    copyBtn.className = 'modal-btn-tiny';
    copyBtn.textContent = 'Copy STIntegrate';
    copyBtn.title = 'Copy the fully-resolved STIntegrate[...] command with every option expanded';
    copyBtn.addEventListener('click', ev => {
      ev.stopPropagation();
      navigator.clipboard.writeText(rec.stCommand).then(
        () => showWarningToast('STIntegrate copied'));
    });
    toolbar.appendChild(copyBtn);
  }

  const body = document.createElement('div');
  body.className = 'integ-setup-body';
  body.id = 'integ-setup-body';
  rows.forEach(([label, value]) => {
    const row = document.createElement('div');
    row.className = 'integ-setup-row';
    const l = document.createElement('span');
    l.className = 'integ-setup-label';
    l.textContent = label;
    const v = document.createElement('span');
    v.className = 'integ-setup-value';
    v.textContent = String(value);
    row.appendChild(l);
    row.appendChild(v);
    body.appendChild(row);
  });

  wrapper.innerHTML = '';
  if (toolbar.childNodes.length) wrapper.appendChild(toolbar);
  wrapper.appendChild(body);
}


function colorCodeLog(text) {
  return text.split('\n').map(line => {
    let cls = '';
    if (line.includes('[SubTropica]')) cls = 'color:var(--green)';
    else if (line.includes('STEvaluate[') || line.includes('STIntegrate[')) cls = 'color:var(--accent);font-weight:600';
    else if (line.includes('WARNING') || line.includes('Error') || line.includes('failed')) cls = 'color:var(--red);font-weight:500';
    else if (line.includes('Combining') || line.includes('combining')) cls = 'color:#4A6A50';
    else if (line.includes('>>')) cls = 'color:var(--green)';
    else if (line.includes('[timing]')) cls = 'color:var(--gold)';
    else if (line.match(/^===|^---/)) cls = 'color:var(--border)';
    else if (line.includes('->')) cls = 'color:var(--text-muted);font-style:italic';
    else if (line.match(/^\s{2,}/)) cls = 'color:var(--text-muted)';
    if (cls) return `<span style="${cls}">${escapeHtml(line)}</span>`;
    return escapeHtml(line);
  }).join('\n');
}

function escapeHtml(s) {
  s = String(s);
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}

// Defensive strip for the stray '&' alignment marker that older kernels
// leaked into single-order symbol TeX (stComputeSymbolFields wrapped in
// \begin{aligned} only when there were multiple orders, but kept the
// '&' marker either way). KaTeX rejects a bare '&' outside an aligned
// block with ParseError: Expected 'EOF', got '&'. Library-cached
// symbolTeX strings from before the kernel-side fix still carry the
// marker, so strip it client-side as well.
// ────────────────────────────────────────────────────────────────────────────
// Alphabet-pill renderer shared by the config-popup and the integrate view.
//
// Prefers `wDefinitions` (v1.0.390+ with kind + pairIndex) when present and
// falls back to the older `alphabet` array (list of LaTeX strings).
//
// Algebraic conjugate pairs (kind === 'algebraic') with the same pairIndex are
// fused into ONE pill labeled `W^{±}_i`.  The pill carries a KaTeX-rendered
// tooltip showing the quadratic, the two roots, and the discriminant.
//
// Rational entries render as before: label followed by `= definition`.
// ────────────────────────────────────────────────────────────────────────────
function _katexToHTML(tex, displayMode) {
  if (typeof katex === 'undefined') return escapeHTML(tex);
  try { return katex.renderToString(tex, { throwOnError: false, displayMode: !!displayMode }); }
  catch { return escapeHTML(tex); }
}
function escapeHTML(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
// Given LaTeX strings for the two roots of a quadratic (produced by
// Mathematica's TeXForm of the Together'd Sqrt expressions), build a single
// unified form that writes both roots with `\pm` instead of separate `\sqrt`
// terms. The two inputs always differ in exactly one place — a leading `-`
// on the `\sqrt{...}` subterm — so longest-common-prefix + longest-common-
// suffix localises the single edit, which we rewrite to `\pm \sqrt{...}`.
// On any mismatch the caller gets back `plusTeX` (fallback: show one root
// explicitly rather than produce a wrong ± form).
function buildPmTeX(minusTeX, plusTeX) {
  const m = (minusTeX || '').trim();
  const p = (plusTeX  || '').trim();
  if (!m || !p) return p || m;
  if (m === p) return p;
  let lp = 0;
  while (lp < m.length && lp < p.length && m[lp] === p[lp]) lp++;
  let ls = 0;
  while (ls < m.length - lp && ls < p.length - lp &&
         m[m.length - 1 - ls] === p[p.length - 1 - ls]) ls++;
  const mMid = m.slice(lp, m.length - ls).trim();
  const pMid = p.slice(lp, p.length - ls).trim();
  const prefix = m.slice(0, lp);
  const suffix = p.slice(p.length - ls);
  // Case A: the minus-root has a lone `-` where the plus-root has nothing —
  // typical after Together'd TeX renders ± as a sign flip on \sqrt{...}. The
  // pm equation puts \pm at the lost `-` position.
  if (mMid === '-' && pMid === '') return prefix + '\\pm ' + suffix;
  if (pMid === '-' && mMid === '') return prefix + '\\mp ' + suffix;
  // Case B: one mid carries `-\sqrt{...}`, the other carries `\sqrt{...}`.
  const reSqrt = /^-\s*(\\sqrt.*)$/;
  if (reSqrt.test(mMid) && /^\\sqrt/.test(pMid))
    return prefix + '\\pm ' + mMid.match(reSqrt)[1] + suffix;
  if (reSqrt.test(pMid) && /^\\sqrt/.test(mMid))
    return prefix + '\\pm ' + pMid.match(reSqrt)[1] + suffix;
  return p;  // fallback: at least one concrete root
}

function _algebraicTooltipHTML(wm, wp) {
  // wm, wp are two wDefinitions entries sharing pairIndex; wm has label W^-_i,
  // wp has label W^+_i. Both carry `polynomialTeX`, `discriminantTeX`, and
  // `definition` (the root formula). All of these can contain raw
  // variable names like "mm2" or "p(1)" -- run them through cleanTeX so
  // the tooltip matches the main result's rendering (m_2^2 etc.).
  const i = wm.pairIndex ?? wp.pairIndex ?? '';
  const poly = wm.polynomialTeX || wp.polynomialTeX || '';
  const disc = wm.discriminantTeX || wp.discriminantTeX || '';
  const wmDef = wm.definition || '';
  const wpDef = wp.definition || '';
  return (
    '<div style="min-width:260px">' +
      '<div style="margin-bottom:4px"><strong>Conjugate algebraic letters</strong>' +
        ' &mdash; roots of a quadratic.</div>' +
      (poly ? '<div>' + _katexToHTML(cleanTeX('0 = ' + poly)) + '</div>' : '') +
      '<div>' + _katexToHTML(cleanTeX('W^{-}_{' + i + '} = ' + wmDef)) + '</div>' +
      '<div>' + _katexToHTML(cleanTeX('W^{+}_{' + i + '} = ' + wpDef)) + '</div>' +
      (disc ? '<div style="margin-top:3px">Discriminant: ' + _katexToHTML(cleanTeX('\\Delta = ' + disc)) + '</div>' : '') +
    '</div>'
  );
}
// Reveal the verified badge in the top-right of the result box, with a
// rich hover popup listing the backend, max rel error, per-order psd-
// vs-symbolic comparison, and the kinematic point used. Callable from
// both the live-verify path (fresh backend run) and onIntegrationComplete
// (library-cached entry that already carries verification metadata).
function revealVerifiedBadge(res, method) {
  const badge = document.getElementById('integ-result-verified-badge');
  if (!badge) return;
  const rawErr = Number(res && res.maxRelErr);
  const sig = (Number.isFinite(rawErr) && rawErr > 0)
    ? Math.max(0, Math.round(-Math.log10(rawErr))) : 0;
  const shortLabel = sig > 0
    ? '\u2713 Verified \u00B7 ' + sig + ' digits'
    : '\u2713 Verified';
  badge.textContent = shortLabel;
  badge.dataset.tipHtml = buildVerifiedTipHtml(res, method);
  badge.style.display = '';
  // Unlock the Submit-to-Library button now that a backend has numerically
  // confirmed the symbolic result.  The gate is set at button-construction
  // time in the integ-modal footer; this is its sole lift point.
  const submit = document.getElementById('integ-submit-btn');
  if (submit && !submit.classList.contains('submitted')) {
    submit.disabled = false;
    submit.title = '';
  }
}

function fmtSci(x) {
  if (x == null || !Number.isFinite(Number(x))) return String(x ?? '?');
  const n = Number(x);
  if (n === 0) return '0';
  const absn = Math.abs(n);
  if (absn >= 1e-3 && absn < 1e4) return n.toPrecision(4);
  return n.toExponential(2);
}

function buildVerifiedTipHtml(res, fallbackMethod) {
  const mth = res && res.method || fallbackMethod || '?';
  const errRaw = res && res.maxRelErr;
  const errStr = (errRaw == null || Number(errRaw) < 0) ? 'n/a' : fmtSci(errRaw);
  const when = res && res.verifiedAt ? res.verifiedAt : '';
  const coeffs = Array.isArray(res && res.coefficients) ? res.coefficients : [];
  const kin = Array.isArray(res && res.kinematicPoint) ? res.kinematicPoint : [];

  let html = '<div style="min-width:300px;font-size:11px">' +
    '<div style="font-weight:600;color:var(--green);margin-bottom:4px">' +
    '\u2713 Numerically verified</div>' +
    '<table style="border-collapse:collapse;margin-bottom:6px">' +
    '<tr><td style="padding:1px 8px 1px 0;color:var(--text-muted)">Backend</td>' +
      '<td><code>' + escapeHtml(mth) + '</code></td></tr>' +
    '<tr><td style="padding:1px 8px 1px 0;color:var(--text-muted)">Max rel. error</td>' +
      '<td><code>' + escapeHtml(errStr) + '</code></td></tr>';
  if (when) {
    html += '<tr><td style="padding:1px 8px 1px 0;color:var(--text-muted)">Verified</td>' +
      '<td>' + escapeHtml(when) + '</td></tr>';
  }
  html += '</table>';

  if (coeffs.length > 0) {
    html += '<div style="color:var(--text-muted);margin-bottom:2px">Per-order comparison</div>' +
      '<table style="border-collapse:collapse;font-family:var(--font-mono);font-size:10px">' +
      '<tr><th style="padding:1px 6px 1px 0;text-align:right">ε^k</th>' +
      '<th style="padding:1px 6px;text-align:right">backend</th>' +
      '<th style="padding:1px 6px;text-align:right">symbolic</th>' +
      '<th style="padding:1px 6px;text-align:right">rel. err</th></tr>';
    for (const c of coeffs) {
      html += '<tr>' +
        '<td style="padding:1px 6px 1px 0;text-align:right">' + (c.order ?? '?') + '</td>' +
        '<td style="padding:1px 6px;text-align:right">' + escapeHtml(fmtSci(parseFloat(c.psd))) + '</td>' +
        '<td style="padding:1px 6px;text-align:right">' + escapeHtml(fmtSci(parseFloat(c.symbolic))) + '</td>' +
        '<td style="padding:1px 6px;text-align:right">' + escapeHtml(fmtSci(c.relErr)) + '</td>' +
        '</tr>';
    }
    html += '</table>';
  }

  if (kin.length > 0) {
    html += '<div style="color:var(--text-muted);margin-top:6px;margin-bottom:2px">Kinematic point</div>' +
      '<div style="font-family:var(--font-mono);font-size:10px;line-height:1.5">';
    for (const k of kin) {
      html += escapeHtml(k.var) + ' = ' + escapeHtml(k.value) + '<br/>';
    }
    html += '</div>';
  }

  html += '</div>';
  return html;
}

function renderAlphabetPills(container, wDefinitions, fallbackAlphabet) {
  if (!container || typeof katex === 'undefined') return;
  const hasWDefs = Array.isArray(wDefinitions) && wDefinitions.length > 0;
  const pillStyle = 'display:inline-block;padding:1px 6px;background:var(--surface);border-radius:3px;font-size:11px;border:1px solid var(--border)';
  const algPillStyle = pillStyle + ';border-style:dashed';

  if (hasWDefs) {
    // Group algebraic entries by pairIndex; rational stays ungrouped.
    const groups = []; // each group: {kind:'rational', def} or {kind:'algebraic', wm, wp}
    const pairs = new Map();
    for (const def of wDefinitions) {
      if (def.kind === 'algebraic' && def.pairIndex != null) {
        const key = def.pairIndex;
        let pair = pairs.get(key);
        if (!pair) { pair = { kind: 'algebraic', pairIndex: key }; pairs.set(key, pair); groups.push(pair); }
        const isMinus = /^W\^-_/.test(def.label) || /^W\^\{-\}/.test(def.label);
        if (isMinus) pair.wm = def; else pair.wp = def;
      } else {
        groups.push({ kind: 'rational', def });
      }
    }
    container.innerHTML = '';
    for (const g of groups) {
      const span = document.createElement('span');
      span.className = 'alphabet-letter';
      span.style.cssText = (g.kind === 'algebraic') ? algPillStyle : pillStyle;
      if (g.kind === 'rational') {
        // Apply the same TeX substitutions as the main result (cleanTeX),
        // so pill contents like "mm2" render as m_2^2, Log→\log, etc.
        const tex = cleanTeX(g.def.label + ' = ' + g.def.definition);
        try { katex.render(tex, span, { throwOnError: false }); }
        catch { span.textContent = tex; }
      } else {
        // Algebraic conjugate pair: single equation with `\pm`, built from
        // the two on-disk root TeX strings (they differ only in the sign of
        // one `\sqrt{...}` subterm, which we rewrite to `\pm\sqrt{...}`).
        const wm = g.wm || {};
        const wp = g.wp || {};
        const i  = g.pairIndex;
        const mDef = wm.definition || wm.originalLetter || '';
        const pDef = wp.definition || wp.originalLetter || '';
        const pmTex = buildPmTeX(mDef, pDef);
        const tex = cleanTeX('W^{\\pm}_{' + i + '} = ' + pmTex);
        try { katex.render(tex, span, { throwOnError: false }); }
        catch { span.textContent = tex; }
        span.dataset.tipHtml = _algebraicTooltipHTML(wm, wp);
        span.style.cursor = 'help';
      }
      container.appendChild(span);
    }
    return;
  }

  // Legacy: render raw alphabet strings as individual pills. Run
  // cleanTeX on each so legacy entries pick up the same mm_i → m_i^2
  // substitutions used by the main result view.
  if (Array.isArray(fallbackAlphabet) && fallbackAlphabet.length > 0) {
    container.innerHTML = fallbackAlphabet.map(() =>
      '<span class="alphabet-letter" style="' + pillStyle + '"></span>'
    ).join('');
    container.querySelectorAll('.alphabet-letter').forEach((el, i) => {
      const tex = cleanTeX(fallbackAlphabet[i]);
      try { katex.render(tex, el, { throwOnError: false }); }
      catch { el.textContent = tex; }
    });
  }
}

function cleanSymbolTeX(tex) {
  if (!tex) return '';
  // Inside an aligned block the '&' column markers are meaningful, so only
  // apply the cleanTeX physics substitutions; skip the '&\colon' strip.
  if (/\\begin\{aligned\}/.test(tex)) return cleanTeX(tex);
  // Otherwise strip the stray alignment marker AND run the full cleanTeX
  // pipeline so the symbol preview picks up the same p(1)→p_{1} substitutions
  // used by the main result view.
  return cleanTeX(tex.replace(/&\\colon/g, '\\colon'));
}

// Strip the outer \left(…\right) pair around every \operatorname{H}\left(…\right)
// group. Walks the string and tracks \left/\right nesting so the
// matching close is found even when the body contains further
// \left … \right pairs (e.g., \left\lbrace … \right\rbrace inside an H).
// Without this, a naive regex replace on just the opener would leave a
// dangling \right) that KaTeX rejects.
function stripHlogOuterDelim(s) {
  const OPENER = '\\operatorname{H}\\left(';
  let out = '';
  let i = 0;
  while (true) {
    const k = s.indexOf(OPENER, i);
    if (k < 0) { out += s.substring(i); return out; }
    out += s.substring(i, k) + '\\operatorname{H}(';
    let j = k + OPENER.length;
    let depth = 1;
    let rightStart = -1;
    while (j < s.length) {
      // \left / \right tokens must be followed by a non-letter delimiter
      // (e.g. `(`, `\{`, `\rbrace`) — guard so we don't match \leftarrow etc.
      if (s.startsWith('\\left', j) && /[^a-zA-Z]/.test(s[j + 5] || ' ')) {
        depth++;
        j += 5;
      } else if (s.startsWith('\\right', j) && /[^a-zA-Z]/.test(s[j + 6] || ' ')) {
        if (depth === 1) { rightStart = j; j += 6; break; }
        depth--;
        j += 6;
      } else {
        j++;
      }
    }
    if (rightStart < 0) {
      // No matching \right — bail and leave the remainder untouched.
      out += s.substring(k + OPENER.length);
      return out;
    }
    // Copy the body, skipping the \right token itself.
    out += s.substring(k + OPENER.length, rightStart);
    i = j;
  }
}

function cleanTeX(tex) {
  if (!tex) return '';
  var t = tex;

  // Strip Null artifacts from CenterDot postprocessing failures
  t = t.replace(/\\text\{Null\}/g, '?');
  t = t.replace(/\bNull\b/g, '?');

  // Step 1: Strip \text{} wrappers (handle nested braces iteratively)
  function stripText(s) {
    var prev = '';
    while (prev !== s) {
      prev = s;
      s = s.replace(/\\text\{([A-Za-z0-9_]+)\}/g, '$1');
      s = s.replace(/\\text\{([^{}]*\{[^{}]*\}[^{}]*)\}/g, '$1');
      s = s.replace(/\\text\{([^{}]+)\}/g, function(m, p1) {
        if (/^[A-Za-z0-9_{}\\^]+$/.test(p1)) return p1;
        if (/\s/.test(p1)) return '\\textrm{' + p1 + '}';  // preserve spaces
        return '\\mathrm{' + p1 + '}';
      });
    }
    return s;
  }
  t = stripText(t);

  // Step 2: Physics substitutions
  // Momentum bracket notation from TeXForm: p(1) -> p_{1}, l(1) -> \ell_{1}
  t = t.replace(/\bl\((\d+)\)/g, '\\ell_{$1}');
  t = t.replace(/\bp\((\d+)\)/g, 'p_{$1}');
  t = t.replace(/\bk\((\d+)\)/g, 'k_{$1}');
  t = t.replace(/\bq\((\d+)\)/g, 'q_{$1}');
  // Mass-squared variables: mm/MM prefix means "mass squared".
  // Numeric index: mm1 -> m_{1}^2, MM1 -> M_{1}^2
  // With power: mm1^2 -> m_{1}^4 (double the exponent since mm = m^2)
  t = t.replace(/\bmm(\d+)\^\{?(\d+)\}?/g, function(m, idx, p) { return 'm_{' + idx + '}^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bMM(\d+)\^\{?(\d+)\}?/g, function(m, idx, p) { return 'M_{' + idx + '}^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bmm(\d+)/g, 'm_{$1}^2');
  t = t.replace(/\bMM(\d+)/g, 'M_{$1}^2');
  // Alphabetic index: mmH -> m_{H}^2, mmtop -> m_{top}^2, MMW -> M_{W}^2
  t = t.replace(/\bmm([A-Z][a-z]*)\^\{?(\d+)\}?/g, function(m, idx, p) { return 'm_{' + idx + '}^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bMM([A-Z][a-z]*)\^\{?(\d+)\}?/g, function(m, idx, p) { return 'M_{' + idx + '}^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bmm([A-Z][a-z]*)/g, 'm_{$1}^2');
  t = t.replace(/\bMM([A-Z][a-z]*)/g, 'M_{$1}^2');
  // Bare mm/MM (single mass scale): mm -> m^2, MM -> M^2
  t = t.replace(/\bmm\^\{?(\d+)\}?/g, function(m, p) { return 'm^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bmm\b/g, 'm^2');
  t = t.replace(/\bMM\^\{?(\d+)\}?/g, function(m, p) { return 'M^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bMM\b/g, 'M^2');
  // Mass variables: m(2) -> m_{2}, M(2) -> M_{2}
  t = t.replace(/\bm\((\d+)\)/g, 'm_{$1}');
  t = t.replace(/\bM\((\d+)\)/g, 'M_{$1}');
  // Mass with indices: M1 -> M_{1}, m1 -> m_{1}
  t = t.replace(/\bM(\d+)/g, 'M_{$1}');
  t = t.replace(/\bm(\d+)/g, 'm_{$1}');
  // Schwinger parameters: x1 -> x_{1}
  t = t.replace(/\bx(\d+)/g, 'x_{$1}');
  // Mandelstam: s(23) -> s_{23}, s12 -> s_{12}
  t = t.replace(/\bs\((\d+)\)/g, 's_{$1}');
  t = t.replace(/\bs(\d{2,})/g, 's_{$1}');
  // External momentum squared: p1sq^2 -> M_{1}^4, p1sq -> M_{1}^2
  t = t.replace(/\bp(\d+)sq\^\{?(\d+)\}?/g, function(m, idx, p) { return 'M_{' + idx + '}^{' + (2*parseInt(p)) + '}'; });
  t = t.replace(/\bp(\d+)sq\b/g, 'M_{$1}^2');
  // Gamma roots: g1 -> g_{1}, gp -> g_{+}, gm -> g_{-}
  t = t.replace(/\bgp\b/g, 'g_{+}');
  t = t.replace(/\bgm\b/g, 'g_{-}');
  t = t.replace(/\bg(\d+)/g, 'g_{$1}');
  // eps -> \varepsilon
  t = t.replace(/\beps\b/g, '\\varepsilon');
  // O[...] and O(...) -> \mathcal{O}
  t = t.replace(/O\(([^)]*)\)/g, '\\mathcal{O}\\left($1\\right)');
  t = t.replace(/O\[([^\]]*)\]/g, '\\mathcal{O}\\left($1\\right)');
  t = t.replace(/O\\left\(/g, '\\mathcal{O}\\left(');
  t = t.replace(/O\(/g, '\\mathcal{O}(');
  // Log -> \log, Hlog -> \operatorname{H}
  t = t.replace(/(?<!\\)\bHlog\b/g, '\\operatorname{H}');
  t = t.replace(/(?<!\\)\bLog\b/g, '\\log');
  // Pi -> \pi
  t = t.replace(/(?<!\\)\bPi\b/g, '\\pi');
  // EulerGamma -> \gamma_E
  t = t.replace(/\bEulerGamma\b/g, '\\gamma_E');
  // Exponential: e^{...} -> \mathrm{e}^{...}
  t = t.replace(/(?<![a-zA-Z])e\^/g, '\\mathrm{e}^');
  // PolyGamma[n, m] -> \psi^{(n)}(m)
  t = t.replace(/PolyGamma\[(\d+),\s*(\d+)\]/g, '\\psi^{($1)}($2)');
  t = t.replace(/\bPolyGamma\b/g, '\\psi');
  // Hlog[z, {a,b,c}] -> \mathrm{Hlog}(z; a,b,c)
  t = t.replace(/\\text\{Hlog\}/g, '\\mathrm{Hlog}');
  t = t.replace(/Hlog\[([^,\]]+),\s*\\?\{([^}]*?)\\?\}\]/g, function(m, z, args) {
    return '\\mathrm{Hlog}\\!\\left(' + z + ';\\,' + args.replace(/,\s*/g, ',\\,') + '\\right)';
  });
  t = t.replace(/\\mathrm\{Hlog\}\(([^,)]+),\s*\\?\{([^}]*?)\\?\}\)/g, function(m, z, args) {
    return '\\mathrm{Hlog}\\!\\left(' + z + ';\\,' + args.replace(/,\s*/g, ',\\,') + '\\right)';
  });
  t = t.replace(/\bHlog\b/g, '\\mathrm{Hlog}');
  // mzv[a,b,c] -> \zeta_{a,b,c}
  t = t.replace(/\\text\{mzv\}/g, '\\zeta');
  t = t.replace(/mzv\[([^\]]+)\]/g, function(m, args) {
    var idx = args.replace(/[{}\\]/g, '').replace(/\s+/g, '').replace(/,/g, ',');
    return '\\zeta_{' + idx + '}';
  });
  t = t.replace(/\\mathrm\{mzv\}\(([^)]+)\)/g, function(m, args) {
    var idx = args.replace(/[{}\\]/g, '').replace(/\s+/g, '').replace(/,/g, ',');
    return '\\zeta_{' + idx + '}';
  });
  t = t.replace(/\bmzv\b/g, '\\zeta');
  // Zeta[n] -> \zeta_n
  t = t.replace(/\\text\{Zeta\}/g, '\\zeta');
  t = t.replace(/Zeta\[(\d+)\]/g, '\\zeta_{$1}');
  t = t.replace(/\\zeta\s*\((\d+)\)/g, '\\zeta_{$1}');
  t = t.replace(/(?<!\\)\bZeta\b/g, '\\zeta');
  // Gamma function
  t = t.replace(/(?<!\\)\bGamma\b/g, '\\Gamma');
  // Sqrt
  t = t.replace(/(?<!\\)\bSqrt\b/g, '\\sqrt');

  // Step 3: Remaining \text{} -> \mathrm{}
  t = t.replace(/\\text\{([^{}]*)\}/g, '\\mathrm{$1}');

  // Step 3b: KaTeX compatibility fixes
  // \left\{ and \right\} as set braces cause nesting issues in KaTeX.
  // Replace with \lbrace / \rbrace which are plain delimiters.
  t = t.replace(/\\left\\{/g, '\\left\\lbrace ');
  t = t.replace(/\\right\\}/g, '\\right\\rbrace ');
  // Unmatched \left..\right from TeXForm: strip to plain parens
  // Count left/right balance; if off, strip all \left \right
  var lc = (t.match(/\\left[^a-zA-Z]/g) || []).length;
  var rc = (t.match(/\\right[^a-zA-Z]/g) || []).length;
  if (lc !== rc) {
    t = t.replace(/\\left\s*([(\[{.|])/g, '$1');
    t = t.replace(/\\right\s*([)\]}.)|])/g, '$1');
    t = t.replace(/\\left\\lbrace\s*/g, '\\{');
    t = t.replace(/\\right\\rbrace\s*/g, '\\}');
  }
  // \operatorname{H}\left(\left\{...\right\},...\right) pattern from Hlog.
  // The outer \left(…\right) pair is redundant for KaTeX and tends to
  // trip up its nested-bracket parser. Strip both halves together
  // (balance-aware) — a naive `\left(` → `(` replace would leave the
  // matching `\right)` dangling and KaTeX would error out on the
  // unbalanced `\right)`.
  t = stripHlogOuterDelim(t);
  t = t.replace(/\\operatorname\{H\}\\\!/g, '\\operatorname{H}');

  // Step 4: Clean up spacing
  t = t.replace(/\\log \^/g, '\\log^');

  // Step 4a: balance \begin{aligned} / \end{aligned}.  The library's
  // save-side TeX generator occasionally truncates long results at
  // '+ \cdots' without closing the environment (KaTeX then fails with
  // "Expected & or \\ or \cr or \end").  Also guards against stray
  // \begin without a matching \end and vice versa.  Same treatment for
  // \left[ / \right] pairs that get cut mid-expression.
  {
    const nBegin = (t.match(/\\begin\{aligned\}/g) || []).length;
    const nEnd   = (t.match(/\\end\{aligned\}/g) || []).length;
    if (nBegin > nEnd) t += '\\end{aligned}'.repeat(nBegin - nEnd);
    else if (nEnd > nBegin) t = '\\begin{aligned}'.repeat(nEnd - nBegin) + t;
  }
  {
    // Any `\right<d>` form closes a `\left`, including `\right.` (matches
    // any opener).  Count `\left<nonletter>` and `\right<nonletter>`
    // tokens so the balancer doesn't double-close when truncation already
    // appended `\right.` closers (see stTruncateTeX).
    const nL = (t.match(/\\left(?![A-Za-z])/g) || []).length;
    const nR = (t.match(/\\right(?![A-Za-z])/g) || []).length;
    if (nL > nR) t += '\\right.'.repeat(nL - nR);
  }

  // Step 4b: wrap double superscripts.  SubTropica's stored TeX emits raw
  // patterns like `W_1^-^{-1}` or `m^2^2` or `(W_1^+)^{-1}` → `W_1^+^{-1}`
  // that LaTeX parses as "A^B^C" — illegal without brace grouping.  KaTeX
  // flags these as "Double superscript" and renders a red error.  Wrap the
  // first superscript with its base so the second becomes unambiguous.
  //
  // Repeated passes handle triple-supers (A^B^C^D → {A^B}^C^D → {{A^B}^C}^D).
  // The pattern below matches a base token (letter/group/subscripted-letter)
  // followed by two consecutive superscript groups; replaces with one
  // brace-wrapped combined base.
  for (let pass = 0; pass < 4; pass++) {
    const before = t;
    t = t.replace(
      // Base (letter with optional subscript, OR a single char, OR ) / ] / } close)
      /([A-Za-z](?:_\{[^{}]*\}|_[A-Za-z0-9])?|[\)\]\}])\^(\{[^{}]*\}|[A-Za-z0-9+\-])\^(\{|\S)/g,
      '{$1^$2}^$3');
    if (t === before) break;
  }

  // Step 5: Compact +/- operators ({+} and {-} render tighter in KaTeX)
  // Only replace at brace depth 0 to avoid breaking \frac{a-b}{c+d}
  var out = '', depth = 0;
  for (var ci = 0; ci < t.length; ci++) {
    if (t[ci] === '{') { depth++; out += '{'; }
    else if (t[ci] === '}') { depth--; out += '}'; }
    else if (depth === 0 && (t[ci] === '+' || t[ci] === '-')) {
      out += '{' + t[ci] + '}';
    } else { out += t[ci]; }
  }
  t = out;

  return t;
}

function wrapSymanzikIntegral(bodyTeX, nvars) {
  if (!nvars || nvars < 1) return bodyTeX;
  var prefactor = '';
  var integrand = bodyTeX;
  // Heuristic: split leading prefactor (fraction without x_{})
  var fracMatch = bodyTeX.match(/^(\\frac\{[^{}]*(?:\{[^{}]*\}[^{}]*)?\}\{[^{}]*(?:\{[^{}]*\}[^{}]*)?\})\s*(.*)$/);
  if (fracMatch) {
    var candidate = fracMatch[1];
    if (candidate.indexOf('x_{') < 0) {
      prefactor = candidate + '\\;';
      integrand = fracMatch[2];
    }
  }
  var measure = '\\int \\frac{\\mathrm{d}^{' + nvars + '}x}{\\mathrm{GL}(1)}\\;';
  return prefactor + measure + integrand;
}

/**
 * Map raw error messages + progress log to user-friendly explanations.
 * Returns an HTML string (may contain <b> tags for emphasis).
 */
function diagnoseFriendlyError(rawError, progressLog) {
  const log = progressLog || '';
  const err = rawError || '';

  // 1. No linearly reducible order found
  if (/[Nn]o linearly reducible|noorder|nolr|No valid gauge found/.test(log + err)) {
    return '<span style="color:var(--red);font-size:18px;vertical-align:middle">\u2716</span> ' +
      '<b>Not linearly reducible.</b> ' +
      'This integral is not linearly reducible. Try a change of variables to rationalize square roots.';
  }

  // 2. Scaleless integral (F polynomial vanishes)
  if (/scaleless|[Zz]eroF|F.*polynomial.*vanish|F.*polynomial.*zero/.test(log + err)) {
    return '<b>Scaleless integral.</b> ' +
      'The F (second Symanzik) polynomial vanishes \u2014 the integral is scaleless and evaluates to 0 in dimensional regularization.';
  }

  // 5. Polymake failure
  if (/polymake|couldn.*find output|Newton polytope/.test(log + err)) {
    return '<b>Polymake error.</b> ' +
      'The Newton polytope computation failed. Check that <code>polymake</code> is installed and configured correctly via <code>ConfigureSubTropica[]</code>.';
  }

  // 6. Timeout / memory limit
  if (/[Tt]ime.*[Bb]ound|[Tt]imeout|[Mm]emory.*limit|[Mm]embound|exceeded.*limit/.test(log + err)) {
    return '<b>Resource limit exceeded.</b> ' +
      'Integration exceeded the time or memory limit. Try increasing <code>TimeUpperBound</code> or reducing the number of gauges.';
  }

  // Aborted
  if (/\$Aborted|[Rr]ecursion[Ll]imit|user abort/.test(err)) {
    return '<b>Integration aborted.</b> ' +
      'The computation was aborted (possibly due to RecursionLimit or a manual interrupt).';
  }

  // Generic fallback
  return escapeHtml(rawError || 'Integration failed.');
}

function onIntegrationComplete(data) {
  if (_integrationDone) return;
  _integrationDone = true;
  stopProgressAnimation();
  _lastIntegrationData = data;

  // Timings render incrementally as "[timing]" lines arrive — no
  // explicit post-mortem hook needed here.

  // Reveal the Copy-failure-report button when the run has any warn/error
  // records, or the result itself is an error. Idempotent (safe on rerender).
  const failureBtn = document.getElementById('log-btn-failure');
  if (failureBtn) {
    const hasBad = _logRecordCounts.error > 0 || _logRecordCounts.warn > 0 ||
      (data && data.status === 'error');
    failureBtn.style.display = hasBad ? '' : 'none';
  }

  // Upgrade the SubTropica-view command to the kernel's authoritative
  // stCommand when it arrives. If the user is currently viewing that mode,
  // re-render so the text updates live.
  if (data && typeof data.stCommand === 'string' && data.stCommand) {
    const view = $('integ-formula-view');
    if (view) {
      view._stCmd = fixCenterDot(data.stCommand);
      if (view.dataset.mode === 'subtropica') {
        switchIntegFormula('subtropica');
      }
    }
  }

  const box = $('integ-result-box');
  const loading = $('integ-loading-state');
  const resultContent = $('integ-result-content');
  const statusEl = $('integ-status-text');

  if (data.status === 'ok') {
    // Snap progress to 100%
    _currentProgress = 100;
    if (box) {
      box.style.setProperty('--progress', '100%');
      box.classList.add('complete');
    }
    // Celebrate only for genuine results (not errors disguised as ok)
    const resultStr = data.result || data.resultTeX || '';
    const hasError = /\$Failed|\$Aborted|error|Error/.test(resultStr);
    // Delay coconut celebration until after result render to avoid choppiness
    if (!data._cached && !hasError) setTimeout(coconutCelebration, 800);

    // Update status
    const isCached = data._cached;
    const wasTransformed = isCached && data._transformDescriptions && data._transformDescriptions.length > 0;
    if (statusEl) {
      statusEl._done = true;
      if (isCached) {
        statusEl.textContent = wasTransformed
          ? '\u2605 Loaded from library (with substitutions)'
          : '\u2605 Result from library';
      } else {
        const elapsed = ((Date.now() - _integStartTime) / 1000).toFixed(1);
        statusEl.textContent = 'Complete \u2014 ' + formatElapsed(Math.round(elapsed));
      }
    }

    // Library-cached entries may carry a verified flag. Surface the
    // top-right badge with whatever metadata the record has (method,
    // max rel err, verifiedAt). Per-order coefficients and the
    // kinematic point aren't persisted, so the hover popup is sparser
    // than the live-verify version but still useful.
    if (isCached && data.verified) {
      revealVerifiedBadge({
        method: data.verifiedMethod,
        maxRelErr: data.verifiedMaxRelErr,
        verifiedAt: data.verifiedAt,
      }, data.verifiedMethod);
    }

    // Append timing/source to summary card
    const params = $('integ-params');
    if (params) {
      const timing = document.createElement('div');
      timing.className = 'integ-timing';
      if (isCached) {
        timing.textContent = '\u2605 Loaded from library';
      } else {
        const elapsed = ((Date.now() - _integStartTime) / 1000).toFixed(1);
        timing.textContent = 'Time: ' + formatElapsed(Math.round(elapsed));
      }
      if (data.byteCount) timing.textContent += ' \u00B7 ' + formatBytes(data.byteCount);
      params.appendChild(timing);

      // Show applied substitutions when a soft-transform produced this result
      if (wasTransformed) {
        const subsNote = document.createElement('div');
        subsNote.style.cssText = 'font-size:10px;color:var(--text-muted);line-height:1.4;margin-top:4px;font-style:italic';
        subsNote.textContent = 'Applied: ' + data._transformDescriptions.join('; ');
        params.appendChild(subsNote);
      }
    }

    // Reveal result (instant for cached, brief delay for live)
    const revealDelay = isCached ? 50 : 400;
    setTimeout(() => {
      if (loading) loading.style.display = 'none';
      if (resultContent) {
        // Build result content. Layout mirrors the Integral frame:
        //   [Result label]
        //   <result TeX>
        //   <alphabet>
        //   <details: Symbol>
        //                                    [Copy LaTeX] [Copy Mathematica]
        // Override the inherited text-align:center from .result-box-inner
        // so the "Result" label sits flush-left, matching the Integral
        // frame's label placement.
        let html = '<div style="margin-bottom:10px;width:100%;flex-shrink:0;text-align:left">';
        html += '<span class="integ-param-label" style="margin:0">Result</span>';
        html += '</div>';
        html += '<div style="width:100%;overflow-x:auto;overflow-y:auto;max-height:280px">';
        html += '<div id="integ-result-tex" style="padding:4px 0;min-width:min-content"></div>';
        html += '<pre id="integ-result-mma" style="font-family:var(--font-mono);font-size:11px;color:var(--text);white-space:pre;line-height:1.5;padding:4px 0;display:none;text-align:left;min-width:min-content"></pre>';
        html += '</div>';

        // Alphabet (always visible as inline pills)
        if (data.wDefinitions && data.wDefinitions.length > 0) {
          html += '<div style="margin-top:10px;border-top:1px solid var(--border);padding-top:8px;display:flex;align-items:baseline;gap:6px;flex-wrap:wrap">';
          html += '<span style="font-size:11px;color:var(--text-muted);font-weight:500;white-space:nowrap">Alphabet</span>';
          html += '<div id="integ-result-alphabet" class="integ-result-alphabet-w" style="display:flex;flex-wrap:wrap;gap:3px;line-height:1"></div>';
          html += '</div>';
        } else if (data.alphabet && data.alphabet.length > 0) {
          html += '<div style="margin-top:10px;border-top:1px solid var(--border);padding-top:8px;display:flex;align-items:baseline;gap:6px;flex-wrap:wrap">';
          html += '<span style="font-size:11px;color:var(--text-muted);font-weight:500;white-space:nowrap">Alphabet</span>';
          html += '<div id="integ-result-alphabet" style="display:flex;flex-wrap:wrap;gap:3px;line-height:1"></div>';
          html += '</div>';
        }

        // Symbol (collapsible, with weight and term count)
        {
          const symTex = data.normalizedSymbolTeX || data.symbolTeX;
          if (symTex) {
            const wt = data.symbolWeight || 0;
            const origNt = data.symbolTerms || 0;
            const normNt = data.normalizedSymbolTerms || 0;
            const symMeta = [];
            if (wt > 0) symMeta.push('weight ' + wt);
            if (normNt > 0 && normNt !== origNt) symMeta.push(origNt + '\u2009\u2192\u2009' + normNt + ' terms');
            else if (origNt > 0) symMeta.push(origNt + ' term' + (origNt !== 1 ? 's' : ''));
            const symLabel = 'Symbol' + (symMeta.length > 0 ? ' (' + symMeta.join(', ') + ')' : '');
            html += '<details style="margin-top:8px">';
            html += '<summary style="cursor:pointer;font-size:12px;color:var(--text-muted);font-weight:500">' + symLabel + '</summary>';
            html += '<div id="integ-result-symbol" style="overflow-x:auto;padding:4px 0;margin-top:4px;max-height:200px;overflow-y:auto"></div>';
            html += '<button class="integ-formula-btn" id="integ-copy-symbol" style="margin-top:4px">Copy Symbol</button>';
            html += '</details>';
          }
        }

        // Footer row with Copy LaTeX / Copy Mathematica pinned bottom-right
        // (mirrors the Integral frame's footer).
        html += '<div class="integ-result-footer">';
        html += '<div class="modal-btn-row">';
        html += '<button class="integ-formula-btn" id="integ-copy-tex">Copy LaTeX</button>';
        html += '<button class="integ-formula-btn" id="integ-copy-mma">Copy Mathematica</button>';
        html += '</div></div>';

        resultContent.innerHTML = html;

        // Render TeX (truncate if too long to avoid KaTeX slowness)
        const texView = $('integ-result-tex');
        const mmaView = $('integ-result-mma');
        let cleaned = cleanTeX(data.resultTeX || '');
        if (cleaned.length > 3000) {
          // Truncate at a safe boundary: find the last top-level + or -
          // before the limit to avoid cutting inside \frac{}{}, \log(), etc.
          let cutPos = 3000;
          let depth = 0;
          // Walk backwards from the limit to find a top-level +/- operator
          for (let ci = Math.min(cleaned.length - 1, 3200); ci >= 2000; ci--) {
            const ch = cleaned[ci];
            if (ch === '}' || ch === ')') depth++;
            else if (ch === '{' || ch === '(') depth--;
            if (depth === 0 && (ch === '+' || ch === '-') && ci < 3200) {
              cutPos = ci;
              break;
            }
          }
          cleaned = cleaned.slice(0, cutPos);
          // Balance any remaining unmatched delimiters
          const nOpen = (cleaned.match(/\{/g) || []).length - (cleaned.match(/\}/g) || []).length;
          const nLeft = (cleaned.match(/\\left/g) || []).length;
          const nRight = (cleaned.match(/\\right/g) || []).length;
          cleaned += '}'.repeat(Math.max(nOpen, 0));
          cleaned += '\\right)'.repeat(Math.max(nLeft - nRight, 0));
          cleaned += ' + \\cdots';
        }
        if (cleaned && typeof katex !== 'undefined') {
          try {
            katex.render('\\displaystyle ' + cleaned, texView, { throwOnError: false, displayMode: true, maxSize: 500 });
          } catch {
            texView.textContent = data.resultTeX || data.result;
          }
        } else {
          texView.style.display = 'none';
          if (mmaView) { mmaView.style.display = 'block'; mmaView.textContent = data.result || ''; }
        }

        // Render symbol TeX (prefer normalized)
        const symbolView = $('integ-result-symbol');
        {
          const symTex = cleanSymbolTeX(data.normalizedSymbolTeX || data.symbolTeX);
          if (symbolView && symTex && typeof katex !== 'undefined') {
            try {
              katex.render('\\displaystyle ' + symTex, symbolView, {
                throwOnError: false, displayMode: true, maxSize: 500
              });
            } catch { symbolView.textContent = symTex; }
          }
        }

        // Render alphabet pills (prefer W definitions; algebraic pairs fuse)
        const alphView = $('integ-result-alphabet');
        renderAlphabetPills(alphView, data.wDefinitions, data.alphabet);

        // Copy handlers
        $('integ-copy-tex')?.addEventListener('click', () => {
          navigator.clipboard.writeText(data.resultTeX || cleaned).then(() => showWarningToast('LaTeX copied'));
        });
        $('integ-copy-mma')?.addEventListener('click', () => {
          navigator.clipboard.writeText(data.result || '').then(() => showWarningToast('Mathematica copied'));
        });
        $('integ-copy-symbol')?.addEventListener('click', () => {
          const symTex = cleanSymbolTeX(data.normalizedSymbolTeX || data.symbolTeX);
          navigator.clipboard.writeText(symTex).then(() => showWarningToast('Symbol LaTeX copied'));
        });

        resultContent.classList.add('visible');
      }
    }, 400);

    // Add footer
    setTimeout(() => {
      const modalBody = $('integrate-body');
      if (!modalBody) return;

      const footer = document.createElement('div');
      footer.style.cssText = 'display:flex;justify-content:space-between;align-items:center;margin-top:16px;padding-top:12px;border-top:1px solid var(--border)';

      if (isCached) {
        // Cached result: offer to recompute from scratch
        const rerunBtn = document.createElement('button');
        rerunBtn.className = 'btn btn-sm btn-secondary';
        rerunBtn.textContent = 'Integrate from scratch';
        rerunBtn.addEventListener('click', () => {
          // Reset state and launch fresh integration in the same modal
          if (_integrationPolling) { clearInterval(_integrationPolling); _integrationPolling = null; }
          _integrationDone = false;
          _cachedSymanzikTeX = null;
          // Re-collect the payload from the current canvas state (the original
          // payload variable is not in scope — it lived in doIntegrate's closure).
          const freshPayload = collectIntegrationPayload();
          doIntegrateForce(freshPayload);
        });
        footer.appendChild(rerunBtn);

        const backBtn = document.createElement('button');
        backBtn.className = 'btn btn-sm btn-primary';
        backBtn.innerHTML = '\u2190 Back to Mathematica';
        backBtn.addEventListener('click', () => returnToMathematica());
        footer.appendChild(backBtn);
      } else {
        // Verify button with hover-popup method selector (like Submit's contributor popup)
        const verifyWrap = document.createElement('div');
        verifyWrap.className = 'integ-verify-wrap';
        let _verifyMethod = 'pySecDec';  // default

        const verifyBtn = document.createElement('button');
        verifyBtn.className = 'btn btn-sm btn-secondary';
        verifyBtn.id = 'integ-verify-btn';
        verifyBtn.innerHTML = '\u2714 Verify numerically';

        // Method popup (revealed on hover)
        const methodPopup = document.createElement('div');
        methodPopup.className = 'integ-verify-popup';

        // Fetch deps fresh if not yet populated (can happen if deps.json
        // wasn't written when detectBackendMode ran at page load).
        const buildMethodList = (deps) => {
          return [
            { key: 'pySecDec', dep: 'pySecDec', label: 'pySecDec' },
            { key: 'FIESTA',   dep: 'FIESTA',   label: 'FIESTA' },
            { key: 'AMFlow',   dep: 'AMFlow',   label: 'AMFlow' },
            { key: 'feyntrop', dep: 'feyntrop', label: 'feyntrop' },
          ].filter(m => deps[m.dep] === 'ok');
        };
        const populatePopup = (methods) => {
          methodPopup.innerHTML = '';
          if (methods.length === 0) {
            verifyBtn.disabled = true;
            verifyBtn.title = 'No numerical backend installed (pySecDec, FIESTA, AMFlow, feyntrop)';
            return;
          }
          // Re-enable in case an earlier synchronous populate ran with an
          // empty deps cache and disabled the button. The async
          // kernel.ping() refresh below would otherwise rebuild the
          // method list but leave disabled=true in place.
          verifyBtn.disabled = false;
          verifyBtn.title = '';
          _verifyMethod = methods[0].key;
          methods.forEach(m => {
            const row = document.createElement('label');
            row.className = 'integ-verify-method-row';
            row.innerHTML = `<input type="radio" name="verify-method" value="${m.key}" ${m.key === _verifyMethod ? 'checked' : ''}/> <span>${m.label}</span>`;
            row.querySelector('input').addEventListener('change', (e) => { _verifyMethod = e.target.value; });
            methodPopup.appendChild(row);
          });
        };

        // Try cached deps first, then refresh from kernel
        let availableMethods = buildMethodList(backendDeps);
        if (availableMethods.length === 0 && backendMode === 'full') {
          // Deps not yet loaded — fetch now
          kernel.ping().then(data => {
            if (data && data.deps) {
              backendDeps = data.deps;
              availableMethods = buildMethodList(backendDeps);
              populatePopup(availableMethods);
            }
          }).catch(() => {});
        }

        populatePopup(availableMethods);
        verifyWrap.appendChild(methodPopup);

        // ── Helpers for the verify flow ──
        // Restore the button to its default state so the user can pick
        // a different backend from the hover popup. The persistent
        // pass/fail signal lives in the top-right badge (on success) or
        // the inline hint row (on failure), not in the button itself.
        const restoreVerifyButton = (btn) => {
          btn.disabled = false;
          btn.innerHTML = '\u2714 Verify numerically';
          btn.className = 'btn btn-sm btn-secondary';
        };
        const clearVerifyHint = () => {
          const h = document.getElementById('integ-verify-hint');
          if (h) { h.innerHTML = ''; h.style.display = 'none'; }
        };
        // Map a verification failure reason to an alternative backend.
        // Returns null when no suggestion applies (or the suggested
        // backend isn't installed, or it's the one that just failed).
        const suggestBackendFor = (reason, currentMethod, avail) => {
          const names = avail.map(m => m.key);
          const pick = (candidates) => candidates.find(
            k => names.includes(k) && k !== currentMethod) || null;
          const r = String(reason || '').toLowerCase();
          if (r.includes('shared-mass') || r.includes('euclidean')) {
            return pick(['AMFlow', 'FIESTA']);
          }
          if (r.includes('ginsh') || r.includes('non-numeric')) {
            return pick(['AMFlow', 'pySecDec', 'FIESTA']);
          }
          if (r.includes('numeric backend failed') || r.includes('network')) {
            return pick(['AMFlow', 'pySecDec', 'FIESTA', 'feyntrop']);
          }
          return null;
        };
        const showVerifyFailHint = (reason, currentMethod, avail) => {
          const h = document.getElementById('integ-verify-hint');
          if (!h) return;
          const suggestion = suggestBackendFor(reason, currentMethod, avail);
          const parts = ['<span class="integ-verify-hint-fail">\u2717 ' +
            escapeHtml(String(reason || 'Verification failed')) + '</span>'];
          parts.push('<button type="button" class="integ-verify-hint-link" ' +
            'data-action="details">Details \u2192 Log</button>');
          if (suggestion) {
            parts.push('<button type="button" class="integ-verify-hint-link" ' +
              'data-action="retry" data-method="' + escapeHtml(suggestion) +
              '">Try ' + escapeHtml(suggestion) + '?</button>');
          }
          h.innerHTML = parts.join(' \u00B7 ');
          h.style.display = '';
          h.querySelectorAll('[data-action]').forEach(el => {
            el.addEventListener('click', ev => {
              ev.preventDefault();
              const act = el.dataset.action;
              if (act === 'details') {
                openLogTab();
                jumpToFirst(_logRecordCounts.error > 0 ? 'error' : 'warn');
              } else if (act === 'retry' && el.dataset.method) {
                _verifyMethod = el.dataset.method;
                runVerify(_verifyMethod);
              }
            });
          });
        };

        // Run verification
        const runVerify = async (method) => {
          clearVerifyHint();
          verifyBtn.disabled = true;
          verifyBtn.innerHTML = '<span class="spinner-sm"></span> ' + method + '\u2026';
          try {
            await fetch('/api/verify', {
              method: 'POST',
              headers: { 'Content-Type': 'application/json' },
              body: JSON.stringify({ tolerance: 1e-3, method })
            });
            const poll = setInterval(async () => {
              try {
                const res = await (await fetch('/api/verify_result')).json();
                if (res.status === 'ok') {
                  clearInterval(poll);
                  if (res.pass) {
                    revealVerifiedBadge(res, method);
                    clearVerifyHint();
                  } else {
                    showVerifyFailHint(
                      res.reason || 'Mismatch', method, availableMethods);
                  }
                  restoreVerifyButton(verifyBtn);
                } else if (res.status === 'error') {
                  clearInterval(poll);
                  showVerifyFailHint(
                    res.error || 'Error', method, availableMethods);
                  restoreVerifyButton(verifyBtn);
                }
              } catch {}
            }, 2000);
          } catch {
            showVerifyFailHint(
              'Network error — could not reach kernel', method, availableMethods);
            restoreVerifyButton(verifyBtn);
          }
        };
        verifyBtn.addEventListener('click', () => runVerify(_verifyMethod));
        verifyWrap.appendChild(verifyBtn);
        footer.appendChild(verifyWrap);

        // Verification hint slot (hidden by default). Populated on
        // failure with the reason text + "Details → Log" jump link +
        // optional "Try <backend>?" suggestion. flex-basis:100% pushes
        // it onto its own row below the buttons.
        const verifyHint = document.createElement('div');
        verifyHint.id = 'integ-verify-hint';
        verifyHint.className = 'integ-verify-hint';
        verifyHint.style.display = 'none';
        footer.appendChild(verifyHint);

        // Live result: Submit button (with contributor hover popup) + Back button
        const submitWrap = document.createElement('div');
        submitWrap.className = 'integ-submit-wrap';

        const submitBtn = document.createElement('button');
        submitBtn.className = 'btn btn-sm btn-submit';
        submitBtn.id = 'integ-submit-btn';
        submitBtn.innerHTML = '\u2191 Submit to Library';
        // Gate submission on numerical verification: the button starts
        // disabled and flips to enabled inside revealVerifiedBadge() once a
        // backend (pySecDec / FIESTA / AMFlow / feyntrop) has confirmed the
        // symbolic result against the numeric value.  Library-cached entries
        // that already carry a verified flag (onIntegrationComplete path)
        // reach the same revealVerifiedBadge() call, so a cached verified
        // result arrives with Submit pre-enabled.  Gate lifted in the submit
        // click handler is not reset on failure because a failed submission
        // does not invalidate verification — the user retries with the same
        // verified result.
        submitBtn.disabled = true;
        submitBtn.title = 'Verify the result numerically before submitting';
        submitBtn.addEventListener('click', async () => {
          submitBtn.disabled = true;
          submitBtn.textContent = 'Submitting\u2026';
          try {
            // Flush the contributor to the kernel before firing /api/submit.
            // The 'input' listener below only runs when the field's value
            // changes via user input — it won't have fired yet if the user
            // typed a name and immediately clicked Submit without blurring.
            // Awaiting setContributor here guarantees STSubmitResult picks
            // up the current DOM state.
            const name = $('integ-contributor-name')?.value || '';
            const anon = $('integ-contributor-anon')?.checked || false;
            await kernel.post('setContributor', { name, anonymous: anon });

            // Create SUBMIT sentinel — kernel polls for this and runs STSubmitResult
            await fetch('/api/submit', {
              method: 'POST',
              headers: { 'Content-Type': 'text/plain' },
              body: ''
            });
            submitBtn.textContent = '\u2713 Submitted';
            submitBtn.classList.add('submitted');
          } catch {
            submitBtn.textContent = 'Submission failed';
            submitBtn.disabled = false;
          }
        });
        submitWrap.appendChild(submitBtn);

        // Contributor popup (revealed on hover)
        const contribPopup = document.createElement('div');
        contribPopup.className = 'integ-contrib-popup';
        contribPopup.innerHTML = `
          <div class="integ-contrib-row">
            <label class="ic-label">Name</label>
            <input type="text" id="integ-contributor-name" class="ic-input" placeholder="Anonymous" disabled style="flex:1;font-size:11px"/>
          </div>
          <label class="integ-contrib-row" style="cursor:pointer">
            <input type="checkbox" id="integ-contributor-anon" checked/>
            <span style="font-size:11px;color:var(--text-mid)">Submit anonymously</span>
          </label>
        `;
        submitWrap.appendChild(contribPopup);

        // Fetch current contributor from kernel (may override anonymous default).
        // Stash the last-seen name on the input so that re-checking "anonymous"
        // doesn't wipe it from memory; the user can restore it by unchecking.
        kernel.get('getContributor').then(c => {
          const nameInput = $('integ-contributor-name');
          const anonCb = $('integ-contributor-anon');
          if (!c) return;
          if (nameInput && c.name) nameInput.dataset.lastName = c.name;
          if (c.anonymous === false) {
            if (anonCb) anonCb.checked = false;
            if (nameInput) {
              nameInput.disabled = false;
              nameInput.placeholder = 'Your name';
              if (c.name) nameInput.value = c.name;
            }
          }
        }).catch(() => {});

        // Save contributor when the name or anonymous state changes. Use
        // 'input' (not 'change') so each keystroke is flushed to the kernel,
        // otherwise a click on Submit before blurring the field would submit
        // with a stale contributor name.
        const saveContrib = () => {
          const name = $('integ-contributor-name')?.value || '';
          const anon = $('integ-contributor-anon')?.checked || false;
          kernel.post('setContributor', { name, anonymous: anon }).catch(() => {});
        };
        const nameInputEl = contribPopup.querySelector('#integ-contributor-name');
        nameInputEl.addEventListener('input', () => {
          if (nameInputEl.value) nameInputEl.dataset.lastName = nameInputEl.value;
          saveContrib();
        });
        contribPopup.querySelector('#integ-contributor-anon').addEventListener('change', function() {
          const nameInput = $('integ-contributor-name');
          if (nameInput) {
            nameInput.disabled = this.checked;
            nameInput.placeholder = this.checked ? 'Anonymous' : 'Your name';
            if (this.checked) {
              // Keep the typed name cached on the dataset so it can come back
              // when the user un-checks anonymous, but clear the visible field
              // so anonymous means anonymous.
              if (nameInput.value) nameInput.dataset.lastName = nameInput.value;
              nameInput.value = '';
            } else if (nameInput.dataset.lastName) {
              nameInput.value = nameInput.dataset.lastName;
            }
          }
          saveContrib();
        });

        footer.appendChild(submitWrap);

        const backBtn = document.createElement('button');
        backBtn.className = 'btn btn-sm btn-primary';
        backBtn.innerHTML = '\u2190 Back to Mathematica';
        backBtn.addEventListener('click', () => returnToMathematica());
        footer.appendChild(backBtn);
      }

      modalBody.appendChild(footer);
    }, 500);

  } else {
    // Failed — fetch progress log for detailed error diagnosis
    if (box) box.classList.add('failed');
    const showError = (msg) => {
      setTimeout(() => {
        if (loading) loading.style.display = 'none';
        if (resultContent) {
          resultContent.innerHTML = '<div style="color:var(--red);font-size:13px;padding:8px 0">' +
            msg + '</div>';
          resultContent.classList.add('visible');
        }
      }, 300);
    };

    // Try to get the progress log for a more informative error message
    kernel.get('progress').then(prog => {
      const log = (prog && prog.log) || '';
      const friendly = diagnoseFriendlyError(data.error || '', log);
      showError(friendly);
    }).catch(() => {
      showError(diagnoseFriendlyError(data.error || '', ''));
    });
  }
}

function formatBytes(b) {
  if (!b || b < 1024) return b + ' B';
  if (b < 1048576) return (b / 1024).toFixed(1) + ' KB';
  return (b / 1048576).toFixed(1) + ' MB';
}

// ── Library result deletion ──

function deleteLibraryResult(topoKey, configKey, resultIndex) {
  // Remove from in-memory library
  if (library && library.topologies) {
    const topo = library.topologies[topoKey];
    if (topo && topo.configs) {
      const cfg = topo.configs[configKey];
      if (cfg) {
        const results = cfg.results || cfg.Results || [];
        if (resultIndex >= 0 && resultIndex < results.length) {
          results.splice(resultIndex, 1);
          if (cfg.results) cfg.results = results;
          if (cfg.Results) cfg.Results = results;
        }
      }
    }
  }

  // In full mode, tell the kernel to delete from disk
  if (backendMode === 'full') {
    kernel.post('deleteResult', {
      topoKey,
      configKey,
      resultIndex,
    }).catch(() => {});
  }

  showWarningToast('Result deleted');
  // Trigger re-match to update star indicators
  onGraphChanged();
}

// ── Library result lookup ──

// Canonicalize a numerator row list to a stable string for equality checks.
// Payload side stores `[{expr, exp}, ...]`; kernel side stores `[[expr, exp], ...]`
// (from the uiResult assoc in handleIntegrate). Normalize both to the pair form.
function _canonicalizeNumerators(rows) {
  if (!rows) return '[]';
  const norm = rows.map(r => {
    if (Array.isArray(r)) return [String(r[0] ?? ''), parseInt(r[1] ?? -1)];
    return [String(r.expr ?? ''), parseInt(r.exp ?? -1)];
  });
  return JSON.stringify(norm);
}

// Canonicalize a mass list for exact-match comparison. The UI payload and
// the kernel's stored form use different shapes — see lookupLibraryResult
// for the full story. We collapse both to the same normal form: drop '0'
// entries and sort so order doesn't matter (positional mass identity is
// TODO soft-transform territory, see the note on lookupLibraryResult).
function _canonicalizeMassList(list) {
  if (!Array.isArray(list)) return '[]';
  const filtered = list.map(m => String(m ?? '0')).filter(m => m && m !== '0' && m !== '0.');
  filtered.sort();
  return JSON.stringify(filtered);
}

// Compute the "internal edges" mask from a payload.edgePairs array. A
// vertex with only one incident edge is an external leg endpoint; an edge
// with any such endpoint is external. This mirrors the logic inside
// collectIntegrationPayload()/handleLock so we can realign payload-shape
// (all edges) to stored-shape (internal edges only) without threading an
// explicit index through the payload.
function _internalEdgeMask(edgePairs) {
  if (!Array.isArray(edgePairs) || edgePairs.length === 0) return [];
  const deg = {};
  for (const [a, b] of edgePairs) {
    deg[a] = (deg[a] || 0) + 1;
    deg[b] = (deg[b] || 0) + 1;
  }
  return edgePairs.map(([a, b]) => (deg[a] || 0) > 1 && (deg[b] || 0) > 1);
}

/**
 * Look up a cached library result for the current diagram + options.
 *
 * Minimal exact-match policy: every result-altering field must match the
 * payload verbatim. That includes mass names (internal and external) and
 * the numerator row list. If anything differs we fall through to a fresh
 * kernel integration.
 *
 * Hard match (all must be identical):
 *   1. Topology Nickel index
 *   2. Mass config (exact via classifyConfigMatch)
 *   3. Dimension
 *   4. Epsilon order (cached >= requested)
 *   5. Propagator exponents
 *   6. Numerator rows (expr + exponent, positional)
 *   7. Internal mass names
 *   8. External mass names
 *   9. Substitutions
 *   10. Normalization
 *
 * TODO(soft-transform): a prior version of this function applied kernel-side
 * rewrites for mass renames, substitutions, and normalization rescaling so
 * a "close enough" cached result could be transformed into the requested
 * one without re-integrating. That path was removed in favor of a strict
 * exact-match check because the canonical forms weren't round-tripping
 * reliably. Re-add once the numerator/mass/substitution canonicalization
 * has a single source of truth shared between UI and kernel.
 */
function lookupLibraryResult(lockResult, payload) {
  if (!library || !library.topologies) return null;
  const rawNickel = lockResult.topology?.nickelIndex;
  if (!rawNickel) return null;

  // The kernel's handleLock returns a CNickelIndex of the form
  // "<bare>:<color>" (e.g. "e11|e|:000|0|"). library.topologies is keyed
  // by the bare part only, so strip the ':<color>' suffix before lookup.
  const nickel = rawNickel.includes(':') ? rawNickel.split(':', 1)[0] : rawNickel;

  // Find topology in library (normalize trailing pipes for matching)
  const normNickel = nickel.replace(/\|+$/, '|');
  let topoKey = null;
  let topo = library.topologies[nickel];
  if (topo) {
    topoKey = nickel;
  } else {
    for (const key in library.topologies) {
      if (key.replace(/\|+$/, '|') === normNickel) {
        topo = library.topologies[key];
        topoKey = key;
        break;
      }
    }
  }
  if (!topo || !topo.configs) return null;

  const dim = payload.dimension || '4 - 2*eps';
  const epsOrder = parseInt(payload.epsOrder ?? 0);
  const pNumCanon = _canonicalizeNumerators(payload.numeratorRows || []);

  // Payload and kernel use different schemas for masses and propExponents:
  //   - payload.internalMasses is ONE PER EDGE including external legs,
  //     with '0' as a placeholder for massless entries.
  //   - payload.externalMasses comes from the config panel inputs with '0'
  //     entries for massless external legs.
  //   - payload.propExponents is one per edge (internal + external).
  // The kernel's uiResult (what STSaveResult persists) uses:
  //   - internalMasses / externalMasses: Select nonzero, sparse list
  //   - propExponents: internal-edges only
  // To compare, we collapse both sides to the same normal form: masses
  // become sorted nonzero multisets, propExponents is filtered to the
  // internal-edge mask. Sorted multiset is intentional — positional mass
  // identity is a soft-transform concern (see TODO in this function).
  const intMask = _internalEdgeMask(payload.edgePairs || []);
  const pIntMassesKey = _canonicalizeMassList(
    (payload.internalMasses || []).filter((_, i) => intMask[i]));
  const pExtMassesKey = _canonicalizeMassList(payload.externalMasses || []);
  const pInternalPropExps = (payload.propExponents || []).filter((_, i) => intMask[i]);
  const pSubs = (payload.substitutions || '{}').trim();
  const pNorm = payload.normalization || 'Automatic';

  // ── Hard check: exact mass config match ──
  // Use the configMatches computed by doLiveMatch
  const matchInfo = currentMatches.find(m => m.topoKey === topoKey);
  const configMatchMap = matchInfo ? matchInfo.configMatches : null;
  if (!configMatchMap) return null;  // can't verify mass compatibility

  for (const ck in topo.configs) {
    // Must be an exact mass config match
    if (configMatchMap[ck] !== 'exact') continue;

    const cfg = topo.configs[ck];
    const results = cfg.results || cfg.Results || [];
    for (const r of results) {
      // ── Hard check: dimension ──
      const rDim = (r.dimension || '').replace(/\s/g, '');
      const pDim = dim.replace(/\s/g, '');
      if (rDim !== pDim) continue;

      // ── Hard check: epsilon order (cached >= requested) ──
      const rEps = parseInt(r.epsOrder ?? -999);
      if (rEps < epsOrder) continue;

      if (!r.resultCompressed && !r.resultInputForm) continue;

      // ── Hard check: propagator exponents (internal-edge portion only) ──
      const rExps = r.propExponents || [];
      const rAllDefault = rExps.length === 0 || rExps.every(e => e === 1);
      const pAllDefault = pInternalPropExps.length === 0 || pInternalPropExps.every(e => e === 1);
      if (rAllDefault !== pAllDefault) continue;
      if (!rAllDefault && JSON.stringify(rExps) !== JSON.stringify(pInternalPropExps)) continue;

      // ── Hard check: numerator rows ──
      if (_canonicalizeNumerators(r.numerators || []) !== pNumCanon) continue;

      // ── Hard check: internal and external mass names ──
      // Sorted-nonzero multiset comparison: see the big comment at the top
      // of this function for why the two sides don't compare 1:1 raw.
      if (_canonicalizeMassList(r.internalMasses || []) !== pIntMassesKey) continue;
      if (_canonicalizeMassList(r.externalMasses || []) !== pExtMassesKey) continue;

      // ── Hard check: substitutions (whitespace-normalized equality) ──
      const rSubs = (r.substitutions || '{}').trim();
      if (rSubs !== pSubs) continue;

      // ── Hard check: normalization ──
      const rNorm = r.normalization || 'Automatic';
      if (rNorm !== pNorm) continue;

      // Build the cache hit (resultCompressed is primary; resultInputForm is legacy fallback)
      return {
        status: 'ok',
        result: r.resultInputForm || '',
        resultCompressed: r.resultCompressed || '',
        resultTeX: r.resultTeX || '',
        symbolTeX: r.symbolTeX || '',
        alphabet: r.alphabet || [],
        symbolWeight: r.symbolWeight || 0,
        symbolTerms: r.symbolTerms || 0,
        symbolScale: r.symbolScale || '',
        wDefinitions: r.wDefinitions || [],
        normalizedAlphabet: r.normalizedAlphabet || [],
        normalizedSymbolTeX: r.normalizedSymbolTeX || '',
        normalizedSymbolTerms: r.normalizedSymbolTerms || 0,
        legOrder: r.legOrder || [],
        byteCount: r.byteCount || 0,
        _cached: true,
        _cacheSource: ck,
        verified: r.verified || false,
        verifiedAt: r.verifiedAt || '',
        verifiedMethod: r.method || '',
        verifiedMaxRelErr: r.maxRelErr,
      };
    }
  }

  // ── Soft-transform pass ──
  // If exact-match didn't hit, try finding a cached result where only the
  // mass names, substitutions, or normalization differ — everything else
  // (topology, mass structure, dimension, eps order, prop exponents) must
  // match exactly. Numerators must be empty on BOTH sides (transform can't
  // rewrite numerator-bearing results reliably).
  //
  // When found, we return the cached result with a _transforms array that
  // doIntegrate sends to the kernel's /api/transformResult endpoint, which
  // applies the renames/substitutions/rescaling on the Mma side.
  //
  // Mass-rename map construction: we pair the sorted nonzero mass name
  // lists from each side. Since the color nickel is an exact structural
  // match, both sides have the same count of each mass scale — the sort
  // makes unique names line up by alphabetical order, which is reliable
  // for single-scale and mostly-reliable for multi-scale (where all labels
  // within a scale are identical). Positional alignment via color-nickel
  // digit traversal would be exact but requires re-canonicalizing the
  // canvas masses; left as a TODO for a future refinement.

  if (pNumCanon !== '[]') return null;  // payload has numerators → no soft path
  if (backendMode !== 'full') return null;  // need kernel for transforms

  for (const ck in topo.configs) {
    if (configMatchMap[ck] !== 'exact') continue;
    const cfg = topo.configs[ck];
    const results = cfg.results || cfg.Results || [];
    for (const r of results) {
      // Stored result must also have no numerators
      if ((r.numerators || []).length > 0) continue;

      // Dimension match
      const rDim = (r.dimension || '').replace(/\s/g, '');
      const pDim = dim.replace(/\s/g, '');
      if (rDim !== pDim) continue;

      // Epsilon order: cached >= requested
      if (parseInt(r.epsOrder ?? -999) < epsOrder) continue;

      if (!r.resultCompressed && !r.resultInputForm) continue;

      // Prop exponents match
      const rExps = r.propExponents || [];
      const rAllDefault = rExps.length === 0 || rExps.every(e => e === 1);
      const pAllDef = pInternalPropExps.length === 0 || pInternalPropExps.every(e => e === 1);
      if (rAllDefault !== pAllDef) continue;
      if (!rAllDefault && JSON.stringify(rExps) !== JSON.stringify(pInternalPropExps)) continue;

      // ── Build transforms ──
      const transforms = [];
      const transformDescriptions = [];

      // Internal mass renames
      const rIntSorted = [...(r.internalMasses || [])].filter(m => m && m !== '0').sort();
      const pIntFiltered = (payload.internalMasses || []).filter((_, i) => intMask[i]);
      const pIntSorted = [...pIntFiltered].filter(m => m && m !== '0').sort();
      if (rIntSorted.length === pIntSorted.length && rIntSorted.length > 0) {
        const renames = {};
        const seen = new Set();
        for (let k = 0; k < rIntSorted.length; k++) {
          if (rIntSorted[k] !== pIntSorted[k] && !seen.has(rIntSorted[k])) {
            renames[rIntSorted[k]] = pIntSorted[k];
            seen.add(rIntSorted[k]);
          }
        }
        if (Object.keys(renames).length > 0) {
          transforms.push({ type: 'internalMasses', renames });
          transformDescriptions.push(
            Object.entries(renames).map(([from, to]) => `${from} \u2192 ${to}`).join(', '));
        }
      } else if (rIntSorted.length !== pIntSorted.length) {
        continue;  // structural mismatch — shouldn't happen if config is 'exact'
      }

      // External mass renames
      const rExtSorted = [...(r.externalMasses || [])].filter(m => m && m !== '0').sort();
      const pExtSorted = [...(payload.externalMasses || [])].filter(m => m && m !== '0').sort();
      if (rExtSorted.length === pExtSorted.length) {
        if (rExtSorted.length > 0) {
          const extRenames = {};
          for (let k = 0; k < rExtSorted.length; k++) {
            if (rExtSorted[k] !== pExtSorted[k]) extRenames[rExtSorted[k]] = pExtSorted[k];
          }
          if (Object.keys(extRenames).length > 0) {
            transforms.push({ type: 'externalMasses', masses: payload.externalMasses, cachedMasses: r.externalMasses || [] });
            transformDescriptions.push(
              Object.entries(extRenames).map(([from, to]) => `${from} \u2192 ${to}`).join(', '));
          }
        }
      } else {
        continue;
      }

      // Substitutions
      const rSubs = (r.substitutions || '{}').trim();
      if (rSubs !== '{}' && rSubs !== '' && rSubs !== pSubs) {
        continue;  // can't un-substitute
      }
      if (pSubs !== '{}' && pSubs !== '' && (rSubs === '{}' || rSubs === '')) {
        transforms.push({ type: 'substitutions', rules: pSubs });
        transformDescriptions.push(pSubs);
      }

      // Normalization
      const rNorm = r.normalization || 'Automatic';
      if (rNorm !== pNorm) {
        transforms.push({ type: 'normalization', from: rNorm, to: pNorm });
        transformDescriptions.push(`normalization: ${rNorm} \u2192 ${pNorm}`);
      }

      if (transforms.length === 0) {
        // Everything actually matches — this should have been caught by the
        // exact-match pass. Return as exact hit.
        return {
          status: 'ok', result: r.resultInputForm || '', resultCompressed: r.resultCompressed || '',
          resultTeX: r.resultTeX || '', symbolTeX: r.symbolTeX || '', alphabet: r.alphabet || [],
          symbolWeight: r.symbolWeight || 0, symbolTerms: r.symbolTerms || 0,
          symbolScale: r.symbolScale || '', wDefinitions: r.wDefinitions || [],
          normalizedAlphabet: r.normalizedAlphabet || [], normalizedSymbolTeX: r.normalizedSymbolTeX || '',
          normalizedSymbolTerms: r.normalizedSymbolTerms || 0, legOrder: r.legOrder || [],
          byteCount: r.byteCount || 0, _cached: true, _cacheSource: ck,
          verified: r.verified || false, verifiedAt: r.verifiedAt || '',
          verifiedMethod: r.method || '', verifiedMaxRelErr: r.maxRelErr,
        };
      }

      return {
        status: 'ok',
        result: r.resultInputForm || '',
        resultCompressed: r.resultCompressed || '',
        resultTeX: r.resultTeX || '',
        symbolTeX: r.symbolTeX || '',
        alphabet: r.alphabet || [],
        symbolWeight: r.symbolWeight || 0,
        symbolTerms: r.symbolTerms || 0,
        symbolScale: r.symbolScale || '',
        wDefinitions: r.wDefinitions || [],
        normalizedAlphabet: r.normalizedAlphabet || [],
        normalizedSymbolTeX: r.normalizedSymbolTeX || '',
        normalizedSymbolTerms: r.normalizedSymbolTerms || 0,
        legOrder: r.legOrder || [],
        byteCount: r.byteCount || 0,
        _cached: true,
        _cacheSource: ck,
        _transforms: transforms,
        _transformDescriptions: transformDescriptions,
        verified: r.verified || false,
        verifiedAt: r.verifiedAt || '',
        verifiedMethod: r.method || '',
        verifiedMaxRelErr: r.maxRelErr,
      };
    }
  }

  return null;
}

// ── doIntegrate (full mode) ──

async function doIntegrate() {
  if (backendMode !== 'full') return;

  // Ensure diagram has a name
  const nameInput = $('ic-name');
  const diagramName = nameInput ? nameInput.value.trim() : '';
  if (!diagramName) {
    showWarningToast('Please name the diagram before integrating', true);
    if (nameInput) { nameInput.focus(); nameInput.style.borderColor = 'var(--red)'; setTimeout(() => nameInput.style.borderColor = '', 2000); }
    // Expand the integral card if collapsed
    if (_integralCardCollapsed) { _integralCardCollapsed = false; _integralCard.classList.remove('collapsed'); localStorage.setItem('subtropica-integral-collapsed', '0'); }
    return;
  }

  const payload = collectIntegrationPayload();
  if (payload.edgePairs.length === 0) {
    showWarningToast('Draw a diagram first');
    return;
  }

  showIntegrationPanel(payload);

  // Prefetch Tier-2 complexity if cold, so the Integral frame's stars
  // and MPL badge reflect the real kernel estimate by the time the
  // integration starts. Skipped when we already have a valid cache —
  // runTier2Check is a pure cache hit in that case. Fire-and-forget:
  // on success the modal's stars re-render automatically via
  // refreshIntegDifficultyStars; on failure we keep Tier-1 stars.
  (async () => {
    try {
      await runTier2Check();
    } catch {}
    refreshIntegDifficultyStars();
  })();

  try {
    // Step 1: Lock topology (send JS chord positions so kernel uses same momentum routing)
    const lockResult = await kernel.post('lock', {
      edgePairs: payload.edgePairs,
      jsChordPositions: payload.jsChordPositions
    });
    if (lockResult.status === 'error') {
      onIntegrationComplete({ status: 'error', error: lockResult.error });
      return;
    }

    // Check library for a cached result before running the kernel.
    // Two tiers: (1) exact match on all fields, (2) soft-transform match
    // where mass names, substitutions, or normalization differ but topology
    // + dimension + eps order + prop exponents + numerators all agree.
    // See lookupLibraryResult() for the full logic.
    const cachedResult = lookupLibraryResult(lockResult, payload);
    if (cachedResult) {
      if (cachedResult._transforms) {
        try {
          const transformed = await kernel.post('transformResult', {
            resultCompressed: cachedResult.resultCompressed || '',
            resultInputForm: cachedResult.result,
            transforms: cachedResult._transforms,
          });
          if (transformed.status === 'ok') {
            cachedResult.result = transformed.result;
            cachedResult.resultTeX = transformed.resultTeX || cachedResult.resultTeX;
            cachedResult.symbolTeX = transformed.symbolTeX || cachedResult.symbolTeX;
            cachedResult.alphabet = transformed.alphabet || cachedResult.alphabet;
            if (transformed.symbolWeight) cachedResult.symbolWeight = transformed.symbolWeight;
            if (transformed.symbolTerms) cachedResult.symbolTerms = transformed.symbolTerms;
            if (transformed.normalizedSymbolTeX) cachedResult.normalizedSymbolTeX = transformed.normalizedSymbolTeX;
            if (transformed.wDefinitions) cachedResult.wDefinitions = transformed.wDefinitions;
            if (transformed.normalizedAlphabet) cachedResult.normalizedAlphabet = transformed.normalizedAlphabet;
            if (transformed.normalizedSymbolTerms) cachedResult.normalizedSymbolTerms = transformed.normalizedSymbolTerms;
          }
        } catch (_) { /* fall through with untransformed result */ }
      }
      _targetProgress = 100;
      onIntegrationComplete(cachedResult);
      return;
    }

    _targetProgress = 5;

    // Step 2: Submit integration
    const intResult = await kernel.post('integrate', payload);
    if (intResult.status === 'error') {
      onIntegrationComplete({ status: 'error', error: intResult.error });
      return;
    }
    _targetProgress = 8;

    // Step 3: Poll for progress, stage, and result
    _integrationPolling = setInterval(async () => {
      try {
        const prog = await kernel.get('progress');
        if (prog.status === 'ok' && prog.log) {
          updateProgressFromLog(prog.log);
        }

        // Structured log tail (sibling of /api/progress, axis 11).
        // ?since=<offset> returns only the bytes past the client's last
        // known offset; the server reports the new total size so the
        // cursor advances. Silent-fail on older kernels that don't
        // implement the endpoint.
        try {
          await pollJsonlTail();
        } catch {}

        // Poll structured stage progress
        await pollStageProgress();

        const res = await kernel.get('result');
        if (res.status === 'ok') {
          clearInterval(_integrationPolling);
          _integrationPolling = null;
          _targetProgress = 100;
          // Final stage fetch for accurate job count
          await pollStageProgress();
          onIntegrationComplete(res);
        } else if (res.status === 'error') {
          clearInterval(_integrationPolling);
          _integrationPolling = null;
          onIntegrationComplete(res);
        }
      } catch (e) {
        // Network error during polling — keep trying
      }
    }, 800);

  } catch (e) {
    onIntegrationComplete({ status: 'error', error: 'Network error: ' + e.message });
  }
}

// Force integration (skip library lookup)
async function doIntegrateForce(payload) {
  showIntegrationPanel(payload);
  try {
    const lockResult = await kernel.post('lock', {
      edgePairs: payload.edgePairs,
      jsChordPositions: payload.jsChordPositions
    });
    if (lockResult.status === 'error') {
      onIntegrationComplete({ status: 'error', error: lockResult.error });
      return;
    }
    _targetProgress = 5;
    const intResult = await kernel.post('integrate', payload);
    if (intResult.status === 'error') {
      onIntegrationComplete({ status: 'error', error: intResult.error });
      return;
    }
    _targetProgress = 8;
    _integrationPolling = setInterval(async () => {
      try {
        const prog = await kernel.get('progress');
        if (prog.status === 'ok' && prog.log) updateProgressFromLog(prog.log);
        try { await pollJsonlTail(); } catch {}
        await pollStageProgress();
        const res = await kernel.get('result');
        if (res.status === 'ok') {
          clearInterval(_integrationPolling);
          _integrationPolling = null;
          _targetProgress = 100;
          await pollStageProgress();
          onIntegrationComplete(res);
        } else if (res.status === 'error') {
          clearInterval(_integrationPolling);
          _integrationPolling = null;
          onIntegrationComplete(res);
        }
      } catch {}
    }, 800);
  } catch (e) {
    onIntegrationComplete({ status: 'error', error: 'Network error: ' + e.message });
  }
}

async function cancelIntegration() {
  if (_integrationPolling) {
    clearInterval(_integrationPolling);
    _integrationPolling = null;
  }
  stopProgressAnimation();
  try { await kernel.post('cancel'); } catch {}
  $('integrate-overlay').classList.remove('visible');
  // Remove wide mode class
  const panel = $('integrate-body')?.closest('.modal-panel');
  if (panel) panel.classList.remove('integ-mode');
}

// ── Linear reducibility (MPL) check ─────────────────────────────────
//
// Renders an inline spinner to the right of the difficulty stars while
// the kernel runs, then swaps in the existing MPL / ×MPL badge via
// renderDifficultyStars. No modal. The verdict is cached in _cachedLR
// (same key as _cachedTier2) so a subsequent Integrate click, Check
// complexity, or repeat MPL click is a pure cache hit.

let _lrRunning = false;

async function checkLinearReducibility() {
  if (backendMode !== 'full') return;
  const stats = getTopologyStats();
  if (stats.nIntEdges === 0) {
    showWarningToast('Draw a diagram first');
    return;
  }

  // Cache key matches _cachedTier2 so the two caches share invalidation.
  const estPayload = buildEstimatePayload();
  if (!estPayload) return;
  const hash = JSON.stringify(estPayload);

  if (_cachedLR && _cachedLR.hash === hash) {
    updateDifficultyStars();  // paint the already-known badge
    return;
  }
  if (_lrRunning) return;     // already running for the same graph

  const payload = collectIntegrationPayload();
  if (payload.edgePairs.length === 0) {
    showWarningToast('Draw a diagram first');
    return;
  }

  _lrRunning = true;
  updateDifficultyStars();    // spinner appears next to the stars

  const finish = (cacheEntry, errMsg) => {
    _lrRunning = false;
    if (cacheEntry) _cachedLR = cacheEntry;
    updateDifficultyStars();
    if (errMsg) showWarningToast('MPL check: ' + errMsg);
  };

  try {
    const lockResult = await kernel.post('lock', {
      edgePairs: payload.edgePairs,
      jsChordPositions: payload.jsChordPositions
    });
    if (lockResult.status === 'error') {
      finish(null, lockResult.error || 'lock failed');
      return;
    }

    const lrPayload = { ...payload, stopAt: 'AfterMinimalLRCheck', suppressCommand: true };
    const intResult = await kernel.post('integrate', lrPayload);
    if (intResult.status === 'error') {
      finish(null, intResult.error || 'submit failed');
      return;
    }

    // Poll for the LR verdict.
    const poll = setInterval(async () => {
      try {
        const res = await kernel.get('result');
        if (res.status === 'ok') {
          clearInterval(poll);
          const rStr = res.result || '';
          const isLR = rStr.includes('AnyLR -> True') || rStr.includes('"AnyLR" -> True');
          let gauges = [];
          const m = rStr.match(/LRGauges\s*->\s*\{([^}]*)\}/);
          if (m) gauges = m[1].replace(/\s/g, '').split(',').filter(Boolean);
          finish({ hash, result: { isLR, lrGauges: gauges } }, null);
        } else if (res.status === 'error') {
          clearInterval(poll);
          finish(null, res.error || 'kernel error');
        }
      } catch {}
    }, 500);
  } catch (e) {
    finish(null, 'connection error: ' + (e.message || e));
  }
}

function closeIntegrationPanel() {
  $('integrate-overlay').classList.remove('visible');
  const panel = $('integrate-body')?.closest('.modal-panel');
  if (panel) panel.classList.remove('integ-mode');
}


// ─── Notebook download ──────────────────────────────────────────────────
//
// Builds a payload in library-entry shape, renders it through render.mjs, and
// hands the user a .wl file that opens as a real Mathematica notebook.
// Three case branches at the renderer level — (i) user-drawn, (ii) library
// entry with no result, (iii) library entry with result — are selected
// automatically from the payload shape.

// Pick the right payload for the notebook renderer based on the current
// canvas + library match state. Returns a library-entry-shaped object.
//   - exact library match → the stored entry (cases ii/iii in the renderer)
//   - otherwise           → synthesized from canvas state (case i)
function buildNotebookPayloadFromCurrentState() {
  if (currentMatches) {
    for (const m of currentMatches) {
      const cm = m.configMatches || {};
      const configs = m.topo.configs || {};
      for (const ck in configs) {
        if (cm[ck] === 'exact') return configs[ck];
      }
    }
  }
  return buildNotebookPayloadFromState();
}

// Synthesize a minimal library-entry payload from the current canvas state.
// Used for case (i): user-drawn graph with no library match.
function buildNotebookPayloadFromState() {
  const deg = getVertexDegrees();
  const distinctMasses = new Set();
  for (const e of state.edges) {
    if (e.mass && e.mass > 0) distinctMasses.add(e.mass);
  }
  const singleMass = distinctMasses.size === 1;

  // Contiguous 1..N renumbering in first-appearance order, matching
  // buildGraphArgJS so the notebook emits the same labels as the Export tab.
  const canon = new Map();
  let nextId = 0;
  const getId = raw => {
    let id = canon.get(raw);
    if (id === undefined) { id = ++nextId; canon.set(raw, id); }
    return id;
  };

  const edges = [];
  const nodes = [];
  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    const extA = (e.a !== e.b) && (deg[e.a] || 0) <= 1;
    const extB = (e.a !== e.b) && (deg[e.b] || 0) <= 1;
    const mLabel = edgeMassToMma(e);
    if (extA || extB) {
      const intV = extA ? e.b : e.a;
      nodes.push(`{${getId(intV)}, ${mLabel}}`);
    } else {
      const ia = getId(e.a);
      const ib = getId(e.b);
      const la = Math.min(ia, ib);
      const lb = Math.max(ia, ib);
      edges.push(`{{${la}, ${lb}}, ${mLabel}}`);
    }
  }

  return {
    edges: `{${edges.join(', ')}}`,
    nodes: `{${nodes.join(', ')}}`,
    NumPropagators: edges.length,
    Records: [],
    Results: [],
  };
}

// Build a filename that survives in filesystems: no slashes/colons/pipes.
function notebookFilename(payload) {
  const stem = payload.CNickelIndex
    ? payload.CNickelIndex.replace(/[^\w]+/g, '_').replace(/_+$/, '')
    : 'SubTropica_starter';
  return `${stem}.nb`;
}

// Parse a .wl file the way Mathematica does when opening it as a notebook:
// (* ::Style:: *) + (* body *) pairs become styled cells; raw code between
// markers becomes Input cells. Returns an array of {style, text} tuples.
function parseNotebookCells(wl) {
  const STYLES = new Set(['Title', 'Chapter', 'Section', 'Subsection',
    'Subsubsection', 'Text', 'Program', 'Code', 'Item']);
  const lines = wl.split('\n');
  const cells = [];
  let i = 0;
  const markerStyle = line => {
    const m = line.match(/^\(\* ::(\w+):: \*\)\s*$/);
    return m && STYLES.has(m[1]) ? m[1] : null;
  };
  const blank = line => /^\s*$/.test(line);
  while (i < lines.length) {
    const style = markerStyle(lines[i]);
    if (style) {
      i++;
      while (i < lines.length && blank(lines[i])) i++;
      if (i >= lines.length) break;
      if (lines[i].startsWith('(*')) {
        const single = lines[i].match(/^\(\*(.*)\*\)\s*$/);
        if (single) { cells.push({ style, text: single[1].trim() }); i++; }
        else {
          let body = lines[i].replace(/^\(\*/, ''); i++;
          while (i < lines.length && !lines[i].includes('*)')) { body += '\n' + lines[i]; i++; }
          if (i < lines.length) { body += '\n' + lines[i].replace(/\*\)\s*$/, ''); i++; }
          cells.push({ style, text: body.trim() });
        }
      } else cells.push({ style, text: '' });
      continue;
    }
    const codeLines = [];
    while (i < lines.length && !markerStyle(lines[i])) { codeLines.push(lines[i]); i++; }
    while (codeLines.length && blank(codeLines[codeLines.length - 1])) codeLines.pop();
    while (codeLines.length && blank(codeLines[0])) codeLines.shift();
    const code = codeLines.join('\n');
    if (!code.trim()) continue;
    if (/^\(\* ::Package:: \*\)\s*$/.test(code)) continue;
    cells.push({ style: 'Input', text: code });
  }
  return cells;
}

// Render parsed cells into an HTML container so the preview reads like a
// rendered notebook rather than a wall of source.
function renderNotebookPreview(container, wl) {
  container.textContent = '';
  container.style.cssText = 'margin:8px 0 0 0;padding:14px 16px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-sm);max-height:380px;overflow:auto;white-space:normal;line-height:1.45';
  const cells = parseNotebookCells(wl);
  const styleMap = {
    Title:       { tag: 'h3', css: 'margin:0 0 8px 0;font-family:var(--font-display);font-size:17px;font-weight:700;color:var(--accent)' },
    Section:     { tag: 'h4', css: 'margin:14px 0 4px 0;font-family:var(--font-display);font-size:14px;font-weight:700;color:var(--accent)' },
    Subsection:  { tag: 'h5', css: 'margin:10px 0 2px 0;font-family:var(--font-display);font-size:12px;font-weight:600;color:var(--text)' },
    Subsubsection: { tag: 'h6', css: 'margin:8px 0 2px 0;font-family:var(--font-display);font-size:11px;font-weight:600;color:var(--text-mid)' },
    Text:        { tag: 'p',  css: 'margin:4px 0;font-size:12px;color:var(--text-mid)' },
    Program:     { tag: 'pre', css: 'margin:6px 0;padding:6px 8px;background:var(--surface-alt);border:1px solid var(--border-light);border-radius:3px;font-family:var(--font-mono);font-size:10px;line-height:1.4;white-space:pre-wrap;color:var(--text-muted);overflow-x:auto' },
    Code:        { tag: 'pre', css: 'margin:6px 0;padding:6px 8px;background:var(--surface-alt);border:1px solid var(--border-light);border-radius:3px;font-family:var(--font-mono);font-size:11px;line-height:1.4;white-space:pre-wrap;color:var(--text);overflow-x:auto' },
    Item:        { tag: 'li', css: 'margin:2px 0;font-size:12px;color:var(--text-mid)' },
    Input:       { tag: 'pre', css: 'margin:6px 0;padding:8px 10px;background:var(--bg-warm);border-left:3px solid var(--accent);border-radius:3px;font-family:var(--font-mono);font-size:11px;line-height:1.45;white-space:pre-wrap;color:var(--text);overflow-x:auto' },
  };
  for (const cell of cells) {
    const spec = styleMap[cell.style] || styleMap.Text;
    const el = document.createElement(spec.tag);
    el.style.cssText = spec.css;
    // Truncate very long Input cells (e.g. the base64 PNG payload) so the
    // preview stays readable. Users can still download the real file.
    let text = cell.text;
    if (cell.style === 'Input' && text.length > 600) {
      text = text.slice(0, 600) + '\n… (truncated in preview — full content in downloaded file)';
    }
    el.textContent = text;
    container.appendChild(el);
  }
}

async function downloadStarterNotebook(entry, recordIdx = 0) {
  try {
    const template = await getStarterTemplate();
    const wl = await renderNotebook(template, entry, recordIdx, { offline: false, stVersion: ST_VERSION });
    // Ship .nb rather than .wl: Mathematica opens .wl files in Package
    // Editor (code view, no inline cell evaluation), but .nb files open as
    // real notebooks where Shift-Enter works on each cell and FeynmanPlot
    // renders the diagram inline.
    const nb = wlToNb(wl);
    const blob = new Blob([nb], { type: 'application/vnd.wolfram.mathematica' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = notebookFilename(entry);
    a.click();
    URL.revokeObjectURL(url);
    showWarningToast('Notebook downloaded');
  } catch (err) {
    console.error('downloadStarterNotebook failed', err);
    showWarningToast('Could not generate notebook');
  }
}

// Preserve the old name as a thin shim — call sites upgraded to pass the
// full entry, but the per-result button in the modal still passes a stored
// result record (which lives inside an entry's Results[]). When that's the
// case, walk currentMatches to recover the owning entry.
function downloadResultNotebook(resultRecord) {
  // Find the entry whose Results[] contains this record.
  const matches = currentMatches || [];
  for (const m of matches) {
    const cm = m.configMatches || {};
    const configs = m.topo.configs || {};
    for (const ck in configs) {
      if (cm[ck] !== 'exact') continue;
      const results = configs[ck].Results || configs[ck].results || [];
      const idx = results.findIndex(r => r === resultRecord ||
        (r.recordId && resultRecord.recordId && r.recordId === resultRecord.recordId));
      if (idx >= 0) return downloadStarterNotebook(configs[ck], idx);
    }
  }
  // Fallback: synthesize a minimal entry wrapping just this result.
  const fallback = { CNickelIndex: 'unknown', edges: '{}', nodes: '{}',
    NumPropagators: (resultRecord.propExponents || []).length,
    Records: [], Results: [resultRecord] };
  return downloadStarterNotebook(fallback, 0);
}

async function returnToMathematica() {
  const body = $('integrate-body');
  const saveToLib = $('integ-save-library')?.checked ?? true;

  // Write save-to-library flag for the kernel
  if (saveToLib) {
    try { await kernel.post('save_diagram', 'SUBMIT_TO_LIBRARY'); } catch {}
    // The actual flag: write a sentinel file
    try {
      await fetch('/api/log_error', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: 'SUBMIT_LIBRARY=1'
      });
    } catch {}
  }

  // For cached results, write result.json so the kernel can pick it up
  if (_lastIntegrationData && _lastIntegrationData._cached) {
    try {
      await fetch('/api/log_error', {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: 'CACHED_RESULT=' + JSON.stringify({
          result: _lastIntegrationData.result,
          resultCompressed: _lastIntegrationData.resultCompressed || '',
          resultTeX: _lastIntegrationData.resultTeX,
        })
      });
    } catch {}
  }

  // Show returning message
  if (body) {
    body.innerHTML = '<div style="padding:48px 24px;text-align:center">' +
      `<div style="font-size:36px;margin-bottom:10px">${mascotEmoji()}</div>` +
      '<div style="font-size:16px;font-weight:600;color:var(--text)">Returning to Mathematica\u2026</div>' +
      '</div>';
  }

  // Signal DONE to the kernel
  try {
    await fetch('/api/done', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: '{}'
    });
    doneSent = true;
  } catch {}

  // Safety timeout in case DONE fetch failed
  if (!doneSent) {
    setTimeout(() => {
      fetch('/api/done', { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}' }).catch(() => {});
    }, 2000);
  }

  // Update UI to confirm
  if (body) {
    body.innerHTML = '<div style="padding:48px 24px;text-align:center">' +
      `<div style="font-size:36px;margin-bottom:10px">${mascotEmoji()}</div>` +
      '<div style="font-size:16px;font-weight:600;color:var(--text)">Returning to Mathematica\u2026</div>' +
      '<div style="font-size:12px;color:var(--text-muted);margin-top:6px">The result is available as the return value of STIntegrate[]</div>' +
      '</div>';
  }
}

/**
 * Capture the integration summary card as a PNG: diagram + params + result.
 * Draws directly on canvas (no foreignObject, no CORS issues).
 * Returns a base64 PNG string (no data: prefix), or null on failure.
 */
function captureSummaryCardPNG() {
  return new Promise(resolve => {
    try {
      const card = $('integ-summary-card');
      if (!card && !_lastIntegrationData) { resolve(null); return; }

      const W = 800, H = 500, S = 2;  // 2x for retina
      const cvs = document.createElement('canvas');
      cvs.width = W * S; cvs.height = H * S;
      const ctx = cvs.getContext('2d');
      ctx.scale(S, S);

      // Colors from CSS vars
      const getCS = (prop, fb) => getComputedStyle(document.body).getPropertyValue(prop).trim() || fb;
      const bg = getCS('--bg', '#1a1a2e');
      const text = getCS('--text', '#e0e0e0');
      const textMid = getCS('--text-mid', '#a0a0b0');
      const accent = getCS('--accent', '#7b68ee');

      // Background
      ctx.fillStyle = bg;
      ctx.fillRect(0, 0, W, H);

      // ── Right side: params + result ──
      const rx = 280, rw = W - rx - 24;
      let y = 28;

      // Collect params from DOM
      const paramEls = card ? card.querySelectorAll(
        '.integ-params .integ-param-label, .integ-params .integ-param-value, .integ-params .integ-timing'
      ) : [];
      paramEls.forEach(el => {
        if (el.classList.contains('integ-param-label')) {
          ctx.font = '600 10px -apple-system, system-ui, sans-serif';
          ctx.fillStyle = textMid;
          y += 14;
          ctx.fillText(el.textContent, rx, y);
        } else if (el.classList.contains('integ-timing')) {
          ctx.font = '11px -apple-system, system-ui, sans-serif';
          ctx.fillStyle = accent;
          y += 14;
          ctx.fillText(el.textContent, rx, y);
        } else {
          ctx.font = '13px -apple-system, system-ui, sans-serif';
          ctx.fillStyle = text;
          // Wrap long values
          const words = el.textContent.split(/\s+/);
          let line = '';
          for (const w of words) {
            const test = line ? line + ' ' + w : w;
            if (ctx.measureText(test).width > rw && line) {
              y += 16;
              ctx.fillText(line, rx, y);
              line = w;
            } else { line = test; }
          }
          if (line) { y += 16; ctx.fillText(line, rx, y); }
        }
      });

      // Result
      if (_lastIntegrationData && _lastIntegrationData.result) {
        y += 20;
        ctx.strokeStyle = textMid + '44';
        ctx.beginPath(); ctx.moveTo(rx, y); ctx.lineTo(rx + rw, y); ctx.stroke();
        y += 16;
        ctx.font = '600 10px -apple-system, system-ui, sans-serif';
        ctx.fillStyle = textMid;
        ctx.fillText('RESULT', rx, y);
        y += 14;
        ctx.font = '11px monospace';
        ctx.fillStyle = text;
        const resultStr = String(_lastIntegrationData.result);
        const lines = resultStr.slice(0, 1200).split(/\n/);
        for (const rawLine of lines) {
          // Wrap long lines
          let remaining = rawLine;
          while (remaining.length > 0) {
            let end = remaining.length;
            while (ctx.measureText(remaining.slice(0, end)).width > rw && end > 10) end--;
            y += 14;
            if (y > H - 16) { ctx.fillText('\u2026', rx, y); remaining = ''; break; }
            ctx.fillText(remaining.slice(0, end), rx, y);
            remaining = remaining.slice(end);
          }
        }
      }

      // ── Left side: diagram ──
      // Rasterize diagram SVG into the left area
      const drawDiagram = () => {
        const svgStr = generateExportSVG();
        if (!svgStr) { finalize(); return; }
        const img = new Image();
        const blob = new Blob([svgStr], { type: 'image/svg+xml;charset=utf-8' });
        const url = URL.createObjectURL(blob);
        img.onload = () => {
          const maxW = 240, maxH = 220, padTop = 28;
          const scale = Math.min(maxW / img.naturalWidth, maxH / img.naturalHeight) * 0.85;
          const dw = img.naturalWidth * scale, dh = img.naturalHeight * scale;
          const dx = (260 - dw) / 2, dy = padTop + (maxH - dh) / 2;
          ctx.drawImage(img, dx, dy, dw, dh);
          URL.revokeObjectURL(url);
          finalize();
        };
        img.onerror = () => { URL.revokeObjectURL(url); finalize(); };
        img.src = url;
      };

      const finalize = () => {
        try {
          const dataUrl = cvs.toDataURL('image/png');
          resolve(dataUrl.replace(/^data:image\/png;base64,/, ''));
        } catch { resolve(null); }
      };

      drawDiagram();
    } catch { resolve(null); }
  });
}

function rasterizeSVGtoPNG(svgStr, width, height) {
  return new Promise((resolve) => {
    const img = new Image();
    const blob = new Blob([svgStr], { type: 'image/svg+xml;charset=utf-8' });
    const url = URL.createObjectURL(blob);
    img.onload = () => {
      const canvas = document.createElement('canvas');
      canvas.width = width;
      canvas.height = height;
      const ctx = canvas.getContext('2d');
      // Transparent background
      ctx.clearRect(0, 0, width, height);
      // Scale to fit
      const scale = Math.min(width / img.naturalWidth, height / img.naturalHeight) * 0.9;
      const dx = (width - img.naturalWidth * scale) / 2;
      const dy = (height - img.naturalHeight * scale) / 2;
      ctx.drawImage(img, dx, dy, img.naturalWidth * scale, img.naturalHeight * scale);
      URL.revokeObjectURL(url);
      try {
        const dataUrl = canvas.toDataURL('image/png');
        // Strip data:image/png;base64, prefix
        resolve(dataUrl.replace(/^data:image\/png;base64,/, ''));
      } catch { resolve(null); }
    };
    img.onerror = () => { URL.revokeObjectURL(url); resolve(null); };
    img.src = url;
  });
}

function addBackButton(footer) {
  const backBtn = document.createElement('button');
  backBtn.className = 'btn btn-sm btn-secondary';
  backBtn.textContent = '\u2190 Back to config';
  backBtn.addEventListener('click', () => {
    populateConfigPanel();
    populateConfigFooter();
  });
  footer.innerHTML = '';
  footer.appendChild(backBtn);
}

// Config panel event wiring
if ($('config-btn')) $('config-btn').addEventListener('click', openConfigPanel);
$('config-tab-pull').addEventListener('click', () => {
  if ($('config-panel').classList.contains('open')) closeConfigPanel();
  else openConfigPanel();
});
if ($('config-close')) $('config-close').addEventListener('click', closeConfigPanel);
$('config-backdrop').addEventListener('click', closeConfigPanel);

// Mobile bottom-sheet: close (X) button + swipe-down handle gesture
if ($('config-sheet-close')) {
  $('config-sheet-close').addEventListener('click', closeConfigPanel);
}
{
  const handle = $('config-sheet-handle');
  const panel = $('config-panel');
  if (handle && panel) {
    let startY = 0, dragging = false, panelH = 0;
    const onMove = (e) => {
      if (!dragging) return;
      const dy = Math.max(0, e.clientY - startY);  // only down-drag is meaningful
      panel.style.transform = `translateY(${dy}px)`;
    };
    const onUp = (e) => {
      if (!dragging) return;
      dragging = false;
      handle.classList.remove('dragging');
      panel.classList.remove('dragging');
      try { handle.releasePointerCapture(e.pointerId); } catch (err) {}
      document.removeEventListener('pointermove', onMove);
      document.removeEventListener('pointerup', onUp);
      document.removeEventListener('pointercancel', onUp);
      const dy = Math.max(0, e.clientY - startY);
      panel.style.transform = '';  // restore CSS-driven transform
      // Drag down >25% of panel height → close
      if (dy > panelH * 0.25) closeConfigPanel();
    };
    handle.addEventListener('pointerdown', (e) => {
      if (e.button !== 0) return;
      dragging = true;
      startY = e.clientY;
      panelH = panel.getBoundingClientRect().height || 1;
      handle.classList.add('dragging');
      panel.classList.add('dragging');  // suppress transform transition while dragging
      try { handle.setPointerCapture(e.pointerId); } catch (err) {}
      document.addEventListener('pointermove', onMove);
      document.addEventListener('pointerup', onUp);
      document.addEventListener('pointercancel', onUp);
      e.preventDefault();
    });
  }
}

// Config panel drag-to-resize
{
  const handle = $('config-resize');
  if (handle) {
    let startX = 0, startW = 0;
    const onMove = (e) => {
      const w = Math.max(280, Math.min(window.innerWidth - 200, startW + (e.clientX - startX)));
      document.documentElement.style.setProperty('--config-panel-w', w + 'px');
    };
    const onUp = () => {
      handle.classList.remove('dragging');
      $('config-panel').classList.remove('resizing');
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      const w = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--config-panel-w'), 10);
      if (w >= 280) localStorage.setItem('config-panel-width', String(w));
      // Re-layout PDF pages if Paper tab is active (viewport width changed)
      if (_pdfDoc && document.getElementById('cfg-paper')?.classList.contains('active')) {
        _pdfRelayoutAll();
      }
    };
    handle.addEventListener('mousedown', (e) => {
      e.preventDefault();
      handle.classList.add('dragging');
      $('config-panel').classList.add('resizing');
      startX = e.clientX;
      startW = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--config-panel-w'), 10) || 420;
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup', onUp);
    });
  }
  // Restore saved width
  const savedW = parseInt(localStorage.getItem('config-panel-width') || '0', 10);
  if (savedW >= 280) document.documentElement.style.setProperty('--config-panel-w', savedW + 'px');
}
$('ic-check-mpl').addEventListener('click', checkLinearReducibility);
$('ic-check-complexity').addEventListener('click', runTier2Check);
if ($('ic-save-exit')) $('ic-save-exit').addEventListener('click', saveAndExit);

// Sync the current diagram's edges/nodes/propagators/quadruple into the
// kernel's $ST* globals, then ask the server to shut down silently so
// FeynmanIntegrate[] returns Null (no Out[] cell in the notebook).
async function saveAndExit() {
  if (backendMode !== 'full') return;
  const btn = $('ic-save-exit');
  if (btn) { btn.disabled = true; btn.textContent = 'Saving…'; }
  try {
    // handleEstimate writes $STEdges / $STNodes / $STGraph / $STPropagators
    // / $STQuadruple from the request payload. skipExpansion:true skips the
    // expensive tropical expansion — we only need the globals updated.
    const payload = buildEstimatePayload();
    if (payload) {
      try {
        await kernel.post('estimate', { ...payload, skipExpansion: true });
      } catch {
        // If the estimate call fails, handleSafeExit will still normalize
        // whatever globals are set; better to exit than leave the UI open.
      }
    }
    try {
      await kernel.post('safeExit', { silent: true });
    } catch {
      // Server may die mid-response — that's the expected success path.
    }
  } finally {
    if (btn) { btn.disabled = false; btn.textContent = 'Save and exit'; }
  }
}
document.querySelectorAll('.config-tab').forEach(tab => {
  tab.addEventListener('click', () => switchConfigTab(tab));
});
$('cfg-auto-norm').addEventListener('change', function() {
  $('cfg-normalization').disabled = this.checked;
  computeConfig.normalization = this.checked ? 'Automatic' : $('cfg-normalization').value;
  updateIntegralCard();
});
$('cfg-normalization').addEventListener('change', function() {
  if (!$('cfg-auto-norm').checked) {
    computeConfig.normalization = this.value;
    updateIntegralCard();
  }
});
$('cfg-auto-gauge').addEventListener('change', function() {
  $('cfg-gauge').disabled = this.checked;
});
$('cfg-add-numerator').addEventListener('click', addNumeratorRow);
$('cfg-remove-numerator').addEventListener('click', removeNumeratorRow);

// Delegated listeners for Advanced-tab controls. Without these, none of the
// selects / inputs / checkboxes inside #cfg-advanced ever flow their values
// back into computeConfig, so toggling them leaves the Export tab (and the
// printed STIntegrate command) showing stale defaults. Using delegation
// covers every control uniformly and keeps new options zero-config.
(function wireAdvancedTabListeners() {
  const adv = $('cfg-advanced');
  if (!adv) return;
  const handler = () => { syncConfigToState(); updateIntegralCard(); };
  // Use both 'input' (text fields) and 'change' (selects, checkboxes).
  adv.addEventListener('input', handler);
  adv.addEventListener('change', handler);
})();


// ─── Integrate modal ────────────────────────────────────────────────

function openIntegrateModal() {
  const overlay = $('integrate-overlay');
  const body = $('integrate-body');
  body.innerHTML = '';

  // ── Input section ──
  const inputFrame = document.createElement('div');
  inputFrame.className = 'modal-frame';
  inputFrame.style.marginBottom = '14px';

  const integral = buildFeynmanIntegralLaTeX();

  // Frame header: "Input" label + tiny copy buttons
  const inputHeader = document.createElement('div');
  inputHeader.className = 'modal-frame-header';
  const inputLabel = document.createElement('span');
  inputLabel.className = 'modal-section-title';
  inputLabel.style.margin = '0';
  inputLabel.textContent = 'Input';
  inputHeader.appendChild(inputLabel);

  if (integral) {
    const btnRow = document.createElement('div');
    btnRow.className = 'modal-btn-row';

    const copyTeX = document.createElement('button');
    copyTeX.className = 'modal-btn-tiny';
    copyTeX.textContent = 'Copy LaTeX';
    copyTeX.addEventListener('click', () => {
      navigator.clipboard.writeText(integral.latex).then(() => showWarningToast('LaTeX copied'));
    });
    btnRow.appendChild(copyTeX);

    const mmaCode = buildFeynmanIntegralMathematica();
    if (mmaCode) {
      const copyMma = document.createElement('button');
      copyMma.className = 'modal-btn-tiny';
      copyMma.textContent = 'Copy Mathematica';
      copyMma.addEventListener('click', () => {
        navigator.clipboard.writeText(mmaCode).then(() => showWarningToast('Mathematica copied'));
      });
      btnRow.appendChild(copyMma);
    }
    inputHeader.appendChild(btnRow);
  }
  inputFrame.appendChild(inputHeader);

  if (integral) {
    const integralDisplay = document.createElement('div');
    integralDisplay.className = 'modal-integral';
    if (typeof katex !== 'undefined') {
      katex.render('\\displaystyle ' + integral.latex, integralDisplay, { throwOnError: false, displayMode: true });
    } else {
      integralDisplay.textContent = integral.latex;
    }
    inputFrame.appendChild(integralDisplay);
  } else {
    const noData = document.createElement('div');
    noData.className = 'modal-placeholder';
    noData.textContent = 'Draw a diagram with at least one loop to see the integral.';
    inputFrame.appendChild(noData);
  }

  body.appendChild(inputFrame);

  // ── Result section: show precomputed results if available ──
  const resultFrame = document.createElement('div');
  resultFrame.className = 'modal-frame';

  const resultHeader = document.createElement('div');
  resultHeader.className = 'modal-frame-header';
  const resultLabel = document.createElement('span');
  resultLabel.className = 'modal-section-title';
  resultLabel.style.margin = '0';
  resultLabel.textContent = 'Result';
  resultHeader.appendChild(resultLabel);
  resultFrame.appendChild(resultHeader);

  // Collect results only from exact-match configs
  const storedResults = [];
  if (currentMatches) {
    for (const m of currentMatches) {
      const cm = m.configMatches || {};
      const configs = m.topo.configs || {};
      for (const ck in configs) {
        if (cm[ck] !== 'exact') continue;
        const results = configs[ck].results || configs[ck].Results || [];
        results.forEach(r => {
          if (r.resultCompressed || r.resultTeX) storedResults.push(r);
        });
      }
    }
  }

  if (storedResults.length > 0) {
    // Show precomputed results
    storedResults.forEach(r => {
      const card = document.createElement('div');
      card.style.cssText = 'border-left:3px solid var(--gold);padding:8px 12px;margin-bottom:10px;background:var(--bg-warm);border-radius:var(--radius)';

      // Parameters line
      const params = [];
      if (r.dimension) params.push({ text: 'D = ' + r.dimension,
        tip: '<strong>Spacetime dimension</strong> used for this computation. <code>4 - 2*eps</code> is standard dim-reg.' });
      if (r.epsOrder !== undefined && r.epsOrder !== null) params.push({ text: '\u03B5 \u2264 ' + r.epsOrder,
        tip: '<strong>Highest &epsilon; order</strong> retained in the expansion. Lower-order coefficients are also included.' });
      if (r.contributor && r.contributor !== 'Anonymous') params.push({ text: r.contributor, tip: 'Contributor who computed this entry.' });
      if (r.stVersion) params.push({ text: r.stVersion,
        tip: '<strong>SubTropica version</strong> that produced this result.' });
      if (r.methodLR) params.push({ text: r.methodLR,
        tip: '<strong>Linear-reducibility algorithm</strong> used. <code>Espresso</code> = fast heuristic; <code>Lungo</code> = thorough discriminant/resultant.' });
      if (params.length > 0) {
        const paramLine = document.createElement('div');
        paramLine.style.cssText = 'font-size:11px;color:var(--text-muted);margin-bottom:6px;display:flex;flex-wrap:wrap;gap:4px;align-items:baseline';
        params.forEach((p, i) => {
          if (i > 0) {
            const sep = document.createElement('span');
            sep.textContent = '\u00B7';
            sep.style.opacity = '0.6';
            paramLine.appendChild(sep);
          }
          const span = document.createElement('span');
          span.textContent = p.text;
          if (p.tip) { span.dataset.tipHtml = p.tip; span.style.cursor = 'help'; }
          paramLine.appendChild(span);
        });
        card.appendChild(paramLine);
      }

      // TeX preview
      const texDiv = document.createElement('div');
      texDiv.style.cssText = 'overflow-x:auto;padding:4px 0;min-height:20px';
      if (r.resultTeX && typeof katex !== 'undefined') {
        try {
          katex.render('\\displaystyle ' + cleanTeX(r.resultTeX), texDiv, { throwOnError: false, displayMode: true, maxSize: 500 });
        } catch { texDiv.textContent = r.resultTeX; }
      } else if (r.resultCompressed) {
        texDiv.innerHTML = '<span style="color:var(--text-muted);font-style:italic">Result stored as binary</span>';
      }
      card.appendChild(texDiv);

      // Alphabet (always visible as inline pills — prefer W definitions)
      if (typeof katex !== 'undefined') {
        const wDefs = r.wDefinitions;
        const alph = r.alphabet;
        if ((wDefs && wDefs.length > 0) || (alph && alph.length > 0)) {
          const alphRow = document.createElement('div');
          alphRow.style.cssText = 'margin-top:8px;display:flex;align-items:baseline;gap:6px;flex-wrap:wrap';
          const alphLabel = document.createElement('span');
          alphLabel.style.cssText = 'font-size:11px;color:var(--text-muted);font-weight:500;white-space:nowrap;cursor:help;border-bottom:1px dashed var(--border)';
          alphLabel.textContent = 'Alphabet';
          alphLabel.dataset.tipHtml = '<strong>Symbol alphabet</strong> &mdash; the minimal set of arguments <code>W<sub>i</sub></code> appearing in all logarithms of the polylogarithmic result. Each <code>W<sub>i</sub></code> is a rational (or algebraic) function of the physical scales. Singular behavior happens exactly where some <code>W<sub>i</sub></code> vanishes or diverges.';
          alphRow.appendChild(alphLabel);
          const alphDiv = document.createElement('div');
          alphDiv.style.cssText = 'display:flex;flex-wrap:wrap;gap:3px;line-height:1';
          if (wDefs && wDefs.length > 0) {
            wDefs.forEach(def => {
              const span = document.createElement('span');
              span.style.cssText = 'display:inline-block;padding:1px 6px;background:var(--surface);border-radius:3px;font-size:11px;border:1px solid var(--border)';
              const tex = def.label + ' = ' + def.definition;
              try { katex.render(tex, span, { throwOnError: false }); }
              catch { span.textContent = tex; }
              alphDiv.appendChild(span);
            });
          } else {
            alph.forEach(a => {
              const span = document.createElement('span');
              span.style.cssText = 'display:inline-block;padding:1px 6px;background:var(--surface);border-radius:3px;font-size:11px;border:1px solid var(--border)';
              try { katex.render(a, span, { throwOnError: false }); }
              catch { span.textContent = a; }
              alphDiv.appendChild(span);
            });
          }
          alphRow.appendChild(alphDiv);
          card.appendChild(alphRow);
        }
      }

      // Symbol (collapsible, with weight and term count — prefer normalized)
      {
        const symTex = cleanSymbolTeX(r.normalizedSymbolTeX || r.symbolTeX);
        if (symTex && typeof katex !== 'undefined') {
          const wt = r.symbolWeight || 0;
          const origNt = r.symbolTerms || 0;
          const normNt = r.normalizedSymbolTerms || 0;
          const symMeta = [];
          if (wt > 0) symMeta.push('weight ' + wt);
          if (normNt > 0 && normNt !== origNt) symMeta.push(origNt + '\u2009\u2192\u2009' + normNt + ' terms');
          else if (origNt > 0) symMeta.push(origNt + ' term' + (origNt !== 1 ? 's' : ''));
          const symDetails = document.createElement('details');
          symDetails.style.cssText = 'margin-top:6px';
          const symSummary = document.createElement('summary');
          symSummary.style.cssText = 'cursor:pointer;font-size:12px;color:var(--text-muted);font-weight:500';
          symSummary.textContent = 'Symbol' + (symMeta.length > 0 ? ' (' + symMeta.join(', ') + ')' : '');
          symSummary.dataset.tipHtml = '<strong>Symbol</strong> &mdash; the iterated tensor product of alphabet letters that encodes the differential structure of the polylogarithm. <strong>Weight</strong> is the transcendental weight (number of nested integrations); <strong>terms</strong> counts the distinct tensor products. A drop from original to normalized terms means W<sub>i</sub>-basis relations consolidated the symbol.';
          const symDiv = document.createElement('div');
          symDiv.style.cssText = 'overflow-x:auto;padding:4px 0;margin-top:4px';
          try {
            katex.render('\\displaystyle ' + symTex, symDiv, { throwOnError: false, displayMode: true, maxSize: 500 });
          } catch { symDiv.textContent = symTex; }
          symDetails.appendChild(symDiv);
          card.appendChild(symDetails);
        }
      }

      // Action buttons
      const btns = document.createElement('div');
      btns.style.cssText = 'display:flex;gap:4px;margin-top:6px';
      if (r.resultTeX) {
        const copyBtn = document.createElement('button');
        copyBtn.className = 'modal-btn-tiny';
        copyBtn.textContent = 'Copy LaTeX';
        copyBtn.addEventListener('click', () => {
          navigator.clipboard.writeText(r.resultTeX).then(() => showWarningToast('LaTeX copied'));
        });
        btns.appendChild(copyBtn);
      }
      {
        const symTex = cleanSymbolTeX(r.normalizedSymbolTeX || r.symbolTeX);
        if (symTex) {
          const copySymBtn = document.createElement('button');
          copySymBtn.className = 'modal-btn-tiny';
          copySymBtn.textContent = 'Copy Symbol';
          copySymBtn.addEventListener('click', () => {
            navigator.clipboard.writeText(symTex).then(() => showWarningToast('Symbol LaTeX copied'));
          });
          btns.appendChild(copySymBtn);
        }
      }
      if (backendMode !== 'full') {
        const dlBtn = document.createElement('button');
        dlBtn.className = 'modal-btn-tiny';
        dlBtn.textContent = 'Download notebook';
        dlBtn.title = 'Starter .wl notebook that reproduces this result with SubTropica';
        dlBtn.addEventListener('click', () => downloadResultNotebook(r));
        btns.appendChild(dlBtn);
      }
      card.appendChild(btns);
      resultFrame.appendChild(card);
    });
  } else {
    // No stored results — offer a starter notebook download. The notebook
    // bakes in install steps, graph definition, STIntegrate/STNIntegrate
    // scaffolding, and (when the current diagram matches an entry with no
    // result yet) an STSubmitResult[] hook so the user can contribute back.
    const resultBody = document.createElement('div');
    resultBody.className = 'modal-result-body';
    resultBody.style.cssText = 'display:flex;flex-direction:column;align-items:center;gap:14px;padding:4px 0 2px 0';

    const blurb = document.createElement('p');
    blurb.style.cssText = 'margin:0;text-align:center;max-width:560px;font-size:13px;color:var(--text-mid);line-height:1.55';
    blurb.innerHTML = 'No precomputed result is available for this diagram. ' +
      'Download a Mathematica notebook that installs <strong>SubTropica</strong>, ' +
      'defines this graph, and drops you at the <code>STIntegrate</code> call.';
    resultBody.appendChild(blurb);

    // Centered primary action.
    const dlBtn = document.createElement('button');
    dlBtn.className = 'btn btn-primary';
    dlBtn.style.cssText = 'padding:10px 20px;font-size:13px;font-weight:600;display:inline-flex;align-items:center;gap:8px';
    dlBtn.innerHTML = '<svg class="ico" viewBox="0 0 24 24" style="stroke:#fff;width:16px;height:16px"><path d="M21 15v4a2 2 0 01-2 2H5a2 2 0 01-2-2v-4"/><polyline points="7 10 12 15 17 10"/><line x1="12" y1="15" x2="12" y2="3"/></svg> Download notebook';
    dlBtn.addEventListener('click', () => {
      const payload = buildNotebookPayloadFromCurrentState();
      downloadStarterNotebook(payload, 0);
    });
    resultBody.appendChild(dlBtn);

    const hint = document.createElement('div');
    hint.style.cssText = 'font-size:11px;color:var(--text-muted);margin-top:-6px';
    hint.textContent = '.nb file · opens in Mathematica 13.1+';
    resultBody.appendChild(hint);

    // Preview — expandable summary of what the notebook contains. Generated
    // lazily on first <details> open so we don't block modal open on fetch.
    const preview = document.createElement('details');
    preview.style.cssText = 'margin-top:4px;width:100%';
    const previewSummary = document.createElement('summary');
    previewSummary.style.cssText = 'cursor:pointer;font-size:12px;color:var(--text-muted);font-weight:500;user-select:none;text-align:center;list-style:none';
    previewSummary.innerHTML = '<span style="border-bottom:1px dashed var(--border)">Preview notebook contents</span>';
    previewSummary.dataset.tipHtml = 'Expands a rendered preview of every section in the notebook you\u2019re about to download \u2014 install steps, graph definition, <code>STIntegrate</code>, numerical backends, IBP hooks, references, BibTeX.';
    preview.appendChild(previewSummary);
    const previewBody = document.createElement('div');
    previewBody.style.cssText = 'margin:10px 0 0 0;padding:14px 18px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-sm);max-height:420px;overflow:auto;text-align:left';
    previewBody.textContent = 'Loading preview…';
    preview.appendChild(previewBody);
    let previewLoaded = false;
    preview.addEventListener('toggle', async () => {
      if (previewLoaded || !preview.open) return;
      previewLoaded = true;
      try {
        const payload = buildNotebookPayloadFromCurrentState();
        const template = await getStarterTemplate();
        const wl = await renderNotebook(template, payload, 0, { offline: true, stVersion: ST_VERSION });
        renderNotebookPreview(previewBody, wl);
      } catch (err) {
        console.error('preview render failed', err);
        previewBody.textContent = '(preview unavailable)';
      }
    });
    resultBody.appendChild(preview);

    resultFrame.appendChild(resultBody);
  }

  // Footer: just a GitHub link to the package repo. The primary "Download
  // notebook" button lives inline in the result body (on each result card, or
  // in the no-result panel), so the footer stays minimal.
  const resultFooter = document.createElement('div');
  resultFooter.style.cssText = 'display:flex;justify-content:flex-end;margin-top:8px';
  const getBtn = document.createElement('a');
  getBtn.className = 'btn btn-sm btn-secondary';
  getBtn.href = 'https://github.com/SubTropica/SubTropica';
  getBtn.target = '_blank';
  getBtn.rel = 'noopener';
  getBtn.innerHTML = '<svg class="ico" viewBox="0 0 24 24"><path d="M9 19c-5 1.5-5-2.5-7-3m14 6v-3.87a3.37 3.37 0 00-.94-2.61c3.14-.35 6.44-1.54 6.44-7A5.44 5.44 0 0020 4.77 5.07 5.07 0 0019.91 1S18.73.65 16 2.48a13.38 13.38 0 00-7 0C6.27.65 5.09 1 5.09 1A5.07 5.07 0 005 4.77a5.44 5.44 0 00-1.5 3.78c0 5.42 3.3 6.61 6.44 7A3.37 3.37 0 009 18.13V22"/></svg> SubTropica on GitHub';
  resultFooter.appendChild(getBtn);
  resultFrame.appendChild(resultFooter);

  body.appendChild(resultFrame);

  overlay.classList.add('visible');
}

$('integrate-fab').addEventListener('click', () => {
  if (backendMode === 'full') doIntegrate();
  else openIntegrateModal();
});
$('integrate-close').addEventListener('click', () => {
  if (_integrationPolling) cancelIntegration();
  else {
    $('integrate-overlay').classList.remove('visible');
    const panel = $('integrate-body')?.closest('.modal-panel');
    if (panel) panel.classList.remove('integ-mode');
  }
});
$('integrate-overlay').addEventListener('click', function(evt) {
  if (evt.target === this) this.classList.remove('visible');
});

// ─── Integral card (live formula preview) ───────────────────────────

const _integralCard = $('integral-card');
const _integralPreview = $('integral-card-preview');
const _integralFormula = $('integral-card-formula');
// Default-collapsed on first mobile visit so the FAB doesn't cover the canvas.
// Desktop default remains expanded. Once the user toggles, localStorage wins.
const _integralStored = localStorage.getItem('subtropica-integral-collapsed');
let _integralCardCollapsed;
if (_integralStored === null) {
  _integralCardCollapsed = typeof matchMedia === 'function' && matchMedia('(max-width: 480px)').matches;
} else {
  _integralCardCollapsed = _integralStored === '1';
}
let _nameEditedByUser = false;  // true once user manually edits the name field
if (_integralCardCollapsed) _integralCard.classList.add('collapsed');

// Toggle via chevron on FAB — click toggles expand/collapse without triggering integrate
$('integral-card-toggle').addEventListener('click', (evt) => {
  evt.stopPropagation();
  _integralCardCollapsed = !_integralCardCollapsed;
  _integralCard.classList.toggle('collapsed', _integralCardCollapsed);
  localStorage.setItem('subtropica-integral-collapsed', _integralCardCollapsed ? '1' : '0');
});

$('integral-copy-tex').addEventListener('click', () => {
  const integral = buildFeynmanIntegralLaTeX();
  if (integral) navigator.clipboard.writeText(integral.latex).then(() => showWarningToast('LaTeX copied'));
});
$('integral-copy-mma').addEventListener('click', () => {
  const mma = buildFeynmanIntegralMathematica();
  if (mma) navigator.clipboard.writeText(mma).then(() => showWarningToast('Mathematica copied'));
});

// Name field: once the user types in it, stop auto-updating. Also live-sync
// so the Export tab's printed command tracks the chosen name.
$('ic-name').addEventListener('input', () => {
  _nameEditedByUser = true;
  computeConfig.diagramName = $('ic-name').value;
  populateExportTab();
});

// Dim and eps order: sync to computeConfig on every keystroke so the Export
// tab stays current. Using 'input' (not 'change') means the preview updates
// as the user types rather than waiting for a blur.
$('ic-dimension').addEventListener('input', () => {
  computeConfig.dimension = $('ic-dimension').value;
  updateIntegralCard();
});
$('ic-eps-order').addEventListener('input', () => {
  computeConfig.epsOrder = $('ic-eps-order').value;
  updateIntegralCard();
});

// Generate an auto-name from library matches or Nickel index
function generateAutoName() {
  if (currentMatches && currentMatches.length > 0) {
    const topo = currentMatches[0].topo;
    return topo.primaryName || topo.name || topo.Name || currentMatches[0].topoKey || '';
  }
  return currentNickel || '';
}

// Sync integral card settings fields from computeConfig
function syncIntegralCardSettings() {
  if ($('ic-dimension')) $('ic-dimension').value = computeConfig.dimension;
  if ($('ic-eps-order')) $('ic-eps-order').value = computeConfig.epsOrder;
  // Auto-populate name if user hasn't manually edited it
  if (!_nameEditedByUser && $('ic-name')) {
    $('ic-name').value = generateAutoName();
    computeConfig.diagramName = $('ic-name').value;
  }
}

// ─── Difficulty estimate (Tier 1: client-side heuristic) ────────────

let _lastDifficultyScore = -1;

function getTopologyStats() {
  const deg = getVertexDegrees();
  let nIntEdges = 0, nExtLegs = 0, nIntVerts = 0;
  const massSet = new Set();
  let hasNonUnitExp = false;

  for (let i = 0; i < state.edges.length; i++) {
    const e = state.edges[i];
    if (e.a === e.b) { nIntEdges++; continue; } // self-loop
    const extA = (deg[e.a] || 0) <= 1;
    const extB = (deg[e.b] || 0) <= 1;
    if (extA || extB) {
      nExtLegs++;
    } else {
      nIntEdges++;
    }
    if (e.mass > 0) massSet.add(e.massLabel || e.mass);
    if (e.propExponent != null && e.propExponent !== 1) hasNonUnitExp = true;
  }

  for (let v = 0; v < state.vertices.length; v++) {
    if ((deg[v] || 0) > 1) nIntVerts++;
  }

  const nLoops = Math.max(nIntEdges - nIntVerts + 1, 0);
  return { nLoops, nIntEdges, nExtLegs, nMassScales: massSet.size, hasNonUnitExp };
}

function estimateDifficulty(stats) {
  //     N     = #loops + #external legs + #(extra mass scales)
  //     stars = clamp((N − 2) / 2, 0, 5)
  //
  // "extra mass scales" = max(0, distinct nonzero mass scales − 1),
  // subtracting the one dimensional scale that can always be factored out
  // as the overall integral dimension. getTopologyStats already pools
  // internal propagator masses and external leg virtualities and dedupes
  // by label.
  //
  // Known blind spots: the formula ignores planarity, numerators, raised
  // propagators, and the dimension / ε order. The separate "Check
  // complexity" button runs the Tier 2 tropical-expansion estimate.
  const { nLoops, nExtLegs, nIntEdges, nMassScales } = stats;
  if (nIntEdges === 0) return 0;
  const extra = Math.max(0, nMassScales - 1);
  const N = nLoops + nExtLegs + extra;
  return Math.max(0, Math.min(5, (N - 2) / 2));
}

function difficultyColor(score) {
  const t = score / 5;
  const hue = 120 * (1 - t);
  return `hsl(${hue}, 75%, 42%)`;
}

const _STAR_PATH = 'M12 2L15.09 8.26 22 9.27 17 14.14 18.18 21.02 12 17.77 5.82 21.02 7 14.14 2 9.27 8.91 8.26Z';

// Unicode-superscript helper for the ε-pole labels. Falls back to
// "eps^k" for exponents outside the range the superscript table covers.
const _EPS_SUP_MAP = {
  '-': '\u207b', '0': '\u2070', '1': '\u00b9', '2': '\u00b2', '3': '\u00b3',
  '4': '\u2074', '5': '\u2075', '6': '\u2076', '7': '\u2077',
  '8': '\u2078', '9': '\u2079',
};
function _fmtEpsPole(k) {
  if (k === 0) return '\u03b5\u2070';
  const s = String(k);
  let out = '\u03b5';
  for (const ch of s) {
    if (!(ch in _EPS_SUP_MAP)) return `\u03b5^${k}`;
    out += _EPS_SUP_MAP[ch];
  }
  return out;
}

// Render a gauge-fixing choice as "x_i = 1" (HTML, using <sub> for the
// subscript). Handles the common "x<digits>" shape that handleEstimate
// ships and falls back to "<name> = 1" for anything else.
function formatGaugeLabel(name) {
  const s = String(name ?? '').trim();
  if (!s) return '';
  const m = s.match(/^([A-Za-z])(\d+)$/);
  if (m) return `${m[1]}<sub>${m[2]}</sub> = 1`;
  return `${s} = 1`;
}

// Build the three-tier tooltip body (facets list, pole orders, Nilsson-Passare
// engagement) from a Tier-2 response. Returns an HTML fragment with no
// outer wrapper — caller wraps it in `<div class="difficulty-tip">`.
// Tiers:
//   (a) nNPTerms == 1  → "Nilsson-Passare not engaged · N facets · prefactor ε^k"
//   (b) 2 ≤ nNPTerms ≤ 10 → lists verbatim
//   (c) nNPTerms > 10  → summary stats (range, mean, mode)
function formatTier2Body(tier2Data) {
  const nNP = tier2Data.nNPTerms ?? 0;
  const facetsList = Array.isArray(tier2Data.facetsList) ? tier2Data.facetsList : [];
  const poleList = Array.isArray(tier2Data.prefactorPoleOrders) ? tier2Data.prefactorPoleOrders : [];
  const totalFacets = tier2Data.nSubIntegrands ?? 0;

  if (nNP <= 0) {
    return `<span style="color:var(--text-muted)">(empty expansion)</span>`;
  }
  if (nNP === 1) {
    const nf = facetsList[0] ?? totalFacets ?? 0;
    const pole = poleList[0] ?? 0;
    return `<span style="color:var(--text-muted)">Nilsson-Passare not engaged</span> \u00b7 ` +
      `${nf} facet${nf !== 1 ? 's' : ''} \u00b7 prefactor ${_fmtEpsPole(pole)}`;
  }
  if (nNP <= 10) {
    const facetsStr = `{${facetsList.join(', ')}}`;
    const polesUniform = poleList.length > 0 && poleList.every(p => p === poleList[0]);
    const polesStr = polesUniform
      ? `${_fmtEpsPole(poleList[0])} (all ${nNP})`
      : `{${poleList.map(_fmtEpsPole).join(', ')}}`;
    return `${nNP} Nilsson-Passare terms \u00b7 facets ${facetsStr} (${totalFacets} total)<br>` +
      `prefactor poles ${polesStr}`;
  }
  // Heavy: summary stats.
  const fMin = Math.min(...facetsList);
  const fMax = Math.max(...facetsList);
  const fMean = (totalFacets / nNP).toFixed(1);
  const pMin = poleList.length > 0 ? Math.min(...poleList) : 0;
  const pMax = poleList.length > 0 ? Math.max(...poleList) : 0;
  const counts = Object.create(null);
  for (const p of poleList) counts[p] = (counts[p] || 0) + 1;
  const modeEntry = Object.entries(counts).sort((a, b) => b[1] - a[1])[0];
  const mode = modeEntry ? parseInt(modeEntry[0], 10) : 0;
  const facetRange = fMin === fMax ? `${fMin}` : `${fMin}\u2013${fMax}`;
  const poleRange = pMin === pMax ? _fmtEpsPole(pMin) : `${_fmtEpsPole(pMin)}\u2026${_fmtEpsPole(pMax)}`;
  return `${nNP} Nilsson-Passare terms \u00b7 facets ${facetRange} (mean ${fMean}, ${totalFacets} total)<br>` +
    `prefactor poles ${poleRange} (mostly ${_fmtEpsPole(mode)})`;
}

function renderDifficultyStars(score, stats, isTier2 = false, tier2Data = null, containerOverride = null) {
  // containerOverride lets callers render into a secondary location
  // (e.g. the integration window). The pulse-animation bookkeeping below
  // (_lastDifficultyScore) is tied to the main container and is skipped
  // when rendering into an override.
  const container = containerOverride || $('difficulty-stars');
  if (!container) return;
  const isOverride = containerOverride !== null;
  const isMain = !isOverride;

  // Resolve an LR verdict for the main container. Tier-2 data (passed in)
  // takes precedence; otherwise fall back to _cachedLR populated by a
  // standalone "Check if MPL" click. _lrRunning dominates everything:
  // render the normal Tier-1 stars with an inline spinner while the
  // kernel is working.
  const mplRunning = isMain && _lrRunning;
  const cachedLRState = (isMain && _cachedLR && _cachedLR.result) ? _cachedLR.result : null;
  const lrFromTier2 = (isTier2 && tier2Data && typeof tier2Data.isLR === 'boolean');
  // "Known not-MPL" takeover: only if we actually have a verdict AND we
  // are not mid-run (avoid swapping the spinner out from under the user).
  const takeoverNotMpl = !mplRunning && (
    (lrFromTier2 && tier2Data.isLR === false) ||
    (!lrFromTier2 && cachedLRState && cachedLRState.isLR === false)
  );

  // Early clear when there's no real diagram to describe. Callers that
  // want to render the MPL badge from _cachedLR alone still pass a valid
  // stats object (see updateDifficultyStars); if stats is missing,
  // treat that as "card is empty — clear everything".
  if (!stats ||
      (score <= 0 && !(isTier2 && tier2Data) && !takeoverNotMpl && !mplRunning && !(cachedLRState && cachedLRState.isLR === true))) {
    container.innerHTML = '';
    container.title = '';
    return;
  }

  // ── Not linearly reducible → replace stars with ×MPL badge ──────────
  if (takeoverNotMpl) {
    let html = `<span class="lr-badge lr-badge-notmpl">\u00d7\u2009MPL</span>`;
    html += `<div class="difficulty-tip">` +
      `<strong style="color:var(--red,#e57373)">\u2718 Not linearly reducible</strong><br>`;
    if (lrFromTier2) {
      html += formatTier2Body(tier2Data) + `<br>`;
    }
    html += `<span style="color:var(--text-muted)">SubTropica cannot evaluate this integral directly.<br>` +
      `Try rationalizing substitutions or changes of variables.</span></div>`;
    container.innerHTML = html;
    container.removeAttribute('title');
    if (!isOverride && _lastDifficultyScore !== -2) {
      _lastDifficultyScore = -2;
      container.classList.remove('difficulty-pulse');
      void container.offsetWidth;
      container.classList.add('difficulty-pulse');
    }
    return;
  }

  const color = difficultyColor(score);
  const opacity = isTier2 ? 0.92 : 0.4;
  let html = '';
  for (let i = 1; i <= 5; i++) {
    const fill = score >= i ? 'full' : (score >= i - 0.5 ? 'half' : 'empty');
    if (fill === 'full') {
      html += `<svg width="13" height="13" viewBox="0 0 24 24" style="--star-opacity:${opacity};opacity:${opacity}"><path d="${_STAR_PATH}" fill="${color}" stroke="${color}" stroke-width="0.5"/></svg>`;
    } else if (fill === 'half') {
      html += `<svg width="13" height="13" viewBox="0 0 24 24" style="--star-opacity:${opacity};opacity:${opacity}"><defs><clipPath id="hsc${i}"><rect x="0" y="0" width="12" height="24"/></clipPath></defs><path d="${_STAR_PATH}" fill="none" stroke="${color}" stroke-width="1.5"/><path d="${_STAR_PATH}" fill="${color}" clip-path="url(#hsc${i})"/></svg>`;
    } else {
      html += `<svg width="13" height="13" viewBox="0 0 24 24" style="--star-opacity:${opacity};opacity:${opacity}"><path d="${_STAR_PATH}" fill="none" stroke="${color}" stroke-width="1.5"/></svg>`;
    }
  }

  // Inline MPL decoration (main container only). Spinner while the LR
  // check is running, or MPL badge when we already have an "is LR" verdict.
  if (isMain && mplRunning) {
    html += `<span class="mpl-spinner" title="Checking polylogarithmicity\u2026"></span>`;
  } else if (isMain && !lrFromTier2 && cachedLRState && cachedLRState.isLR === true) {
    html += `<span class="lr-badge lr-badge-mpl">MPL</span>`;
  }

  // Tooltip with explanation
  const label = score <= 1 ? 'Easy' : score <= 2 ? 'Moderate' : score <= 3 ? 'Involved' : score <= 4 ? 'Hard' : 'Very hard';
  // Tooltip: always show Tier-1 formula; append Tier-2 expert details when available
  const extra = Math.max(0, stats.nMassScales - 1);
  const N = stats.nLoops + stats.nExtLegs + extra;
  let tip = `<div class="difficulty-tip">` +
    `<strong>${label}</strong> (${score.toFixed(1)}/5)<br>` +
    `${stats.nLoops} loop${stats.nLoops !== 1 ? 's' : ''} + ` +
    `${stats.nExtLegs} leg${stats.nExtLegs !== 1 ? 's' : ''} + ` +
    `${extra} extra scale${extra !== 1 ? 's' : ''} = ${N}`;

  if (isTier2 && tier2Data) {
    // MPL badge next to stars
    if (tier2Data.isLR) {
      html += `<span class="lr-badge lr-badge-mpl" style="opacity:${opacity}">MPL</span>`;
    }
    // Expert details from tropical expansion
    tip += `<br><hr style="border:none;border-top:1px solid var(--border);margin:4px 0">`;
    tip += formatTier2Body(tier2Data);
    if (tier2Data.isLR) {
      tip += `<br><span style="color:var(--green,#4caf50)">\u2714 Linearly reducible` +
        (tier2Data.lrGauge ? ` (gauge ${formatGaugeLabel(tier2Data.lrGauge)})` : '') + `</span>`;
    }
  } else {
    // Report LR verdict from a standalone MPL check, if any.
    if (isMain && mplRunning) {
      tip += `<br><span style="color:var(--text-muted)">Checking polylogarithmicity\u2026</span>`;
    } else if (isMain && cachedLRState && cachedLRState.isLR === true) {
      const gauges = cachedLRState.lrGauges || [];
      tip += `<br><span style="color:var(--green,#4caf50)">\u2714 Linearly reducible` +
        (gauges.length === 1 ? ` (gauge ${formatGaugeLabel(gauges[0])})`
          : gauges.length > 1 ? ` (${gauges.length} gauges: ${gauges.map(formatGaugeLabel).join(', ')})` : '') + `</span>`;
    }
    if (container.id === 'integ-difficulty-stars') {
      tip += `<br><span style="color:var(--text-muted)">Computing kernel estimate\u2026</span>`;
    } else if (backendMode === 'full') {
      tip += `<br><span style="color:var(--text-muted)">Click <b>Check complexity</b> for the kernel-backed estimate.</span>`;
    } else {
      tip += `<br><span style="color:var(--text-muted)">Heuristic from loops + external legs + distinct mass scales.</span>`;
    }
  }
  tip += `</div>`;
  html += tip;

  container.innerHTML = html;
  container.removeAttribute('title');

  if (!isOverride && score !== _lastDifficultyScore) {
    _lastDifficultyScore = score;
    container.classList.remove('difficulty-pulse');
    void container.offsetWidth; // reflow
    container.classList.add('difficulty-pulse');
  }
}

function updateDifficultyStars() {
  const stats = getTopologyStats();
  // Prefer a cached Tier-2 result if one is still valid for the current
  // payload — it carries the richer metrics and the MPL badge, and we
  // shouldn't silently demote it back to Tier-1 on unrelated re-renders
  // (e.g. kinematics edits that don't touch the estimate payload).
  if (_cachedTier2 && _cachedTier2.result
      && !(_cachedTier2.result.timeout || _cachedTier2.result.nTerms < 0)) {
    const score = computeTier2Score(_cachedTier2.result, stats);
    renderDifficultyStars(score, stats, true, _cachedTier2.result);
    return;
  }
  const score = estimateDifficulty(stats);
  renderDifficultyStars(score, stats);
}

// Re-paint the difficulty stars inside the integration modal (if open).
// Mirrors updateDifficultyStars but targets the #integ-difficulty-stars
// override container — safe to call before or after cache updates.
function refreshIntegDifficultyStars() {
  const holder = document.getElementById('integ-difficulty-stars');
  if (!holder) return;
  const stats = getTopologyStats();
  // Always use Tier-1 heuristic for the star count (simple, stable),
  // but pass Tier-2 data alongside so the tooltip shows expert details
  // (facets, NP terms, pole orders, LR verdict) when available.
  const score = estimateDifficulty(stats);
  const hasTier2 = _cachedTier2 && _cachedTier2.result
      && !(_cachedTier2.result.timeout || _cachedTier2.result.nTerms < 0);
  renderDifficultyStars(score, stats, hasTier2, hasTier2 ? _cachedTier2.result : null, holder);
}

// ─── Difficulty estimate (Tier 2: kernel-backed tropical expansion) ──

let _estimateTimer = null;
let _estimateGenId = 0;
let _estimateAbort = null;
let _lastEstimateHash = '';
let _cachedTier2 = null;  // { hash, result }
// Shared LR verdict cache. Populated either as a side effect of
// runTier2Check (handleEstimate returns isLR/lrGauge alongside complexity
// metrics) or by checkLinearReducibility standalone. Cache key = the same
// hash used by _cachedTier2 so the two stay in lockstep and invalidate
// together.
let _cachedLR = null;     // { hash, result: { isLR, lrGauges } }
// Cached integrand-only estimate result (graphStr/propsStr/quadStr/
// exponentsStr/isLR/lrGauge) used by the Export tab so it can populate
// the propagator / quadruple forms without waiting for Check complexity.
// Populated by requestIntegrandBuild(), a fire-and-forget call that
// posts /api/estimate with skipExpansion -> True. Superseded by
// _cachedTier2 when a full Tier-2 run lands.
let _cachedIntegrand = null;  // { hash, result }
let _integrandAbort = null;

function cancelEstimate() {
  clearTimeout(_estimateTimer);
  _estimateGenId++;
  if (_estimateAbort) { _estimateAbort.abort(); _estimateAbort = null; }
  const c = $('difficulty-stars');
  if (c) c.classList.remove('difficulty-loading');
}

// Manual Tier 2 trigger. Called when the user clicks "Check complexity"
// (or implicitly on startup from the difficulty-stars hover path, if we
// want to — but currently only the button wires it up).
//
// Tier 2 used to run automatically on a 2s debounce after every graph
// change, which was expensive and surprised users with late-arriving
// updates to the Export tab. Now Tier 2 only runs on demand.
async function runTier2Check() {
  if (backendMode !== 'full') {
    showWarningToast('Tier 2 complexity check requires the SubTropica backend');
    return;
  }
  const stats = getTopologyStats();
  if (stats.nIntEdges === 0) {
    showWarningToast('Draw a diagram first');
    return;
  }

  const payload = buildEstimatePayload();
  if (!payload) return;
  const hash = JSON.stringify(payload);

  // Cache hit — reuse previous Tier 2 result instantly.
  if (hash === _lastEstimateHash && _cachedTier2) {
    applyTier2Result(_cachedTier2.result);
    return;
  }

  // Cancel any in-flight estimate (shouldn't normally happen, since we
  // dropped the auto-schedule, but defensive).
  cancelEstimate();

  const btn = $('ic-check-complexity');
  const prevLabel = btn ? btn.textContent : '';
  if (btn) { btn.disabled = true; btn.textContent = 'Checking\u2026'; }
  try {
    await requestEstimate(payload, hash);
  } finally {
    if (btn) {
      btn.disabled = false;
      btn.textContent = prevLabel || 'Check complexity';
    }
  }
}

function buildEstimatePayload() {
  syncConfigToState();
  const deg = getVertexDegrees();
  const nEdges = state.edges.length;
  if (nEdges === 0) return null;

  const distinctMasses = new Set();
  for (const e of state.edges) {
    if (e.mass && e.mass > 0) distinctMasses.add(e.mass);
  }
  const singleMass = distinctMasses.size === 1;

  const edgePairs = [];
  const internalMasses = [];
  const propExponents = [];
  for (let i = 0; i < nEdges; i++) {
    const e = state.edges[i];
    edgePairs.push([e.a, e.b]);
    internalMasses.push(edgeMassToMma(e));
    propExponents.push(e.propExponent ?? 1);
  }

  // External masses
  const extMassInputs = document.querySelectorAll('#cfg-ext-masses [data-ext-mass]');
  let externalMasses;
  if (extMassInputs.length > 0) {
    externalMasses = Array.from(extMassInputs).map(inp => {
      const v = inp.value.trim();
      return (!v || v === '0') ? '0' : massLabelToMma(v);
    });
  } else {
    const extEdgeList = [];
    for (let i = 0; i < nEdges; i++) {
      const e = state.edges[i];
      if (e.a === e.b) continue;
      if ((deg[e.a]||0) <= 1 || (deg[e.b]||0) <= 1) extEdgeList.push(i);
    }
    externalMasses = extEdgeList.map(i => edgeMassToMma(state.edges[i]));
  }

  // Pass numerator rows to handleEstimate so the Tier 2 propagator / quad
  // strings match the actual kernel-run integrand. Without this the Export
  // tab's Propagator form would omit numerators and differ from what the
  // Integrate button runs.
  const numeratorRows = (computeConfig.numeratorRows || [])
    .filter(r => r && r.expr && r.expr.trim())
    .map(r => ({
      expr: parsePhysicsExpr(r.expr.trim()).mma,
      exp: String(r.exp ?? '-1'),
    }));

  return {
    edgePairs, internalMasses, externalMasses, propExponents,
    numeratorRows,
    dimension: computeConfig.dimension || '4 - 2*eps',
    representation: computeConfig.representation || 'Schwinger',
  };
}

async function requestEstimate(payload, hash) {
  const genId = ++_estimateGenId;
  const ctrl = new AbortController();
  _estimateAbort = ctrl;

  const container = $('difficulty-stars');
  if (container) container.classList.add('difficulty-loading');

  try {
    const r = await fetch('/api/estimate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...payload, generationId: String(genId) }),
      signal: ctrl.signal,
    });
    const data = await r.json();

    // Discard stale responses
    if (genId !== _estimateGenId) return;

    if (data.status === 'ok' && data.generationId === String(genId)) {
      _lastEstimateHash = hash;
      _cachedTier2 = { hash, result: data };
      // The full Tier-2 response is a superset of the integrand-only
      // response; caching it here means the Export tab picks it up
      // immediately and the auto-fired integrand build becomes a no-op.
      _cachedIntegrand = { hash, result: data };
      applyTier2Result(data);
    }
  } catch (e) {
    if (e.name === 'AbortError') return;
    // Network error or timeout — keep Tier 1
  } finally {
    if (container) container.classList.remove('difficulty-loading');
    if (_estimateAbort === ctrl) _estimateAbort = null;
  }
}

// Lightweight background prefetch for the Export tab. Fires
// /api/estimate with skipExpansion -> True so the kernel returns
// graphStr / propsStr / quadStr / exponentsStr / isLR after running
// just STGenerateIntegrand + the one-shot LR probe, without the
// expensive tropical expansion. Result lands in _cachedIntegrand.
async function requestIntegrandBuild() {
  if (backendMode !== 'full') return;
  const payload = buildEstimatePayload();
  if (!payload) return;
  const hash = JSON.stringify(payload);
  // Already cached (from a previous light prefetch or a full Tier-2 run)?
  if (_cachedIntegrand && _cachedIntegrand.hash === hash) return;
  // Cancel any in-flight light request for a stale graph.
  if (_integrandAbort) { try { _integrandAbort.abort(); } catch {} }
  const ctrl = new AbortController();
  _integrandAbort = ctrl;
  try {
    const r = await fetch('/api/estimate', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ ...payload, skipExpansion: true }),
      signal: ctrl.signal,
    });
    const data = await r.json();
    if (ctrl.signal.aborted) return;
    if (data.status === 'ok') {
      _cachedIntegrand = { hash, result: data };
      // Also fill the LR cache from the side-effect isLR / lrGauge.
      if (typeof data.isLR === 'boolean') {
        _cachedLR = {
          hash,
          result: {
            isLR: data.isLR,
            lrGauges: data.lrGauge ? [String(data.lrGauge)] : [],
          },
        };
      }
      // Re-render the Export tab + difficulty-stars tooltip so the
      // new props/quad + MPL badge show up without a user action.
      populateExportTab();
      updateDifficultyStars();
    }
  } catch (e) {
    if (e.name === 'AbortError') return;
    // Network/timeout errors are non-fatal — the user can still click
    // Check complexity to force a full estimate.
  } finally {
    if (_integrandAbort === ctrl) _integrandAbort = null;
  }
}

let _integrandPrefetchTimer = null;
function scheduleIntegrandPrefetch() {
  if (backendMode !== 'full') return;
  clearTimeout(_integrandPrefetchTimer);
  _integrandPrefetchTimer = setTimeout(requestIntegrandBuild, 600);
}

function applyTier2Result(data) {
  if (data.timeout || data.nTerms < 0) return;  // keep Tier 1
  // handleEstimate ships isLR / lrGauge alongside the complexity metrics,
  // so every Tier-2 run also fills the LR cache. A later "Check if MPL"
  // click with the same graph is then a pure cache hit.
  if (typeof data.isLR === 'boolean' && _lastEstimateHash) {
    _cachedLR = {
      hash: _lastEstimateHash,
      result: {
        isLR: data.isLR,
        lrGauges: data.lrGauge ? [String(data.lrGauge)] : [],
      },
    };
  }
  const stats = getTopologyStats();
  const score = computeTier2Score(data, stats);
  renderDifficultyStars(score, stats, true, data);
  // Refresh export tab with expanded InputForm strings
  populateExportTab();
}

function computeTier2Score(data, stats) {
  if (data.scaleless) return 0;

  const nVars = data.nVars || 0;
  const nNPTerms = data.nNPTerms ?? 0;
  // totalFacets mirrors the old nSubIntegrands scalar the kernel still emits.
  const totalFacets = data.nSubIntegrands || 0;
  const leadingEpsOrder = data.leadingEpsOrder || 0;

  if (nNPTerms === 0 && totalFacets === 0) return 0.5;

  let score = 0.3 * nVars;

  // Total-facet bracket (what the user will actually have to integrate).
  if (totalFacets <= 1)       score += 0;
  else if (totalFacets <= 3)  score += 0.5;
  else if (totalFacets <= 6)  score += 1.0;
  else if (totalFacets <= 20) score += 1.5;
  else if (totalFacets <= 100) score += 2.0;
  else                         score += 2.5;

  // NP engagement: a genuine multi-sector decomposition adds difficulty
  // beyond the raw facet count. The step at 20 terms is where the tooltip
  // itself switches to "summary stats" mode.
  if (nNPTerms > 1)  score += 0.5;
  if (nNPTerms > 20) score += 0.5;

  // Pole depth (deepest prefactor ε pole across all NP terms).
  score += 0.3 * Math.max(0, -leadingEpsOrder);

  // Mass scales
  score += 0.15 * (stats.nMassScales || 0);

  return Math.max(0, Math.min(5, Math.round(score * 2) / 2));
}

// ─────────────────────────────────────────────────────────────────────

function updateIntegralCard() {
  // Hide the integration card entirely in review mode — no kernels are running,
  // the Compute button would hang, and it clutters the review workspace.
  if (reviewMode) {
    _integralPreview.classList.add('empty');
    _integralCard.classList.add('collapsed');
    return;
  }
  const integral = buildFeynmanIntegralLaTeX();
  if (!integral) {
    _integralPreview.classList.add('empty');
    if (!_integralCardCollapsed) {
      _integralCard.classList.add('collapsed');
    }
    renderDifficultyStars(0);
    cancelEstimate();
    return;
  }
  _integralPreview.classList.remove('empty');
  if (!_integralCardCollapsed) {
    _integralCard.classList.remove('collapsed');
  }
  // Render formula
  _integralFormula.innerHTML = '';
  if (typeof katex !== 'undefined') {
    katex.render('\\displaystyle ' + integral.latex, _integralFormula, { throwOnError: false, displayMode: true });
  } else {
    _integralFormula.textContent = integral.latex;
  }
  // Auto-populate name (if not manually edited)
  if (!_nameEditedByUser && $('ic-name')) {
    $('ic-name').value = generateAutoName();
    computeConfig.diagramName = $('ic-name').value;
  }
  // Show the Tier-2 complexity check + "Check if MPL" buttons only in
  // full mode with at least one edge.
  const fullModeHasGraph = backendMode === 'full' && state.edges.length > 0;
  const mplBtn = $('ic-check-mpl');
  if (mplBtn) mplBtn.style.display = fullModeHasGraph ? '' : 'none';
  const cplxBtn = $('ic-check-complexity');
  if (cplxBtn) cplxBtn.style.display = fullModeHasGraph ? '' : 'none';
  const saveExitBtn = $('ic-save-exit');
  if (saveExitBtn) saveExitBtn.style.display = fullModeHasGraph ? '' : 'none';

  // Tier 2 / MPL are manual ("Check complexity" / "Check if MPL").
  // Invalidate caches when the underlying payload changes so the
  // Export tab, stars, and inline MPL badge don't display a stale verdict
  // from a previous diagram.
  let currentPayloadHash = '';
  if (backendMode === 'full') {
    const currentPayload = buildEstimatePayload();
    currentPayloadHash = currentPayload ? JSON.stringify(currentPayload) : '';
    if (_cachedTier2 && (!currentPayload || currentPayloadHash !== _lastEstimateHash)) {
      _cachedTier2 = null;
      _lastEstimateHash = '';
      cancelEstimate();
    }
    if (_cachedLR && (!currentPayload || currentPayloadHash !== _cachedLR.hash)) {
      _cachedLR = null;
    }
    if (_cachedIntegrand && (!currentPayload || currentPayloadHash !== _cachedIntegrand.hash)) {
      _cachedIntegrand = null;
      if (_integrandAbort) { try { _integrandAbort.abort(); } catch {} _integrandAbort = null; }
    }
  }

  // Render Tier-1 difficulty stars (cheap, client-side heuristic). Tier 2
  // overwrites these when the user clicks "Check complexity" and the
  // result arrives.
  updateDifficultyStars();
  // Keep export tab current
  populateExportTab();

  // Auto-fire the lightweight integrand prefetch so the Export tab's
  // propagator / quadruple forms and the inline MPL badge populate
  // without waiting for an explicit Check complexity click. Cached hash
  // matches → this is an instant no-op. Debounced to avoid firing on
  // every edge drag.
  if (backendMode === 'full' && currentPayloadHash &&
      !(_cachedIntegrand && _cachedIntegrand.hash === currentPayloadHash) &&
      !(_cachedTier2 && _cachedTier2.hash === currentPayloadHash)) {
    scheduleIntegrandPrefetch();
  }
}

// ─── Auto-save / restore ────────────────────────────────────────────

const SAVE_KEY = 'subtropica-diagram';

function saveDiagram() {
  try {
    localStorage.setItem(SAVE_KEY, JSON.stringify({
      vertices: state.vertices,
      edges: state.edges.map(copyEdge),
      labelMode, showArrows,
      computeConfig,
    }));
  } catch (_) { /* quota exceeded — silently skip */ }
}

function restoreDiagram() {
  try {
    const saved = localStorage.getItem(SAVE_KEY);
    if (!saved) return false;
    const data = JSON.parse(saved);
    if (data.vertices && data.vertices.length > 0) {
      state.vertices = data.vertices;
      state.edges = (data.edges || []).map(e => ({
        ...e,
        propExponent: e.propExponent ?? 1,
      }));
      if (data.labelMode && LABEL_MODES.includes(data.labelMode)) {
        labelMode = data.labelMode;
        $('momenta-toggle').checked = labelMode === 'momenta';
      }
      if (data.showArrows !== undefined) {
        showArrows = data.showArrows;
        $('arrows-toggle').checked = showArrows;
      }
      // Restore computation config
      if (data.computeConfig) {
        Object.assign(computeConfig, data.computeConfig);
      }
      return true;
    }
  } catch (_) { /* corrupted data — ignore */ }
  return false;
}

// ─── Deep-link URLs (share diagram via hash) ─────────────────────────

function diagramToHash() {
  if (state.vertices.length === 0) return '';
  const data = {
    v: state.vertices.map(v => [Math.round(v.x * 1000) / 1000, Math.round(v.y * 1000) / 1000]),
    e: state.edges.map(e => {
      const r = [e.a, e.b];
      if (e.mass) r.push(e.mass);
      if (e.style && e.style !== 'solid') r.push(e.style);
      if (e.massLabel) r.push('m:' + e.massLabel);
      if (e.edgeLabel) r.push('l:' + e.edgeLabel);
      return r;
    }),
  };
  try {
    return btoa(unescape(encodeURIComponent(JSON.stringify(data))));
  } catch (_) { return ''; }
}

function diagramFromHash(hash) {
  try {
    const json = decodeURIComponent(escape(atob(hash)));
    const data = JSON.parse(json);
    if (!data.v || !data.e) return false;
    state.vertices = data.v.map(([x, y]) => ({ x, y }));
    state.edges = data.e.map(arr => {
      const edge = { a: arr[0], b: arr[1], mass: 0, style: 'solid', massLabel: '', edgeLabel: '', extMomLabel: '', propExponent: 1 };
      for (let i = 2; i < arr.length; i++) {
        const val = arr[i];
        if (typeof val === 'number') edge.mass = val;
        else if (typeof val === 'string') {
          if (val.startsWith('m:')) edge.massLabel = val.slice(2);
          else if (val.startsWith('l:')) edge.edgeLabel = val.slice(2);
          else if (LINE_STYLES.some(s => s.id === val)) edge.style = val;
        }
      }
      return edge;
    });
    return true;
  } catch (_) { return false; }
}

function updateUrlHash() {
  const h = diagramToHash();
  if (h) history.replaceState(null, '', '#d=' + h);
  else history.replaceState(null, '', window.location.pathname + window.location.search);
}

function restoreFromUrlHash() {
  const hash = window.location.hash;
  if (!hash.startsWith('#d=')) return false;
  return diagramFromHash(hash.slice(3));
}

// ─── Disable autocomplete globally ───────────────────────────────────

// Set autocomplete="off" on all existing inputs and observe future ones
document.querySelectorAll('input').forEach(el => el.setAttribute('autocomplete', 'off'));
new MutationObserver(mutations => {
  for (const m of mutations) {
    for (const node of m.addedNodes) {
      if (node.nodeType !== 1) continue;
      if (node.tagName === 'INPUT') node.setAttribute('autocomplete', 'off');
      else node.querySelectorAll?.('input')?.forEach(el => el.setAttribute('autocomplete', 'off'));
    }
  }
}).observe(document.body, { childList: true, subtree: true });

// ════════════════════════════════════════════════════════════════════
// REVIEW MODE
// ════════════════════════════════════════════════════════════════════
//
// Internal review toolkit, activated by ?review=1 URL flag.
// Hard-gated on backendMode === 'full' — every entry point bails out
// in lite mode. Never reachable from subtropi.ca.
//
// Three layers of protection (also gated server-side):
//   1. The /api/review/* endpoints don't exist on Firebase hosting.
//   2. SubTropica.wl's local Python server binds to 127.0.0.1 only.
//   3. reviewIsActive() checks backendMode before any DOM/network action.

let reviewMode = false;
let reviewCurrentEntryPath = null;  // updated on every openDetailPanel
let _reviewLoadingEntry = false;    // guard: suppress dirty detection during canvas load
let _reviewLoadedCNickel = null;    // CNickelIndex of the currently loaded entry (for dirty comparison)
let _reviewNameIndex = null;        // lazy: name (lowercase) → count of entries sharing that name
let _reviewNameSuggestions = null;  // lazy: loaded from /api/review/name_suggestions
const reviewState = {
  queueType: 'pairs',     // 'pairs' or 'rejects'
  queueFilter: 'pending', // 'pending' | 'all' | 'verified'
  allItems: [],           // unfiltered queue from server
  queue: [],              // filtered subset that prev/next operate on
  cursor: 0,
  loaded: false,
  focusedRecordIndex: 0,
  // Client-side filter chips (sets of integer values to keep)
  filters: { loops: new Set(), legs: new Set(), massScales: new Set() },
};

const reviewKernel = {
  async getQueue(type, filter) {
    const r = await fetch('/api/review/queue?type=' + encodeURIComponent(type) +
                          '&filter=' + encodeURIComponent(filter));
    return r.json();
  },
  async getEntry(path) {
    const r = await fetch('/api/review/entry?path=' + encodeURIComponent(path));
    return r.json();
  },
  async verifyPair(path, recordId, verified) {
    const r = await fetch('/api/review/verify_pair', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, recordId, verified }),
    });
    return r.json();
  },
  async promoteFromWaitlist(path) {
    const r = await fetch('/api/review/promote_from_waitlist', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path }),
    });
    return r.json();
  },
  async saveEntry(path, entry) {
    const r = await fetch('/api/review/save_entry', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, entry }),
    });
    return r.json();
  },
  async removeRecord(path, recordId) {
    const r = await fetch('/api/review/remove_record', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path, recordId }),
    });
    return r.json();
  },
  async deleteEntry(path) {
    const r = await fetch('/api/review/delete_entry', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path }),
    });
    return r.json();
  },
};

function reviewIsActive() {
  return reviewMode === true && backendMode === 'full';
}

// Convert a topology key like "1112|3|344|5|e5|e|" + config "0000|0|000|0|10|1|"
// into the on-disk relative path "1112_3_344_5_e5_e_/0000_0_000_0_10_1_/entry.json"
function reviewPathForKey(topoKey, configKey) {
  if (!topoKey || !configKey) return null;
  const t = String(topoKey).replace(/\|/g, '_');
  const c = String(configKey).replace(/\|/g, '_');
  return t + '/' + c + '/entry.json';
}

async function reviewInit() {
  const params = new URLSearchParams(window.location.search);
  if (params.get('review') !== '1') return;
  // Wait briefly for backendMode detection to complete.
  for (let i = 0; i < 30; i++) {
    if (backendMode === 'full') break;
    await new Promise(r => setTimeout(r, 100));
  }
  if (backendMode !== 'full') {
    console.warn('[review] ?review=1 ignored: backendMode is not "full"');
    return;
  }
  reviewMode = true;
  document.body.classList.add('review-mode');
  reviewSetupToolbar();
  await reviewLoadQueue();
}

function reviewSetupToolbar() {
  if (!reviewIsActive()) return;
  const tb = document.getElementById('review-toolbar');
  if (!tb) return;
  tb.style.display = 'flex';
  document.getElementById('review-prev-btn')?.addEventListener('click', reviewPrev);
  document.getElementById('review-next-btn')?.addEventListener('click', reviewNext);
  document.getElementById('review-reload-btn')?.addEventListener('click', reviewLoadQueue);
  document.getElementById('review-exit-btn')?.addEventListener('click', reviewExit);
  document.querySelectorAll('.review-tab').forEach(tab => {
    tab.addEventListener('click', () => {
      reviewState.queueType = tab.dataset.queueType;
      document.querySelectorAll('.review-tab').forEach(x =>
        x.classList.toggle('review-tab-active', x === tab));
      reviewLoadQueue();
    });
  });
  const filterSel = document.getElementById('review-filter');
  if (filterSel) {
    filterSel.value = reviewState.queueFilter;
    filterSel.addEventListener('change', () => {
      reviewState.queueFilter = filterSel.value;
      reviewLoadQueue();
    });
  }
  document.getElementById('review-filter-clear')?.addEventListener('click', reviewClearFilters);
  // Jump-to-index: commit on Enter or blur
  const progInput = document.getElementById('review-progress-input');
  if (progInput) {
    const commitJump = () => {
      const n = parseInt(progInput.value, 10);
      if (!isNaN(n) && n >= 1 && n <= reviewState.queue.length) {
        const target = n - 1;
        if (target !== reviewState.cursor) {
          if (reviewState.dirty && !confirm('Discard unsaved canvas changes?')) {
            progInput.value = String(reviewState.cursor + 1);
            return;
          }
          reviewSetDirty(false);
          reviewState.cursor = target;
          reviewLoadCurrent();
          reviewUpdateProgress();
        }
      } else {
        progInput.value = String(reviewState.cursor + 1);
      }
    };
    progInput.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); progInput.blur(); } });
    progInput.addEventListener('blur', commitJump);
    progInput.addEventListener('focus', () => progInput.select());
  }
  // reviewSetupPdfPanel() is called unconditionally at page init (works in both modes)
  reviewSetupSaveBar();
}

function reviewExit() {
  // Strip ?review=1 and reload — back to normal lite/full UX.
  const u = new URL(window.location.href);
  u.searchParams.delete('review');
  window.location.href = u.toString();
}

async function reviewLoadQueue() {
  if (!reviewIsActive()) return;
  reviewUpdateProgress('Loading…');
  const data = await reviewKernel.getQueue(reviewState.queueType, reviewState.queueFilter);
  if (data.status !== 'ok') {
    showWarningToast('Review queue load failed: ' + (data.error || ''), true);
    return;
  }
  reviewState.allItems = data.items || [];
  reviewState.loaded = true;
  reviewRestoreFilters();
  reviewRenderFilterChips();
  reviewApplyFilters({ resetCursor: true, loadFirst: true });
}

function reviewUpdateProgress(textOverride) {
  const inp = document.getElementById('review-progress-input');
  const tot = document.getElementById('review-progress-total');
  if (!inp || !tot) return;
  if (textOverride) { inp.value = '…'; tot.textContent = textOverride; return; }
  if (reviewState.queue.length === 0) {
    inp.value = '0'; tot.textContent = '0';
    return;
  }
  inp.value = String(reviewState.cursor + 1);
  tot.textContent = String(reviewState.queue.length);
}

// ── Filter chips (loops / legs / mass scales) ──
//
// Filters are client-side: the full queue is loaded once, then filtered
// against the chip selection. Filter state persists in localStorage so it
// survives page reloads.

const REVIEW_FILTER_STORAGE = 'subtropica-review-filters';
const REVIEW_FILTER_GROUPS = [
  { key: 'loops', field: 'loops', containerId: 'review-filter-loops' },
  { key: 'legs',  field: 'legs',  containerId: 'review-filter-legs' },
  { key: 'massScales', field: 'massScales', containerId: 'review-filter-ms' },
];

function reviewRestoreFilters() {
  try {
    const raw = localStorage.getItem(REVIEW_FILTER_STORAGE);
    if (!raw) return;
    const saved = JSON.parse(raw);
    for (const g of REVIEW_FILTER_GROUPS) {
      reviewState.filters[g.key] = new Set(Array.isArray(saved[g.key]) ? saved[g.key] : []);
    }
  } catch (_) {}
}

function reviewSaveFilters() {
  const out = {};
  for (const g of REVIEW_FILTER_GROUPS) {
    out[g.key] = [...reviewState.filters[g.key]];
  }
  try { localStorage.setItem(REVIEW_FILTER_STORAGE, JSON.stringify(out)); } catch (_) {}
}

function reviewClearFilters() {
  for (const g of REVIEW_FILTER_GROUPS) reviewState.filters[g.key].clear();
  reviewSaveFilters();
  reviewRenderFilterChips();
  reviewApplyFilters({ resetCursor: true, loadFirst: true });
}

function reviewRenderFilterChips() {
  const row = document.getElementById('review-filter-row');
  if (!row) return;
  // Hide the row entirely if there's nothing to filter on
  let totalDistinctValues = 0;
  for (const g of REVIEW_FILTER_GROUPS) {
    const vals = new Set();
    for (const it of reviewState.allItems) {
      const v = it[g.field];
      if (v !== undefined && v !== null) vals.add(v);
    }
    totalDistinctValues += vals.size;
  }
  if (totalDistinctValues === 0) {
    row.style.display = 'none';
    document.body.classList.remove('review-has-filters');
    return;
  }
  row.style.display = 'flex';
  document.body.classList.add('review-has-filters');

  for (const g of REVIEW_FILTER_GROUPS) {
    const container = document.getElementById(g.containerId);
    if (!container) continue;
    // Compute distinct values from the full queue
    const valuesSet = new Set();
    for (const it of reviewState.allItems) {
      const v = it[g.field];
      if (v !== undefined && v !== null) valuesSet.add(v);
    }
    const values = [...valuesSet].sort((a, b) => a - b);
    // Drop the group entirely if there's only one distinct value
    if (values.length <= 1) {
      container.style.display = 'none';
      continue;
    }
    container.style.display = 'flex';
    // Remove old chip elements (keep the label)
    [...container.querySelectorAll('.review-filter-chip')].forEach(el => el.remove());
    // Drop persisted selections that no longer exist in the data
    for (const v of [...reviewState.filters[g.key]]) {
      if (!valuesSet.has(v)) reviewState.filters[g.key].delete(v);
    }
    for (const v of values) {
      const chip = document.createElement('button');
      chip.className = 'review-filter-chip';
      chip.textContent = String(v);
      chip.dataset.value = String(v);
      if (reviewState.filters[g.key].has(v)) chip.classList.add('active');
      chip.addEventListener('click', () => {
        if (reviewState.filters[g.key].has(v)) {
          reviewState.filters[g.key].delete(v);
          chip.classList.remove('active');
        } else {
          reviewState.filters[g.key].add(v);
          chip.classList.add('active');
        }
        reviewSaveFilters();
        reviewApplyFilters({ resetCursor: true, loadFirst: true });
      });
      container.appendChild(chip);
    }
  }
}

function reviewApplyFilters(opts) {
  opts = opts || {};
  const filters = reviewState.filters;
  const hasFilter = REVIEW_FILTER_GROUPS.some(g => filters[g.key].size > 0);
  if (!hasFilter) {
    reviewState.queue = reviewState.allItems.slice();
  } else {
    reviewState.queue = reviewState.allItems.filter(it => {
      for (const g of REVIEW_FILTER_GROUPS) {
        const sel = filters[g.key];
        if (sel.size === 0) continue;
        if (!sel.has(it[g.field])) return false;
      }
      return true;
    });
  }
  if (opts.resetCursor) reviewState.cursor = 0;
  reviewUpdateProgress();
  if (opts.loadFirst) {
    if (reviewState.queue.length > 0) {
      reviewLoadCurrent();
    } else {
      const lbl = document.getElementById('review-current-label');
      if (lbl) lbl.textContent = '(no matches)';
    }
  }
}

function reviewLoadCurrent() {
  if (!reviewIsActive()) return;
  const item = reviewState.queue[reviewState.cursor];
  if (!item) return;

  if (reviewState.queueType === 'rejects') {
    reviewLoadReject(item);
  } else {
    reviewLoadPair(item);
  }
}

function reviewLoadPair(item) {
  reviewSetDirty(false);
  reviewCurrentEntryPath = item.path;
  _reviewLoadedCNickel = item.cNickel || null;
  reviewState.focusedRecordIndex = item.recordIndex || 0;
  const lbl = document.getElementById('review-current-label');
  if (lbl) {
    const name = (item.names && item.names[0]) || item.cNickel || '?';
    lbl.textContent = name + '  \u00b7  ' + (item.reference || '');
  }
  _reviewLoadingEntry = true;
  if (library && library.topologies) {
    let topoKey = null, configKey = null;
    for (const tk in library.topologies) {
      const cfgs = library.topologies[tk].configs || {};
      for (const ck in cfgs) {
        const cni = cfgs[ck].nickel || cfgs[ck].CNickelIndex || cfgs[ck].CNickel;
        if (cni === item.cNickel) {
          topoKey = tk; configKey = ck; break;
        }
      }
      if (topoKey) break;
    }
    if (topoKey && configKey) loadFromNickel(topoKey, configKey);
  }
  _reviewLoadingEntry = false;
  reviewPopulateSidePanel(item);
  const pdfArxivId = reviewExtractArxivId(item.reference);
  if (pdfArxivId) reviewLoadPdf(pdfArxivId);
}

function reviewLoadReject(item) {
  reviewSetDirty(false);
  reviewCurrentEntryPath = null;
  _reviewLoadedCNickel = null;
  const lbl = document.getElementById('review-current-label');
  if (lbl) lbl.textContent = (item.familyName || '?') + '  \u00b7  ' + (item.arxivId || '');

  // Pre-fill the canvas from candidate edges if available
  _reviewLoadingEntry = true;
  const c = item.candidate;
  if (c && Array.isArray(c.edges) && c.edges.length > 0 && Array.isArray(c.edges[0])) {
    reviewLoadCandidateCanvas(c);
  } else {
    clearAll();
  }
  _reviewLoadingEntry = false;

  reviewPopulateRejectSidePanel(item);
  if (item.arxivId) reviewLoadPdf(item.arxivId);
}

function reviewLoadCandidateCanvas(c) {
  try {
    const edges = c.edges;
    const extVerts = new Set(c.extVerts || []);
    const edgeMasses = c.edgeMasses || [];
    // Collect all nodes
    const nodes = new Set();
    edges.forEach(([a, b]) => { nodes.add(a); nodes.add(b); });
    const nodeList = [...nodes].sort((a, b) => a - b);
    const nodeMap = {};
    nodeList.forEach((n, i) => { nodeMap[n] = i; });
    const verts = nodeList.map((_, i) => ({
      x: Math.cos(2 * Math.PI * i / nodeList.length) * 1.5,
      y: Math.sin(2 * Math.PI * i / nodeList.length) * 1.5
    }));
    let nv = verts.length;
    const canvasEdges = [];
    edges.forEach(([a, b], idx) => {
      const ma = nodeMap[a], mb = nodeMap[b];
      const mass = edgeMasses[idx] || 0;
      canvasEdges.push({ a: Math.min(ma, mb), b: Math.max(ma, mb), mass });
    });
    // Add legs for external vertices
    for (const ev of extVerts) {
      if (!(ev in nodeMap)) continue;
      const iv = nodeMap[ev];
      const legIdx = nv++;
      verts.push({ x: verts[iv].x * 1.8, y: verts[iv].y * 1.8 });
      canvasEdges.push({ a: legIdx, b: iv, mass: 0 });
    }
    const laid = computeForceLayout(verts, canvasEdges);
    pushUndoState();
    state.vertices = laid;
    state.edges = canvasEdges;
    _momentumLabels = null;
    zoomLevel = 1.0;
    panOffset = { x: 0, y: 0 };
    applyZoom();
    onGraphChanged();
  } catch (e) {
    console.warn('[review] Failed to load candidate canvas:', e);
    clearAll();
  }
}

function reviewPopulateRejectSidePanel(item) {
  if (!reviewIsActive()) return;
  const panel = document.getElementById('review-side-panel');
  if (!panel) return;
  panel.style.display = 'block';
  const nameEl = document.getElementById('review-side-name');
  const nickelEl = document.getElementById('review-side-nickel');
  const badgesEl = document.getElementById('review-side-badges');
  const body = document.getElementById('review-side-body');

  if (nameEl) nameEl.textContent = item.familyName || '(unnamed)';
  if (nickelEl) nickelEl.textContent = 'arXiv:' + (item.arxivId || '?');
  if (badgesEl) {
    badgesEl.innerHTML =
      `<span class="badge badge-muted">${item.loops ?? '?'} loop${item.loops !== 1 ? 's' : ''}</span>` +
      `<span class="badge badge-muted">${item.legs ?? '?'} leg${item.legs !== 1 ? 's' : ''}</span>` +
      `<span class="badge badge-accent" style="background:var(--red-bg);color:var(--red)">rejected</span>`;
  }
  body.innerHTML = '';

  // Reject reason
  const reasonSec = document.createElement('div');
  reasonSec.className = 'review-side-section';
  reasonSec.innerHTML = '<div class="review-side-section-title">Reject Reason</div>' +
    '<div class="review-side-kv"><span class="review-side-kv-val" style="color:var(--red)">' +
    escapeHtml(item.id ? item.id.split('::')[1] || '' : '') + '</span></div>';
  body.appendChild(reasonSec);

  // Candidate metadata (from extracted_integrals.json)
  const c = item.candidate;
  if (c) {
    const metaSec = document.createElement('div');
    metaSec.className = 'review-side-section';
    metaSec.innerHTML = '<div class="review-side-section-title">Extracted Metadata</div>';
    const kvs = [
      ['Title', c.title || ''],
      ['Authors', typeof c.authors === 'string' ? c.authors : ''],
      ['Year', c.year ? String(c.year) : ''],
      ['FunctionClass', c.functionClass || ''],
      ['DimScheme', c.dimScheme || ''],
      ['EpsilonOrder', c.epsilonOrder || ''],
      ['ComputationLevel', c.computationLevel || ''],
      ['MassConfig', c.massConfig || ''],
      ['Location', c.location || ''],
    ];
    for (const [k, v] of kvs) {
      if (!v) continue;
      const row = document.createElement('div');
      row.className = 'review-side-kv';
      row.innerHTML = `<span class="review-side-kv-key">${escapeHtml(k)}</span><span class="review-side-kv-val">${escapeHtml(v)}</span>`;
      metaSec.appendChild(row);
    }
    body.appendChild(metaSec);

    // Description
    if (c.description) {
      const descSec = document.createElement('div');
      descSec.className = 'review-side-section';
      descSec.innerHTML = '<div class="review-side-section-title">Description</div>' +
        '<div class="review-side-record-desc">' + escapeHtml(c.description) + '</div>';
      body.appendChild(descSec);
    }

    // Propagators (if present)
    if (c.propagators && c.propagators.length > 0) {
      const propSec = document.createElement('div');
      propSec.className = 'review-side-section';
      propSec.innerHTML = '<div class="review-side-section-title">Propagators</div>';
      const list = document.createElement('div');
      list.style.cssText = 'font-family:var(--mono,monospace);font-size:10px;line-height:1.6;color:var(--text-mid)';
      c.propagators.forEach(p => {
        const d = document.createElement('div');
        d.textContent = String(p);
        list.appendChild(d);
      });
      propSec.appendChild(list);
      body.appendChild(propSec);
    }
  }

  // Action buttons
  const actionSec = document.createElement('div');
  actionSec.className = 'review-side-section';
  actionSec.style.cssText = 'display:flex;gap:8px;margin-top:12px;';

  const acceptBtn = document.createElement('button');
  acceptBtn.className = 'review-verify-btn';
  acceptBtn.style.cssText = 'flex:1;justify-content:center;padding:8px;font-size:12px;';
  acceptBtn.textContent = '\u2713 Accept (A)';
  acceptBtn.title = 'Create a library entry from the current canvas state';
  acceptBtn.addEventListener('click', () => reviewAcceptReject(item));

  const rejectBtn = document.createElement('button');
  rejectBtn.className = 'review-remove-btn';
  rejectBtn.style.cssText = 'flex:1;text-align:center;padding:8px;font-size:12px;';
  rejectBtn.textContent = '\u2715 Confirm Reject (R)';
  rejectBtn.title = 'Mark this candidate as legitimately not a library entry';
  rejectBtn.addEventListener('click', () => reviewConfirmReject(item));

  actionSec.appendChild(acceptBtn);
  actionSec.appendChild(rejectBtn);
  body.appendChild(actionSec);
}

async function reviewAcceptReject(item) {
  // Build CNickelIndex from current canvas state
  const edgeData = buildEdgeData();
  if (!edgeData || edgeData.edges.length === 0) {
    showWarningToast('Draw a diagram on the canvas first', true);
    return;
  }
  let canonResult;
  try {
    if (!isConnected(edgeData.edges)) {
      showWarningToast('Graph is not connected', true);
      return;
    }
    canonResult = canonicalize(edgeData.edges);
  } catch (e) {
    showWarningToast('Canonicalization failed: ' + e.message, true);
    return;
  }
  const topoNickel = canonResult.string;
  // Build the mass config key from canvas masses in canonical order
  const canvasMasses = getCanvasMassArray(
    canonResult.nickel, canonResult.nodeMaps[0], edgeData.edges, edgeData.masses
  );
  // Assign config labels: 0 for massless, 1/2/3/... for distinct nonzero masses
  const massToLabel = {};
  let nextLabel = 1;
  const configChars = [];
  let massScales = 0;
  let massIdx = 0;
  for (let i = 0; i < canonResult.nickel.length; i++) {
    for (const j of canonResult.nickel[i]) {
      const m = canvasMasses[massIdx++] || 0;
      if (m === 0) {
        configChars.push('0');
      } else {
        if (!(m in massToLabel)) { massToLabel[m] = nextLabel++; massScales++; }
        configChars.push(String(massToLabel[m]));
      }
    }
    configChars.push('|');
  }
  const configKey = configChars.join('');
  const cNickelIndex = topoNickel + ':' + configKey;
  // Build the relative path: pipes → underscores
  const topoDir = topoNickel.replace(/\|/g, '_');
  const cfgDir = configKey.replace(/\|/g, '_');
  const relPath = topoDir + '/' + cfgDir + '/entry.json';

  // Compute loops and legs from the graph
  const nInternal = new Set();
  const nLegs = { count: 0 };
  edgeData.edges.forEach(([a, b]) => {
    if (a >= 0) nInternal.add(a);
    if (b >= 0) nInternal.add(b);
    if (a === LEG) nLegs.count++;
    if (b === LEG) nLegs.count++;
  });
  const V = nInternal.size;
  const E = edgeData.edges.length - nLegs.count; // internal edges only
  const L = E - V + 1; // Euler formula for loops

  // Build the candidate metadata into an entry
  const c = item.candidate || {};
  const arxivRef = item.arxivId ? ('arXiv:' + item.arxivId) : '';
  const newEntry = {
    CNickelIndex: cNickelIndex,
    MassScales: massScales,
    FunctionClass: c.functionClass || 'unknown',
    EpsilonOrder: c.epsilonOrder || '',
    References: arxivRef ? [arxivRef] : [],
    Records: [{
      epsOrders: c.epsilonOrder || '',
      reference: arxivRef,
      authors: c.authors || '',
      description: c.description || '',
      recordId: 'bf_' + item.id.replace(/[^a-zA-Z0-9]/g, '').slice(0, 12),
      verified: false,
      verifiedAt: '',
      location: c.location || '',
      computationLevel: c.computationLevel || '',
      dimScheme: c.dimScheme || '',
      familyName: item.familyName || '',
    }],
    Source: ['arXiv'],
    Results: [],
    Names: [item.familyName || ''],
  };
  const topoStub = {
    NickelIndex: topoNickel,
    Name: item.familyName || topoNickel,
    Loops: L,
    Legs: nLegs.count,
    Propagators: E,
  };

  if (!confirm('Create library entry?\n\n' +
    'CNickelIndex: ' + cNickelIndex + '\n' +
    'Path: library-bundled/' + relPath + '\n' +
    'Loops: ' + L + ', Legs: ' + nLegs.count + ', Props: ' + E + '\n' +
    'MassScales: ' + massScales)) return;

  // Create the entry
  const createResult = await fetch('/api/review/create_entry', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ path: relPath, entry: newEntry, topology: topoStub, force: false }),
  }).then(r => r.json());
  if (createResult.status === 'conflict') {
    showWarningToast('Entry already exists at ' + relPath + ' \u2014 skipping creation', true);
  } else if (createResult.status !== 'ok') {
    showWarningToast('Create failed: ' + (createResult.error || ''), true);
    return;
  }

  // Mark as accepted in the rejects state
  await fetch('/api/review/reject_state', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: item.id, status: 'accepted', acceptedAs: relPath }),
  }).then(r => r.json());

  // Reload library so the new entry shows up
  library = null;
  await loadLibrary();
  showWarningToast('Entry created at ' + relPath, false);
  setTimeout(reviewNext, 300);
}

async function reviewConfirmReject(item) {
  if (!confirm('Confirm this candidate should stay rejected?\n\n' +
    item.familyName + ' (' + item.arxivId + ')')) return;
  const result = await fetch('/api/review/reject_state', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: item.id, status: 'confirmed_rejected' }),
  }).then(r => r.json());
  if (result.status !== 'ok') {
    showWarningToast('Reject failed: ' + (result.error || ''), true);
    return;
  }
  setTimeout(reviewNext, 300);
}

// ── PDF panel (left side, arxiv paper viewer) ──

let _pdfLib = null;        // lazy-loaded pdfjsLib module
let _pdfDoc = null;        // current PDFDocumentProxy
let _pdfPage = 1;          // current page number (tracked via scroll)
let _pdfArxivId = null;    // arxivId of the currently loaded PDF
let _pdfPages = [];        // per-page state: {num, container, canvas, textLayer, rendered, scale, renderTask}
let _pdfObserver = null;   // IntersectionObserver for lazy render + page tracking
let _pdfBaseDims = null;   // {width, height} of page 1 at scale 1.0 — used for placeholder sizing
let _pdfResizeRAF = 0;     // rAF throttle for panel-width resize re-layout
let _pdfBibtexCache = {};  // arxivId -> bibtex text

async function reviewEnsurePdfLib() {
  if (_pdfLib) return _pdfLib;
  try {
    _pdfLib = await import('https://cdnjs.cloudflare.com/ajax/libs/pdf.js/4.4.168/pdf.min.mjs');
    _pdfLib.GlobalWorkerOptions.workerSrc =
      'https://cdnjs.cloudflare.com/ajax/libs/pdf.js/4.4.168/pdf.worker.min.mjs';
    return _pdfLib;
  } catch (e) {
    console.error('[review] PDF.js import failed:', e);
    return null;
  }
}

function reviewExtractArxivId(ref) {
  if (!ref) return null;
  const m = ref.match(/(\d{4}\.\d{4,5})/);
  if (m) return m[1];
  const old = ref.match(/((?:hep-(?:ph|th|lat|ex)|astro-ph|gr-qc|cond-mat|math-ph|nucl-th|quant-ph|nlin|math)\/\d{7})/);
  return old ? old[1] : null;
}

function getPdfUrl(arxivId) {
  if (backendMode === 'full') {
    return '/api/review/pdf?arxivId=' + encodeURIComponent(arxivId);
  }
  // Online mode: use Cloudflare Worker proxy
  return 'https://subtropica-submit.subtropica.workers.dev/pdf?arxivId=' + encodeURIComponent(arxivId);
}

async function openPdfPanel(arxivId, rec) {
  if (!arxivId) return;
  const inReview = document.body.classList.contains('review-mode');
  if (inReview) {
    // Review mode: dedicated left panel
    const panel = document.getElementById('review-pdf-panel');
    if (!panel) return;
    panel.style.display = 'flex';
    document.body.classList.add('review-has-pdf');
  } else {
    // Main UI: open config panel → PDF preview tab, bring to front
    openConfigPanel();
    const paperTab = document.querySelector('.config-tab[data-tab="cfg-paper"]');
    if (paperTab) switchConfigTab(paperTab);
    const empty = document.getElementById('cfg-pdf-empty');
    if (empty) empty.style.display = 'none';
    const nav = document.getElementById('cfg-pdf-nav');
    if (nav) nav.style.display = '';
    const pages = document.getElementById('cfg-pdf-pages');
    if (pages) pages.style.display = '';
    const back = document.getElementById('cfg-pdf-back');
    if (back) back.style.display = 'inline-block';
    focusPanel('config');
  }
  _populatePdfRef(arxivId, rec, inReview);
  if (!inReview) _populatePdfCoverage(arxivId);
  await _loadPdfIntoPanel(arxivId);
}

// Render an author line (comma-separated linked names) from either an array
// of INSPIRE author records ({full_name, inspire_id, display_name}) or a
// plain string like "Doe, J., Smith, A.". Returns HTML fragment, or '' if
// no usable author data.
function _renderAuthorsHtml(authors) {
  if (!authors) return '';
  let formatted, hadEtAl = false;
  if (Array.isArray(authors)) {
    if (authors.length === 0) return '';
    formatted = authors.map(a => {
      const name = (typeof a === 'string') ? a : (a.full_name || a.display_name || '');
      if (!name) return '';
      const iid = (typeof a === 'object') ? a.inspire_id : null;
      const url = iid
        ? `https://inspirehep.net/authors/${iid}`
        : `https://inspirehep.net/authors?q=${encodeURIComponent(name)}`;
      return `<a href="${url}" target="_blank" rel="noopener">${escapeHtml(name)}</a>`;
    }).filter(Boolean);
  } else {
    let authStr = authors.replace(/,?\s*et\s+al\.?\s*$/i, '').trim();
    hadEtAl = /et\s+al/i.test(authors);
    const parts = authStr.split(',').map(s => s.trim()).filter(Boolean);
    // Pair "Last, First X., Last, First X." only when the full list reads
    // as clean last/first pairs: even length, even-indexed tokens look like
    // single-word surnames, odd-indexed tokens start with a capital.
    const pairable = parts.length >= 2 && parts.length % 2 === 0 &&
      parts.every((p, i) => (i % 2 === 0) ? /^[A-Z][A-Za-z\u00C0-\u017F\-']+$/.test(p)
                                          : /^[A-Z]/.test(p));
    let names;
    if (pairable) {
      names = [];
      for (let i = 0; i < parts.length; i += 2) names.push(parts[i + 1] + ' ' + parts[i]);
    } else {
      names = parts;
    }
    formatted = names.map(name => {
      const url = `https://inspirehep.net/authors?q=${encodeURIComponent(name)}`;
      return `<a href="${url}" target="_blank" rel="noopener">${escapeHtml(name)}</a>`;
    });
  }
  if (formatted.length === 0) return '';
  return formatted.length > 6
    ? formatted.slice(0, 6).join(', ') + ' et al.'
    : formatted.join(', ') + (hadEtAl ? ' et al.' : '');
}

// Plain-text variant of _renderAuthorsHtml for contexts that set
// textContent (no links), e.g. the matched-papers list rows. Normalizes
// both structured INSPIRE author arrays and freeform "Last, First X.,
// Last, First" strings into "First Last, First Last (et al.)".
function _renderAuthorsText(authors, max) {
  if (!authors) return '';
  max = max || 3;
  let names = [];
  if (Array.isArray(authors)) {
    names = authors
      .map(a => (typeof a === 'string') ? a : (a.display_name || a.full_name || ''))
      .map(n => String(n).trim())
      .filter(Boolean);
  } else if (typeof authors === 'string') {
    const cleaned = authors.replace(/,?\s*et\s+al\.?\s*$/i, '').trim();
    if (!cleaned) return '';
    const parts = cleaned.split(',').map(s => s.trim()).filter(Boolean);
    // Detect "Last, First X., Last, First X." pairing: even number of parts
    // where even-indexed tokens look like single-word surnames and
    // odd-indexed tokens start with a capital (given names or initials).
    const pairable = parts.length >= 2 && parts.length % 2 === 0 &&
      parts.every((p, i) => (i % 2 === 0) ? /^[A-Z][A-Za-z\u00C0-\u017F\-']+$/.test(p)
                                          : /^[A-Z]/.test(p));
    if (pairable) {
      for (let i = 0; i < parts.length; i += 2) {
        names.push(parts[i + 1] + ' ' + parts[i]);
      }
    } else {
      names = parts;
    }
  }
  if (names.length === 0) return '';
  if (names.length > max) return names.slice(0, max).join(', ') + ' et al.';
  return names.join(', ');
}

// Some older library entries store the Authors field as a raw Mathematica
// association (or list of them), e.g. <|"full_name" -> "Doe, J.", ...|>.
// Parse the full_name occurrences into a structured array so downstream
// rendering treats them like INSPIRE authors.
function _parseMmaAuthors(s) {
  if (typeof s !== 'string' || !s.includes('<|')) return null;
  const matches = [...s.matchAll(/"full_name"\s*->\s*"([^"]+)"/g)];
  if (matches.length === 0) return null;
  const iidMatches = [...s.matchAll(/"inspire_id"\s*->\s*"([^"]*)"/g)];
  return matches.map((m, i) => ({
    full_name: m[1],
    inspire_id: iidMatches[i] ? (iidMatches[i][1] || null) : null,
  }));
}

function _populatePdfRef(arxivId, rec, inReview) {
  const el = document.getElementById(inReview ? 'review-pdf-ref' : 'cfg-pdf-ref');
  if (!el) return;
  rec = rec || {};

  const ref = cleanMmaEscapes(rec.Reference || rec.reference || '');
  let authors = cleanMmaEscapes(rec.Authors || rec.authors || '');
  // Recover structured authors from MMA-association literals, and drop raw
  // MMA literals so the INSPIRE fallback can take over. Without this, the
  // record dump like <|"full_name" -> "Bargiela, Piotr", ...|> would render
  // as literal underlined text.
  if (typeof authors === 'string' && authors.includes('<|')) {
    const parsed = _parseMmaAuthors(authors);
    authors = parsed || '';
  }
  const desc = cleanMmaEscapes(rec.Description || rec.description || '');
  const epsOrd = rec.EpsOrders || rec.epsOrders || '';

  const pfx = inReview ? 'review' : 'cfg';
  let html = '';

  // Title placeholder (filled by INSPIRE)
  html += `<div class="cfg-pdf-ref-title" id="${pfx}-pdf-ref-title" style="display:none"></div>`;

  // Authors — reserve a container even when the record is silent so the
  // INSPIRE callback below can backfill without needing to reflow the block.
  const authorsHtml = _renderAuthorsHtml(authors);
  const hiddenAttr = authorsHtml ? '' : ' style="display:none"';
  html += `<div class="cfg-pdf-ref-authors" id="${pfx}-pdf-ref-authors"${hiddenAttr}>${authorsHtml}</div>`;

  // Reference line
  if (ref) html += `<div class="cfg-pdf-ref-cite">${linkifyRef(ref)}</div>`;

  // Badges
  const badges = [];
  if (epsOrd) badges.push(`<span class="badge badge-gold">\u03B5: ${epsOrd}</span>`);
  const ds = rec.dimScheme || rec.dim_scheme;
  if (ds) badges.push(`<span class="badge badge-muted">${escapeHtml(String(ds).replace(/\bCDR\b/g, 'd=4-2*eps').replace(/\bD=/g, 'd=').replace(/2eps\b/g, '2*eps'))}</span>`);
  if (badges.length > 0) html += `<div class="cfg-pdf-ref-badges">${badges.join(' ')}</div>`;

  // Description — routed through renderInlineMathString so inline $...$
  // spans are KaTeX-rendered. Text outside math is HTML-escaped internally.
  if (desc) html += `<div class="cfg-pdf-ref-desc">${renderInlineMathString(desc)}</div>`;

  el.innerHTML = html;

  // Enrich from INSPIRE: fill in the title, and backfill authors whenever
  // the record didn't supply them (or supplied a raw MMA association that
  // we couldn't parse). INSPIRE's structured author list carries inspire_id
  // so the generated links jump straight to each author's profile.
  const inspireId = extractArxivId(ref) || arxivId;
  if (inspireId) {
    fetchInspireData(inspireId, (data) => {
      const titleEl = document.getElementById(pfx + '-pdf-ref-title');
      if (data.title && titleEl) {
        titleEl.textContent = data.title;
        titleEl.style.display = '';
      }
      const authEl = document.getElementById(pfx + '-pdf-ref-authors');
      if (authEl && !authEl.innerHTML.trim() && Array.isArray(data.authors) && data.authors.length) {
        authEl.innerHTML = _renderAuthorsHtml(data.authors);
        authEl.style.display = '';
      }
    });
  }
}

// ───────────────────────────────────────────────────────────────────
// Matched-papers list shown in the Paper tab when no PDF is loaded.
// Walks currentMatches + currentSubtopoMatches, collects each config's
// references, dedupes by arXiv ID, and renders a clickable list. Each
// row loads the paper into the same viewer via openPdfPanel.
// ───────────────────────────────────────────────────────────────────
function _populateMatchedPapersList() {
  const empty = document.getElementById('cfg-pdf-empty');
  if (!empty) return;
  // If a PDF is currently loaded, don't overwrite the canvas view.
  if (_pdfDoc) { empty.style.display = 'none'; return; }

  // Build the dedup'd list of {arxivId, rec, topoKey, cfgKey, topoName}
  const seen = new Map();  // arxivId → entry
  const addFromMatch = (topoKey, topo) => {
    if (!topo || !topo.configs) return;
    const topoName = topo.primaryName || topo.name || topo.Name || topoKey;
    for (const ck in topo.configs) {
      const cfg = topo.configs[ck];
      const recs = cfg.Records || cfg.records || [];
      const refs = cfg.References || cfg.references || [];
      // Merge refs-only strings with records into a uniform shape
      const allSources = [
        ...recs.map(r => ({ rec: r, ref: r.Reference || r.reference || '' })),
        ...refs.map(r => ({ rec: null, ref: r })),
      ];
      for (const { rec, ref } of allSources) {
        if (!ref) continue;
        const aid = extractArxivId(ref);
        if (!aid || seen.has(aid)) continue;
        seen.set(aid, { arxivId: aid, rec, ref, topoKey, cfgKey: ck, topoName, cfg });
      }
    }
  };
  for (const m of (currentMatches || [])) addFromMatch(m.topoKey, m.topo);
  for (const m of (currentSubtopoMatches || [])) addFromMatch(m.topoKey, m.topo);

  const entries = Array.from(seen.values());

  if (entries.length === 0) {
    empty.style.display = '';
    empty.innerHTML = currentNickel
      ? `<div style="padding:18px;text-align:center;font-size:12px;color:var(--text-muted)">No library papers cite this topology yet.</div>`
      : `<div style="padding:18px;text-align:center;font-size:12px;color:var(--text-muted)">Draw a diagram to see related papers, or click a paper thumbnail elsewhere in the UI.</div>`;
    return;
  }

  empty.style.display = '';
  empty.innerHTML = '';
  const header = document.createElement('div');
  header.className = 'matched-papers-header';
  header.textContent = `${entries.length} paper${entries.length !== 1 ? 's' : ''} matching this topology`;
  empty.appendChild(header);

  const list = document.createElement('div');
  list.className = 'matched-papers-list';

  for (const entry of entries) {
    const row = document.createElement('div');
    row.className = 'matched-paper-row';
    row.dataset.arxivId = entry.arxivId;

    const title = document.createElement('div');
    title.className = 'matched-paper-title';
    // Will be filled by INSPIRE async — start with arxiv ID as placeholder
    title.textContent = entry.arxivId;
    row.appendChild(title);

    const meta = document.createElement('div');
    meta.className = 'matched-paper-meta';
    // Initial author line from the record (if any), then replaced by INSPIRE
    // below whenever cleaner structured data is available.
    let recAuthors = entry.rec && (entry.rec.Authors || entry.rec.authors);
    if (typeof recAuthors === 'string' && recAuthors.includes('<|')) {
      recAuthors = _parseMmaAuthors(recAuthors) || '';
    }
    const initialAuthorsText = _renderAuthorsText(recAuthors, 3);
    meta.textContent = initialAuthorsText || entry.topoName;
    row.appendChild(meta);

    const refLine = document.createElement('div');
    refLine.className = 'matched-paper-ref';
    refLine.textContent = entry.ref;
    row.appendChild(refLine);

    row.addEventListener('click', () => {
      // Open the PDF panel viewer for this paper; it reuses the same
      // _loadPdfIntoPanel code as clicking a library thumbnail.
      openPdfPanel(entry.arxivId, entry.rec, false);
    });

    list.appendChild(row);

    // Async enrich from INSPIRE: always overwrite the title with the
    // canonical title, and use the structured INSPIRE authors whenever
    // present — they parse cleanly where the record's freeform string
    // ("Last, First X., Last, First") breaks naive comma splits.
    fetchInspireData(entry.arxivId, (data) => {
      if (!data) return;
      if (data.title) title.textContent = data.title;
      if (Array.isArray(data.authors) && data.authors.length) {
        const txt = _renderAuthorsText(data.authors, 3);
        if (txt) meta.textContent = txt;
      }
    });
  }
  empty.appendChild(list);
}

function _populatePdfCoverage(arxivId) {
  const el = document.getElementById('cfg-pdf-coverage');
  if (!el) return;
  if (!arxivId || !library || !library.topologies) { el.innerHTML = ''; return; }

  // Find all configs citing this arXiv ID
  const entries = [];
  for (const tk in library.topologies) {
    const t = library.topologies[tk];
    const cfgs = t.configs || {};
    for (const ck in cfgs) {
      const cfg = cfgs[ck];
      const recs = cfg.Records || cfg.records || [];
      const refs = cfg.References || cfg.references || [];
      const allRefs = [...refs, ...recs.map(r => r.Reference || r.reference || '')];
      if (allRefs.some(r => reviewExtractArxivId(r) === arxivId)) {
        // Use the official diagram name when available: CanonicalName is the
        // per-config canonical name assigned by the diagram-name system;
        // fall back to the first Name[] entry, then the nickel key.
        const names = cfg.Names || cfg.names || [];
        const name = cfg.CanonicalName || cfg.canonicalName
          || (Array.isArray(names) ? names[0] : names) || ck;
        entries.push({ tk, ck, name, cfg, topo: t });
      }
    }
  }

  if (entries.length === 0) { el.innerHTML = ''; return; }

  el.innerHTML = `<div class="cfg-pdf-coverage-title">Diagrams in this paper (${entries.length})</div>`;
  const list = document.createElement('div');
  list.className = 'cfg-pdf-coverage-list';
  entries.forEach(ce => {
    const card = document.createElement('div');
    card.className = 'cfg-pdf-coverage-card';
    try {
      const thumb = generateThumbnail(ce.tk, ce.ck);
      thumb.style.cssText = 'width:36px;height:36px;flex-shrink:0;';
      card.appendChild(thumb);
    } catch (_) {}
    const label = document.createElement('span');
    label.className = 'cfg-pdf-coverage-label';
    label.textContent = ce.name;
    card.appendChild(label);

    // Hover tooltip with larger preview
    const tip = document.createElement('div');
    tip.className = 'cfg-pdf-coverage-tip';
    try {
      const bigThumb = generateThumbnail(ce.tk, ce.ck);
      bigThumb.style.cssText = 'width:80px;height:80px;flex-shrink:0;';
      tip.appendChild(bigThumb);
    } catch (_) {}
    const tipInfo = document.createElement('div');
    tipInfo.className = 'cfg-pdf-coverage-tip-info';
    tipInfo.innerHTML = `<div style="font-weight:600;font-size:11px;margin-bottom:2px">${escapeHtml(ce.name)}</div>`;
    const ms = ce.cfg.MassScales ?? ce.cfg.massScales;
    if (ms != null) tipInfo.innerHTML += `<div style="font-size:10px;color:var(--text-muted)">Mass scales: ${ms}</div>`;
    const fc = ce.cfg.FunctionClass || ce.cfg.functionClass;
    if (fc) tipInfo.innerHTML += `<div style="font-size:10px;color:var(--text-muted)">${escapeHtml(fc)}</div>`;
    tip.appendChild(tipInfo);
    card.appendChild(tip);

    card.addEventListener('click', () => {
      loadFromNickel(ce.tk, ce.ck);
      openDetailPanel(ce.tk, ce.topo, null, ce.ck, {});
      focusPanel('detail');
    });
    list.appendChild(card);
  });
  el.appendChild(list);
}

async function reviewLoadPdf(arxivId) {
  // Review-mode wrapper (preserves existing behavior)
  await openPdfPanel(arxivId);
}

// Returns element ID prefix based on current mode ('review' or 'cfg')
function _pdfPrefix() {
  return document.body.classList.contains('review-mode') ? 'review' : 'cfg';
}

async function _loadPdfIntoPanel(arxivId) {
  const pfx = _pdfPrefix();
  const pageInp = document.getElementById(pfx + '-pdf-page-input');
  const totalEl = document.getElementById(pfx + '-pdf-page-total');

  // If same paper already loaded into the same panel, keep the scroll position.
  // Paper cached in the wrong panel (e.g. mode switch) falls through to a reload.
  const host = document.getElementById(pfx + '-pdf-pages');
  const sameHost = host && host.children.length > 0;
  if (arxivId === _pdfArxivId && _pdfDoc && sameHost) {
    return;
  }
  _pdfTeardown();
  _pdfArxivId = arxivId;
  _pdfPage = 1;
  if (pageInp) pageInp.value = '\u2026';
  if (totalEl) totalEl.textContent = '\u2026';

  const skeleton = document.getElementById(pfx + '-pdf-skeleton');
  const pagesEl = document.getElementById(pfx + '-pdf-pages');
  if (skeleton) skeleton.style.display = '';
  if (pagesEl) pagesEl.style.display = 'none';

  const lib = await reviewEnsurePdfLib();
  if (!lib) {
    showWarningToast('PDF.js failed to load', true);
    if (skeleton) skeleton.style.display = 'none';
    return;
  }

  try {
    const url = getPdfUrl(arxivId);
    const loadingTask = lib.getDocument({ url, disableAutoFetch: false });
    _pdfDoc = await loadingTask.promise;
    if (skeleton) skeleton.style.display = 'none';
    if (pagesEl) pagesEl.style.display = '';
    await _pdfBuildPageStack();
    const savedPage = parseInt(localStorage.getItem('review-pdf-page-' + arxivId) || '1', 10);
    _pdfScrollToPage(Math.min(Math.max(1, savedPage), _pdfDoc.numPages), { smooth: false });
    _pdfUpdatePageIndicator();
  } catch (e) {
    showWarningToast('PDF load failed: ' + String(e).slice(0, 60), true);
    if (skeleton) skeleton.style.display = 'none';
    console.error('[pdf] Load error:', e);
  }
}

// Clear any state from a previously loaded PDF. Cancels in-flight render
// tasks, disconnects the IntersectionObserver, empties the pages container.
function _pdfTeardown() {
  if (_pdfObserver) { try { _pdfObserver.disconnect(); } catch {} _pdfObserver = null; }
  if (_pdfScrollRoot && _pdfScrollHandlerBound) {
    _pdfScrollRoot.removeEventListener('scroll', _pdfScrollHandlerBound);
  }
  _pdfScrollRoot = null;
  _pdfScrollHandlerBound = null;
  if (_pdfScrollRAF) { cancelAnimationFrame(_pdfScrollRAF); _pdfScrollRAF = 0; }
  for (const p of _pdfPages) {
    if (p.renderTask) { try { p.renderTask.cancel(); } catch {} }
  }
  _pdfPages = [];
  _pdfBaseDims = null;
  _pdfDoc = null;
  for (const pfx of ['review', 'cfg']) {
    const el = document.getElementById(pfx + '-pdf-pages');
    if (el) el.innerHTML = '';
  }
}

// Figure out the CSS width of the scroll viewport for the active panel.
// Used to convert fit-to-width into a scale factor.
function _pdfViewportWidth() {
  const pfx = _pdfPrefix();
  if (pfx === 'review') {
    const panelW = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--review-pdf-w'), 10) || 480;
    return Math.max(120, panelW - 20);
  }
  const body = document.getElementById('config-body');
  return body ? Math.max(120, body.clientWidth - 32) : 388;
}

// Render scale for one page — always fit-to-panel-width. Page dims come
// from pdf.js at scale=1 (72 dpi points).
function _pdfEffectiveScale(pageDims) {
  return _pdfViewportWidth() / pageDims.width;
}

// Build N page placeholders in the pages container. Each placeholder gets a
// canvas + text-layer div and a min-width/height so layout height equals the
// final scrolled document height. Rendering is lazy (IntersectionObserver).
async function _pdfBuildPageStack() {
  if (!_pdfDoc) return;
  const pfx = _pdfPrefix();
  const host = document.getElementById(pfx + '-pdf-pages');
  if (!host) return;
  host.innerHTML = '';

  // Grab page-1 dimensions as a proxy for the rest (most papers are uniform).
  // We do this once so placeholder sizing matches the eventual render.
  const firstPage = await _pdfDoc.getPage(1);
  const viewport1 = firstPage.getViewport({ scale: 1.0 });
  _pdfBaseDims = { width: viewport1.width, height: viewport1.height };

  const scale = _pdfEffectiveScale(_pdfBaseDims);
  const cssW = Math.floor(_pdfBaseDims.width * scale);
  const cssH = Math.floor(_pdfBaseDims.height * scale);

  _pdfPages = [];
  for (let i = 1; i <= _pdfDoc.numPages; i++) {
    const wrap = document.createElement('div');
    wrap.className = 'pdf-page pending';
    wrap.dataset.pageNum = String(i);
    wrap.style.width = cssW + 'px';
    wrap.style.height = cssH + 'px';
    const canvas = document.createElement('canvas');
    const tl = document.createElement('div');
    tl.className = 'textLayer';
    tl.style.width = cssW + 'px';
    tl.style.height = cssH + 'px';
    wrap.appendChild(canvas);
    wrap.appendChild(tl);
    host.appendChild(wrap);
    _pdfPages.push({
      num: i,
      container: wrap,
      canvas,
      textLayer: tl,
      rendered: false,
      scale: 0,
      renderTask: null,
    });
  }

  // Lazy-render observer: fires with 400px pre-load rootMargin so pages are
  // rendered just before they scroll into view. This observer is NOT used
  // for current-page tracking — intersection callbacks only report pages
  // that crossed a threshold in this tick, not every currently-visible
  // page, which would cause the indicator to flip to stale values.
  const rootEl = (pfx === 'review')
    ? document.getElementById('review-pdf-viewport')
    : document.getElementById('config-body');
  _pdfObserver = new IntersectionObserver((entries) => {
    for (const e of entries) {
      if (e.isIntersecting) {
        const n = parseInt(e.target.dataset.pageNum, 10);
        _pdfRenderPage(n).catch(err => console.error('[pdf] render error p=' + n, err));
      }
    }
  }, { root: rootEl, rootMargin: '400px 0px', threshold: [0, 0.25] });
  for (const p of _pdfPages) _pdfObserver.observe(p.container);

  // Current-page tracking: scroll-driven. Measures every page's bounding
  // rect against the real viewport and picks whichever has the largest
  // intersection area. Robust under fast scrolling and button spam (where
  // the observer-based approach was racy: successive _pdfScrollToPage
  // calls would fire mid-scroll observer callbacks that overwrote _pdfPage
  // with intermediate values).
  _pdfAttachScrollTracker(rootEl);
}

let _pdfScrollRAF = 0;
let _pdfScrollRoot = null;
let _pdfScrollHandlerBound = null;
function _pdfAttachScrollTracker(rootEl) {
  if (_pdfScrollRoot && _pdfScrollHandlerBound) {
    _pdfScrollRoot.removeEventListener('scroll', _pdfScrollHandlerBound);
  }
  _pdfScrollRoot = rootEl;
  _pdfScrollHandlerBound = () => {
    if (_pdfScrollRAF) return;
    _pdfScrollRAF = requestAnimationFrame(() => {
      _pdfScrollRAF = 0;
      _pdfRecomputeCurrentPage();
    });
  };
  rootEl.addEventListener('scroll', _pdfScrollHandlerBound, { passive: true });
  _pdfRecomputeCurrentPage();
}

function _pdfRecomputeCurrentPage() {
  if (!_pdfDoc || !_pdfPages.length || !_pdfScrollRoot) return;
  const root = _pdfScrollRoot.getBoundingClientRect();
  let bestPage = _pdfPages[0].num, bestOverlap = -1;
  for (const p of _pdfPages) {
    const r = p.container.getBoundingClientRect();
    const overlap = Math.max(0, Math.min(r.bottom, root.bottom) - Math.max(r.top, root.top));
    if (overlap > bestOverlap) { bestOverlap = overlap; bestPage = p.num; }
  }
  if (bestOverlap <= 0) return;  // no page in viewport (e.g. during transition)
  if (bestPage !== _pdfPage) {
    _pdfPage = bestPage;
    _pdfUpdatePageIndicator();
    if (_pdfArxivId) localStorage.setItem('review-pdf-page-' + _pdfArxivId, String(_pdfPage));
  }
}

// Render (or re-render) a single page's canvas + text layer at the current
// effective scale. Idempotent when scale hasn't changed. Cancels any prior
// in-flight render task on the same page.
async function _pdfRenderPage(pageNum) {
  if (!_pdfDoc) return;
  const entry = _pdfPages[pageNum - 1];
  if (!entry) return;
  const page = await _pdfDoc.getPage(pageNum);
  const pageDims = { width: page.view[2] - page.view[0], height: page.view[3] - page.view[1] };
  const scale = _pdfEffectiveScale(pageDims);
  if (entry.rendered && Math.abs(entry.scale - scale) < 1e-4) return;
  // Cancel any in-flight render at the old scale.
  if (entry.renderTask) { try { entry.renderTask.cancel(); } catch {} entry.renderTask = null; }

  const viewport = page.getViewport({ scale });
  const dpr = window.devicePixelRatio || 1;
  const cssW = Math.floor(viewport.width);
  const cssH = Math.floor(viewport.height);
  entry.container.style.width = cssW + 'px';
  entry.container.style.height = cssH + 'px';
  entry.canvas.width = Math.floor(viewport.width * dpr);
  entry.canvas.height = Math.floor(viewport.height * dpr);
  entry.canvas.style.width = cssW + 'px';
  entry.canvas.style.height = cssH + 'px';
  entry.textLayer.style.width = cssW + 'px';
  entry.textLayer.style.height = cssH + 'px';
  entry.textLayer.innerHTML = '';

  const ctx = entry.canvas.getContext('2d', { alpha: false });
  const transform = dpr !== 1 ? [dpr, 0, 0, dpr, 0, 0] : null;
  entry.renderTask = page.render({ canvasContext: ctx, viewport, transform });
  try {
    await entry.renderTask.promise;
  } catch (e) {
    // RenderingCancelledException when we intentionally cancel — ignore.
    if (e && e.name === 'RenderingCancelledException') return;
    throw e;
  }
  entry.renderTask = null;
  entry.rendered = true;
  entry.scale = scale;
  entry.container.classList.remove('pending');

  // Text layer — enables selection and Cmd+F. Safe to fail open.
  try {
    const textContent = await page.getTextContent();
    // pdf.js 4.x API: renderTextLayer or TextLayer class.
    if (_pdfLib && typeof _pdfLib.renderTextLayer === 'function') {
      _pdfLib.renderTextLayer({
        textContentSource: textContent,
        container: entry.textLayer,
        viewport,
      });
    } else if (_pdfLib && _pdfLib.TextLayer) {
      const tl = new _pdfLib.TextLayer({
        textContentSource: textContent,
        container: entry.textLayer,
        viewport,
      });
      await tl.render();
    }
  } catch (e) {
    // Text layer is a nice-to-have — don't break the canvas render on failure.
    console.warn('[pdf] text layer error p=' + pageNum, e);
  }
}

// Invalidate all pages (mark them as needing re-render at the new scale)
// and resize their placeholders so the scroll height matches the new total.
// Pages that are currently visible re-render immediately via the observer;
// the rest pick it up when they scroll into view.
function _pdfRelayoutAll() {
  if (!_pdfDoc || _pdfPages.length === 0 || !_pdfBaseDims) return;
  const scale = _pdfEffectiveScale(_pdfBaseDims);
  const cssW = Math.floor(_pdfBaseDims.width * scale);
  const cssH = Math.floor(_pdfBaseDims.height * scale);
  for (const p of _pdfPages) {
    p.container.style.width = cssW + 'px';
    p.container.style.height = cssH + 'px';
    p.rendered = false;
    p.scale = 0;
    if (p.renderTask) { try { p.renderTask.cancel(); } catch {} p.renderTask = null; }
    p.container.classList.add('pending');
  }
  // Kick the observer to re-fire for currently-visible pages.
  if (_pdfObserver) {
    for (const p of _pdfPages) {
      _pdfObserver.unobserve(p.container);
      _pdfObserver.observe(p.container);
    }
  }
}

function _pdfUpdatePageIndicator() {
  if (!_pdfDoc) return;
  const pfx = _pdfPrefix();
  const inp = document.getElementById(pfx + '-pdf-page-input');
  const total = document.getElementById(pfx + '-pdf-page-total');
  if (inp && document.activeElement !== inp) {
    inp.value = String(_pdfPage);
    inp.size = Math.max(2, String(_pdfDoc.numPages).length);
  }
  if (total) total.textContent = String(_pdfDoc.numPages);
  // Disable prev/next at the ends for visual feedback.
  const prev = document.getElementById(pfx + '-pdf-prev');
  const next = document.getElementById(pfx + '-pdf-next');
  if (prev) prev.disabled = (_pdfPage <= 1);
  if (next) next.disabled = (_pdfPage >= _pdfDoc.numPages);
}

function _pdfScrollToPage(n, opts) {
  if (!_pdfDoc) return;
  const p = Math.min(Math.max(1, n), _pdfDoc.numPages);
  const entry = _pdfPages[p - 1];
  if (!entry) return;
  entry.container.scrollIntoView({
    behavior: opts && opts.smooth === false ? 'auto' : 'smooth',
    block: 'start',
  });
  _pdfPage = p;
  _pdfUpdatePageIndicator();
}

function reviewPdfPrev() {
  if (!_pdfDoc || _pdfPage <= 1) return;
  _pdfScrollToPage(_pdfPage - 1);
}

function reviewPdfNext() {
  if (!_pdfDoc || _pdfPage >= _pdfDoc.numPages) return;
  _pdfScrollToPage(_pdfPage + 1);
}

function reviewPdfGoto(n) {
  if (!_pdfDoc) return;
  _pdfScrollToPage(n);
}

function _pdfOpenInNewTab() {
  if (!_pdfArxivId) { showWarningToast('No paper loaded', true); return; }
  // arxiv.org abstract page gives the user both abstract + "Download PDF" link
  // and a persistent URL worth sharing. Prefer this over the proxied blob URL.
  const url = 'https://arxiv.org/abs/' + encodeURIComponent(_pdfArxivId);
  window.open(url, '_blank', 'noopener');
}

// Fetch a BibTeX entry for the current arXiv ID from INSPIRE and copy it to
// the clipboard. Results are cached per-arxivId for the session.
async function _pdfCopyBibtex() {
  if (!_pdfArxivId) { showWarningToast('No paper loaded', true); return; }
  const aid = _pdfArxivId;
  try {
    let bib = _pdfBibtexCache[aid];
    if (!bib) {
      const url = `https://inspirehep.net/api/literature?q=eprint+${encodeURIComponent(aid)}&format=bibtex`;
      const r = await fetch(url);
      if (!r.ok) throw new Error('HTTP ' + r.status);
      bib = (await r.text()).trim();
      if (!bib || !bib.startsWith('@')) throw new Error('No BibTeX returned');
      _pdfBibtexCache[aid] = bib;
    }
    await navigator.clipboard.writeText(bib);
    showWarningToast('BibTeX copied to clipboard', false);
  } catch (e) {
    console.error('[pdf] bibtex error:', e);
    showWarningToast('BibTeX fetch failed: ' + String(e.message || e).slice(0, 60), true);
  }
}

function reviewPdfClose() {
  const panel = document.getElementById('review-pdf-panel');
  if (panel) panel.style.display = 'none';
  document.body.classList.remove('review-has-pdf');
}

// Back from the PDF viewer to the list of papers citing the current diagram.
// Tears down the PDF, hides the paper-specific chrome (toolbar, ref, coverage,
// back button), and re-renders the matched-papers list into the empty-state
// container — leaving the user where they were before they clicked a paper.
function _pdfBackToMatchedList() {
  _pdfTeardown();
  _pdfArxivId = null;
  const nav = document.getElementById('cfg-pdf-nav');
  if (nav) nav.style.display = 'none';
  const pages = document.getElementById('cfg-pdf-pages');
  if (pages) pages.style.display = 'none';
  const ref = document.getElementById('cfg-pdf-ref');
  if (ref) ref.innerHTML = '';
  const cov = document.getElementById('cfg-pdf-coverage');
  if (cov) cov.innerHTML = '';
  const back = document.getElementById('cfg-pdf-back');
  if (back) back.style.display = 'none';
  const empty = document.getElementById('cfg-pdf-empty');
  if (empty) empty.style.display = '';
  _populateMatchedPapersList();
}

function reviewSetupPdfPanel() {
  // Wire the same set of toolbar controls to both the review-mode panel and
  // the config-panel "PDF preview" tab. Handlers are shared — the active
  // panel is selected at call time by _pdfPrefix().
  const wireControls = (pfx) => {
    document.getElementById(pfx + '-pdf-prev')?.addEventListener('click', reviewPdfPrev);
    document.getElementById(pfx + '-pdf-next')?.addEventListener('click', reviewPdfNext);
    document.getElementById(pfx + '-pdf-open')?.addEventListener('click', _pdfOpenInNewTab);
    document.getElementById(pfx + '-pdf-bibtex')?.addEventListener('click', _pdfCopyBibtex);
  };
  wireControls('review');
  wireControls('cfg');
  document.getElementById('review-pdf-close')?.addEventListener('click', reviewPdfClose);
  document.getElementById('cfg-pdf-back')?.addEventListener('click', _pdfBackToMatchedList);

  // Inline-editable page number inside the "N / M" counter — Enter
  // commits the jump, Escape reverts, blur also commits.
  const wireInlinePage = (inputId) => {
    const inp = document.getElementById(inputId);
    if (!inp) return;
    const commit = () => {
      const n = parseInt(inp.value, 10);
      if (!isNaN(n)) reviewPdfGoto(n);
      if (_pdfDoc) inp.value = String(_pdfPage);
    };
    inp.addEventListener('focus', () => inp.select());
    inp.addEventListener('keydown', (e) => {
      if (e.key === 'Enter') { e.preventDefault(); commit(); inp.blur(); }
      else if (e.key === 'Escape') {
        if (_pdfDoc) inp.value = String(_pdfPage);
        inp.blur();
      }
    });
    inp.addEventListener('blur', commit);
  };
  wireInlinePage('review-pdf-page-input');
  wireInlinePage('cfg-pdf-page-input');

  // Restore saved width
  const savedW = parseInt(localStorage.getItem('review-pdf-width') || '0', 10);
  if (savedW >= 200) document.documentElement.style.setProperty('--review-pdf-w', savedW + 'px');

  // Drag-to-resize handle — re-layout pages at the new viewport width.
  const handle = document.getElementById('review-pdf-resize');
  if (handle) {
    let startX = 0, startW = 0;
    const onMove = (e) => {
      const w = Math.max(200, Math.min(window.innerWidth - 500, startW + (e.clientX - startX)));
      document.documentElement.style.setProperty('--review-pdf-w', w + 'px');
      // rAF-throttle relayout so drag stays smooth on large papers.
      if (!_pdfResizeRAF) {
        _pdfResizeRAF = requestAnimationFrame(() => {
          _pdfResizeRAF = 0;
          if (_pdfDoc && _pdfPrefix() === 'review') _pdfRelayoutAll();
        });
      }
    };
    const onUp = () => {
      handle.classList.remove('dragging');
      document.removeEventListener('mousemove', onMove);
      document.removeEventListener('mouseup', onUp);
      const w = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--review-pdf-w'), 10);
      if (w >= 200) localStorage.setItem('review-pdf-width', String(w));
      if (_pdfDoc) _pdfRelayoutAll();
    };
    handle.addEventListener('mousedown', (e) => {
      e.preventDefault();
      handle.classList.add('dragging');
      startX = e.clientX;
      startW = parseInt(getComputedStyle(document.documentElement).getPropertyValue('--review-pdf-w'), 10) || 480;
      document.addEventListener('mousemove', onMove);
      document.addEventListener('mouseup', onUp);
    });
  }

  // Keep fit-width in sync when the window or config panel changes size.
  window.addEventListener('resize', () => {
    if (!_pdfDoc) return;
    if (!_pdfResizeRAF) {
      _pdfResizeRAF = requestAnimationFrame(() => {
        _pdfResizeRAF = 0;
        _pdfRelayoutAll();
      });
    }
  });
}


// ── Persistent right-side review panel ──

async function reviewPopulateSidePanel(item) {
  if (!reviewIsActive()) return;
  const panel = document.getElementById('review-side-panel');
  if (!panel) return;
  panel.style.display = 'block';
  const nameEl = document.getElementById('review-side-name');
  const nickelEl = document.getElementById('review-side-nickel');
  const badgesEl = document.getElementById('review-side-badges');
  const body = document.getElementById('review-side-body');

  const primaryName = (item.names && item.names[0]) || item.cNickel || '?';
  if (nameEl) nameEl.textContent = primaryName;
  if (nickelEl) nickelEl.textContent = item.cNickel || '';
  // Build the name collision index lazily from the library
  if (!_reviewNameIndex && library && library.topologies) {
    const idx = {};
    for (const tk in library.topologies) {
      const cfgs = library.topologies[tk].configs || {};
      for (const ck in cfgs) {
        const names = cfgs[ck].Names || cfgs[ck].names || [];
        const arr = Array.isArray(names) ? names : [names];
        for (const n of arr) {
          if (!n) continue;
          const key = String(n).toLowerCase().trim();
          if (!idx[key]) idx[key] = { count: 0, entries: [] };
          idx[key].count++;
          idx[key].entries.push(tk + ':' + ck);
        }
      }
    }
    _reviewNameIndex = idx;
  }
  // Check for name collision
  let nameCollisionBadge = '';
  if (_reviewNameIndex && primaryName) {
    const key = primaryName.toLowerCase().trim();
    const info = _reviewNameIndex[key];
    if (info && info.count > 1) {
      nameCollisionBadge = `<span class="badge" style="background:var(--gold-bg,#fef3cd);color:var(--gold,#856404);border:1px solid #f0d77c" title="${info.count} entries share this name: ${info.entries.slice(0, 5).join(', ')}${info.count > 5 ? '...' : ''}">\u26A0 Name collision (${info.count})</span>`;
    }
  }
  if (badgesEl) {
    badgesEl.innerHTML =
      `<span class="badge badge-muted">${item.loops ?? '?'} loop${item.loops !== 1 ? 's' : ''}</span>` +
      `<span class="badge badge-muted">${item.legs ?? '?'} leg${item.legs !== 1 ? 's' : ''}</span>` +
      `<span class="badge badge-muted">${item.props ?? '?'} prop${item.props !== 1 ? 's' : ''}</span>` +
      (item.massScales != null ? `<span class="badge badge-accent">${item.massScales} scale${item.massScales !== 1 ? 's' : ''}</span>` : '') +
      (item.functionClass && item.functionClass !== 'unknown' ? functionBadge(item.functionClass) : '') +
      nameCollisionBadge;
  }

  body.innerHTML = '<div class="review-side-empty">Loading\u2026</div>';
  const resp = await reviewKernel.getEntry(item.path);
  if (resp.status !== 'ok' || !resp.entry) {
    body.innerHTML = '<div class="review-side-empty">Failed to load entry</div>';
    return;
  }
  const entry = resp.entry;
  body.innerHTML = '';

  // Metadata (editable fields for FunctionClass, EpsilonOrder, Names; read-only for rest)
  const metaSec = document.createElement('div');
  metaSec.className = 'review-side-section';
  metaSec.innerHTML = '<div class="review-side-section-title">Metadata</div>';

  // Helper: create an editable key-value row. Click the value to edit; blur/Enter saves.
  function editableKV(key, value, onSave) {
    const row = document.createElement('div');
    row.className = 'review-side-kv';
    const keyEl = document.createElement('span');
    keyEl.className = 'review-side-kv-key';
    keyEl.textContent = key;
    const valEl = document.createElement('span');
    valEl.className = 'review-side-kv-val review-editable';
    valEl.textContent = value || '(empty)';
    valEl.title = 'Click to edit';
    valEl.style.cursor = 'pointer';
    if (!value) valEl.style.fontStyle = 'italic';
    valEl.addEventListener('click', () => {
      const input = document.createElement('textarea');
      input.value = value || '';
      input.rows = 1;
      input.style.cssText = 'width:100%;font-size:11px;padding:2px 4px;border:1px solid var(--accent);border-radius:3px;background:var(--surface);color:var(--text);font-family:inherit;line-height:1.45;resize:vertical;overflow:hidden;min-height:22px;';
      const autoResize = () => { input.style.height = 'auto'; input.style.height = input.scrollHeight + 'px'; };
      const finish = () => {
        const newVal = input.value.trim();
        if (newVal !== (value || '')) {
          value = newVal;
          onSave(newVal);
        }
        valEl.textContent = value || '(empty)';
        valEl.style.fontStyle = value ? '' : 'italic';
      };
      input.addEventListener('blur', finish);
      input.addEventListener('input', autoResize);
      input.addEventListener('keydown', (e) => {
        if (e.key === 'Escape') { input.value = value || ''; input.blur(); }
      });
      valEl.textContent = '';
      valEl.appendChild(input);
      input.focus();
      autoResize();
      input.select();
    });
    row.appendChild(keyEl);
    row.appendChild(valEl);
    return row;
  }

  function readOnlyKV(key, value) {
    const row = document.createElement('div');
    row.className = 'review-side-kv';
    row.innerHTML = `<span class="review-side-kv-key">${escapeHtml(key)}</span><span class="review-side-kv-val">${escapeHtml(value)}</span>`;
    return row;
  }

  // Save helper: patches the entry on disk via save_entry
  async function saveField(fieldName, newVal) {
    entry[fieldName] = newVal;
    const result = await reviewKernel.saveEntry(item.path, entry);
    if (result.status !== 'ok') {
      showWarningToast('Save failed: ' + (result.error || ''), true);
    }
  }

  // FunctionClass — dropdown with known values + "other" for custom input
  const FC_OPTIONS = ['MPL', 'elliptic', 'Calabi-Yau', 'hypergeometric', 'mixed', 'rational', 'unknown'];
  {
    const row = document.createElement('div');
    row.className = 'review-side-kv';
    const keyEl = document.createElement('span');
    keyEl.className = 'review-side-kv-key';
    keyEl.textContent = 'FunctionClass';
    const valWrap = document.createElement('span');
    valWrap.className = 'review-side-kv-val';
    const sel = document.createElement('select');
    sel.style.cssText = 'font-size:11px;padding:1px 4px;border:1px solid var(--border);border-radius:3px;background:var(--surface);color:var(--text);cursor:pointer;';
    const curFC = entry.FunctionClass || 'unknown';
    const allOpts = FC_OPTIONS.includes(curFC) ? FC_OPTIONS : [curFC, ...FC_OPTIONS];
    allOpts.forEach(opt => {
      const o = document.createElement('option');
      o.value = opt;
      o.textContent = opt;
      if (opt === curFC) o.selected = true;
      sel.appendChild(o);
    });
    sel.addEventListener('change', () => saveField('FunctionClass', sel.value));
    valWrap.appendChild(sel);
    row.appendChild(keyEl);
    row.appendChild(valWrap);
    metaSec.appendChild(row);
  }
  metaSec.appendChild(editableKV('EpsilonOrder', entry.EpsilonOrder || '', v => saveField('EpsilonOrder', v)));
  metaSec.appendChild(readOnlyKV('MassScales', String(entry.MassScales ?? '')));
  metaSec.appendChild(editableKV('Names', (entry.Names || []).join(', '), v => saveField('Names', v.split(',').map(s => s.trim()).filter(Boolean))));
  const srcVal = Array.isArray(entry.Source) ? entry.Source.join(', ') : String(entry.Source || '');
  if (srcVal) metaSec.appendChild(readOnlyKV('Source', srcVal));

  // Name suggestion (loaded lazily from /api/review/name_suggestions)
  (async () => {
    if (!_reviewNameSuggestions) {
      try {
        const resp = await fetch('/api/review/name_suggestions');
        _reviewNameSuggestions = await resp.json();
      } catch (_) { _reviewNameSuggestions = {}; }
    }
    // Look up by topology Nickel (part before ':' in CNickelIndex)
    const topoNickel = (entry.CNickelIndex || '').split(':')[0];
    const suggestion = _reviewNameSuggestions[topoNickel];
    if (suggestion && suggestion.primary) {
      const currentNames = (entry.Names || []).map(n => n.toLowerCase().trim());
      const sugPrimary = suggestion.primary;
      const sugAlts = suggestion.alternatives || [];
      const allSuggestions = [sugPrimary, ...sugAlts];
      // Only show if the primary suggestion isn't already in the entry's names
      if (!currentNames.includes(sugPrimary.toLowerCase().trim())) {
        const sugRow = document.createElement('div');
        sugRow.className = 'review-side-kv';
        sugRow.style.cssText = 'background:var(--gold-bg,#fef3cd);border-radius:4px;padding:4px 6px;margin-top:4px;';
        const sugKey = document.createElement('span');
        sugKey.className = 'review-side-kv-key';
        sugKey.style.color = 'var(--gold,#856404)';
        sugKey.textContent = 'Suggested';
        const sugVal = document.createElement('span');
        sugVal.className = 'review-side-kv-val';
        sugVal.style.display = 'flex';
        sugVal.style.alignItems = 'center';
        sugVal.style.gap = '6px';
        sugVal.style.flexWrap = 'wrap';
        const sugText = document.createElement('span');
        sugText.textContent = allSuggestions.join(', ');
        sugText.style.fontWeight = '600';
        sugText.style.color = 'var(--gold,#856404)';
        const acceptBtn = document.createElement('button');
        acceptBtn.textContent = 'Accept';
        acceptBtn.style.cssText = 'font-size:10px;padding:1px 8px;border-radius:3px;border:1px solid var(--gold,#856404);background:var(--gold,#856404);color:#fff;cursor:pointer;font-weight:600;';
        acceptBtn.addEventListener('click', async () => {
          // Prepend the suggested primary name; add alternatives if not present
          const names = entry.Names || [];
          const lower = names.map(n => n.toLowerCase().trim());
          const toAdd = allSuggestions.filter(s => !lower.includes(s.toLowerCase().trim()));
          entry.Names = [...toAdd, ...names];
          const result = await reviewKernel.saveEntry(item.path, entry);
          if (result.status !== 'ok') {
            showWarningToast('Save failed: ' + (result.error || ''), true);
            return;
          }
          sugRow.remove();
          // Refresh the Names display
          const namesRow = metaSec.querySelector('.review-editable');
          if (namesRow) namesRow.textContent = entry.Names.join(', ');
        });
        sugVal.appendChild(sugText);
        sugVal.appendChild(acceptBtn);
        sugRow.appendChild(sugKey);
        sugRow.appendChild(sugVal);
        metaSec.appendChild(sugRow);
      }
    }
  })();

  body.appendChild(metaSec);

  // Records
  const recsSec = document.createElement('div');
  recsSec.className = 'review-side-section';
  recsSec.innerHTML = '<div class="review-side-section-title">References &amp; Records</div>';
  const records = entry.Records || [];
  if (records.length === 0) {
    recsSec.innerHTML += '<div class="review-side-empty">No records</div>';
  }
  records.forEach((rec, idx) => {
    const card = document.createElement('div');
    card.className = 'review-side-record';
    if (idx === (item.recordIndex || 0)) card.classList.add('review-record-focused');

    const ref = rec.reference || '';
    const authors = typeof rec.authors === 'string' ? rec.authors
      : Array.isArray(rec.authors) ? rec.authors.map(a => a.full_name || a).join(', ')
      : '';
    const desc = rec.description || '';
    const epsOrd = rec.epsOrders || '';

    let html = '';
    if (ref) html += `<div class="review-side-record-ref">${linkifyRef(ref)}</div>`;
    if (authors) {
      const short = authors.length > 120 ? authors.slice(0, 117) + '\u2026' : authors;
      html += `<div class="review-side-record-authors">${escapeHtml(short)}</div>`;
    }
    card.innerHTML = html;

    // Generic helpers to add editable rows to the card
    const DROPDOWN_FIELDS = {
      computationLevel: ['full', 'analytic', 'DE_only', 'numerical', 'IBP_only', 'basis', 'C-matrix', 'maximal_cut', 'integrand_basis', 'canonical_DE', 'partial_cut', 'reduction', 'IBP reduction', 'master_integrals', 'none'],
      ancillaryFiles: ['true', 'false'],
      dimScheme: ['d=4-2*eps', 'd=D', 'd=4-2eps', 'd=2-2*eps', 'd=6-2*eps', 'd=3-2*eps', 'd=2w', 'd=D-2*eps', 'd=4', 'd=D=4-2*eps'],
      transcendentalWeight: ['1', '2', '3', '4', '5', '6', '7', '8', '9', '12'],
    };
    function addRecDropdown(label, fieldKey, fallbackKey) {
      const curVal = String(rec[fieldKey] ?? rec[fallbackKey] ?? '');
      const row = document.createElement('div');
      row.className = 'review-side-kv';
      row.innerHTML = `<span class="review-side-kv-key">${escapeHtml(label)}</span>`;
      const sel = document.createElement('select');
      sel.style.cssText = 'font-size:10px;padding:1px 3px;border:1px solid var(--border);border-radius:3px;background:var(--surface);color:var(--text);cursor:pointer;max-width:180px;';
      const opts = DROPDOWN_FIELDS[fieldKey] || [];
      const allOpts = (curVal && !opts.includes(curVal)) ? [curVal, ...opts] : ['', ...opts];
      allOpts.forEach(o => {
        const opt = document.createElement('option');
        opt.value = o; opt.textContent = o || '(none)';
        if (o === curVal || (!curVal && o === '')) opt.selected = true;
        sel.appendChild(opt);
      });
      sel.addEventListener('change', async () => {
        let v = sel.value;
        if (fieldKey === 'ancillaryFiles') v = v === 'true';
        else if (fieldKey === 'transcendentalWeight' || fieldKey === 'masterCount') v = v ? parseInt(v, 10) : null;
        rec[fieldKey] = v;
        entry.Records[idx] = rec;
        await reviewKernel.saveEntry(item.path, entry);
      });
      row.appendChild(sel);
      card.appendChild(row);
    }
    function addRecEditable(label, fieldKey, fallbackKey, isTextarea) {
      const curVal = String(rec[fieldKey] ?? rec[fallbackKey] ?? '');
      const row = document.createElement('div');
      row.className = 'review-side-kv';
      row.style.cursor = 'pointer';
      row.title = 'Click to edit';
      row.innerHTML = `<span class="review-side-kv-key">${escapeHtml(label)}</span>` +
        `<span class="review-side-kv-val">${curVal ? escapeHtml(curVal.length > 80 && !isTextarea ? curVal.slice(0, 77) + '\u2026' : curVal) : '<em style="color:var(--text-muted)">(empty)</em>'}</span>`;
      const valEl = row.querySelector('.review-side-kv-val');
      valEl.addEventListener('click', () => {
        // Always use textarea so long text is fully visible; auto-resize to content
        const el = document.createElement('textarea');
        el.rows = 1;
        el.value = String(rec[fieldKey] ?? rec[fallbackKey] ?? '');
        el.style.cssText = 'width:100%;font-size:11px;padding:2px 4px;border:1px solid var(--accent);border-radius:3px;background:var(--surface);color:var(--text);font-family:inherit;line-height:1.45;resize:vertical;overflow:hidden;min-height:22px;';
        const autoResize = () => { el.style.height = 'auto'; el.style.height = el.scrollHeight + 'px'; };
        const finish = async () => {
          const nv = el.value.trim();
          if (nv !== String(rec[fieldKey] ?? '')) {
            rec[fieldKey] = nv;
            entry.Records[idx] = rec;
            await reviewKernel.saveEntry(item.path, entry);
          }
          valEl.innerHTML = rec[fieldKey] ? escapeHtml(String(rec[fieldKey])) : '<em style="color:var(--text-muted)">(empty)</em>';
        };
        el.addEventListener('blur', finish);
        el.addEventListener('input', autoResize);
        el.addEventListener('keydown', (e) => {
          if (e.key === 'Escape') { el.value = String(rec[fieldKey] ?? ''); el.blur(); }
        });
        valEl.innerHTML = '';
        valEl.appendChild(el);
        el.focus();
        autoResize();
        if (!isTextarea) el.select();
      });
      card.appendChild(row);
    }
    function addRecReadonly(label, value) {
      if (!value) return;
      const display = typeof value === 'object' ? JSON.stringify(value).slice(0, 120) : String(value);
      const row = document.createElement('div');
      row.className = 'review-side-kv';
      row.innerHTML = `<span class="review-side-kv-key">${escapeHtml(label)}</span><span class="review-side-kv-val" style="font-size:10px">${escapeHtml(display)}</span>`;
      card.appendChild(row);
    }

    // All fields, in a sensible order
    addRecEditable('epsOrders', 'epsOrders', '', false);
    addRecEditable('location', 'location', '', false);
    addRecDropdown('computationLevel', 'computationLevel', 'computation_level');
    addRecEditable('dimScheme', 'dimScheme', 'dim_scheme', false);
    addRecDropdown('transcWeight', 'transcendentalWeight', 'transcendentalWeight');
    addRecEditable('familyName', 'familyName', '', false);
    addRecEditable('method', 'method', '', false);
    addRecEditable('masterCount', 'masterCount', 'master_integral_count', false);
    addRecDropdown('ancillaryFiles', 'ancillaryFiles', 'ancillaryFiles');
    addRecEditable('doi', 'doi', '', false);
    addRecEditable('texkey', 'texkey', '', false);
    // Complex fields — read-only display
    addRecReadonly('tools', rec.tools);
    addRecReadonly('relatedPapers', rec.relatedPapers || rec.related_papers);
    addRecReadonly('ancillaryPaths', rec.ancillaryPaths);
    // Description last (takes up more space)
    addRecEditable('description', 'description', '', true);

    // Verify + Remove controls
    const recordId = rec.recordId || '';
    if (recordId) {
      const controls = document.createElement('div');
      controls.className = 'review-record-controls';
      const verifyBtn = document.createElement('button');
      verifyBtn.className = 'review-verify-btn' + (rec.verified ? ' verified' : '');
      verifyBtn.innerHTML = '<span class="check-icon">' +
        (rec.verified ? '\u2713' : '') + '</span>' +
        (rec.verified ? 'Reference verified' : 'Mark verified');
      verifyBtn.addEventListener('click', async () => {
        const newVal = !rec.verified;
        const result = await reviewKernel.verifyPair(item.path, recordId, newVal);
        if (result.status !== 'ok') {
          showWarningToast('Verify failed: ' + (result.error || ''), true);
          return;
        }
        rec.verified = newVal;
        rec.verifiedAt = newVal ? new Date().toISOString().slice(0, 10) : '';
        verifyBtn.classList.toggle('verified', newVal);
        verifyBtn.innerHTML = '<span class="check-icon">' +
          (newVal ? '\u2713' : '') + '</span>' +
          (newVal ? 'Reference verified' : 'Mark verified');
        // The backend removes the config from ui/waitlist.json on verify-true;
        // sync the client-side set so the library browser drops the blue
        // highlight next time it rebuilds, and so a page reload won't resurrect
        // the waitlisted state incorrectly.
        if (result.promoted) {
          const path = String(item.path || '');
          const segs = path.split('/');
          if (segs.length >= 2) {
            const tk = segs[0].replace(/_/g, '|');
            const ck = segs[1].replace(/_/g, '|');
            _waitlistConfigSet.delete(waitlistKey(tk, ck));
            // Rebuild of _waitlistTopoSet is lazy — populateBrowser() recomputes
            // topology-level waitlisting from the config set on next open.
            _waitlistTopoSet.delete(tk);
          }
        }
        const qi = reviewState.queue[reviewState.cursor];
        if (qi && qi.recordId === recordId) {
          qi.verified = newVal; qi.verifiedAt = rec.verifiedAt;
        }
        if (newVal && reviewState.queueFilter === 'pending') {
          setTimeout(reviewNext, 250);
        }
      });
      const removeBtn = document.createElement('button');
      removeBtn.className = 'review-remove-btn';
      removeBtn.textContent = '\u2715 Remove paper';
      removeBtn.title = 'Paper does not match this diagram';
      removeBtn.addEventListener('click', async () => {
        if (!confirm('Remove this paper from the entry?\n\n' + ref)) return;
        const result = await reviewKernel.removeRecord(item.path, recordId);
        if (result.status !== 'ok') {
          showWarningToast('Remove failed: ' + (result.error || ''), true);
          return;
        }
        if (result.emptyAfter && confirm('No records remain. Delete the entry entirely?\n\n(Cancel = keep as empty entry)')) {
          await reviewKernel.deleteEntry(item.path);
          // delete_entry also removes the config from ui/waitlist.json,
          // so drop it from the client-side set.
          const path = String(item.path || '');
          const segs = path.split('/');
          if (segs.length >= 2) {
            const tk = segs[0].replace(/_/g, '|');
            const ck = segs[1].replace(/_/g, '|');
            _waitlistConfigSet.delete(waitlistKey(tk, ck));
            _waitlistTopoSet.delete(tk);
          }
        }
        card.remove();
        library = null;
        await loadLibrary();
        setTimeout(reviewNext, 100);
      });
      controls.appendChild(verifyBtn);
      controls.appendChild(removeBtn);
      card.appendChild(controls);
    }
    recsSec.appendChild(card);
  });
  body.appendChild(recsSec);

  // Results with LaTeX preview
  const results = entry.Results || [];
  if (results.length > 0) {
    const resSec = document.createElement('div');
    resSec.className = 'review-side-section';
    resSec.innerHTML = `<div class="review-side-section-title">Computed Results (${results.length})</div>`;
    results.forEach(r => {
      const card = document.createElement('div');
      card.className = 'review-side-record';
      card.style.padding = '8px 10px';
      const dim = r.dimension || '';
      const eps = r.epsOrder != null ? '\u03B5^' + r.epsOrder : '';
      const ver = r.stVersion || '';
      card.innerHTML = `<div class="review-side-record-badges" style="margin-bottom:4px">` +
        `<span class="badge badge-muted">${escapeHtml(dim)}</span>` +
        (eps ? `<span class="badge badge-gold">${escapeHtml(eps)}</span>` : '') +
        `<span class="badge badge-muted">${escapeHtml(ver)}</span>` +
        `<span class="badge badge-muted">${escapeHtml(r.contributor || '')}</span>` +
        `<span class="badge badge-muted">${formatBytes(r.byteCount || 0)}</span>` +
        `</div>`;
      // Render TeX if available
      if (r.resultTeX && typeof katex !== 'undefined') {
        const texEl = document.createElement('div');
        texEl.style.cssText = 'overflow-x:auto;padding:6px 0;font-size:11px;';
        try {
          const cleaned = cleanTeX(r.resultTeX);
          katex.render('\\displaystyle ' + cleaned, texEl, {
            throwOnError: false, displayMode: true, trust: true,
            maxSize: 500, maxExpand: 10000
          });
        } catch (e) {
          texEl.textContent = r.resultTeX;
          texEl.style.fontFamily = 'var(--mono, monospace)';
          texEl.style.fontSize = '10px';
          texEl.style.color = 'var(--text-muted)';
        }
        card.appendChild(texEl);
      }
      resSec.appendChild(card);
    });
    body.appendChild(resSec);
  }

  // Paper coverage: all entries in the library citing the same paper
  const currentRef = (records[0] || {}).reference || '';
  const currentArxivId = reviewExtractArxivId(currentRef);
  if (currentArxivId && library && library.topologies) {
    const coverageSec = document.createElement('div');
    coverageSec.className = 'review-side-section';
    const coverageEntries = [];
    for (const tk in library.topologies) {
      const t = library.topologies[tk];
      const cfgs = t.configs || {};
      for (const ck in cfgs) {
        const cfg = cfgs[ck];
        const recs = cfg.Records || cfg.records || [];
        const refs = cfg.References || cfg.references || [];
        const allRefs = [...refs, ...recs.map(r => r.reference || '')];
        const cites = allRefs.some(r => {
          const aid = reviewExtractArxivId(r);
          return aid === currentArxivId;
        });
        if (cites) {
          const names = cfg.Names || cfg.names || [];
          const name = (Array.isArray(names) ? names[0] : names) || ck;
          const cni = cfg.nickel || cfg.CNickelIndex || cfg.CNickel || (tk + ':' + ck);
          coverageEntries.push({ tk, ck, name, cni, cfg, topo: t });
        }
      }
    }
    coverageSec.innerHTML = `<div class="review-side-section-title">Paper coverage \u2014 arXiv:${escapeHtml(currentArxivId)} (${coverageEntries.length} entries)</div>`;

    if (coverageEntries.length === 0) {
      coverageSec.innerHTML += '<div class="review-side-empty">No entries cite this paper</div>';
    } else {
      const list = document.createElement('div');
      list.style.cssText = 'display:flex;flex-wrap:wrap;gap:6px;';
      coverageEntries.forEach(ce => {
        const card = document.createElement('div');
        card.style.cssText = 'display:flex;align-items:center;gap:6px;background:var(--surface-alt);border:1px solid var(--border);border-radius:4px;padding:4px 8px;cursor:pointer;font-size:10px;max-width:100%;';
        card.title = ce.cni;
        // Highlight the current entry
        if (ce.cni === (entry.CNickelIndex || '')) {
          card.style.borderColor = 'var(--accent)';
          card.style.background = 'var(--accent-light)';
        }
        try {
          const thumb = generateThumbnail(ce.tk, ce.ck);
          thumb.style.cssText = 'width:36px;height:36px;flex-shrink:0;';
          card.appendChild(thumb);
        } catch (_) {}
        const label = document.createElement('span');
        label.style.cssText = 'overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';
        label.textContent = ce.name;
        card.appendChild(label);
        card.addEventListener('click', () => {
          // Load this entry into the canvas + side panel
          _reviewLoadingEntry = true;
          loadFromNickel(ce.tk, ce.ck);
          _reviewLoadingEntry = false;
          reviewCurrentEntryPath = reviewPathForKey(ce.tk, ce.ck);
          reviewPopulateSidePanel({
            ...item,
            path: reviewCurrentEntryPath,
            cNickel: ce.cni,
            names: ce.cfg.Names || ce.cfg.names || [],
            massScales: ce.cfg.MassScales ?? ce.cfg.massScales ?? null,
            functionClass: ce.cfg.FunctionClass || ce.cfg.functionClass || '',
          });
        });
        list.appendChild(card);
      });
      coverageSec.appendChild(list);
    }

    // "Add new entry for this paper" button
    const addBtn = document.createElement('button');
    addBtn.className = 'review-verify-btn';
    addBtn.style.cssText = 'margin-top:10px;width:100%;justify-content:center;padding:8px;font-size:12px;';
    addBtn.textContent = '+ Add new entry for this paper';
    addBtn.addEventListener('click', async () => {
      const info = reviewBuildCanvasNickelInfo();
      if (!info) {
        showWarningToast('Draw a diagram on the canvas first', true);
        return;
      }
      const arxivRef = 'arXiv:' + currentArxivId;
      const newEntry = {
        CNickelIndex: info.cNickelIndex,
        MassScales: info.massScales,
        FunctionClass: 'unknown',
        EpsilonOrder: '',
        References: [arxivRef],
        Records: [{
          epsOrders: '',
          reference: arxivRef,
          authors: '',
          description: '',
          recordId: 'bf_' + Date.now().toString(36),
          verified: false,
          verifiedAt: '',
          location: '',
        }],
        Source: ['arXiv'],
        Results: [],
        Names: [],
      };
      const topoStub = {
        NickelIndex: info.topoNickel,
        Name: '',
        Loops: info.loops,
        Legs: info.legs,
        Propagators: info.props,
      };
      if (!confirm('Create new entry?\n\n' +
        'CNickelIndex: ' + info.cNickelIndex + '\n' +
        'Path: library-bundled/' + info.relPath + '\n' +
        'Reference: ' + arxivRef)) return;
      const result = await fetch('/api/review/create_entry', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ path: info.relPath, entry: newEntry, topology: topoStub, force: false }),
      }).then(r => r.json());
      if (result.status === 'conflict') {
        showWarningToast('Entry already exists at ' + info.relPath + ' \u2014 add a record to it instead', true);
        return;
      }
      if (result.status !== 'ok') {
        showWarningToast('Create failed: ' + (result.error || ''), true);
        return;
      }
      library = null;
      _reviewNameIndex = null;
      await loadLibrary();
      reviewCurrentEntryPath = info.relPath;
      showWarningToast('Entry created at ' + info.relPath, false);
      // Refresh side panel to show the new entry in coverage
      reviewPopulateSidePanel({ ...item, path: info.relPath, cNickel: info.cNickelIndex });
    });
    coverageSec.appendChild(addBtn);

    body.appendChild(coverageSec);
  }
}

// ── Canvas-edit dirty state + save/move flow ──

function reviewSetDirty(dirty) {
  reviewState.dirty = dirty;
  const bar = document.getElementById('review-save-bar');
  if (bar) bar.style.display = dirty ? 'flex' : 'none';
}

function reviewSetupSaveBar() {
  document.getElementById('review-save-btn')?.addEventListener('click', reviewSaveCanvasChanges);
  document.getElementById('review-discard-btn')?.addEventListener('click', reviewDiscardCanvasChanges);
}

function reviewDiscardCanvasChanges() {
  // Re-load the current entry's diagram from the library
  reviewSetDirty(false);
  const item = reviewState.queue[reviewState.cursor];
  if (!item) return;
  if (reviewState.queueType === 'rejects') {
    _reviewLoadingEntry = true;
    const c = item.candidate;
    if (c && Array.isArray(c.edges) && c.edges.length > 0 && Array.isArray(c.edges[0])) {
      reviewLoadCandidateCanvas(c);
    } else {
      clearAll();
    }
    _reviewLoadingEntry = false;
  } else {
    _reviewLoadingEntry = true;
    if (library && library.topologies) {
      for (const tk in library.topologies) {
        const cfgs = library.topologies[tk].configs || {};
        for (const ck in cfgs) {
          const cni = cfgs[ck].nickel || cfgs[ck].CNickelIndex || cfgs[ck].CNickel;
          if (cni === item.cNickel) { loadFromNickel(tk, ck); break; }
        }
      }
    }
    _reviewLoadingEntry = false;
  }
}

function reviewBuildCanvasNickelInfo() {
  // Returns { topoNickel, configKey, cNickelIndex, relPath, massScales, loops, legs, props }
  // or null if the canvas is empty / not connected.
  const edgeData = buildEdgeData();
  if (!edgeData || edgeData.edges.length === 0) return null;
  try {
    if (!isConnected(edgeData.edges)) return null;
    const result = canonicalize(edgeData.edges);
    const topoNickel = result.string;
    const canvasMasses = getCanvasMassArray(
      result.nickel, result.nodeMaps[0], edgeData.edges, edgeData.masses
    );
    const massToLabel = {};
    let nextLabel = 1, massScales = 0;
    const configChars = [];
    let massIdx = 0;
    for (let i = 0; i < result.nickel.length; i++) {
      for (const j of result.nickel[i]) {
        const m = canvasMasses[massIdx++] || 0;
        if (m === 0) { configChars.push('0'); }
        else {
          if (!(m in massToLabel)) { massToLabel[m] = nextLabel++; massScales++; }
          configChars.push(String(massToLabel[m]));
        }
      }
      configChars.push('|');
    }
    const configKey = configChars.join('');
    const cNickelIndex = topoNickel + ':' + configKey;
    const topoDir = topoNickel.replace(/\|/g, '_');
    const cfgDir = configKey.replace(/\|/g, '_');
    const relPath = topoDir + '/' + cfgDir + '/entry.json';
    // Compute loops/legs/props
    const nInternal = new Set();
    let nLegs = 0;
    edgeData.edges.forEach(([a, b]) => {
      if (a >= 0) nInternal.add(a);
      if (b >= 0) nInternal.add(b);
      if (a === LEG) nLegs++;
      if (b === LEG) nLegs++;
    });
    const V = nInternal.size;
    const E = edgeData.edges.length - nLegs;
    const L = E - V + 1;
    return { topoNickel, configKey, cNickelIndex, relPath, massScales, loops: L, legs: nLegs, props: E };
  } catch (e) {
    console.warn('[review] canonicalization failed:', e);
    return null;
  }
}

async function reviewSaveCanvasChanges() {
  if (!reviewIsActive() || !reviewState.dirty) return;
  const info = reviewBuildCanvasNickelInfo();
  if (!info) {
    showWarningToast('Cannot save: canvas is empty or graph is disconnected', true);
    return;
  }
  const oldPath = reviewCurrentEntryPath;
  const newPath = info.relPath;

  if (!oldPath) {
    // Rejects mode: no existing entry to move from. Use the accept flow instead.
    showWarningToast('Use "Accept (A)" to create a new entry from the canvas', false);
    return;
  }

  if (oldPath === newPath) {
    // Same path: topology and masses haven't changed structurally.
    // Nothing to move — metadata edits already save inline.
    reviewSetDirty(false);
    showWarningToast('No structural change detected (same CNickelIndex)', false);
    return;
  }

  // Structural change: need to move the entry.
  // Fetch the full current entry to get Records/Results counts.
  const resp = await reviewKernel.getEntry(oldPath);
  if (resp.status !== 'ok' || !resp.entry) {
    showWarningToast('Could not load current entry', true);
    return;
  }
  const entry = resp.entry;
  const records = entry.Records || [];
  const results = entry.Results || [];
  const oldCNickel = entry.CNickelIndex || '';

  // Build the confirmation dialog
  const recordIds = records.map(r => r.recordId).filter(Boolean);
  const msg = [
    'The canvas topology/mass assignment has changed.',
    '',
    'Old: ' + oldCNickel,
    'New: ' + info.cNickelIndex,
    '',
    'Old path: library-bundled/' + oldPath,
    'New path: library-bundled/' + newPath,
    '',
    records.length + ' record(s) will move to the new entry.',
    results.length > 0 ? results.length + ' result(s) will be DROPPED (computed for the old topology).' : 'No results to drop.',
    '',
    'Proceed with the move?',
  ].join('\n');
  if (!confirm(msg)) return;

  // Ask about the old entry
  const keepOld = records.length > 0 ? false : undefined;
  // If there are records, they all move → old entry will be empty → ask keep/delete
  let keepOldEmpty = false;
  if (records.length > 0) {
    keepOldEmpty = !confirm('Delete the old (now-empty) entry?\n\n' +
      'library-bundled/' + oldPath + '\n\n(Cancel = keep as empty entry)');
  }

  // Update the entry's CNickelIndex and MassScales to reflect the new canvas
  const newEntry = { ...entry };
  newEntry.CNickelIndex = info.cNickelIndex;
  newEntry.MassScales = info.massScales;

  // Execute the move
  const moveResult = await fetch('/api/review/move_entry', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({
      oldPath,
      newPath,
      entry: newEntry,
      recordsToMove: recordIds,
      dropResults: true,
      force: true,
      keepOldEmpty,
    }),
  }).then(r => r.json());

  if (moveResult.status !== 'ok') {
    showWarningToast('Move failed: ' + (moveResult.error || ''), true);
    return;
  }

  reviewSetDirty(false);
  reviewCurrentEntryPath = newPath;
  _reviewLoadedCNickel = info.cNickelIndex;

  // Rebuild library + refresh
  library = null;
  _reviewNameIndex = null;
  if (library) library._canonIndex = null;
  await loadLibrary();

  // Update the queue item in place
  const qi = reviewState.queue[reviewState.cursor];
  if (qi) {
    qi.path = newPath;
    qi.cNickel = info.cNickelIndex;
    qi.massScales = info.massScales;
  }

  // Refresh the side panel
  if (qi) reviewPopulateSidePanel(qi);
  showWarningToast('Entry moved to ' + newPath + (moveResult.droppedResults > 0 ? ' (' + moveResult.droppedResults + ' results dropped)' : ''), false);
}

function reviewPrev() {
  if (!reviewIsActive() || reviewState.cursor <= 0) return;
  if (reviewState.dirty && !confirm('Discard unsaved canvas changes?')) return;
  reviewSetDirty(false);
  reviewState.cursor--;
  reviewLoadCurrent();
  reviewUpdateProgress();
}

function reviewNext() {
  if (!reviewIsActive() || reviewState.cursor >= reviewState.queue.length - 1) return;
  if (reviewState.dirty && !confirm('Discard unsaved canvas changes?')) return;
  reviewSetDirty(false);
  reviewState.cursor++;
  reviewLoadCurrent();
  reviewUpdateProgress();
}

// Decorator hook called from openDetailPanel to add per-record verify/remove
// buttons. No-op when review mode is not active.
function reviewDecorateRecords(refsSection, cfg, topoKey, configKey) {
  if (!reviewIsActive()) return;
  const path = reviewCurrentEntryPath || reviewPathForKey(topoKey, configKey);
  if (!path) return;
  const records = cfg.Records || cfg.records || [];
  const cards = refsSection.querySelectorAll('.popup-record');
  cards.forEach((card, idx) => {
    const rec = records[idx];
    if (!rec) return;
    const recordId = rec.recordId || '';
    if (!recordId) return;
    // Build the controls row
    const controls = document.createElement('div');
    controls.className = 'review-record-controls';
    controls.dataset.recordIndex = String(idx);

    const verifyBtn = document.createElement('button');
    verifyBtn.className = 'review-verify-btn' + (rec.verified ? ' verified' : '');
    verifyBtn.innerHTML = '<span class="check-icon">' +
      (rec.verified ? '\u2713' : '') +
      '</span>' +
      (rec.verified ? 'Reference verified' : 'Mark verified');
    verifyBtn.addEventListener('click', async (evt) => {
      evt.stopPropagation();
      const newVal = !rec.verified;
      const result = await reviewKernel.verifyPair(path, recordId, newVal);
      if (result.status !== 'ok') {
        showWarningToast('Verify failed: ' + (result.error || 'unknown'), true);
        return;
      }
      rec.verified = newVal;
      rec.verifiedAt = newVal ? new Date().toISOString().slice(0, 10) : '';
      verifyBtn.classList.toggle('verified', newVal);
      verifyBtn.innerHTML = '<span class="check-icon">' +
        (newVal ? '\u2713' : '') +
        '</span>' +
        (newVal ? 'Reference verified' : 'Mark verified');
      // Sync the queue item
      const item = reviewState.queue[reviewState.cursor];
      if (item && item.recordId === recordId) {
        item.verified = newVal;
        item.verifiedAt = rec.verifiedAt;
      }
      // If "pending" filter is active and we just verified, advance to next
      if (newVal && reviewState.queueFilter === 'pending') {
        setTimeout(reviewNext, 250);
      }
    });

    const removeBtn = document.createElement('button');
    removeBtn.className = 'review-remove-btn';
    removeBtn.textContent = '\u2715 Remove paper';
    removeBtn.title = 'Remove this paper from the entry (paper does not match diagram)';
    removeBtn.addEventListener('click', async (evt) => {
      evt.stopPropagation();
      if (!confirm('Remove this paper from the entry?\n\n' + (rec.reference || ''))) return;
      const result = await reviewKernel.removeRecord(path, recordId);
      if (result.status !== 'ok') {
        showWarningToast('Remove failed: ' + (result.error || 'unknown'), true);
        return;
      }
      // If empty after removal, ask keep-or-delete
      if (result.emptyAfter) {
        if (confirm('No records remain in this entry. Delete the entry entirely?\n\n(Cancel = keep as empty entry)')) {
          const del = await reviewKernel.deleteEntry(path);
          if (del.status !== 'ok') {
            showWarningToast('Delete failed: ' + (del.error || 'unknown'), true);
            return;
          }
        }
      }
      // Remove the card from the DOM and reload library so the browser is consistent
      card.remove();
      library = null;
      await loadLibrary();
      // Advance to next item
      setTimeout(reviewNext, 100);
    });

    controls.appendChild(verifyBtn);
    controls.appendChild(removeBtn);
    card.appendChild(controls);
    if (idx === reviewState.focusedRecordIndex) {
      card.classList.add('review-record-focused');
    }
  });
}

// Keystroke handler for review mode. Returns true if the event was handled.
function reviewHandleKey(evt) {
  if (!reviewIsActive()) return false;
  // Cmd+S / Ctrl+S — save canvas changes (must be checked before modifier bail-out)
  if ((evt.ctrlKey || evt.metaKey) && evt.key === 's') {
    if (reviewState.dirty) reviewSaveCanvasChanges();
    evt.preventDefault();
    return true;
  }
  const tag = (evt.target.tagName || '').toLowerCase();
  if (tag === 'input' || tag === 'textarea' || tag === 'select') return false;
  if (evt.ctrlKey || evt.metaKey || evt.altKey) return false;
  if (evt.key === 'j' || evt.key === 'ArrowRight') {
    reviewNext();
    evt.preventDefault();
    return true;
  }
  if (evt.key === 'k' || evt.key === 'ArrowLeft') {
    reviewPrev();
    evt.preventDefault();
    return true;
  }
  if (evt.key === 'v' || evt.key === 'V') {
    // Trigger the focused record's verify button (only if the panel is open)
    const focused = document.querySelector('.review-record-focused .review-verify-btn');
    const target = focused || document.querySelector('.review-side-record .review-verify-btn') || document.querySelector('.popup-record .review-verify-btn');
    if (target) {
      target.click();
      evt.preventDefault();
      return true;
    }
  }
  // Rejects-mode keystrokes
  if (reviewState.queueType === 'rejects') {
    if (evt.key === 'a' || evt.key === 'A') {
      const item = reviewState.queue[reviewState.cursor];
      if (item) { reviewAcceptReject(item); evt.preventDefault(); return true; }
    }
    if (evt.key === 'r' || evt.key === 'R') {
      const item = reviewState.queue[reviewState.cursor];
      if (item) { reviewConfirmReject(item); evt.preventDefault(); return true; }
    }
  }
  return false;
}

// Warn on tab close if there are unsaved canvas changes
window.addEventListener('beforeunload', (e) => {
  if (reviewMode && reviewState.dirty) {
    e.preventDefault();
    e.returnValue = '';
  }
});

// ════════════════════════════════════════════════════════════════════
// END REVIEW MODE
// ════════════════════════════════════════════════════════════════════


// ─── Init ────────────────────────────────────────────────────────────

// Diagram state is auto-saved to localStorage, no need to warn on close

if (localStorage.getItem('subtropica-dark') === '1') {
  document.body.classList.add('dark');
}
updateMascot();

const _restored = restoreFromUrlHash() || restoreDiagram();
render();
loadLibrary();
reviewSetupPdfPanel();          // wire up PDF panel buttons (works in both normal & review modes)
detectBackendMode().then(() => reviewInit());

// Hide hint if diagram was restored; otherwise it stays until first vertex is placed
if (_restored) {
  $('draw-hint').classList.add('hidden');
}

// Test-only debug hook: when the page is opened with ?test=1, expose a
// handful of module-scoped bindings on window.__stTest so Playwright can
// drive the UI without DOM scraping. The getters capture the live bindings
// by closure, so values stay current as the app state evolves. This block
// is a no-op in normal page loads.
if (location.search.includes('test=1')) {
  window.__stTest = {
    get library() { return library; },
    get state() { return state; },
    get currentNickel() { return currentNickel; },
    get currentMatches() { return currentMatches; },
    get computeConfig() { return computeConfig; },
    get lastIntegrationData() { return _lastIntegrationData; },
    doIntegrate,
    loadFromNickel,
    collectIntegrationPayload,
    lookupLibraryResult,
    onGraphChanged,
    // Bypass backend-mode gating for static-serve debugging: build the
    // payload and open the modal directly. Safe behind the test flag.
    openIntegratePanelForTesting() {
      const p = collectIntegrationPayload();
      showIntegrationPanel(p);
      return p;
    },
    buildEdgeData,
    canonicalize,
    getCanvasMassArray,
    generateThumbnail,
    openPdfPanel,
    openDetailPanel,
    fetchInspireData,
    // Mass-labeling internals (Phase 1 regression tests).
    massLabelToMma,
    massLabelToTeX,
    getMassDisplayLabel,
    getEdgeMassKind,
    getDistinctMassSlots,
    setEdgeMass,
    openMassPicker,
    closeMassPicker,
    openConfigPanel,
    closeConfigPanel,
    switchConfigTab,
    cleanTeX,
    cleanSymbolTeX,
    render,
    // Export-panel hooks so Playwright can exercise the JS-built
    // graph/props command strings without going through the backend.
    buildGraphArgJS,
    buildPropsInfoJS,
    buildPropsExtMassSubsJS,
    populateExportTab,
    get pdfDoc() { return _pdfDoc; },
    get inspireCache() { return _inspireCache; },
  };
}
