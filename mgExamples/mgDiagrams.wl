(* ::Package:: *)

(* mgDiagrams.wl
   Curated STBenchmark test cases.

   Two splits exist for the `diagrams` category:

       diagramsShort \[LongDash] 7 cases, the default smoke test.  Covers the main
                     STIntegrate code paths inside the `diagrams` family
                     (Substitutions + CleanOutput, MethodLR -> "Lungo",
                     IR-divergent box, banana with mass remap, D = 6 - 2eps
                     + Gauge, Nilsson-Passare analytic continuation, and
                     FindRoots algebraic letters).  The remaining four
                     categories (propagators, eulerIntegrands,
                     nIntDiagrams, nIntEuler) are common between Short
                     and Long, so Short also smoke-tests Form-2 propagator
                     input, Form-3 Euler integrands, and the numerical
                     STNIntegrate paths via those common lists.

       diagramsLong  \[LongDash] 22 cases, every one verified numerically
                     against pySecDec at relErr < 10^-3 in a prior
                     session.  Use this for developer regression coverage.

   Apply with:
       STBenchmark[]                       (* runs Short; default *)
       STBenchmark["Suite" -> "Long"]      (* runs the full set *)
*)


(* ================================================================== *)
(*        SHORT SUITE \[LongDash] default STBenchmark[] smoke test, 7 cases     *)
(* ================================================================== *)

diagramsShort = {

  (* \[HorizontalLine]\[HorizontalLine] 1-loop triangle with symbolic node masses + numeric Substitutions \[HorizontalLine]\[HorizontalLine]
     exercises: Substitutions, Order, CleanOutput *)
  {{{{{1, 2}, 0}, {{1, 3}, 0}, {{2, 3}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 2]}, {3, Subscript[M, 3]}}},
   "Order" -> 0,
   "Substitutions" -> {MM1 -> (1 - z) (1 - zb), MM2 -> z zb, MM3 -> 1},
   "CleanOutput" -> True},

  (* \[HorizontalLine]\[HorizontalLine] 3-loop cylinder (fully massless) with MethodLR -> "Lungo" \[HorizontalLine]\[HorizontalLine]
     exercises: MethodLR = "Lungo" path at a non-trivial loop order *)
  {{{{{1, 2}, 0}, {{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{3, 4}, 0}, {{1, 4}, 0}},
    {{1, 0}, {2, 0}, {3, 0}, {4, 0}}},
   "MethodLR" -> "Lungo",
   "Order" -> 0},

  (* \[HorizontalLine]\[HorizontalLine] 1-loop massless box (4 vertices, 4 edges, all external legs) \[HorizontalLine]\[HorizontalLine]
     exercises: IR-divergent default path at D = 4 - 2eps *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{1, 4}, 0}},
    {{1, 0}, {2, 0}, {3, 0}, {4, 0}}}},

  (* \[HorizontalLine]\[HorizontalLine] Banana-2 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine]
     exercises: non-default Dimension + mass-parameter remap via Substitutions *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] Triangle at D = 6 - 2eps with explicit Gauge \[HorizontalLine]\[HorizontalLine]
     exercises: Gauge -> {x1 -> 1} option at D = 6 - 2eps *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{1, 3}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] Pentagon with 2 internal masses + 2 vertex masses (Nilsson-Passare) \[HorizontalLine]\[HorizontalLine]
     exercises: mixed-mass topology triggering Nilsson-Passare analytic
     continuation in STExpandIntegral *)
  {{{{{1, 2}, 0}, {{2, 3}, Subscript[m, 2]}, {{3, 4}, Subscript[m, 3]},
     {{1, 4}, 0}, {{2, 4}, 0}},
    {{1, 0}, {2, Subscript[M, 2]}, {3, 0}, {4, Subscript[M, 4]}}}},

  (* \[HorizontalLine]\[HorizontalLine] Diagonal massive box with FindRoots \[HorizontalLine]\[HorizontalLine]
     exercises: fully-massive regime with two diagonal massive edges +
     FindRoots algebraic-letters path (Wm/Wp introduction via HyperInt) *)
  {{{{{1, 3}, m}, {{1, 2}, 0}, {{2, 4}, m}, {{3, 4}, 0}},
    {{1, m}, {2, m}, {3, m}, {4, m}}},
   "FindRoots" -> True}

};


(* ================================================================== *)
(*        LONG SUITE \[LongDash] full developer regression coverage, 22 cases   *)
(* ================================================================== *)

