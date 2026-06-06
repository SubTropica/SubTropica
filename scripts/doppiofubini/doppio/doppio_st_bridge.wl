(* doppio_st_bridge.wl
   -------------------------------------------------------------------------
   Task 8 bridge: make the Doppio variant-C engine callable as a per-face
   "MethodLR" backend inside a SUBTROPICA kernel (no SPQR / FiniteFlow32).

   Loading this file Gets doppio_lib.wl (whose SPQR/DiscKosky loads are
   graceful; the variant-C path -- Lungo-core generator + per-subset Euler
   chi-drop filter on the self-contained cleared-dlog counters -- is pure
   Mathematica) and defines

     STFubiniDoppio2[groupPoly, xvars, opts]

   with EXACTLY the per-face engine contract of stDispatchFubini2 /
   STEspressoFubini2 / STFubiniAT2 (SubTropica.wl ~13686):

     FindRoots -> False   :  {order, score}        or {NOLR, Infinity}
     FindRoots truthy     :  {{order, score}, rootPolys}
                             or {{NOLR, Infinity}, {}}

   The caller (the per-face LR scoring loop, SubTropica.wl ~15240) hands the
   AUGMENTED groups  Join[#, xvars] & /@ polysAndPairs[[;;, 1]]  -- exactly
   the production-matched input dpLungoCore expects.

   Mapping:
     FindRoots -> False   ->  "KeepRule" -> "Strict"
     FindRoots truthy     ->  "KeepRule" -> "FindRoots" (relaxed tiers:
                              terminal quadratics + carried sqrt
                              obligations; cf. doppio_lib.wl)
   The returned order is the SCORE-MINIMAL admissible order (the
   collaborator-confidence end of the FindRoots spectrum; deep-carry orders
   are returned only when nothing shallower exists).

   rootPolys: the quadratic letters along the chosen order that need formal
   roots (the production consumer is STApplyRootFactoring + HyperInt's
   Wm/Wp letters) -- recovered by walking the chosen order through the
   chi-filtered table and collecting, at every step, the deg-2-in-v letters
   that are not Euler-rationalizable: exactly the letters whose roots the
   FindRoots semantics takes.  Under FindRoots -> False the strict rule
   admitted every step without formal roots, so rootPolys is empty by
   construction and the flat shape is returned.

   The "Heuristic" option is accepted for call-site parity and ignored (the
   Doppio score is the per-subset LeafCount sum; cf. DoppioFubini).

   This file deliberately does NOT modify SubTropica.wl.  The 6-line
   dispatch patch that routes "MethodLR" -> "Doppio" to this engine lives
   in task8_methodlr_doppio.patch next to this file, awaiting user
   application (SubTropica.wl carries unrelated uncommitted changes).
   ------------------------------------------------------------------------- *)

Get["/Users/smizera/Projects/SubTropica-branchSM/scripts/doppiofubini/doppio/doppio_lib.wl"];

(* ----------------------------------------------------------------------
   dpRootPolysForOrder[table, ng, order, allVars]

     The formal-root letters of a chosen admissible order: walk the order;
     at each step (S = prefix, v) collect, over all groups, the letters of
     the chi-filtered table at S that are quadratic in v and NOT
     Euler-rationalizable (dpRationalizableQ) -- the letters whose roots
     the FindRoots semantics introduces (terminal quadratics included:
     their roots are the kinematic algebraic letters of the answer).
     Deduplicated by edCanon class.  An order admitted by the STRICT rule
     returns {} by construction.
   ---------------------------------------------------------------------- *)
dpRootPolysForOrder[table_Association, ng_Integer, order_List, allVars_List] :=
 Module[{roots = {}, S = {}, lo},
  Do[
    lo = Complement[allVars, Append[S, v]];
    Do[
      Do[If[Exponent[p, v] === 2 && ! dpRationalizableQ[p, v, lo]["ok"],
            AppendTo[roots, p]],
         {p, table[{g, Sort[S]}]}],
      {g, ng}];
    S = Append[S, v],
    {v, order}];
  edCanonSet[roots]];

(* ----------------------------------------------------------------------
   STFubiniDoppio2[groupPoly, xvars, opts]  --  the per-face engine
   ---------------------------------------------------------------------- *)
Options[STFubiniDoppio2] = {FindRoots -> False, Heuristic -> 1,
   "Exponents" -> None};

STFubiniDoppio2[groupPoly_List, xvars_List, opts : OptionsPattern[]] :=
 Module[{frEff, res, best, rootPolys},
  frEff = (OptionValue[FindRoots] =!= False);   (* Automatic counts as True *)
  res = DoppioFubini[groupPoly, xvars, "Doppio" -> "C",
     "KeepRule" -> If[frEff, "FindRoots", "Strict"],
     "Exponents" -> OptionValue["Exponents"]];
  If[res["orders"] === {},
     Return[If[frEff, {{NOLR, Infinity}, {}}, {NOLR, Infinity}]]];
  best = First[res["orders"]];   (* score-minimal *)
  If[! frEff,
     {best[[1]], best[[2]]},
     {{best[[1]], best[[2]]},
      dpRootPolysForOrder[res["table"], Length[groupPoly], best[[1]],
         res["vars"]]}]];
