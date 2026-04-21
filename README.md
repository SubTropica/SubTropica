# 🥥 SubTropica

[![Version](https://img.shields.io/badge/version-1.1.0-blue)](https://github.com/SubTropica/SubTropica)
[![Mathematica](https://img.shields.io/badge/Mathematica-12.1%2B-red)](https://www.wolfram.com/mathematica/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Data: CC BY-NC-SA 4.0](https://img.shields.io/badge/data_license-CC_BY--NC--SA_4.0-orange)](LICENSE-DATA)
[![Website](https://img.shields.io/badge/web-subtropica.org-purple)](https://subtropica.org)

A Mathematica package for computing Feynman integrals via tropical geometry. SubTropica automates the tropical subtraction algorithm — from drawing a diagram to obtaining an analytic result in terms of multiple polylogarithms — through a single function call or an interactive GUI.

> **Paper:** M. Giroux, S. Mizera, G. Salvatori, *SubTropica*, arXiv:26XX.XXXXX [hep-th]

All code snippets quoted in the paper are collected and checked in the accompanying notebook checksPaper.wl.

## Features

- **Tropical subtraction** — Newton polytope analysis, singular subtraction, and epsilon expansion for generic Euler integrals
- **HyperIntica** — built-in integration engine for hyperlogarithms (a native Mathematica reimplementation of [HyperInt](https://arxiv.org/abs/1401.4361))
- **Finite-field arithmetic** — optional [FiniteFlow](https://github.com/peraro/finiteflow) + [SPQR](https://github.com/Giu989/SPQR) backend to avoid intermediate expression swell in partial fractions
- **Interactive GUI** — draw Feynman diagrams, assign masses, configure options, and integrate, all from a graphical interface launched with `STIntegrate[]`
- **Multiple input formats** — Feynman graphs, propagator lists with numerators, or raw Euler integrands
- **Parallelized pipeline** — automatic GL(1) gauge fixing, linear reducibility analysis, tropical subtraction scheme, and parallel integration of hyperlogarithms

## Online version & library

An online companion is hosted at **[subtropica.org](https://subtropica.org)**. It provides a browser-based diagram editor with real-time topology matching against a curated library of known Feynman integrals.

The library currently contains:

| | |
| **Topologies** | 313 |
| **Mass configurations** | 731 |
| **Papers scanned** | 1283 |
| **Computed results** | 181 |

The full library ships with the package under `library-bundled/` and is compiled into `ui/library.json` for the web interface.

## Installation

### Install the paclet

The recommended install path for end users is the stable paclet endpoint:

```mathematica
PacletInstall["https://subtropi.ca/SubTropica.paclet"]
```

After install, load with `Needs["SubTropica`"]`. Upgrades happen automatically on the next `PacletInstall` call (the `.paclet` archive carries the version).

### Development install from source

If you plan to modify the package or track `main` between paclet releases:

```bash
git clone https://github.com/SubTropica/SubTropica.git
```

Add the cloned directory to Mathematica's `$Path`, then load the package:

```mathematica
Get["SubTropica`"];
```

### Configuration

SubTropica uses several external tools. Configure them with:

```mathematica
ConfigureSubTropica[
  (* required *)
  PolymakePath   -> "/opt/homebrew/bin/polymake",
  (* recommended *)
  FiniteFlowPath -> "path/to/FiniteFlow",
  SPQRPath       -> "path/to/SPQR",
  (* optional *)
  GinshPath      -> "path/to/ginsh",
  MaplePath      -> "path/to/maple"
];
```

The configuration is persisted (`$UserBaseDirectory/Kernel/SubTropicaConfig.m`) and reapplied on every subsequent load, so `ConfigureSubTropica` needs to be called only once per machine. Clear it with `STResetConfig[]`.

### Dependencies

| Dependency | Required? | What it does |
|---|---|---|
| [polymake](https://polymake.org/) | **Yes** | Newton polytope computations. On macOS: `brew install polymake` |
| [FiniteFlow](https://github.com/peraro/finiteflow) | Recommended | Finite-field arithmetic for partial fractions |
| [SPQR](https://github.com/Giu989/SPQR) | Recommended | Polynomial quotient via finite-field reconstruction (used with FiniteFlow) |
| [pySecDec](https://github.com/gudrunhe/secdec) / [FIESTA](https://github.com/compphys-sbras/FIESTA) / [AMFlow](https://gitlab.com/multiloop-pku/amflow) / [feyntrop](https://github.com/michibo/feyntrop) | Optional | Numerical cross-checks via `STNIntegrate` / `STVerify` |
| [ginsh](https://www.ginac.de/) | Optional | Numerical evaluation of hyperlogarithms |
| [Maple](https://www.maplesoft.com/products/maple/) | Optional | Symbolic cross-checks |

If FiniteFlow and SPQR are already on Mathematica's `$Path`, the package detects and loads them automatically — no need to set `FiniteFlowPath`/`SPQRPath` in that case. After configuration, verify the install with `STBenchmark[]`.

## Architecture

SubTropica consists of three modules:

1. **Tropical subtraction** — Newton polytope decomposition, tropical subtraction scheme, singular subtractions, and epsilon expansion
2. **HyperIntica** — hyperlogarithm integration engine with linear reducibility analysis and MZV lookup tables
3. **Feynman graph interface** — diagram drawing/plotting (`FeynmanDraw`, `FeynmanPlot`), automatic kinematic setup, and the unified `STIntegrate` entry point that orchestrates the full pipeline

## Quick start

### GUI mode

Call `STIntegrate` with no arguments to open the interactive interface:

```mathematica
STIntegrate[]
```

This launches a canvas where you can draw a Feynman diagram, assign masses and numerators, set the spacetime dimension and epsilon order, and run the integration — all without writing additional code. The GUI returns both the result and a reproducible `STIntegrate[...]` command for scripted use.

### Feynman graph input

Specify a diagram via edge and node lists (following the [SOFIA](https://arxiv.org/abs/2503.16601) convention):

```mathematica
edges = {{{1, 2}, m}, {{2, 3}, 0}, {{3, 4}, 0}, {{1, 4}, 0}};
nodes = {{1, m}, {2, 0}, {3, 0}, {4, M}};

STIntegrate[{edges, nodes}]
```

Mandelstam invariants (`s12`, `s23`, ...) are assigned automatically in a cyclic basis. Masses are squared into symbols: `m` → `mm`, `M` → `MM`.

### Propagator list input

Pass propagators and numerators directly, with exponents controlling which factors are propagators (positive) and which are numerators (negative):

```mathematica
STIntegrate[
  {l[1]^2 - m^2, (l[1] - p[1])^2, (l[1] - p[1] - p[2])^2,
   (l[1] - p[1] - p[2] - p[3])^2, q l[1]},
  "Exponents" -> {1, 1, 1, 1, -1},
  "Substitutions" -> {M[1] -> m, M[2] -> 0, M[3] -> 0, M[4] -> M}]
```

### Generic Euler integrals

For integrals not tied to a Feynman diagram, use Mathematica-style integration limits:

```mathematica
STIntegrate[integrand, {x, 0, 1}, {y, 0, Infinity}, opts]
```

Bare symbols default to the range [0, ∞):

```mathematica
STIntegrate[integrand, x, y, opts]
```

## Documentation

Full API documentation ships with the paclet and is indexed into Mathematica's Documentation Center. After install:

- `PacletDocumentation["SubTropica"]` opens the guide page, which lists every documented symbol grouped by topic (entry points, tropical subtraction, HyperIntica, numerical evaluation, setup, library).
- Press **F1** on any SubTropica symbol in a notebook to open its reference page directly.
- The guide's Installation section mirrors this README's and links to `ConfigureSubTropica`, `STVerify`, `STNIntegrate`, `STDependencies`.

If you installed from source (no paclet), reference pages live under `Documentation/English/ReferencePages/Symbols/` and the guide at `Documentation/English/Guides/SubTropica.nb`.

## Authors

Mathieu Giroux, Sebastian Mizera, Giulio Salvatori

## Acknowledgments

Development of SubTropica was assisted by Claude Opus 4.6-7.

## License

- **Code** (SubTropica.wl, UI, scripts): [MIT License](LICENSE)
- **Library data** (library-bundled/, ui/library.json): [CC BY-NC-SA 4.0](LICENSE-DATA) — see [LICENSE-DATA](LICENSE-DATA) for details, including restrictions on machine learning use

## Citation

If you use SubTropica in your research, please cite the paper:

```bibtex
@article{Giroux:2026xxx,
    author  = "Giroux, Mathieu and Mizera, Sebastian and Salvatori, Giulio",
    title   = "{SubTropica}",
    eprint  = "26XX.XXXXX",
    archiveprefix = "arXiv",
    primaryclass  = "hep-th",
    year    = "2026"
}
```
