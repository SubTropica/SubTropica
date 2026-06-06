# 🥥 SubTropica

[![Version](https://img.shields.io/badge/version-1.2.2-blue)](https://github.com/SubTropica/SubTropica)
[![Mathematica](https://img.shields.io/badge/Mathematica-13.1%2B-red)](https://www.wolfram.com/mathematica/)
[![License: MIT](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![Data: CC BY-NC-SA 4.0](https://img.shields.io/badge/data_license-CC_BY--NC--SA_4.0-orange)](LICENSE-DATA)
[![Website](https://img.shields.io/badge/web-subtropi.ca-purple)](https://subtropi.ca)

A Mathematica package for computing Feynman integrals via tropical geometry. SubTropica automates the tropical subtraction algorithm — from drawing a diagram to obtaining an analytic result in terms of hyperlogarithms and multiple polylogarithms — through a single function call or an interactive GUI.

> **Paper:** M. Giroux, S. Mizera, G. Salvatori, *SubTropica*, [arXiv:2604.20954](https://arxiv.org/abs/2604.20954) [hep-th].

Every code listing from the paper is reproduced and checked in [`PaperChecks.wl`](PaperChecks.wl) at the repository root — evaluate it end-to-end after ``Needs["SubTropica`"]`` to regenerate all quoted outputs.

## Features

- **Tropical subtraction** — Newton polytope analysis, singular subtraction, and epsilon expansion for generic Euler integrals
- **HyperIntica** — built-in integration engine for hyperlogarithms (a native Mathematica reimplementation of [HyperInt](https://arxiv.org/abs/1401.4361))
- **Finite-field arithmetic** — optional [FiniteFlow](https://github.com/peraro/finiteflow) + [SPQR](https://github.com/Giu989/SPQR) backend to avoid intermediate expression swell in partial fractions
- **Interactive GUI** — draw Feynman diagrams, assign masses, configure options, and integrate, all from a graphical interface launched with `STIntegrate[]`.
- **Multiple input formats** — Feynman graphs, propagator lists with numerators, or raw Euler integrands
- **Parallelized pipeline** — automatic GL(1) gauge fixing, linear reducibility analysis, tropical subtraction scheme, and parallel integration of hyperlogarithms
- **HyperFLINT**: optional C++17/FLINT analytic backend for hyperlogarithm integration (macOS/Linux; install the `HyperFLINT` add-on paclet)

## Online version & library

An online companion is hosted at **[subtropi.ca](https://subtropi.ca)**. It provides a browser-based diagram editor with real-time topology matching against a curated library of known Feynman integrals.

The library currently contains:

|                         |       |
| :---------------------- | ----: |
| **Topologies**          | 361 |
| **Mass configurations** | 861 |
| **Literature records**  | 1,587 |
| **Papers scanned**      | 1,298 |
| **Computed results**    | 193 |

The full library ships with the package under `library-bundled/` and is compiled into `ui/library.json` for the web interface.

## Installation

### Install the paclet

The recommended install path for end users is the stable paclet endpoint:

```mathematica
PacletInstall["https://subtropi.ca/SubTropica.paclet"]
```

After install, load with ``Needs["SubTropica`"]``. Upgrades happen automatically on the next `PacletInstall` call (the `.paclet` archive carries the version).

#### Optional: the HyperFLINT backend (macOS)

HyperFLINT is a fast analytic backend (a C++17 reimplementation of the hyperlogarithm algorithm on FLINT). It does **not** ship with the core paclet — install the add-on once:

```mathematica
PacletInstall["https://subtropi.ca/HyperFLINT.paclet"]
```

Once installed, HyperFLINT is picked up automatically and becomes the **default** symbolic integrator and LR-order search engine of `STIntegrate` (since v1.2.2) — no options needed. Check availability with ``SubTropica`$HyperFLINTAvailable`` (the `[✓] HyperFLINT` banner badge shows the same). Pass `"Integrator" -> "HyperIntica"` / `"LROrderBackend" -> "HyperIntica"` to revert to the built-in engine for a call; without the add-on, `STIntegrate` uses the built-in HyperIntica engine throughout. The add-on bundles its native libraries, so there is nothing else to install. Supported platforms: macOS arm64/x86_64 and Linux x86-64 (Windows users get the core paclet and the HyperIntica engine).

If a browser-downloaded paclet is blocked by Gatekeeper, clear the quarantine flag once on the installed add-on directory (its location is ``PacletObject["HyperFLINT"]["Location"]``):

```bash
xattr -dr com.apple.quarantine ~/Library/Wolfram/Paclets/Repository/HyperFLINT*
```

To build HyperFLINT from source (developers, or other platforms), see [`HyperFLINT/README.md`](HyperFLINT/README.md): the build needs `brew install flint gmp mpfr libomp mimalloc` plus CMake.

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
  PolymakePath     -> "/opt/homebrew/bin/polymake",
  (* recommended *)
  FiniteFlowPath   -> "path/to/FiniteFlow",
  SPQRPath         -> "path/to/SPQR",
  (* optional — numerical backends for STNIntegrate / STVerify *)
  PythonPath       -> "path/to/python3",
  FIESTAPath       -> "path/to/fiesta/FIESTA5",
  AMFlowPath       -> "path/to/amflow",
  LiteRedPath      -> "path/to/LiteRed",
  FIREPath         -> "path/to/fire/FIRE6",
  FeyntropPath     -> "path/to/feyntrop",
  (* optional — IBP ecosystem *)
  KiraPath         -> "path/to/kira",
  NeatIBPPath      -> "path/to/NeatIBP",
  SingularPath     -> "/opt/homebrew/bin/Singular",
  FermatPath       -> "path/to/fer64",
  FormPath         -> "path/to/form",     (* auto-detected from $PATH if omitted *)
  (* optional — analytic / canonical-basis tools *)
  PolyLogToolsPath -> "path/to/PolyLogTools",
  LibraPath        -> "path/to/Libra",
  DiffExpPath      -> "path/to/diffexp",
  (* optional — Groebner backend + symbolic evaluators *)
  MsolvePath       -> "msolve",           (* resolved from PATH if omitted *)
  IterIntPath      -> "path/to/iterint_mpfr",
  (* optional — alternative integrators *)
  GinshPath        -> "path/to/ginsh",
  MaplePath        -> "path/to/maple",
  HyperIntPath     -> "path/to/HyperInt.mpl"
];
```

The configuration is persisted (`$UserBaseDirectory/Kernel/SubTropicaConfig.m`) and reapplied on every subsequent load, so `ConfigureSubTropica` needs to be called only once per machine. Clear it with `STResetConfig[]`.

### Dependencies

| Dependency | Required? | What it does |
|---|---|---|
| [polymake](https://polymake.org/) | **Yes** | Newton polytope computations. On macOS: `brew install polymake` |
| [FiniteFlow](https://github.com/peraro/finiteflow) | Recommended | Finite-field arithmetic for partial fractions |
| [SPQR](https://github.com/Giu989/SPQR) | Recommended | Polynomial quotient via finite-field reconstruction (paired with FiniteFlow) |
| Python ≥ 3.8 | Optional | Interactive GUI (`STIntegrate[]`) and the pySecDec driver |
| [pySecDec](https://github.com/gudrunhe/secdec) / [FIESTA](https://bitbucket.org/feynmanIntegrals/fiesta) / [AMFlow](https://gitlab.com/multiloop-pku/amflow) / [feyntrop](https://github.com/michibo/feyntrop) | Optional | Numerical cross-checks via `STNIntegrate` / `STVerify` |
| [LiteRed](https://inp.nsk.su/~lee/programs/LiteRed/) / [FIRE](https://gitlab.com/feynmanintegrals/fire) | Optional | IBP reducers used by the AMFlow backend |
| [ginsh](https://www.ginac.de/) | Optional | Numerical evaluation of hyperlogarithms |
| [msolve](https://msolve.lip6.fr) | Optional | Groebner-basis backend for experimental linear-reducibility tooling; `brew install msolve` |
| [IterInt](https://github.com/baugid/IterInt) | Optional | Iterated-integral symbolic evaluator for `STVerify` (`SymbolicEvaluator -> "iterint"`); building its driver needs GSL, Boost, MPFR, MPC (`brew install gsl boost mpfr libmpc`) |
| [Maple](https://www.maplesoft.com/products/maple/) + [HyperInt](https://bitbucket.org/PanzerErik/hyperint) | Optional | Alternative integrator (`"Integrator" -> "HyperInt"`) |
| GNU `make` ≥ 4, `curl` | System | pySecDec builds (`make`); library sync / submission (`curl`) |
| HyperFLINT add-on | Optional | Fast analytic backend (`"Integrator" -> "HyperFLINT"`); macOS arm64/x86_64 + Linux x86-64; `PacletInstall` the `HyperFLINT` paclet |

If FiniteFlow and SPQR are already on Mathematica's `$Path`, the package detects and loads them automatically — no need to set `FiniteFlowPath`/`SPQRPath` in that case. After configuration, verify the install with `STBenchmark[]`.

Most of the open-source tools above can be installed for you: `STInstallDependencies[]` (or the *install missing packages* link that appears under the welcome banner after a new install) runs the documented install commands — Homebrew formulas, the pySecDec pip install, the IterInt driver build — after showing you exactly what it will execute. Tools that need a license or a manual download (Maple, Fermat, FIESTA, AMFlow, Kira) print their install instructions instead.

#### Optional IBP ecosystem tools

These tools are auto-detected and shown in the welcome banner; none is required by SubTropica itself.

| Tool | What it does | Source | `ConfigureSubTropica` option |
|---|---|---|---|
| [Kira](https://gitlab.com/kira-pyred/kira) | IBP reduction via Liteweight algorithm | `gitlab.com/kira-pyred/kira` | `KiraPath -> "/path/to/kira"` |
| [FireFly](https://gitlab.com/firefly-library/firefly) | Rational-function reconstruction (Kira backend) | Ships bundled with Kira; build with `-DWITH_FIREFLY=ON` | No separate path; detected via Kira |
| [Fermat](https://home.bway.net/lewis) | Polynomial arithmetic backend used by FireFly | Prebuilt binaries at `home.bway.net/lewis` | `FermatPath -> "/path/to/fer64"` |
| [NeatIBP](https://github.com/yzhphy/NeatIBP) | IBP generation via syzygy method | `github.com/yzhphy/NeatIBP` | `NeatIBPPath -> "/path/to/NeatIBP"` |
| SpaSM | Sparse RREF linear algebra (NeatIBP dependency) | Built as part of NeatIBP setup; produces `spasm_macos/libspasm.dylib` | No separate path; located under `NeatIBPPath` |
| [Singular](https://www.singular.uni-kl.de/) | CAS backend for NeatIBP's Groebner / syzygy computations | `brew install singular` or `www.singular.uni-kl.de` | `SingularPath -> "/path/to/Singular"` |
| [FORM](https://github.com/vermaseren/form) | Vermaseren's symbolic manipulator | `brew install form` or `github.com/vermaseren/form` | `FormPath -> "/path/to/form"` (auto-detected from `$PATH`) |
| [PolyLogTools](https://gitlab.com/hampel-classen/polylogtools) | Multiple polylogarithm and symbol manipulation | `gitlab.com/hampel-classen/polylogtools` | `PolyLogToolsPath -> "/path/to/PolyLogTools"` |
| [Libra](https://github.com/Jiaqi-Li-IBM/Libra) | Canonical differential equation form | `github.com/Jiaqi-Li-IBM/Libra` | `LibraPath -> "/path/to/Libra"` |
| [DiffExp](https://github.com/LinusHepp/DiffExp) | Differential-equation numeric transport | `github.com/LinusHepp/DiffExp` | `DiffExpPath -> "/path/to/diffexp"` |

Typical setup call for the IBP ecosystem:

```mathematica
ConfigureSubTropica[
  KiraPath         -> "/path/to/kira_install/bin/kira",
  NeatIBPPath      -> "/path/to/NeatIBP",
  SingularPath     -> "/opt/homebrew/bin/Singular",
  FermatPath       -> "/path/to/Ferm7a/fer64",
  PolyLogToolsPath -> "/path/to/PolyLogTools",
  LibraPath        -> "/path/to/Libra",
  DiffExpPath      -> "/path/to/diffexp"
];
```

`FormPath` and `SingularPath` are auto-detected from `$PATH` (Homebrew installs) if not specified. `FireFly` and `SpaSM` require no separate path option; they are located relative to `KiraPath` and `NeatIBPPath`, respectively.

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
nodes = {{1, 0}, {2, 0}, {3, 0}, {4, 0}};

STIntegrate[{edges, nodes}]
```

Internal-propagator masses (on edges) use `m, m[1], m[2], ...`; external-leg masses (on nodes) use `M, M1, M2, ...` — the latter are square roots of squared external momenta. Mandelstam invariants (`s12`, `s23`, ...) are assigned automatically in a cyclic basis. Masses are squared into symbols: `m → mm`, `M_i → MM_i`.

### Propagator list input

Pass propagators and numerators directly, with exponents controlling which factors are propagators (positive) and which are numerators (negative). Here the box above is rewritten as a propagator list with an added tensor numerator `ℓ₁·p₂`:

```mathematica
STIntegrate[
  {l[1]^2 - m^2, (l[1] - p[1])^2, (l[1] - p[1] - p[2])^2,
   (l[1] - p[1] - p[2] - p[3])^2, CenterDot[l[1], p[2]]},
  "Exponents" -> {1, 1, 1, 1, -1},
  "Substitutions" -> {M[1] -> 0, M[2] -> 0, M[3] -> 0, M[4] -> 0}]
```

Use `CenterDot` (the `·` operator, `Esc . Esc` in a notebook) for momentum dot products in numerators. The loop momentum `l[1]` is user-labelled, so the propagator list also pins the routing — useful when a specific tensor decomposition is wanted.

### Generic Euler integrals

For integrals not tied to a Feynman diagram, use Mathematica-style integration limits:

```mathematica
STIntegrate[integrand, {x, 0, 1}, {y, 0, Infinity}, opts]
```

Bare symbols default to the range [0, ∞):

```mathematica
STIntegrate[integrand, x, y, opts]
```

### Numerical cross-check

`STVerify` evaluates the symbolic result against a numerical backend (pySecDec by default; FIESTA / AMFlow / feyntrop on request) at an auto-generated Euclidean kinematic point and reports the relative error per ε order:

```mathematica
result = STIntegrate[{edges, nodes}];
STVerify[{edges, nodes}, result]
(* -> <|"pass" -> True, "maxRelErr" -> 6.4e-5, "kinPoint" -> {...}, ...|> *)
```

The same signature works for propagator-list and raw-quadruple inputs. Pass `"Method" -> "FIESTA"` (or `"AMFlow"` / `"feyntrop"`) to use a different backend.

## Documentation

Full API documentation ships with the paclet and is indexed into Mathematica's Documentation Center. After install:

- `PacletDocumentation["SubTropica"]` opens the guide page, which lists every documented symbol grouped by topic (entry points, tropical subtraction, HyperIntica, numerical evaluation, setup, library).
- Press **F1** on any SubTropica symbol in a notebook to open its reference page directly.
- The guide's Installation section mirrors this README's and links to `ConfigureSubTropica`, `STVerify`, `STNIntegrate`, `STBenchmark`.

If you installed from source (no paclet), reference pages live under `Documentation/English/ReferencePages/Symbols/` and the guide at `Documentation/English/Guides/SubTropica.nb`.

## Authors

Mathieu Giroux, Sebastian Mizera, Giulio Salvatori

## Acknowledgments

Development of SubTropica was assisted by Claude Opus 4.6-8.

## License

- **Code** (SubTropica.wl, UI, scripts): [MIT License](LICENSE)
- **Library data** (library-bundled/, ui/library.json): [CC BY-NC-SA 4.0](LICENSE-DATA) — see [LICENSE-DATA](LICENSE-DATA) for details, including restrictions on machine learning use
- **HyperFLINT** (`HyperFLINT/`): [MIT License](HyperFLINT/LICENSE); bundles statically-linked LGPL dependencies (FLINT, GMP, MPFR), see [`HyperFLINT/THIRD-PARTY-LICENSES`](HyperFLINT/THIRD-PARTY-LICENSES)

## Citation

If you use SubTropica in your research, please cite the paper:

```bibtex
@article{Giroux:2026tgd,
    author = "Giroux, Mathieu and Mizera, Sebastian and Salvatori, Giulio",
    title = "{SubTropica}",
    eprint = "2604.20954",
    archivePrefix = "arXiv",
    primaryClass = "hep-th",
    month = "4",
    year = "2026"
}
```