diagramsLong = {

  (* \[HorizontalLine]\[HorizontalLine] 1-loop triangle with symbolic node masses + numeric Substitutions \[HorizontalLine]\[HorizontalLine]
     exercises: Substitutions, Order, CleanOutput *)
  {{{{{1, 2}, 0}, {{1, 3}, 0}, {{2, 3}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 2]}, {3, Subscript[M, 3]}}},
   "Order" -> 0,
   "Substitutions" -> {MM1 -> (1 - z) (1 - zb), MM2 -> z zb, MM3 -> 1},
   "CleanOutput" -> True},

  (* \[HorizontalLine]\[HorizontalLine] 3-point 6-edge topology with symbolic node masses \[HorizontalLine]\[HorizontalLine]
     exercises: higher-loop topology under the same {M_i, z, zb} Substitutions *)
  {{{{{1, 4}, 0}, {{2, 4}, 0}, {{2, 3}, 0}, {{3, 5}, 0}, {{1, 5}, 0}, {{4, 5}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 2]}, {3, Subscript[M, 3]}}},
   "Substitutions" -> {MM1 -> (1 - z) (1 - zb), MM2 -> z zb, MM3 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 3-loop cylinder (fully massless) with MethodLR -> "Lungo" \[HorizontalLine]\[HorizontalLine]
     exercises: MethodLR = "Lungo" path at a non-trivial loop order *)
  {{{{{1, 2}, 0}, {{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{3, 4}, 0}, {{1, 4}, 0}},
    {{1, 0}, {2, 0}, {3, 0}, {4, 0}}},
   "MethodLR" -> "Lungo",
   "Order" -> 0},

  (* \[HorizontalLine]\[HorizontalLine] 1-loop massless box (4 vertices, 4 edges, all external legs) \[HorizontalLine]\[HorizontalLine]
     exercises: IR-divergent default path at D = 4 - 2eps *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{1, 4}, 0}},
    {{1, 0}, {2, 0}, {3, 0}, {4, 0}}}},

  (* \[HorizontalLine]\[HorizontalLine] Banana-2 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine]
     exercises: non-default Dimension + mass-parameter remap via Substitutions *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] Banana-3 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]}, {{1, 2}, 0}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] Banana-4 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]}, {{1, 2}, 0}, {{1, 2}, 0}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] Banana-5 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]},
     {{1, 2}, 0}, {{1, 2}, 0}, {{1, 2}, 0}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] 2-point 5-edge topology at D = 6 - 2eps (variant A) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, 0}, {{1, 3}, 0}, {{3, 4}, 0}, {{2, 4}, 0}, {{3, 4}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 1]}}},
   "Dimension" -> 6 - 2 eps},

  (* \[HorizontalLine]\[HorizontalLine] 2-point 5-edge topology at D = 6 - 2eps (variant B) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 3}, 0}, {{2, 3}, 0}, {{1, 4}, 0}, {{2, 4}, 0}, {{3, 4}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 1]}}},
   "Dimension" -> 6 - 2 eps},

  (* \[HorizontalLine]\[HorizontalLine] Triangle at D = 6 - 2eps with Gauge \[HorizontalLine]\[HorizontalLine]
     exercises: Gauge -> {x1 -> 1} option at D = 6 - 2eps *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{1, 3}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 1 (variant A) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 2 (variant A) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}},
    {{1, 0}, {2, M}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 3 (variant A) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}},
    {{1, 0}, {2, 0}, {3, M}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 1 (variant B) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 4}, 0}, {{3, 4}, 0}, {{3, 5}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 2 (variant B) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 4}, 0}, {{3, 4}, 0}, {{3, 5}, 0}},
    {{1, 0}, {2, M}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 3 (variant B) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 4}, 0}, {{3, 4}, 0}, {{3, 5}, 0}},
    {{1, 0}, {2, 0}, {3, M}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 1 (variant C) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}, {{4, 5}, 0}, {{1, 5}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps, mass on vertex 2 (variant C) \[HorizontalLine]\[HorizontalLine] *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}, {{4, 5}, 0}, {{1, 5}, 0}},
    {{1, 0}, {2, M}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] Pentagon with 2 internal masses + 2 vertex masses (Nilsson-Passare) \[HorizontalLine]\[HorizontalLine]
     exercises: mixed-mass topology triggering Nilsson-Passare analytic
     continuation in STExpandIntegral *)
  {{{{{1, 2}, 0}, {{2, 3}, Subscript[m, 2]}, {{3, 4}, Subscript[m, 3]},
     {{1, 4}, 0}, {{2, 4}, 0}},
    {{1, 0}, {2, Subscript[M, 2]}, {3, 0}, {4, Subscript[M, 4]}}}},

  (* \[HorizontalLine]\[HorizontalLine] Diagonal massive box with FindRoots \[HorizontalLine]\[HorizontalLine]
     exercises: fully-massive regime with two diagonal massive edges +
     FindRoots algebraic-letters path (Wm/Wp introduction via HyperInt) *)
  {{{{{1, 3}, m}, {{1, 2}, 0}, {{2, 4}, m}, {{3, 4}, 0}},
    {{1, m}, {2, m}, {3, m}, {4, m}}},
   "FindRoots" -> True},

  (* \[HorizontalLine]\[HorizontalLine] 2-loop triangle-box, three distinct external masses (z, zb) \[HorizontalLine]\[HorizontalLine]
     exercises: SetProblemID + (z, zb) rationalizing Substitutions; the
     weight-4 answer reproduces the closed form in arXiv:2507.17815.
     Source: paperChecks.wl Lstlisting 19 (Sec. 4.1.2). *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 2]}, {3, Subscript[M, 3]}}},
   "SetProblemID"  -> "TriangleBox-Long",
   "Substitutions" -> {MM1 -> (1 - zz)(1 - zzb), MM2 -> zz zzb, MM3 -> 1}}

};


