(* doppio_lib.wl
   -------------------------------------------------------------------------
   Doppio intermediate-stage Euler-discriminant LR engine.

   This module implements `dpEulerLocus`, a stripped-down variant of the
   full `eulerDiscriminant` from euler_disc_lib.wl.  The key difference:
   dpEulerLocus differentiates only with respect to the S-variables
   (S-marginal critical system), keeping the remaining integration variables
   as parameters.  It therefore produces the singularity locus of the
   S-marginal integrand rather than the full Euler discriminant of the face
   polynomial.

   Coefficient denominators are reconstructed mod a single prime (NO
   FFRatRec rational reconstruction over Q).  Callers compare loci by
   monomial support in the kinematic parameters, so mod-p factors are
   sufficient.

   References:
     Fevola-Mizera-Telen, arXiv:2311.16219 (Principal Landau Determinants)
     Matsubara-Heo,        arXiv:2505.13163 (Euler discriminant = singular
                           locus over torus)

   Load order: AFTER load_spqr.wl, in a FRESH kernel.
   (Running kernels cache $LinearFactorsCache and similar globals; stale
   caches produce silently wrong results.)
   ------------------------------------------------------------------------- *)

(* backend guard: SPQR is loaded by load_spqr.wl for the FF32/msolve-side
   variants (A, B, DoppioCrawl, the IncidenceLine chi backend).  The load is
   GRACEFUL here because the variant-C production path (Lungo-core + the
   self-contained cleared-dlog DiscKosky counters) is pure Mathematica and
   must be loadable inside a SubTropica kernel with no SPQR paclet present
   (Task 8: the MethodLR -> "Doppio" bridge).  dpBackendOK[] still reports
   the truth, and the A/B/crawl entry points still abort without SPQR. *)
Quiet@Check[Needs["SPQR`"], Null];

(* DiscKosky (Crisanti/Lippstreu/McLeod/Polackova): direct Euler-characteristic
   counter, the DEFAULT genuineness backend for Doppio C (user decision,
   2026-06-03).  Pure Mathematica (GroebnerBasis mod 64-bit primes), no
   FiniteFlow/SPQR dependency, so it coexists with any kernel.  Loaded
   gracefully: without the paclet, dpGenuineQ falls back to the incidence-line
   backend with a one-time warning. *)
Quiet@Check[Needs["DiscKosky`"], Null];

dp$graphCounter = 0;

Get["/Users/smizera/Projects/SubTropica-branchSM/scripts/doppiofubini/euler_disc/euler_disc_lib.wl"];

(* ----------------------------------------------------------------------
   dpToAtomic[expr]
     Rename indexed masses  m[i] -> mi  so that FiniteFlow32 / SPQR see
     only plain symbols as parameters.  No inverse is needed at this stage
     because outputs are compared by monomial support in the kinematic
     parameters, not by coefficient value.
   ---------------------------------------------------------------------- *)
dpToAtomic[expr_] := expr /. m[i_Integer] :> Symbol["m" <> ToString[i]];

(* ----------------------------------------------------------------------
   dpEulerLocus[polysIn, S, allVars, prime]

     polysIn  : list of face polynomials {P_1, ..., P_k}
     S        : integration variables to differentiate (the "S-marginal"
                subsystem); remaining allVars variables enter as params
     allVars  : ALL integration variables {x1, ..., xn}
     prime    : large prime used for mod-p coefficient reconstruction
                (pass FFPrimeNo[0] for the default 64-bit prime)

   Algorithm
   ---------
   1. Rename m[i] -> mi for FF32 compatibility (dpToAtomic).
   2. Draw generic seeded exponents b_i, c_j for polys and all variables.
   3. Form the master log-function  Psi = Sum b_i log P_i + Sum c_j log x_j.
   4. Differentiate Psi w.r.t. the S-variables ONLY (S-marginal critical
      system); the remaining variables appear as parameters in the ideal.
   5. Build the Rabinowitsch generator  1 - x0 LCM(denominators)  to
      cut out the very-affine torus (Cstar)^Length[S] minus {P_i = 0}.
   6. For each kept variable y in {x0} union S:
        eliminate the rest via SPQR-backed FF Groebner (mod p);
        recover the GENUINE (non-monic) leading coefficient L1 as the
        LCM of the coefficient denominators of the monic-over-Q(params)
        basis (see euler_disc_lib.wl §"genuine leading coefficient" for
        the derivation);
        factor both L1 and the trailing numerator mod prime;
        drop constant/unit factors (those that share NO variable with the
        parameter set).
   7. Return the union of all mod-p factors over all kept variables.
   ---------------------------------------------------------------------- *)
