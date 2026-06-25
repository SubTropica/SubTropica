# Changelog

All notable public-facing changes to [SubTropica](https://subtropi.ca) are
documented in this file.  The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

## [Unreleased]

### Added

### Changed

### Fixed


## [1.2.7] - 2026-06-25

### Added

- **Experimental automatic rationalization.**  The `Rationalize` option of
  `STIntegrate` looks for a rationalizing change of variables for square-root
  letters (currently the genus-zero, conic case) through an Euler substitution,
  extending the hyperlogarithm integrators to some integrals whose natural
  alphabet is algebraic.  `Rationalize` (`Automatic` / `True` / `False`) is the
  user-facing umbrella for the per-face root-handling escalation (strict, then
  `FindRoots` algebraic letters, then the Euler-rationalization rung) and
  supersedes the deprecated `Carry` option.
- **n-gon kinematic family collection.**  Explicit closed-form results for the
  one-loop n-gon integrals are added as native library results: the massless
  pentagon, hexagon (in harmonic polylogarithms), and heptagon (Murakami
  spherical-tetrahedron volume), the all-mass box (Murakami-Yano hyperbolic
  tetrahedron) and all-mass pentagon, and explicit multiple-polylogarithm forms
  for the Basso-Dixon, ladder, and fishnet families.  Generic-mass member
  configurations are registered for n = 2 through 8.
- **Per-result "Download .nb"** button, and Export / Notebook buttons on diagram
  entries, in the web interface.
- Bare Euler-integrand input to `STIntegrate` now defaults to the HyperFLINT
  backend.

### Changed

- **Computed-results library expansion (209 to 417).**  Most of the growth is the
  arXiv:1705.06483 phi^4 period import (two- through six-loop periods, with the
  external scale restored), together with the n-gon volumes and the exact
  ladder / box / triangle / fishnet closed forms above.  The bundled library now
  holds 503 topologies and 1,105 mass configurations.
- **Library reference audit.**  The n = 2, 3, and 4 external-leg entries were
  fact-checked against their cited references; off-shell references that had been
  attached to scaleless or wrong-mass entries were re-homed to the correct
  sibling configurations.

### Fixed

- n = 4 mis-encodings re-canonicalized into the correct off-shell
  configurations; one spurious n = 4 entry not present in its cited reference was
  removed.
- Closed-form display: `p_i^2` rendered as `M_i^2` in 19 ladder / box / triangle
  results, the pentagon `H(0; .)` reduced to `log`, ladder / fishnet cross-ratios
  grounded in Mandelstam `s_{ij}`, and `g_k` defined explicitly in the all-mass
  box result.
- Web UI: result-star precedence, collection reference cards, the
  install-packages banner copy, and the paper-thumbnail placeholder.


## [1.2.6] - 2026-06-19

### Added

- `STHyperFlint` integrator-level `"Carry"` option (`carry_discharge`): the carry
  DFS discharges carried degree-2 `Wm` / `Wp` letters at their integration step
  (requires `FindRoots -> True`).
- `ScorePruneFactor`, a score-driven branch-and-bound prune for `find_lr_orders`
  (HyperFLINT linear-reducibility order search).
- Library browser "Sort by" control (loops, propagators, mass scales, references).
- Three-loop sub-sector topologies from arXiv:2111.13595 (Issue #46): 29 new
  topologies plus single-scale bare-scalar results, with an import-attribution
  panel in the web interface.

### Changed

- Renamed the partial-fraction-decomposition function `PartialFractions` to
  `STPartialFractions` (its `Options` and usage text follow). Wolfram Language
  15.0 introduced a built-in, Protected `System`PartialFractions`, which
  shadowed the package symbol and silently rejected SubTropica's own definition
  at load (`Set::write` / `SetDelayed::write`). `STPartialFractions` follows the
  public `ST*` naming convention. Calls to `PartialFractions[f, var]` should
  become `STPartialFractions[f, var]`.

- Web UI About window now reports **computed results**, **singularity analyses**,
  and **proposed alphabets** as separate counts (matching the README library
  table), instead of a single lumped "results" figure.

### Fixed

- Wolfram Language 15.0 compatibility. SubTropica now loads and integrates
  cleanly under v15. The only incompatibility was the `PartialFractions` symbol
  collision noted above; the HyperFLINT LibraryLink dylib loads unchanged, and
  the package uses no functions whose signature or behavior changed in 15.0.

- `verify_order_is_lr` no longer reports a multi-group false negative: the
  HyperFLINT linear-reducibility verification now reads the intersection-refined
  set table, matching the order search by construction.
- Web UI: result-provenance stars are restored under the split-result schema,
  and library search now matches aliases and author names.
- Submission worker: the CORS header is set on all responses, so POST replies
  are readable (fixes the spurious "Network error" reported on submission).
- Library data: several one-loop entries re-expressed to the physical `s + i0`
  channel.


## [1.2.5] — 2026-06-16

### Added

- **`STNewton` and `STForgetCoefficients`.**  `STNewton[polynomial, xvars]`
  returns the Newton-polytope data of a polynomial (vertices, rays, facets,
  equations); `STForgetCoefficients[polynomial, xvars]` reduces a polynomial
  to its monomial support (coefficients set to one).
- **`STHyperFlint` factored output (`SimplifyOutput -> False`).**  Returns
  the unexpanded, factored result instead of the fully reduced rational
  form.  For single-variable, single-linear-pole integrands this is the
  closed residue form with spectator polynomials kept factored, produced
  in a fraction of the time of the reduced result and numerically
  agreeing with it.
- **Library: singularity analyses and proposed alphabets.**  The library
  now ships 415 Landau-singularity analyses and 404 proposed alphabets
  alongside the computed integral results, browsable on the website.
- **Website: diagram deep-linking.**  A `?q=<Nickel index>` URL opens the
  browser directly to the corresponding diagram.
- **Website: planar diagrams drawn planar.**  Provably planar graphs are
  laid out without edge crossings (precomputed Tutte embeddings and
  planar external-leg order), in both the library view and the editor
  canvas.
- **Website: provenance and alphabet display.**  Per-record provenance
  indicators, highlighting of verified alphabet letters, an Euler-drop
  tooltip on each rational letter, and display of the change of variables
  used for a result.

### Changed

- **Diagram names and planarity flags corrected.**  CanonicalNames that
  contradicted computed planarity were fixed, mass-scale descriptors were
  normalized, and a few misattached results were re-homed.

### Fixed

- **Linearly reducible order search no longer reports false positives.**
  `STIntegrate`, `STHyperFlint`, and `STFindLROrdersHF` could previously
  accept an integration order that is not actually linearly reducible and
  then fail at integration time with a nonlinear-denominator error.  The
  order search now agrees with HyperInt's polynomial reduction, so
  automatically found orders integrate reliably.


## [1.2.4] — 2026-06-13

HyperFORM integrator backend; Espresso retired (Lungo is now the sole
Fubini method); larger result submissions and faster website library
browsing; factor-prediction lookups for the Fubini reduction.

### Added

- **HyperFORM integrator backend.**  `"Integrator" -> "HyperFORM"` and
  the standalone `STHyperForm[integrand, vars]` evaluate Euler
  integrals with Adam Kardos' HyperFORM package (hyperlogarithms in
  FORM, github.com/adamkardos/HyperFORM).  FORM (>= 5.0) and HyperFORM
  are new optional dependencies: configure with
  `ConfigureSubTropica[FormPath -> ..., HyperFormPath -> ...]` (both
  auto-discovered), check via the `HyperFORM` badge in the welcome
  banner or `STCheckDependencies[]`.  It handles individually
  convergent rational faces with multiple-zeta-value boundary
  constants; faces outside that scope fail loudly rather than
  returning a partial result.
- **`STBuildFactorTable` and `STFactorPredictor`.**  Tabulate and look
  up the polynomial factorizations that arise along a known
  linearly reducible order, so the Fubini reduction can reuse
  factor information instead of recomputing it at each variable.
- **New `STIntegrate` option `"Carry" -> False`.**  Exposes the
  carry-discharge keep rule of the linearly reducible order search as
  a user-visible toggle (default off).

- **`HF_LAZY_SUM` lazy-sum lever, exposed through `STHyperFlint`** (dev
  1.2.3.5).  Setting the `HF_LAZY_SUM` environment variable to `"1"`
  (e.g. `SetEnvironment["HF_LAZY_SUM" -> "1"]`) makes HyperFLINT
  integrate the top-level addends of a sum SEPARATELY and add the
  result tables, instead of fusing them at parse time.  On R-class
  counterterm faces (sums of denominator-disjoint addends, where fusion
  cross-multiplies the denominators into a multi-million-term
  numerator) this is ~10^2 on a Log^2 face and up to ~10^3-10^5 on the
  heavier Log^5 faces, with a byte-identical-value result.  The flag is
  read by the engine per call, so it works on both the in-process
  LibraryLink dylib and the CLI subprocess; the dylib must be built
  with the lazy-sum source.  Default off.  MWE:
  `notes/hf_tree_merge/lazy_sum_MWE.wl`.

- **`$STHyperFlintAllowCLI` transport master-switch** (dev 1.2.3.5).  A
  global flag (default `False`) that restricts ST -> HF communication
  (`STHyperFlint`, `STFindLROrdersHF`, `STBuildFactorTable`,
  `STFindLROrdersScanHF`) to the in-process LibraryLink dylib.  Set
  `True` to re-enable the CLI subprocess transport for development.

### Changed

- **Espresso retired.**  `MethodLR -> "Espresso"` is no longer a valid
  setting; the accepted values are now `{"Lungo", "Doppio"}`, and
  Lungo is the sole Fubini method (it carries the `FindRoots` and
  `Carry` modifiers).  Selecting the removed method aborts with
  `STIntegrate::badmlr`.
- **Larger result submissions and faster library browsing.**  Result
  submissions now accept payloads up to 16 MB (previously 1 MB), so
  high-weight results with large symbols can be contributed.  The
  online library loads each result's heavy data (series, symbol,
  alphabet) on demand when you open it, so browsing the catalogue is
  faster.  Stored results are unchanged for kernel users:
  `STVerify`, library lookups, and notebook export all work as
  before.  Note: builds older than 1.2.4 cannot `STVerify` against
  the updated online library; upgrade to this release.
- **ST -> HF transport defaults to LibraryLink only** (dev 1.2.3.5).
  With the new `$STHyperFlintAllowCLI = False` default, `STHyperFlint`
  and the HF order-finding / factor-table ops no longer fall back to
  the (slower, easily-confused) CLI subprocess when the in-process
  LibraryLink dylib is unavailable on the main kernel; they now fail
  loudly with `::clidisabled` instead.  The Parallelize subkernel CLI
  exception (the RSS lever, `OMP_NUM_THREADS=1` per subprocess) is
  preserved regardless of the flag, and `HF_FORCE_CLI` is honoured only
  when `$STHyperFlintAllowCLI = True`.  CONSEQUENCE: because the dylib
  version gate is strict-equality (`hf_version` must equal
  `$SubTropicaVersion`), a fresh checkout whose LibraryLink dylib is
  stale or version-mismatched now hard-errors by default until a
  version-matched dylib is built (`cd HyperFLINT && cmake -S . -B
  build-omp -DHF_VERSION=<version> && cmake --build build-omp --target
  hyperflint_librarylink`), OR `$STHyperFlintAllowCLI = True` is set.
  Dev/benchmark scripts that pinned the CLI transport
  (`$STHyperFlintUseLibraryLink = False`) were updated to also set
  `$STHyperFlintAllowCLI = True`.

- **Library results split** (dev 1.2.3.5).  Heavy result payloads
  (`resultCompressed`, `resultTeX`, `symbolTeX`, `normalizedSymbolTeX`,
  `wDefinitions`, `algebraicLetters`, `resultInputForm`) moved out of
  `entry.json` into an id-addressed sibling `results.json` per config
  directory; stubs keep every light field plus `resultDataId` (16-hex,
  `.n` collision suffix) and an inline `resultTeXPreview`.  All 179
  result-bearing entries (191 heavy records) migrated;  `ui/library.json`
  shrank 18.9 % (7.20 MB -> 5.84 MB).  The website lazy-loads sibling
  data on demand (`ensureResultData`, kernel-server `/api/results`
  route, preview rendering until the fetch lands); the submission
  workflow and the kernel write paths (`STSaveResult`, result deletion)
  write the split format; alignment validator + de-migration guard
  enforced in CI (`scripts/library_audit/validate_results_split.py
  --armed`).  The heavy-field list is pinned in three languages
  (`scripts/_results_split_common.py`, `SubTropica.wl`, `ui/app.js`);
  schema documented in `docs/naming-conventions.md` §9.1 and
  `notes/conventions.md` §10.
  **Compatibility**: paclets older than this build cannot `STVerify`
  against the post-migration public library (remote entries are now
  light stubs and the old reader does not fetch the sibling); upgrade
  to a split-aware build.  **Deploy warning**: the Firebase deploy
  procedure MUST gain the `results/` flatten-copy step (Plan 3) before
  the next website deploy, or the deployed site loses access to heavy
  result data.

### Fixed

- **Carry-discharge default.**  A carry-discharge rule for the
  FindRoots linearly reducible order search shipped enabled by default
  in v1.2.3; it is now off by default, restoring the 1.2.2 linearly
  reducible order semantics.  Set `"Carry" -> True` to opt in.
- **Result rendering on the website.**  Fixed several library results
  that rendered as errors in the browser (oversized expansions and a
  notation-cleanup bug affecting square-root letters).


## [1.2.3] — 2026-06-10

AnTropica retirement; HyperFLINT multi-pole partial-fraction and
PERFPOW performance wave; the issue #41 shadowing fix; n-gon family
and a major library refresh; SubTropicaII preview.

### Added

- **`SubTropicaII.wl` (preview).**  A standalone companion package
  with an LR scan and an improved Maple interface.  Ships in the
  repository only; not loaded by the core paclet.
- **HyperFLINT performance wave.**  FactoredRat-Cauchy multi-pole
  partial fractions (`HF_FR_CAUCHY_PF`, default ON), a perfect-power
  fast path (`HF_LF_PERFPOW_FAST`) with detector sub-timers, a
  `UnivarRat` type for R(y)[x] polynomial arithmetic, and a strict
  Euler-drop filter with the `HF_LR_MAX_DEG` cap for the LR-order
  search.
- **n-gon kinematic family** in the library, plus the inlined
  box-ladder closed form (Davydychev $\Phi^{(L)}$) on the family
  card.  Library counts: 424 topologies, 1,015 mass configurations,
  1,816 literature records, 203 computed results.

### Changed

- **AnTropica retired** (dev 1.2.2.7).  The experimental BVSW
  rationalization engine and all its SubTropica wiring are gone:
  `MethodLR -> "AnTropica"`, `FindRoots -> "AnTropica"`,
  `STFubiniWithAnTropica`, `STFubiniAT2`, and the `$STAnTropica*`
  globals.  Superseded by the Doppio FindRoots tier and the HyperFLINT
  carry-discharge port.  The package lives on unmaintained in `attic/`;
  Doppio now uses an inlined `dpEulerConic` (parity-gated, t25).
  `MethodLR` values are validated at option ingest: anything outside
  `{"Lungo", "Espresso", "Doppio"}` aborts with `STIntegrate::badmlr`
  instead of silently falling through to Espresso.  (List as of
  dev 1.2.3.4: `{"Lungo", "Doppio"}` — Espresso has since been
  retired, see above.)

### Fixed

- **Front-end symbol shadowing after `Needs["SubTropica`"]`**
  ([#41](https://github.com/SubTropica/SubTropica/issues/41)).  Bare
  context-qualified tokens in the package source minted `Global`-context
  twins of the public symbols `eps`, `M`, `m`, `MM`, `mm`, `l`, `p`, `s`,
  `mzv` at load time (Wolfram Language creates a symbol the moment the
  reader encounters its token, even inside code that never runs), so
  notebooks highlighted those names as shadowed.  The symbols are now
  constructed from name strings at run time and a fresh load no longer
  creates any `Global` twin of an on-path public symbol, enforced by a
  new regression test (`scripts/test_no_global_twin_mint.wl`).  For the
  optional FIESTA / ginsh / IterInt backends the corresponding twins
  (`l`, `p`, `mzv`) still appear at first backend use; this is inherent
  to their `Global`-context interfaces.
- **Library `MassScales` audit**: 11 entries carried `MassScales: 0`
  despite massive kinematics; corrected from their mass configurations.
- **`STFindSingularities`** now passes the full filtered candidate set
  to `FindLetters` (previously truncated, losing odd-letter candidates).
- **Collections UI**: member cards open the specific diagram (mass
  configuration) rather than the bare topology; computed-result stars
  require an actual computed result (family-closed-form seeds were
  demoted from `Results[]`, hence 228 -> 203 computed results); period
  badge takes precedence over MPL; wide member thumbnails keep the
  graph aspect ratio; two-parameter families display both ranges.

---

## [1.2.2] — 2026-06-05

### Added

- **Experimental: Doppio linear-reducibility engine** (`MethodLR ->
  "Doppio"`, dev preview — undocumented, interface may change).  A
  per-face LR backend that builds the integration-order table from
  genuine Landau loci (Euler-discriminant eliminations) instead of the
  sequential discriminant/resultant chain, with every letter
  chi-certified.  A corresponding scan ships inside HyperFLINT
  (`HF_EULER_FILTER`, default OFF).  Both engines use msolve for their
  Groebner bases.

- **`STToIterInt` and the IterInt symbolic backend.**  Translates
  SubTropica hyperlogarithm results into IterInt's iterated-integral
  representation and wires IterInt in as a symbolic evaluator for
  `STVerify` (via `$STSymbolicEvaluator`).

- **`"CheckDivergences"` option on `STIntegrate`** (divergence policy,
  2026-06-03). One policy source for the boundary-divergence scan in
  all three integrator backends (HyperIntica, Maple HyperInt,
  HyperFLINT). `Automatic` (default) arms the scan for the
  raw-integrand forms in *record-and-continue* mode — faces of the
  subtraction pipeline are individually log-divergent by construction,
  so detections are recorded in `$HyperInticaDivergences` (per kernel)
  and summarized once via `STIntegrate::divergencesRecorded` instead of
  aborting — and disables it for the diagram forms (tropical geometry
  guarantees face-level finiteness). Explicit `True`/`False` overrides
  on any form. The Maple preamble now sets both
  `_hyper_check_divergences` and `_hyper_abort_on_divergence` from the
  policy; every HyperFLINT request carries an explicit
  `"check_divergences"` field (request-side field; no response-schema
  bump — absent fields keep engine defaults). Standalone `STHyperFlint`
  is slated to default the scan ON but this is deferred pending
  HF-DIVCHECK-PARITY (HF's zero test false-positives on multi-pole
  convergent integrands; repro matrix and fix sketch in
  `notes/hf_divcheck_parity.md`). HyperFLINT additionally gained
  spectator-variable projection in its divergence check
  (`hyperflint_sym` `spectator_var_indices`).
- **Namespace privatization — Stage A (branch `namespace-privatization`).**
  The `Begin["`Private`"]` that had been commented out since early development
  is reactivated.  A generated public-API declaration block near the top of
  `SubTropica.wl` creates every public symbol in the `SubTropica`` context
  before `Begin["`Private`"]` opens, so all subsequent `Module` / helper
  definitions attach to the correct private context.  Public symbol surface
  shrinks from 5135 to 956 (Stage-A figure; the v1.2.2 regeneration against
  the merged tree declares 430 public names = the Stage-A/B1 surface plus the
  ten v1.2.2 API additions).  User-symbol shadowing (`General::shdw` warnings
  for `s`, `m`, `eps`, `l`, `t`, and similar single-letter names;
  `Parallel`Developer`` collisions on subkernels) is eliminated except for the
  documented reserved formal set: kinematic / formal symbols `eps`,
  `m`/`M`/`mm`/`MM`, `s`/`t`/`p`/`q`/`P`/`w`/`z`, `s12`/`s15`/`s23`/`s34`/`s45`,
  `zeta`, `G`, `ln`, `l`, `hlpI`/`hlpF`/`zz`, and the `SOFIASymanzik` option
  keys.  These remain intentionally in the public context because they appear
  in user-written integrand expressions.

- **Load-time namespace guard (`General::stnsleak`).** A `With` block before
  `End[]` scans `Names["SubTropica`Private`ST*"]` and
  `Names["SubTropica`Private`st*"]` for symbols that have definitions but were
  never declared public.  If any are found a loud warning is issued at load
  time.  The two intentionally private option-coercion helpers
  (`stHasNormalizableOpts`, `stNormalizeOptKeys`) and any generated-local
  names (containing `$`) are exempt.  Regeneration tooling lives at
  `notes/namespace_refactor/public_api/`
  (`build_public_list.wls`, `emit_declarations.wls`, `reinsert.py`).

- **Namespace privatization — B4 (HyperIntica leaf de-export).** 14 internal
  leaf helpers de-exported from `HyperIntica`` (evidence-gated per symbol:
  no SubTropica call sites, no saved-data strings, no runtime state, no live
  external callers; ledger at `notes/namespace_refactor/hyperintica_b4/`).
  22 candidates kept (live qualified calls in `HyperFLINT/test/cross/run_mma.wls`);
  the `DistributeDefinitions["HyperIntica`"]` overlay is unaffected (no
  de-exported symbol carries OwnValues). New gate G15. NOTE for gate authors:
  `BeginPackage` resolves the `HyperIntica`` dependency through `$Path`, which
  can pick a SIBLING clone — always pre-`Get` the worktree `HyperIntica.wl` by
  absolute path before loading `SubTropica.wl` in tests.
- **Namespace privatization — Stage B1 (demanded-only rule for ST*/st*).**
  The public surface is further pruned from 956 to 687 symbols by applying
  a strict demand-union rule: a name is retained as public only if it appears
  in at least one of (i) actual usage across `.wl`/`.nb`/`.wls` files,
  (ii) bare-name references in scripts, docs, or `Documentation/` notebooks,
  (iii) `DistributeDefinitions` call sites, (iv) the opt-coercion allow list,
  (v) the `public-api-inventory.md` P0/P1/P2 buckets, (vi) reach-in call
  sites from `HyperIntica.wl` or external packages, or (vii) option-key
  strings resolved at run time.  Names satisfying none of these criteria are
  moved to `SubTropica`Private``.  The complete ledger of the newly
  privatized symbols (287 at B1 time; 286 after the v1.2.2 regeneration, in
  which the IterInt driver family and the Doppio bridge became demanded
  public API) lives at
  `notes/namespace_refactor/public_api/b1_privatized_st.txt`; the 19
  pre-`Begin` bootstrap escapees are recorded in `b1_ledger_escapees.txt`.  The `$*` globals
  (package-level associations prefixed with `$`) retain the Stage-A blanket
  declaration.  The namespace guard was fire-tested against the B1 ledger
  and correctly raises `General::stnsleak` for any name outside the
  demand-union set.  Performance A/B on dbox-1m, 3l3pt, and STBenchmark-Long
  is neutral, with byte-identical output and identical peak memory.

- **`HF_USE_BASIS_CTX=1` opt-in slim-ctx path for HyperFLINT**
  (basis-ctx campaign, 2026-05-28; full record at
  `notes/hf_mzv_weight_cap_2026-05-28/`). When the env flag is set,
  HyperFLINT eliminates the 700 MZV reduction-rule LHS variables
  from its runtime `PolyCtx`. The slim ctx contains only the
  10-element basis (Log2 + 9 irreducible MZVs) plus user variables
  (Feynman params, Mandelstams, masses).

  Per-term FLINT primitive cost scales linearly in `num_vars`, so
  the 47.7× ctx-width shrink (715 → 15 vars on Smirnov tst2) yields
  a measured **−14.48% wall on tst2 default build** (paired N=3,
  OMP=13 pinned, CV<3%; pre-committed gate ≥8% cleared by ~1.9×
  margin). Lower-loop fixtures benefit disproportionately:
  tst0 −44.44%, tst1 −49.52%.

  Mechanism: at `MzvExpansionTable::load_mzv_expansion()` time, every
  reduction-rule LHS is eagerly pre-expanded into a basis-only
  `fmpq_mpoly` Rat using **Rat-level substitution** (not textual; an
  adversarial chained-rule test fixture locks the correct
  `-(mzv_2*Log2^2 - mzv_2^2)^2` expansion against the textual-
  substitution operator-precedence trap). At MZV mint time in
  `to_mzv_one_word`, a three-arm lookup dispatches: (1) basis name →
  `Poly::gen`; (2) expansion-table hit → `cross_ctx_transfer_rat`
  from `basis_ctx` into the integrator ctx; (3) legacy fallback →
  `Rat::parse` for callers without an active expansion.
  `apply_mzv_reductions` becomes a no-op on slim ctx via an
  early-return guard; the legacy code path is retained verbatim for
  the algebraic-letters fixture class (`introduce_al=true`), which
  intentionally keeps the wide ctx.

  Output is **byte-identical** to the wide-ctx baseline on tst0/1/2
  (after stripping the `vars` field, which records the 47.7× ctx
  shrink). TSan shows zero new races; the slim path actually has ~9×
  fewer races than baseline because the narrower ctx shrinks the
  shared-data surface that hosts pre-existing FLINT-internal +
  static-local-cache races.

  Currently scoped to the `op=hyperflint` bridge handler
  (`handlers.cpp::hyperflint_sym`); other op handlers
  (`evaluate_periods`, `fibration_basis`, …) still build the wide
  ctx. Default OFF; users opting in via `HF_USE_BASIS_CTX=1` are
  protected by a hard-asserting bridge input scanner that rejects
  any reducible-LHS or out-of-table MZV token in the request body.

  **HEAVY-INTEGRAND CARVE-OUT (added post-iter-22, 2026-05-28)**: the
  slim ctx **regresses by +77% wall on tst3** (single-shot reproduced;
  default 561 s → slim 987 s; math output byte-identical). The
  campaign was gated only on tst2-scale fixtures (gate 8). On heavier
  integrands like tst3, the per-mint downstream-multiply inflation
  (each mint now substitutes a 5.5×-mean-term basis Rat into the outer
  expression, instead of a 1-term `Poly::gen` placeholder) propagates
  through `transform_shuffle` / `integration_step` and dominates the
  per-term ctx-width savings. The legacy wide-ctx path implicitly
  exploits placeholder-level cancellation that slim ctx loses (see
  round-3.5 physics reviewer
  `notes/hf_mzv_weight_cap_2026-05-28/reviews/round35_physics_a75f28674fb6ad7f9.md`
  for the structural argument). The string-roundtrip
  `cross_ctx_transfer_rat` (design.md §5.3 deferred-work item) may
  also contribute; pending F-D profiling falsifier to discriminate.

  **DO NOT enable `HF_USE_BASIS_CTX=1` for heavy integrands.** The
  campaign delivers wins on tst0/1/2-scale (low-loop, few mints,
  modest intermediate expressions) and loses badly above that scale.
  Until either the F-A per-term FLINT repack (design §5.3 deferred)
  or the F-8 lazy-expansion redesign lands, users should treat this
  as opt-in for light integrands only. The
  `apply_mzv_reductions` / `parse_rhs_cached` / tolerance machinery
  scheduled for v1.1.13 deletion (FOLLOW_UP.md F4) is DEFERRED
  INDEFINITELY for the same reason.

- **`HF_PERIOD_TUPLES=1` opt-in period-tuple representation for
  HyperFLINT** (phases 1-2).  Keeps transcendental periods as opaque
  tuples with lazy boundary reduction instead of expanding them into the
  kinematic polynomial context.  First-ever HyperFLINT computation of
  the Smirnov tst4 fixture (42m54s wall / 194GB peak on a 32-thread
  Linux node, vs ~16h reported for Maple HyperInt).  Default OFF; the
  n=4 falsifier shows wall/RSS regressions on tst2/tst3-scale fixtures,
  so the default flip is deferred.

- **`HF_FR_MAT_PEEL` factor-peel for FactoredRat (default ON).**  Cures
  the 1m-tbox face-family slowdowns (7x to >25x on the affected faces,
  including a rescue from a double-timeout); opt out with
  `HF_FR_MAT_PEEL=0`.  Forwarded through the CLI subprocess environment
  together with `HF_PERIOD_TUPLES`/`HF_PROGRESS` (the request
  environment is REPLACED, not inherited; the flags were previously
  silently stripped in CLI transport).

- **One-click dependency installer: `STInstallDependencies[]` + a banner
  button.**  Automatable tools (polymake, ginsh/GiNaC, msolve, FORM,
  Singular, GNU make, pySecDec, IterInt -- the latter compiles the bundled
  `scripts/iterint_mpfr_driver.cpp` against brew GSL/Boost/MPFR/MPC) carry
  literal install-command lists (`$STInstallCommands`, executed via
  `RunProcess` with no shell); `STInstallDependency["name"]` runs them
  after a consent dialog (notebooks) or an explicit `"Confirm" -> True`
  (headless; `Automatic` prints the commands without executing,
  `False` is a dry run), then re-probes and reports the new status.
  Manual-only tools (Maple, Fermat, FIESTA, AMFlow, Kira, ...) print
  their install hints.  When the banner detects missing automatable
  tools in a notebook, it renders a small "Install packages (n missing)"
  button underneath (`Method -> "Queued"`).  brew's exit-1 on an
  already-installed formula is treated as continue (the re-probe then
  surfaces the real problem, e.g. a stale `ConfigureSubTropica` path).
  The core paclet now ships the Doppio runtime
  (`scripts/doppiofubini/doppio/`) and the IterInt driver source, and
  `stEnsureDoppioLoaded` resolves through `$SubTropicaInstallDir` (the
  old `FindFile` anchor pointed at `Kernel/` in paclet installs, so
  `MethodLR -> "Doppio"` only worked from a dev tree).

- **IterInt probe executes the driver (GSL/Boost dylib detection).**  The
  `iterint_mpfr` driver links GSL/Boost/MPFR/MPC; a build whose dylibs
  were since removed still path-resolves but dies at first use with a
  cryptic dyld error.  The dependency probe now runs the driver briefly:
  a dynamic-loader failure demotes the badge to an error whose
  `STCheckDependencies[]` message carries the fix
  (`brew install gsl boost mpfr libmpc`).  The libraries themselves are
  deliberately not badged (the registry lists invocable tools, not their
  build libraries).

- **Known issue (msolve >= 0.9.5):** internal research variants that drive
  msolve through FiniteFlow32's `FFAlgGroebner` fail against msolve 0.9.5
  (`FF::badgroebnercoeffnode`).  Nothing user-facing is affected.

- **msolve dependency badge + `MsolvePath` option.**  msolve joins the
  dependency registry as the Groebner-basis backend of the experimental
  linear-reducibility tooling: probed at load (`msolve -V`), shown in a
  new engines badge row (`HyperFLINT  IterInt  msolve`), configurable via
  `ConfigureSubTropica[MsolvePath -> ...]` (default: `msolve` resolved
  from `PATH`; `brew install msolve`).  The tools badge row no longer
  overflows the banner width (the old 7-badge row was clamped flush-left
  and spilled past the art's right edge).

- **Ecosystem badges + tool-path options.**  The welcome banner now
  probes and displays the loop-calculation ecosystem (pySecDec, FIESTA,
  AMFlow, feyntrop, FiniteFlow, SPQR, LiteRed, FIRE, HyperInt, ginsh,
  IterInt, Kira, FireFly, Fermat, NeatIBP, SpaSM, Singular, FORM,
  PolyLogTools, Libra, DiffExp, ...), and `ConfigureSubTropica` gains
  the matching path options (`KiraPath`, `FermatPath`, `FormPath`,
  `SingularPath`, `LibraPath`, `DiffExpPath`, `NeatIBPPath`,
  `PolyLogToolsPath`).

- **Documentation notebook wave.**  Reference pages under
  `Documentation/English/ReferencePages/Symbols/` for the public API
  (STIntegrate, STVerify, STBenchmark, FeynmanDraw/FeynmanPlot,
  HyperIntica entry points, configuration and library tooling), plus the
  SubTropica guide notebook.

### Changed

- **HyperFLINT is the default symbolic engine when available.**  The
  `"Integrator"` and `"LROrderBackend"` options of `STIntegrate` now
  default to `Automatic`-style dynamic resolution (a `RuleDelayed`
  default evaluated at each read): `"HyperFLINT"` when a usable
  HyperFLINT install is present (`$HyperFLINTAvailable`, which tracks
  `ConfigureSubTropica` overrides and add-on installs), `"HyperIntica"`
  otherwise.  Explicit option settings behave as before;
  `"Integrator" -> "HyperIntica"` reverts a call to the built-in engine.
  The same dynamic default applies to the `STLaunchHyperIntica*` layer.

- Dev-string convention: public releases are 3-part (this release
  collapses the 1.2.2.N dev strings); development continues on 4-part
  1.2.2.N until the next public cut.

### Fixed

- **CRITICAL (HyperFLINT parser): unary minus vs `^` precedence.**
  `-(F)^(-even)` lost its overall sign (`-x^2` parsed as `(-x)^2`).
  Counterterm-only surface in production use; tst0/1/2 and 1m-tbox are
  unaffected, the L=3 triangle ladder was invalid.  Fixed with a
  regression test (parse-tree anchored, not A/B byte-identity, which
  both arms shared through the same parser).

- **`FeynmanDraw` headless guard.**  The symbol carries an OwnValue that
  opens the interactive Graph Editor; any bare evaluation in a headless
  kernel (symbol sweeps, docs builds, harvest tooling) popped GUI
  windows.  Headless kernels now get `FeynmanDraw::nofe` + `$Failed`;
  notebook behavior is unchanged.

- **HyperFLINT discovery: repo `dist/` no longer shadowed by an installed
  add-on.**  On a repository checkout, the CLI search order placed the
  HyperFLINT add-on paclet BETWEEN the local build dir and the repo's
  LFS-shipped `dist/<arch>/`, so a stale installed add-on (e.g. v1.2.1)
  resolved first and the dylib version gate then silently forced the CLI
  transport.  Order is now: local build, repo `dist/`, add-on, static
  fallbacks (paclet-only installs are unaffected).  An explicit
  `ConfigureSubTropica[HyperFLINTPath -> ...]` pin still overrides
  discovery entirely.

- **`STToIterInt` dependency-check ordering.**  The tool registry's
  `getPath` no longer forward-references `stIterIntDriver` (the
  dependency check runs before the implementation definitions; the
  banner badge wrongly showed `[-]`), plus the adversarial-review fixes
  on the translator.

- **Bounded-domain endpoint poles** (carried from 1.1.11.2's class):
  `STIntegrate[integrand, {x, 0, 1}, ...]` dropped 1/eps poles on
  endpoint-regulated integrals; see [1.1.11.2].

---

## [1.2.1] — 2026-06-02

`HyperFLINT` add-on release: ships the **in-process LibraryLink dylib**
(macOS arm64 + Linux x86-64) alongside the CLI; the add-on paclet is
renamed `SubTropicaHyperFLINT` -> `HyperFLINT` (the loader falls back to
the old name for existing installs).  Linux GMP/MPFR/FLINT are built
`-fPIC` from source; per-arch dylibs ship via Git LFS.  The dylib embeds
`hf_version` and the loader version-gates it against
`$SubTropicaVersion` (mismatch falls back to the CLI transport).

## [1.2.0] — 2026-05-29

**HyperFLINT goes public** (MIT): the C++17 hyperlogarithm engine ships
as source plus a prebuilt CLI via the optional add-on paclet, built per
release by CI (macOS arm64 + x86-64).  Static FLINT/GMP/MPFR linking
(dynamic FLINT crashes the Wolfram kernel); runtime `chmod +x` on the
bundled binary (`PacletInstall` strips the execute bit).

---

## [1.1.11.2] — 2026-05-28

Bug fix: bounded-domain `STIntegrate[integrand, {x, 0, 1}, ...]` (and the
`{x, 1, Infinity}` variant) silently dropped 1/ε poles emerging from
endpoint-regulated integrals.  Beta-like integrands such as
`x^(-1+eps) (1-x)^eps` returned `O[eps]^1` instead of
`Gamma[eps] Gamma[1+eps] / Gamma[1+2 eps] = 1/eps + 0 - (Pi^2/6) eps + O(eps^2)`.

### Fixed

- **Bounded `STIntegrate[..., {x, 0, 1}, ...]` recovers endpoint poles.**
  The pre-fix path (`stIntegrateBoundedEps`) eps-expanded the integrand
  *before* integration and handed each non-negative ε-coefficient to
  `HyperIntica` with the user's bounds intact.  This loses the regulator
  on integrals where ε lives in an endpoint exponent: the ε⁰ coefficient
  of `x^(eps-1)(1-x)^eps` is `1/x`, and `HyperIntica[1/x, {x,0,1}]` silently
  collapses to 0 via `ZeroInfPeriod -> ZeroInfPeriodAsMpl // FullSimplify`,
  so the 1/ε pole that should have emerged from regulating ∫dx/x at x=0
  is invisible.  The new path applies a change of variables that maps
  each bounded interval to (0, ∞)

  ```
  {x, 0, 1}        ->  x = t/(1+t),  dx = dt/(1+t)^2
  {x, 1, Infinity} ->  x = 1 + t,    dx = dt
  ```

  and tail-recurses `STIntegrate` with all-(0, ∞) limits.  The standard
  projective pipeline (`STEvaluateEulerIntegral` -> `STExpandIntegral`)
  then sector-decomposes endpoint regulators and recovers the poles
  correctly.  Regression test: `scripts/test_bounded_cov_2026-05-28.wl`
  (the Beta integral plus two single-factor pole cases).

### Changed

- **`stIntegrateBoundedEps` removed; `stBoundedToInfinity` added.**  The
  ~80-line expand-then-integrate helper is replaced by a ~25-line CoV
  helper.  Its two diagnostic messages (`STIntegrate::boundedCoeffFailed`,
  `STIntegrate::boundedSeriesFailed`) are no longer reachable and have
  also been removed.
- **HyperFLINT prebuilt distribution rebuilt for 1.1.11.2** (`release-tuned`,
  `-mcpu=apple-m4`, static FLINT).

---

## [1.1.11.1] — 2026-05-27

Robustness fixes around `MethodLR` / pinned-gauge integration and the
HyperFLINT LR backend.  No user-facing API changes; existing scripts
keep working unchanged.

### Fixed

- **Pinned-gauge `Fast` -> `Standard` (+ `FindRoots` -> True) recovery.**
  In `STEvaluateGraph`'s non-automatic-gauge branch, the first
  `STfindLinearlyReducibleOrders2` call now traps the `STEspressoFubini::noorder`
  `Abort[]` and, when still on `MethodPolysAndPairs -> "Fast"`, rebuilds
  under `"Standard"` and retries with `FindRoots -> True` (the maximal
  permitted combo) before declaring NOLR.  Mirrors what the gauge-scan
  path already achieves via the scoring-time Fast -> Standard rerun.
  A genuine NOLR under `"Standard"` re-aborts as before.  The fix is
  also mirrored in `STEvaluateEulerIntegral`'s NO-SCAN branch.
  Previously, `STIntegrate[..., "Gauge" -> {x_i -> 1}, ...]` could
  `$Abort` on diagrams the scan path handles fine (e.g. the off-shell
  massless-leg hexagon under any pinned gauge).
- **`stDispatchFubini2` falls back to HyperIntica on a HyperFLINT NOLR
  verdict, not only on a hard `$Failed`.**  HF's step-strategy routing
  can NOLR a face that `STFasterFubini2` reduces (observed on
  pinned-gauge hexagon faces), so its NOLR is now cross-checked with
  HyperIntica before being trusted.  New helper `stLRResultNOLRQ`
  detects NOLR in both FindRoots-shape conventions
  (`{NOLR, Infinity}` and `{{NOLR, Infinity}, _}`).

### Changed

- **HyperFLINT prebuilt distribution rebuilt for 1.1.10.1.**  The
  previous `dist/macos-arm64/hyperflint` was staged for
  `SubTropica.wl 1.1.8.12` and emitted no `schema_version` /
  `hf_version` in its `eval-json` response, triggering
  `STFindLROrdersHF::schemamismatch` and `versionmismatch` warnings.
  Rebuilt with the `release-tuned` preset
  (`-mcpu=apple-m4`, static FLINT) at `HF_VERSION=1.1.10.1`, staged
  via `HyperFLINT/scripts/stage_dist.sh`.

### Documentation

- **`docs/antropica-usage.md`** — practical guide to the three
  `STIntegrate` options that drive AnTropica rationalization
  (`MethodLR -> "AnTropica"`, `"AutoRationalize" -> True`,
  `FindRoots -> "AnTropica"`): what each one does, when to use which,
  diagnostics, and current limitations.

---

## [1.1.9] — 2026-05-21

Library expansion and a new in-UI request-paper workflow.  No kernel
API changes; user-visible additions are concentrated in the library
browser and the submission pathway.

### Added

- **"Request paper analysis" in the library browser.**  An empty-state
  toast appended to the bottom of the library list (also shown when a
  search yields no matches) opens a modal that takes an arXiv ID plus
  an optional reason / name / email.  Submitting relays the request
  through a Cloudflare Worker (`/request-paper`) and a GitHub Actions
  workflow that files a public issue on the SubTropica repository
  with the `paper-request` label.  Repeated submissions of the same
  arXiv ID within 24 h are deduplicated.
- **Spiering, Wilhelm, Zhang (arXiv:2406.15549) ingest** — 3 new
  library entries from two-loop intersection-theory examples.
- **Bargiela et al. (arXiv:2512.13794) ingest** — 79 new library
  entries covering a parametric two-loop family `I_{a,b,c}` (with
  starred variants).
- **`function_class: "higher_genus"` schema extension** for integrals
  whose period structure is a Riemann surface of genus g > 1.  Two
  optional companion fields: `genus` (positive integer) and
  `hyperelliptic_confirmed` (boolean).  Existing elliptic entries
  carry `genus: 1` automatically.
- **Marquee `PrimaryName` curation** for 7 newly-introduced frontier
  topologies.

### Changed

- **Inline LaTeX renders correctly in library titles and
  descriptions.**  146 topology / configuration titles and 79
  record descriptions had their embedded math (e.g. `I_{4,3,3}`,
  `M^2 - s_{12} - s_{23}`) wrapped with `$...$` so the UI typesets
  them via KaTeX.  Previously the same fragments were shown as
  raw LaTeX source.  Affected fields: `Name`, `PrimaryName`,
  `CanonicalName`, `Names[]`, and `Records[].description`.
- **`PrimaryName` now reaches the UI.**  The library.json builder
  was silently dropping `PrimaryName` on rollup; 327 curated names
  authored across previous releases had never been visible.  The
  aggregator now emits both `name` and `primaryName` per topology.
- **110 duplicate paper records auto-removed** across 72 entries
  (same `texkey`, identical or near-identical descriptions; the
  highest-quality record is kept).
- **19 new paper thumbnails** added.
- **Community submissions**: 3 results from PRs #30 and #33 on the
  public repo (`e12|e2|e|` and `e12|e3|e3|e|` two-loop one-mass
  families).

### Fixed

- **Spurious n=1 "Sunrise tadpole" topology removed.**  The entry at
  `111|e|:000|0|` was physically impossible (with one external leg,
  momentum conservation forces $p \to 0$, contradicting the cited
  $k_1^2 = M^2$).  The record (Zhang, arXiv:1612.02249, Exercise
  3.1) was describing the n=2 self-energy `e111|e|:1000|1|`; it
  has been migrated to the canonical entry, which now carries 9
  references.
- **Machine-generated audit text scrubbed from descriptions**: 158
  entries had inlined correction blocks and internal classifier
  tags pasted into `Records[].description`; the audit trail now
  lives in `Records[].structural.corrections[]`.

---

## [1.1.8] — 2026-04-27

UI and numerical-verification stability release.  Closes a systematic
30-case regression campaign over the live UI → kernel → backend path.

### Added

- **Per-call FIESTA `DataPath` isolation.**  `STNIntegrate[..., Method ->
  "FIESTA"]` now writes each call into its own scratch directory; back-to-back
  FIESTA verifications no longer trip on stale workspace artefacts.
- **`STVerify[..., MaxTime -> N]`.**  Per-call wall-clock cap, propagated to
  every backend's own time controls and to the outer `TimeConstrained`
  boundary.
- **Notebook output is no longer flooded by UI activity.**  `Print` / `Echo`
  emitted by request handlers (and by third-party numerical backends) is
  redirected to a per-session `kernel.log`; the JSON response file
  remains the single user-visible channel.

### Changed

- **Library "Load to editor" pre-fills the diagram name** with the per-config
  `CanonicalName` (e.g. *"double box, 1 mass"*), falling back to the
  topology-level name.
- **UI default LR algorithm is `Lungo`** in the Advanced panel; the old
  `Espresso` selection in the HTML disagreed with the JS default and could
  silently flip the active method on first interaction.
- **Export tab no longer emits `MethodLR` / `FindRoots` options when they
  match the current defaults** — only non-default options surface in the
  emitted `STIntegrate` command.
- **Library thumbnails: shorter external legs.**  Force-layout shrink applied
  to leg vertices in thumbnails so the diagram body dominates small previews.
- **UI multi-slot leg masses now emit kernel-canonical `M[v]` form**, matching
  `STCNickelToGraph`.  Single-slot legs still emit the bare symbol `M`.

### Fixed

- **`handleIntegrate` / verify IPC handlers surface the real exception** from
  the inner `STIntegrate` / `STVerify` call instead of a generic
  *"STIntegrate crashed"* / *"STVerify crashed"* string.  Errors like
  `STEspressoFubini::noorder`, `Symbolic::ivar`, and `STSymanzik::nodim`
  now reach the UI banner and the friendly-error classifier.
- **AMFlow result-shape and shared/massless-leg handling.**  AMFlow's IBP/DE
  output was being parsed in one shape on physical inputs and a different
  shape on shared-mass / massless-leg edge cases; both are now normalized
  before comparison against the analytic series.
- **`STToGinsh` `N[]` fallback no longer produces silently-wrong numbers.**
  The fallback path (used when ginsh isn't on `$Path`) skipped the
  `Sqrt[...]` unwrapping step, returning numerically incorrect values
  without any error.  The fallback now asserts the same prerequisites
  ginsh does and refuses to run if they don't hold.
- **`ConfigureSubTropica[<partial>]` no longer clobbers globals.**  Partial
  calls (e.g. `ConfigureSubTropica[PythonPath -> "..."]`) were resetting
  every unspecified global to its package default; the setter now writes
  only the arguments the caller passed.
- **Several UI shared-mass topologies (1-mass triangles, boxes) no longer
  return empty `SeriesData`.**  Two independent bugs were fused: the UI
  was emitting `Subscript[M, slot]` for multi-slot leg masses (kernel
  expected `M[v]`), tripping `stFindEuclideanRegion`'s head-based check
  and selecting a degenerate kinematic point; and the kernel-side
  memoization cache key ignored `MassConfig`, so a stale hit from a
  prior different-mass run shadowed the recomputation.  Both ends fixed;
  the misleading reason string *"shared-mass on-shell — unsupported"*
  is replaced by *"no Euclidean kinematic point found."*
- **UI ×MPL false-negative on diagrams that need rationalization.**  The
  "Check if MPL" button reported ×MPL on topologies whose Symanzik
  polynomial requires fibered rationalization first, even though a route
  to MPLs exists once `FindRoots` is enabled; the Tier-2 estimate now
  consults `FindRoots` state before issuing the verdict.


## [1.1.4] — 2026-04-23

### Fixed

- **`STSubmitResult` from a notebook** no longer aborts with "no edges/nodes
  available." `stGateVerification` now reads the graph from
  `$integrationResult["uiResult"]` (populated by notebook `STIntegrate`) and
  falls back to `$integrationConfig` only for the UI/HTTP path.
- **`STSubmitResult` local dedup** now canonicalizes dedup-key fields before
  comparison. Previously `epsOrder` ("0" vs 0), `dimension` (`"4 - 2*eps"` vs
  `"4-2*eps"`), and `substitutions` (`""` vs `"{}"`) were compared with `===`
  and diverged on type or whitespace, causing spurious submissions of results
  already in `library-bundled/`.
- **`STNIntegrate[..., Method -> "FIESTA"]` with imaginary internal/external
  mass labels** no longer trips `Greater::nord` inside FIESTA's sector
  decomposition. Auto-detection of `ComplexMode` now uses
  `TrueQ[Positive[...]]`, which correctly returns `False` for
  `I*Sqrt[|Msq|]` entries coming from `stMakeVerificationPoint` in the
  Euclidean region.

## [1.1.0] — 2026-04-22

First public release of SubTropica — a Mathematica package for computing
Feynman integrals via tropical geometry.  The package automates the tropical
subtraction algorithm end-to-end: from a Feynman diagram (drawn in a GUI,
a graph topology, a propagator list, or a raw Euler integrand) to an analytic
expression in terms of hyperlogarithms, multiple polylogarithms, and MZVs.

Companion paper:
M. Giroux, S. Mizera, G. Salvatori, *SubTropica*,
[arXiv:2604.20954](https://arxiv.org/abs/2604.20954) [hep-th].

### Package at a glance

- **Core integrator.** `STIntegrate` is the single entry point for the
  full pipeline.  It accepts five input forms — interactive GUI, graph
  topology `{edges, nodes}`, propagator list, Mathematica-style `{x,a,b}`
  limits, and pre-built Euler quadruples `{pref, integrand, xvars, coeffs}`.
  Options cover dimension, ε-order, gauge fixing, heuristics, parallelism
  granularity, algebraic-letter handling, and post-processing.
- **Tropical subtraction engine.** Automatic Newton-polytope analysis,
  tropical subtraction scheme, singular subtractions, and ε-expansion for
  generic Euler integrals.  Handles logarithmic and power divergences;
  supports explicit and automatic Nilsson–Passare continuation for cases
  outside the geometric locus.
- **HyperIntica.** A native Mathematica reimplementation of
  [HyperInt](https://arxiv.org/abs/1401.4361): hyperlogarithm integration
  with linear reducibility analysis, gauge scoring heuristics, parallel
  face-by-face integration, and an internal MZV lookup table.
- **Numerical backends.**  `STNIntegrate` and `STVerify` route to four
  independent backends — [pySecDec](https://github.com/gudrunhe/secdec),
  [FIESTA](https://bitbucket.org/feynmanIntegrals/fiesta),
  [AMFlow](https://gitlab.com/multiloop-pku/amflow) (FiniteFlow+LiteRed or
  FIRE+LiteRed), and [feyntrop](https://github.com/michibo/feyntrop) —
  for independent numerical verification of analytic results.
- **Finite-field pipeline.**  Optional
  [FiniteFlow](https://github.com/peraro/finiteflow) +
  [SPQR](https://github.com/Giu989/SPQR) backend for finite-field
  arithmetic on partial fractions, avoiding intermediate expression swell.
- **Interactive GUI.**  Browser-backed diagram editor launched by calling
  `STIntegrate[]` with no arguments — draw the graph, assign masses, set
  options, integrate, and inspect timings, kernel logs, and symbol output
  in tabbed panels.  The Export panel emits ready-to-paste `STIntegrate`
  commands in three input forms (graph, propagator list, Euler quadruple)
  and is fully client-side, so it renders in real time as you edit.
- **Non-blocking UI.**  `STBrowser[]` starts the local server and opens the
  UI in your default browser without spawning the native viewer, parallel
  kernels, or the main polling loop.  The evaluation cell returns
  immediately, letting you use the library browser, Review tool, and
  Export panel in parallel with other notebook work.  Use `STIntegrate[]`
  when you need the UI to call back into Mathematica for integration;
  `STStop[]` shuts everything down.
- **Companion library.**  A curated library of Feynman integrals ships
  with the package under `library-bundled/` (one directory per Nickel
  canonical topology × mass configuration).  The web front-end at
  [subtropi.ca](https://subtropi.ca) provides real-time topology matching
  against this library.  Current inventory:
  - **314** canonical topologies
  - **731** mass configurations
  - **178** computed symbolic results (all numerically verified against
    pySecDec or FIESTA where applicable)
  - **1,283** arXiv papers scanned for references

### Notation and conventions

- **Measure:** `dx / x` internal convention with explicit flattening at
  the user-facing API surface; see `docs/README.md` §2 for the full
  convention flow-chart.
- **Symanzik polynomials:** `U > 0`, `F ≤ 0` in the Euclidean region
  (both sign conventions supported internally); see
  [`notes/normalization.tex`](notes/normalization.tex) for the audit.
- **Algebraic letters:** `Wm[i]` / `Wp[i]` pairs carried as atoms in the
  returned series; explicit root substitutions available via
  `GetAlgebraicBackSubRules[]`.
- **Mass scales:** internal mass > external mass > Mandelstam, with a
  canonicalized symbol alphabet (`W_i` labels, trivial-1 drop, no
  compound `W` expressions).
- **Nickel index:** Mathematica-canonical first-appearance digit
  assignment throughout the library.

### Library & web companion

- 1,283 arXiv papers scanned via the extraction pipeline
  (`process_arxiv_papers` → validator → `extracted_to_library.wl`).
- Per-topology canonical names via `data/topology_names.json` (Tier-1 +
  compound chains + fallback).
- Per-diagram canonical names computed via the library-audit pipeline.
- Unphysical topologies (disconnected graphs, valence-≤ 2) quarantined
  under `library-quarantined/`.
- Submissions of new results: either push a PR to the public repo or use
  the Cloudflare Worker endpoint behind the `Submit` button in the web UI
  (auto-opens a curated GitHub PR).

### Numerical-verification toolchain

- **STVerify** — evaluates symbolic result at an auto-generated Euclidean
  kinematic point and compares against a numerical backend.  Detects
  non-Euclidean regions, handles shared on-shell/internal masses per
  backend, resolves the iε sheet via the `conjugate-fallback` pass, and
  auto-resolves delta-sign algebraic-letter ambiguities introduced by
  `FindRoots`.
- **Backends:** pySecDec + FIESTA + AMFlow (FF+LR or FIRE+LR) + feyntrop.
  Each is routed through its own compilation / IBP / sector-decomposition
  pipeline; STVerify forwards only user-supplied options to preserve
  backend-specific defaults.

### Installation

```mathematica
PacletInstall["https://subtropi.ca/SubTropica.paclet"]
Needs["SubTropica`"]
```

`ConfigureSubTropica[…]` persists tool paths (polymake, ginsh, pySecDec,
FIESTA, AMFlow/LiteRed/FIRE, feyntrop, FiniteFlow/SPQR, Maple+HyperInt)
in `$UserBaseDirectory`.  The package auto-detects tools on `$Path`, so
most users only set `PolymakePath` once.

### Requirements

- Mathematica 13.1+ (tested through 14.2)
- polymake ≥ 4.0 (required for Newton-polytope computations)
- Python ≥ 3.8 (required for the GUI and pySecDec driver)
- Optional: FiniteFlow, SPQR, pySecDec, FIESTA, AMFlow, feyntrop, ginsh,
  Maple + HyperInt — each enables a specific backend or convenience
  feature.  `STBenchmark[]` reports which dependencies are live.

### License

- **Code:** MIT ([`LICENSE`](LICENSE))
- **Curated library data:** CC BY-NC-SA 4.0 ([`LICENSE-DATA`](LICENSE-DATA))

### Citation

If SubTropica contributes to work you publish, please cite the companion
paper above and (optionally) the paclet itself:

```
@software{SubTropica,
  author  = {Giroux, Mathieu and Mizera, Sebastian and Salvatori, Giulio},
  title   = {{SubTropica}: Feynman Integrals via Tropical Geometry},
  version = {1.1.0},
  year    = {2026},
  url     = {https://subtropi.ca}
}
```
