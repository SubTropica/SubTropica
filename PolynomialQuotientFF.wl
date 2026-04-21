(* ::Package:: *)

(* ::Package:: *)
(**)


(*Quit[]*)


(*
   SPQRPolynomialQuotient
   
   A drop-in replacement for Mathematica's PolynomialQuotient[] built on top
   of the SPQR package (arXiv:2511.14875).
   
   Core idea (from the paper, Sections 2.1.1\[Dash]2.1.3 and 2.5):
   Given  f(x) = q(x) p(x) + r(x)  with deg(r) < deg(p), SPQR computes
   r(x) = f(x) mod p(x)  via Macaulay-matrix row reduction over finite fields.
   The quotient is then recovered algebraically:
   
       q(x) = ( f(x) - r(x) ) / p(x)
   
   Because r(x) is exact (rational coefficients reconstructed from finite
   fields), this final division is exact; no expression swell.
   
   Two methods are provided:
   
   Method "Direct" (default):
     Uses BuildPolynomialSystem / ReconstructPolynomialRemainder.
   
   Method "CompanionMatrix":
     Uses BuildCompanionMatrices / BuildTargetCompanionMatrices /
     ReconstructTargetCompanionMatrices. Better for batch divisions.
   
   Prerequisites:  SPQR and FiniteFlow must be installed.
 *)

(* FiniteFlow and SPQR must already be loaded before this file is Get[]-ed.
   ConfigureSubTropica[] in SubTropica.wl handles that automatically.
   Do not load this file directly. *)
If[!MemberQ[$Packages, "FiniteFlow`"] || !MemberQ[$Packages, "SPQR`"],
  Print["ERROR: PolynomialQuotientFF.wl requires FiniteFlow` and SPQR` to be loaded first."];
  Print["Use ConfigureSubTropica[FiniteFlowPath -> \"...\", SPQRPath -> \"...\"] to load them."];
  Abort[]
];


(* 
   SPQRPolynomialQuotientRemainder
   
   Returns {quotient, remainder} for univariate polynomial division
   f(x) / p(x), computed through SPQR's finite-field pipeline.
   
   Arguments:
     f        ; the dividend polynomial
     p        ; the divisor polynomial  (nonzero, deg >= 1)
     x        ; the polynomial variable
   
   Options:
     "Method"         -> "Direct" | "CompanionMatrix"
     "MaxWeight"      -> {wmin, wmax} for the Macaulay system (default {1,20})
     "MonomialOrder"  -> ordering passed to SPQR (default Lexicographic)
     "PrintDebugInfo" -> 0, 1, or 2
   *)

Options[SPQRPolynomialQuotientRemainder] = {
  "Method"         -> "Direct",
  "MaxWeight"      -> {1, (*20*)100},
  "MonomialOrder"  -> Lexicographic,
  "PrintDebugInfo" -> 0
};

SPQRPolynomialQuotientRemainder[f_, p_, x_, OptionsPattern[]] := Module[
  {
    degF, degP, vars, remainder, quotient,
    irreds, system, result, cmats, fcmats,
    method, maxW, monOrd, dbg
  },
  
  (* \[HorizontalLine]\[HorizontalLine] read options \[HorizontalLine]\[HorizontalLine] *)
  method = OptionValue["Method"];
  maxW   = OptionValue["MaxWeight"];
  monOrd = OptionValue["MonomialOrder"];
  dbg    = OptionValue["PrintDebugInfo"];
  
  (* basic validation *)
  degF = Exponent[f, x];
  degP = Exponent[p, x];
  
  If[p === 0 || degP === -Infinity,
    Message[SPQRPolynomialQuotientRemainder::divzero]; Return[$Failed]
  ];
  
  (* trivial case: deg(f) < deg(p) *)
  If[degF < degP,
    Return[{0, f}]
  ];
  
  vars = {x};
  
  If[method === "Direct",
    
    (* For a univariate degree-d divisor, the irreducible monomials
       are always {x^(d-1), ..., x, 1}; no Groebner basis needed. *)
    irreds = Table[x^k, {k, degP - 1, 0, -1}];
    
    (* Build the Macaulay system for reducing f mod <p> *)
    system = BuildPolynomialSystem[
      {f}, {p}, vars, maxW,
      "IrreducibleMonomials" -> irreds,
      "MonomialOrder"        -> monOrd,
      "PrintDebugInfo"       -> dbg
    ];
    
    If[Head[system] === $Failed,
      Print["SPQRPolynomialQuotientRemainder: BuildPolynomialSystem failed. ",
            "Try increasing \"MaxWeight\"."];
      Return[$Failed]
    ];
    
    (* Reconstruct the remainder r(x) = f(x) mod p(x) *)
    result = ReconstructPolynomialRemainder[system,
      "PrintDebugInfo" -> dbg
    ];
    remainder = result // First;
    
    (* Recover the quotient: q = (f - r) / p.
       This is an exact polynomial division; no swell, because
       both f and r are already in simplified form. *)
    quotient = Cancel[(f - remainder) / p] // Expand;
    
    Return[{quotient, remainder}]
  ];
  
  
  If[method === "CompanionMatrix",
    
    irreds = Table[x^k, {k, degP - 1, 0, -1}];
    
    (* Build companion matrices for <p(x)> *)
    cmats = BuildCompanionMatrices[
      {p}, vars, maxW, irreds,
      "MonomialOrder"  -> monOrd,
      "PrintDebugInfo" -> dbg
    ];
    
    If[Head[cmats] === $Failed,
      Print["SPQRPolynomialQuotientRemainder: BuildCompanionMatrices failed."];
      Return[$Failed]
    ];
    
    (* Build & reconstruct target companion matrices for f *)
    fcmats = BuildTargetCompanionMatrices[{f}, cmats];
    result = ReconstructTargetCompanionMatrices[fcmats,
      "PrintDebugInfo" -> dbg
    ];
    remainder = result // First;
    
    quotient = Cancel[(f - remainder) / p] // Expand;
    
    Return[{quotient, remainder}]
  ];
  
  (* fallback *)
  Print["SPQRPolynomialQuotientRemainder: Unknown method \"", method, "\"."];
  Return[$Failed]
];

