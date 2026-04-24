# Changelog

All notable public-facing changes to [SubTropica](https://subtropi.ca) are
documented in this file.  The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

---

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