(* Backwards-compatible alias for anything that imported the old
   `diagrams` symbol directly. *)
diagrams = diagramsShort;


(* ================================================================== *)
(*  COMMON CATEGORIES \[LongDash] run in BOTH Short and Long suites               *)
(*  These lists populate the four formerly-empty STBenchmark categories. *)
(*  Kept light enough that even Short stays manageable.               *)
(* ================================================================== *)

(* Form-2 (propagator-list) STIntegrate cases. *)
propagators = {

  (* \[HorizontalLine]\[HorizontalLine] Squared tadpole with tensor numerators \[HorizontalLine]\[HorizontalLine]
     exercises: Form-2 propagator list + tensor numerators + Exponents
                with negative powers (numerator factors).
     Note: l, q must be in SubTropica` context (not Global`); without
           explicit "LoopMomenta", STIntegrate's auto-detection compares
           against SubTropica`l / SubTropica`q.  Under the loader's
           restricted ContextPath bare `l` would land in Global`, the
           auto-detect would miss it, and the dispatcher would fall into
           a degenerate path (no warning, but a wrong/trivial answer).
     Source: paperChecks.wl Lstlisting 9 (Sec. 2.3). *)
  {{SubTropica`l[1]\[CenterDot]SubTropica`l[1] - mm,
    SubTropica`q[1]\[CenterDot]SubTropica`l[1],
    SubTropica`q[2]\[CenterDot]SubTropica`l[1]},
   "Exponents" -> {2, -1, -1}},

  (* \[HorizontalLine]\[HorizontalLine] Soft anomalous dimensions, 2 loops (arXiv:2509.18017 Eq. D.11) \[HorizontalLine]\[HorizontalLine]
     exercises: eikonal (v.k) propagators + tensor numerators + LoopMomenta
                + custom Normalization + MethodPolysAndPairs + Gauge.  The
                option-densest single case in the suite.
     Source: paperChecks.wl Lstlisting 22 (Sec. 4.2). *)
  {{k[1]\[CenterDot]k[1], k[2]\[CenterDot]k[2],
    (k[1] + k[2])\[CenterDot](k[1] + k[2]),
    v[1]\[CenterDot]k[1] - 1, v[2]\[CenterDot]k[2] - 1,
    -\[Beta]\[CenterDot](k[1] + k[2]),
    v[2]\[CenterDot]k[1], \[Beta]\[CenterDot]k[2], v[1]\[CenterDot]k[2]},
   "Exponents"           -> {1, 1, 1, 1, 1, 1, -1, 0, 0},
   "Substitutions"       -> {v[1]\[CenterDot]v[1] | v[2]\[CenterDot]v[2] -> 1,
                             v[1]\[CenterDot]\[Beta] -> -y, v[2]\[CenterDot]\[Beta] -> -1,
                             \[Beta]\[CenterDot]\[Beta] -> 0,
                             v[1]\[CenterDot]v[2] -> -(1/2)(1/a12 + a12)},
   "LoopMomenta"         -> {k[1], k[2]},
   "Normalization"       -> -(4 Exp[EulerGamma])^(2 eps),
   "Order"               -> -1,
   "MethodPolysAndPairs" -> "Standard",
   "Gauge"               -> {x5 -> 1}}

};


(* Form-3 (Euler integrand) STIntegrate cases. *)
eulerIntegrands = {

  (* \[HorizontalLine]\[HorizontalLine] Toy 2-variable Euler integrand x1^eps x2^eps (1+x1+x2)^(-3 eps) \[HorizontalLine]\[HorizontalLine]
     exercises: Form-3 bare-symbol input (default [0,Infinity) integration);
                exercises the tropical-fan analysis at minimum complexity.
     Source: paperChecks.wl Sec. 4 (Tropical algorithm, line 213). *)
  {x1^eps x2^eps (1 + x1 + x2)^(-3 eps), x1, x2},

  (* \[HorizontalLine]\[HorizontalLine] EEC quadruple at J1=1, J2=2 (arXiv:2512.23791 Eq. A.14) \[HorizontalLine]\[HorizontalLine]
     exercises: pre-built {prefactor, integrand, xvars, coeffs} tuple
                input form on a non-Feynman Euler integrand from
                gravitational energy-energy correlators.  Uses
                HyperIntica`STFactorAndTrackRoots to handle
                quadratically-irreducible polynomials in the denominator.
     Source: paperChecks.wl Sec. 4.3 (Sec 7/3a, line 449), pre-substituted
             at J1=1, J2=2 to remove the angular-momentum series expansion. *)
  {{1,
    ((x^(-2 eps) (1 + x)^(-3 + 2 eps) (1 + x (1 - z))^(2 eps)) /
     ((HyperIntica`STFactorAndTrackRoots[
         Numerator[Factor[(1 - y2 + x^2 (-1 + y1) (-1 + z) - x (-2 + y1 + y2 + z))/(1 + x)^2]],
         x, P] /
       Numerator[Factor[(1 - y2 + x^2 (-1 + y1) (-1 + z) - x (-2 + y1 + y2 + z))/(1 + x)^2]]) *
      (HyperIntica`STFactorAndTrackRoots[
         Numerator[Factor[(y2 + x (y1 + x y1 + y2 - (1 + x y1) z))/(1 + x)^2]],
         x, Q] /
       Numerator[Factor[(y2 + x (y1 + x y1 + y2 - (1 + x y1) z))/(1 + x)^2]]))),
    {x},
    {ee, \[CapitalDelta], z, y1, y2}},
   "SimplifyOutput" -> Identity}

};