SPQRPolynomialQuotientRemainder::divzero = "Division by zero polynomial.";


(* 
   SPQRPolynomialQuotient ; returns only the quotient
   (drop-in replacement for PolynomialQuotient)
    *)

Options[SPQRPolynomialQuotient] = Options[SPQRPolynomialQuotientRemainder];

SPQRPolynomialQuotient[f_, p_, x_, opts : OptionsPattern[]] :=
  SPQRPolynomialQuotientRemainder[f, p, x, opts] // First;


(* 
   SPQRPolynomialRemainder ; returns only the remainder
    *)

Options[SPQRPolynomialRemainder] = Options[SPQRPolynomialQuotientRemainder];

SPQRPolynomialRemainder[f_, p_, x_, opts : OptionsPattern[]] :=
  SPQRPolynomialQuotientRemainder[f, p, x, opts] // Last;


(* 
   SPQRPolynomialQuotientRemainderBatch
   
   Divide MULTIPLE polynomials {f1, f2, ...} by the SAME divisor p(x).
   
   Uses the CompanionMatrix method internally: the companion matrices
   for <p> are built once and reused for all targets.
    *)

Options[SPQRPolynomialQuotientRemainderBatch] = {
  "MaxWeight"      -> {1, 20},
  "MonomialOrder"  -> Lexicographic,
  "PrintDebugInfo" -> 0
};

SPQRPolynomialQuotientRemainderBatch[fList_List, p_, x_, OptionsPattern[]] := Module[
  {
    degP, vars, irreds, cmats, fcmats, remainders, quotients,
    maxW, monOrd, dbg
  },
  
  maxW   = OptionValue["MaxWeight"];
  monOrd = OptionValue["MonomialOrder"];
  dbg    = OptionValue["PrintDebugInfo"];
  
  degP = Exponent[p, x];
  If[p === 0 || degP === -Infinity,
    Print["SPQRPolynomialQuotientRemainderBatch: Division by zero polynomial."];
    Return[$Failed]
  ];
  
  vars   = {x};
  irreds = Table[x^k, {k, degP - 1, 0, -1}];
  
  (* Build companion matrices ONCE for <p(x)> *)
  cmats = BuildCompanionMatrices[
    {p}, vars, maxW, irreds,
    "MonomialOrder"  -> monOrd,
    "PrintDebugInfo" -> dbg
  ];
  
  If[Head[cmats] === $Failed, Return[$Failed]];
  
  (* Build target companion matrices for ALL targets at once *)
  fcmats     = BuildTargetCompanionMatrices[fList, cmats];
  remainders = ReconstructTargetCompanionMatrices[fcmats,
    "DeleteGraph"    -> True,
    "PrintDebugInfo" -> dbg
  ];
  
  (* Recover each quotient *)
  quotients = Table[
    Cancel[(fList[[i]] - remainders[[i]]) / p] // Expand,
    {i, 1, Length[fList]}
  ];
  
  Return[Transpose[{quotients, remainders}]]
];



(*f2 = ((a^5 + b^3*c^2 - 7*a*b + 1)*x^8 + (a*b*c - 3)*x^4 + a^2 - b)^4;
p2 = ((a^2 - b*c)*x^3 + (a + b)*x - c)^3;   
SPQRPolynomialQuotient[f2, p2, x]//EchoTiming
PolynomialQuotient[f2, p2, x]//EchoTiming
%-%%//Factor*)


(*f2 = ((a^7 + (a+x c) x^5 + b^3*c^2 - 7*a*b + 1)*x^8 + (a*b*c - 3)*x^4 + a^2 - b)^4;
p2 = ((a^2 - b*c)*x^3 + (a + b)*x - c)^3;   
SPQRPolynomialQuotient[f2, p2, x]//EchoTiming
PolynomialQuotient[f2, p2, x]//EchoTiming
%-%%//Factor*)


(*{q2, r2} = SPQRPolynomialQuotientRemainder[f2, p2, x]*)
(*Expand[f2 - (q2*p2 + r2)] // Factor*)


(*PolynomialQuotient[x^2, x + a, x]
Apart[x^2/( x + a),x]*)


(*SPQRPolynomialQuotient[x^2, x + a, x]
SPQRPolynomialQuotientRemainder[x^2, x + a, x][[1]]*)
