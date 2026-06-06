// Regression tests for the starter-notebook generator.
//
// Guards the three bugs found in the 2026-05-31 audit:
//   1. Program example blocks with a nested (* ... *) comment were split into a
//      truncated Program cell + a syntax-error Input cell ending in a dangling *).
//   2. A missing token (e.g. STVERSION for case ii) emitted "(* MISSING: X *)",
//      whose *) broke the Metadata Program cell the same way.
//   3. Case-ii STIntegrate options were built from descriptive record metadata
//      ("d=D", "all_orders") -> "Dimension" -> D / "Order" -> all_orders.
//   4. The "View this entry" SystemOpen cell was live and launched a browser on
//      Evaluate Notebook.
//
// Run: node --test ui/notebook/notebook.test.mjs   (from the repo root)

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import { renderNotebook } from './render.mjs';
import { wlToNb, parseCells } from './wl-to-nb.mjs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const template = await readFile(join(__dirname, 'template.wl'), 'utf8');

// Representative payloads, one per renderer case branch.
const caseIII = {
  CNickelIndex: 'e11|e|:122|1|',
  edges: '{{{1, 2}, m}, {{1, 2}, m}}',
  nodes: '{{1, M}, {2, M}}',
  Records: [{ recordId: 'bf_test', familyName: 'bub', texkey: '' }],
  Results: [{
    propExponents: [1, 1], dimension: 'd=4-2*eps', epsOrder: '0',
    resultCompressed: '1:abc', resultTeX: 'x', stVersion: 'v1', contributor: 'me',
  }],
};
const caseII = {
  CNickelIndex: 'e11|e|:000|0|',
  edges: '{{{1, 2}, 0}, {{1, 2}, 0}}',
  nodes: '{{1, 0}, {2, 0}}',
  Records: [{ recordId: 'bx_test', dimScheme: 'd=D', epsOrders: 'all_orders', familyName: 'mb', texkey: '' }],
  Results: [],
};
const caseI = {
  edges: '{{{1, 2}, m}, {{1, 2}, m}, {{2, 3}, m}, {{1, 3}, m}}',
  nodes: '{{1, 0}}',
  NumPropagators: 4, Records: [], Results: [],
};

async function build(entry, idx = 0) {
  const wl = await renderNotebook(template, entry, idx, { offline: true });
  return { wl, cells: parseCells(wl), nbStr: wlToNb(wl) };
}

const countOpens = s => (s.match(/\(\*/g) || []).length;
const countCloses = s => (s.match(/\*\)/g) || []).length;

test('no Input cell is an orphan comment-close (Bugs 1 & 2)', async () => {
  for (const [name, e] of [['i', caseI], ['ii', caseII], ['iii', caseIII]]) {
    const { cells } = await build(e);
    for (const c of cells.filter(c => c.style === 'Input')) {
      assert.ok(countOpens(c.text) >= countCloses(c.text),
        `case ${name}: Input cell has more *) than (* (dangling close):\n${c.text}`);
      assert.ok(!/^\s*\*\)/.test(c.text),
        `case ${name}: Input cell starts with an orphan *):\n${c.text}`);
    }
  }
});

test('library-example blocks survive intact as single Program cells (Bug 1)', async () => {
  const { cells } = await build(caseI);
  const prog = cells.filter(c => c.style === 'Program');
  const allMass = prog.find(c => /All mass configurations/.test(c.text));
  assert.ok(allMass, 'missing the "All mass configurations" Program cell');
  assert.match(allMass.text, /KeyValueMap/, 'Program cell lost its code body');
  assert.match(allMass.text, /libraryIndex\["topologies"\]/, 'Program cell was truncated');

  const sameCount = prog.find(c => /SAME loop and leg count/.test(c.text));
  assert.ok(sameCount, 'missing the "same loop and leg count" browse-example Program cell');
  assert.match(sameCount.text, /AnyTrue/, 'browse-example Program cell was truncated');
});

test('no MISSING token placeholder leaks into any case (Bug 2)', async () => {
  for (const [name, e] of [['i', caseI], ['ii', caseII], ['iii', caseIII]]) {
    const { wl } = await build(e);
    assert.ok(!/MISSING/.test(wl), `case ${name}: a MISSING token placeholder leaked through`);
  }
});

test('case-ii STIntegrate falls back to the bare call (Bug 3)', async () => {
  const { wl } = await build(caseII);
  assert.ok(!/"Dimension"\s*->\s*D\b/.test(wl), 'emitted invalid "Dimension" -> D');
  assert.ok(!/all_orders/.test(wl), 'emitted invalid "Order" -> all_orders');
  assert.match(wl, /result = STIntegrate\[\{edges, nodes\}\];/,
    'case ii should emit a bare STIntegrate[{edges, nodes}] call');
});

