(* ::Package:: *)

(* mgDiagrams.wl
   Curated STBenchmark test cases.  Two splits:

       diagramsShort \[LongDash] 10 cases, the default smoke test.  Chosen to
                     cover the main STIntegrate code paths: Substitutions
                     + CleanOutput, MethodLR -> "Lungo", fully-massive
                     topology, non-default Dimension (D = 2 - 2eps and
                     D = 6 - 2eps), Gauge fixing, Nilsson-Passare analytic
                     continuation, and FindRoots algebraic letters.

       diagramsLong  \[LongDash] 21 cases, every one verified numerically
                     against pySecDec at relErr < 10^-3 in a prior
                     session.  Use this for developer regression coverage.

   Apply with:
       STBenchmark[]                       (* runs Short; default *)
       STBenchmark["Suite" -> "Long"]      (* runs the full 21-case set *)
*)


(* ================================================================== *)
(*        SHORT SUITE (10) \[LongDash] default STBenchmark[] smoke test         *)
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

  (* \[HorizontalLine]\[HorizontalLine] Banana-5 at D = 2 - 2eps with mass Substitutions \[HorizontalLine]\[HorizontalLine]
     exercises: deeper banana (5 internal lines) at the same kinematic point *)
  {{{{{1, 2}, Subscript[m, 1]}, {{1, 2}, Subscript[m, 2]},
     {{1, 2}, 0}, {{1, 2}, 0}, {{1, 2}, 0}},
    {{1, M}, {2, M}}},
   "Dimension" -> 2 - 2 eps,
   "Substitutions" -> {MM -> 1, mm1 -> w wb, mm2 -> (1 - w) (1 - wb)}},

  (* \[HorizontalLine]\[HorizontalLine] 2-point 5-edge topology at D = 6 - 2eps \[HorizontalLine]\[HorizontalLine]
     exercises: non-default Dimension at D = 6 - 2eps with a sub-bubble *)
  {{{{{1, 2}, 0}, {{1, 3}, 0}, {{3, 4}, 0}, {{2, 4}, 0}, {{3, 4}, 0}},
    {{1, Subscript[M, 1]}, {2, Subscript[M, 1]}}},
   "Dimension" -> 6 - 2 eps},

  (* \[HorizontalLine]\[HorizontalLine] Triangle at D = 6 - 2eps with explicit Gauge \[HorizontalLine]\[HorizontalLine]
     exercises: Gauge -> {x1 -> 1} option at D = 6 - 2eps *)
  {{{{{1, 2}, 0}, {{2, 3}, 0}, {{1, 3}, 0}},
    {{1, M}, {2, 0}, {3, 0}}},
   "Dimension" -> 6 - 2 eps,
   "ShowTimings" -> False,
   "Gauge" -> {x1 -> 1}},

  (* \[HorizontalLine]\[HorizontalLine] 6-edge pentagon at D = 6 - 2eps with Gauge \[HorizontalLine]\[HorizontalLine]
     exercises: representative of the {M,0,0} pentagon family with Gauge fixing *)
  {{{{{1, 4}, 0}, {{1, 5}, 0}, {{2, 5}, 0}, {{2, 3}, 0}, {{3, 4}, 0}, {{4, 5}, 0}},
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
(*        LONG SUITE (21) \[LongDash] full developer regression coverage        *)
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
   "FindRoots" -> True}

};


(* Backwards-compatible alias for anything that imported the old
   `diagrams` symbol directly. *)
diagrams = diagramsShort;


(* The other category lists are empty in this curated file; STBenchmark
   still expects them to exist so its loader can iterate. *)
propagators     = {};
eulerIntegrands = {};
nIntDiagrams    = {};
nIntEuler       = {};