dpEulerLocus[polysIn_List, S_List, allVars_List, prime_] :=
 Module[{polys, x0, bs, cs, logPsi, dlog, num, den, rab,
    ideal, idealVars, keepList, params, perY, gname, learn, nmon, cmod,
    leadL1, trailNum, facs},

  (* --- step 1: rename m[i] -> mi for FF32 --- *)
  polys = dpToAtomic /@ polysIn;
  x0 = Symbol["x0"];

  (* --- step 2: generic seeded exponents ---
     Fixed seed for reproducibility; value is arbitrary; change only if
     the drawn exponents happen to make the ideal degenerate (chi = Infinity).
     Range[3, 400]: wider draw range than the full engine's Range[3, 200];
     genericity is what matters, the extra width is harmless. *)
  SeedRandom[20240602];
  bs = RandomSample[Range[3, 400], Length[polys]];
  cs = RandomSample[Complement[Range[3, 400], bs], Length[allVars]];

  (* --- step 3: master log-function --- *)
  logPsi = Total[MapThread[#2 Log[#1] &, {polys, bs}]] +
           Total[MapThread[#2 Log[#1] &, {allVars, cs}]];

  (* --- step 4: differentiate w.r.t. S only (S-marginal critical system) --- *)
  dlog = Factor[D[logPsi, {S}]];
  num = Numerator[dlog]; den = Denominator[dlog];

  (* --- step 5: Rabinowitsch generator (very-affine torus) --- *)
  rab = 1 - x0 (Apply[PolynomialLCM, den]);
  ideal = Join[num, {rab}];

  (* elimination variables are {x0} union S; remaining allVars are params *)
  idealVars = Prepend[S, x0];

  params = Complement[Variables[ideal], idealVars];

  (* --- step 6: per-keep elimination, leading/trailing factor union --- *)
  keepList = idealVars;
  perY = Function[keepVar,
     dp$graphCounter++;
     gname = "dpg" <> ToString[dp$graphCounter];
     Quiet@FFDeleteGraph[gname];
     FFNewGraph[gname, "params", params];
     FFAlgGroebner[gname, "gb", ideal, idealVars, params,
        EliminateVariables -> DeleteCases[idealVars, keepVar]];
     FFGraphOutput[gname, "gb"];
     learn = FFGroebnerLearn[gname, idealVars];
     If[! (MatchQ[learn, {_List}] && Length[First[learn]] >= 2),
        Quiet@FFDeleteGraph[gname]; Return[{}, Module]];
     nmon = Length[First[learn]];
     FFAlgTake[gname, "coeff", {"gb"}, Table[{1, j}, {j, 1, nmon}]];
     FFGraphOutput[gname, "coeff"];
     (* reconstruct coefficients mod prime: NO FFRatRec rational reconstruction;
        mod-p factors are sufficient because callers compare by monomial support *)
     cmod = Together /@ Flatten[{FFReconstructFunctionMod[gname, params,
          "StartingPrimeNo" -> dpPrimeNo[prime]]}];
     Quiet@FFDeleteGraph[gname];
     (* leadL1 = genuine non-monic leading coefficient = LCM of the
        coefficient denominators of the monic-over-Q(params) basis;
        see euler_disc_lib.wl for the derivation (L1 = LCM denom c_i) *)
     leadL1 = Fold[PolynomialLCM[#1, Denominator[#2], Modulus -> prime] &, 1, cmod];
     trailNum = Numerator[Last[cmod]];
     facs = Join[
        FactorList[leadL1, Modulus -> prime][[All, 1]],
        FactorList[trailNum, Modulus -> prime][[All, 1]]];
     (* drop unit/constant factors: those that carry no params variable.
        Exponent[c, params] returns a LIST for list-valued params, so the
        naive "== 0" predicate never matches; test membership of params in
        the factor's variables instead. *)
     DeleteCases[facs, c_ /; (Intersection[Variables[c], params] === {})]];

  (* --- step 7: union over all kept variables --- *)
  Union[Flatten[perY /@ keepList]]];

dpPrimeNo[prime_] := SelectFirst[Range[0, 10], FFPrimeNo[#] === prime &, 0];

(* ----------------------------------------------------------------------
   dpDegreeInVar[locus, v]

     Degree of each polynomial in the locus list `locus` in the single
     variable `v`.  Uses Mathematica's built-in Exponent, which returns
     the highest power of `v` that appears in each polynomial (0 if `v`
     is absent).

     The function reads the mod-p polynomials directly -- no rational
     reconstruction is needed, because degree in a single variable is
     invariant under mod-p reduction (the leading monomial in v cannot
     vanish mod a prime for polynomials with integer coefficients in the
     regime we use, since the prime is much larger than any coefficient).

     Typical use: after dpLocus2 returns a locus list, call
       dpDegreeInVar[locus, s]
     to see which factors have kinematic dependence on the Mandelstam s.
     A degree-0 entry means that factor is independent of s (e.g., a
     pure-mass threshold); degree >= 2 signals a non-linear letter.
   ---------------------------------------------------------------------- *)
dpDegreeInVar[locus_List, v_] := Exponent[#, v] & /@ locus;

(* ----------------------------------------------------------------------
   dpBackendOK[]
   dpAssertBackend[]

     SPQR backend guard.

     load_spqr.wl loads the SPQR paclet, which defines
     FindIrreducibleMonomials in the context "SPQR`".  If load_spqr.wl
     was NOT loaded before doppio_lib.wl is used, the symbol
     FindIrreducibleMonomials stays in the Global` context as an
     unevaluated head.  In that situation, Length[FindIrreducibleMonomials[...]]
     would return the number of ARGUMENTS rather than the true Euler
     characteristic -- a silent wrong result that propagates through the
     entire DoppioFubini engine without any visible error message.

     dpBackendOK[] returns True iff FindIrreducibleMonomials lives in
     "SPQR`" (i.e., the SPQR package is loaded and active in this kernel).

     dpAssertBackend[] calls dpBackendOK[]; on failure it prints a
     diagnostic and calls Abort[].  All entry-point functions in
     doppio_lib.wl that invoke FindIrreducibleMonomials call
     dpAssertBackend[] at the top so the error fires early, with a clear
     message, rather than silently at the point of the wrong chi value.
   ---------------------------------------------------------------------- *)
dpBackendOK[] := (Context[FindIrreducibleMonomials] === "SPQR`");

dpAssertBackend[] := If[! dpBackendOK[],
   Print["[doppio] FATAL: SPQR backend not loaded (Get load_spqr.wl first)."];
   Abort[]];

(* ----------------------------------------------------------------------
   dpSupp[poly, vars]

     Monomial support of `poly` in the variable order `vars`, with all
     coefficients discarded.  Returned as a SORTED list of exponent vectors
     (the Keys of CoefficientRules).  Two polynomials that differ only by
     overall scaling or by their coefficient VALUES share the same support;
     this is exactly the invariant we use to decide whether two mod-p
     factorizations describe the same geometric locus.
   ---------------------------------------------------------------------- *)
dpSupp[poly_, vars_] := Sort[Keys[CoefficientRules[poly, vars]]];

(* ----------------------------------------------------------------------
   dpAgreeFactors[polys, vars, p1, p2]

     Two-prime false-split guard.

   Motivation
   ----------
   A single Mersenne prime can SPURIOUSLY split a sqrt-threshold letter.
   The canonical example is the box threshold  M2^2 - 2 s^2, which is
   irreducible over Q (no rational sqrt(2)) yet factors mod p1 = 2^31 - 1
   as (M2 + 65536 s)(M2 - 65536 s), because 65536^2 == 2 (mod 2^31 - 1):
   a square root of 2 happens to exist in F_{p1}.  Reconstructing the
   Landau locus from such a factorization would manufacture two phantom
   linear letters where physics has a single quadratic one.

   Guard
   -----
   Factor `polys` mod each of the two primes, then compare the two
   factorizations by their MONOMIAL SUPPORTS (coefficient values differ
   between primes and are meaningless for this comparison; supports do not).

     * If the support-multisets agree across the primes, the split is
       genuine over Q -- both primes saw the same geometry -- so we keep
       the mod-p1 factorization f1.

     * If they disagree, at least one prime introduced a phantom root.
       We keep the COARSER (fewer-factor) factorization: a false split can
       only ADD factors relative to the true rational one, never remove
       them, so the prime that produced fewer factors is the one that did
       not split.  In the box example mod p2 = 2147483629 leaves
       M2^2 - 2 s^2 irreducible (one factor), so the Length[f1] <= Length[f2]
       test selects f2 and the quadratic letter survives whole.

   Constant/unit factors (those sharing no variable with `vars`) are
   stripped first so that prime-dependent leading units do not perturb
   either the support comparison or the factor count.
   ---------------------------------------------------------------------- *)
dpAgreeFactors[polys_List, vars_List, p1_, p2_] :=
 Module[{f1, f2, s1, s2},
  (* factor mod each prime; flatten the per-poly factor lists into one bag *)
  f1 = Flatten[FactorList[#, Modulus -> p1][[All, 1]] & /@ polys];
  f2 = Flatten[FactorList[#, Modulus -> p2][[All, 1]] & /@ polys];
  (* drop constant/unit factors that carry none of the locus variables *)
  f1 = DeleteCases[f1, c_ /; Intersection[Variables[c], vars] === {}];
  f2 = DeleteCases[f2, c_ /; Intersection[Variables[c], vars] === {}];
  (* compare by support only (coefficient values are prime-specific) *)
  s1 = Sort[dpSupp[#, vars] & /@ f1];
  s2 = Sort[dpSupp[#, vars] & /@ f2];
  If[s1 === s2, Return[f1]];
  (* disagreement: keep the coarser (fewer-factor) factorization *)
  If[Length[f1] <= Length[f2], f1, f2]];

(* ----------------------------------------------------------------------
   dpCoarser[l1, l2, vars]

     Direct two-prime false-split guard operating on ALREADY-FACTORED locus
     lists l1 and l2 (the outputs of dpEulerLocus at two distinct primes).

   Why dpAgreeFactors is wrong for this job
   -----------------------------------------
   dpAgreeFactors refactors a joined pool mod each prime.  If a phantom
   factor from prime p1 (say the pair {M2+65536 s, M2-65536 s} that arises
   because 65536^2 == 2 mod p1) is already in l1, re-factoring the join
   leaves the pair intact at BOTH primes -- it is individually irreducible
   at both -- so the guard sees "both primes agree on two factors" and
   incorrectly keeps the split.  The guard is defeated by its own input.

   Correct approach: compare l1 and l2 directly.
   -----------------------------------------------
   A phantom factorization can only ADD factors relative to the true
   rational factorization, never remove them.  Therefore:

     * If the monomial-support multisets of l1 and l2 agree, the split is
       genuine at both primes (consistent with a rational factorization),
       so we keep l1.

     * If they disagree, at least one prime introduced spurious factors.
       We keep the COARSER list (the one with FEWER factors): the coarser
       prime did not split the phantom, so its output is closer to the true
       rational locus.

   Monomial supports (dpSupp) are used throughout because coefficient
   values are prime-specific and meaningless for the comparison; only the
   set of monomials that appears in each factor carries geometric content.
   ---------------------------------------------------------------------- *)
dpCoarser[l1_List, l2_List, vars_] := Module[{s1, s2},
   s1 = Sort[dpSupp[#, vars] & /@ l1];
   s2 = Sort[dpSupp[#, vars] & /@ l2];
   If[s1 === s2, l1, If[Length[l1] <= Length[l2], l1, l2]]];

(* ----------------------------------------------------------------------
   dpLocus2[polys, S, allVars]

     Per-subset Landau locus computed over TWO primes with the false-split
     guard applied.  Runs dpEulerLocus at p1 = FFPrimeNo[0] and
     p2 = FFPrimeNo[1], then compares the two ALREADY-FACTORED locus lists
     directly via dpCoarser -- it never re-factors a joined pool, which
     would let a phantom split survive the guard (see dpCoarser comment).

     The comparison variables are taken from the loci THEMSELVES (the union
     of variables actually appearing in l1 and l2), not from a precomputed
     Complement[allVars, S]: the elimination may have removed integration
     variables, and only the parameters that survive into the locus matter
     for the support comparison.

     Returns the kept mod-p factors (coarser of the two on disagreement),
     in the same shape as dpEulerLocus.
   ---------------------------------------------------------------------- *)
dpLocus2[polys_List, S_List, allVars_List] :=
 Module[{p1 = FFPrimeNo[0], p2 = FFPrimeNo[1], l1, l2, vars},
  l1 = dpEulerLocus[polys, S, allVars, p1];
  l2 = dpEulerLocus[polys, S, allVars, p2];
  vars = Union[Flatten[Variables /@ Join[l1, l2]]];
  dpCoarser[l1, l2, vars]];

(* ----------------------------------------------------------------------
   dpSquareQ[poly, vars]

     True iff `poly` is a perfect square as a polynomial in `vars`, i.e.
     its square root is itself a polynomial (with rational coefficients).

   Robust test (squaring-guard contract):
     1. Expand the input.  The zero polynomial is trivially a square (0^2).
     2. Form a candidate square root and confirm it is a polynomial in `vars`
        AND that squaring it recovers the expanded input exactly.  The
        squaring check (Expand[rt^2 - e] === 0) is what makes the test sound:
        PolynomialQ alone could be fooled by sign/branch ambiguities, but the
        exact identity rt^2 == e cannot.

   Extraction note (why not bare Together[Sqrt[e]]):
     Together does NOT simplify a square root -- Together[Sqrt[x3^2]] returns
     Sqrt[x3^2] unevaluated, and likewise Sqrt[1 + 2 x3 + x3^2] is left as a
     radical -- so a Together-based root never satisfies PolynomialQ and the
     test would reject EVERY perfect square.  We therefore factor first and
     let Sqrt act on the factored form: PowerExpand[Sqrt[Factor[e]]] pulls out
     even-power factors (Sqrt[(1+x3)^2] -> 1+x3, Sqrt[4 x3^2] -> 2 x3).
   Soundness guard (FreeQ[rt, Power[_, _Rational]]):
     PowerExpand will also pull a rational constant out of a non-square
     content (it would write Sqrt[2 x3^2] as Sqrt[2] x3).  A leftover radical
     such as Sqrt[2] -- or Sqrt[x3] from a non-square like x3(x3+1) -- means
     the input is NOT a perfect square over Q[vars], so we reject any
     candidate that still contains a fractional power.  Combined with the
     squaring identity this correctly accepts 4 x3^2, (x3-2)^2 (x3+1)^2, and
     rejects 2 x3^2, 3 (x3+1)^2, x3, x3(x3+1).

   Imaginary guard (FreeQ[rt, Complex]):
     For a negative square such as -x3^2, Mathematica evaluates
     PowerExpand[Sqrt[-x3^2]] -> I x3.  Here PolynomialQ[I x3, {x3}] = True
     (I is treated as a numeric coefficient) and the squaring identity
     (I x3)^2 = -x3^2 holds, so without this guard the test would falsely
     accept every negated perfect square.  The FreeQ[rt, Complex] check
     rejects any candidate whose square root is complex-valued.

   This is the perfect-square check over Q used by the Euler-conic keep-test
   (dpRationalizableQ).  We work over Q after a one-off lift of just this one
   locus polynomial -- it is the rare branch, since most locus polynomials
   are linear in the next integration variable and never reach here.
   ---------------------------------------------------------------------- *)
dpSquareQ[poly_, vars_] := Module[{e = Expand[poly], rt},
   If[e === 0, Return[True]];
   rt = PowerExpand[Sqrt[Factor[e]]];
   PolynomialQ[rt, vars] && FreeQ[rt, Power[_, _Rational]] &&
     FreeQ[rt, Complex] && Expand[rt^2 - e] === 0];

(* ----------------------------------------------------------------------
   dpRationalizableQ[q, v, leftoverVars]

     Quadratic Euler-conic keep-test.  The locus polynomial `q` is degree
     <= 2 in the next integration variable `v`:  q = A v^2 + B v + C, with
     A, B, C depending on the leftover integration variables.

   Rationalizability criterion
   ---------------------------
   After eliminating `v`, the singularity contributes a square root
   Sqrt[B^2 - 4 A C] (the discriminant of the conic u^2 = q).  This root is
   rationalizable by an Euler substitution -- so the integral remains in the
   class of hyperlogarithms -- precisely when the conic u^2 = q has a
   rational point, which here holds iff BOTH the leading coefficient A and
   the constant term C are perfect squares in the leftover variables.  (A
   square A lets one read off a rational point at v -> Infinity; a square C
   gives the rational base point (0, Sqrt[C]) that ANEulerConic projects
   from below.)  When either fails, the root is genuinely algebraic over
   Q(leftoverVars) and the next v-integration leaves the hyperlogarithm
   class -- the locus is NOT kept.

   Recorded letter
   ---------------
   On success we record the rationalization (sqrt) letter by calling
   ANEulerConic[q, v] (AnTropica.wl, signature ANEulerConic[poly, var]),
   which returns the conic parametrization {v -> rational(w), sqrt(w)}.
   ANEulerConic is wrapped in Quiet@Check so that, if it is unavailable or
   fails (e.g. AnTropica.wl not loaded in this kernel), we fall back to the
   bare discriminant B^2 - 4 A C as the recorded letter.  The discriminant
   is also used directly in the not-ok branch, where no conic parametrization
   is meaningful.

   Returns an Association:
     "ok"     -> True/False  (rationalizable -> keep this locus)
     "A","C"  -> the leading and constant coefficients (for diagnostics)
     "letter" -> ANEulerConic output on success, else B^2 - 4 A C
   ---------------------------------------------------------------------- *)
dpRationalizableQ[qIn_, v_, leftoverVars_] :=
 Module[{q = Expand[qIn], A, B, C, ok, letter},
  A = Coefficient[q, v, 2]; B = Coefficient[q, v, 1]; C = Coefficient[q, v, 0];
  (* both A and C perfect squares in the leftover variables -> conic has a
     rational point -> Euler substitution rationalizes the discriminant root *)
  ok = dpSquareQ[A, leftoverVars] && dpSquareQ[C, leftoverVars];
  letter = If[ok,
     Quiet@Check[ANEulerConic[q, v], B^2 - 4 A C], B^2 - 4 A C];
  <|"ok" -> ok, "A" -> A, "C" -> C, "letter" -> letter|>];

(* ----------------------------------------------------------------------
   dpRationalizableNumQ[qSym, v, leftoverVars, kinematics, npts]

     Kinematics-NUMERIC rationalizability verdict: substitute the kinematics to
     random integers at `npts` independent points and require dpRationalizableQ
     to return ok at ALL of them.

   Why multi-point.  The Euler conic of q = A v^2 + B v + C rationalizes iff A
   and C are perfect squares in the leftover integration variables OVER the
   coefficient field Q(kinematics).  Numericizing the kinematics turns "perfect
   square as a function" into "perfect square at a point":
     * a genuine perfect-square function stays a perfect square at every point
       (so a rationalizable letter is accepted at all npts), while
     * a non-square function (e.g. a Kallen combination M1 M2 - s t) is a
       non-square integer at generic points, so it is rejected -- EXCEPT on the
       measure-zero set where it happens to land on a perfect-square integer.
   Requiring agreement across npts independent points drives that accident to
   ~(perfect-square density)^npts, negligible for npts >= 2-3.  This lets the
   crawl decide a quadratic-in-v letter without the expensive kinematics-symbolic
   dpLocus2; the per-point dpRationalizableQ runs on integer-coefficient A, C.

   Used as a unit-testable primitive (t13) and mirrored at the locus level by
   DoppioCrawl's KinNumeric degree-2 branch (which AND-s the verdict over npts
   kinematic points of the numeric locus).
   ---------------------------------------------------------------------- *)
dpRationalizableNumQ[qSym_, v_, leftoverVars_, kinematics_, npts_Integer: 3] :=
 AllTrue[Range[npts], Function[pt,
    Module[{subst},
     SeedRandom[271828 + pt];
     subst = Thread[kinematics -> RandomInteger[{10^4, 10^5}, Length[kinematics]]];
     dpRationalizableQ[qSym /. subst, v, leftoverVars]["ok"]]]];

(* ----------------------------------------------------------------------
   dpEulerLocusQ[polysIn, S, allVars]
   dpLocusQ[polysIn, S, allVars]

     OVER-Q variant of the S-marginal Euler locus, for the kinematics-numeric
     crawl.  Identical elimination to dpEulerLocus (same shared ideal via
     dpBuildIdeal, same per-keep-variable FF Groebner, same genuine non-monic
     leading coefficient = LCM of the monic-basis coefficient denominators),
     but the eliminant coefficients are reconstructed OVER Q
     (FFReconstructFunction, multi-prime rational reconstruction) and the
     letters are factored OVER Q (FactorList with no Modulus).

   Why over Q.  When the kinematics have been substituted to integers, a
   purely-kinematic quadratic letter (e.g. the box2mh terminal letter
   M1 M2 x2^2 + (M1+M2-s) x2 + 1 with discriminant = Kallen lambda(M1,M2,s))
   has a NUMERIC discriminant.  Mod p that number is a quadratic residue at a
   fair fraction of primes -- the two adjacent SubTropica guard primes are
   QR-correlated for lambda at ~80% of random points -- so the mod-p
   factorization false-splits the letter into two linears and the crawl
   over-accepts (box2mh read 18 admissible orders instead of 0).  Over Q the
   integer discriminant is plainly non-square (negative, at the test point),
   the quadratic stays irreducible, and the verdict is deterministic.  The
   residual risk is the measure-zero accident of the discriminant landing on a
   perfect-square INTEGER (~1e-5 for products of 1e4..1e5 draws, and impossible
   when it is negative); the degree-2 rationalizability layer additionally
   AND-s over several kinematic points.

   No two-prime guard is needed (dpLocusQ is a single exact reconstruction):
   the mod-p false-split the guard existed for cannot occur over Q.

   A failed reconstruction (FFMissingPrimes / FFMissingPoints / FFImpossible)
   ABORTS loudly instead of returning {}: an empty locus reads as "no letters,
   trivially admissible", so silently swallowing a failure would manufacture a
   false-LR verdict.
   ---------------------------------------------------------------------- *)
dpEulerLocusQ[polysIn_List, S_List, allVars_List] :=
 Module[{built, ideal, idealVars, params, keepList, perY, gname, learn, nmon,
    cq, leadL1, trailNum, facs},
  built = dpBuildIdeal[polysIn, S, allVars];
  ideal = built["ideal"]; idealVars = built["idealVars"]; params = built["params"];
  keepList = idealVars;
  perY = Function[keepVar,
     dp$graphCounter++;
     gname = "dpq" <> ToString[dp$graphCounter];
     Quiet@FFDeleteGraph[gname];
     FFNewGraph[gname, "params", params];
     FFAlgGroebner[gname, "gb", ideal, idealVars, params,
        EliminateVariables -> DeleteCases[idealVars, keepVar]];
     FFGraphOutput[gname, "gb"];
     learn = FFGroebnerLearn[gname, idealVars];
     If[! (MatchQ[learn, {_List}] && Length[First[learn]] >= 2),
        Quiet@FFDeleteGraph[gname]; Return[{}, Module]];
     nmon = Length[First[learn]];
     FFAlgTake[gname, "coeff", {"gb"}, Table[{1, j}, {j, 1, nmon}]];
     FFGraphOutput[gname, "coeff"];
     (* over-Q multi-prime rational reconstruction (NOT FFReconstructFunctionMod):
        the kinematics-numeric coefficients are large integers (products of the
        1e4..1e5 kinematic draws), beyond a single 31-bit prime's rational-
        reconstruction range *)
     cq = Together /@ Flatten[{FFReconstructFunction[gname, params]}];
     Quiet@FFDeleteGraph[gname];
     If[! FreeQ[cq, FFMissingPrimes | FFMissingPoints | FFImpossible | $Failed],
        Print["[doppio] FATAL: dpEulerLocusQ over-Q reconstruction failed at S=",
              S, " keep=", keepVar, " (", cq, ")"];
        Abort[]];
     leadL1 = Fold[PolynomialLCM[#1, Denominator[#2]] &, 1, cq];
     trailNum = Numerator[Last[cq]];
     facs = Join[FactorList[leadL1][[All, 1]], FactorList[trailNum][[All, 1]]];
     DeleteCases[facs, c_ /; (Intersection[Variables[c], params] === {})]];
  Union[Flatten[perY /@ keepList]]];

dpLocusQ[polysIn_List, S_List, allVars_List] := dpEulerLocusQ[polysIn, S, allVars];

(* ----------------------------------------------------------------------
   dpReduceA[frontierPolys, v, allVars]

     Variant-A one-variable reduction step (letters-into-letters).

   What it does
   ------------
   Given the already-computed frontier locus L_{S\{v}}^g (a list of mod-p
   polynomials in the kinematic parameters), partition the frontier into:

     * FREE letters: those that do not depend on `v` at all.  Integrating
       `v` leaves a singularity that is independent of `v` unchanged -- it
       must persist in the new frontier.  The old implementation dropped
       them, causing path-dependent (and often empty) terminals.

     * DEPENDENT letters: those that involve `v`.  These are replaced by
       the S-marginal Euler discriminant of the frontier with respect to `v`
       (letters-into-letters, dpLocus2), just as before.

   The new frontier is the edCanonSet union of both parts.  edCanonSet
   deduplicates by primitive-content canonical form; it is already loaded
   from euler_disc_lib.wl.

   This is the letters-into-letters propagation that defines variant A.
   Each size-|S| locus is built by reducing the size-(|S|-1) frontier of
   the same group, rather than being recomputed from scratch from the
   original group polynomials (as variant B does).

   Two-prime guard: dpLocus2 already applies the dpCoarser false-split
   guard internally, so dpReduceA inherits two-prime protection at no extra
   cost; the free letters are already at their final rational form (they
   came from a previous reduction step that applied the same guard), so no
   additional primality check is needed for them.

   Degenerate input: if `frontierPolys` is empty or consists entirely of
   constants (no kinematic dependence), the free-letter Select returns {}
   and dpLocus2 returns {} (vacuous critical system), so the union is {}
   -- correct and safe.
   ---------------------------------------------------------------------- *)
dpReduceA[frontierPolys_List, v_, allVars_] :=
   edCanonSet @ Join[
      Select[frontierPolys, FreeQ[#, v] &],        (* persist: free of v *)
      dpLocus2[frontierPolys, {v}, allVars]];

(* ======================================================================
   PROJECTIVE-INPUT DETECTION + AUTOMATIC CHENG-WU
   ----------------------------------------------------------------------
   A projective integrand -- every group polynomial HOMOGENEOUS in the
   integration variables AND  Sum_i e_i deg P_i == -n  identically in the
   analytic regulators (e_i = the twist exponents of the integrand, n =
   #vars; the measure d^n x contributes +n under scaling) -- is scale-
   invariant: one of the n integrations never happens.  Crawling all n
   variables then demands a fictitious extra reduction; the uq5 5-var NOLR
   (2026-06-04) was exactly this gauge artifact.  The cure is the Cheng-Wu
   gauge: fix one variable to 1 and crawl the remaining n-1.

   Detection REQUIRES the exponents: homogeneity alone is necessary but not
   sufficient (a homogeneous integrand with Sum e_i deg P_i != -n is not
   scale-invariant and its n-variable crawl is legitimate).  The drivers
   therefore take the exponents as an option:

     "Exponents"       -> None (default) | flat exponent list (one entry
                          per polynomial, applied to every group; groups of
                          differing lengths need the list-of-lists form) |
                          list of per-group exponent lists
     "ChengWu"         -> Automatic (default; gauge iff projective) |
                          True (force; ABORT if not verifiable-projective) |
                          False (never gauge)
     "ChengWuVariable" -> Automatic (default; Last[allVars], the
                          collaborator-validated x5 = 1 path) | a variable

   The applied gauge is recorded in the result association under "chengWu"
   ({var -> 1}, or None), together with the EFFECTIVE variable list under
   "vars"; the table / orders / alphabet are over the reduced variables.
   ====================================================================== *)

(* ----------------------------------------------------------------------
   dpProjectiveQ[polys, exps, allVars]

     True iff the twisted integrand  prod_i P_i^{e_i} d^n x  is projective:
     every P_i homogeneous in allVars and  Sum_i e_i deg P_i == -n
     IDENTICALLY in whatever regulator symbols (eps, ...) the exponents
     carry.  The uq5 check: (1+2 eps) * 1 + (-3-eps) * 2 == -5 == -n.

     The zero polynomial and an exps/polys length mismatch return False
     (degenerate input is never auto-gauged; the preamble Aborts separately
     on a malformed forced request).
   ---------------------------------------------------------------------- *)
dpProjectiveQ[polys_List, exps_List, allVars_List] := Module[{crs, degs},
  If[Length[exps] =!= Length[polys] || polys === {}, Return[False]];
  (* zero-polynomial guard via CoefficientRules (it normalizes, so an
     expression that is zero only after expansion is caught too --
     adversarial F8; a bare FreeQ[polys, 0] misses that case) *)
  crs = CoefficientRules[#, allVars] & /@ polys;
  If[MemberQ[crs, {}], Return[False]];
  If[! AllTrue[polys, dpHomogeneousQ[#, allVars] &], Return[False]];
  degs = Total[First[Keys[#]]] & /@ crs;
  (* Together, not Expand: exponents may be RATIONAL functions of the
     regulator (eps-resummed weights, dimension shifts); Expand leaves
     e.g. 1 - 1/(1+eps) - eps/(1+eps) unsimplified and would false-negate
     a genuinely projective input (adversarial F9) *)
  Together[Total[exps degs] + Length[allVars]] === 0];

(* ----------------------------------------------------------------------
   dpChengWuPreamble[groupPolys, allVars, exps, cwOpt, cwVar]

     The shared driver preamble (DoppioFubini AND DoppioCrawl).  Decides
     whether to apply the Cheng-Wu gauge and applies it.  Returns
       <|"polys" -> gauged groupPolys, "vars" -> effective variable list,
         "chengWu" -> {var -> 1} | None|>.

     exps shapes: None (no detection); a FLAT list = one exponent per
     polynomial, applied to every group (all groups must then have that
     same length); a LIST OF LISTS matching the per-group shapes.  The
     gauge is applied only when EVERY group is projective (the groups are
     alternative representations of the same integrand, so a mixed verdict
     signals an input inconsistency: warn, do not gauge -- unless forced,
     which Aborts).  Length[allVars] == 1 is never gauged (nothing would
     remain to crawl).

     A homogeneous-everywhere input WITHOUT exponents gets a one-time hint
     (the un-gauge-fixed raw Symanzik pair is the classic projective trap),
     but is never auto-gauged.
   ---------------------------------------------------------------------- *)
dp$cwHintWarned = False;

dpChengWuPreamble[groupPolys_List, allVars_List, exps_, cwOpt_, cwVar_] :=
 Module[{expsPerGroup, projAll, projAny, cv, untouched},
  untouched = <|"polys" -> groupPolys, "vars" -> allVars, "chengWu" -> None|>;
  (* degenerate input: no groups / no polynomials -- never gauge (an empty
     And@@{} is vacuously True and would gauge on nothing; adversarial F7) *)
  If[groupPolys === {} || Flatten[groupPolys] === {}, Return[untouched]];
  If[cwOpt === False, Return[untouched]];
  (* "ChengWu" -> "Scan" (the FAST gauge scan, user spec 2026-06-04): do NOT
     substitute a gauge; the driver crawls the UNGAUGED n-variable system to
     depth n-1 and the un-integrated variable IS the gauge (projectivity:
     integrating any n-1 variables == fixing the leftover to 1).  One table
     serves ALL n gauges at ~2x the cost of a single gauged run, vs the n-fold
     ChengWuRetry.  Requires verified projectivity (Exponents mandatory). *)
  If[cwOpt === "Scan",
     If[exps === None,
        Print["[doppio] FATAL: \"ChengWu\" -> \"Scan\" requires \"Exponents\"."];
        Abort[]];
     expsPerGroup = Which[
        MatchQ[exps, {__List}] && Length[exps] === Length[groupPolys] &&
          And @@ MapThread[Length[#1] === Length[#2] &, {exps, groupPolys}],
        exps,
        MatchQ[exps, _List] && NoneTrue[exps, ListQ] &&
          AllTrue[groupPolys, Length[#] === Length[exps] &],
        ConstantArray[exps, Length[groupPolys]],
        True,
        Print["[doppio] FATAL: \"Exponents\" shape does not match groupPolys."];
        Abort[]];
     If[! (And @@ MapThread[dpProjectiveQ[#1, #2, allVars] &,
            {groupPolys, expsPerGroup}]),
        Print["[doppio] FATAL: \"ChengWu\" -> \"Scan\" on a non-projective ",
              "input (homogeneity + Sum e_i deg P_i == -n fails)."];
        Abort[]];
     If[Length[allVars] < 2, Return[untouched]];
     Return[<|"polys" -> groupPolys, "vars" -> allVars, "chengWu" -> "Scan"|>]];
  If[exps === None,
     If[cwOpt === True,
        Print["[doppio] FATAL: \"ChengWu\" -> True requires \"Exponents\" ",
              "(projectivity is not decidable from the polynomials alone)."];
        Abort[]];
     (* hint: all-homogeneous input with no exponents is the classic
        projective trap (e.g. a raw Symanzik {U, F} pair, un-gauge-fixed) *)
     If[! TrueQ[dp$cwHintWarned] && Length[allVars] >= 2 &&
        AllTrue[Flatten[groupPolys], dpHomogeneousQ[#, allVars] &],
        dp$cwHintWarned = True;
        Print["[doppio] hint: every group polynomial is homogeneous in the ",
              "integration variables; if the integrand is projective, pass ",
              "\"Exponents\" for automatic Cheng-Wu."]];
     Return[untouched]];
  (* normalize the exponent shape to per-group lists *)
  expsPerGroup = Which[
     MatchQ[exps, {__List}] && Length[exps] === Length[groupPolys] &&
       And @@ MapThread[Length[#1] === Length[#2] &, {exps, groupPolys}],
     exps,
     (* flat list = no element is itself a list (NB FreeQ[exps, _List] would
        be False for EVERY list -- the levelspec includes the expression
        itself -- so element-wise NoneTrue is the correct test) *)
     MatchQ[exps, _List] && NoneTrue[exps, ListQ] &&
       AllTrue[groupPolys, Length[#] === Length[exps] &],
     ConstantArray[exps, Length[groupPolys]],
     True,
     Print["[doppio] FATAL: \"Exponents\" shape does not match groupPolys ",
           "(flat list for equal-length groups, or one list per group)."];
     Abort[]];
  projAll = And @@ MapThread[dpProjectiveQ[#1, #2, allVars] &,
     {groupPolys, expsPerGroup}];
  projAny = Or @@ MapThread[dpProjectiveQ[#1, #2, allVars] &,
     {groupPolys, expsPerGroup}];
  If[! projAll,
     If[cwOpt === True,
        Print["[doppio] FATAL: \"ChengWu\" -> True but the input is not ",
              "projective (homogeneity + Sum e_i deg P_i == -n fails)."];
        Abort[]];
     If[projAny,
        Print["[doppio] WARNING: some but not all groups are projective; ",
              "NOT gauging (groups should represent the same integrand)."]];
     Return[untouched]];
  If[Length[allVars] < 2,
     If[cwOpt === True,
        Print["[doppio] WARNING: \"ChengWu\" -> True on a single-variable ",
              "input is a no-op (nothing would remain to crawl)."]];
     Return[untouched]];
  cv = cwVar /. Automatic -> Last[allVars];
  If[! MemberQ[allVars, cv],
     Print["[doppio] FATAL: \"ChengWuVariable\" ", cv,
           " is not an integration variable."];
     Abort[]];
  (* no degenerate-gauge guard is needed (physics A3): for a HOMOGENEOUS
     nonzero P, two distinct monomials of equal total degree cv^a mu1 and
     cv^b mu2 (mu_i free of cv) map under cv -> 1 to mu1, mu2, which
     coincide only if mu1 == mu2 and hence a == b -- i.e. the monomials
     were identical.  No cancellation: the gauged P cannot vanish. *)
  <|"polys" -> (groupPolys /. cv -> 1),
    "vars" -> DeleteCases[allVars, cv],
    "chengWu" -> {cv -> 1}|>];

(* ----------------------------------------------------------------------
   dpFRJudge[p, v, lo, allVars]

     The per-letter FindRoots tier judgment (the unit-testable core of
     stepFR inside DoppioFubini; see the KeepRule section comment there).
     p = the letter, v = the variable being integrated, lo = integration
     variables still pending AFTER v, allVars = all (gauged) integration
     variables.  Returns
       <|"ok" -> True|False,
         "carry" -> {canonicalized variable-dependent sqrt obligations},
         "kin"  -> 0|1   (a pure-kinematic sqrt was accepted),
         "term" -> 0|1   (the terminal-quadratic tier fired)|>.

   Tier order: deg <= 1 keep; deg >= 3 block; rationalizable quadratic keep
   (Euler conic); terminal quadratic keep (lo === {}; its roots are
   kinematic letters of the answer); otherwise examine sqrt(disc_v p).

   Zero discriminant (adversarial F1 + physics A9): disc == 0 means
   p = A (v - r)^2 with r = -B/(2A) RATIONAL in the pending variables --
   a double linear letter.  Its dlog is d log[A (v-r)^2] = dA/A +
   2 d(v-r)/(v-r): no square root arises at all, so the letter is
   accepted with no counter (blocking it was a pure false-negative).

   Odd-multiplicity part: sqrt(disc) is a rational function times a
   CONSTANT sqrt(numeric content) iff every non-numeric irreducible factor
   has even multiplicity.  The numeric content is deliberately discarded:
   sqrt(2) is a global algebraic constant, not a function of any variable,
   so the hyperlogarithm FUNCTION CLASS is unchanged (physics A8: this
   branch certifies the function-class statement, NOT literal rational
   roots).  Mixed discs (adversarial F2): when the odd part has BOTH
   kinematic and variable-dependent factors, the variable-dependent ones
   are carried AND the kinematic sqrt content is counted in "kin" -- both
   are part of the same sqrt(disc).

   The variable-dependence filter tests against allVars rather than lo
   (physics A10): for table letters the two coincide because L_S letters
   are free of the S-variables; for depth >= 2 carried obligations a factor
   may involve an ALREADY-INTEGRATED variable, which allVars still
   classifies as a (carried) obligation rather than silently calling it
   kinematic -- the conservative direction for this heuristic tier.
   ---------------------------------------------------------------------- *)
(* memoized: dpFRJudge is a pure function of its arguments, and the carry
   DFS re-judges the same (letter, v, lo) triple along combinatorially many
   paths -- on catalogue-size fixtures the memo turns the order enumeration
   from the dominant cost into a table lookup.  Per-kernel cache; fresh
   kernels (the standing discipline) keep it trivially safe. *)
dpFRJudge[p_, v_, lo_List, allVars_List] :=
  dpFRJudge[p, v, lo, allVars] = Module[
   {d = Exponent[p, v], disc, odd, oblig, kinOdd},
   Which[
     d <= 1, <|"ok" -> True, "carry" -> {}, "kin" -> 0, "term" -> 0|>,
     d >= 3, <|"ok" -> False, "carry" -> {}, "kin" -> 0, "term" -> 0|>,
     dpRationalizableQ[p, v, lo]["ok"],
       <|"ok" -> True, "carry" -> {}, "kin" -> 0, "term" -> 0|>,
     lo === {}, <|"ok" -> True, "carry" -> {}, "kin" -> 0, "term" -> 1|>,
     True,
     disc = Discriminant[p, v, Method -> "Subresultants"];
     If[PossibleZeroQ[disc],
        (* double linear letter A (v - r)^2: no sqrt, keep (F1/A9) *)
        Return[<|"ok" -> True, "carry" -> {}, "kin" -> 0, "term" -> 0|>,
           Module]];
     odd = DeleteCases[
        (First[#]^Mod[Last[#], 2]) & /@ FactorList[disc], _?NumericQ];
     oblig = Select[odd, Intersection[Variables[#], allVars] =!= {} &];
     kinOdd = Complement[odd, oblig];
     Which[
       odd === {},
         (* odd part empty: sqrt(disc) = const * rational function *)
         <|"ok" -> True, "carry" -> {}, "kin" -> 0, "term" -> 0|>,
       oblig === {},
         (* pure-kinematic sqrt letter (Lungo's FindRoots exemption) *)
         <|"ok" -> True, "carry" -> {}, "kin" -> 1, "term" -> 0|>,
       True,
         <|"ok" -> True, "carry" -> edCanonSet[oblig],
           "kin" -> If[kinOdd === {}, 0, 1], "term" -> 0|>]]];

(* ----------------------------------------------------------------------
   DoppioFubini[groupPolys, allVars, "Doppio" -> "B"]
   DoppioFubini[groupPolys, allVars, "Doppio" -> "A"]

     The Doppio dynamic-program driver.  Supports two variants selected by
     the "Doppio" option:

       "B"  (default) -- DIRECT variant: each L_S^g computed straight from
                         the original group polynomials via a single
                         S-marginal Euler discriminant.  Independent of the
                         loci at smaller subsets; no frontier drift.
       "A"            -- SEQUENTIAL FRONTIER variant: each L_S^g is built
                         by reducing the already-computed frontier
                         L_{S\{vv}}^g (where vv = First[S]) via a single
                         one-variable dpReduceA step (letters-into-letters).
                         Lighter per-step cost; Task 7 measures whether the
                         sequential propagation reintroduces fictitious
                         factors that variant B avoids.

   What it is
   ----------
   A bottom-up dynamic program over the subsets S of the integration
   variables `allVars`, organized by increasing |S|.  S records WHICH
   variables have already been integrated out; the per-subset locus L_S^g
   for group g is the singularity locus of the S-marginal integrand of that
   group.  The driver decides which integration ORDERS keep every successive
   one-variable integration inside the hyperlogarithm (rationalizable) class,
   and scores each admissible order by the symbolic size of the loci it
   visits (smaller = simpler letters along the way).

   Fill order (variant A correctness requirement)
   -----------------------------------------------
   subsetsBySize = GatherBy[Subsets[allVars], Length] groups subsets into
   size classes {{},  {x1},{x2},...,  {x1,x2},..., ...}.  Iterating over
   Rest[subsetsBySize] (skipping the |S|=0 class) and within each class over
   every S, the dependency S\{vv} has size |S|-1 and was therefore filled in
   a PREVIOUS size class -- the fill is always available when needed.  This
   is the topological order guarantee that makes the sequential variant
   correct without any explicit dependency check.

   The empty-S seed (correctness, differs from the plan draft)
   -----------------------------------------------------------
   S = {} means "nothing integrated yet", and the frontier there is the
   ORIGINAL group polynomials themselves -- NOT {} and NOT a dpLocus2 call.
   A dpLocus2[..., {}, ...] would dlog over an EMPTY variable set (the
   S-marginal critical system is vacuous) and return {}, which is degenerate.
   Seeding  table[{g, {}}] = groupPolys[[g]]  instead makes the admissibility
   test for the FIRST integration variable correctly ask whether the original
   integrand is linear / rationalizable-quadratic in that variable, exactly
   as the later steps ask of L_S^g.  dpLocus2 is therefore called ONLY for
   |S| >= 1.

   Groups
   ------
   Groups are processed SEPARATELY: each carries its own per-(g, S) locus.
   The combined alphabet / table entry for a subset S is the UNION over
   groups (deduplicated by edCanonSet).  Admissibility of appending a
   variable to S must hold for ALL groups simultaneously -- one group with a
   non-rationalizable quadratic blocks the extension for everyone.

   Returns an Association:
     "table"    -> the full DP table.  Keys come in two shapes:
                     {g, Sort[S]}  -> the per-group locus L_S^g (a list)
                     Sort[S]       -> the combined union-over-groups locus
                   so callers can read either the terminal alphabet
                   table[Sort[allVars]] or any intermediate per-group locus.
     "orders"   -> list of {order, cumulativeScore} pairs, one per full
                   integration order all of whose one-variable extensions
                   are admissible, sorted by ascending score (simplest first).
                   {} means no fully-admissible order exists.
     "alphabet" -> the terminal combined locus table[Sort[allVars]]
                   (the Landau alphabet of the fully-integrated integrand).
     "variant"  -> the "Doppio" option value ("A" or "B").
   ---------------------------------------------------------------------- *)
Options[DoppioFubini] = {"Doppio" -> "B", "Exponents" -> None,
   "ChengWu" -> Automatic, "ChengWuVariable" -> Automatic,
   "ChengWuRetry" -> True, "KeepRule" -> "Strict"};

DoppioFubini[groupPolysIn_List, allVarsIn_List, OptionsPattern[]] :=
 Module[{groupPolys, allVars, pre, chengWu, keepRule, cwRetryHit,
         variant, ng, leftoverOf, subsetsBySize, table, alphabet,
         admissibleVQ, stepFR, stepScan, score, orders},
  variant = OptionValue["Doppio"];
  keepRule = OptionValue["KeepRule"];
  If[! MemberQ[{"Strict", "FindRoots"}, keepRule],
     Print["[doppio] DoppioFubini: unknown KeepRule ", keepRule,
           " (use \"Strict\" or \"FindRoots\")."];
     Abort[]];

  (* --- variant guard: "A", "B", "CI", "CII" ("C" = alias for "CII", the
         robust hyperplane-twisted form) --- *)
  If[! MemberQ[{"A", "B", "C", "CI", "CII"}, variant],
     Print["[doppio] DoppioFubini: unknown variant ", variant,
           " (use \"A\", \"B\", \"CI\" or \"CII\")."];
     Abort[]];
  If[variant === "C", variant = "CII"];

  (* --- backend guard, VARIANT-AWARE (Task 8): A and B run FF32/msolve
         eliminations and need the SPQR paclet; the CI/CII production path
         (Lungo-core + self-contained cleared-dlog counters) is pure
         Mathematica and runs in any kernel, e.g. inside SubTropica with no
         SPQR present.  (The IncidenceLine chi backend would still need
         SPQR; dpGenuineQ's default is the self-contained DiscKosky copy.) *)
  If[MemberQ[{"A", "B"}, variant], dpAssertBackend[]];

  (* --- projective-input detection + automatic Cheng-Wu (see the section
         header above dpProjectiveQ); everything below runs on the gauged
         polynomials over the reduced variable list --- *)
  pre = dpChengWuPreamble[groupPolysIn, allVarsIn, OptionValue["Exponents"],
     OptionValue["ChengWu"], OptionValue["ChengWuVariable"]];
  groupPolys = pre["polys"]; allVars = pre["vars"]; chengWu = pre["chengWu"];

  ng = Length[groupPolys];
  (* integration variables not yet in S (parameters of the S-marginal) *)
  leftoverOf[S_] := Complement[allVars, S];

  (* --- table fill, variant-aware ---
     L_{} = original group polys for BOTH variants (empty-S seed; see above).
     Variant B: L_S (|S| >= 1) = direct per-group S-marginal Euler discriminant
                of the ORIGINAL group polys.
     Variant A: L_S = dpReduceA applied to the FRONTIER L_{S\{vv}}, where
                vv = First[S].  Subsets are visited in increasing-|S| order
                (via subsetsBySize) so the dependency is always pre-filled. *)
  subsetsBySize = GatherBy[Subsets[allVars], Length];
  table = Association[];
  Do[table[{g, {}}] = groupPolys[[g]], {g, ng}];
  Which[
   variant === "B",
   (* variant B: recompute each locus directly from the original group polys *)
   Do[If[Length[S] >= 1,
       Do[table[{g, Sort[S]}] = dpLocus2[groupPolys[[g]], S, allVars], {g, ng}]],
      {S, Subsets[allVars]}],
   variant === "A",
   (* variant A: sequential frontier reduction by increasing |S|.
      Reach S by removing its first variable vv and reducing the frontier
      table[{g, S\{vv}}] by vv.  Rest[subsetsBySize] skips the |S|=0 class
      (already filled above); within each size class the order is arbitrary
      because all dependencies are one size smaller and hence pre-filled. *)
   Do[Do[Do[
       With[{vv = First[S]},
        table[{g, Sort[S]}] =
         dpReduceA[table[{g, Sort[DeleteCases[S, vv]]}], vv, allVars]],
       {g, ng}], {S, sizeClass}], {sizeClass, Rest[subsetsBySize]}],
   MemberQ[{"CI", "CII"}, variant],
   (* variants CI/CII: Lungo-core generator (leading coeffs + discriminants +
      pairwise resultants, factored, Fubini-intersected over removal choices,
      AUGMENTED seed {group polys, x_1..x_n}) with the per-subset Euler
      chi-drop filter dropping fictitious letters.  CI tests against the
      master product P_1^p_1...P_m^p_m; CII additionally carries the
      coordinate-hyperplane factors v^{q_v} (see dpDKPsi) -- the robust form,
      and the "C" alias.  Boundary letters x_j are EXEMPT from the filter:
      they are integration-domain boundaries (and the source of trailing
      coefficients via Resultant[f, x_j]), not Landau components -- testing
      them would reproduce the boundary-poly draining bug.  Letters here are
      symbolic over Q, so the admissibility and score layers below operate
      exactly. *)
   With[{hyp = (variant === "CII")},
    table = dpLungoCore[groupPolys, allVars,
       Function[{g, Ss, letters},
         Select[letters,
           MemberQ[allVars, #] ||
             dpGenuineQ[groupPolys[[g]], Ss, allVars, #,
                "Hyperplanes" -> hyp] &]]
       ]["table"]]
  ];

  (* combined per-S entry = union over groups (deduplicated by edCanonSet).
     edCanon makes each mod-p factor primitive over Z; for a single group
     this is just per-poly canonicalization (no cross-group dedup needed). *)
  Do[table[Sort[S]] = edCanonSet@Flatten[Table[table[{g, Sort[S]}], {g, ng}]],
     {S, Subsets[allVars]}];
  alphabet = table[Sort[allVars]];

  (* --- admissibility of appending v to S ---
     For EVERY group, every locus polynomial of L_S^g must be either
       (a) linear or lower in v (degree <= 1), or
       (b) a quadratic in v whose conic rationalizes (dpRationalizableQ).
     The leftover variables passed to the rationalizability test are those
     remaining AFTER v is integrated, leftoverOf[Append[S, v]]: A and C must
     be perfect squares in exactly the still-pending integration variables. *)
  admissibleVQ[S_, v_] := AllTrue[Range[ng], Function[g,
     Module[{L = table[{g, Sort[S]}], degs},
       degs = dpDegreeInVar[L, v];
       AllTrue[Transpose[{L, degs}], Function[lp,
         lp[[2]] <= 1 ||
         (lp[[2]] == 2 &&
            dpRationalizableQ[lp[[1]], v, leftoverOf[Append[S, v]]]["ok"])]]]]];

  (* --- score of reaching S ---
     sum over groups of the total LeafCount of the per-group locus L_S^g.
     Smaller loci = simpler intermediate letters; used to rank orders. *)
  score[S_] := Total[Table[Total[LeafCount /@ table[{g, Sort[S]}]], {g, ng}]];

  (* --- FindRoots relaxed step ("KeepRule" -> "FindRoots") ---
     Per-letter tiers at step (S, v), leftover lo = vars pending AFTER v:
       deg_v <= 1                      -> keep (as Strict);
       deg_v == 2, conic rationalizes  -> keep (as Strict);
       deg_v == 2, lo === {}           -> keep: TERMINAL quadratic; its roots
                                          are kinematic letters of the answer
                                          (FindRoots semantics);
       deg_v == 2, otherwise           -> examine the v-discriminant's
                                          ODD-multiplicity part (the actual
                                          sqrt argument):
            empty (perfect square)     -> keep (rational roots, no sqrt);
            pure-kinematic             -> keep: kinematic sqrt letter
                                          (Lungo's FindRoots exemption);
            variable-dependent         -> keep AND CARRY each new obligation
                                          polynomial: it joins the letter set
                                          of every later step and must itself
                                          pass these same tiers (in uq5 the
                                          carried x4-quadratic is discharged
                                          by the terminal tier);
       deg_v >= 3                      -> block (both rules).
     Carried obligations are canonicalized (edCanon) and discharged once they
     are free of every still-pending integration variable.  Counters:
     carriedSqrts = distinct variable-dependent obligations carried along the
     path; kinSqrts = pure-kinematic sqrt acceptances (non-terminal);
     terminalQuads = terminal-tier acceptances.  This tier is a NECESSARY-only
     relaxation in the same sense as production Lungo's FindRoots -> True:
     the algebraic extension's own downstream disc/resultant letters are NOT
     regenerated, and the reported alphabet is NOT extended by the carried
     root letters (the chi-filter still certifies the table letters).
     Variants A/B carry mod-p letters (dpEulerLocus reconstructs with
     FFReconstructFunctionMod, so coefficients are mod-p representatives --
     exact whenever the true integer coefficients are small, which is the
     box-family regime), so the disc there is over those representatives
     (same approximation their rationalizability test already makes);
     variant C letters are exact over Q.

     CONFIDENCE GRADIENT (adversarial F5): under FindRoots EVERY returned
     order satisfies only the necessary tiers; orders with LARGE
     carriedSqrts counts are nested-radical chains far from the
     collaborator-validated depth-1 pattern.  Consumers should prefer the
     SCORE-MINIMAL order (the sorted head) and treat deep-carry orders as
     speculative candidates, not certified reductions. *)
  stepFR[S_, v_, carried_] := Module[{lo, letters, newCarried,
      nsq = 0, nkin = 0, ntq = 0, ok},
    lo = leftoverOf[Append[S, v]];
    (* one representative per proportionality class: groups are alternative
       representations of the same integrand, so the same letter surfacing
       from several groups (e.g. the +-F pairs of per-face data) must be
       judged ONCE or the kinSqrts/terminalQuads counters over-count
       (adversarial F3).  GroupBy on the edCanon key keeps the FIRST raw
       representative, so the judged polynomial is byte-identical to one the
       strict path sees; FindRoots admissibility itself is unaffected by the
       choice of representative (deg <= 1 / >= 3 and the discriminant's odd
       part are sign-insensitive, and deg-2 letters never hard-block). *)
    letters = Values[GroupBy[
       Join[Flatten[Table[table[{g, Sort[S]}], {g, ng}]], carried],
       edCanon, First]];
    (* discharge carried obligations now free of every pending variable *)
    newCarried = Select[carried,
       Intersection[Variables[#], lo] =!= {} &];
    ok = AllTrue[letters, Function[p, Module[{r, fresh},
       r = dpFRJudge[p, v, lo, allVars];
       If[! r["ok"], False,
          nkin += r["kin"]; ntq += r["term"];
          If[r["carry"] =!= {},
             fresh = Select[r["carry"], ! MemberQ[newCarried, #] &];
             nsq += Length[fresh];
             newCarried = edCanonSet[Join[newCarried, fresh]]];
          True]]]];
    <|"ok" -> ok, "carried" -> newCarried, "newSqrts" -> nsq,
      "newKinSqrts" -> nkin, "newTerminalQuads" -> ntq|>];

  (* --- fast-gauge-scan step ("ChengWu" -> "Scan") ---
     Per-gauge admissibility on the UNGAUGED table: each still-viable gauge
     j tests the letters DEHOMOGENIZED at x_j -> 1 (exactly the gauge-j
     run's letters: dehomogenization commutes with the disc/resultants in
     v != x_j and preserves irreducibility for x_j-coprime letters; x_j
     itself dehomogenizes to a dropped constant).  Strict and FindRoots
     judges as in the gauged paths; carried obligations and counters are
     tracked PER GAUGE.  A gauge drops out when integrated or when a letter
     fails its test; an empty survivor set prunes the subtree. *)
  stepScan[S_, v_, st_] := Module[{lo, lettersRaw, out = <||>},
    lo = leftoverOf[Append[S, v]];
    lettersRaw = Flatten[Table[table[{g, Sort[S]}], {g, ng}]];
    Do[
      Module[{gj = gz, loJ, lettersJ, carried, newCarried,
              nsq, nkin, ntq, ok},
       If[gj === v, Continue[]];
       loJ = DeleteCases[lo, gj];
       (* dehomogenize FIRST, dedup AFTER (per gauge), and dedup ACROSS the
          join with the carried obligations: two distinct homogeneous
          letters can collapse to one gauge-j letter, and a carried disc
          can coincide with a table letter -- the explicit gauged stepFR
          judges each edCanon class ONCE (table representative first), so
          the scan must too or the kin/term counters drift (both caught by
          t24's profile-parity assert) *)
       carried = st[gj, "carried"];
       lettersJ = Values[GroupBy[
          Join[DeleteCases[
              (If[FreeQ[#, gj], #, # /. gj -> 1] &) /@ lettersRaw,
              _?NumericQ], carried],
          edCanon, First]];
       nsq = st[gj, "nsq"]; nkin = st[gj, "nkin"]; ntq = st[gj, "ntq"];
       newCarried = Select[carried,
          Intersection[Variables[#], loJ] =!= {} &];
       ok = AllTrue[lettersJ, Function[pd,
          Module[{r, fresh, d},
           If[keepRule === "FindRoots",
            r = dpFRJudge[pd, v, loJ, allVars];
            If[! r["ok"], False,
               nkin += r["kin"]; ntq += r["term"];
               If[r["carry"] =!= {},
                  fresh = Select[r["carry"], ! MemberQ[newCarried, #] &];
                  nsq += Length[fresh];
                  newCarried = edCanonSet[Join[newCarried, fresh]]];
               True],
            d = Exponent[pd, v];
            d <= 1 || (d == 2 && dpRationalizableQ[pd, v, loJ]["ok"])]]]];
       If[ok, out[gj] = <|"carried" -> newCarried, "nsq" -> nsq,
          "nkin" -> nkin, "ntq" -> ntq|>]],
      {gz, Keys[st]}];
    out];

  orders = Which[
     chengWu === "Scan",
     dpAdmissibleOrdersScan[allVars, stepScan, score],
     keepRule === "FindRoots",
     dpAdmissibleOrdersCarry[allVars, stepFR, score],
     True,
     dpAdmissibleOrders[allVars, admissibleVQ, score]];

  (* --- Cheng-Wu gauge retry (adversarial F4) ---
     The integral is gauge-invariant but the CRAWL is not: the admissible-
     order set depends on which variable was fixed, and the default
     Last[allVars] can under-find (observed up to 2x on small fixtures) or
     even read NOLR where another gauge succeeds.  So a NOLR verdict under
     an applied gauge is retried at every other gauge variable before being
     reported; opt out with "ChengWuRetry" -> False (the recursion itself
     always passes False).  A NOLR that survives all gauges is reported as
     such -- it is still only gauge-exhaustive, not a reducibility proof. *)
  cwRetryHit = If[
     chengWu =!= None && chengWu =!= "Scan" && orders === {} &&
       TrueQ[OptionValue["ChengWuRetry"]],
     Catch[
       Module[{res2},
        Do[
          res2 = DoppioFubini[groupPolysIn, allVarsIn, "Doppio" -> variant,
             "Exponents" -> OptionValue["Exponents"], "ChengWu" -> Automatic,
             "ChengWuVariable" -> v2, "ChengWuRetry" -> False,
             "KeepRule" -> keepRule];
          If[res2["orders"] =!= {},
             Print["[doppio] NOLR at Cheng-Wu gauge ", chengWu[[1, 1]],
                   " -> 1; retry found ", Length[res2["orders"]],
                   " admissible orders at gauge ", v2, " -> 1."];
             Throw[res2, "dpCWRetry"]],
          {v2, DeleteCases[allVarsIn, chengWu[[1, 1]]]}];
        Print["[doppio] NOLR at every Cheng-Wu gauge (all ",
              Length[allVarsIn], " variables tried)."];
        Null],
       "dpCWRetry"],
     Null];
  If[AssociationQ[cwRetryHit], Return[cwRetryHit]];

  <|"table" -> table, "orders" -> orders, "alphabet" -> alphabet,
    "variant" -> variant, "chengWu" -> chengWu, "vars" -> allVars,
    "keepRule" -> keepRule|>];

(* ----------------------------------------------------------------------
   dpAdmissibleOrders[allVars, admissibleVQ, score]

     Enumerate every full integration order (permutation of allVars) all of
     whose one-variable prefixes extend admissibly, ranked by cumulative
     score (lower = simpler loci visited).

   Method
   ------
   Depth-first search over admissible prefixes.  Starting from the empty
   prefix S = {}, at each node we keep only the not-yet-integrated variables
   v for which admissibleVQ[S, v] holds, and recurse on S U {v}, accumulating
   score[S U {v}].  A leaf at |S| = |allVars| records the completed order with
   its cumulative score.  The pruning is what keeps this tractable: an
   inadmissible variable cuts the whole subtree below it, so non-rationalizable
   branches are never explored to full depth.

   Returns the list of {order, cumulativeScore} pairs sorted by ascending
   score; {} if no fully-admissible order exists.
   ---------------------------------------------------------------------- *)
dpAdmissibleOrders[allVars_, admissibleVQ_, score_] :=
 Module[{results = {}, extend},
  extend[S_, ord_, sc_] := Module[{cands},
    If[Length[S] === Length[allVars],
       AppendTo[results, {ord, sc}]; Return[Null]];
    cands = Select[Complement[allVars, S], admissibleVQ[S, #] &];
    Do[extend[Append[S, v], Append[ord, v], sc + score[Append[S, v]]],
       {v, cands}]];
  extend[{}, {}, 0];
  SortBy[results, Last]];

(* ----------------------------------------------------------------------
   dpAdmissibleOrdersCarry[allVars, stepFn, score]

     The carried-state variant of dpAdmissibleOrders, for the FindRoots
     relaxed keep-rule.  Admissibility is PATH-DEPENDENT there: a tier-(b)
     acceptance carries the sqrt argument (the odd part of the letter's
     v-discriminant) as an obligation that every LATER step of the same
     path must re-judge, so the DFS threads {carried, counters} alongside
     the prefix.  stepFn[S, v, carried] returns
       <|"ok" ->, "carried" -> updated obligation set, "newSqrts" ->,
         "newKinSqrts" ->, "newTerminalQuads" ->|>
     (see stepFR inside DoppioFubini).  Completed orders are recorded as
       {order, cumulativeScore,
        <|"carriedSqrts" ->, "kinSqrts" ->, "terminalQuads" ->|>}
     triples, sorted by ascending score.  An inadmissible candidate prunes
     its whole subtree exactly as in the strict DFS.
   ---------------------------------------------------------------------- *)
dpAdmissibleOrdersCarry[allVars_, stepFn_, score_] :=
 Module[{results = {}, extend},
  extend[S_, ord_, sc_, carried_, nsq_, nkin_, ntq_] := Module[{r},
    If[Length[S] === Length[allVars],
       AppendTo[results, {ord, sc,
          <|"carriedSqrts" -> nsq, "kinSqrts" -> nkin,
            "terminalQuads" -> ntq|>}]; Return[Null]];
    Do[
      r = stepFn[S, v, carried];
      If[TrueQ[r["ok"]],
         extend[Append[S, v], Append[ord, v], sc + score[Append[S, v]],
            r["carried"], nsq + r["newSqrts"], nkin + r["newKinSqrts"],
            ntq + r["newTerminalQuads"]]],
      {v, Complement[allVars, S]}]];
  extend[{}, {}, 0, {}, 0, 0, 0];
  SortBy[results, #[[2]] &]];

(* ----------------------------------------------------------------------
   dpAdmissibleOrdersScan[allVars, stepFn, score]

     The FAST-GAUGE-SCAN DFS ("ChengWu" -> "Scan"): enumerate admissible
     orders of n-1 OF THE n ungauged variables; the un-integrated leftover
     variable IS the Cheng-Wu gauge.  The state threaded per prefix is an
     Association  gauge -> <|"carried" -> _, "nsq" -> _, "nkin" -> _,
     "ntq" -> _|>  over the gauges still viable for this path (a gauge
     drops out when it gets integrated, or when a letter fails its
     DEHOMOGENIZED admissibility test at that gauge -- the per-gauge mask
     that makes the scan EXACTLY the union of the n gauged runs).
     stepFn[S, v, state] returns the surviving-gauge state (<||> prunes).
     Records {order, score, gauge, profile} per surviving gauge at depth
     n-1, sorted by score.
   ---------------------------------------------------------------------- *)
dpAdmissibleOrdersScan[allVars_, stepFn_, score_] :=
 Module[{results = {}, n = Length[allVars], extend},
  extend[S_, ord_, sc_, st_] := Module[{stNew},
    If[Length[S] === n - 1,
       Do[AppendTo[results, {ord, sc, g,
           <|"carriedSqrts" -> st[g, "nsq"], "kinSqrts" -> st[g, "nkin"],
             "terminalQuads" -> st[g, "ntq"]|>}], {g, Keys[st]}];
       Return[Null]];
    Do[
      stNew = stepFn[S, v, st];
      If[stNew =!= <||>,
         extend[Append[S, v], Append[ord, v], sc + score[Append[S, v]], stNew]],
      {v, Complement[allVars, S]}]];
  extend[{}, {}, 0, Association[# -> <|"carried" -> {}, "nsq" -> 0,
     "nkin" -> 0, "ntq" -> 0|> & /@ allVars]];
  SortBy[results, #[[2]] &]];

(* ======================================================================
   CRAWL PATH  --  no multivariate reconstruction
   ----------------------------------------------------------------------
   The functions below implement the FAST, TIMED crawl path.  Unlike the
   alphabet path (dpEulerLocus / dpLocus2 / DoppioFubini), which reconstructs
   the per-subset locus to full multivariate mod-p symbolic letters, the crawl
   path needs only two things at each (S, v):

     (a) the DEGREE of the locus in the next integration variable v, factor by
         factor (linear / rationalizable-quadratic / drop >= 3), and
     (b) a LeafCount-style score for ranking admissible orders.

   Neither needs the multivariate letters.  The crawl therefore fixes every
   parameter EXCEPT v to a random F_p value, leaving v the only symbol; the
   elimination then yields a UNIVARIATE-in-v object, which is reconstructed and
   factored over F_p[v] (O(deg_v) samples, cheap univariate factoring) instead
   of the slow multivariate FFReconstructFunctionMod over all params.

   FALSE-SPLIT HAZARD AND THE COARSEST GUARD
   -----------------------------------------
   Numericizing every other parameter to a POINT is less robust than the
   alphabet path's multivariate factorization: a genuine irreducible quadratic
   in v, say v^2 - D(params), specialized at a random point becomes v^2 - d for
   a fixed d in F_p, and that splits over F_p exactly when d is a quadratic
   residue -- probability ~1/2.  A split quadratic reads as TWO linear letters,
   which would wrongly mark an inadmissible (non-rationalizable) quadratic as
   admissible.  Specialization can only SPLIT factors, never MERGE them, so the
   COARSEST factorization seen over K independent random points is the closest
   to the true factorization over Q(params).  dpDegProbeMultiset takes K samples
   and returns the coarsest (max total v-degree, then fewest factors) multiset;
   the residual miss probability for a genuine quadratic is ~2^{-K}.  The
   alphabet path remains the authoritative cross-check.
   ====================================================================== *)

(* ----------------------------------------------------------------------
   dpBuildIdeal[polysIn, S, allVars]

     Shared constructor of the S-marginal critical ideal used by the crawl
     probe.  Mirrors steps 1-5 of dpEulerLocus EXACTLY (same seeded exponents,
     same Rabinowitsch x0, same idealVars = {x0} U S) so that the crawl probe
     and the alphabet path see the identical ideal; only the downstream
     reconstruction differs (univariate-in-v point sampling vs. full
     multivariate).  Returns an Association with keys "ideal", "idealVars",
     "params", "x0".

     Requires Length[S] >= 1 (the empty-S marginal is handled directly by
     dpDegProbeMultiset, which reads the raw group polynomials -- the DP seed).
   ---------------------------------------------------------------------- *)
dpBuildIdeal[polysIn_List, S_List, allVars_List] :=
 Module[{polys, x0, bs, cs, logPsi, dlog, num, den, rab, ideal, idealVars, params},
  polys = dpToAtomic /@ polysIn;
  x0 = Symbol["x0"];
  SeedRandom[20240602];
  bs = RandomSample[Range[3, 400], Length[polys]];
  cs = RandomSample[Complement[Range[3, 400], bs], Length[allVars]];
  logPsi = Total[MapThread[#2 Log[#1] &, {polys, bs}]] +
           Total[MapThread[#2 Log[#1] &, {allVars, cs}]];
  dlog = Factor[D[logPsi, {S}]];
  num = Numerator[dlog]; den = Denominator[dlog];
  rab = 1 - x0 (Apply[PolynomialLCM, den]);
  ideal = Join[num, {rab}];
  idealVars = Prepend[S, x0];
  params = Complement[Variables[ideal], idealVars];
  <|"ideal" -> ideal, "idealVars" -> idealVars, "params" -> params, "x0" -> x0|>];

(* ----------------------------------------------------------------------
   dpMonicModP[f, v, p]

     Normalize the univariate-in-v polynomial f to be monic in v modulo p,
     so that two factors that describe the same letter but were returned with
     different F_p leading coefficients (e.g. the same letter surfacing from
     two different keep-variable projections) dedup to a single representative.
     f is assumed univariate in v after numericization, so its leading
     coefficient is a plain F_p integer and PowerMod gives its inverse.
   ---------------------------------------------------------------------- *)
dpMonicModP[f_, v_, p_] := Module[{d = Exponent[f, v], lc},
   lc = Coefficient[f, v, d];
   PolynomialMod[Expand[f PowerMod[lc, -1, p]], p]];

(* ----------------------------------------------------------------------
   dpProbeProject[idealNum, idealVars, keepVar, v, prime]

     Single keep-variable projection of the NUMERICIZED ideal (all parameters
     except v already substituted to F_p constants).  Eliminates every idealVar
     except keepVar via the SPQR-backed FF Groebner, reconstructs the eliminant
     coefficients as functions of the single parameter v (cheap univariate
     reconstruction), then returns the v-DEPENDENT factors of the genuine
     non-monic leading coefficient L1 and of the trailing numerator, over F_p[v].

     Mirrors dpEulerLocus's perY exactly, with params = {v} instead of the full
     parameter list.  v-free factors (pure F_p constants and any leftover-unit)
     are dropped: they carry no v-dependence and are trivially admissible.
   ---------------------------------------------------------------------- *)
dpProbeProject[idealNum_List, idealVars_List, keepVar_, v_, prime_] :=
 Module[{gname, learn, nmon, cmod, leadL1, trailNum, facs},
  dp$graphCounter++;
  gname = "dpp" <> ToString[dp$graphCounter];
  Quiet@FFDeleteGraph[gname];
  FFNewGraph[gname, "params", {v}];
  FFAlgGroebner[gname, "gb", idealNum, idealVars, {v},
     EliminateVariables -> DeleteCases[idealVars, keepVar]];
  FFGraphOutput[gname, "gb"];
  learn = FFGroebnerLearn[gname, idealVars];
  If[! (MatchQ[learn, {_List}] && Length[First[learn]] >= 2),
     Quiet@FFDeleteGraph[gname]; Return[{}, Module]];
  nmon = Length[First[learn]];
  FFAlgTake[gname, "coeff", {"gb"}, Table[{1, j}, {j, 1, nmon}]];
  FFGraphOutput[gname, "coeff"];
  cmod = Together /@ Flatten[{FFReconstructFunctionMod[gname, {v},
       "StartingPrimeNo" -> dpPrimeNo[prime]]}];
  Quiet@FFDeleteGraph[gname];
  leadL1 = Fold[PolynomialLCM[#1, Denominator[#2], Modulus -> prime] &, 1, cmod];
  trailNum = Numerator[Last[cmod]];
  facs = Join[
     FactorList[leadL1, Modulus -> prime][[All, 1]],
     FactorList[trailNum, Modulus -> prime][[All, 1]]];
  DeleteCases[facs, c_ /; FreeQ[c, v]]];

(* ----------------------------------------------------------------------
   dpDegProbeSample[polysIn, S, allVars, v, prime, seed]

     ONE numericized sample of the S-marginal locus's v-dependence.  Builds the
     shared ideal, substitutes every parameter except v to DISTINCT nonzero
     random F_p values (distinctness avoids the intra-sample collision where two
     genuinely different letters would coincide), runs every keep-variable
     projection, and returns the deduplicated list of v-dependent factors
     (each monic in v mod prime).
   ---------------------------------------------------------------------- *)
dpDegProbeSample[polysIn_List, S_List, allVars_List, v_, prime_, seed_] :=
 Module[{built, ideal, idealVars, params, otherParams, subst, idealNum, factors},
  built = dpBuildIdeal[polysIn, S, allVars];
  ideal = built["ideal"]; idealVars = built["idealVars"]; params = built["params"];
  otherParams = DeleteCases[params, v];
  SeedRandom[seed];
  subst = Thread[otherParams ->
     RandomSample[Range[1, prime - 1], Length[otherParams]]];
  idealNum = ideal /. subst;
  factors = Flatten[dpProbeProject[idealNum, idealVars, #, v, prime] & /@ idealVars];
  DeleteDuplicates[dpMonicModP[#, v, prime] & /@ factors]];

(* ----------------------------------------------------------------------
   dpDegProbeMultiset[polysIn, S, allVars, v, opts]

     The crawl degree probe: the per-letter v-degree multiset of the S-marginal
     locus, restricted to v-dependent letters, computed WITHOUT multivariate
     reconstruction.  Options:
       "Samples" -> K     number of random points for the coarsest guard (8)
       "Prime"   -> p     prime for the F_p[v] factoring (FFPrimeNo[0])

     S = {} is handled directly: the locus is the raw group polynomials (the DP
     seed), whose degree in v is exact and symbolic -- no probe needed.

     For |S| >= 1, takes K random-point samples and returns the COARSEST
     v-degree multiset: among the samples of maximal total v-degree (which
     discards rare leading-coefficient degeneracies), the one with the fewest
     factors (which discards spurious point-splits of a genuine quadratic).
     Returns also kept as a bare multiset (sorted ascending) so admissibility
     reads Max and counts of 2's directly.
   ---------------------------------------------------------------------- *)
(* ----------------------------------------------------------------------
   dpCoarsestMultiset[multisets]

     Select the true v-degree partition from K per-sample multisets.

   Each multiset is the sorted list of v-degrees observed at one random point.
   Across samples every multiset is a REFINEMENT of a single underlying
   partition (numeric specialization only splits factors, never merges them) and
   -- away from a rare leading-coefficient degeneracy -- preserves the total
   v-degree.  Therefore the underlying partition is recovered as:

     1. the maximal total v-degree over the samples (this discards any sample in
        which a leading coefficient happened to vanish at the point, dropping a
        factor's degree and hence the total), then
     2. among those, the multiset with the FEWEST parts (this discards spurious
        point-splits of a genuine higher-degree factor, since a split only adds
        parts).

   Pure list logic, no kernel involved; tested directly in t09.
   ---------------------------------------------------------------------- *)
dpCoarsestMultiset[multisets_List] := Module[{maxTotal, cand},
   maxTotal = Max[Total /@ multisets];
   cand = Select[multisets, Total[#] === maxTotal &];
   First[SortBy[cand, Length]]];

Options[dpDegProbeMultiset] = {"Samples" -> 8, "Prime" -> Automatic};
dpDegProbeMultiset[polysIn_List, S_List, allVars_List, v_, OptionsPattern[]] :=
 Module[{k = OptionValue["Samples"], prime, multisets},
  If[S === {},
     Return[Sort[DeleteCases[dpDegreeInVar[polysIn, v], 0]]]];
  prime = OptionValue["Prime"] /. Automatic -> FFPrimeNo[0];
  multisets = Table[
     Sort[Exponent[#, v] & /@ dpDegProbeSample[polysIn, S, allVars, v, prime, 1000 + j]],
     {j, 1, k}];
  dpCoarsestMultiset[multisets]];

(* ----------------------------------------------------------------------
   DoppioCrawl[groupPolys, allVars, "Probe" -> mode]

     The TIMED crawl driver.  Finds every admissible (linearly reducible)
     integration order and returns the SAME admissible orders as DoppioFubini
     variant B (validated in t11/t12), driving admissibility from a cheap
     degree-in-v probe instead of from the alphabet path's symbolic letters.

   Two probe modes ("Probe" option):

     "KinNumeric" (DEFAULT) -- numericize the KINEMATICS to random integers and
        keep the INTEGRATION variables symbolic.  One multivariate reconstruction
        over the integration variables (dpLocus2 on polys with kinematics
        substituted) gives the per-subset locus; the degree in EVERY leftover v
        is read from that single locus.  Because the integration variables stay
        symbolic, the discriminant of a quadratic-in-v letter is a POLYNOMIAL
        (irreducible-or-not robustly), so there is NO half-probability false
        split and NO sample guard.  Far fewer reconstruction parameters than the
        symbolic-kinematics engine (the kinematics are gone), so each
        reconstruction is much cheaper -- this is the fast, robust crawl.

     "UnivariateV" -- numericize EVERY parameter except v to random F_p values,
        eliminate to a univariate-in-v object, factor over F_p[v], take the
        coarsest of K samples (dpDegProbeMultiset).  Strictly no multivariate
        reconstruction, but it needs the K-sample false-split guard and re-runs
        the elimination once per numeric point per (S, v); on small fixtures it
        is slower than both KinNumeric and B.  Kept for the record/comparison.

   The rare degree-2 branch (rat2) decides whether a quadratic-in-v letter's
   Euler conic rationalizes.  Under "KinNumeric" it stays kinematics-numeric:
   it AND-s the per-letter dpRationalizableQ verdict over the nRatPts ("RatPoints")
   numeric locus points, so a non-rationalizable (e.g. Kallen) letter is rejected
   without the expensive kinematics-symbolic reconstruction, and the rare
   perfect-square-integer accident is killed by multi-point agreement (cf.
   dpRationalizableNumQ, t13).  Under "UnivariateV" it falls back to the
   kinematics-symbolic locus (raw polys at S = {}, dpLocus2 for |S| >= 1).  The
   branch never fires on fully linear integrands (e.g. the two-mass box).

   Memoization: the per-(group, sorted-S) locus (KinNumeric) or per-(group,
   sorted-S, v) probe (UnivariateV) is computed at most once, so the DFS over
   admissible prefixes never repeats work when a subset is reachable by several
   orders.

   Returns an Association:
     "orders"     -> {order, cumulativeProxyScore} pairs for every fully
                     admissible order, sorted by ascending proxy score.  {} if
                     none.  The order SET matches DoppioFubini-B; the proxy score
                     is a crawl-only ranking, NOT the alphabet path's exact
                     LeafCount.
     "variant"    -> "Crawl"
     "probe"      -> the "Probe" mode used
     "samples"    -> K (UnivariateV guard sample count; unused by KinNumeric)
     "probeCalls" -> distinct probe/locus computations run (a work diagnostic).
   ---------------------------------------------------------------------- *)
Options[DoppioCrawl] = {"Samples" -> 8, "Probe" -> "KinNumeric",
   "KinSeed" -> 314159, "RatPoints" -> 3, "Exponents" -> None,
   "ChengWu" -> Automatic, "ChengWuVariable" -> Automatic,
   "ChengWuRetry" -> True, "KeepRule" -> "Strict"};
DoppioCrawl[groupPolysIn_List, allVarsIn_List, OptionsPattern[]] :=
 Module[{k = OptionValue["Samples"], probe = OptionValue["Probe"],
         kinSeed = OptionValue["KinSeed"], nRatPts = OptionValue["RatPoints"],
         groupPolys, allVars, pre, chengWu, cwRetryHit,
         ng, leftoverOf, kinematics, kinSubstList, locusKinPt, degOf, rat2,
         admissibleVQ, edgeCost, orders, nProbe = 0},
  dpAssertBackend[];
  If[! MemberQ[{"KinNumeric", "UnivariateV"}, probe],
     Print["[doppio] DoppioCrawl: unknown Probe ", probe,
           " (use \"KinNumeric\" or \"UnivariateV\")."]; Abort[]];
  (* the FindRoots relaxed keep-rule is DoppioFubini-only (deferred for the
     crawl); without this guard the option would be silently swallowed and
     a user expecting relaxed semantics would get Strict orders
     (adversarial F6) *)
  If[OptionValue["KeepRule"] =!= "Strict",
     Print["[doppio] DoppioCrawl: \"KeepRule\" -> ",
           OptionValue["KeepRule"], " is not supported here -- the ",
           "FindRoots tier is DoppioFubini-only (deferred for the crawl)."];
     Abort[]];
  If[OptionValue["ChengWu"] === "Scan",
     Print["[doppio] DoppioCrawl: \"ChengWu\" -> \"Scan\" is ",
           "DoppioFubini-only (the crawl has no depth-(n-1) semantics)."];
     Abort[]];

  (* --- projective-input detection + automatic Cheng-Wu (see the section
         header above dpProjectiveQ); the crawl runs on the gauged
         polynomials over the reduced variable list --- *)
  pre = dpChengWuPreamble[groupPolysIn, allVarsIn, OptionValue["Exponents"],
     OptionValue["ChengWu"], OptionValue["ChengWuVariable"]];
  groupPolys = pre["polys"]; allVars = pre["vars"]; chengWu = pre["chengWu"];

  ng = Length[groupPolys];
  leftoverOf[S_] := Complement[allVars, S];

  (* --- probe + degree-2 branch, mode-dependent --- *)
  If[probe === "KinNumeric",
   (* kinematics -> random integers at nRatPts independent points; per-subset
      locus reconstructed over the integration variables only at each point
      (memoized per (group, sorted-S, point)).  The locus is computed OVER Q
      (dpLocusQ): mod-p factoring false-splits purely-kinematic quadratic
      letters (numeric discriminant a quadratic residue), which over-accepted
      box2mh (18 instead of 0); over Q the integer discriminant decides
      irreducibility deterministically.  The degree is read from point 1; the
      degree-2 rationalizability AND-s the verdict over all nRatPts points
      (kinematics-numeric, no expensive symbolic dpLocus2 -- this keeps the crawl
      fast even when non-rationalizable quadratics appear). *)
   kinematics = Complement[Variables[Flatten[groupPolys]], allVars];
   kinSubstList = Table[SeedRandom[kinSeed + pt];
      Thread[kinematics -> RandomInteger[{10^4, 10^5}, Length[kinematics]]],
      {pt, nRatPts}];
   locusKinPt[g_, Ss_, pt_] := locusKinPt[g, Ss, pt] =
      (nProbe++;
       If[Ss === {}, groupPolys[[g]] /. kinSubstList[[pt]],
          dpLocusQ[(groupPolys[[g]] /. kinSubstList[[pt]]), Ss, allVars]]);
   degOf[g_, S_, v_] :=
      Sort[DeleteCases[dpDegreeInVar[locusKinPt[g, Sort[S], 1], v], 0]];
   rat2[g_, S_, v_] := AllTrue[Range[nRatPts], Function[pt,
      Module[{quad = Select[locusKinPt[g, Sort[S], pt], Exponent[#, v] === 2 &]},
        AllTrue[quad, dpRationalizableQ[#, v, leftoverOf[Append[S, v]]]["ok"] &]]]],
   (* UnivariateV: numericize every parameter but v; coarsest of K samples.
      Degree-2 rationalizability stays kinematics-SYMBOLIC here (raw polys at
      S = {}, dpLocus2 for |S| >= 1); this mode is the strict-no-reconstruction
      record, not the fast path. *)
   degOf[g_, S_, v_] := degOf[g, Sort[S], v] =
      (nProbe++;
       dpDegProbeMultiset[groupPolys[[g]], Sort[S], allVars, v, "Samples" -> k]);
   rat2[g_, S_, v_] := Module[{L, quad},
      L = If[S === {}, groupPolys[[g]], dpLocus2[groupPolys[[g]], S, allVars]];
      quad = Select[L, Exponent[#, v] === 2 &];
      AllTrue[quad, dpRationalizableQ[#, v, leftoverOf[Append[S, v]]]["ok"] &]]
  ];

  (* admissibility of appending v to S: for EVERY group, the locus must be, factor
     by factor, linear-or-lower (deg <= 1) or a rationalizable quadratic (deg 2);
     any deg >= 3 blocks the extension. *)
  admissibleVQ[S_, v_] := AllTrue[Range[ng], Function[g,
     Module[{degs = degOf[g, S, v], mx},
       mx = Max[Append[degs, 0]];
       Which[mx <= 1, True, mx >= 3, False, True, rat2[g, S, v]]]]];

  (* LeafCount-style edge cost (crawl proxy): per group, the number of
     v-dependent letters plus their total v-degree at (S, v). *)
  edgeCost[S_, v_] := Total[Table[
     With[{d = degOf[g, S, v]}, Length[d] + Total[d]], {g, ng}]];

  orders = dpCrawlOrders[allVars, admissibleVQ, edgeCost];

  (* Cheng-Wu gauge retry on NOLR (adversarial F4; see DoppioFubini) *)
  cwRetryHit = If[
     chengWu =!= None && chengWu =!= "Scan" && orders === {} &&
       TrueQ[OptionValue["ChengWuRetry"]],
     Catch[
       Module[{res2},
        Do[
          res2 = DoppioCrawl[groupPolysIn, allVarsIn,
             "Samples" -> k, "Probe" -> probe, "KinSeed" -> kinSeed,
             "RatPoints" -> nRatPts,
             "Exponents" -> OptionValue["Exponents"], "ChengWu" -> Automatic,
             "ChengWuVariable" -> v2, "ChengWuRetry" -> False];
          If[res2["orders"] =!= {},
             Print["[doppio] crawl NOLR at Cheng-Wu gauge ", chengWu[[1, 1]],
                   " -> 1; retry found ", Length[res2["orders"]],
                   " admissible orders at gauge ", v2, " -> 1."];
             Throw[res2, "dpCWRetry"]],
          {v2, DeleteCases[allVarsIn, chengWu[[1, 1]]]}];
        Print["[doppio] crawl NOLR at every Cheng-Wu gauge (all ",
              Length[allVarsIn], " variables tried)."];
        Null],
       "dpCWRetry"],
     Null];
  If[AssociationQ[cwRetryHit], Return[cwRetryHit]];

  <|"orders" -> orders, "variant" -> "Crawl", "probe" -> probe,
    "samples" -> k, "probeCalls" -> nProbe,
    "chengWu" -> chengWu, "vars" -> allVars|>];

(* ======================================================================
   DOPPIO C  --  Lungo-core generator + per-step Euler genuineness filter
   ----------------------------------------------------------------------
   Variant C (user spec, 2026-06-03): run Lungo's cheap symbolic per-step
   reduction (leading coefficients + discriminants + pairwise resultants,
   factored and deduplicated, with the Fubini INTERSECTION over which variable
   was removed last), and AFTER the intersection at every subset, test each
   surviving letter against the GENUINE Euler discriminant of the S-marginal
   and drop the fictitious ones.  Lungo's set is a SUPERSET of the truth, so a
   membership FILTER (cheap chi-drop counts) replaces Doppio B's full
   elimination; the letters stay symbolic over Q(kinematics), so degree and
   rationalizability tests are exact -- none of the numericization hazards of
   the KinNumeric probe.

   The generator mirrors STFubiniLR + STFasterFubini2 (SubTropica.wl:12764,
   13959): trailing coefficients arise automatically as Resultant[f, x_v] via
   the AUGMENTED seed {group polys} U {x_1..x_n} (production-matched input; a
   bare seed is the documented boundary-poly bug).  The genuineness test is the
   chi-drop of 06_chi_drop_nontoric.wl, generalized to need no handcrafted
   rational point (see dpGenuineQ).
   ====================================================================== *)

(* ----------------------------------------------------------------------
   dpLungoStep[polys, v]

     One-variable Lungo reduction step (the STFubiniLR mirror).  For each
     polynomial: its LEADING coefficient in v (the polynomial itself if v-free,
     matching CoefficientList[f,v][[-1]]), plus its discriminant in v when
     v-dependent; for each pair of v-dependent polynomials: their resultant in
     v.  Everything is factored over Q, canonicalized up to nonzero scalar
     (edCanon: primitive over Z, sign-fixed), and numeric factors are dropped.
     Discriminants of v-linear polynomials are the constant 1 and disappear in
     the numeric filter, exactly as in STFubiniLR.
   ---------------------------------------------------------------------- *)
dpLungoStep[polys_List, v_] := Module[{term1, term2, combined, facs},
  term1 = Flatten[Table[
     {Last[CoefficientList[f, v]],
      If[FreeQ[f, v], Nothing,
         Discriminant[f, v, Method -> "Subresultants"]]},
     {f, polys}]];
  term2 = Flatten[Table[
     If[FreeQ[polys[[ii]], v] || FreeQ[polys[[jj]], v], Nothing,
        Resultant[polys[[ii]], polys[[jj]], v, Method -> "Subresultants"]],
     {ii, Length[polys]}, {jj, ii + 1, Length[polys]}]];
  combined = Join[term1, term2];
  facs = Flatten[Table[
     If[poly =!= 0 && ! NumericQ[poly], FactorList[poly][[All, 1]], Nothing],
     {poly, combined}]];
  edCanonSet[facs]];

(* ----------------------------------------------------------------------
   dpLungoCore[groupPolys, allVars, filterFn]

     The Fubini dynamic program over subsets (the STFasterFubini2 table,
     production-matched), parameterized by a per-subset letter filter:

       table[{g, {}}]      = canonical factors of the AUGMENTED seed
                             (group polys plus every boundary polynomial x_j)
       table[{g, Sort[S]}] = filterFn[g, Sort[S],
                                Intersection over v in S of
                                   dpLungoStep[table[{g, S\{v}}], v]]

     The intersection (under the edCanon proportional canonical form) is
     Lungo's defence against order-dependent fictitious factors; filterFn is
     Doppio C's Euler genuineness filter (pass dpKeepAllLetters for the bare
     Lungo-core, used to validate the generator against the real Lungo).

     Subsets are filled by increasing size, so every dependency S\{v} is ready
     when needed.  Combined union-over-groups entries table[Sort[S]] are added
     at the end, in the same shape as DoppioFubini's table.

     Letters here are SYMBOLIC over Q (unlike the mod-p letters of variants
     A/B), so downstream degree / rationalizability tests are exact.
   ---------------------------------------------------------------------- *)
dpKeepAllLetters = Function[{g, S, letters}, letters];

dpLungoCore[groupPolys_List, allVars_List, filterFn_] :=
 Module[{ng = Length[groupPolys], table, cands, inter},
  table = Association[];
  Do[table[{g, {}}] = edCanonSet[Flatten[
       FactorList[#][[All, 1]] & /@
         DeleteDuplicates[Join[groupPolys[[g]], allVars]]]], {g, ng}];
  Do[Do[Do[
      cands = Table[dpLungoStep[table[{g, Sort[DeleteCases[S, v]]}], v], {v, S}];
      inter = Fold[Intersection, First[cands], Rest[cands]];
      table[{g, Sort[S]}] = filterFn[g, Sort[S], inter],
      {g, ng}], {S, sizeClass}],
     {sizeClass, Rest[GatherBy[Subsets[allVars], Length]]}];
  Do[table[Sort[S]] = edCanonSet@Flatten[Table[table[{g, Sort[S]}], {g, ng}]],
     {S, Subsets[allVars]}];
  <|"table" -> table|>];

(* ----------------------------------------------------------------------
   dpChiCount[idealNum, ivars]

     Number of solutions (with multiplicity) of a NUMERIC zero-dimensional
     ideal = number of standard monomials reported by SPQR's
     FindIrreducibleMonomials (the validated Euler-characteristic counter,
     cf. euler_disc_lib.wl and 06_chi_drop_nontoric.wl).  Returns Infinity
     when the ideal is not zero-dimensional (degenerate draw -- caller retries).
   ---------------------------------------------------------------------- *)
dpChiCount[idealNum_List, ivars_List] := Module[{r},
  r = Quiet@FindIrreducibleMonomials[idealNum, ivars,
       "MonomialOrder" -> DegreeReverseLexicographic];
  If[ListQ[r], Length[r], Infinity]];

(* ----------------------------------------------------------------------
   dpGenuineQ[polysOrig, S, allVars, letter]

     Euler genuineness of a candidate letter at the S-marginal: is {letter = 0}
     a component of the GENUINE Euler discriminant of the S-marginal critical
     system of the ORIGINAL integrand?  True = genuine (keep), False =
     fictitious (drop).

   Method: incidence-line chi-drop (the 06_chi_drop_nontoric.wl test, automated
   so that NO handcrafted rational on-letter point is needed).

     1. chi_gen: the solution count of the S-marginal critical system
        (dpBuildIdeal: d log Psi over S, boundaries via the c_j log x_j terms,
        Rabinowitsch x0) at a generic random integer parameter point.
        Memoized per (polys, S); degenerate draws are retried.
     2. Restrict the parameters to a generic rational LINE p0 + dpLineT*d and
        ADJOIN the (squarefree-in-dpLineT) letter lt as a generator.  The
        joint count over {x0} U S U {dpLineT} equals the sum of the marginal
        counts over the deg_t(lt) roots of lt -- all of which lie ON the letter:
          fictitious <=> count == deg_t * chi_gen   (every root is generic)
          genuine    <=> count <  deg_t * chi_gen   (on-letter fibers drop,
                                                     cf. the validated 13->12)
     This works uniformly for letters with NO Q-linear variable (Kallen-type
     quadratics), since msolve counts over the algebraic closure -- the roots
     of lt never need to be rational.

   Error directions: a genuine letter false-reading "no drop" is the dangerous
   one (it would drop a real singularity), so the FICTITIOUS verdict requires
   TWO independent no-drop lines; any valid line showing a drop returns genuine
   immediately; persistent degeneracies KEEP the letter (conservative,
   Lungo-status-quo) with a warning.  The line parameter is the dedicated
   symbol dpLineT, never a kinematic name (t is a Mandelstam).
   ---------------------------------------------------------------------- *)
dp$chiGenCache = <||>;

(* ----------------------------------------------------------------------
   DiscKosky backend (DEFAULT)
   --------------------------
   dpDKOK[]            -- is the DiscKosky paclet loaded?
   dpDKTotal[G,S,c,sd] -- seeded, Quiet-ed wrapper around
                          CountSectorsUnregulated[G, S, {}, "Constraint" -> c]:
                          the total unregulated-sector count of the
                          Lee-Pomeransky polynomial G with the integrated
                          subset S as propagator variables (leftover x's and
                          kinematics are auto-numericized inside DiscKosky).
                          Returns the integer total or $Failed.  Seeding via
                          BlockRandom makes the stochastic prime/point draws
                          reproducible.
   dpGenuineDKQ        -- genuine(letter at S) <=> total("Constraint"->letter)
                          drops below the generic total.  Validated on the spin
                          fixtures: DiscKosky's totals reproduce the PLD-
                          database chi values exactly (par_generic_zero 13,
                          D3/D6 -> 12; massless box 3, s/t/s+t -> 1/1/2).
                          chi_gen is the MAX of two seeded draws (an unlucky
                          prime/point can only undercount); the destructive
                          verdict (fictitious -> drop the letter) requires TWO
                          independent no-drop draws; failures keep the letter
                          (conservative).  Multi-polynomial groups enter via
                          their product (single-poly groups are the current
                          fixtures; revisit for per-face multi-poly groups).
   ---------------------------------------------------------------------- *)
(* the counting core is now self-contained (the adapted copy below), so the
   backend is always available; the paclet load above is kept only so the
   original CountSectorsUnregulated stays callable for cross-checks *)
dpDKOK[] := (DownValues[dpCountSectorsF] =!= {});

dp$dkChiGenCache = <||>;
dp$dkWarned = False;

(* ======================================================================
   ADAPTED DISCKOSKY COUNTING CORE
   ----------------------------------------------------------------------
   Copied from DiscKosky 0.0.1, Kernel/irreducible_monomials.m and
   Kernel/euler_characteristic.m (Crisanti, Lippstreu, McLeod, Polackova,
   2026), per user instruction (2026-06-03 late), with these MODIFICATIONS:

   1. FACTORED INPUT + CLEARED DLOG NUMERATORS.  The original countInSector
      differentiates the RAW input polynomial, so twist exponents become
      polynomial POWERS and the cost explodes with the exponent values even
      though the critical variety is dlog-determined and exponent-value-
      independent (G^17 of a 16-term cubic hung GroebnerBasis for ~17 min).
      The adapted dpCountInSectorF takes the FACTOR list {P_1, ..., P_k} with
      exponents {e_1, ..., e_k} and builds the cleared dlog system
          N_v = Sum_i e_i (prod_{j != i} P_j) D[P_i, v]   for each v,
          Rabinowitsch 1 - z prod_i P_i        (exponent 1: only the support
                                                of the divisor matters),
      so the exponents enter as COEFFICIENTS only -- their values are free
      and LARGE random primes restore full twist genericity at zero cost.
      CI = group factors; CII = group factors + coordinate-hyperplane factors
      v (exponent q_v) -- which auto-zero every sub-sector, so one sector
      loop serves both variants.
   2. dpFindIrredMonos is the original findIrreducibleMonomials (numeric
      GroebnerBasis mod a 64-bit prime + staircase walk) with the prime list
      GENERATED (descending primes below 2^63, same leading entries) and an
      added guard for the no-pure-power positive-dimension corner the
      original walk would not terminate on.
   3. The Constraint mechanics are copied: linear constraints solved exactly
      and substituted into the factors; nonlinear constraints via Diophantine
      FindInstance mod a prime MATCHED to the counting prime ("PrimeIndex").
   ====================================================================== *)

(* descending 63-bit primes (the original hardcodes FiniteFlow's list; the
   leading entries coincide); 201 entries to match the Diophantine indexing.
   NextPrime[n, -1] is the documented previous-prime form (there is NO
   PreviousPrime in Mathematica). *)
dp$dk64Primes = NestList[NextPrime[#, -1] &, NextPrime[2^63, -1], 200];

dpDividesQ[e_, m_] := And @@ Thread[m >= e];

Options[dpFindIrredMonos] = {"MonomialOrder" -> DegreeReverseLexicographic,
   "Sort" -> True, "PrimeIndex" -> Random};
dpFindIrredMonos[polySystem_List, vars1_List, OptionsPattern[]] :=
 Module[{ord, params, paramsNsub, gb, lt, n, pure, pureSums, bounds, todo,
    seen = <||>, res = {}, v, vv, i, prime, vars},
  ord = OptionValue["MonomialOrder"];
  prime = If[OptionValue["PrimeIndex"] === Random,
     RandomSample[dp$dk64Primes, 1][[1]],
     dp$dk64Primes[[1 + OptionValue["PrimeIndex"]]]];
  params = Complement[Variables[polySystem], vars1];
  paramsNsub = Thread[params ->
     RandomInteger[{1, dp$dk64Primes[[-1]]}, Length[params]]];
  vars = If[OptionValue["Sort"],
     Last[GroebnerBasis`DistributedTermsList[polySystem /. paramsNsub, vars1,
        MonomialOrder -> ord, CoefficientDomain -> RationalFunctions,
        Modulus -> prime, Sort -> True]],
     vars1];
  gb = GroebnerBasis[polySystem /. paramsNsub, vars, MonomialOrder -> ord,
     CoefficientDomain -> RationalFunctions, Modulus -> prime];
  lt = Map[Exponent[#, vars] &, MonomialList[gb, vars, ord][[All, 1]]];
  n = Length[vars];
  pure = Table[Select[lt, #[[i]] > 0 && Total[Drop[#, {i}]] == 0 &], {i, n}];
  (* positive-dimension checks: a variable with NO pure power in the
     leading-term ideal means an infinite staircase.  (The added pureSums
     === {} clause covers the corner with no pure powers at all, where the
     original bounds would be Infinity and the walk would not terminate;
     GB == {1} is excluded because its origin leading term divides
     everything.) *)
  pureSums = Select[lt, Length[DeleteCases[#, 0]] == 1 &];
  If[(pureSums === {} && ! MemberQ[lt, ConstantArray[0, n]]) ||
     (pureSums =!= {} && MemberQ[Total[pureSums], 0]),
     Return[Infinity]];
  bounds = (Min /@ MapThread[#1[[All, #2]] &, {pure, Range[n]}]) - 1;
  todo = {ConstantArray[0, n]};
  While[todo =!= {},
    v = First[todo]; todo = Rest[todo];
    If[! KeyExistsQ[seen, v],
       seen[v] = True;
       If[NoneTrue[lt, dpDividesQ[#, v] &],
          AppendTo[res, Times @@ (vars^v)];
          Do[If[v[[i]] < bounds[[i]],
              vv = ReplacePart[v, i -> v[[i]] + 1];
              todo = Append[todo, vv]], {i, n}]]]];
  DeleteCases[MonomialList[Plus @@ res, vars, ord], 0]];

(* ----------------------------------------------------------------------
   dpCountInSectorF[facs, exps, propVars, opts]  --  adapted countInSector

     Solution count (standard monomials) of the cleared-dlog critical system
     of the twist  prod_i facs[[i]]^exps[[i]]  in propVars, on the complement
     of {prod facs = 0}, with every non-propagator symbol numericized to a
     random prime.  "Constraint" -> c restricts to a generic point of {c = 0}
     (linear: solved exactly into the factors; nonlinear: Diophantine
     FindInstance mod the matched counting prime, the original mechanism).
     Returns the count, Indeterminate (positive-dimensional), or $Failed.
   ---------------------------------------------------------------------- *)
dpClearedNums[fl_List, el_List, pv_List] := Table[
   Sum[el[[i]] (Times @@ Delete[fl, i]) D[fl[[i]], v], {i, Length[fl]}],
   {v, pv}];

Options[dpCountInSectorF] = {"MonomialOrder" -> DegreeReverseLexicographic,
   "Sort" -> True, "Constraint" -> 0, "Diophantine" -> True};
dpCountInSectorF[facsIn_List, exps_List, propVars_List,
   opts : OptionsPattern[]] :=
 Module[{facs, kinPoly, kinPolyVars, lowestPowerCoeff, mandelstamVar,
    exponent, solvedConstraint, params, paramsNsub, numerators, denominator,
    kinPolyN, system, systemVariables, monomials, indexList, inst,
    primeIndexAndSub, newvar},
  kinPoly = Numerator[Together[OptionValue["Constraint"]]];
  kinPolyVars = Variables[kinPoly];
  If[Length[kinPolyVars] != 0,
     lowestPowerCoeff = First[PositionSmallest[Map[Max,
        Transpose[Keys[CoefficientRules[kinPoly]]]]]];
     mandelstamVar = kinPolyVars[[lowestPowerCoeff]];
     exponent = Exponent[kinPoly, mandelstamVar],
     mandelstamVar = {}; exponent = 1];

  If[exponent == 1 && mandelstamVar =!= {},
     (* linear constraint: solve exactly and substitute into the FACTORS *)
     solvedConstraint = Flatten[Solve[kinPoly == 0, mandelstamVar]];
     facs = facsIn /. solvedConstraint;
     params = Complement[Variables[facs], propVars];
     paramsNsub = Thread[params -> Map[Prime,
        RandomInteger[{1, 10^8 + Length[params]}, Length[params]]]];
     facs = facs /. paramsNsub;
     numerators = If[Length[propVars] === 0, {},
        dpClearedNums[facs, exps, propVars]];
     denominator = Times @@ facs;
     system = DeleteCases[Join[numerators, {1 - newvar denominator}], 0];
     systemVariables = Flatten[{newvar, propVars}];
     monomials = dpFindIrredMonos[system, systemVariables,
        Sequence @@ FilterRules[{opts}, Options[dpFindIrredMonos]]];
     Return[If[monomials === Infinity, Indeterminate, Length[monomials]]]];

  facs = facsIn;
  params = DeleteCases[Complement[
     DeleteDuplicates[Join[Variables[facs], kinPolyVars]], propVars],
     mandelstamVar];
  paramsNsub = Thread[params -> Map[Prime,
     RandomInteger[{1, 10^8 + Length[params]}, Length[params]]]];
  facs = facs /. paramsNsub;
  numerators = If[Length[propVars] === 0, {},
     dpClearedNums[facs, exps, propVars]];
  denominator = Times @@ facs;
  kinPolyN = kinPoly /. paramsNsub;
  If[OptionValue["Diophantine"] && mandelstamVar =!= {},
     (* nonlinear constraint: find a root mod a prime and count mod the SAME
        prime (the original Diophantine mechanism) *)
     indexList = RandomSample[Range[201] - 1];
     primeIndexAndSub = DeleteDuplicates[Table[
        inst = FindInstance[kinPolyN == 0, {mandelstamVar},
           Modulus -> dp$dk64Primes[[pInd + 1]]];
        If[Length[inst] > 0, Return[{pInd, Flatten[inst]}, Table]],
        {pInd, indexList}]];
     If[primeIndexAndSub === {Null},
        Print["[doppio] dpCountInSectorF: Diophantine instance not found ",
              "for constraint ", kinPoly]; Return[$Failed]];
     system = DeleteCases[Join[numerators, {1 - newvar denominator}], 0] /.
        primeIndexAndSub[[2]];
     systemVariables = Flatten[{newvar, propVars}];
     monomials = dpFindIrredMonos[system, systemVariables,
        "PrimeIndex" -> primeIndexAndSub[[1]],
        Sequence @@ FilterRules[FilterRules[{opts}, Options[dpFindIrredMonos]],
           Except["PrimeIndex"]]];
     If[monomials === Infinity, Indeterminate, Length[monomials]],
     (* generic point (Constraint -> 0), or nonlinear with Diophantine off:
        keep the constraint variable symbolic and adjoin the constraint *)
     system = DeleteCases[Join[{kinPolyN}, numerators,
        {1 - newvar denominator}], 0];
     systemVariables = Flatten[{mandelstamVar, newvar, propVars}];
     monomials = dpFindIrredMonos[system, systemVariables,
        Sequence @@ FilterRules[{opts}, Options[dpFindIrredMonos]]];
     If[monomials === Infinity, Indeterminate, Length[monomials]/exponent]]];

(* ----------------------------------------------------------------------
   dpCountSectorsF[facs, exps, propVars, opts]  --  adapted
   CountSectorsUnregulated: sector sum over subsets of propVars, each sector
   restricting the FACTOR list (complement variables -> 0; a sector on which
   any factor vanishes identically contributes 0 -- this is also what makes
   CII's hyperplane factors fold everything into the top sector).  Returns
   {total, perSector, sectors}; Indeterminate/$Failed propagate into total.
   ---------------------------------------------------------------------- *)
Options[dpCountSectorsF] = Options[dpCountInSectorF];
dpCountSectorsF[facsIn_List, exps_List, propVars_List,
   opts : OptionsPattern[]] :=
 Module[{sectors, secCounts},
  sectors = Subsets[propVars];
  secCounts = Table[
     Module[{facsSec = Expand[facsIn /.
          Thread[Complement[propVars, sec] -> 0]]},
       If[MemberQ[facsSec, 0],
          0,
          dpCountInSectorF[facsSec, exps, sec, opts]]],
     {sec, sectors}];
  {Plus @@ secCounts, secCounts, sectors}];

(* ----------------------------------------------------------------------
   dpDKTwist[polys, S, hyper, seed]

     The factored twist for CI/CII: facs = group polys (CI) or group polys +
     the coordinate hyperplanes of S (CII); exps = LARGE distinct random
     primes (their values are free in the cleared-dlog representation, so
     full genericity costs nothing -- the user's dlog observation).
   ---------------------------------------------------------------------- *)
dpDKTwist[polys_List, S_List, hyper : True | False, seed_Integer] :=
 Module[{facs, exps},
  facs = Join[polys, If[hyper, S, {}]];
  BlockRandom[SeedRandom[seed];
    exps = Prime /@ RandomSample[Range[30, 230], Length[facs]]];
  {facs, exps}];

(* dpDKTotal: seeded, Quiet-ed total of the adapted sector count.
   Indeterminate (a positive-dimensional sector) is passed through AS
   Indeterminate -- the caller applies the user rule "Indeterminate counts
   as an Euler drop". *)
dpDKTotal[facs_List, exps_List, pv_List, constraint_, seed_Integer] :=
 Module[{r},
  BlockRandom[
    SeedRandom[seed];
    r = Quiet[dpCountSectorsF[facs, exps, pv, "Constraint" -> constraint]]];
  Which[
    ListQ[r] && NumericQ[First[r]], First[r],
    ListQ[r] && ! FreeQ[First[r], Indeterminate], Indeterminate,
    True, $Failed]];

(* dpHomogeneousQ: is p homogeneous as a polynomial in vars (coefficients in
   everything else)?  Used by the Euler-relation guard below. *)
dpHomogeneousQ[p_, vars_List] :=
  Length[DeleteDuplicates[Total /@ Keys[CoefficientRules[p, vars]]]] <= 1;

Options[dpGenuineDKQ] = {"Seed" -> 87178, "Hyperplanes" -> True};
dpGenuineDKQ[polysOrig_List, S_List, allVars_List, letter_, OptionsPattern[]] :=
 Module[{hyper = OptionValue["Hyperplanes"], seedTw, seedDraw, facs, exps,
         pv, key, chiGen, onTot, onTot2},
  (* the twist exponents must be LETTER-INDEPENDENT so the cached generic
     count and every constrained count refer to the same master function *)
  seedTw = OptionValue["Seed"] + Mod[Hash[{polysOrig, Sort[S], hyper}], 10^6];
  seedDraw = seedTw + Mod[Hash[letter], 10^6];
  {facs, exps} = dpDKTwist[polysOrig, Sort[S], hyper, seedTw];
  pv = Sort[S];
  (* EULER-RELATION GUARD: when EVERY factor is homogeneous in the propagator
     variables the twist has no torus critical points at all
     (Sum_v x_v dlog_v = Sum_i e_i deg P_i != 0), so every count is
     identically 0 and carries no signal.  Happens exactly for the raw
     homogeneous Symanzik data at the TERMINAL subset (at intermediate S the
     numericized leftover variables already break the grading) -- the case
     the inhomogeneous Lee-Pomeransky SUM was invented for.  Principled fix:
     the C*-quotient chart (gauge-fix the last propagator variable to 1 and
     drop it; the standard FMT/PLD chart). *)
  If[AllTrue[facs, dpHomogeneousQ[#, pv] &] && Length[pv] >= 2,
     facs = facs /. Last[pv] -> 1;
     pv = Most[pv]];
  key = {polysOrig, Sort[S], hyper};
  chiGen = Lookup[dp$dkChiGenCache, Key[key],
     Module[{a, b},
       a = dpDKTotal[facs, exps, pv, 0, seedTw + 1];
       b = dpDKTotal[facs, exps, pv, 0, seedTw + 2];
       dp$dkChiGenCache[key] = Which[
          NumericQ[a] && NumericQ[b], Max[a, b],
          NumericQ[a], a,
          NumericQ[b], b,
          True, $Failed]]];
  If[! NumericQ[chiGen],
     Print["[doppio] WARNING: DiscKosky chi_generic unusable at S=", S,
           " (Hyperplanes=", hyper, "); keeping letter (conservative)."];
     Return[True]];
  onTot = dpDKTotal[facs, exps, pv, letter, seedDraw + 3];
  Which[
    (* USER RULE: an Indeterminate constrained count counts as an Euler drop
       -- the singularity is conservatively considered included (expected in
       CI when the constraint kills a variable's equation; CII's hyperplane
       factors usually prevent it, but the If stays for both). *)
    onTot === Indeterminate, True,
    onTot === $Failed,
      Print["[doppio] WARNING: DiscKosky constrained count failed at S=", S,
            ", letter=", letter, "; keeping (conservative)."]; True,
    onTot < chiGen, True,                       (* drop seen: genuine *)
    True,                                        (* no drop: confirm the drop-less
                                                    reading before dropping *)
      onTot2 = dpDKTotal[facs, exps, pv, letter, seedDraw + 4];
      Which[
        onTot2 === Indeterminate, True,
        onTot2 === $Failed, True,
        True, onTot2 < chiGen]]];

Options[dpGenuineQ] = {"Seed" -> 424242, "MaxRetries" -> 5,
   "ChiBackend" -> "DiscKosky", "Hyperplanes" -> True};
dpGenuineQ[polysOrig_List, S_List, allVars_List, letter_, OptionsPattern[]] :=
 Module[{seed = OptionValue["Seed"], maxR = OptionValue["MaxRetries"],
         backend = OptionValue["ChiBackend"],
         built, ideal, idealVars, params, key, chiGen, lineVerdict,
         nFalse = 0, k = 0, r},
  (* --- backend dispatch: DiscKosky is the default; the incidence-line
         implementation below remains as "ChiBackend" -> "IncidenceLine" and
         as the automatic fallback when the paclet is absent.  "Hyperplanes"
         selects the CII (True, coordinate-hyperplane factors) vs CI (False)
         master-function form; the incidence-line backend is intrinsically
         CII-like (its dpBuildIdeal carries the c_j log x_j twists) and
         ignores the option. --- *)
  If[backend === "DiscKosky",
     If[dpDKOK[],
        Return[dpGenuineDKQ[polysOrig, S, allVars, letter,
           "Hyperplanes" -> OptionValue["Hyperplanes"]]],
        If[! TrueQ[dp$dkWarned],
           dp$dkWarned = True;
           Print["[doppio] WARNING: DiscKosky paclet not loaded; ",
                 "falling back to the incidence-line chi backend."]]]];
  built = dpBuildIdeal[polysOrig, S, allVars];
  ideal = built["ideal"]; idealVars = built["idealVars"];
  params = Sort[Union[built["params"], Variables[letter]]];

  (* --- chi at a generic numeric parameter point (memoized per marginal) --- *)
  key = {polysOrig, Sort[S], allVars};
  chiGen = Lookup[dp$chiGenCache, Key[key],
     Module[{c = Infinity, kk = 0, pt},
       While[c === Infinity && kk < maxR,
         kk++;
         SeedRandom[seed + 17 kk];
         pt = Thread[params -> RandomInteger[{10^3, 10^4}, Length[params]]];
         c = dpChiCount[ideal /. pt, idealVars]];
       dp$chiGenCache[key] = c; c]];
  If[chiGen === Infinity,
     Print["[doppio] WARNING: dpGenuineQ chi_generic degenerate at S=", S,
           "; keeping letter (conservative)."];
     Return[True]];

  (* --- one incidence-line verdict: True = drop seen (genuine), False = no
         drop, $Failed = degenerate draw (retry) --- *)
  lineVerdict[j_] := Module[{p0, dd, line, lt, ltSf, degt, chiInc},
     SeedRandom[seed + 1000 j + Mod[Hash[letter], 10^6]];
     p0 = RandomInteger[{10^3, 10^4}, Length[params]];
     dd = RandomInteger[{10, 10^3}, Length[params]];
     line = Thread[params -> (p0 + dpLineT dd)];
     lt = Expand[letter /. line];
     ltSf = Times @@ (FactorSquareFreeList[lt][[All, 1]]);
     degt = Exponent[ltSf, dpLineT];
     If[degt === 0, Return[$Failed, Module]];
     chiInc = dpChiCount[Append[ideal /. line, ltSf], Append[idealVars, dpLineT]];
     If[chiInc === Infinity || chiInc > degt chiGen, Return[$Failed, Module]];
     chiInc < degt chiGen];

  While[k < maxR,
    k++;
    r = lineVerdict[k];
    Which[
      r === True, Return[True, Module],
      r === False, nFalse++; If[nFalse >= 2, Return[False, Module]]]];
  If[nFalse >= 1, Return[False, Module]];   (* one clean no-drop, rest degenerate *)
  Print["[doppio] WARNING: dpGenuineQ no valid line at S=", S,
        ", letter=", letter, "; keeping (conservative)."];
  True];

(* ----------------------------------------------------------------------
   dpCrawlOrders[allVars, admissibleVQ, edgeCost]

     Depth-first enumeration of every fully admissible integration order, ranked
     by cumulative EDGE cost.  Identical in shape to dpAdmissibleOrders, except
     the cost accumulates per appended-variable edge (edgeCost[S, v]) rather than
     per reached subset, matching the crawl's edge-local probe data.  Inadmissible
     candidates prune their whole subtree.
   ---------------------------------------------------------------------- *)
dpCrawlOrders[allVars_, admissibleVQ_, edgeCost_] :=
 Module[{results = {}, extend},
  extend[S_, ord_, sc_] := Module[{cands},
    If[Length[S] === Length[allVars],
       AppendTo[results, {ord, sc}]; Return[Null]];
    cands = Select[Complement[allVars, S], admissibleVQ[S, #] &];
    Do[extend[Append[S, v], Append[ord, v], sc + edgeCost[S, v]],
       {v, cands}]];
  extend[{}, {}, 0];
  SortBy[results, Last]];