test('clean non-default options are still emitted', async () => {
  const withOpts = {
    ...caseIII,
    Results: [{ ...caseIII.Results[0], dimension: 'd=6-2*eps', epsOrder: '2', propExponents: [2, 1] }],
  };
  const { wl } = await build(withOpts);
  assert.match(wl, /"Dimension" -> 6-2\*eps/, 'eps-dependent non-default Dimension should be emitted');
  assert.match(wl, /"Order" -> 2/, 'integer non-zero Order should be emitted');
  assert.match(wl, /"Exponents" -> \{2, 1\}/, 'non-trivial Exponents should be emitted');
});

test('SystemOpen cell is commented out (Bug 4)', async () => {
  const { wl } = await build(caseIII);
  assert.match(wl, /\(\*\s*SystemOpen\[/, 'SystemOpen should be wrapped in a comment');
  // And it must not appear as a live (uncommented) statement.
  assert.ok(!/^\s*SystemOpen\[/m.test(wl), 'SystemOpen must not be a live cell');
});

test('every case still produces a valid Notebook[] string', async () => {
  for (const [name, e] of [['i', caseI], ['ii', caseII], ['iii', caseIII]]) {
    const { nbStr } = await build(e);
    assert.match(nbStr, /^\(\* Content-type: application\/vnd\.wolfram\.mathematica \*\)/,
      `case ${name}: missing content-type header`);
    assert.match(nbStr, /Notebook\[\{/, `case ${name}: missing Notebook[] wrapper`);
    // The cell list closes (`},`) into the notebook options block.
    assert.match(nbStr, /\}\s*,\s*\nCellGrouping->Manual/, `case ${name}: malformed notebook body`);
    assert.match(nbStr, /WindowSize->\{960/, `case ${name}: missing WindowSize option`);
  }
});

test('sections nest into Open CellGroupData (foldable, expanded at open)', async () => {
  for (const [name, e] of [['i', caseI], ['ii', caseII], ['iii', caseIII]]) {
    const { nbStr } = await build(e);
    assert.match(nbStr, /CellGroupData\[\{/, `case ${name}: cells should be grouped`);
    assert.match(nbStr, /\}, Open\]\]/, `case ${name}: groups must be Open (expanded)`);
    assert.ok(!/, Closed\]\]/.test(nbStr), `case ${name}: no Closed groups (all open at start)`);
    // Manual grouping is required, else the custom stylesheet's Automatic
    // grouping dissolves the explicit groups and sections stop folding.
    assert.match(nbStr, /CellGrouping->Manual/, `case ${name}: must set CellGrouping->Manual`);
    // A heading is the first member of its group.
    assert.match(nbStr, /CellGroupData\[\{\s*Cell\["[^"]*", "(Title|Section)"/,
      `case ${name}: a Title/Section heading starts a group`);
  }
});

test('generated timestamp is human-readable, not a raw ISO string', async () => {
  const wl = await renderNotebook(template, caseIII, 0,
    { offline: true, stVersion: '1.2.0', generatedAt: 'June 1, 2026 at 02:01 UTC' });
  assert.match(wl, /Auto-generated by subtropi\.ca on June 1, 2026 at 02:01 UTC/,
    'header should show the human-readable date');
  // And the default (no override) must not emit an ISO timestamp either.
  const wlDefault = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  assert.ok(!/\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}/.test(wlDefault), 'no raw ISO timestamp may leak');
});

test('header reports the SubTropica version, not a separate generator version', async () => {
  const wl = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  assert.match(wl, /Generated with SubTropica 1\.2\.0/, 'header should show ST version');
  assert.ok(!/0\.1\.0/.test(wl), 'stale GEN_VERSION 0.1.0 must be gone');
  assert.ok(!/Generator version/.test(wl), '"Generator version" label must be gone');
});

test('HyperFLINT cell offers Integrator and LROrderBackend', async () => {
  const wl = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  // single combined form: both order-search and integration through HyperFLINT
  assert.match(wl, /"Integrator" -> "HyperFLINT"/);
  assert.match(wl, /"LROrderBackend" -> "HyperFLINT"/);
  assert.ok(!/STIntegrateHF/.test(wl), 'HF cell collapsed to one form (no STIntegrateHF/Integrator-only variants)');
});

test('IBP family export cells present and commented', async () => {
  const { parseCells } = await import('./wl-to-nb.mjs');
  const wl = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  const cells = parseCells(wl);
  const inputs = cells.filter(c => c.style === 'Input');
  assert.match(wl, /SubTropica`\$LiteRedPath/);
  assert.match(wl, /SubTropica`\$FIREPath/);
  assert.match(wl, /Declare\[/);
  assert.match(wl, /integralfamilies\.yaml/);
  assert.match(wl, /NeatIBP/);
  for (const c of inputs) {
    if (/NewBasis|FIRE6|integralfamilies|NeatIBP/.test(c.text)) {
      assert.ok(/^\s*\(\*/.test(c.text) && /\*\)\s*$/.test(c.text),
        `tool cell must be a comment:\n${c.text.slice(0,80)}`);
    }
  }
});

test('diagram Graphics and TikZ cells appear only when supplied', async () => {
  const gbox = 'GraphicsBox[{LineBox[{{0,0},{1,1}}]}, ImageSize->320]';
  const withArt = await renderNotebook(template, caseIII, 0,
    { offline: true, stVersion: '1.2.0', graphicsBox: gbox, tikz: '\\draw (0,0)--(1,0);' });
  assert.match(withArt, /GraphicsBox\[\{LineBox/, 'GraphicsBox present in .wl');
  assert.match(withArt, /\\draw \(0,0\)--\(1,0\);/, 'TikZ source present');
  // It must become a pre-rendered Output cell (renders on open), NOT a base64
  // PNG Input cell that only shows after evaluation.
  const nb = wlToNb(withArt);
  assert.match(nb, /Cell\[BoxData\[GraphicsBox\[\{LineBox/, 'GraphicsBox emitted as a BoxData Output cell');
  assert.ok(!/ImportByteArray\[BaseDecode/.test(nb), 'no base64 PNG ImportByteArray cell');
  const without = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  assert.ok(!/GraphicsBox/.test(without), 'no diagram cell without data');
  assert.ok(!/tikzpicture|\\draw/.test(without), 'no TikZ cell without data');
});

test('library-result LaTeX is a single-backslash Program cell, not a doubled WL string', async () => {
  const withTeX = {
    ...caseIII,
    // JS source 'O\\left...' is the string  O\left(\varepsilon^1\right)  (single backslashes).
    Results: [{ ...caseIII.Results[0], resultTeX: 'O\\left(\\varepsilon^1\\right)' }],
  };
  const wl = await renderNotebook(template, withTeX, 0, { offline: true, stVersion: '1.2.0' });
  // The old `referenceResultTeX = "..."` WL-string assignment forced the .wl source
  // to carry doubled backslashes (\\left), which reads as a bug. It must be gone.
  assert.ok(!/referenceResultTeX\s*=/.test(wl), 'TeX must not be a WL-string assignment');
  // Raw single-backslash LaTeX present as the Program-cell body.
  assert.match(wl, /O\\left\(\\varepsilon\^1\\right\)/, 'raw single-backslash LaTeX in the Program cell');
  // After conversion to .nb: one escaping layer => exactly \\left, never the
  // quad-backslash \\\\left that the old double-escape produced.
  const nb = wlToNb(wl);
  assert.ok(!/\\{4}left/.test(nb), 'no quad-backslash (double-escape) of LaTeX in the .nb');
  assert.match(nb, /\\\\left/, 'single-source backslash escaped exactly once in the .nb');
});

test('Libra and DiffExp scaffolds present and commented', async () => {
  const wl = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  assert.match(wl, /Libra/);
  assert.match(wl, /DiffExp/);
  assert.match(wl, /LoadConfiguration|TransportTo/);
});
test('PolyLogTools cell only for entries with a result', async () => {
  const withRes = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  assert.match(withRes, /PolyLogTools`/);
  assert.match(withRes, /Hlog\[.*\] :> G\[/);
  assert.match(withRes, /TensorProduct -> CiTi/);
  const noRes = await renderNotebook(template, caseII, 0, { offline: true, stVersion: '1.2.0' });
  assert.ok(!/PolyLogTools`/.test(noRes), 'no PolyLogTools cell without a result');
});

test('ConfigureSubTropica cell lists every tool path option', async () => {
  const wl = await renderNotebook(template, caseIII, 0, { offline: true, stVersion: '1.2.0' });
  for (const opt of ['PolymakePath','PythonPath','LiteRedPath','FIREPath','AMFlowPath',
       'FIESTAPath','FeyntropPath','HyperFlintPath','GinshPath','FiniteFlowPath','SPQRPath',
       'KiraPath','NeatIBPPath','SingularPath','PolyLogToolsPath','LibraPath','DiffExpPath']) {
    assert.match(wl, new RegExp(opt), `ConfigureSubTropica cell must mention ${opt}`);
  }
});