(* Numerical STNIntegrate cases on graph-form input. *)
nIntDiagrams = {

  (* \[HorizontalLine]\[HorizontalLine] Elliptic kite, generic masses (not linearly reducible) \[HorizontalLine]\[HorizontalLine]
     exercises: numerical fallback to pySecDec on a non-LR (elliptic)
                topology where the symbolic STIntegrate path would block.
                Substitution values use the SubTropica squared-mass
                convention (mm_i = (linear m_i)^2; MM = (external M)^2 =
                (31 I/5)^2 = -961/25, real-valued so pySecDec accepts it).
     Source: paperChecks.wl Lstlisting 13 (Sec. 2.4). *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 3}, Subscript[m, 2]}, {{1, 4}, Subscript[m, 3]},
     {{2, 3}, Subscript[m, 4]}, {{2, 4}, Subscript[m, 5]}},
    {{3, M}, {4, M}}},
   "Substitutions" -> {mm1 -> (67/23)^2, mm2 -> (59/31)^2, mm3 -> (159/31)^2,
                       mm4 -> (59/131)^2, mm5 -> (117/137)^2, MM -> -961/25}}

};


(* Numerical STNIntegrate cases on Euler-integrand input. *)
nIntEuler = {

  (* \[HorizontalLine]\[HorizontalLine] Massless double-box Symanzik integrand, bare-symbol form \[HorizontalLine]\[HorizontalLine]
     exercises: numerical STNIntegrate on a parametric integrand built by
                hand from U/F polynomials (no graph form, no automatic
                Symanzik build).  Bare symbols default the integration
                domain to [0,Infinity).
     Note: Mandelstams must be in SubTropica` context (not Global`); under
           the loader's restricted ContextPath, bare `s12` would land in
           Global` and the Form-6 dispatcher's pySecDec backend would not
           recognise it (the `coeffs` extraction runs in SubTropica`Private`,
           where Global` is off ContextPath).  Explicit context qualification
           bypasses the issue.
     Source: paperChecks.wl Lstlisting 15 (Sec. 2.4, line 179). *)
  {Exp[3 eps EulerGamma] Gamma[3 eps]
     (x1 x2 + x1 x3 + x2 x3 + x1 x5 + x2 x5 + x1 x2 x5 + x1 x3 x5 +
      x2 x3 x5 + x1 x6 + x2 x6 + x1 x5 x6 + x2 x5 x6)^(2 (-1 + 2 eps))
     (-(SubTropica`s23 x1 x2 x5 +
        SubTropica`s12 (x1 x3 x6 + x2 x3 x6 + x1 x3 x5 x6 +
                        x2 x3 x5 x6)))^(-3 eps),
   x1, x2, x3, x5, x6,
   "Substitutions" -> {SubTropica`s12 -> -7/31, SubTropica`s23 -> -43/89}}

};
