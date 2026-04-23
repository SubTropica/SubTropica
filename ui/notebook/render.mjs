// Pure notebook renderer — zero Node-only imports. Works in Node, in a
// Cloudflare Worker, and in the browser. The CLI wrapper (generate.mjs) handles
// filesystem I/O; callers in the UI fetch the template string themselves and
// pass it in.
//
// Public API:
//   renderNotebook(templateString, entry, recordIdx, opts) → Promise<string>
//   buildTokens(entry, recordIdx, opts)                    → {tokens, flags}
//   renderTemplate(template, tokens, flags)                → string
//
// `entry` is either a library entry.json object (cases ii/iii) or a minimal
// user-drawn payload (case i): {edges, nodes, NumPropagators?, Records?, Results?}.

const GEN_VERSION = '0.1.0';

// ---------------------------------------------------------------------------
// Template engine: a single pass that handles {{TOKEN}} substitution and
// {{#IF_FLAG}} ... {{/IF_FLAG}} section guards. No nesting beyond one level;
// IF blocks must not overlap. That's all this notebook template needs.
// ---------------------------------------------------------------------------

export function renderTemplate(template, tokens, flags) {
  let out = template;

  // Evaluate IF blocks first, so substituted tokens can't accidentally
  // produce marker syntax. Loop until stable so nested IFs resolve (outer
  // pass keeps the inner markers literally in `body`; second pass removes them).
  const ifPattern = /\{\{#IF_([A-Z_]+)\}\}([\s\S]*?)\{\{\/IF_\1\}\}/g;
  let prev;
  do {
    prev = out;
    out = out.replace(ifPattern, (_, flag, body) =>
      flags[flag] ? body : ''
    );
  } while (out !== prev);

  // Token substitution. Unknown tokens become "(* MISSING: name *)" so the
  // notebook still opens and the gap is visible.
  out = out.replace(/\{\{([A-Z0-9_]+)\}\}/g, (_, name) => {
    if (Object.prototype.hasOwnProperty.call(tokens, name)) {
      const v = tokens[name];
      return v === null || v === undefined ? '' : String(v);
    }
    return `(* MISSING: ${name} *)`;
  });

  // Collapse any run of >=2 blank lines to exactly one blank line. Excess
  // blanks become trailing whitespace inside the preceding cell when
  // Mathematica opens the .wl, so we normalize to one blank between cells.
  out = out.replace(/\n{3,}/g, '\n\n');
  // Trim trailing blank lines at end of file.
  out = out.replace(/\n{2,}$/, '\n');

  return out;
}

// ---------------------------------------------------------------------------
// Helpers for converting entry.json data into Mathematica syntax.
// ---------------------------------------------------------------------------

function wlString(s) {
  return String(s)
    .replace(/\\/g, '\\\\')
    .replace(/"/g, '\\"');
}

function wlList(arr) {
  return '{' + arr.map(x => (typeof x === 'number' ? x : String(x))).join(', ') + '}';
}

function normalizeCase(entry) {
  if (!entry.CNickelIndex) {
    return { case: 'i', label: 'user-drawn, not in library' };
  }
  if (Array.isArray(entry.Results) && entry.Results.length > 0) {
    return { case: 'iii', label: 'entry with result' };
  }
  return { case: 'ii', label: 'entry without result' };
}

function pickRecord(entry, idx) {
  const records = entry.Records || [];
  return records[idx] ?? records[0] ?? {};
}

function pickResult(entry, idx) {
  const results = entry.Results || [];
  return results[idx] ?? results[0] ?? null;
}

function wDefinitionsToWL(wdefs) {
  if (!wdefs || wdefs.length === 0) return '{}';
  const rows = wdefs.map(w =>
    `  <|"label" -> "${wlString(w.label)}", ` +
    `"definitionTeX" -> "${wlString(w.definition)}", ` +
    `"originalLetterTeX" -> "${wlString(w.originalLetter)}"|>`
  );
  return '{\n' + rows.join(',\n') + '\n}';
}

function alphabetToWL(alphabet) {
  if (!alphabet || alphabet.length === 0) return '{}';
  const rows = alphabet.map(a => `  "${wlString(a)}"`);
  return '{\n' + rows.join(',\n') + '\n}';
}

// Derive a Mathematica substitution-rule string
//   {Wm[i] -> minusRoot, Wp[i] -> plusRoot, [delta[i] -> deltaSign], ...}
// from the structured `algebraicLetters` field (schema v3, v1.0.455+).
// Supersedes the dropped `rootSubstitutions` InputForm string.
function algebraicLettersToRulesWL(algebraicLetters) {
  if (!algebraicLetters || algebraicLetters.length === 0) return '{}';
  const rules = [];
  for (const letter of algebraicLetters) {
    const i = letter.index;
    if (letter.minusRoot) rules.push(`Wm[${i}] -> ${letter.minusRoot}`);
    if (letter.plusRoot)  rules.push(`Wp[${i}] -> ${letter.plusRoot}`);
    if (letter.deltaSign === 1 || letter.deltaSign === -1) {
      rules.push(`delta[${i}] -> ${letter.deltaSign}`);
    }
  }
  return rules.length === 0 ? '{}' : '{' + rules.join(', ') + '}';
}

function referencesBlock(refs) {
  if (!refs || refs.length === 0) return '';
  return refs.map((r, i) => `  [${i + 1}] ${r}`).join('\n');
}

// INSPIRE BibTeX fetcher. Browser and modern Node both have `fetch`. CORS on
// inspirehep.net is permissive for GET; if it flakes, we return null and the
// caller falls back to a stub placeholder.
const bibtexCache = new Map();
async function fetchBibtexFromInspire(texkey, { offline = false } = {}) {
  if (offline) return null;
  if (bibtexCache.has(texkey)) return bibtexCache.get(texkey);
  const url = `https://inspirehep.net/api/literature?q=texkey:${encodeURIComponent(texkey)}&format=bibtex`;
  try {
    const response = await fetch(url);
    if (!response.ok) {
      bibtexCache.set(texkey, null);
      return null;
    }
    const text = (await response.text()).trim();
    const result = text.length > 0 ? text : null;
    bibtexCache.set(texkey, result);
    return result;
  } catch {
    bibtexCache.set(texkey, null);
    return null;
  }
}

function bibtexFallback(texkey) {
  return `@article{${texkey},\n  note = "not found on INSPIRE — retrieve manually: https://inspirehep.net/literature?q=texkey:${texkey}"\n}`;
}

async function bibtexBlock(records, opts = {}) {
  const keys = [...new Set(
    records.map(r => r.texkey).filter(k => k && k.length > 0)
  )];
  if (keys.length === 0) return null;

  const entries = await Promise.all(keys.map(async k => {
    const bib = await fetchBibtexFromInspire(k, opts);
    return bib ?? bibtexFallback(k);
  }));
  return entries.join('\n\n');
}

function displayName(entry, record) {
  if (record && record.familyName) return record.familyName;
  if (entry.Names && entry.Names.length > 0) return entry.Names[0];
  if (entry.CNickelIndex) return entry.CNickelIndex;
  return 'User-drawn Feynman integral';
}

// ---------------------------------------------------------------------------
// Token builder.
// ---------------------------------------------------------------------------

export async function buildTokens(entry, recordIdx = 0, opts = {}) {
  const { case: caseId, label: caseLabel } = normalizeCase(entry);
  const record = pickRecord(entry, recordIdx);
  const result = pickResult(entry, recordIdx);

  const propExponents = result?.propExponents ?? Array(entry.NumPropagators || 0).fill(1);
  const dimRaw = result?.dimension ?? record?.dimScheme ?? '4 - 2*eps';
  // Library stores human-readable dimScheme strings like "d=4-2*eps".
  // Strip the leading "d=" (or "D=") so the emitted STIntegrate option is an
  // expression, not `"Dimension" -> d=4-2*eps` (parses as `(Dimension->d) = 4-2*eps`
  // and trips Set::write on the Protected Rule tag).
  const dim = dimRaw.replace(/^\s*[dD]\s*=\s*/, '');
  const epsOrder = result?.epsOrder ?? record?.epsOrders?.split(',').pop() ?? '0';

  // Non-default option rules for STIntegrate. Defaults are d = 4 - 2 eps,
  // Order = Automatic (→ eps^0), Exponents = 1s, MethodLR = "Espresso".
  const integrateOpts = [];
  const dimIsDefault = /^\s*4\s*-\s*2\s*\*?\s*eps\s*$/.test(dim);
  if (!dimIsDefault) integrateOpts.push(`"Dimension" -> ${dim}`);
  if (epsOrder !== '0' && epsOrder !== 'Automatic' && epsOrder !== '') {
    integrateOpts.push(`"Order" -> ${epsOrder}`);
  }
  const expsAreDefault = propExponents.every(n => n === 1);
  if (!expsAreDefault) integrateOpts.push(`"Exponents" -> ${wlList(propExponents)}`);
  const methodLR = result?.methodLR;
  if (methodLR && methodLR !== '' && methodLR !== 'Lungo') {
    integrateOpts.push(`"MethodLR" -> "${methodLR}"`);
  }
  const integrateExtraOpts = integrateOpts.length
    ? ',\n\t' + integrateOpts.join(',\n\t')
    : '';

  const tokens = {
    CNICKEL: entry.CNickelIndex || '',
    CASE: caseId,
    CASE_LABEL: caseLabel,
    DISPLAY_NAME: displayName(entry, record),
    GENERATED_AT: new Date().toISOString(),
    GEN_VERSION,

    EDGES: entry.edges || '{}',
    NODES: entry.nodes || '{}',

    PROP_EXPONENTS: wlList(propExponents),
    DIM: /eps|Eps|\*/.test(dim) ? dim : `"${dim}"`,
    EPS_ORDER: epsOrder,

    RECORD_ID: record.recordId || '',
    FAMILY_ID: (record.recordId || 'custom').replace(/[^A-Za-z0-9]/g, ''),

    STINTEGRATE_EXTRA_OPTS: integrateExtraOpts,
  };

  if (result) {
    tokens.RESULT_TEX_ESCAPED = wlString(result.resultTeX || '');
    tokens.RESULT_COMPRESSED = wlString(result.resultCompressed || '');
    tokens.STVERSION = result.stVersion || '';
    tokens.CONTRIBUTOR = result.contributor || '';
    tokens.SYMBOL_WEIGHT = result.symbolWeight ?? '';
    tokens.SYMBOL_TERMS = result.symbolTerms ?? '';
    tokens.ALPHABET_WL = alphabetToWL(result.normalizedAlphabet?.length ? result.normalizedAlphabet : result.alphabet);
    tokens.W_DEFS_WL = wDefinitionsToWL(result.wDefinitions);
    tokens.ROOT_SUBS_WL = algebraicLettersToRulesWL(result.algebraicLetters);
  }

  tokens.REFERENCES_BLOCK = referencesBlock(entry.References);
  const bib = await bibtexBlock(entry.Records || [], opts);
  if (bib) tokens.BIBTEX_BLOCK = bib;

  tokens.LIBRARY_COMMIT = tokens.LIBRARY_COMMIT || 'unknown';

  tokens.SUBMIT_CONTEXT = caseId === 'i'
    ? 'This graph is not yet in the SubTropica library.'
    : 'This library entry does not yet have a computed result.';

  const hasAlphabet =
    (result?.normalizedAlphabet?.length ?? 0) > 0 ||
    (result?.alphabet?.length ?? 0) > 0;

  const flags = {
    CASE_I: caseId === 'i',
    LIBRARY_ENTRY: caseId !== 'i',
    RESULT: Boolean(result),
    NO_RESULT: !result,
    ALPHABET: hasAlphabet,
    W_DEFS: Boolean(result?.wDefinitions?.length),
    REFERENCES: Boolean(entry.References?.length),
    BIBTEX: Boolean(bib),
  };

  return { tokens, flags };
}

// ---------------------------------------------------------------------------
// Main entry point.
// ---------------------------------------------------------------------------

export async function renderNotebook(templateString, entry, recordIdx = 0, opts = {}) {
  const { tokens, flags } = await buildTokens(entry, recordIdx, opts);
  return renderTemplate(templateString, tokens, flags);
}
