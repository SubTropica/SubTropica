// ─────────────────────────────────────────────────────────────────────────────
// tex-clean.mjs — shared TeX preprocessing for SubTropica library strings.
//
// Single source of truth for the cleaners applied before KaTeX rendering.
// Imported by BOTH:
//   - ui/app.js (browser, <script type=module>)
//   - scripts/tests/test_tex_parseability.mjs (the CI parseability gate)
// Per the results-split spec §8.5: do NOT copy these functions anywhere;
// import them. A copy WILL drift (families.json prose-sync lesson).
//
// Functions moved verbatim from ui/app.js on 2026-06-11 (Plan 1, Task 1).
// ─────────────────────────────────────────────────────────────────────────────

// Strip the outer \left(…\right) pair around every \operatorname{H}\left(…\right)
// group. Walks the string and tracks \left/\right nesting so the
// matching close is found even when the body contains further
// \left … \right pairs (e.g., \left\lbrace … \right\rbrace inside an H).
// Without this, a naive regex replace on just the opener would leave a
// dangling \right) that KaTeX rejects.
export function stripHlogOuterDelim(s) {
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

export function cleanTeX(tex) {
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

  // Rationalizing change-of-variables: the conjugate variables zb, wb (and the
  // doubled zzb, wwb) are stored as plain ASCII; render them with a bar:
  // zb -> \bar{z}, wb -> \bar{w}. The bare z, w render fine as single letters.
  // Run after stripText so \text{wb} has already become wb. \b prevents matching
  // inside longer tokens (e.g. zzb is handled by its own rule, not \bzb\b).
  t = t.replace(/\bzzb\b/g, '\\bar{z}\\!\\bar{z}');
  t = t.replace(/\bwwb\b/g, '\\bar{w}\\!\\bar{w}');
  t = t.replace(/\bzb\b/g, '\\bar{z}');
  t = t.replace(/\bwb\b/g, '\\bar{w}');

  // Step 2: Physics substitutions
  // Momentum bracket notation from TeXForm: p(1) -> p_{1}, l(1) -> \ell_{1}
  t = t.replace(/\bl\((\d+)\)/g, '\\ell_{$1}');
  t = t.replace(/\bp\((\d+)\)/g, 'p_{$1}');
  t = t.replace(/\bk\((\d+)\)/g, 'k_{$1}');
  t = t.replace(/\bq\((\d+)\)/g, 'q_{$1}');
  // Mass-squared variables: mm/MM prefix means "mass squared".
  // Numeric index: mm1 -> m_{1}^2, MM1 -> M_{1}^2
  // With power: mm1^2 -> m_{1}^4 (double the exponent since mm = m^2)
  // Exponent braces are matched both-or-neither ((?:\{(\d+)\}|(\d+)), handler
  // takes pb||pp = braced-or-plain digits): the old \^\{?(\d+)\}? form made
  // each brace independently optional and ate a foreign closing brace, e.g.
  // \sqrt{x+\text{MM}^2} lost its } (6 staged letterTeX mangled; see
  // scripts/tests/test_tex_clean_unit.mjs). Same idiom on all 7 power rules.
  t = t.replace(/\bmm(\d+)\^(?:\{(\d+)\}|(\d+))/g, function(m, idx, pb, pp) { return 'm_{' + idx + '}^{' + (2*parseInt(pb || pp)) + '}'; });
  t = t.replace(/\bMM(\d+)\^(?:\{(\d+)\}|(\d+))/g, function(m, idx, pb, pp) { return 'M_{' + idx + '}^{' + (2*parseInt(pb || pp)) + '}'; });
  t = t.replace(/\bmm(\d+)/g, 'm_{$1}^2');
  t = t.replace(/\bMM(\d+)/g, 'M_{$1}^2');
  // Alphabetic index: mmH -> m_{H}^2, mmtop -> m_{top}^2, MMW -> M_{W}^2
  t = t.replace(/\bmm([A-Z][a-z]*)\^(?:\{(\d+)\}|(\d+))/g, function(m, idx, pb, pp) { return 'm_{' + idx + '}^{' + (2*parseInt(pb || pp)) + '}'; });
  t = t.replace(/\bMM([A-Z][a-z]*)\^(?:\{(\d+)\}|(\d+))/g, function(m, idx, pb, pp) { return 'M_{' + idx + '}^{' + (2*parseInt(pb || pp)) + '}'; });
  t = t.replace(/\bmm([A-Z][a-z]*)/g, 'm_{$1}^2');
  t = t.replace(/\bMM([A-Z][a-z]*)/g, 'M_{$1}^2');
  // Bare mm/MM (single mass scale): mm -> m^2, MM -> M^2
  t = t.replace(/\bmm\^(?:\{(\d+)\}|(\d+))/g, function(m, pb, pp) { return 'm^{' + (2*parseInt(pb || pp)) + '}'; });
  t = t.replace(/\bmm\b/g, 'm^2');
  t = t.replace(/\bMM\^(?:\{(\d+)\}|(\d+))/g, function(m, pb, pp) { return 'M^{' + (2*parseInt(pb || pp)) + '}'; });
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
  t = t.replace(/\bp(\d+)sq\^(?:\{(\d+)\}|(\d+))/g, function(m, idx, pb, pp) { return 'M_{' + idx + '}^{' + (2*parseInt(pb || pp)) + '}'; });
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

export function cleanSymbolTeX(tex) {
  if (!tex) return '';
  // Inside an aligned block the '&' column markers are meaningful, so only
  // apply the cleanTeX physics substitutions; skip the '&\colon' strip.
  if (/\\begin\{aligned\}/.test(tex)) return cleanTeX(tex);
  // Otherwise strip the stray alignment marker AND run the full cleanTeX
  // pipeline so the symbol preview picks up the same p(1)→p_{1} substitutions
  // used by the main result view.
  return cleanTeX(tex.replace(/&\\colon/g, '\\colon'));
}

// Given LaTeX strings for the two roots of a quadratic (produced by
// Mathematica's TeXForm of the Together'd Sqrt expressions), build a single
// unified form that writes both roots with `\pm` instead of separate `\sqrt`
// terms. The two inputs always differ in exactly one place — a leading `-`
// on the `\sqrt{...}` subterm — so longest-common-prefix + longest-common-
// suffix localises the single edit, which we rewrite to `\pm \sqrt{...}`.
// On any mismatch the caller gets back `plusTeX` (fallback: show one root
// explicitly rather than produce a wrong ± form).
export function buildPmTeX(minusTeX, plusTeX) {
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
