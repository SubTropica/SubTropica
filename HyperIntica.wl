(* ::Package:: *)

(*Get["MultivariateApart`"]*)


(* Print["HyperIntica.wl was last updated on Apr. 1 2026."] *)


(*Print["Currently debugging..."]*)


(* ::Section:: *)
(*Begin*)


BeginPackage["HyperIntica`"]

$STContourHandling = "Abort";


(* ::Section:: *)
(*Description*)


(* ::Subsection:: *)
(*Symbol/Variable Definitions & Transformations Between Symbols*)


(*Definitions.*)

Hlog::usage = "Hlog[z, {w1, w2, ...}] represents a hyperlogarithm.

The iterated integral from 0 to z with singularities at w1, w2, ...:
  Hlog[z; w1,...,wn] = \[Integral]_{0<tn<...<t1<z} dt1/(t1-w1) \[And] ... \[And] dtn/(tn-wn)

Special cases:
  Hlog[z, {}]    = 1
  Hlog[z, {0}]   = Log[z]
  Hlog[z, {1}]   = -Log[1-z]
  Hlog[z, {0,1}] = -PolyLog[2, z]";
  
Mpl::usage = "Mpl[{n1,...,nk}, {z1,...,zk}] represents Li_{n1,...,nk}(z1,...,zk)";


ZeroInfPeriod::usage = "ZeroInfPeriod[word] represents the regularized period 
integral from 0 to \[Infinity].

  ZeroInfPeriod[w] = Reg\[Infinity] Reg\:2080 \[Integral]_0^\[Infinity] \[Omega]_w

For words with letters in {-1, 0}, this evaluates to MZVs.";

ZeroOnePeriod::usage = "ZeroOnePeriod[word] represents the regularized period 
integral from 0 to 1.

  ZeroOnePeriod[w] = Reg\:2081 Reg\:2080 Hlog[1; w]

For words with letters in {-1, 0, 1}, this evaluates to MZVs.";

delta::usage = "delta[var] represents the sign of the imaginary part when 
approaching the real axis during analytic continuation.

  delta[var] = sign(Im(var + i*\[CurlyEpsilon])) = \[PlusMinus]1

Used to track contour deformation around singularities on the 
integration path [0, \[Infinity]).";

mzv::usage = "mzv[n1, n2, ...] represents a multiple zeta value.

  mzv[n1,...,nk] = \[CapitalSigma]_{m1>...>mk\[GreaterEqual]1} sign(n1)^m1...sign(nk)^mk / (m1^|n1|...mk^|nk|)

Positive indices give classical MZVs; negative indices give alternating sums.

Examples:
  mzv[2]    = \[Zeta](2) = \[Pi]\.b2/6
  mzv[3]    = \[Zeta](3) \[TildeTilde] 1.202
  mzv[-1]   = -Log[2]
  mzv[1,2]  = \[Zeta](3)
  mzv[-1,2] = -3/2 Log[2] \[Zeta](2) + \[Zeta](3)";
  
$HyperVerbosity::usage = "Controls verbosity level (0=silent, 1=normal, 2+=debug).";

$HyperInticaCheckDivergences::usage = "If True, check that boundary divergences cancel.";
$HyperInticaAbortOnDivergence::usage = "If True (default), abort with $Failed when a divergence is detected. If False, store the divergence in $HyperInticaDivergences and continue.";
$HyperInticaDivergences::usage = "Association storing any divergences encountered during integration when $HyperInticaAbortOnDivergence is False.";
IntegrationStep::divergence = "Divergence detected integrating out `1` at `2`, pole structure `3`. Set $HyperInticaAbortOnDivergence = False to continue anyway.";

$HyperEvaluatePeriods::usage = "If True, automatically evaluate periods to MZVs.";

$HyperSplittingField::usage = "Algebraic extension field used for factorization throughout HyperIntica (e.g. FactorList, PartialFractions). Set to {} for rationals (default), or e.g. {Sqrt[2]} to adjoin algebraic numbers.";

$QuietPrint::usage = "If True (default), suppresses informational Print[] output from HyperIntica internals. Set to False to see verbose internal prints.";

$HyperAlgebraicRoots::usage = "Allows algebraic roots to appear in LinearFactor[]. 
	The default option is False.";
	
$NoAlgebraicRootsContributions::usage = "Given a linear order of integration, this variable sets wether or 
	not the algebraic roots to appear in LinearFactor[] should be set to zero. 
	The default is True.";
	
NonlinearFactorsCatch::usage = "Non-linear factor catch variable. It is set to True if a non-linear term is encountered and left undefined otherwise.";

$HyperWarnZeroed::usage = "If True (default), print a reminder whenever LinearFactors[] sets a contribution to zero due to a non-linear factor, asking you to confirm a linearly reducible ordering was passed to HyperInt[]. Set to False to suppress these reminders.";

$HyperIntroduceAlgebraicLetters::usage = "If True, LinearFactors[p, var] introduces fresh symbolic letters Wm[i], Wp[i] for each degree-2 factor it encounters, rather than failing or zeroing. Metadata (roots, Vieta sum/product, discriminant) is stored in $HyperAlgebraicLetterTable[i]. Default: False.";
$HyperAlgebraicLetterCounter::usage = "Monotone counter for algebraic letters introduced by LinearFactors when $HyperIntroduceAlgebraicLetters is True. Reset with ClearAlgebraicLetters[].";
$HyperAlgebraicLetterTable::usage = "Association mapping each algebraic-letter index i to a metadata record with keys Polynomial, Variable, LC, Sum, Product, Discriminant, WmValue, WpValue.";
ClearAlgebraicLetters::usage = "ClearAlgebraicLetters[] resets $HyperAlgebraicLetterCounter and $HyperAlgebraicLetterTable.";
GetAlgebraicBackSubRules::usage = "GetAlgebraicBackSubRules[] returns rules {Wm[i] -> r_minus, Wp[i] -> r_plus} for every letter i in $HyperAlgebraicLetterTable.";
GetAlgebraicVietaRules::usage = "GetAlgebraicVietaRules[] returns rules collapsing Wm[i]+Wp[i] and Wm[i]*Wp[i] to their rational Vieta values.";
SimplifyWithVieta::usage = "SimplifyWithVieta[expr] reduces expr to at-most-linear form in each Wm[i], Wp[i] pair via Wm*Wp -> product, Wm^2 -> s*Wm - p, etc. Symmetric combinations collapse to rational expressions; antisymmetric residues keep Wm[i] - Wp[i].";
CanonicalizeAlgebraicLetters::usage = "CanonicalizeAlgebraicLetters[results_:None] prunes $HyperAlgebraicLetterTable to indices that actually appear in `results` (when given), dedupes survivors up to the mirror-swap equivalence Sort[{Together[WmValue], Together[WpValue]}], renumbers survivors 1..k consecutively (mutating the table in place), and returns a rule list {Wm[old] -> Wm[new], Wp[old] -> Wp[new], ...} (or the swapped pair when the group's survivor used the opposite sign convention) to apply to any expression carrying the letters. Pass the final result so that Fubini-intermediate letters that cancelled in the end are dropped from the on-disk metadata.";
Wm::usage = "Wm[i] is the i-th algebraic root letter with the '-Sqrt' branch; introduced by LinearFactors[] when $HyperIntroduceAlgebraicLetters is True. Metadata in $HyperAlgebraicLetterTable[i].";
Wp::usage = "Wp[i] is the i-th algebraic root letter with the '+Sqrt' branch; introduced by LinearFactors[] when $HyperIntroduceAlgebraicLetters is True. Metadata in $HyperAlgebraicLetterTable[i].";
WmOverWp::usage = "WmOverWp[i] is the compound alphabet letter Wm[i]/Wp[i] introduced by the symbol-level ratio-merge pass (stCombineWmWpRatios) when two symbol terms differ only in one slot by the Wm \[LeftRightArrow] Wp conjugation with opposite coefficients.  Kept as an atomic symbol rather than the literal ratio so downstream lookup tables and LaTeX rendering can treat it as a first-class letter.";
STShowAlgebraicLetters::usage = "STShowAlgebraicLetters[] prints a table of the currently-defined algebraic root letters Wm[i]/Wp[i] introduced by the most recent FindRoots integration: the polynomial, its Feynman-parameter variable, the discriminant, the two roots, and Vieta sum/product. Call it from a notebook after STIntegrate[..., FindRoots -> True] to see what each Wm[i]/Wp[i] stands for.";

$UseFFPolynomialQuotient::usage = "If True, PartialFractions[] uses SPQRPolynomialQuotient[] \
(finite-field arithmetic via FiniteFlow/SPQR) instead of PolynomialQuotient[]. \
Set automatically by ConfigureSubTropica[].";

$PartialFractionsMaxWeight::usage = "MaxWeight parameter {wmin, wmax} passed to SPQRPolynomialQuotient[] \
when $UseFFPolynomialQuotient is True. Default {1, 100}. \
Set this before calling HyperInt[] or STEvaluate[] to tune the finite-field computation. \
Can also be overridden per-call via the \"MaxWeight\" option of PartialFractions[].";

(*Conversion.*)

HlogAsMpl::usage = "HlogAsMpl[z, {w1,...,wn}] expresses a hyperlogarithm as a 
multiple polylogarithm.

  Hlog[z, word] = (-1)^depth * Mpl[{n1,...,nk}, {z1,...,zk}]

The conversion reads the word from right to left:
  - Each non-zero letter becomes a pole in the MPL
  - Consecutive zeros before a non-zero letter increase the index

Example:
  HlogAsMpl[z, {0, 0, 1}] returns -Mpl[{3}, {z}] = -Li\:2083(z)";
  
ConvertToMpl::usage = "Convert any expression containing Hlog to Mpl notation";


ConvertToHlogRegInf::usage = "ConvertToHlogRegInf[expr] converts a mathematical 
expression to the internal wordlist format.

Handles:
  \[Bullet] Hlog expressions
  \[Bullet] Log and PolyLog functions  
  \[Bullet] Products (via shuffle product)
  \[Bullet] Sums
  \[Bullet] Powers with positive integer exponents

Returns:
  Internal format {{coef, shuffleKey}, ...} suitable for IntegrationStep.";
  
ConvertZeroOne::usage = "ConvertZeroOne[wordlist] converts zero-to-infinity 
integrals to zero-to-one integrals via z \[RightTeeArrow] 1/(1+z).";
  
ZeroInfPeriodEval::usage = "ZeroInfPeriodEval[word] evaluates a zero-to-infinity 
period, handling positive letters via contour deformation.";

TryReduceZeroOnePeriod::usage = "TryReduceZeroOnePeriod[word] attempts to reduce 
a ZeroOnePeriod to known MZVs using various transformations.";

ToMZV::usage = "ToMZV[wordlist] converts words in {-1,0,1} alphabet to 
mzv[...] notation.";

mzvAllReductions::usage = "Association of MZV reduction rules to the standard basis.

Contains rules like mzv[1,2] :> mzv[3] to reduce MZVs to the minimal basis
{mzv[2], mzv[3], mzv[5], mzv[7], ...} plus products.";

MplAsHlog::usage = "MplAsHlog[{n1,...,nk}, {z1,...,zk}] expresses a multiple 
polylogarithm as a hyperlogarithm.

  Li_{n1,...,nk}(z1,...,zk) = (-1)^k Hlog[1; word]

where word encodes the indices and arguments.";

ZeroInfPeriodAsMpl::usage = "ZeroInfPeriodAsMpl[word] converts a ZeroInfPeriod 
to Mpl (multiple polylogarithm) notation.
Example:
  ZeroInfPeriodAsMpl[{X13/X24}] returns -Log[-X13/X24]
  ZeroInfPeriodAsMpl[{X13/X24, -1}] returns Mpl[{2}, {1 + X13/X24}]";


$HyperIgnoreNonlinearPolynomials::usage = "If True, silently discard nonlinear 
polynomial factors instead of throwing an error. The default is False.";

$NonlinearFactorsCache::usage = "Association caching information about nonlinear 
polynomial factors encountered during LinearFactors[].

Each entry has the form:
  {polynomial, variable} -> <|
    \"Polynomial\" -> poly,
    \"Variable\" -> var,
    \"Degree\" -> deg,
    \"Timestamp\" -> date,
    \"Fatal\" -> True|False
  |>

Use GetNonlinearFactors[] to retrieve all cached entries.";


(* ::Subsection::Closed:: *)
(*Core Functions*)


HyperInt::usage = "HyperInt[f, vars] integrates the expression f over the
variables vars from 0 to \[Infinity].
Option: \"Monitor\" -> False. If True, prints progress to stdout as each integration variable is processed.

Arguments:
  f    - Expression to integrate (automatically converted to internal format)
  vars - Variable or list of variables to integrate over

Returns:
  The integrated result as a combination of multiple zeta values.

Example:
  HyperInt[1/(1+x)/(1+y), {x, y}]  returns  mzv[2]";
  
HyperIntica::usage = "HyperIntica[f, vars] integrates the expression f over the
variables vars from 0 to \[Infinity].
Option: \"Monitor\" -> False. If True, prints progress to stdout as each integration variable is processed.

Arguments:
  f    - Expression to integrate (automatically converted to internal format)
  vars - Variable or list of variables to integrate over

Supported calling conventions:
  HyperIntica[f, {x, y}]                      - integrate over x, y from 0 to \[Infinity]
  HyperIntica[f, {x, 0, 1}, {y, 0, Infinity}] - Mathematica Integrate-style limits
  HyperIntica[f, {x -> {0, 1}, y -> {0, 1}}]  - Rule-based limits

Returns:
  The integrated result as a combination of multiple zeta values.

Example:
  HyperIntica[1/(1+x)/(1+y), {x, y}]  returns  mzv[2]";

IntegrationStep::usage = "IntegrationStep[wordlist, var] performs one integration 
step, integrating var from 0 to \[Infinity].

Arguments:
  wordlist - Internal representation {{coef, shuffleKey}, ...}
  var      - Integration variable

Returns:
  Updated wordlist with var integrated out.

This is the core function implementing the integration algorithm. It:
  1. Transforms words via TransformShuffle
  2. Integrates rational prefactors 
  3. Computes boundary expansions at 0 and \[Infinity]
  4. Checks divergence cancellation
  5. Handles analytic continuation for positive letters";  
  
FibrationBasis::usage = "FibrationBasis[wordlist, vars] transforms wordlist into 
a fibration basis representation.

Arguments:
  wordlist - Internal wordlist or expression (like PolyLog[__,__])
  vars     - List of variables defining the fibration order

Returns:
  Expression in terms of Hlog[var, word] products.";


(* ::Subsection::Closed:: *)
(*Auxiliary Functions*)


ShuffleProduct::usage = "ShuffleProduct[a, b] computes the shuffle product of 
two wordlists.

The shuffle product is the sum over all interleavings that preserve
the relative order of letters within each word.

Example:
  ShuffleProduct[{{1, {a}}}, {{1, {b}}}] 
  returns {{1, {a,b}}, {1, {b,a}}}";
  
EvaluatePeriods::usage = "EvaluatePeriods[result] evaluates all symbolic periods 
in the result to numerical/algebraic values.

Converts ZeroInfPeriod[word] expressions to combinations of MZVs.";

BreakUpContour::usage = "BreakUpContour[wordlist, onAxis] handles positive letters 
by decomposing the contour integral.

Arguments:
  wordlist - {{coef, word}, ...}
  onAxis   - {{letter, delta[var]}, ...} specifying which positive letters
             are on the integration path and their approach direction

Returns:
  Wordlist with no positive letters (contour deformed to avoid them).";
  
TransformShuffle::usage = "TransformShuffle[wordlist, var] transforms a shuffle 
product of words into integration constants plus (simpler!) words in var. 
This is the basis function for FibrationBasis[].";

TransformWord::usage = "TransformWord[word, var] transforms a single word, 
extracting the dependence on var and computing integration constants.";

ReglimWord::usage = "ReglimWord[word, var] computes the regularized limit of 
a word as var \[RightArrow] 0.";


GetNonlinearFactors::usage = "GetNonlinearFactors[] returns the association 
$NonlinearFactorsCache containing all nonlinear factors encountered.

Each key is {polynomial, variable} and each value is an association with:
  \"Polynomial\" -> the polynomial that did not factor linearly
  \"Variable\" -> the integration variable
  \"Degree\" -> degree of the polynomial
  \"Timestamp\" -> when it was encountered
  \"Fatal\" -> True if it caused an error, False if ignored";

HasNonlinearFactors::usage = "HasNonlinearFactors[] returns True if any 
nonlinear polynomial factors were encountered during computation, False otherwise.

Example:
  ForgetAllMemo[];
  result = HyperInt[...];
  If[HasNonlinearFactors[], Print[\"Warning: nonlinear factors found\"]]";

GetFatalNonlinearFactors::usage = "GetFatalNonlinearFactors[] returns only the 
nonlinear factors that were fatal (i.e., caused $Failed to be returned rather 
than being silently ignored via $HyperIgnoreNonlinearPolynomials).";

PrintNonlinearSummary::usage = "PrintNonlinearSummary[] prints a human-readable 
summary of all nonlinear polynomial factors encountered during computation.

Output format:
  Nonlinear factors encountered: n
    poly1 in variable x [FATAL]
    poly2 in variable y [ignored]
    ...";

ClearNonlinearCache::usage = "ClearNonlinearCache[] clears only the 
$NonlinearFactorsCache without affecting other memoization caches.";



HyperD::usage = "HyperD[expr, var] computes the derivative of expr with respect to var, 
correctly handling Hlog and Mpl functions.

Unlike the built-in D[], HyperD properly differentiates through Hlog and Mpl 
in complex expressions where these functions appear with var-dependent arguments.

Examples:
  HyperD[Mpl[{2}, {1 - x/u}], x]
  HyperD[Hlog[z, {a, b}], a]
  HyperD[Log[x] * Mpl[{1,2}, {x, y}], x]";

DiffHlog::usage = "DiffHlog[arg, word, var] computes the derivative of Hlog[arg, word] 
with respect to var.

Implements the standard formula for differentiating iterated integrals:
  d/dvar Hlog[z; w1,...,wn] includes contributions from:
    - The upper boundary z
    - Differences of neighboring letters (wi - w_{i+1})
    - The last letter wn (if nonzero)
    - The first letter w1 (if z \[NotEqual] \[Infinity])

This is the core differentiation routine called by HyperD for Hlog terms.";

DiffMpl::usage = "DiffMpl[ns, zs, var] computes the derivative of Mpl[ns, zs] 
with respect to var.

Uses the recursive structure of multiple polylogarithms:
  d/dvar Li_{n1,...,nk}(z1,...,zk) 
involves lowering indices and merging adjacent arguments.

Supports only positive integer indices. This is the core differentiation 
routine called by HyperD for Mpl terms.

Example:
  DiffMpl[{2}, {z}, z] returns Mpl[{1}, {z}]/z
  DiffMpl[{1, 2}, {x, y}, y] differentiates Li_{1,2}(x,y) w.r.t. y";


HyperSeries::usage = "HyperSeries[expr, {var, point, order}] computes the series expansion of expr around var = point up to O[var - point]^(order+1).

Handles expressions containing Hlog and Mpl by selecting a strategy for each factor:
  - Hlog with arg -> 0: ZeroExpansion.
  - Mpl with last arg -> 0: MplSum (defining partial sum, all depths).
  - Mpl with first arg -> 0: MplSum (outer sum vanishes, all depths).
  - Mpl of depth 1 (= PolyLog[n,z]): built-in Series via PolyLog.
  - Mpl of depth > 1 with last arg -> 1 and first arg fixed: returns unevaluated with a warning (log-series singularity; not yet implemented).
  - Otherwise: Taylor expansion via HyperD.

Unlike the built-in Series[], HyperSeries correctly expands through Hlog and Mpl in expressions where these functions appear with var-dependent arguments. Power::infy and Infinity::indet messages are suppressed internally.

Examples:
  HyperSeries[Mpl[{2}, {z}], {z, 0, 5}]
  HyperSeries[Mpl[{2}, {1 - x/u}], {x, 0, 3}]
  HyperSeries[Hlog[z, {0, 1}]/z, {z, 0, 4}]
  HyperSeries[Hlog[1, {u/(-1+u+\[Delta])}], {\[Delta], 0, 2}]";

HlogSeries::usage = "HlogSeries[arg, word, var, order] computes the series expansion of Hlog[arg, word] around var = 0 up to O[var]^(order+1).

Strategy:
  - If arg -> 0 as var -> 0: uses ZeroExpansion.
  - Otherwise: Taylor expansion via HyperD.

Examples:
  HlogSeries[z, {0, 1}, z, 5]
  HlogSeries[1, {u/(u-1+\[Delta])}, \[Delta], 3]";

MplSeries::usage = "MplSeries[ns, zs, var, order] computes the series expansion of Mpl[ns, zs] around var = 0 up to O[var]^(order+1).

Strategy:
  - If last arg -> 0: MplSum (defining partial sum, all depths).
  - If first arg -> 0: MplSum (outer sum vanishes, valid for any last arg).
  - If depth = 1 (= PolyLog[n,z]): delegates to Mathematica's built-in Series.
  - If depth > 1 and last arg -> 1 with first arg fixed: log-series singularity;
    returns unevaluated with a warning.
  - Otherwise: Taylor expansion via HyperD.

Examples:
  MplSeries[{2}, {z}, z, 5]
  MplSeries[{1, 2}, {x, 1-x/u}, x, 3]";

MplSum::usage = "MplSum[ns, zs, maxN] computes the defining partial sum of Mpl[ns, zs] with summation indices running from 1 to maxN.

  MplSum[{n1,...,nk}, {z1,...,zk}, N] = Sum_{N>=m1>...>mk>=1} z1^m1...zk^mk / (m1^n1...mk^nk)

Used internally by MplSeries and HyperSeries.";


(*HlogDerivative::usage = "HlogDerivative[arg, word, var] computes the derivative 
of Hlog[arg, word] with respect to var.";

MplDerivative::usage = "MplDerivative[ns, zs, var] computes the derivative 
of Mpl[ns, zs] with respect to var.";*)


(* ::Subsection:: *)
(*Miscellanea*)


CollectWords::usage = "CollectWords[wordlist] collects terms with identical words.

Combines entries {{c1, w}, {c2, w}, ...} into {{c1+c2, w}, ...}
and removes terms with zero coefficients.";

ForgetAllMemo::usage = "ForgetAllMemo[] clears all memoization caches.

Call this when changing parameters or starting a new calculation
to ensure fresh results.";

simplifyMZVeven::usage = "Express mzv[n] in terms of mzv[2] when n is even.";

(*Primitive and symbol-related functions:*)
IntegrateII::usage = "IntegrateII[wordlist, var] finds an unevaluated primitive of wordlist with respect to var, returning a new wordlist whose differentiation with respect to var via DifferentiateWordlist[] recovers the original.";

DifferentiateWordlist::usage = "DifferentiateWordlist[wordlist, var] differentiates 
a flat wordlist {{coef, word}, ...} with respect to var. Used to verify IntegrateII 
via DifferentiateWordlist[IntegrateII[wl, x], x] == wl.";

ShuffleCompress::usage = "ShuffleCompress[wordlist] expands shuffle representation 
{{coef, {w1, w2, ...}}, ...} to flat format {{coef, word}, ...} for use with IntegrateII. 
Equivalent to Maple's shuffleCompress.";

ConvertToSymbol::usage = "ConvertToSymbol[expr, opts] converts an expression 
containing hyperlogarithms to its symbol representation.

Options:
  \"Expand\" -> True       - Expand symbol letters into irreducible factors
  \"AsList\" -> False      - If True, return as {{coef, {letter1, ...}}, ...}
                             If False, return as sum of Sym[{...}] terms
  \"DropConstants\" -> False - Remove entries containing only constant letters

The symbol captures the leading singularity structure of polylogarithms.";

Sym::usage = "Sym[{a1, a2, ...}] represents a symbol tensor a1 \[CircleTimes] a2 \[CircleTimes] ...
  Sym[{}] = 1 (empty tensor)";

SymbolExpand::usage = "SymbolExpand[symbolList] expands each letter in the symbol 
into its irreducible polynomial factors.";

SymbolSimplify::usage = "SymbolSimplify[symbolList] simplifies a symbol by:
  1. Combining terms with identical tensor structure
  2. Removing terms with zero coefficients
  3. Optionally factoring letters";
  
MaybeQuiet::usage = "MaybeQuiet[expr] evaluates expr while suppressing print output when $HyperVerbosity = 0. Used internally throughout the code to gate diagnostic messages.";
  
RegTail::usage = "RegTail[wordlist, letter, substitute] regularizes words in wordlist by stripping trailing occurrences of letter and replacing them with the regulator value substitute (default 0).

Used internally to compute the regularized limit Reg_0 Hlog[z; w] as z -> 0.

Arguments:
  wordlist   - {{coef, word}, ...}
  letter     - letter to strip from the tail (typically 0)
  substitute - replacement value for each stripped letter (default 0)";

RegHead::usage = "RegHead[wordlist, letter, substitute] regularizes words in wordlist by stripping leading occurrences of letter and replacing them with the regulator value substitute (default 0).

Used internally to compute the regularized value Reg_1 Hlog[1; w].

Arguments:
  wordlist   - {{coef, word}, ...}
  letter     - letter to strip from the head (typically 0 or 1)
  substitute - replacement value for each stripped letter (default 0)";

ShuffleWords::usage = "ShuffleWords[v, w] returns the list of all shuffles of word lists v and w; i.e., all interleavings that preserve the relative ordering of letters within each input.

Example:
  ShuffleWords[{a, b}, {c}] = {{a, b, c}, {a, c, b}, {c, a, b}}

Used internally by ShuffleProduct. Results are memoized in \$ShuffleWordsCache.";


PartialFractions::usage = "PartialFractions[f, var] computes the partial fraction decomposition of the rational function f in var over \$HyperSplittingField.

Returns a list {poly, {zero1, m1, c_{1,1},...,c_{1,m1}}, ...} where:
  poly    - polynomial (integer) part
  zero_i  - pole location
  m_i     - pole order at zero_i
  c_{i,j} - coefficient of 1/(var - zero_i)^j

Uses NestList of derivatives instead of Apart[] for efficiency. Results are memoized in \$PartialFractionsCache.
If \$UseFFPolynomialQuotient is True (set by ConfigureSubTropica[]), uses SPQRPolynomialQuotient[] for the polynomial division step.
Option \"MaxWeight\" -> {wmin,wmax} overrides \$PartialFractionsMaxWeight for this call only.";

LinearFactors::usage = "LinearFactors[p, var] factors p over \$HyperSplittingField and returns the list of linear zeros as {{multiplicity, zero}, ...}.

For degree > 1 irreducible factors, the behavior depends on:
  - \$HyperAlgebraicRoots = True: uses Root[] objects
  - \$HyperIgnoreNonlinearPolynomials = True: silently discards
  - Otherwise: fires LinearFactors::nonlinear or LinearFactorsE::nonlinear

Results are memoized in \$LinearFactorsCache.";

PoleDegree::usage = "PoleDegree[p, var] returns the pole degree (minimum Laurent exponent) of the rational function p in var.

  PoleDegree[p, var] = min_deg(numerator) - min_deg(denominator)

Returns 0 if p is free of var, Infinity if p = 0. Results are memoized in \$PoleDegreeCache.";

RatResidue::usage = "RatResidue[f, var] returns the leading coefficient in the Laurent expansion of f in var; i.e., the coefficient of var^PoleDegree[f, var].

Used internally to extract integration constants during TransformWord[]. Results are memoized in \$RatResidueCache.";

ConvertToSymbolInternal::usage = "ConvertToSymbolInternal[expr] is the internal recursive engine for ConvertToSymbol[]. Returns the symbol of expr as a wordlist {{coef, {letter1,...}}, ...}.

Handles: sums (recursively), products (via ShuffleProduct), integer powers, Hlog, Log, PolyLog, Mpl (via MplAsHlog), and constants (mzv, Zeta, Pi -> {}).";


factorCompletely::usage = "factorCompletely[poly, x] factors poly completely by solving for roots numerically (Cubics and Quartics options are off) and returns the explicit product form lcoeff * (x - r1) * (x - r2) * ...";

factorCompletely2::usage = "factorCompletely2[poly, x, label] factors poly like factorCompletely[] but substitutes abstract symbols for the roots, storing the map from abstract symbols to numerical values in explicit[label].

Used when a symbolic factored form is needed that can be evaluated later via explicit[label].";

explicit::usage = "explicit[label] holds the substitution rules created by factorCompletely2[poly, x, label], mapping abstract root symbols to their numerical values.";

STFactorAndTrackRoots::usage = "STFactorAndTrackRoots[poly, x, label] factors poly in x completely, substituting abstract symbols for the roots and storing the map from those symbols to their numerical values in explicit[label].";


(* ::Section::Closed:: *)
(*Begin Private Environment*)


Begin["`Private`"]


(* ::Section::Closed:: *)
(*MZV Reductions up to Weight 6*)


(*Generated from Maple HyperInt zeroOnePeriod*)

mzvAllReductions = Join[{
  (*word [-1]*)
  -mzv[-1] -> Log[2],

  (*word [-1, -1]*)
  mzv[1,-1] -> 1/2*Log[2]^2,

  (*word [-1, 1]*)
  mzv[-1,-1] -> 1/2*Log[2]^2-1/2*mzv[2],

  (*word [0, -1]*)
  -mzv[-2] -> 1/2*mzv[2],

  (*word [-1, -1, -1]*)
  -mzv[1,1,-1] -> 1/6*Log[2]^3,

  (*word [-1, -1, 1]*)
  -mzv[-1,1,-1] -> 1/6*Log[2]^3-1/8*mzv[3],

  (*word [-1, 0, -1]*)
  mzv[2,-1] -> 1/2*Log[2]*mzv[2]-1/4*mzv[3],

  (*word [-1, 0, 1]*)
  mzv[-2,-1] -> -Log[2]*mzv[2]+5/8*mzv[3],

  (*word [-1, 1, -1]*)
  -mzv[-1,-1,-1] -> 1/6*Log[2]^3-1/2*Log[2]*mzv[2]+1/4*mzv[3],

  (*word [-1, 1, 1]*)
  -mzv[1,-1,-1] -> 1/6*Log[2]^3-1/2*Log[2]*mzv[2]+7/8*mzv[3],

  (*word [0, -1, -1]*)
  mzv[1,-2] -> 1/8*mzv[3],

  (*word [0, -1, 1]*)
  mzv[-1,-2] -> 3/2*Log[2]*mzv[2]-13/8*mzv[3],

  (*word [0, 0, -1]*)
  -mzv[-3] -> 3/4*mzv[3],

  (*word [0, 1, -1]*)
  mzv[-1,2] -> -3/2*Log[2]*mzv[2]+mzv[3],

  (*word [0, 1, 1]*)
  mzv[1,2] -> mzv[3],

  (*word [-1, -1, -1, -1]*)
  mzv[1,1,1,-1] -> 1/24*Log[2]^4,

  (*word [-1, -1, -1, 1]*)
  mzv[-1,1,1,-1] -> 1/24*Log[2]^4-1/40*mzv[2]^2+1/2*mzv[1,-3],

  (*word [-1, -1, 0, -1]*)
  -mzv[2,1,-1] -> 1/4*Log[2]^2*mzv[2]-1/4*Log[2]*mzv[3]+3/40*mzv[2]^2-3/2*mzv[1,-3],

  (*word [-1, -1, 0, 1]*)
  -mzv[-2,1,-1] -> -1/2*Log[2]^2*mzv[2]+5/8*Log[2]*mzv[3]-3/40*mzv[2]^2,

  (*word [-1, -1, 1, -1]*)
  mzv[-1,-1,1,-1] -> 1/24*Log[2]^4-1/8*Log[2]*mzv[3]+3/40*mzv[2]^2-3/2*mzv[1,-3],

  (*word [-1, -1, 1, 1]*)
  mzv[1,-1,1,-1] -> 1/24*Log[2]^4-1/8*Log[2]*mzv[3]+1/20*mzv[2]^2,

  (*word [-1, 0, -1, -1]*)
  -mzv[1,2,-1] -> 1/8*Log[2]*mzv[3]-3/40*mzv[2]^2+3/2*mzv[1,-3],

  (*word [-1, 0, -1, 1]*)
  -mzv[-1,2,-1] -> 3/2*Log[2]^2*mzv[2]-5/2*Log[2]*mzv[3]+1/4*mzv[2]^2+2*mzv[1,-3],

  (*word [-1, 0, 0, -1]*)
  mzv[3,-1] -> 3/4*Log[2]*mzv[3]-1/8*mzv[2]^2,

  (*word [-1, 0, 0, 1]*)
  mzv[-3,-1] -> -Log[2]*mzv[3]+3/20*mzv[2]^2+mzv[1,-3],

  (*word [-1, 0, 1, -1]*)
  -mzv[-1,-2,-1] -> -3/2*Log[2]^2*mzv[2]+15/8*Log[2]*mzv[3]-1/10*mzv[2]^2-2*mzv[1,-3],

  (*word [-1, 0, 1, 1]*)
  -mzv[1,-2,-1] -> Log[2]*mzv[3]-1/4*mzv[2]^2-1/2*mzv[1,-3],

  (*word [-1, 1, -1, -1]*)
  mzv[1,-1,-1,-1] -> 1/24*Log[2]^4-1/4*Log[2]^2*mzv[2]+1/4*Log[2]*mzv[3]-3/40*mzv[2]^2+3/2*mzv[1,-3],

  (*word [-1, 1, -1, 1]*)
  mzv[-1,-1,-1,-1] -> 1/24*Log[2]^4-1/4*Log[2]^2*mzv[2]+1/4*Log[2]*mzv[3]+1/40*mzv[2]^2,

  (*word [-1, 1, 0, -1]*)
  -mzv[-2,-1,-1] -> 1/4*Log[2]^2*mzv[2]-1/4*Log[2]*mzv[3]-3/20*mzv[2]^2+2*mzv[1,-3],

  (*word [-1, 1, 0, 1]*)
  -mzv[2,-1,-1] -> -1/2*Log[2]^2*mzv[2]+5/8*Log[2]*mzv[3]+1/10*mzv[2]^2-1/2*mzv[1,-3],

  (*word [-1, 1, 1, -1]*)
  mzv[-1,1,-1,-1] -> 1/24*Log[2]^4-1/4*Log[2]^2*mzv[2]+7/8*Log[2]*mzv[3]-1/8*mzv[2]^2,

  (*word [-1, 1, 1, 1]*)
  mzv[1,1,-1,-1] -> 1/24*Log[2]^4-1/4*Log[2]^2*mzv[2]+7/8*Log[2]*mzv[3]-3/8*mzv[2]^2-1/2*mzv[1,-3],

  (*word [0, -1, -1, -1]*)
  -mzv[1,1,-2] -> 1/40*mzv[2]^2-1/2*mzv[1,-3],

  (*word [0, -1, -1, 1]*)
  -mzv[-1,1,-2] -> -3/4*Log[2]^2*mzv[2]+7/4*Log[2]*mzv[3]-3/10*mzv[2]^2-mzv[1,-3],

  (*word [0, -1, 0, -1]*)
  mzv[2,-2] -> 1/8*mzv[2]^2-2*mzv[1,-3],

  (*word [0, -1, 0, 1]*)
  mzv[-2,-2] -> -3/40*mzv[2]^2,

  (*word [0, -1, 1, -1]*)
  -mzv[-1,-1,-2] -> 3/2*Log[2]^2*mzv[2]-21/8*Log[2]*mzv[3]+7/20*mzv[2]^2,

  (*word [0, -1, 1, 1]*)
  -mzv[1,-1,-2] -> 3/4*Log[2]^2*mzv[2]-21/8*Log[2]*mzv[3]+23/40*mzv[2]^2+3/2*mzv[1,-3],

  (*word [0, 0, -1, 1]*)
  mzv[-1,-3] -> 7/4*Log[2]*mzv[3]-11/20*mzv[2]^2-mzv[1,-3],

  (*word [0, 0, 0, -1]*)
  -mzv[-4] -> 7/20*mzv[2]^2,

  (*word [0, 0, 0, 1]*)
  -mzv[4] -> -2/5*mzv[2]^2,

  (*word [0, 0, 1, -1]*)
  mzv[-1,3] -> -7/4*Log[2]*mzv[3]+19/40*mzv[2]^2,

  (*word [0, 0, 1, 1]*)
  mzv[1,3] -> 1/10*mzv[2]^2,

  (*word [0, 1, -1, -1]*)
  -mzv[1,-1,2] -> -3/4*Log[2]^2*mzv[2]+7/8*Log[2]*mzv[3]-1/8*mzv[2]^2+mzv[1,-3],

  (*word [0, 1, -1, 1]*)
  -mzv[-1,-1,2] -> -3/2*Log[2]^2*mzv[2]+21/8*Log[2]*mzv[3]-1/4*mzv[2]^2-3/2*mzv[1,-3],

  (*word [0, 1, 0, -1]*)
  mzv[-2,2] -> -11/40*mzv[2]^2+2*mzv[1,-3],

  (*word [0, 1, 0, 1]*)
  mzv[2,2] -> 3/10*mzv[2]^2,

  (*word [0, 1, 1, -1]*)
  -mzv[-1,1,2] -> 3/4*Log[2]^2*mzv[2]-3/40*mzv[2]^2+1/2*mzv[1,-3],

  (*word [0, 1, 1, 1]*)
  -mzv[1,1,2] -> -2/5*mzv[2]^2,

  (*word [-1, -1, -1, -1, -1]*)
  -mzv[1,1,1,1,-1] -> 1/120*Log[2]^5,

  (*word [-1, -1, -1, -1, 1]*)
  -mzv[-1,1,1,1,-1] -> 1/120*Log[2]^5+1/4*mzv[2]*mzv[3]-31/64*mzv[5]-1/2*mzv[1,1,-3],

  (*word [-1, -1, -1, 0, -1]*)
  mzv[2,1,1,-1] -> 1/12*Log[2]^3*mzv[2]-1/8*Log[2]^2*mzv[3]+3/40*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]+mzv[2]*mzv[3]-31/16*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, -1, -1, 0, 1]*)
  mzv[-2,1,1,-1] -> -1/6*Log[2]^3*mzv[2]+5/16*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2-3/16*mzv[2]*mzv[3]+25/64*mzv[5]-1/2*mzv[1,1,-3],

  (*word [-1, -1, -1, 1, -1]*)
  -mzv[-1,-1,1,1,-1] -> 1/120*Log[2]^5-1/40*Log[2]*mzv[2]^2+1/2*Log[2]*mzv[1,-3]-mzv[2]*mzv[3]+31/16*mzv[5]+2*mzv[1,1,-3],

  (*word [-1, -1, -1, 1, 1]*)
  -mzv[1,-1,1,1,-1] -> 1/120*Log[2]^5-1/40*Log[2]*mzv[2]^2+1/2*Log[2]*mzv[1,-3]-3/4*mzv[2]*mzv[3]+93/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [-1, -1, 0, -1, -1]*)
  mzv[1,2,1,-1] -> 1/16*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]-3/2*mzv[2]*mzv[3]+93/32*mzv[5]+3*mzv[1,1,-3],

  (*word [-1, -1, 0, -1, 1]*)
  mzv[-1,2,1,-1] -> 3/4*Log[2]^3*mzv[2]-27/16*Log[2]^2*mzv[3]+2/5*Log[2]*mzv[2]^2+1/2*Log[2]*mzv[1,-3]+21/16*mzv[2]*mzv[3]-87/32*mzv[5],

  (*word [-1, -1, 0, 0, -1]*)
  -mzv[3,1,-1] -> 3/8*Log[2]^2*mzv[3]-1/8*Log[2]*mzv[2]^2-7/16*mzv[2]*mzv[3]+59/64*mzv[5]+mzv[1,1,-3],

  (*word [-1, -1, 0, 0, 1]*)
  -mzv[-3,1,-1] -> -1/2*Log[2]^2*mzv[3]+3/20*Log[2]*mzv[2]^2+Log[2]*mzv[1,-3]-1/4*mzv[2]*mzv[3]+3/8*mzv[5]+2*mzv[1,1,-3],

  (*word [-1, -1, 0, 1, -1]*)
  mzv[-1,-2,1,-1] -> -3/4*Log[2]^3*mzv[2]+11/8*Log[2]^2*mzv[3]-1/4*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]-3/4*mzv[2]*mzv[3]+99/64*mzv[5]+3/2*mzv[1,1,-3],

  (*word [-1, -1, 0, 1, 1]*)
  mzv[1,-2,1,-1] -> 1/2*Log[2]^2*mzv[3]-1/4*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+1/4*mzv[2]*mzv[3]-17/64*mzv[5]-1/2*mzv[1,1,-3],

  (*word [-1, -1, 1, -1, -1]*)
  -mzv[1,-1,-1,1,-1] -> 1/120*Log[2]^5-1/16*Log[2]^2*mzv[3]+3/40*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]+3/2*mzv[2]*mzv[3]-93/32*mzv[5]-3*mzv[1,1,-3],

  (*word [-1, -1, 1, -1, 1]*)
  -mzv[-1,-1,-1,1,-1] -> 1/120*Log[2]^5-1/16*Log[2]^2*mzv[3]+3/40*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]+9/4*mzv[2]*mzv[3]-69/16*mzv[5]-3/2*mzv[1,1,-3],

  (*word [-1, -1, 1, 0, -1]*)
  mzv[-2,-1,1,-1] -> 1/12*Log[2]^3*mzv[2]-1/8*Log[2]^2*mzv[3]+3/40*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]+27/16*mzv[2]*mzv[3]-211/64*mzv[5]-7/2*mzv[1,1,-3],

  (*word [-1, -1, 1, 0, 1]*)
  mzv[2,-1,1,-1] -> -1/6*Log[2]^3*mzv[2]+5/16*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2+7/16*mzv[2]*mzv[3]-47/64*mzv[5]+2*mzv[1,1,-3],

  (*word [-1, -1, 1, 1, -1]*)
  -mzv[-1,1,-1,1,-1] -> 1/120*Log[2]^5-1/16*Log[2]^2*mzv[3]+1/20*Log[2]*mzv[2]^2-3/64*mzv[5],

  (*word [-1, -1, 1, 1, 1]*)
  -mzv[1,1,-1,1,-1] -> 1/120*Log[2]^5-1/16*Log[2]^2*mzv[3]+1/20*Log[2]*mzv[2]^2+1/4*mzv[2]*mzv[3]-35/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [-1, 0, -1, -1, -1]*)
  mzv[1,1,2,-1] -> 1/40*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+mzv[2]*mzv[3]-31/16*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, 0, -1, -1, 1]*)
  mzv[-1,1,2,-1] -> -3/4*Log[2]^3*mzv[2]+35/16*Log[2]^2*mzv[3]-5/8*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]-5/16*mzv[2]*mzv[3]+31/32*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, 0, -1, 0, -1]*)
  -mzv[2,2,-1] -> 1/8*Log[2]*mzv[2]^2-2*Log[2]*mzv[1,-3]+15/8*mzv[2]*mzv[3]-59/16*mzv[5]-4*mzv[1,1,-3],

  (*word [-1, 0, -1, 0, 1]*)
  -mzv[-2,2,-1] -> -3/40*Log[2]*mzv[2]^2-1/2*mzv[2]*mzv[3]+33/32*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, 0, -1, 1, -1]*)
  mzv[-1,-1,2,-1] -> 3/2*Log[2]^3*mzv[2]-7/2*Log[2]^2*mzv[3]+7/10*Log[2]*mzv[2]^2+4*Log[2]*mzv[1,-3]-2*mzv[2]*mzv[3]+7/2*mzv[5]+4*mzv[1,1,-3],

  (*word [-1, 0, -1, 1, 1]*)
  mzv[1,-1,2,-1] -> 3/4*Log[2]^3*mzv[2]-49/16*Log[2]^2*mzv[3]+43/40*Log[2]*mzv[2]^2+4*Log[2]*mzv[1,-3]-45/16*mzv[2]*mzv[3]+145/32*mzv[5]+3*mzv[1,1,-3],

  (*word [-1, 0, 0, -1, -1]*)
  -mzv[1,3,-1] -> Log[2]*mzv[1,-3]-1/2*mzv[2]*mzv[3]+59/64*mzv[5]+mzv[1,1,-3],

  (*word [-1, 0, 0, -1, 1]*)
  -mzv[-1,3,-1] -> 7/4*Log[2]^2*mzv[3]-33/40*Log[2]*mzv[2]^2-2*Log[2]*mzv[1,-3]+mzv[2]*mzv[3]-21/16*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, 0, 0, 0, -1]*)
  mzv[4,-1] -> 7/20*Log[2]*mzv[2]^2+3/8*mzv[2]*mzv[3]-17/16*mzv[5],

  (*word [-1, 0, 0, 0, 1]*)
  mzv[-4,-1] -> -2/5*Log[2]*mzv[2]^2-3/4*mzv[2]*mzv[3]+59/32*mzv[5],

  (*word [-1, 0, 0, 1, -1]*)
  -mzv[-1,-3,-1] -> -7/4*Log[2]^2*mzv[3]+3/4*Log[2]*mzv[2]^2+Log[2]*mzv[1,-3]-15/32*mzv[5],

  (*word [-1, 0, 0, 1, 1]*)
  -mzv[1,-3,-1] -> 1/10*Log[2]*mzv[2]^2-3/8*mzv[2]*mzv[3]+9/16*mzv[5]-mzv[1,1,-3],

  (*word [-1, 0, 1, -1, -1]*)
  mzv[1,-1,-2,-1] -> -3/4*Log[2]^3*mzv[2]+21/16*Log[2]^2*mzv[3]-3/20*Log[2]*mzv[2]^2-5/2*Log[2]*mzv[1,-3]+7/4*mzv[2]*mzv[3]-211/64*mzv[5]-7/2*mzv[1,1,-3],

  (*word [-1, 0, 1, -1, 1]*)
  mzv[-1,-1,-2,-1] -> -3/2*Log[2]^3*mzv[2]+7/2*Log[2]^2*mzv[3]-3/5*Log[2]*mzv[2]^2-11/2*Log[2]*mzv[1,-3]+65/16*mzv[2]*mzv[3]-241/32*mzv[5]-11/2*mzv[1,1,-3],

  (*word [-1, 0, 1, 0, -1]*)
  -mzv[-2,-2,-1] -> -11/40*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-27/16*mzv[2]*mzv[3]+57/16*mzv[5]+4*mzv[1,1,-3],

  (*word [-1, 0, 1, 0, 1]*)
  -mzv[2,-2,-1] -> 3/10*Log[2]*mzv[2]^2+1/8*mzv[2]*mzv[3]-43/64*mzv[5]+2*mzv[1,1,-3],

  (*word [-1, 0, 1, 1, -1]*)
  mzv[-1,1,-2,-1] -> 3/4*Log[2]^3*mzv[2]-7/16*Log[2]^2*mzv[3]-9/40*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-7/4*mzv[2]*mzv[3]+113/32*mzv[5]+7/2*mzv[1,1,-3],

  (*word [-1, 0, 1, 1, 1]*)
  mzv[1,1,-2,-1] -> -2/5*Log[2]*mzv[2]^2+3/16*mzv[2]*mzv[3]+21/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [-1, 1, -1, -1, -1]*)
  -mzv[1,1,-1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+1/8*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]-mzv[2]*mzv[3]+31/16*mzv[5]+2*mzv[1,1,-3],

  (*word [-1, 1, -1, -1, 1]*)
  -mzv[-1,1,-1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+1/8*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]-35/16*mzv[2]*mzv[3]+135/32*mzv[5]+3/2*mzv[1,1,-3],

  (*word [-1, 1, -1, 0, -1]*)
  mzv[2,-1,-1,-1] -> 1/12*Log[2]^3*mzv[2]-1/8*Log[2]^2*mzv[3]-3/20*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-7/4*mzv[2]*mzv[3]+7/2*mzv[5]+4*mzv[1,1,-3],

  (*word [-1, 1, -1, 0, 1]*)
  mzv[-2,-1,-1,-1] -> -1/6*Log[2]^3*mzv[2]+5/16*Log[2]^2*mzv[3]+1/10*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]-5/8*mzv[2]*mzv[3]+mzv[5]-5/2*mzv[1,1,-3],

  (*word [-1, 1, -1, 1, -1]*)
  -mzv[-1,-1,-1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+1/8*Log[2]^2*mzv[3]+1/40*Log[2]*mzv[2]^2-1/8*mzv[2]*mzv[3]+3/16*mzv[5],

  (*word [-1, 1, -1, 1, 1]*)
  -mzv[1,-1,-1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+1/8*Log[2]^2*mzv[3]+1/40*Log[2]*mzv[2]^2-11/16*mzv[2]*mzv[3]+19/16*mzv[5]-3/2*mzv[1,1,-3],

  (*word [-1, 1, 0, -1, -1]*)
  mzv[1,-2,-1,-1] -> 1/16*Log[2]^2*mzv[3]-3/40*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]-13/16*mzv[2]*mzv[3]+99/64*mzv[5]+3/2*mzv[1,1,-3],

  (*word [-1, 1, 0, -1, 1]*)
  mzv[-1,-2,-1,-1] -> 3/4*Log[2]^3*mzv[2]-27/16*Log[2]^2*mzv[3]+9/2*Log[2]*mzv[1,-3]-15/4*mzv[2]*mzv[3]+241/32*mzv[5]+13/2*mzv[1,1,-3],

  (*word [-1, 1, 0, 0, -1]*)
  -mzv[-3,-1,-1] -> 3/8*Log[2]^2*mzv[3]-1/8*Log[2]*mzv[2]^2+1/8*mzv[2]*mzv[3]-15/32*mzv[5],

  (*word [-1, 1, 0, 0, 1]*)
  -mzv[3,-1,-1] -> -1/2*Log[2]^2*mzv[3]+3/20*Log[2]*mzv[2]^2+Log[2]*mzv[1,-3]-15/8*mzv[2]*mzv[3]+245/64*mzv[5]+mzv[1,1,-3],

  (*word [-1, 1, 0, 1, -1]*)
  mzv[-1,2,-1,-1] -> -3/4*Log[2]^3*mzv[2]+11/8*Log[2]^2*mzv[3]+3/20*Log[2]*mzv[2]^2-9/2*Log[2]*mzv[1,-3]+7/2*mzv[2]*mzv[3]-113/16*mzv[5]-8*mzv[1,1,-3],

  (*word [-1, 1, 0, 1, 1]*)
  mzv[1,2,-1,-1] -> 1/2*Log[2]^2*mzv[3]-1/4*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]-3/8*mzv[2]*mzv[3]+51/64*mzv[5]-2*mzv[1,1,-3],

  (*word [-1, 1, 1, -1, -1]*)
  -mzv[1,-1,1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+7/16*Log[2]^2*mzv[3]-1/8*Log[2]*mzv[2]^2+1/16*mzv[2]*mzv[3]-3/64*mzv[5],

  (*word [-1, 1, 1, -1, 1]*)
  -mzv[-1,-1,1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+7/16*Log[2]^2*mzv[3]-1/8*Log[2]*mzv[2]^2+1/8*mzv[2]*mzv[3]-9/32*mzv[5]+3/2*mzv[1,1,-3],

  (*word [-1, 1, 1, 0, -1]*)
  mzv[-2,1,-1,-1] -> 1/12*Log[2]^3*mzv[2]-1/8*Log[2]^2*mzv[3]-3/20*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-13/8*mzv[2]*mzv[3]+113/32*mzv[5]+7/2*mzv[1,1,-3],

  (*word [-1, 1, 1, 0, 1]*)
  mzv[2,1,-1,-1] -> -1/6*Log[2]^3*mzv[2]+5/16*Log[2]^2*mzv[3]+1/10*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+1/16*mzv[2]*mzv[3]-43/64*mzv[5]+mzv[1,1,-3],

  (*word [-1, 1, 1, 1, -1]*)
  -mzv[-1,1,1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+7/16*Log[2]^2*mzv[3]-3/8*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+1/16*mzv[2]*mzv[3]+3/16*mzv[5]-mzv[1,1,-3],

  (*word [-1, 1, 1, 1, 1]*)
  -mzv[1,1,1,-1,-1] -> 1/120*Log[2]^5-1/12*Log[2]^3*mzv[2]+7/16*Log[2]^2*mzv[3]-3/8*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+1/4*mzv[2]*mzv[3]+33/64*mzv[5]-1/2*mzv[1,1,-3],

  (*word [0, -1, -1, -1, -1]*)
  mzv[1,1,1,-2] -> -1/4*mzv[2]*mzv[3]+31/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [0, -1, -1, -1, 1]*)
  mzv[-1,1,1,-2] -> 1/4*Log[2]^3*mzv[2]-7/8*Log[2]^2*mzv[3]+13/40*Log[2]*mzv[2]^2+1/2*Log[2]*mzv[1,-3]+5/8*mzv[2]*mzv[3]-93/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [0, -1, -1, 0, -1]*)
  -mzv[2,1,-2] -> -23/16*mzv[2]*mzv[3]+177/64*mzv[5],

  (*word [0, -1, -1, 0, 1]*)
  -mzv[-2,1,-2] -> 5/16*mzv[2]*mzv[3]-5/8*mzv[5],

  (*word [0, -1, -1, 1, -1]*)
  mzv[-1,-1,1,-2] -> -3/4*Log[2]^3*mzv[2]+35/16*Log[2]^2*mzv[3]-13/20*Log[2]*mzv[2]^2-Log[2]*mzv[1,-3]-25/16*mzv[2]*mzv[3]+217/64*mzv[5]+1/2*mzv[1,1,-3],

  (*word [0, -1, -1, 1, 1]*)
  mzv[1,-1,1,-2] -> -1/2*Log[2]^3*mzv[2]+35/16*Log[2]^2*mzv[3]-7/8*Log[2]*mzv[2]^2-5/2*Log[2]*mzv[1,-3]+17/16*mzv[2]*mzv[3]-81/64*mzv[5]-5/2*mzv[1,1,-3],

  (*word [0, -1, 0, -1, -1]*)
  -mzv[1,2,-2] -> 1/2*mzv[2]*mzv[3]-59/64*mzv[5]+2*mzv[1,1,-3],

  (*word [0, -1, 0, -1, 1]*)
  -mzv[-1,2,-2] -> 1/5*Log[2]*mzv[2]^2-2*Log[2]*mzv[1,-3]+3*mzv[2]*mzv[3]-6*mzv[5]-2*mzv[1,1,-3],

  (*word [0, -1, 0, 0, -1]*)
  mzv[3,-2] -> -3/4*mzv[2]*mzv[3]+51/32*mzv[5],

  (*word [0, -1, 0, 0, 1]*)
  mzv[-3,-2] -> 21/8*mzv[2]*mzv[3]-83/16*mzv[5],

  (*word [0, -1, 0, 1, -1]*)
  -mzv[-1,-2,-2] -> -1/5*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-25/8*mzv[2]*mzv[3]+199/32*mzv[5]+4*mzv[1,1,-3],

  (*word [0, -1, 0, 1, 1]*)
  -mzv[1,-2,-2] -> 11/16*mzv[2]*mzv[3]-41/32*mzv[5],

  (*word [0, -1, 1, -1, -1]*)
  mzv[1,-1,-1,-2] -> 3/4*Log[2]^3*mzv[2]-7/4*Log[2]^2*mzv[3]+19/40*Log[2]*mzv[2]^2-Log[2]*mzv[1,-3]+41/16*mzv[2]*mzv[3]-329/64*mzv[5]-5/2*mzv[1,1,-3],

  (*word [0, -1, 1, -1, 1]*)
  mzv[-1,-1,-1,-2] -> Log[2]^3*mzv[2]-21/8*Log[2]^2*mzv[3]+3/5*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]+15/16*mzv[2]*mzv[3]-2*mzv[5]+3/2*mzv[1,1,-3],

  (*word [0, -1, 1, 0, -1]*)
  -mzv[-2,-1,-2] -> 2/5*Log[2]*mzv[2]^2-4*Log[2]*mzv[1,-3]+69/16*mzv[2]*mzv[3]-283/32*mzv[5]-8*mzv[1,1,-3],

  (*word [0, -1, 1, 0, 1]*)
  -mzv[2,-1,-2] -> -3/8*Log[2]*mzv[2]^2+5/2*mzv[2]*mzv[3]-257/64*mzv[5],

  (*word [0, -1, 1, 1, -1]*)
  mzv[-1,1,-1,-2] -> -21/16*Log[2]^2*mzv[3]+13/20*Log[2]*mzv[2]^2+Log[2]*mzv[1,-3]-1/4*mzv[2]*mzv[3]+1/2*mzv[1,1,-3],

  (*word [0, -1, 1, 1, 1]*)
  mzv[1,1,-1,-2] -> 1/4*Log[2]^3*mzv[2]-21/16*Log[2]^2*mzv[3]+39/40*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]+1/8*mzv[2]*mzv[3]-97/64*mzv[5]+3/2*mzv[1,1,-3],

  (*word [0, 0, -1, -1, 1]*)
  -mzv[-1,1,-3] -> -7/8*Log[2]^2*mzv[3]+11/20*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-7/4*mzv[2]*mzv[3]+87/32*mzv[5]+2*mzv[1,1,-3],

  (*word [0, 0, -1, 0, -1]*)
  mzv[2,-3] -> -5/8*mzv[2]*mzv[3]+41/32*mzv[5],

  (*word [0, 0, -1, 0, 1]*)
  mzv[-2,-3] -> -9/4*mzv[2]*mzv[3]+67/16*mzv[5],

  (*word [0, 0, -1, 1, -1]*)
  -mzv[-1,-1,-3] -> 7/4*Log[2]^2*mzv[3]-41/40*Log[2]*mzv[2]^2-Log[2]*mzv[1,-3]-1/2*mzv[2]*mzv[3]+15/8*mzv[5],

  (*word [0, 0, -1, 1, 1]*)
  -mzv[1,-1,-3] -> 7/8*Log[2]^2*mzv[3]-13/20*Log[2]*mzv[2]^2-Log[2]*mzv[1,-3]-9/8*mzv[2]*mzv[3]+93/32*mzv[5]-mzv[1,1,-3],

  (*word [0, 0, 0, -1, -1]*)
  mzv[1,-4] -> 1/2*mzv[2]*mzv[3]-29/32*mzv[5],

  (*word [0, 0, 0, -1, 1]*)
  mzv[-1,-4] -> 3/4*Log[2]*mzv[2]^2+3/4*mzv[2]*mzv[3]-91/32*mzv[5],

  (*word [0, 0, 0, 0, -1]*)
  -mzv[-5] -> 15/16*mzv[5],

  (*word [0, 0, 0, 1, -1]*)
  mzv[-1,4] -> -3/4*Log[2]*mzv[2]^2-3/8*mzv[2]*mzv[3]+2*mzv[5],

  (*word [0, 0, 0, 1, 1]*)
  mzv[1,4] -> -mzv[2]*mzv[3]+2*mzv[5],

  (*word [0, 0, 1, -1, -1]*)
  -mzv[1,-1,3] -> -7/8*Log[2]^2*mzv[3]+19/40*Log[2]*mzv[2]^2-Log[2]*mzv[1,-3]+29/16*mzv[2]*mzv[3]-61/16*mzv[5]-2*mzv[1,1,-3],

  (*word [0, 0, 1, -1, 1]*)
  -mzv[-1,-1,3] -> -7/4*Log[2]^2*mzv[3]+41/40*Log[2]*mzv[2]^2+Log[2]*mzv[1,-3]+11/8*mzv[2]*mzv[3]-225/64*mzv[5]+mzv[1,1,-3],

  (*word [0, 0, 1, 0, -1]*)
  mzv[-2,3] -> 1/4*mzv[2]*mzv[3]-21/32*mzv[5],

  (*word [0, 0, 1, 0, 1]*)
  mzv[2,3] -> 3*mzv[2]*mzv[3]-11/2*mzv[5],

  (*word [0, 0, 1, 1, -1]*)
  -mzv[-1,1,3] -> 7/8*Log[2]^2*mzv[3]-3/8*Log[2]*mzv[2]^2-9/16*mzv[2]*mzv[3]+85/64*mzv[5]+mzv[1,1,-3],

  (*word [0, 0, 1, 1, 1]*)
  -mzv[1,1,3] -> mzv[2]*mzv[3]-2*mzv[5],

  (*word [0, 1, -1, -1, -1]*)
  mzv[1,1,-1,2] -> -1/4*Log[2]^3*mzv[2]+7/16*Log[2]^2*mzv[3]-3/20*Log[2]*mzv[2]^2+3/2*Log[2]*mzv[1,-3]-23/16*mzv[2]*mzv[3]+45/16*mzv[5]+2*mzv[1,1,-3],

  (*word [0, 1, -1, -1, 1]*)
  mzv[-1,1,-1,2] -> -7/16*Log[2]^2*mzv[3]+7/40*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-37/16*mzv[2]*mzv[3]+273/64*mzv[5]+2*mzv[1,1,-3],

  (*word [0, 1, -1, 0, -1]*)
  -mzv[2,-1,2] -> -2/5*Log[2]*mzv[2]^2+4*Log[2]*mzv[1,-3]-15/4*mzv[2]*mzv[3]+243/32*mzv[5]+8*mzv[1,1,-3],

  (*word [0, 1, -1, 0, 1]*)
  -mzv[-2,-1,2] -> 3/8*Log[2]*mzv[2]^2-13/4*mzv[2]*mzv[3]+363/64*mzv[5],

  (*word [0, 1, -1, 1, -1]*)
  mzv[-1,-1,-1,2] -> -Log[2]^3*mzv[2]+21/8*Log[2]^2*mzv[3]-3/5*Log[2]*mzv[2]^2-3/2*Log[2]*mzv[1,-3]-3/8*mzv[2]*mzv[3]+mzv[5],

  (*word [0, 1, -1, 1, 1]*)
  mzv[1,-1,-1,2] -> -3/4*Log[2]^3*mzv[2]+21/8*Log[2]^2*mzv[3]-33/40*Log[2]*mzv[2]^2-3*Log[2]*mzv[1,-3]-9/8*mzv[2]*mzv[3]+169/64*mzv[5]-3*mzv[1,1,-3],

  (*word [0, 1, 0, -1, -1]*)
  -mzv[1,-2,2] -> 9/16*mzv[2]*mzv[3]-37/32*mzv[5]-2*mzv[1,1,-3],

  (*word [0, 1, 0, -1, 1]*)
  -mzv[-1,-2,2] -> -23/40*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]-1/2*mzv[2]*mzv[3]+127/64*mzv[5]+2*mzv[1,1,-3],

  (*word [0, 1, 0, 0, -1]*)
  mzv[-3,2] -> -1/8*mzv[2]*mzv[3]-11/32*mzv[5],

  (*word [0, 1, 0, 0, 1]*)
  mzv[3,2] -> -2*mzv[2]*mzv[3]+9/2*mzv[5],

  (*word [0, 1, 0, 1, -1]*)
  -mzv[-1,2,2] -> 23/40*Log[2]*mzv[2]^2-2*Log[2]*mzv[1,-3]+9/8*mzv[2]*mzv[3]-95/32*mzv[5]-4*mzv[1,1,-3],

  (*word [0, 1, 0, 1, 1]*)
  -mzv[1,2,2] -> -3*mzv[2]*mzv[3]+11/2*mzv[5],

  (*word [0, 1, 1, -1, -1]*)
  mzv[1,-1,1,2] -> 1/2*Log[2]^3*mzv[2]-7/16*Log[2]^2*mzv[3]+1/20*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]+19/16*mzv[2]*mzv[3]-145/64*mzv[5]-2*mzv[1,1,-3],

  (*word [0, 1, 1, -1, 1]*)
  mzv[-1,-1,1,2] -> 3/4*Log[2]^3*mzv[2]-21/16*Log[2]^2*mzv[3]+7/40*Log[2]*mzv[2]^2+2*Log[2]*mzv[1,-3]+19/16*mzv[2]*mzv[3]-161/64*mzv[5]+2*mzv[1,1,-3],

  (*word [0, 1, 1, 0, -1]*)
  -mzv[-2,1,2] -> -3/16*mzv[2]*mzv[3]+53/64*mzv[5],

  (*word [0, 1, 1, 0, 1]*)
  -mzv[2,1,2] -> 2*mzv[2]*mzv[3]-9/2*mzv[5],

  (*word [0, 1, 1, 1, -1]*)
  mzv[-1,1,1,2] -> -1/4*Log[2]^3*mzv[2]-13/40*Log[2]*mzv[2]^2-1/2*Log[2]*mzv[1,-3]-3/8*mzv[2]*mzv[3]+17/16*mzv[5]-mzv[1,1,-3],

  (*word [0, 1, 1, 1, 1]*)
  mzv[1,1,1,2] -> mzv[5],

  (*word [-1, -1, -1, -1, -1, -1]*)
  mzv[1,1,1,1,1,-1] -> 1/720*Log[2]^6,

  (*word [-1, -1, -1, -1, -1, 1]*)
  mzv[-1,1,1,1,1,-1] -> 1/720*Log[2]^6-11/280*mzv[2]^3+1/8*mzv[3]^2-1/4*mzv[1,-5]+1/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, -1, 0, -1]*)
  -mzv[2,1,1,1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+11/56*mzv[2]^3-5/8*mzv[3]^2+5/4*mzv[1,-5]-5/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, -1, 0, 1]*)
  -mzv[-2,1,1,1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2-3/16*Log[2]*mzv[2]*mzv[3]+25/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]+81/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-63/128*mzv[3]^2+mzv[1,-5]-mzv[1,1,1,-3],

  (*word [-1, -1, -1, -1, 1, -1]*)
  mzv[-1,-1,1,1,1,-1] -> 1/720*Log[2]^6+1/4*Log[2]*mzv[2]*mzv[3]-31/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]+11/56*mzv[2]^3-5/8*mzv[3]^2+5/4*mzv[1,-5]-5/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, -1, 1, 1]*)
  mzv[1,-1,1,1,1,-1] -> 1/720*Log[2]^6+1/4*Log[2]*mzv[2]*mzv[3]-31/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]+11/70*mzv[2]^3-1/2*mzv[3]^2+mzv[1,-5]-mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, -1, -1]*)
  -mzv[1,2,1,1,-1] -> 1/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-3/2*Log[2]*mzv[2]*mzv[3]+93/32*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-11/28*mzv[2]^3+5/4*mzv[3]^2-5/2*mzv[1,-5]+5*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, -1, 1]*)
  -mzv[-1,2,1,1,-1] -> 1/4*Log[2]^4*mzv[2]-17/24*Log[2]^3*mzv[3]+11/40*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]+5/2*Log[2]*mzv[2]*mzv[3]-323/64*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]+2/7*mzv[2]^3-mzv[2]*mzv[1,-3]-51/64*mzv[3]^2+5/2*mzv[1,-5]+1/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, 0, -1]*)
  mzv[3,1,1,-1] -> 1/8*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2-7/16*Log[2]*mzv[2]*mzv[3]+59/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-263/1680*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]+15/32*mzv[3]^2-5/4*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, 0, 1]*)
  mzv[-3,1,1,-1] -> -1/6*Log[2]^3*mzv[3]+3/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-1/4*Log[2]*mzv[2]*mzv[3]+3/8*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-61/120*mzv[2]^3-mzv[2]*mzv[1,-3]+111/64*mzv[3]^2-3*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, 1, -1]*)
  -mzv[-1,-2,1,1,-1] -> -1/4*Log[2]^4*mzv[2]+29/48*Log[2]^3*mzv[3]-1/5*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-31/16*Log[2]*mzv[2]*mzv[3]+31/8*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-121/140*mzv[2]^3+177/64*mzv[3]^2-13/2*mzv[1,-5]+7/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 0, 1, 1]*)
  -mzv[1,-2,1,1,-1] -> 1/6*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/4*Log[2]*mzv[2]*mzv[3]-17/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]-71/560*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+49/128*mzv[3]^2,

  (*word [-1, -1, -1, 1, -1, -1]*)
  mzv[1,-1,-1,1,1,-1] -> 1/720*Log[2]^6-1/80*Log[2]^2*mzv[2]^2+1/4*Log[2]^2*mzv[1,-3]-Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-11/28*mzv[2]^3+5/4*mzv[3]^2-5/2*mzv[1,-5]+5*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 1, -1, 1]*)
  mzv[-1,-1,-1,1,1,-1] -> 1/720*Log[2]^6-1/80*Log[2]^2*mzv[2]^2+1/4*Log[2]^2*mzv[1,-3]-Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-121/210*mzv[2]^3+59/32*mzv[3]^2-4*mzv[1,-5]+7/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 1, 0, -1]*)
  -mzv[-2,-1,1,1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+367/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-137/64*mzv[3]^2+21/4*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [-1, -1, -1, 1, 0, 1]*)
  -mzv[2,-1,1,1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2-3/16*Log[2]*mzv[2]*mzv[3]+25/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]+169/280*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]-31/16*mzv[3]^2+23/4*mzv[1,-5]-7/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 1, 1, -1]*)
  mzv[-1,1,-1,1,1,-1] -> 1/720*Log[2]^6-1/80*Log[2]^2*mzv[2]^2+1/4*Log[2]^2*mzv[1,-3]-3/4*Log[2]*mzv[2]*mzv[3]+93/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-11/210*mzv[2]^3+5/32*mzv[3]^2+1/2*mzv[1,1,1,-3],

  (*word [-1, -1, -1, 1, 1, 1]*)
  mzv[1,1,-1,1,1,-1] -> 1/720*Log[2]^6-1/80*Log[2]^2*mzv[2]^2+1/4*Log[2]^2*mzv[1,-3]-3/4*Log[2]*mzv[2]*mzv[3]+93/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-23/140*mzv[2]^3+1/2*mzv[3]^2,

  (*word [-1, -1, 0, -1, -1, -1]*)
  -mzv[1,1,2,1,-1] -> 1/80*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+11/28*mzv[2]^3-5/4*mzv[3]^2+5/2*mzv[1,-5]-5*mzv[1,1,1,-3],

  (*word [-1, -1, 0, -1, -1, 1]*)
  -mzv[-1,1,2,1,-1] -> -3/8*Log[2]^4*mzv[2]+21/16*Log[2]^3*mzv[3]-11/20*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-25/8*Log[2]*mzv[2]*mzv[3]+211/32*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-71/140*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+45/32*mzv[3]^2-15/4*mzv[1,-5],

  (*word [-1, -1, 0, -1, 0, -1]*)
  mzv[2,2,1,-1] -> 1/16*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+15/8*Log[2]*mzv[2]*mzv[3]-59/16*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+263/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-91/64*mzv[3]^2+15/4*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [-1, -1, 0, -1, 0, 1]*)
  mzv[-2,2,1,-1] -> -3/80*Log[2]^2*mzv[2]^2-1/2*Log[2]*mzv[2]*mzv[3]+33/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+411/560*mzv[2]^3+5/2*mzv[2]*mzv[1,-3]-331/128*mzv[3]^2+13/4*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [-1, -1, 0, -1, 1, -1]*)
  -mzv[-1,-1,2,1,-1] -> 3/4*Log[2]^4*mzv[2]-35/16*Log[2]^3*mzv[3]+27/40*Log[2]^2*mzv[2]^2+5/2*Log[2]^2*mzv[1,-3]+1/16*Log[2]*mzv[2]*mzv[3]-49/64*Log[2]*mzv[5]+5/2*Log[2]*mzv[1,1,-3]+11/70*mzv[2]^3-27/64*mzv[3]^2-3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 0, -1, 1, 1]*)
  -mzv[1,-1,2,1,-1] -> 3/8*Log[2]^4*mzv[2]-7/4*Log[2]^3*mzv[3]+69/80*Log[2]^2*mzv[2]^2+5/2*Log[2]^2*mzv[1,-3]-7/4*Log[2]*mzv[2]*mzv[3]+133/64*Log[2]*mzv[5]+7/2*Log[2]*mzv[1,1,-3]+23/280*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-3/32*mzv[3]^2-3*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 0, -1, -1]*)
  mzv[1,3,1,-1] -> 1/2*Log[2]^2*mzv[1,-3]-1/2*Log[2]*mzv[2]*mzv[3]+59/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]+1/128*mzv[3]^2,

  (*word [-1, -1, 0, 0, -1, 1]*)
  mzv[-1,3,1,-1] -> 7/8*Log[2]^3*mzv[3]-11/20*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]+13/16*Log[2]*mzv[2]*mzv[3]-49/64*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+437/560*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-343/128*mzv[3]^2+13/2*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 0, 0, -1]*)
  -mzv[4,1,-1] -> 7/40*Log[2]^2*mzv[2]^2+3/8*Log[2]*mzv[2]*mzv[3]-17/16*Log[2]*mzv[5]-13/42*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+71/64*mzv[3]^2-5/2*mzv[1,-5],

  (*word [-1, -1, 0, 0, 0, 1]*)
  -mzv[-4,1,-1] -> -1/5*Log[2]^2*mzv[2]^2-3/4*Log[2]*mzv[2]*mzv[3]+59/32*Log[2]*mzv[5]+11/168*mzv[2]^3+mzv[2]*mzv[1,-3]-3/8*mzv[3]^2,

  (*word [-1, -1, 0, 0, 1, -1]*)
  mzv[-1,-3,1,-1] -> -7/8*Log[2]^3*mzv[3]+41/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]+3/16*Log[2]*mzv[2]*mzv[3]-65/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]+3/280*mzv[2]^3+1/16*mzv[3]^2-3/4*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [-1, -1, 0, 0, 1, 1]*)
  mzv[1,-3,1,-1] -> 1/20*Log[2]^2*mzv[2]^2-3/8*Log[2]*mzv[2]*mzv[3]+9/16*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]+13/20*mzv[2]^3+mzv[2]*mzv[1,-3]-17/8*mzv[3]^2+3*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 1, -1, -1]*)
  -mzv[1,-1,-2,1,-1] -> -3/8*Log[2]^4*mzv[2]+7/8*Log[2]^3*mzv[3]-13/80*Log[2]^2*mzv[2]^2-9/4*Log[2]^2*mzv[1,-3]+5/2*Log[2]*mzv[2]*mzv[3]-149/32*Log[2]*mzv[5]-5*Log[2]*mzv[1,1,-3]+341/280*mzv[2]^3-63/16*mzv[3]^2+39/4*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 1, -1, 1]*)
  -mzv[-1,-1,-2,1,-1] -> -3/4*Log[2]^4*mzv[2]+35/16*Log[2]^3*mzv[3]-5/8*Log[2]^2*mzv[2]^2-13/4*Log[2]^2*mzv[1,-3]+2*Log[2]*mzv[2]*mzv[3]-209/64*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+223/560*mzv[2]^3-171/128*mzv[3]^2+15/4*mzv[1,-5]-3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 1, 0, -1]*)
  mzv[-2,-2,1,-1] -> -11/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-27/16*Log[2]*mzv[2]*mzv[3]+57/16*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-79/80*mzv[2]^3+199/64*mzv[3]^2-15/2*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 1, 0, 1]*)
  mzv[2,-2,1,-1] -> 3/20*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-43/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-37/70*mzv[2]^3-mzv[2]*mzv[1,-3]+241/128*mzv[3]^2-13/4*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [-1, -1, 0, 1, 1, -1]*)
  -mzv[-1,1,-2,1,-1] -> 3/8*Log[2]^4*mzv[2]-7/16*Log[2]^3*mzv[3]-9/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-3/4*Log[2]*mzv[2]*mzv[3]+55/32*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-1/10*mzv[2]^3+9/32*mzv[3]^2-3/4*mzv[1,-5],

  (*word [-1, -1, 0, 1, 1, 1]*)
  -mzv[1,1,-2,1,-1] -> -1/5*Log[2]^2*mzv[2]^2+3/16*Log[2]*mzv[2]*mzv[3]+21/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-111/560*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+63/128*mzv[3]^2-mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, -1, -1]*)
  mzv[1,1,-1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+3/2*Log[2]*mzv[2]*mzv[3]-93/32*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+11/28*mzv[2]^3-5/4*mzv[3]^2+5/2*mzv[1,-5]-5*mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, -1, 1]*)
  mzv[-1,1,-1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+3/2*Log[2]*mzv[2]*mzv[3]-93/32*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+11/14*mzv[2]^3-323/128*mzv[3]^2+6*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, 0, -1]*)
  -mzv[2,-1,-1,1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+27/16*Log[2]*mzv[2]*mzv[3]-211/64*Log[2]*mzv[5]-7/2*Log[2]*mzv[1,1,-3]+263/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-91/64*mzv[3]^2+15/4*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, 0, 1]*)
  -mzv[-2,-1,-1,1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+7/16*Log[2]*mzv[2]*mzv[3]-47/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-8/7*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+29/8*mzv[3]^2-45/4*mzv[1,-5]+15/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, 1, -1]*)
  mzv[-1,-1,-1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+9/4*Log[2]*mzv[2]*mzv[3]-69/16*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]+11/70*mzv[2]^3-31/64*mzv[3]^2-3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, -1, 1, 1]*)
  mzv[1,-1,-1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+9/4*Log[2]*mzv[2]*mzv[3]-69/16*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]+29/60*mzv[2]^3-191/128*mzv[3]^2,

  (*word [-1, -1, 1, 0, -1, -1]*)
  -mzv[1,-2,-1,1,-1] -> 1/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-3/2*Log[2]*mzv[2]*mzv[3]+93/32*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-341/280*mzv[2]^3+251/64*mzv[3]^2-39/4*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 0, -1, 1]*)
  -mzv[-1,-2,-1,1,-1] -> 1/4*Log[2]^4*mzv[2]-17/24*Log[2]^3*mzv[3]+11/40*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]+41/16*Log[2]*mzv[2]*mzv[3]-169/32*Log[2]*mzv[5]-11/2*Log[2]*mzv[1,1,-3]+673/280*mzv[2]^3-491/64*mzv[3]^2+19*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 0, 0, -1]*)
  mzv[-3,-1,1,-1] -> 1/8*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2-7/16*Log[2]*mzv[2]*mzv[3]+59/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-391/336*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+241/64*mzv[3]^2-37/4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 0, 0, 1]*)
  mzv[3,-1,1,-1] -> -1/6*Log[2]^3*mzv[3]+3/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-1/4*Log[2]*mzv[2]*mzv[3]+3/8*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-169/336*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+209/128*mzv[3]^2-5*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 0, 1, -1]*)
  -mzv[-1,2,-1,1,-1] -> -1/4*Log[2]^4*mzv[2]+29/48*Log[2]^3*mzv[3]-1/5*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-2*Log[2]*mzv[2]*mzv[3]+263/64*Log[2]*mzv[5]+7*Log[2]*mzv[1,1,-3]-43/14*mzv[2]^3+631/64*mzv[3]^2-25*mzv[1,-5]+15*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 0, 1, 1]*)
  -mzv[1,2,-1,1,-1] -> 1/6*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/4*Log[2]*mzv[2]*mzv[3]-17/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]+47/140*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-145/128*mzv[3]^2-3/4*mzv[1,-5]-1/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 1, -1, -1]*)
  mzv[1,-1,1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+1/40*Log[2]^2*mzv[2]^2-3/64*Log[2]*mzv[5]+1/128*mzv[3]^2,

  (*word [-1, -1, 1, 1, -1, 1]*)
  mzv[-1,-1,1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+1/40*Log[2]^2*mzv[2]^2-3/64*Log[2]*mzv[5]+23/56*mzv[2]^3-85/64*mzv[3]^2+15/4*mzv[1,-5]-3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 1, 0, -1]*)
  -mzv[-2,1,-1,1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-3/4*Log[2]^2*mzv[1,-3]+27/16*Log[2]*mzv[2]*mzv[3]-211/64*Log[2]*mzv[5]-7/2*Log[2]*mzv[1,1,-3]+263/280*mzv[2]^3-383/128*mzv[3]^2+7*mzv[1,-5]-11/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 1, 0, 1]*)
  -mzv[2,1,-1,1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+7/16*Log[2]*mzv[2]*mzv[3]-47/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-55/112*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+103/64*mzv[3]^2-13/4*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 1, 1, -1]*)
  mzv[-1,1,1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+1/40*Log[2]^2*mzv[2]^2+1/4*Log[2]*mzv[2]*mzv[3]-35/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-337/840*mzv[2]^3+169/128*mzv[3]^2-15/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [-1, -1, 1, 1, 1, 1]*)
  mzv[1,1,1,-1,1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^3*mzv[3]+1/40*Log[2]^2*mzv[2]^2+1/4*Log[2]*mzv[2]*mzv[3]-35/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]+1/70*mzv[2]^3-mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, 0, -1, -1, -1, -1]*)
  -mzv[1,1,1,2,-1] -> -1/4*Log[2]*mzv[2]*mzv[3]+31/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-11/56*mzv[2]^3+5/8*mzv[3]^2-5/4*mzv[1,-5]+5/2*mzv[1,1,1,-3],

  (*word [-1, 0, -1, -1, -1, 1]*)
  -mzv[-1,1,1,2,-1] -> 1/4*Log[2]^4*mzv[2]-49/48*Log[2]^3*mzv[3]+39/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]+31/16*Log[2]*mzv[2]*mzv[3]-279/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]+69/280*mzv[2]^3-mzv[2]*mzv[1,-3]-19/32*mzv[3]^2+5/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [-1, 0, -1, -1, 0, -1]*)
  mzv[2,1,2,-1] -> -23/16*Log[2]*mzv[2]*mzv[3]+177/64*Log[2]*mzv[5]-3/80*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+1/32*mzv[3]^2,

  (*word [-1, 0, -1, -1, 0, 1]*)
  mzv[-2,1,2,-1] -> 5/16*Log[2]*mzv[2]*mzv[3]-5/8*Log[2]*mzv[5]-57/560*mzv[2]^3-3/2*mzv[2]*mzv[1,-3]+59/128*mzv[3]^2+3/4*mzv[1,-5],

  (*word [-1, 0, -1, -1, 1, -1]*)
  -mzv[-1,-1,1,2,-1] -> -3/4*Log[2]^4*mzv[2]+21/8*Log[2]^3*mzv[3]-79/80*Log[2]^2*mzv[2]^2-13/4*Log[2]^2*mzv[1,-3]+1/8*Log[2]*mzv[2]*mzv[3]+55/64*Log[2]*mzv[5]-11/2*Log[2]*mzv[1,1,-3]+11/40*mzv[2]^3-33/32*mzv[3]^2+15/4*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [-1, 0, -1, -1, 1, 1]*)
  -mzv[1,-1,1,2,-1] -> -1/2*Log[2]^4*mzv[2]+119/48*Log[2]^3*mzv[3]-103/80*Log[2]^2*mzv[2]^2-4*Log[2]^2*mzv[1,-3]+57/16*Log[2]*mzv[2]*mzv[3]-309/64*Log[2]*mzv[5]-15/2*Log[2]*mzv[1,1,-3]+37/80*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-113/64*mzv[3]^2+7*mzv[1,-5]-11/2*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 0, -1, -1]*)
  mzv[1,2,2,-1] -> 1/2*Log[2]*mzv[2]*mzv[3]-59/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-121/280*mzv[2]^3+89/64*mzv[3]^2-15/4*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 0, -1, 1]*)
  mzv[-1,2,2,-1] -> 1/5*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+43/8*Log[2]*mzv[2]*mzv[3]-343/32*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+71/140*mzv[2]^3-2*mzv[2]*mzv[1,-3]-43/32*mzv[3]^2+4*mzv[1,-5],

  (*word [-1, 0, -1, 0, 0, -1]*)
  -mzv[3,2,-1] -> -3/4*Log[2]*mzv[2]*mzv[3]+51/32*Log[2]*mzv[5]+215/336*mzv[2]^3+mzv[2]*mzv[1,-3]-71/32*mzv[3]^2+5*mzv[1,-5],

  (*word [-1, 0, -1, 0, 0, 1]*)
  -mzv[-3,2,-1] -> 21/8*Log[2]*mzv[2]*mzv[3]-83/16*Log[2]*mzv[5]+5/21*mzv[2]^3-2*mzv[2]*mzv[1,-3]-33/64*mzv[3]^2+5/2*mzv[1,-5],

  (*word [-1, 0, -1, 0, 1, -1]*)
  mzv[-1,-2,2,-1] -> -1/5*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-11/2*Log[2]*mzv[2]*mzv[3]+175/16*Log[2]*mzv[5]+6*Log[2]*mzv[1,1,-3]-62/35*mzv[2]^3+179/32*mzv[3]^2-12*mzv[1,-5]+8*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 0, 1, 1]*)
  mzv[1,-2,2,-1] -> 11/16*Log[2]*mzv[2]*mzv[3]-41/32*Log[2]*mzv[5]-177/280*mzv[2]^3-mzv[2]*mzv[1,-3]+265/128*mzv[3]^2-2*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, -1, -1]*)
  -mzv[1,-1,-1,2,-1] -> 3/4*Log[2]^4*mzv[2]-35/16*Log[2]^3*mzv[3]+53/80*Log[2]^2*mzv[2]^2+11/4*Log[2]^2*mzv[1,-3]-19/16*Log[2]*mzv[2]*mzv[3]+53/32*Log[2]*mzv[5]+5*Log[2]*mzv[1,1,-3]-121/280*mzv[2]^3+93/64*mzv[3]^2-15/4*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, -1, 1]*)
  -mzv[-1,-1,-1,2,-1] -> Log[2]^4*mzv[2]-77/24*Log[2]^3*mzv[3]+19/20*Log[2]^2*mzv[2]^2+11/2*Log[2]^2*mzv[1,-3]-41/8*Log[2]*mzv[2]*mzv[3]+289/32*Log[2]*mzv[5]+11*Log[2]*mzv[1,1,-3]-79/40*mzv[2]^3+409/64*mzv[3]^2-31/2*mzv[1,-5]+11*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, 0, -1]*)
  mzv[-2,-1,2,-1] -> 2/5*Log[2]^2*mzv[2]^2-4*Log[2]^2*mzv[1,-3]+63/8*Log[2]*mzv[2]*mzv[3]-515/32*Log[2]*mzv[5]-16*Log[2]*mzv[1,1,-3]+221/40*mzv[2]^3+mzv[2]*mzv[1,-3]-565/32*mzv[3]^2+42*mzv[1,-5]-24*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, 0, 1]*)
  mzv[2,-1,2,-1] -> -3/8*Log[2]^2*mzv[2]^2+15/8*Log[2]*mzv[2]*mzv[3]-37/16*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+303/140*mzv[2]^3-233/32*mzv[3]^2+35/2*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, 1, -1]*)
  -mzv[-1,1,-1,2,-1] -> -21/16*Log[2]^3*mzv[3]+39/40*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]-21/16*Log[2]*mzv[2]*mzv[3]+Log[2]*mzv[5]+31/35*mzv[2]^3-171/64*mzv[3]^2+15/2*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, 0, -1, 1, 1, 1]*)
  -mzv[1,1,-1,2,-1] -> 1/4*Log[2]^4*mzv[2]-35/24*Log[2]^3*mzv[3]+49/40*Log[2]^2*mzv[2]^2+11/4*Log[2]^2*mzv[1,-3]-23/8*Log[2]*mzv[2]*mzv[3]+43/16*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-107/280*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]+7/4*mzv[3]^2-2*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [-1, 0, 0, -1, -1, -1]*)
  mzv[1,1,3,-1] -> -Log[2]*mzv[1,1,-3]+121/840*mzv[2]^3-15/32*mzv[3]^2+5/4*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [-1, 0, 0, -1, -1, 1]*)
  mzv[-1,1,3,-1] -> -7/8*Log[2]^3*mzv[3]+11/16*Log[2]^2*mzv[2]^2+5/2*Log[2]^2*mzv[1,-3]-13/4*Log[2]*mzv[2]*mzv[3]+317/64*Log[2]*mzv[5]+5*Log[2]*mzv[1,1,-3]-1147/1680*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+301/128*mzv[3]^2-6*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [-1, 0, 0, -1, 0, -1]*)
  -mzv[2,3,-1] -> -5/8*Log[2]*mzv[2]*mzv[3]+41/32*Log[2]*mzv[5]-215/336*mzv[2]^3+65/32*mzv[3]^2-5*mzv[1,-5],

  (*word [-1, 0, 0, -1, 0, 1]*)
  -mzv[-2,3,-1] -> -9/4*Log[2]*mzv[2]*mzv[3]+67/16*Log[2]*mzv[5]-85/168*mzv[2]^3+105/64*mzv[3]^2-5/2*mzv[1,-5],

  (*word [-1, 0, 0, -1, 1, -1]*)
  mzv[-1,-1,3,-1] -> 7/4*Log[2]^3*mzv[3]-13/10*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+1/2*Log[2]*mzv[2]*mzv[3]+33/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]-59/84*mzv[2]^3+2*mzv[3]^2-5*mzv[1,-5],

  (*word [-1, 0, 0, -1, 1, 1]*)
  mzv[1,-1,3,-1] -> 7/8*Log[2]^3*mzv[3]-63/80*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]+1/4*Log[2]*mzv[2]*mzv[3]+33/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]-871/1680*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+185/128*mzv[3]^2-5/2*mzv[1,-5],

  (*word [-1, 0, 0, 0, -1, -1]*)
  -mzv[1,4,-1] -> 1/2*Log[2]*mzv[2]*mzv[3]-29/32*Log[2]*mzv[5]+13/42*mzv[2]^3-65/64*mzv[3]^2+5/2*mzv[1,-5],

  (*word [-1, 0, 0, 0, -1, 1]*)
  -mzv[-1,4,-1] -> 3/4*Log[2]^2*mzv[2]^2+15/8*Log[2]*mzv[2]*mzv[3]-23/4*Log[2]*mzv[5]+13/15*mzv[2]^3-75/32*mzv[3]^2+4*mzv[1,-5],

  (*word [-1, 0, 0, 0, 0, -1]*)
  mzv[5,-1] -> 15/16*Log[2]*mzv[5]-7/40*mzv[2]^3+9/32*mzv[3]^2,

  (*word [-1, 0, 0, 0, 0, 1]*)
  mzv[-5,-1] -> -Log[2]*mzv[5]+23/70*mzv[2]^3-3/4*mzv[3]^2+mzv[1,-5],

  (*word [-1, 0, 0, 0, 1, -1]*)
  -mzv[-1,-4,-1] -> -3/4*Log[2]^2*mzv[2]^2-3/2*Log[2]*mzv[2]*mzv[3]+157/32*Log[2]*mzv[5]-613/840*mzv[2]^3+63/32*mzv[3]^2-4*mzv[1,-5],

  (*word [-1, 0, 0, 0, 1, 1]*)
  -mzv[1,-4,-1] -> -Log[2]*mzv[2]*mzv[3]+2*Log[2]*mzv[5]-49/120*mzv[2]^3+5/4*mzv[3]^2-3/2*mzv[1,-5],

  (*word [-1, 0, 0, 1, -1, -1]*)
  mzv[1,-1,-3,-1] -> -7/8*Log[2]^3*mzv[3]+49/80*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]+37/16*Log[2]*mzv[2]*mzv[3]-333/64*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+103/84*mzv[2]^3-247/64*mzv[3]^2+37/4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, 0, 0, 1, -1, 1]*)
  mzv[-1,-1,-3,-1] -> -7/4*Log[2]^3*mzv[3]+13/10*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]+3/8*Log[2]*mzv[2]*mzv[3]-171/64*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]+83/1680*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+5/32*mzv[3]^2-3/4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, 0, 0, 1, 0, -1]*)
  -mzv[-2,-3,-1] -> 1/4*Log[2]*mzv[2]*mzv[3]-21/32*Log[2]*mzv[5]+71/105*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-69/32*mzv[3]^2+5*mzv[1,-5],

  (*word [-1, 0, 0, 1, 0, 1]*)
  -mzv[2,-3,-1] -> 3*Log[2]*mzv[2]*mzv[3]-11/2*Log[2]*mzv[5]+451/840*mzv[2]^3-mzv[2]*mzv[1,-3]-109/64*mzv[3]^2+7/2*mzv[1,-5],

  (*word [-1, 0, 0, 1, 1, -1]*)
  mzv[-1,1,-3,-1] -> 7/8*Log[2]^3*mzv[3]-41/80*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]-15/16*Log[2]*mzv[2]*mzv[3]+151/64*Log[2]*mzv[5]-167/840*mzv[2]^3+37/64*mzv[3]^2-3/4*mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, 0, 0, 1, 1, 1]*)
  mzv[1,1,-3,-1] -> Log[2]*mzv[2]*mzv[3]-2*Log[2]*mzv[5]-29/210*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+33/64*mzv[3]^2+mzv[1,1,1,-3],

  (*word [-1, 0, 1, -1, -1, -1]*)
  -mzv[1,1,-1,-2,-1] -> -1/4*Log[2]^4*mzv[2]+7/12*Log[2]^3*mzv[3]-13/80*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-11/16*Log[2]*mzv[2]*mzv[3]+93/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-187/280*mzv[2]^3+137/64*mzv[3]^2-21/4*mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, 0, 1, -1, -1, 1]*)
  -mzv[-1,1,-1,-2,-1] -> -7/16*Log[2]^3*mzv[3]+13/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-1/4*Log[2]*mzv[2]*mzv[3]+1/2*Log[2]*mzv[1,1,-3]+1/56*mzv[2]^3,

  (*word [-1, 0, 1, -1, 0, -1]*)
  mzv[2,-1,-2,-1] -> -2/5*Log[2]^2*mzv[2]^2+4*Log[2]^2*mzv[1,-3]-117/16*Log[2]*mzv[2]*mzv[3]+475/32*Log[2]*mzv[5]+16*Log[2]*mzv[1,1,-3]-109/20*mzv[2]^3-mzv[2]*mzv[1,-3]+35/2*mzv[3]^2-42*mzv[1,-5]+24*mzv[1,1,1,-3],

  (*word [-1, 0, 1, -1, 0, 1]*)
  mzv[-2,-1,-2,-1] -> 3/8*Log[2]^2*mzv[2]^2-21/8*Log[2]*mzv[2]*mzv[3]+127/32*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-171/70*mzv[2]^3+1033/128*mzv[3]^2-19*mzv[1,-5]+12*mzv[1,1,1,-3],

  (*word [-1, 0, 1, -1, 1, -1]*)
  -mzv[-1,-1,-1,-2,-1] -> -Log[2]^4*mzv[2]+77/24*Log[2]^3*mzv[3]-19/20*Log[2]^2*mzv[2]^2-11/2*Log[2]^2*mzv[1,-3]+91/16*Log[2]*mzv[2]*mzv[3]-321/32*Log[2]*mzv[5]-19/2*Log[2]*mzv[1,1,-3]+8/7*mzv[2]^3-119/32*mzv[3]^2+8*mzv[1,-5]-8*mzv[1,1,1,-3],

  (*word [-1, 0, 1, -1, 1, 1]*)
  -mzv[1,-1,-1,-2,-1] -> -3/4*Log[2]^4*mzv[2]+49/16*Log[2]^3*mzv[3]-5/4*Log[2]^2*mzv[2]^2-25/4*Log[2]^2*mzv[1,-3]+23/4*Log[2]*mzv[2]*mzv[3]-603/64*Log[2]*mzv[5]-23/2*Log[2]*mzv[1,1,-3]+223/140*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-333/64*mzv[3]^2+23/2*mzv[1,-5]-9*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 0, -1, -1]*)
  mzv[1,-2,-2,-1] -> 9/16*Log[2]*mzv[2]*mzv[3]-37/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+19/20*mzv[2]^3-97/32*mzv[3]^2+15/2*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 0, -1, 1]*)
  mzv[-1,-2,-2,-1] -> -23/40*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-37/16*Log[2]*mzv[2]*mzv[3]+199/32*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]+27/140*mzv[2]^3+2*mzv[2]*mzv[1,-3]-155/128*mzv[3]^2,

  (*word [-1, 0, 1, 0, 0, -1]*)
  -mzv[-3,-2,-1] -> -1/8*Log[2]*mzv[2]*mzv[3]-11/32*Log[2]*mzv[5]-1199/1680*mzv[2]^3-mzv[2]*mzv[1,-3]+21/8*mzv[3]^2-5*mzv[1,-5],

  (*word [-1, 0, 1, 0, 0, 1]*)
  -mzv[3,-2,-1] -> -2*Log[2]*mzv[2]*mzv[3]+9/2*Log[2]*mzv[5]-8/105*mzv[2]^3+2*mzv[2]*mzv[1,-3]-7/32*mzv[3]^2-3/2*mzv[1,-5],

  (*word [-1, 0, 1, 0, 1, -1]*)
  mzv[-1,2,-2,-1] -> 23/40*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+47/16*Log[2]*mzv[2]*mzv[3]-461/64*Log[2]*mzv[5]-6*Log[2]*mzv[1,1,-3]+8/7*mzv[2]^3-107/32*mzv[3]^2+8*mzv[1,-5]-8*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 0, 1, 1]*)
  mzv[1,2,-2,-1] -> -3*Log[2]*mzv[2]*mzv[3]+11/2*Log[2]*mzv[5]-1/80*mzv[2]^3+mzv[2]*mzv[1,-3]+1/16*mzv[3]^2-5/4*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 1, -1, -1]*)
  -mzv[1,-1,1,-2,-1] -> 1/2*Log[2]^4*mzv[2]-35/48*Log[2]^3*mzv[3]-1/80*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-37/16*Log[2]*mzv[2]*mzv[3]+73/16*Log[2]*mzv[5]+5*Log[2]*mzv[1,1,-3]-32/35*mzv[2]^3+373/128*mzv[3]^2-7*mzv[1,-5]+11/2*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 1, -1, 1]*)
  -mzv[-1,-1,1,-2,-1] -> 3/4*Log[2]^4*mzv[2]-7/4*Log[2]^3*mzv[3]+11/40*Log[2]^2*mzv[2]^2+19/4*Log[2]^2*mzv[1,-3]-37/8*Log[2]*mzv[2]*mzv[3]+547/64*Log[2]*mzv[5]+11*Log[2]*mzv[1,1,-3]-85/56*mzv[2]^3+mzv[2]*mzv[1,-3]+627/128*mzv[3]^2-27/2*mzv[1,-5]+21/2*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 1, 0, -1]*)
  mzv[-2,1,-2,-1] -> -3/16*Log[2]*mzv[2]*mzv[3]+53/64*Log[2]*mzv[5]-1/8*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+25/128*mzv[3]^2,

  (*word [-1, 0, 1, 1, 0, 1]*)
  mzv[2,1,-2,-1] -> 2*Log[2]*mzv[2]*mzv[3]-9/2*Log[2]*mzv[5]+13/16*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-295/128*mzv[3]^2+15/4*mzv[1,-5],

  (*word [-1, 0, 1, 1, 1, -1]*)
  -mzv[-1,1,1,-2,-1] -> -1/4*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-1/4*Log[2]^2*mzv[2]^2-5/4*Log[2]^2*mzv[1,-3]+25/16*Log[2]*mzv[2]*mzv[3]-137/64*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+197/280*mzv[2]^3-311/128*mzv[3]^2+6*mzv[1,-5]-11/2*mzv[1,1,1,-3],

  (*word [-1, 0, 1, 1, 1, 1]*)
  -mzv[1,1,1,-2,-1] -> Log[2]*mzv[5]-1/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-65/128*mzv[3]^2+1/4*mzv[1,-5]-1/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, -1, -1, -1]*)
  mzv[1,1,1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-11/56*mzv[2]^3+5/8*mzv[3]^2-5/4*mzv[1,-5]+5/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, -1, -1, 1]*)
  mzv[-1,1,1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-859/1680*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+107/64*mzv[3]^2-4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, 1, -1, -1, 0, -1]*)
  -mzv[2,1,-1,-1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]-3/40*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-7/4*Log[2]*mzv[2]*mzv[3]+7/2*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-7/20*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+31/32*mzv[3]^2-15/4*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, -1, 0, 1]*)
  -mzv[-2,1,-1,-1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]+1/20*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-5/8*Log[2]*mzv[2]*mzv[3]+Log[2]*mzv[5]-5/2*Log[2]*mzv[1,1,-3]+199/280*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-283/128*mzv[3]^2+15/2*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [-1, 1, -1, -1, 1, -1]*)
  mzv[-1,-1,1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-35/16*Log[2]*mzv[2]*mzv[3]+135/32*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-3/80*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+1/32*mzv[3]^2,

  (*word [-1, 1, -1, -1, 1, 1]*)
  mzv[1,-1,1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-35/16*Log[2]*mzv[2]*mzv[3]+135/32*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-187/210*mzv[2]^3+179/64*mzv[3]^2-15/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 0, -1, -1]*)
  -mzv[1,2,-1,-1,-1] -> 1/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-13/16*Log[2]*mzv[2]*mzv[3]+99/64*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-67/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+29/64*mzv[3]^2+3/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 0, -1, 1]*)
  -mzv[-1,2,-1,-1,-1] -> 1/4*Log[2]^4*mzv[2]-17/24*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2+7/2*Log[2]^2*mzv[1,-3]-39/8*Log[2]*mzv[2]*mzv[3]+321/32*Log[2]*mzv[5]+13*Log[2]*mzv[1,1,-3]-277/70*mzv[2]^3-mzv[2]*mzv[1,-3]+811/64*mzv[3]^2-61/2*mzv[1,-5]+21*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 0, 0, -1]*)
  mzv[3,-1,-1,-1] -> 1/8*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-15/32*Log[2]*mzv[5]+991/1680*mzv[2]^3-29/16*mzv[3]^2+5*mzv[1,-5],

  (*word [-1, 1, -1, 0, 0, 1]*)
  mzv[-3,-1,-1,-1] -> -1/6*Log[2]^3*mzv[3]+3/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-15/8*Log[2]*mzv[2]*mzv[3]+245/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-881/1680*mzv[2]^3+25/16*mzv[3]^2-13/4*mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, 1, -1, 0, 1, -1]*)
  -mzv[-1,-2,-1,-1,-1] -> -1/4*Log[2]^4*mzv[2]+29/48*Log[2]^3*mzv[3]+1/5*Log[2]^2*mzv[2]^2-7/2*Log[2]^2*mzv[1,-3]+37/8*Log[2]*mzv[2]*mzv[3]-153/16*Log[2]*mzv[5]-29/2*Log[2]*mzv[1,1,-3]+135/28*mzv[2]^3+mzv[2]*mzv[1,-3]-31/2*mzv[3]^2+38*mzv[1,-5]-24*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 0, 1, 1]*)
  -mzv[1,-2,-1,-1,-1] -> 1/6*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-3/8*Log[2]*mzv[2]*mzv[3]+51/64*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+17/140*mzv[2]^3-55/128*mzv[3]^2+4*mzv[1,-5]-7/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 1, -1, -1]*)
  mzv[1,-1,-1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]+1/80*Log[2]^2*mzv[2]^2-1/8*Log[2]*mzv[2]*mzv[3]+3/16*Log[2]*mzv[5]-67/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+29/64*mzv[3]^2+3/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 1, -1, 1]*)
  mzv[-1,-1,-1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]+1/80*Log[2]^2*mzv[2]^2-1/8*Log[2]*mzv[2]*mzv[3]+3/16*Log[2]*mzv[5]-1/112*mzv[2]^3+1/32*mzv[3]^2,

  (*word [-1, 1, -1, 1, 0, -1]*)
  -mzv[-2,-1,-1,-1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]-3/40*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-7/4*Log[2]*mzv[2]*mzv[3]+7/2*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-661/560*mzv[2]^3-mzv[2]*mzv[1,-3]+31/8*mzv[3]^2-8*mzv[1,-5]+8*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 1, 0, 1]*)
  -mzv[2,-1,-1,-1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]+1/20*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-5/8*Log[2]*mzv[2]*mzv[3]+Log[2]*mzv[5]-5/2*Log[2]*mzv[1,1,-3]+71/70*mzv[2]^3+mzv[2]*mzv[1,-3]-215/64*mzv[3]^2+8*mzv[1,-5]-13/2*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 1, 1, -1]*)
  mzv[-1,1,-1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]+1/80*Log[2]^2*mzv[2]^2-11/16*Log[2]*mzv[2]*mzv[3]+19/16*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]+461/560*mzv[2]^3-169/64*mzv[3]^2+15/2*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, 1, -1, 1, 1, 1]*)
  mzv[1,1,-1,-1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+1/24*Log[2]^3*mzv[3]+1/80*Log[2]^2*mzv[2]^2-11/16*Log[2]*mzv[2]*mzv[3]+19/16*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]+29/112*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-27/32*mzv[3]^2+4*mzv[1,-5]-7/2*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, -1, -1]*)
  -mzv[1,1,-2,-1,-1] -> 1/80*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+477/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-177/64*mzv[3]^2+13/2*mzv[1,-5]-7/2*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, -1, 1]*)
  -mzv[-1,1,-2,-1,-1] -> -3/8*Log[2]^4*mzv[2]+21/16*Log[2]^3*mzv[3]-7/20*Log[2]^2*mzv[2]^2-9/4*Log[2]^2*mzv[1,-3]+21/8*Log[2]*mzv[2]*mzv[3]-321/64*Log[2]*mzv[5]-7*Log[2]*mzv[1,1,-3]+1147/560*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-837/128*mzv[3]^2+61/4*mzv[1,-5]-21/2*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, 0, -1]*)
  mzv[2,-2,-1,-1] -> 1/16*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+15/8*Log[2]*mzv[2]*mzv[3]-59/16*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+197/112*mzv[2]^3+2*mzv[2]*mzv[1,-3]-187/32*mzv[3]^2+12*mzv[1,-5]-8*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, 0, 1]*)
  mzv[-2,-2,-1,-1] -> -3/80*Log[2]^2*mzv[2]^2-1/2*Log[2]*mzv[2]*mzv[3]+33/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+391/560*mzv[2]^3-mzv[2]*mzv[1,-3]-139/64*mzv[3]^2+15/2*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, 1, -1]*)
  -mzv[-1,-1,-2,-1,-1] -> 3/4*Log[2]^4*mzv[2]-35/16*Log[2]^3*mzv[3]+11/40*Log[2]^2*mzv[2]^2+13/2*Log[2]^2*mzv[1,-3]-37/4*Log[2]*mzv[2]*mzv[3]+579/32*Log[2]*mzv[5]+37/2*Log[2]*mzv[1,1,-3]-277/56*mzv[2]^3+63/4*mzv[3]^2-38*mzv[1,-5]+24*mzv[1,1,1,-3],

  (*word [-1, 1, 0, -1, 1, 1]*)
  -mzv[1,-1,-2,-1,-1] -> 3/8*Log[2]^4*mzv[2]-7/4*Log[2]^3*mzv[3]+53/80*Log[2]^2*mzv[2]^2+9/2*Log[2]^2*mzv[1,-3]-99/16*Log[2]*mzv[2]*mzv[3]+721/64*Log[2]*mzv[5]+23/2*Log[2]*mzv[1,1,-3]-267/80*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+1371/128*mzv[3]^2-23*mzv[1,-5]+15*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 0, -1, -1]*)
  mzv[1,-3,-1,-1] -> 1/2*Log[2]^2*mzv[1,-3]-1/2*Log[2]*mzv[2]*mzv[3]+59/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-3/280*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+1/16*mzv[3]^2+3/4*mzv[1,-5]+mzv[1,1,1,-3],

  (*word [-1, 1, 0, 0, -1, 1]*)
  mzv[-1,-3,-1,-1] -> 7/8*Log[2]^3*mzv[3]-11/20*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]+3*Log[2]*mzv[2]*mzv[3]-359/64*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+599/560*mzv[2]^3-101/32*mzv[3]^2+23/4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 0, 0, -1]*)
  -mzv[-4,-1,-1] -> 7/40*Log[2]^2*mzv[2]^2+3/8*Log[2]*mzv[2]*mzv[3]-17/16*Log[2]*mzv[5]+533/1680*mzv[2]^3-39/32*mzv[3]^2+4*mzv[1,-5],

  (*word [-1, 1, 0, 0, 0, 1]*)
  -mzv[4,-1,-1] -> -1/5*Log[2]^2*mzv[2]^2-3/4*Log[2]*mzv[2]*mzv[3]+59/32*Log[2]*mzv[5]-13/420*mzv[2]^3+19/64*mzv[3]^2-3/2*mzv[1,-5],

  (*word [-1, 1, 0, 0, 1, -1]*)
  mzv[-1,3,-1,-1] -> -7/8*Log[2]^3*mzv[3]+41/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-2*Log[2]*mzv[2]*mzv[3]+245/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-19/80*mzv[2]^3+1/2*mzv[3]^2,

  (*word [-1, 1, 0, 0, 1, 1]*)
  mzv[1,3,-1,-1] -> 1/20*Log[2]^2*mzv[2]^2-3/8*Log[2]*mzv[2]*mzv[3]+9/16*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]-61/560*mzv[2]^3+19/64*mzv[3]^2+9/4*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, -1, -1]*)
  -mzv[1,-1,2,-1,-1] -> -3/8*Log[2]^4*mzv[2]+7/8*Log[2]^3*mzv[3]+3/80*Log[2]^2*mzv[2]^2-17/4*Log[2]^2*mzv[1,-3]+97/16*Log[2]*mzv[2]*mzv[3]-381/32*Log[2]*mzv[5]-13*Log[2]*mzv[1,1,-3]+351/112*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]-639/64*mzv[3]^2+25*mzv[1,-5]-15*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, -1, 1]*)
  -mzv[-1,-1,2,-1,-1] -> -3/4*Log[2]^4*mzv[2]+35/16*Log[2]^3*mzv[3]-9/40*Log[2]^2*mzv[2]^2-29/4*Log[2]^2*mzv[1,-3]+181/16*Log[2]*mzv[2]*mzv[3]-177/8*Log[2]*mzv[5]-20*Log[2]*mzv[1,1,-3]+199/40*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-63/4*mzv[3]^2+38*mzv[1,-5]-24*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, 0, -1]*)
  mzv[-2,2,-1,-1] -> -11/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-27/16*Log[2]*mzv[2]*mzv[3]+57/16*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-135/112*mzv[2]^3-5/4*mzv[2]*mzv[1,-3]+127/32*mzv[3]^2-8*mzv[1,-5]+8*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, 0, 1]*)
  mzv[2,2,-1,-1] -> 3/20*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-43/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-543/560*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+205/64*mzv[3]^2-31/4*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, 1, -1]*)
  -mzv[-1,1,2,-1,-1] -> 3/8*Log[2]^4*mzv[2]-7/16*Log[2]^3*mzv[3]-5/16*Log[2]^2*mzv[2]^2+3*Log[2]^2*mzv[1,-3]-45/8*Log[2]*mzv[2]*mzv[3]+729/64*Log[2]*mzv[5]+19/2*Log[2]*mzv[1,1,-3]-1361/560*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+495/64*mzv[3]^2-35/2*mzv[1,-5]+27/2*mzv[1,1,1,-3],

  (*word [-1, 1, 0, 1, 1, 1]*)
  -mzv[1,1,2,-1,-1] -> -1/5*Log[2]^2*mzv[2]^2+3/16*Log[2]*mzv[2]*mzv[3]+21/64*Log[2]*mzv[5]+1/2*Log[2]*mzv[1,1,-3]-421/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+149/64*mzv[3]^2-15/4*mzv[1,-5]+5/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, -1, -1, -1]*)
  mzv[1,1,-1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/16*Log[2]*mzv[2]*mzv[3]-3/64*Log[2]*mzv[5]+67/1680*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-5/32*mzv[3]^2-1/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, -1, -1, 1]*)
  mzv[-1,1,-1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/16*Log[2]*mzv[2]*mzv[3]-3/64*Log[2]*mzv[5]+17/560*mzv[2]^3-15/128*mzv[3]^2,

  (*word [-1, 1, 1, -1, 0, -1]*)
  -mzv[2,-1,1,-1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]-3/40*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-13/8*Log[2]*mzv[2]*mzv[3]+113/32*Log[2]*mzv[5]+7/2*Log[2]*mzv[1,1,-3]-461/560*mzv[2]^3+mzv[2]*mzv[1,-3]+157/64*mzv[3]^2-15/2*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, 1, 1, -1, 0, 1]*)
  -mzv[-2,-1,1,-1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]+1/20*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/16*Log[2]*mzv[2]*mzv[3]-43/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-7/8*mzv[2]^3-mzv[2]*mzv[1,-3]+197/64*mzv[3]^2-6*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, -1, 1, -1]*)
  mzv[-1,-1,-1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-9/32*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-489/560*mzv[2]^3+183/64*mzv[3]^2-15/2*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, 1, 1, -1, 1, 1]*)
  mzv[1,-1,-1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-9/32*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-69/80*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+373/128*mzv[3]^2-6*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 0, -1, -1]*)
  -mzv[1,-2,1,-1,-1] -> 1/48*Log[2]^3*mzv[3]-3/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-13/16*Log[2]*mzv[2]*mzv[3]+99/64*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]+1/16*mzv[2]^3-11/64*mzv[3]^2+3/4*mzv[1,-5],

  (*word [-1, 1, 1, 0, -1, 1]*)
  -mzv[-1,-2,1,-1,-1] -> 1/4*Log[2]^4*mzv[2]-17/24*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2+7/2*Log[2]^2*mzv[1,-3]-87/16*Log[2]*mzv[2]*mzv[3]+751/64*Log[2]*mzv[5]+9*Log[2]*mzv[1,1,-3]-57/40*mzv[2]^3+7/4*mzv[2]*mzv[1,-3]+251/64*mzv[3]^2-13*mzv[1,-5]+15/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 0, 0, -1]*)
  mzv[-3,1,-1,-1] -> 1/8*Log[2]^3*mzv[3]-1/16*Log[2]^2*mzv[2]^2+1/8*Log[2]*mzv[2]*mzv[3]-15/32*Log[2]*mzv[5]+103/1680*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]+5/64*mzv[3]^2+3/4*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [-1, 1, 1, 0, 0, 1]*)
  mzv[3,1,-1,-1] -> -1/6*Log[2]^3*mzv[3]+3/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-15/8*Log[2]*mzv[2]*mzv[3]+245/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-43/840*mzv[2]^3+mzv[2]*mzv[1,-3]-35/128*mzv[3]^2-3/2*mzv[1,-5],

  (*word [-1, 1, 1, 0, 1, -1]*)
  -mzv[-1,2,1,-1,-1] -> -1/4*Log[2]^4*mzv[2]+29/48*Log[2]^3*mzv[3]+1/5*Log[2]^2*mzv[2]^2-7/2*Log[2]^2*mzv[1,-3]+83/16*Log[2]*mzv[2]*mzv[3]-721/64*Log[2]*mzv[5]-21/2*Log[2]*mzv[1,1,-3]+127/56*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]-439/64*mzv[3]^2+35/2*mzv[1,-5]-27/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 0, 1, 1]*)
  -mzv[1,2,1,-1,-1] -> 1/6*Log[2]^3*mzv[3]-1/8*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-3/8*Log[2]*mzv[2]*mzv[3]+51/64*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+377/560*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-277/128*mzv[3]^2+25/4*mzv[1,-5]-5*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 1, -1, -1]*)
  mzv[1,-1,1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/16*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/16*Log[2]*mzv[2]*mzv[3]+3/16*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]+179/420*mzv[2]^3-183/128*mzv[3]^2+15/4*mzv[1,-5]-3/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 1, -1, 1]*)
  mzv[-1,-1,1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/16*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/16*Log[2]*mzv[2]*mzv[3]+3/16*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]+61/80*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-157/64*mzv[3]^2+4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 1, 0, -1]*)
  -mzv[-2,1,1,-1,-1] -> 1/48*Log[2]^4*mzv[2]-1/24*Log[2]^3*mzv[3]-3/40*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-13/8*Log[2]*mzv[2]*mzv[3]+113/32*Log[2]*mzv[5]+7/2*Log[2]*mzv[1,1,-3]-569/560*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+381/128*mzv[3]^2-6*mzv[1,-5]+11/2*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 1, 0, 1]*)
  -mzv[2,1,1,-1,-1] -> -1/24*Log[2]^4*mzv[2]+5/48*Log[2]^3*mzv[3]+1/20*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/16*Log[2]*mzv[2]*mzv[3]-43/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-41/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]+23/32*mzv[3]^2-15/4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [-1, 1, 1, 1, 1, -1]*)
  mzv[-1,1,1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/16*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/4*Log[2]*mzv[2]*mzv[3]+33/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]-3/16*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+49/128*mzv[3]^2,

  (*word [-1, 1, 1, 1, 1, 1]*)
  mzv[1,1,1,1,-1,-1] -> 1/720*Log[2]^6-1/48*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/16*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]+1/4*Log[2]*mzv[2]*mzv[3]+33/64*Log[2]*mzv[5]-1/2*Log[2]*mzv[1,1,-3]-53/280*mzv[2]^3-1/8*mzv[3]^2+1/4*mzv[1,-5]-1/2*mzv[1,1,1,-3],

  (*word [0, -1, -1, -1, -1, -1]*)
  -mzv[1,1,1,1,-2] -> 11/280*mzv[2]^3-1/8*mzv[3]^2+1/4*mzv[1,-5]-1/2*mzv[1,1,1,-3],

  (*word [0, -1, -1, -1, -1, 1]*)
  -mzv[-1,1,1,1,-2] -> -1/16*Log[2]^4*mzv[2]+7/24*Log[2]^3*mzv[3]-13/80*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-7/8*Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]-51/280*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+31/64*mzv[3]^2-mzv[1,-5],

  (*word [0, -1, -1, -1, 0, -1]*)
  mzv[2,1,1,-2] -> -463/1680*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+59/64*mzv[3]^2-5/2*mzv[1,-5],

  (*word [0, -1, -1, -1, 0, 1]*)
  mzv[-2,1,1,-2] -> 223/840*mzv[2]^3+5/4*mzv[2]*mzv[1,-3]-121/128*mzv[3]^2,

  (*word [0, -1, -1, -1, 1, -1]*)
  -mzv[-1,-1,1,1,-2] -> 1/4*Log[2]^4*mzv[2]-49/48*Log[2]^3*mzv[3]+39/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]+35/16*Log[2]*mzv[2]*mzv[3]-155/32*Log[2]*mzv[5]+27/56*mzv[2]^3-mzv[2]*mzv[1,-3]-43/32*mzv[3]^2+11/4*mzv[1,-5]-3/2*mzv[1,1,1,-3],

  (*word [0, -1, -1, -1, 1, 1]*)
  -mzv[1,-1,1,1,-2] -> 3/16*Log[2]^4*mzv[2]-49/48*Log[2]^3*mzv[3]+3/5*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]-7/16*Log[2]*mzv[2]*mzv[3]-3/16*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]+3/140*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+7/64*mzv[3]^2-3*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [0, -1, -1, 0, -1, -1]*)
  mzv[1,2,1,-2] -> 121/280*mzv[2]^3-179/128*mzv[3]^2+15/4*mzv[1,-5],

  (*word [0, -1, -1, 0, -1, 1]*)
  mzv[-1,2,1,-2] -> -7/4*Log[2]*mzv[2]*mzv[3]+217/64*Log[2]*mzv[5]-99/112*mzv[2]^3-5/4*mzv[2]*mzv[1,-3]+365/128*mzv[3]^2-7/4*mzv[1,-5],

  (*word [0, -1, -1, 0, 0, -1]*)
  -mzv[3,1,-2] -> 71/560*mzv[2]^3-25/64*mzv[3]^2+mzv[1,-5],

  (*word [0, -1, -1, 0, 0, 1]*)
  -mzv[-3,1,-2] -> -29/70*mzv[2]^3-3/2*mzv[2]*mzv[1,-3]+45/32*mzv[3]^2,

  (*word [0, -1, -1, 0, 1, -1]*)
  mzv[-1,-2,1,-2] -> 7/4*Log[2]*mzv[2]*mzv[3]-217/64*Log[2]*mzv[5]+53/280*mzv[2]^3-mzv[2]*mzv[1,-3]-61/128*mzv[3]^2+mzv[1,-5],

  (*word [0, -1, -1, 0, 1, 1]*)
  mzv[1,-2,1,-2] -> -1/560*mzv[2]^3+1/128*mzv[3]^2,

  (*word [0, -1, -1, 1, -1, -1]*)
  -mzv[1,-1,-1,1,-2] -> -3/8*Log[2]^4*mzv[2]+21/16*Log[2]^3*mzv[3]-9/16*Log[2]^2*mzv[2]^2-33/8*Log[2]*mzv[2]*mzv[3]+273/32*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-241/280*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+81/32*mzv[3]^2-6*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [0, -1, -1, 1, -1, 1]*)
  -mzv[-1,-1,-1,1,-2] -> -7/16*Log[2]^4*mzv[2]+77/48*Log[2]^3*mzv[3]-5/8*Log[2]^2*mzv[2]^2-5/4*Log[2]^2*mzv[1,-3]-5/2*Log[2]*mzv[2]*mzv[3]+345/64*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]-131/140*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+377/128*mzv[3]^2-7/4*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [0, -1, -1, 1, 0, -1]*)
  mzv[-2,-1,1,-2] -> -1/5*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-23/4*Log[2]*mzv[2]*mzv[3]+743/64*Log[2]*mzv[5]+8*Log[2]*mzv[1,1,-3]-797/336*mzv[2]^3+mzv[2]*mzv[1,-3]+941/128*mzv[3]^2-35/2*mzv[1,-5]+12*mzv[1,1,1,-3],

  (*word [0, -1, -1, 1, 0, 1]*)
  mzv[2,-1,1,-2] -> 3/16*Log[2]^2*mzv[2]^2-35/16*Log[2]*mzv[2]*mzv[3]+217/64*Log[2]*mzv[5]-659/840*mzv[2]^3-mzv[2]*mzv[1,-3]+355/128*mzv[3]^2-7/4*mzv[1,-5],

  (*word [0, -1, -1, 1, 1, -1]*)
  -mzv[-1,1,-1,1,-2] -> -1/8*Log[2]^4*mzv[2]+7/6*Log[2]^3*mzv[3]-61/80*Log[2]^2*mzv[2]^2-7/4*Log[2]^2*mzv[1,-3]+21/16*Log[2]*mzv[2]*mzv[3]-81/64*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+229/560*mzv[2]^3-193/128*mzv[3]^2+15/4*mzv[1,-5]-5/2*mzv[1,1,1,-3],

  (*word [0, -1, -1, 1, 1, 1]*)
  -mzv[1,1,-1,1,-2] -> -3/16*Log[2]^4*mzv[2]+7/6*Log[2]^3*mzv[3]-37/40*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+15/16*Log[2]*mzv[2]*mzv[3]+1/4*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+15/112*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]-7/8*mzv[3]^2+4*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [0, -1, 0, -1, -1, -1]*)
  mzv[1,1,2,-2] -> -121/840*mzv[2]^3+15/32*mzv[3]^2-5/4*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [0, -1, 0, -1, -1, 1]*)
  mzv[-1,1,2,-2] -> -1/10*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-5/2*Log[2]*mzv[2]*mzv[3]+325/64*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-61/210*mzv[2]^3+11/4*mzv[2]*mzv[1,-3]+85/128*mzv[3]^2-27/4*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [0, -1, 0, -1, 0, -1]*)
  -mzv[2,2,-2] -> -319/560*mzv[2]^3-mzv[2]*mzv[1,-3]+31/16*mzv[3]^2-4*mzv[1,-5],

  (*word [0, -1, 0, -1, 0, 1]*)
  -mzv[-2,2,-2] -> 89/80*mzv[2]^3+5*mzv[2]*mzv[1,-3]-63/16*mzv[3]^2,

  (*word [0, -1, 0, -1, 1, -1]*)
  mzv[-1,-1,2,-2] -> 1/5*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+49/8*Log[2]*mzv[2]*mzv[3]-391/32*Log[2]*mzv[5]-6*Log[2]*mzv[1,1,-3]+221/120*mzv[2]^3-mzv[2]*mzv[1,-3]-91/16*mzv[3]^2+13*mzv[1,-5]-8*mzv[1,1,1,-3],

  (*word [0, -1, 0, -1, 1, 1]*)
  mzv[1,-1,2,-2] -> 1/10*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+37/16*Log[2]*mzv[2]*mzv[3]-151/32*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+557/336*mzv[2]^3+5/2*mzv[2]*mzv[1,-3]-685/128*mzv[3]^2+9/2*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [0, -1, 0, 0, -1, -1]*)
  -mzv[1,3,-2] -> -17/105*mzv[2]^3+17/32*mzv[3]^2-3/2*mzv[1,-5],

  (*word [0, -1, 0, 0, -1, 1]*)
  -mzv[-1,3,-2] -> -27/8*Log[2]*mzv[2]*mzv[3]+217/32*Log[2]*mzv[5]-355/336*mzv[2]^3+201/64*mzv[3]^2-7/2*mzv[1,-5],

  (*word [0, -1, 0, 0, 0, -1]*)
  mzv[4,-2] -> -29/120*mzv[2]^3+15/16*mzv[3]^2-4*mzv[1,-5],

  (*word [0, -1, 0, 0, 0, 1]*)
  mzv[-4,-2] -> -239/840*mzv[2]^3+3/4*mzv[3]^2,

  (*word [0, -1, 0, 0, 1, -1]*)
  -mzv[-1,-3,-2] -> 27/8*Log[2]*mzv[2]*mzv[3]-217/32*Log[2]*mzv[5]+449/840*mzv[2]^3-3/2*mzv[3]^2+mzv[1,-5],

  (*word [0, -1, 0, 0, 1, 1]*)
  -mzv[1,-3,-2] -> 82/105*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]-163/64*mzv[3]^2,

  (*word [0, -1, 0, 1, -1, -1]*)
  mzv[1,-1,-2,-2] -> -1/10*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-29/8*Log[2]*mzv[2]*mzv[3]+457/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-377/1680*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+67/128*mzv[3]^2-3/2*mzv[1,-5],

  (*word [0, -1, 0, 1, -1, 1]*)
  mzv[-1,-1,-2,-2] -> -1/5*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-49/8*Log[2]*mzv[2]*mzv[3]+391/32*Log[2]*mzv[5]+6*Log[2]*mzv[1,1,-3]-73/30*mzv[2]^3-mzv[2]*mzv[1,-3]+493/64*mzv[3]^2-25/2*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [0, -1, 0, 1, 0, -1]*)
  -mzv[-2,-2,-2] -> -3/560*mzv[2]^3,

  (*word [0, -1, 0, 1, 0, 1]*)
  -mzv[2,-2,-2] -> -459/560*mzv[2]^3-3*mzv[2]*mzv[1,-3]+91/32*mzv[3]^2,

  (*word [0, -1, 0, 1, 1, -1]*)
  mzv[-1,1,-2,-2] -> 1/10*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+61/16*Log[2]*mzv[2]*mzv[3]-15/2*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+2371/1680*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]-71/16*mzv[3]^2+10*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [0, -1, 0, 1, 1, 1]*)
  mzv[1,1,-2,-2] -> -179/1680*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+51/128*mzv[3]^2,

  (*word [0, -1, 1, -1, -1, -1]*)
  -mzv[1,1,-1,-1,-2] -> 1/4*Log[2]^4*mzv[2]-35/48*Log[2]^3*mzv[3]+5/16*Log[2]^2*mzv[2]^2-5/4*Log[2]^2*mzv[1,-3]+4*Log[2]*mzv[2]*mzv[3]-509/64*Log[2]*mzv[5]-9/2*Log[2]*mzv[1,1,-3]+201/280*mzv[2]^3-mzv[2]*mzv[1,-3]-139/64*mzv[3]^2+21/4*mzv[1,-5]-5*mzv[1,1,1,-3],

  (*word [0, -1, 1, -1, -1, 1]*)
  -mzv[-1,1,-1,-1,-2] -> 3/16*Log[2]^4*mzv[2]-7/16*Log[2]^3*mzv[3]+3/20*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]+39/8*Log[2]*mzv[2]*mzv[3]-301/32*Log[2]*mzv[5]-9/2*Log[2]*mzv[1,1,-3]+29/16*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-93/16*mzv[3]^2+19/2*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [0, -1, 1, -1, 0, -1]*)
  mzv[2,-1,-1,-2] -> 2/5*Log[2]^2*mzv[2]^2-4*Log[2]^2*mzv[1,-3]+129/16*Log[2]*mzv[2]*mzv[3]-263/16*Log[2]*mzv[5]-16*Log[2]*mzv[1,1,-3]+1781/420*mzv[2]^3-mzv[2]*mzv[1,-3]-427/32*mzv[3]^2+33*mzv[1,-5]-24*mzv[1,1,1,-3],

  (*word [0, -1, 1, -1, 0, 1]*)
  mzv[-2,-1,-1,-2] -> -3/8*Log[2]^2*mzv[2]^2+23/4*Log[2]*mzv[2]*mzv[3]-155/16*Log[2]*mzv[5]+331/210*mzv[2]^3+mzv[2]*mzv[1,-3]-683/128*mzv[3]^2+5*mzv[1,-5],

  (*word [0, -1, 1, -1, 1, -1]*)
  -mzv[-1,-1,-1,-1,-2] -> 1/2*Log[2]^4*mzv[2]-7/4*Log[2]^3*mzv[3]+3/5*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]+21/16*Log[2]*mzv[2]*mzv[3]-3*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]+31/140*mzv[2]^3-21/32*mzv[3]^2,

  (*word [0, -1, 1, -1, 1, 1]*)
  -mzv[1,-1,-1,-1,-2] -> 7/16*Log[2]^4*mzv[2]-7/4*Log[2]^3*mzv[3]+57/80*Log[2]^2*mzv[2]^2+9/4*Log[2]^2*mzv[1,-3]+33/16*Log[2]*mzv[2]*mzv[3]-297/64*Log[2]*mzv[5]+9/2*Log[2]*mzv[1,1,-3]+451/560*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-159/64*mzv[3]^2-9/4*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [0, -1, 1, 0, -1, -1]*)
  mzv[1,-2,-1,-2] -> -1/16*Log[2]*mzv[2]*mzv[3]+15/64*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-703/280*mzv[2]^3-mzv[2]*mzv[1,-3]+1043/128*mzv[3]^2-20*mzv[1,-5]+12*mzv[1,1,1,-3],

  (*word [0, -1, 1, 0, -1, 1]*)
  mzv[-1,-2,-1,-2] -> 31/40*Log[2]^2*mzv[2]^2-4*Log[2]^2*mzv[1,-3]+85/16*Log[2]*mzv[2]*mzv[3]-205/16*Log[2]*mzv[5]-12*Log[2]*mzv[1,1,-3]+319/140*mzv[2]^3-mzv[2]*mzv[1,-3]-839/128*mzv[3]^2+19*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [0, -1, 1, 0, 0, -1]*)
  -mzv[-3,-1,-2] -> -5/8*Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]-261/560*mzv[2]^3+mzv[2]*mzv[1,-3]+33/32*mzv[3]^2-6*mzv[1,-5],

  (*word [0, -1, 1, 0, 0, 1]*)
  -mzv[3,-1,-2] -> 37/8*Log[2]*mzv[2]*mzv[3]-155/16*Log[2]*mzv[5]+411/560*mzv[2]^3-2*mzv[2]*mzv[1,-3]-107/64*mzv[3]^2+5*mzv[1,-5],

  (*word [0, -1, 1, 0, 1, -1]*)
  mzv[-1,2,-1,-2] -> -31/40*Log[2]^2*mzv[2]^2+4*Log[2]^2*mzv[1,-3]-97/16*Log[2]*mzv[2]*mzv[3]+897/64*Log[2]*mzv[5]+16*Log[2]*mzv[1,1,-3]-89/20*mzv[2]^3+2*mzv[2]*mzv[1,-3]+109/8*mzv[3]^2-38*mzv[1,-5]+24*mzv[1,1,1,-3],

  (*word [0, -1, 1, 0, 1, 1]*)
  mzv[1,2,-1,-2] -> 59/16*Log[2]*mzv[2]*mzv[3]-217/32*Log[2]*mzv[5]+843/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-311/64*mzv[3]^2+7/2*mzv[1,-5],

  (*word [0, -1, 1, 1, -1, -1]*)
  -mzv[1,-1,1,-1,-2] -> -1/8*Log[2]^4*mzv[2]-7/24*Log[2]^3*mzv[3]+3/10*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-23/16*Log[2]*mzv[2]*mzv[3]+145/64*Log[2]*mzv[5]+5/2*Log[2]*mzv[1,1,-3]-77/80*mzv[2]^3+203/64*mzv[3]^2-15/2*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [0, -1, 1, 1, -1, 1]*)
  -mzv[-1,-1,1,-1,-2] -> -3/16*Log[2]^4*mzv[2]+19/80*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]-23/16*Log[2]*mzv[2]*mzv[3]+161/64*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]-69/112*mzv[2]^3-mzv[2]*mzv[1,-3]+249/128*mzv[3]^2+1/4*mzv[1,-5]-3/2*mzv[1,1,1,-3],

  (*word [0, -1, 1, 1, 0, -1]*)
  mzv[-2,1,-1,-2] -> 1/5*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+9/2*Log[2]*mzv[2]*mzv[3]-619/64*Log[2]*mzv[5]-8*Log[2]*mzv[1,1,-3]+2053/840*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-953/128*mzv[3]^2+33/2*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [0, -1, 1, 1, 0, 1]*)
  mzv[2,1,-1,-2] -> -3/16*Log[2]^2*mzv[2]^2+1/2*Log[2]*mzv[2]*mzv[3]+31/64*Log[2]*mzv[5]-1483/1680*mzv[2]^3-11/4*mzv[2]*mzv[1,-3]+311/128*mzv[3]^2-1/4*mzv[1,-5],

  (*word [0, -1, 1, 1, 1, -1]*)
  -mzv[-1,1,1,-1,-2] -> 1/8*Log[2]^4*mzv[2]-7/16*Log[2]^3*mzv[3]+13/20*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]+1/2*Log[2]*mzv[2]*mzv[3]-165/64*Log[2]*mzv[5]+5/2*Log[2]*mzv[1,1,-3]-3/40*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+69/128*mzv[3]^2-4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [0, -1, 1, 1, 1, 1]*)
  -mzv[1,1,1,-1,-2] -> 1/16*Log[2]^4*mzv[2]-7/16*Log[2]^3*mzv[3]+39/80*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]+1/8*Log[2]*mzv[2]*mzv[3]-161/64*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]+5/14*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]-1/4*mzv[3]^2-1/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [0, 0, -1, -1, -1, 1]*)
  mzv[-1,1,1,-3] -> 7/24*Log[2]^3*mzv[3]-11/40*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+7/4*Log[2]*mzv[2]*mzv[3]-87/32*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+67/280*mzv[2]^3-mzv[2]*mzv[1,-3]-27/32*mzv[3]^2+9/2*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [0, 0, -1, -1, 0, -1]*)
  -mzv[2,1,-3] -> 113/210*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-55/32*mzv[3]^2+mzv[1,-5],

  (*word [0, 0, -1, -1, 0, 1]*)
  -mzv[-2,1,-3] -> -41/168*mzv[2]^3-mzv[2]*mzv[1,-3]+27/32*mzv[3]^2,

  (*word [0, 0, -1, -1, 1, -1]*)
  mzv[-1,-1,1,-3] -> -7/8*Log[2]^3*mzv[3]+63/80*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]-5/4*Log[2]*mzv[2]*mzv[3]+27/32*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]+143/560*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]-31/64*mzv[3]^2-3/4*mzv[1,-5]+mzv[1,1,1,-3],

  (*word [0, 0, -1, -1, 1, 1]*)
  mzv[1,-1,1,-3] -> -7/12*Log[2]^3*mzv[3]+3/5*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]-5/8*Log[2]*mzv[2]*mzv[3]-3/16*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-67/140*mzv[2]^3-mzv[2]*mzv[1,-3]+61/32*mzv[3]^2-3*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [0, 0, -1, 0, -1, -1]*)
  -mzv[1,2,-3] -> 1/15*mzv[2]^3-17/64*mzv[3]^2+7/2*mzv[1,-5],

  (*word [0, 0, -1, 0, -1, 1]*)
  -mzv[-1,2,-3] -> 13/8*Log[2]*mzv[2]*mzv[3]-93/32*Log[2]*mzv[5]-43/210*mzv[2]^3-2*mzv[2]*mzv[1,-3]+45/64*mzv[3]^2+3/2*mzv[1,-5],

  (*word [0, 0, -1, 0, 0, -1]*)
  mzv[3,-3] -> 5/8*mzv[2]^3-63/32*mzv[3]^2+6*mzv[1,-5],

  (*word [0, 0, -1, 0, 0, 1]*)
  mzv[-3,-3] -> -4/35*mzv[2]^3+9/32*mzv[3]^2,

  (*word [0, 0, -1, 0, 1, -1]*)
  -mzv[-1,-2,-3] -> -13/8*Log[2]*mzv[2]*mzv[3]+93/32*Log[2]*mzv[5]+29/336*mzv[2]^3-mzv[2]*mzv[1,-3]-3/32*mzv[3]^2+mzv[1,-5],

  (*word [0, 0, -1, 0, 1, 1]*)
  -mzv[1,-2,-3] -> -403/840*mzv[2]^3+95/64*mzv[3]^2,

  (*word [0, 0, -1, 1, -1, -1]*)
  mzv[1,-1,-1,-3] -> 7/8*Log[2]^3*mzv[3]-3/4*Log[2]^2*mzv[2]^2-37/16*Log[2]*mzv[2]*mzv[3]+91/16*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-33/40*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+149/64*mzv[3]^2-13/4*mzv[1,-5]+3*mzv[1,1,1,-3],

  (*word [0, 0, -1, 1, -1, 1]*)
  mzv[-1,-1,-1,-3] -> 7/6*Log[2]^3*mzv[3]-41/40*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]-15/8*Log[2]*mzv[2]*mzv[3]+345/64*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]-349/560*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+13/8*mzv[3]^2-7/4*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [0, 0, -1, 1, 0, -1]*)
  -mzv[-2,-1,-3] -> -7/8*Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]+211/1680*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-21/32*mzv[3]^2+4*mzv[1,-5],

  (*word [0, 0, -1, 1, 0, 1]*)
  -mzv[2,-1,-3] -> -21/4*Log[2]*mzv[2]*mzv[3]+155/16*Log[2]*mzv[5]-631/840*mzv[2]^3+3*mzv[2]*mzv[1,-3]+73/32*mzv[3]^2-5*mzv[1,-5],

  (*word [0, 0, -1, 1, 1, -1]*)
  mzv[-1,1,-1,-3] -> -11/80*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]-9/16*Log[2]*mzv[2]*mzv[3]+101/64*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]+247/560*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]-49/32*mzv[3]^2+23/4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [0, 0, -1, 1, 1, 1]*)
  mzv[1,1,-1,-3] -> 7/24*Log[2]^3*mzv[3]-13/40*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]-17/8*Log[2]*mzv[2]*mzv[3]+157/32*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]-27/40*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+7/4*mzv[3]^2-3/2*mzv[1,-5]-mzv[1,1,1,-3],

  (*word [0, 0, 0, -1, -1, -1]*)
  -mzv[1,1,-4] -> -1/14*mzv[2]^3+1/4*mzv[3]^2-3/2*mzv[1,-5],

  (*word [0, 0, 0, -1, -1, 1]*)
  -mzv[-1,1,-4] -> -3/8*Log[2]^2*mzv[2]^2-1/4*Log[2]*mzv[2]*mzv[3]+31/16*Log[2]*mzv[5]-9/280*mzv[2]^3+mzv[2]*mzv[1,-3]-3/8*mzv[3]^2-mzv[1,-5],

  (*word [0, 0, 0, -1, 0, -1]*)
  mzv[2,-4] -> -5/24*mzv[2]^3+3/4*mzv[3]^2-4*mzv[1,-5],

  (*word [0, 0, 0, -1, 0, 1]*)
  mzv[-2,-4] -> 97/420*mzv[2]^3-3/4*mzv[3]^2,

  (*word [0, 0, 0, -1, 1, -1]*)
  -mzv[-1,-1,-4] -> 3/4*Log[2]^2*mzv[2]^2+9/8*Log[2]*mzv[2]*mzv[3]-155/32*Log[2]*mzv[5]+257/560*mzv[2]^3-3/4*mzv[3]^2,

  (*word [0, 0, 0, -1, 1, 1]*)
  -mzv[1,-1,-4] -> 3/8*Log[2]^2*mzv[2]^2+7/4*Log[2]*mzv[2]*mzv[3]-155/32*Log[2]*mzv[5]+59/70*mzv[2]^3-mzv[2]*mzv[1,-3]-17/8*mzv[3]^2+5/2*mzv[1,-5],

  (*word [0, 0, 0, 0, -1, 1]*)
  mzv[-1,-5] -> 31/16*Log[2]*mzv[5]-39/70*mzv[2]^3+3/4*mzv[3]^2-mzv[1,-5],

  (*word [0, 0, 0, 0, 0, -1]*)
  -mzv[-6] -> 31/140*mzv[2]^3,

  (*word [0, 0, 0, 0, 0, 1]*)
  -mzv[6] -> -8/35*mzv[2]^3,

  (*word [0, 0, 0, 0, 1, -1]*)
  mzv[-1,5] -> -31/16*Log[2]*mzv[5]+111/280*mzv[2]^3-9/32*mzv[3]^2,

  (*word [0, 0, 0, 0, 1, 1]*)
  mzv[1,5] -> 6/35*mzv[2]^3-1/2*mzv[3]^2,

  (*word [0, 0, 0, 1, -1, -1]*)
  -mzv[1,-1,4] -> -3/8*Log[2]^2*mzv[2]^2-7/8*Log[2]*mzv[2]*mzv[3]+93/32*Log[2]*mzv[5]-7/40*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+3/16*mzv[3]^2+mzv[1,-5],

  (*word [0, 0, 0, 1, -1, 1]*)
  -mzv[-1,-1,4] -> -3/4*Log[2]^2*mzv[2]^2-9/8*Log[2]*mzv[2]*mzv[3]+155/32*Log[2]*mzv[5]-121/140*mzv[2]^3+131/64*mzv[3]^2-5/2*mzv[1,-5],

  (*word [0, 0, 0, 1, 0, -1]*)
  mzv[-2,4] -> 221/840*mzv[2]^3-15/16*mzv[3]^2+4*mzv[1,-5],

  (*word [0, 0, 0, 1, 0, 1]*)
  mzv[2,4] -> -32/105*mzv[2]^3+mzv[3]^2,

  (*word [0, 0, 0, 1, 1, -1]*)
  -mzv[-1,1,4] -> 3/8*Log[2]^2*mzv[2]^2-5/8*Log[2]*mzv[2]*mzv[3]+9/70*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]-7/64*mzv[3]^2+3/2*mzv[1,-5],

  (*word [0, 0, 0, 1, 1, 1]*)
  -mzv[1,1,4] -> -23/70*mzv[2]^3+mzv[3]^2,

  (*word [0, 0, 1, -1, -1, -1]*)
  mzv[1,1,-1,3] -> -7/24*Log[2]^3*mzv[3]+19/80*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]+29/16*Log[2]*mzv[2]*mzv[3]-61/16*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]-33/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+43/128*mzv[3]^2-3/2*mzv[1,-5],

  (*word [0, 0, 1, -1, -1, 1]*)
  mzv[-1,1,-1,3] -> -3/80*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]+57/16*Log[2]*mzv[2]*mzv[3]-209/32*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+731/560*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-547/128*mzv[3]^2+15/2*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [0, 0, 1, -1, 0, -1]*)
  -mzv[2,-1,3] -> 7/8*Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]-23/60*mzv[2]^3-mzv[2]*mzv[1,-3]+47/32*mzv[3]^2-4*mzv[1,-5],

  (*word [0, 0, 1, -1, 0, 1]*)
  -mzv[-2,-1,3] -> 21/4*Log[2]*mzv[2]*mzv[3]-155/16*Log[2]*mzv[5]+137/120*mzv[2]^3-2*mzv[2]*mzv[1,-3]-227/64*mzv[3]^2+5*mzv[1,-5],

  (*word [0, 0, 1, -1, 1, -1]*)
  mzv[-1,-1,-1,3] -> -7/6*Log[2]^3*mzv[3]+41/40*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]+15/8*Log[2]*mzv[2]*mzv[3]-345/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]+111/280*mzv[2]^3-15/16*mzv[3]^2,

  (*word [0, 0, 1, -1, 1, 1]*)
  mzv[1,-1,-1,3] -> -7/8*Log[2]^3*mzv[3]+67/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]+5/2*Log[2]*mzv[2]*mzv[3]-411/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]+303/280*mzv[2]^3-3/2*mzv[2]*mzv[1,-3]-187/64*mzv[3]^2+5/4*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [0, 0, 1, 0, -1, -1]*)
  -mzv[1,-2,3] -> -347/1680*mzv[2]^3+43/64*mzv[3]^2-5/2*mzv[1,-5],

  (*word [0, 0, 1, 0, -1, 1]*)
  -mzv[-1,-2,3] -> -11/4*Log[2]*mzv[2]*mzv[3]+155/32*Log[2]*mzv[5]-109/210*mzv[2]^3+2*mzv[2]*mzv[1,-3]+53/32*mzv[3]^2-5/2*mzv[1,-5],

  (*word [0, 0, 1, 0, 0, -1]*)
  mzv[-3,3] -> -113/280*mzv[2]^3+39/32*mzv[3]^2-6*mzv[1,-5],

  (*word [0, 0, 1, 0, 0, 1]*)
  mzv[3,3] -> -4/35*mzv[2]^3+1/2*mzv[3]^2,

  (*word [0, 0, 1, 0, 1, -1]*)
  -mzv[-1,2,3] -> 11/4*Log[2]*mzv[2]*mzv[3]-155/32*Log[2]*mzv[5]+691/1680*mzv[2]^3+mzv[2]*mzv[1,-3]-49/32*mzv[3]^2-mzv[1,-5],

  (*word [0, 0, 1, 0, 1, 1]*)
  -mzv[1,2,3] -> 29/30*mzv[2]^3-3*mzv[3]^2,

  (*word [0, 0, 1, 1, -1, -1]*)
  mzv[1,-1,1,3] -> 7/12*Log[2]^3*mzv[3]-17/40*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-19/8*Log[2]*mzv[2]*mzv[3]+329/64*Log[2]*mzv[5]+3*Log[2]*mzv[1,1,-3]-41/40*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]+405/128*mzv[3]^2-15/2*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [0, 0, 1, 1, -1, 1]*)
  mzv[-1,-1,1,3] -> 7/8*Log[2]^3*mzv[3]-7/10*Log[2]^2*mzv[2]^2-1/2*Log[2]^2*mzv[1,-3]-31/16*Log[2]*mzv[2]*mzv[3]+155/32*Log[2]*mzv[5]-47/56*mzv[2]^3+mzv[2]*mzv[1,-3]+301/128*mzv[3]^2-5/2*mzv[1,-5],

  (*word [0, 0, 1, 1, 0, -1]*)
  -mzv[-2,1,3] -> 5/336*mzv[2]^3+1/64*mzv[3]^2-mzv[1,-5],

  (*word [0, 0, 1, 1, 0, 1]*)
  -mzv[2,1,3] -> -53/105*mzv[2]^3+3/2*mzv[3]^2,

  (*word [0, 0, 1, 1, 1, -1]*)
  mzv[-1,1,1,3] -> -7/24*Log[2]^3*mzv[3]+3/16*Log[2]^2*mzv[2]^2+25/16*Log[2]*mzv[2]*mzv[3]-213/64*Log[2]*mzv[5]-Log[2]*mzv[1,1,-3]+379/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-67/32*mzv[3]^2+11/4*mzv[1,-5]-2*mzv[1,1,1,-3],

  (*word [0, 0, 1, 1, 1, 1]*)
  mzv[1,1,1,3] -> 6/35*mzv[2]^3-1/2*mzv[3]^2,

  (*word [0, 1, -1, -1, -1, -1]*)
  -mzv[1,1,1,-1,2] -> -1/16*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/40*Log[2]^2*mzv[2]^2+3/4*Log[2]^2*mzv[1,-3]-19/16*Log[2]*mzv[2]*mzv[3]+149/64*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-1/80*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]+1/128*mzv[3]^2+mzv[1,1,1,-3],

  (*word [0, 1, -1, -1, -1, 1]*)
  -mzv[-1,1,1,-1,2] -> -1/8*Log[2]^4*mzv[2]+7/16*Log[2]^3*mzv[3]-19/80*Log[2]^2*mzv[2]^2+1/2*Log[2]^2*mzv[1,-3]-33/16*Log[2]*mzv[2]*mzv[3]+273/64*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]-31/40*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]+159/64*mzv[3]^2-15/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [0, 1, -1, -1, 0, -1]*)
  mzv[2,1,-1,2] -> -1/5*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-37/16*Log[2]*mzv[2]*mzv[3]+309/64*Log[2]*mzv[5]+8*Log[2]*mzv[1,1,-3]-583/420*mzv[2]^3+9/4*mzv[2]*mzv[1,-3]+271/64*mzv[3]^2-14*mzv[1,-5]+12*mzv[1,1,1,-3],

  (*word [0, 1, -1, -1, 0, 1]*)
  mzv[-2,1,-1,2] -> 3/16*Log[2]^2*mzv[2]^2-57/16*Log[2]*mzv[2]*mzv[3]+403/64*Log[2]*mzv[5]-599/840*mzv[2]^3+301/128*mzv[3]^2-13/4*mzv[1,-5],

  (*word [0, 1, -1, -1, 1, -1]*)
  -mzv[-1,-1,1,-1,2] -> 3/16*Log[2]^4*mzv[2]-7/8*Log[2]^3*mzv[3]+33/80*Log[2]^2*mzv[2]^2+3/2*Log[2]^2*mzv[1,-3]-3/4*Log[2]*mzv[2]*mzv[3]+7/8*Log[2]*mzv[5]+3/2*Log[2]*mzv[1,1,-3]+277/560*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]-105/64*mzv[3]^2+7/4*mzv[1,-5],

  (*word [0, 1, -1, -1, 1, 1]*)
  -mzv[1,-1,1,-1,2] -> 1/8*Log[2]^4*mzv[2]-7/8*Log[2]^3*mzv[3]+21/40*Log[2]^2*mzv[2]^2+9/4*Log[2]^2*mzv[1,-3]-27/8*Log[2]*mzv[2]*mzv[3]+177/32*Log[2]*mzv[5]+9/2*Log[2]*mzv[1,1,-3]-361/280*mzv[2]^3+543/128*mzv[3]^2-15/2*mzv[1,-5]+9/2*mzv[1,1,1,-3],

  (*word [0, 1, -1, 0, -1, -1]*)
  mzv[1,2,-1,2] -> 1/16*Log[2]*mzv[2]*mzv[3]-15/64*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+279/140*mzv[2]^3-5/4*mzv[2]*mzv[1,-3]-101/16*mzv[3]^2+37/2*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [0, 1, -1, 0, -1, 1]*)
  mzv[-1,2,-1,2] -> -31/40*Log[2]^2*mzv[2]^2+4*Log[2]^2*mzv[1,-3]-4*Log[2]*mzv[2]*mzv[3]+317/32*Log[2]*mzv[5]+12*Log[2]*mzv[1,1,-3]-177/70*mzv[2]^3-2*mzv[2]*mzv[1,-3]+125/16*mzv[3]^2-35/2*mzv[1,-5]+12*mzv[1,1,1,-3],

  (*word [0, 1, -1, 0, 0, -1]*)
  -mzv[3,-1,2] -> 5/8*Log[2]*mzv[2]*mzv[3]-31/16*Log[2]*mzv[5]+23/40*mzv[2]^3-mzv[2]*mzv[1,-3]-3/2*mzv[3]^2+6*mzv[1,-5],

  (*word [0, 1, -1, 0, 0, 1]*)
  -mzv[-3,-1,2] -> -37/8*Log[2]*mzv[2]*mzv[3]+155/16*Log[2]*mzv[5]-199/280*mzv[2]^3+2*mzv[2]*mzv[1,-3]+7/4*mzv[3]^2-5*mzv[1,-5],

  (*word [0, 1, -1, 0, 1, -1]*)
  mzv[-1,-2,-1,2] -> 31/40*Log[2]^2*mzv[2]^2-4*Log[2]^2*mzv[1,-3]+19/4*Log[2]*mzv[2]*mzv[3]-711/64*Log[2]*mzv[5]-16*Log[2]*mzv[1,1,-3]+135/28*mzv[2]^3+mzv[2]*mzv[1,-3]-61/4*mzv[3]^2+38*mzv[1,-5]-24*mzv[1,1,1,-3],

  (*word [0, 1, -1, 0, 1, 1]*)
  mzv[1,-2,-1,2] -> -59/16*Log[2]*mzv[2]*mzv[3]+217/32*Log[2]*mzv[5]-247/280*mzv[2]^3+2*mzv[2]*mzv[1,-3]+43/16*mzv[3]^2-7/2*mzv[1,-5],

  (*word [0, 1, -1, 1, -1, -1]*)
  -mzv[1,-1,-1,-1,2] -> -7/16*Log[2]^4*mzv[2]+35/24*Log[2]^3*mzv[3]-43/80*Log[2]^2*mzv[2]^2-1/4*Log[2]^2*mzv[1,-3]-47/16*Log[2]*mzv[2]*mzv[3]+393/64*Log[2]*mzv[5]+5/2*Log[2]*mzv[1,1,-3]-659/560*mzv[2]^3-3/2*mzv[2]*mzv[1,-3]+245/64*mzv[3]^2-23/4*mzv[1,-5]+4*mzv[1,1,1,-3],

  (*word [0, 1, -1, 1, -1, 1]*)
  -mzv[-1,-1,-1,-1,2] -> -1/2*Log[2]^4*mzv[2]+7/4*Log[2]^3*mzv[3]-3/5*Log[2]^2*mzv[2]^2-3/2*Log[2]^2*mzv[1,-3]-21/16*Log[2]*mzv[2]*mzv[3]+3*Log[2]*mzv[5]-3/2*Log[2]*mzv[1,1,-3]-31/280*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+15/64*mzv[3]^2-3/2*mzv[1,1,1,-3],

  (*word [0, 1, -1, 1, 0, -1]*)
  mzv[-2,-1,-1,2] -> -2/5*Log[2]^2*mzv[2]^2+4*Log[2]^2*mzv[1,-3]-129/16*Log[2]*mzv[2]*mzv[3]+263/16*Log[2]*mzv[5]+16*Log[2]*mzv[1,1,-3]-101/21*mzv[2]^3-11/4*mzv[2]*mzv[1,-3]+31/2*mzv[3]^2-33*mzv[1,-5]+24*mzv[1,1,1,-3],

  (*word [0, 1, -1, 1, 0, 1]*)
  mzv[2,-1,-1,2] -> 3/8*Log[2]^2*mzv[2]^2-23/4*Log[2]*mzv[2]*mzv[3]+155/16*Log[2]*mzv[5]-479/840*mzv[2]^3+7/2*mzv[2]*mzv[1,-3]+7/4*mzv[3]^2-5*mzv[1,-5],

  (*word [0, 1, -1, 1, 1, -1]*)
  -mzv[-1,1,-1,-1,2] -> -3/16*Log[2]^4*mzv[2]+21/16*Log[2]^3*mzv[3]-59/80*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]-7/8*Log[2]*mzv[2]*mzv[3]+169/64*Log[2]*mzv[5]-7/2*Log[2]*mzv[1,1,-3]+163/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-33/32*mzv[3]^2+23/4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [0, 1, -1, 1, 1, 1]*)
  -mzv[1,1,-1,-1,2] -> -1/4*Log[2]^4*mzv[2]+21/16*Log[2]^3*mzv[3]-9/10*Log[2]^2*mzv[2]^2-9/4*Log[2]^2*mzv[1,-3]-5/4*Log[2]*mzv[2]*mzv[3]+133/32*Log[2]*mzv[5]-9/2*Log[2]*mzv[1,1,-3]-9/140*mzv[2]^3+2*mzv[2]*mzv[1,-3]-11/32*mzv[3]^2+5/2*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [0, 1, 0, -1, -1, -1]*)
  mzv[1,1,-2,2] -> -121/840*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]+51/128*mzv[3]^2-2*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [0, 1, 0, -1, -1, 1]*)
  mzv[-1,1,-2,2] -> 23/80*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+17/16*Log[2]*mzv[2]*mzv[3]-201/64*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+127/168*mzv[2]^3-271/128*mzv[3]^2+23/4*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [0, 1, 0, -1, 0, -1]*)
  -mzv[2,-2,2] -> 79/140*mzv[2]^3+4*mzv[2]*mzv[1,-3]-35/16*mzv[3]^2,

  (*word [0, 1, 0, -1, 0, 1]*)
  -mzv[-2,-2,2] -> -19/70*mzv[2]^3-2*mzv[2]*mzv[1,-3]+35/32*mzv[3]^2,

  (*word [0, 1, 0, -1, 1, -1]*)
  mzv[-1,-1,-2,2] -> -23/40*Log[2]^2*mzv[2]^2+2*Log[2]^2*mzv[1,-3]-13/8*Log[2]*mzv[2]*mzv[3]+317/64*Log[2]*mzv[5]+6*Log[2]*mzv[1,1,-3]-611/420*mzv[2]^3+mzv[2]*mzv[1,-3]+67/16*mzv[3]^2-13*mzv[1,-5]+8*mzv[1,1,1,-3],

  (*word [0, 1, 0, -1, 1, 1]*)
  mzv[1,-1,-2,2] -> -23/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]+5/2*Log[2]*mzv[2]*mzv[3]-225/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]-209/420*mzv[2]^3-3*mzv[2]*mzv[1,-3]+23/16*mzv[3]^2-1/4*mzv[1,-5]+2*mzv[1,1,1,-3],

  (*word [0, 1, 0, 0, -1, -1]*)
  -mzv[1,-3,2] -> 17/840*mzv[2]^3-3/2*mzv[2]*mzv[1,-3]+1/64*mzv[3]^2+5/2*mzv[1,-5],

  (*word [0, 1, 0, 0, -1, 1]*)
  -mzv[-1,-3,2] -> 15/8*Log[2]*mzv[2]*mzv[3]-155/32*Log[2]*mzv[5]+701/840*mzv[2]^3-125/64*mzv[3]^2+5/2*mzv[1,-5],

  (*word [0, 1, 0, 0, 0, -1]*)
  mzv[-4,2] -> 67/840*mzv[2]^3-3/4*mzv[3]^2+4*mzv[1,-5],

  (*word [0, 1, 0, 0, 0, 1]*)
  mzv[4,2] -> 10/21*mzv[2]^3-mzv[3]^2,

  (*word [0, 1, 0, 0, 1, -1]*)
  -mzv[-1,3,2] -> -15/8*Log[2]*mzv[2]*mzv[3]+155/32*Log[2]*mzv[5]-857/1680*mzv[2]^3+mzv[3]^2-mzv[1,-5],

  (*word [0, 1, 0, 0, 1, 1]*)
  -mzv[1,3,2] -> -53/105*mzv[2]^3+3/2*mzv[3]^2,

  (*word [0, 1, 0, 1, -1, -1]*)
  mzv[1,-1,2,2] -> 23/80*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+9/16*Log[2]*mzv[2]*mzv[3]-29/16*Log[2]*mzv[5]-2*Log[2]*mzv[1,1,-3]-5/168*mzv[2]^3-2*mzv[2]*mzv[1,-3]+25/64*mzv[3]^2+5/2*mzv[1,-5],

  (*word [0, 1, 0, 1, -1, 1]*)
  mzv[-1,-1,2,2] -> 23/40*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+13/8*Log[2]*mzv[2]*mzv[3]-317/64*Log[2]*mzv[5]-6*Log[2]*mzv[1,1,-3]+313/240*mzv[2]^3+mzv[2]*mzv[1,-3]-247/64*mzv[3]^2+35/4*mzv[1,-5]-6*mzv[1,1,1,-3],

  (*word [0, 1, 0, 1, 0, -1]*)
  -mzv[-2,2,2] -> 3/112*mzv[2]^3-3*mzv[2]*mzv[1,-3]+1/4*mzv[3]^2+4*mzv[1,-5],

  (*word [0, 1, 0, 1, 0, 1]*)
  -mzv[2,2,2] -> -3/70*mzv[2]^3,

  (*word [0, 1, 0, 1, 1, -1]*)
  mzv[-1,1,2,2] -> -23/80*Log[2]^2*mzv[2]^2+Log[2]^2*mzv[1,-3]-33/8*Log[2]*mzv[2]*mzv[3]+271/32*Log[2]*mzv[5]+4*Log[2]*mzv[1,1,-3]-2381/1680*mzv[2]^3-5/4*mzv[2]*mzv[1,-3]+145/32*mzv[3]^2-29/4*mzv[1,-5]+6*mzv[1,1,1,-3],

  (*word [0, 1, 0, 1, 1, 1]*)
  mzv[1,1,2,2] -> -32/105*mzv[2]^3+mzv[3]^2,

  (*word [0, 1, 1, -1, -1, -1]*)
  -mzv[1,1,-1,1,2] -> 3/16*Log[2]^4*mzv[2]-7/24*Log[2]^3*mzv[3]+1/10*Log[2]^2*mzv[2]^2-Log[2]^2*mzv[1,-3]+21/8*Log[2]*mzv[2]*mzv[3]-325/64*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+57/56*mzv[2]^3+1/2*mzv[2]*mzv[1,-3]-423/128*mzv[3]^2+27/4*mzv[1,-5]-9/2*mzv[1,1,1,-3],

  (*word [0, 1, 1, -1, -1, 1]*)
  -mzv[-1,1,-1,1,2] -> 1/8*Log[2]^4*mzv[2]-1/16*Log[2]^2*mzv[2]^2-5/4*Log[2]^2*mzv[1,-3]+7/2*Log[2]*mzv[2]*mzv[3]-209/32*Log[2]*mzv[5]-4*Log[2]*mzv[1,1,-3]+577/560*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-105/32*mzv[3]^2+15/2*mzv[1,-5]-4*mzv[1,1,1,-3],

  (*word [0, 1, 1, -1, 0, -1]*)
  mzv[2,-1,1,2] -> 1/5*Log[2]^2*mzv[2]^2-2*Log[2]^2*mzv[1,-3]+57/16*Log[2]*mzv[2]*mzv[3]-433/64*Log[2]*mzv[5]-8*Log[2]*mzv[1,1,-3]+1987/840*mzv[2]^3+11/4*mzv[2]*mzv[1,-3]-507/64*mzv[3]^2+15*mzv[1,-5]-12*mzv[1,1,1,-3],

  (*word [0, 1, 1, -1, 0, 1]*)
  mzv[-2,-1,1,2] -> -3/16*Log[2]^2*mzv[2]^2+21/4*Log[2]*mzv[2]*mzv[3]-651/64*Log[2]*mzv[5]+487/840*mzv[2]^3-7/2*mzv[2]*mzv[1,-3]-177/128*mzv[3]^2+21/4*mzv[1,-5],

  (*word [0, 1, 1, -1, 1, -1]*)
  -mzv[-1,-1,-1,1,2] -> 7/16*Log[2]^4*mzv[2]-21/16*Log[2]^3*mzv[3]+31/80*Log[2]^2*mzv[2]^2+7/4*Log[2]^2*mzv[1,-3]+25/16*Log[2]*mzv[2]*mzv[3]-225/64*Log[2]*mzv[5]+2*Log[2]*mzv[1,1,-3]+103/560*mzv[2]^3+3/4*mzv[2]*mzv[1,-3]-33/64*mzv[3]^2-7/4*mzv[1,-5]+1/2*mzv[1,1,1,-3],

  (*word [0, 1, 1, -1, 1, 1]*)
  -mzv[1,-1,-1,1,2] -> 3/8*Log[2]^4*mzv[2]-21/16*Log[2]^3*mzv[3]+1/2*Log[2]^2*mzv[2]^2+5/2*Log[2]^2*mzv[1,-3]+37/16*Log[2]*mzv[2]*mzv[3]-165/32*Log[2]*mzv[5]+5*Log[2]*mzv[1,1,-3]-5/14*mzv[2]^3-3*mzv[2]*mzv[1,-3]+209/128*mzv[3]^2-5/2*mzv[1,-5]+5*mzv[1,1,1,-3],

  (*word [0, 1, 1, 0, -1, -1]*)
  mzv[1,-2,1,2] -> 1/16*mzv[2]^3-21/128*mzv[3]^2+3/4*mzv[1,-5],

  (*word [0, 1, 1, 0, -1, 1]*)
  mzv[-1,-2,1,2] -> -35/16*Log[2]*mzv[2]*mzv[3]+341/64*Log[2]*mzv[5]-53/560*mzv[2]^3+2*mzv[2]*mzv[1,-3]-63/128*mzv[3]^2-11/4*mzv[1,-5],

  (*word [0, 1, 1, 0, 0, -1]*)
  -mzv[-3,1,2] -> -1/140*mzv[2]^3+3/2*mzv[2]*mzv[1,-3]+1/4*mzv[3]^2-mzv[1,-5],

  (*word [0, 1, 1, 0, 0, 1]*)
  -mzv[3,1,2] -> 13/70*mzv[2]^3-mzv[3]^2,

  (*word [0, 1, 1, 0, 1, -1]*)
  mzv[-1,2,1,2] -> 35/16*Log[2]*mzv[2]*mzv[3]-341/64*Log[2]*mzv[5]+87/560*mzv[2]^3+1/4*mzv[2]*mzv[1,-3]-mzv[1,-5],

  (*word [0, 1, 1, 0, 1, 1]*)
  mzv[1,2,1,2] -> -4/35*mzv[2]^3+1/2*mzv[3]^2,

  (*word [0, 1, 1, 1, -1, -1]*)
  -mzv[1,-1,1,1,2] -> -3/16*Log[2]^4*mzv[2]+7/48*Log[2]^3*mzv[3]-3/16*Log[2]^2*mzv[2]^2-25/16*Log[2]*mzv[2]*mzv[3]+213/64*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-309/560*mzv[2]^3-1/4*mzv[2]*mzv[1,-3]+55/32*mzv[3]^2-3*mzv[1,-5]+5/2*mzv[1,1,1,-3],

  (*word [0, 1, 1, 1, -1, 1]*)
  -mzv[-1,-1,1,1,2] -> -1/4*Log[2]^4*mzv[2]+7/16*Log[2]^3*mzv[3]-1/4*Log[2]^2*mzv[2]^2-5/4*Log[2]^2*mzv[1,-3]-25/16*Log[2]*mzv[2]*mzv[3]+229/64*Log[2]*mzv[5]-3*Log[2]*mzv[1,1,-3]+7/16*mzv[2]^3+7/4*mzv[2]*mzv[1,-3]-49/32*mzv[3]^2+5/4*mzv[1,-5]-3*mzv[1,1,1,-3],

  (*word [0, 1, 1, 1, 0, -1]*)
  mzv[-2,1,1,2] -> -11/336*mzv[2]^3-3/4*mzv[2]*mzv[1,-3]-3/16*mzv[3]^2+5/2*mzv[1,-5],

  (*word [0, 1, 1, 1, 0, 1]*)
  mzv[2,1,1,2] -> 10/21*mzv[2]^3-mzv[3]^2,

  (*word [0, 1, 1, 1, 1, -1]*)
  -mzv[-1,1,1,1,2] -> 1/16*Log[2]^4*mzv[2]+13/80*Log[2]^2*mzv[2]^2+1/4*Log[2]^2*mzv[1,-3]+3/8*Log[2]*mzv[2]*mzv[3]-1/16*Log[2]*mzv[5]+Log[2]*mzv[1,1,-3]-13/35*mzv[2]^3-1/2*mzv[2]*mzv[1,-3]+mzv[3]^2-5/4*mzv[1,-5]+3/2*mzv[1,1,1,-3],

  (*word [0, 1, 1, 1, 1, 1]*)
  -mzv[1,1,1,1,2] -> -8/35*mzv[2]^3
}/.Rule[-mzv[X___],a_]:>Rule[mzv[X],-a],
{ mzv[1] -> 0,
  mzv[1,1] -> 0,
  mzv[2] -> ((1/6)*(Pi^2)),
  mzv[1,1,1] -> 0,
  mzv[1,2] -> Zeta[3],
  mzv[2,1] -> (-2*Zeta[3]),
  mzv[3] -> Zeta[3],
  mzv[1,1,1,1] -> 0,
  mzv[1,1,2] -> ((1/90)*(Pi^4)),
  mzv[1,2,1] -> ((-1/30)*(Pi^4)),
  mzv[1,3] -> ((1/360)*(Pi^4)),
  mzv[2,1,1] -> ((1/30)*(Pi^4)),
  mzv[2,2] -> ((1/120)*(Pi^4)),
  mzv[3,1] -> ((-1/72)*(Pi^4)),
  mzv[4] -> ((1/90)*(Pi^4)),
  mzv[1,1,1,1,1] -> 0,
  mzv[1,1,1,2] -> Zeta[5],
  mzv[1,1,2,1] -> (-4*Zeta[5]),
  mzv[1,1,3] -> (((-1/6)*(Pi^2)*Zeta[3])+(2*Zeta[5])),
  mzv[1,2,1,1] -> (6*Zeta[5]),
  mzv[1,2,2] -> (((-11/2)*Zeta[5])+((1/2)*(Pi^2)*Zeta[3])),
  mzv[1,3,1] -> ((-1/2)*Zeta[5]),
  mzv[1,4] -> (((-1/6)*(Pi^2)*Zeta[3])+(2*Zeta[5])),
  mzv[2,1,1,1] -> (-4*Zeta[5]),
  mzv[2,1,2] -> (((9/2)*Zeta[5])+((-1/3)*(Pi^2)*Zeta[3])),
  mzv[2,2,1] -> (((-1/3)*(Pi^2)*Zeta[3])+(2*Zeta[5])),
  mzv[2,3] -> (((-11/2)*Zeta[5])+((1/2)*(Pi^2)*Zeta[3])),
  mzv[3,1,1] -> (((1/6)*(Pi^2)*Zeta[3])+((-1/2)*Zeta[5])),
  mzv[3,2] -> (((9/2)*Zeta[5])+((-1/3)*(Pi^2)*Zeta[3])),
  mzv[4,1] -> (((1/6)*(Pi^2)*Zeta[3])+(-3*Zeta[5])),
  mzv[5] -> Zeta[5],
  mzv[1,1,1,1,1,1] -> 0,
  mzv[1,1,1,1,2] -> ((1/945)*(Pi^6)),
  mzv[1,1,1,2,1] -> ((-1/189)*(Pi^6)),
  mzv[1,1,1,3] -> (((1/1260)*(Pi^6))+((-1/2)*(Zeta[3]^2))),
  mzv[1,1,2,1,1] -> ((2/189)*(Pi^6)),
  mzv[1,1,2,2] -> (((-4/2835)*(Pi^6))+(Zeta[3]^2)),
  mzv[1,1,3,1] -> (((-1/567)*(Pi^6))+(Zeta[3]^2)),
  mzv[1,1,4] -> (((23/15120)*(Pi^6))+(-1*(Zeta[3]^2))),
  mzv[1,2,1,1,1] -> ((-2/189)*(Pi^6)),
  mzv[1,2,1,2] -> (((-1/1890)*(Pi^6))+((1/2)*(Zeta[3]^2))),
  mzv[1,2,2,1] -> (((1/189)*(Pi^6))+(-4*(Zeta[3]^2))),
  mzv[1,2,3] -> (((-29/6480)*(Pi^6))+(3*(Zeta[3]^2))),
  mzv[1,3,1,1] -> ((1/2)*(Zeta[3]^2)),
  mzv[1,3,2] -> (((53/22680)*(Pi^6))+((-3/2)*(Zeta[3]^2))),
  mzv[1,4,1] -> (((-11/4536)*(Pi^6))+((3/2)*(Zeta[3]^2))),
  mzv[1,5] -> (((1/1260)*(Pi^6))+((-1/2)*(Zeta[3]^2))),
  mzv[2,1,1,1,1] -> ((1/189)*(Pi^6)),
  mzv[2,1,1,2] -> (((5/2268)*(Pi^6))+(-1*(Zeta[3]^2))),
  mzv[2,1,2,1] -> (((-1/180)*(Pi^6))+(2*(Zeta[3]^2))),
  mzv[2,1,3] -> (((53/22680)*(Pi^6))+((-3/2)*(Zeta[3]^2))),
  mzv[2,2,1,1] -> (((1/3780)*(Pi^6))+(2*(Zeta[3]^2))),
  mzv[2,2,2] -> ((1/5040)*(Pi^6)),
  mzv[2,3,1] -> ((-3*(Zeta[3]^2))+((37/9072)*(Pi^6))),
  mzv[2,4] -> (((-4/2835)*(Pi^6))+(Zeta[3]^2)),
  mzv[3,1,1,1] -> (((-1/11340)*(Pi^6))+(-1*(Zeta[3]^2))),
  mzv[3,1,2] -> (((-13/15120)*(Pi^6))+(Zeta[3]^2)),
  mzv[3,2,1] -> (((-143/45360)*(Pi^6))+(Zeta[3]^2)),
  mzv[3,3] -> (((-1/1890)*(Pi^6))+((1/2)*(Zeta[3]^2))),
  mzv[4,1,1] -> (((89/45360)*(Pi^6))+((-1/2)*(Zeta[3]^2))),
  mzv[4,2] -> (((5/2268)*(Pi^6))+(-1*(Zeta[3]^2))),
  mzv[5,1] -> (((-1/540)*(Pi^6))+((1/2)*(Zeta[3]^2))),
  mzv[6] -> ((1/945)*(Pi^6)),
  mzv[1,1,1,1,1,1,1] -> 0,
  mzv[1,1,1,1,1,2] -> Zeta[7],
  mzv[1,1,1,1,2,1] -> (-6*Zeta[7]),
  mzv[1,1,1,1,3] -> (((-1/6)*(Pi^2)*Zeta[5])+((-1/90)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[1,1,1,2,1,1] -> (15*Zeta[7]),
  mzv[1,1,1,2,2] -> (((5/6)*(Pi^2)*Zeta[5])+((1/45)*(Pi^4)*Zeta[3])+(-11*Zeta[7])),
  mzv[1,1,1,3,1] -> (((1/30)*(Pi^4)*Zeta[3])+(-4*Zeta[7])),
  mzv[1,1,1,4] -> (((-1/3)*(Pi^2)*Zeta[5])+((-1/72)*(Pi^4)*Zeta[3])+(5*Zeta[7])),
  mzv[1,1,2,1,1,1] -> (-20*Zeta[7]),
  mzv[1,1,2,1,2] -> (((-5/3)*(Pi^2)*Zeta[5])+(17*Zeta[7])),
  mzv[1,1,2,2,1] -> (((-4/45)*(Pi^4)*Zeta[3])+(10*Zeta[7])),
  mzv[1,1,2,3] -> (((11/12)*(Pi^2)*Zeta[5])+((7/180)*(Pi^4)*Zeta[3])+((-221/16)*Zeta[7])),
  mzv[1,1,3,1,1] -> (((-1/45)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[1,1,3,2] -> (((5/12)*(Pi^2)*Zeta[5])+((-1/24)*(Pi^4)*Zeta[3])+((5/8)*Zeta[7])),
  mzv[1,1,4,1] -> (((7/120)*(Pi^4)*Zeta[3])+((-109/16)*Zeta[7])),
  mzv[1,1,5] -> (((-1/3)*(Pi^2)*Zeta[5])+((-1/72)*(Pi^4)*Zeta[3])+(5*Zeta[7])),
  mzv[1,2,1,1,1,1] -> (15*Zeta[7]),
  mzv[1,2,1,1,2] -> (((5/3)*(Pi^2)*Zeta[5])+((1/90)*(Pi^4)*Zeta[3])+(-18*Zeta[7])),
  mzv[1,2,1,2,1] -> (((-1/30)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[1,2,1,3] -> (((-3/4)*(Pi^2)*Zeta[5])+((61/8)*Zeta[7])),
  mzv[1,2,2,1,1] -> (((1/6)*(Pi^4)*Zeta[3])+(-18*Zeta[7])),
  mzv[1,2,2,2] -> (((-5/4)*(Pi^2)*Zeta[5])+((1/40)*(Pi^4)*Zeta[3])+((157/16)*Zeta[7])),
  mzv[1,2,3,1] -> (((-17/120)*(Pi^4)*Zeta[3])+((131/8)*Zeta[7])),
  mzv[1,2,4] -> (((11/12)*(Pi^2)*Zeta[5])+((7/180)*(Pi^4)*Zeta[3])+((-221/16)*Zeta[7])),
  mzv[1,3,1,1,1] -> (((-1/30)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[1,3,1,2] -> (((1/360)*(Pi^4)*Zeta[3])+((-1/4)*Zeta[7])),
  mzv[1,3,2,1] -> (((17/180)*(Pi^4)*Zeta[3])+((-179/16)*Zeta[7])),
  mzv[1,3,3] -> (((-3/4)*(Pi^2)*Zeta[5])+((61/8)*Zeta[7])),
  mzv[1,4,1,1] -> (((-23/360)*(Pi^4)*Zeta[3])+((61/8)*Zeta[7])),
  mzv[1,4,2] -> (((-109/16)*Zeta[7])+((-1/72)*(Pi^4)*Zeta[3])+((5/6)*(Pi^2)*Zeta[5])),
  mzv[1,5,1] -> (((1/60)*(Pi^4)*Zeta[3])+(-2*Zeta[7])),
  mzv[1,6] -> (((-1/6)*(Pi^2)*Zeta[5])+((-1/90)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[2,1,1,1,1,1] -> (-6*Zeta[7]),
  mzv[2,1,1,1,2] -> (((-2/3)*(Pi^2)*Zeta[5])+((-1/45)*(Pi^4)*Zeta[3])+(10*Zeta[7])),
  mzv[2,1,1,2,1] -> (((-2/3)*(Pi^2)*Zeta[5])+((1/15)*(Pi^4)*Zeta[3])+(-4*Zeta[7])),
  mzv[2,1,1,3] -> (((-109/16)*Zeta[7])+((-1/72)*(Pi^4)*Zeta[3])+((5/6)*(Pi^2)*Zeta[5])),
  mzv[2,1,2,1,1] -> (((Pi^2)*Zeta[5])+((-1/15)*(Pi^4)*Zeta[3])+(3*Zeta[7])),
  mzv[2,1,2,2] -> (((-11/12)*(Pi^2)*Zeta[5])+((75/8)*Zeta[7])),
  mzv[2,1,3,1] -> (((-1/12)*(Pi^2)*Zeta[5])+((1/24)*(Pi^4)*Zeta[3])+((-67/16)*Zeta[7])),
  mzv[2,1,4] -> (((5/12)*(Pi^2)*Zeta[5])+((-1/24)*(Pi^4)*Zeta[3])+((5/8)*Zeta[7])),
  mzv[2,2,1,1,1] -> (((-2/3)*(Pi^2)*Zeta[5])+((-1/15)*(Pi^4)*Zeta[3])+(10*Zeta[7])),
  mzv[2,2,1,2] -> ((2*(Pi^2)*Zeta[5])+((-1/60)*(Pi^4)*Zeta[3])+((-291/16)*Zeta[7])),
  mzv[2,2,2,1] -> (((1/3)*(Pi^2)*Zeta[5])+((-1/60)*(Pi^4)*Zeta[3])+(-2*Zeta[7])),
  mzv[2,2,3] -> (((-5/4)*(Pi^2)*Zeta[5])+((1/40)*(Pi^4)*Zeta[3])+((157/16)*Zeta[7])),
  mzv[2,3,1,1] -> (((-1/12)*(Pi^2)*Zeta[5])+((13/120)*(Pi^4)*Zeta[3])+((-179/16)*Zeta[7])),
  mzv[2,3,2] -> (((-11/12)*(Pi^2)*Zeta[5])+((75/8)*Zeta[7])),
  mzv[2,4,1] -> (((-1/2)*(Pi^2)*Zeta[5])+((-7/360)*(Pi^4)*Zeta[3])+((115/16)*Zeta[7])),
  mzv[2,5] -> (((5/6)*(Pi^2)*Zeta[5])+((1/45)*(Pi^4)*Zeta[3])+(-11*Zeta[7])),
  mzv[3,1,1,1,1] -> (((1/6)*(Pi^2)*Zeta[5])+((1/30)*(Pi^4)*Zeta[3])+(-4*Zeta[7])),
  mzv[3,1,1,2] -> (((-11/12)*(Pi^2)*Zeta[5])+((7/360)*(Pi^4)*Zeta[3])+((61/8)*Zeta[7])),
  mzv[3,1,2,1] -> (((3/4)*(Pi^2)*Zeta[5])+((-17/360)*(Pi^4)*Zeta[3])+((-67/16)*Zeta[7])),
  mzv[3,1,3] -> (((1/360)*(Pi^4)*Zeta[3])+((-1/4)*Zeta[7])),
  mzv[3,2,1,1] -> (((-11/12)*(Pi^2)*Zeta[5])+((-7/180)*(Pi^4)*Zeta[3])+((131/8)*Zeta[7])),
  mzv[3,2,2] -> ((2*(Pi^2)*Zeta[5])+((-1/60)*(Pi^4)*Zeta[3])+((-291/16)*Zeta[7])),
  mzv[3,3,1] -> (((3/4)*(Pi^2)*Zeta[5])+((-1/72)*(Pi^4)*Zeta[3])+((-51/8)*Zeta[7])),
  mzv[3,4] -> (((-5/3)*(Pi^2)*Zeta[5])+(17*Zeta[7])),
  mzv[4,1,1,1] -> (((1/3)*(Pi^2)*Zeta[5])+((7/360)*(Pi^4)*Zeta[3])+((-109/16)*Zeta[7])),
  mzv[4,1,2] -> (((-11/12)*(Pi^2)*Zeta[5])+((7/360)*(Pi^4)*Zeta[3])+((61/8)*Zeta[7])),
  mzv[4,2,1] -> (((-11/12)*(Pi^2)*Zeta[5])+((1/180)*(Pi^4)*Zeta[3])+((115/16)*Zeta[7])),
  mzv[4,3] -> (((5/3)*(Pi^2)*Zeta[5])+((1/90)*(Pi^4)*Zeta[3])+(-18*Zeta[7])),
  mzv[5,1,1] -> (((1/3)*(Pi^2)*Zeta[5])+((-1/360)*(Pi^4)*Zeta[3])+(-2*Zeta[7])),
  mzv[5,2] -> (((-2/3)*(Pi^2)*Zeta[5])+((-1/45)*(Pi^4)*Zeta[3])+(10*Zeta[7])),
  mzv[6,1] -> ((-4*Zeta[7])+((1/90)*(Pi^4)*Zeta[3])+((1/6)*(Pi^2)*Zeta[5])),
  mzv[7] -> Zeta[7],
  mzv[1,1,1,1,1,1,1,1] -> 0,
  mzv[1,1,1,1,1,1,2] -> ((1/9450)*(Pi^8)),
  mzv[1,1,1,1,1,2,1] -> ((-1/1350)*(Pi^8)),
  mzv[1,1,1,1,1,3] -> ((-1*Zeta[3]*Zeta[5])+((1/7560)*(Pi^8))),
  mzv[1,1,1,1,2,1,1] -> ((1/450)*(Pi^8)),
  mzv[1,1,1,1,2,2] -> ((2*Zeta[3]*Zeta[5])+((-7/27000)*(Pi^8))+((-2/5)*mzv[3,5])),
  mzv[1,1,1,1,3,1] -> ((4*Zeta[3]*Zeta[5])+((-101/189000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[1,1,1,1,4] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-3*Zeta[3]*Zeta[5])+((61/226800)*(Pi^8))),
  mzv[1,1,1,2,1,1,1] -> ((-1/270)*(Pi^8)),
  mzv[1,1,1,2,1,2] -> mzv[3,5],
  mzv[1,1,1,2,2,1] -> ((-10*Zeta[3]*Zeta[5])+((7/5400)*(Pi^8))),
  mzv[1,1,1,2,3] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(7*Zeta[3]*Zeta[5])+((-1133/1701000)*(Pi^8))+((-7/10)*mzv[3,5])),
  mzv[1,1,1,3,1,1] -> ((-5*Zeta[3]*Zeta[5])+((13/18900)*(Pi^8))+(-1*mzv[3,5])),
  mzv[1,1,1,3,2] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+((5/2)*Zeta[3]*Zeta[5])+((157/3402000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[1,1,1,4,1] -> (((11/2)*Zeta[3]*Zeta[5])+((-137/189000)*(Pi^8))+((3/10)*mzv[3,5])),
  mzv[1,1,1,5] -> (((1/6)*(Pi^2)*(Zeta[3]^2))+(-4*Zeta[3]*Zeta[5])+((499/1814400)*(Pi^8))),
  mzv[1,1,2,1,1,1,1] -> ((1/270)*(Pi^8)),
  mzv[1,1,2,1,1,2] -> ((1/113400)*(Pi^8)),
  mzv[1,1,2,1,2,1] -> (((-1/37800)*(Pi^8))+(-4*mzv[3,5])),
  mzv[1,1,2,1,3] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-2*Zeta[3]*Zeta[5])+((29/226800)*(Pi^8))+((5/2)*mzv[3,5])),
  mzv[1,1,2,2,1,1] -> ((20*Zeta[3]*Zeta[5])+((-97/37800)*(Pi^8))+(4*mzv[3,5])),
  mzv[1,1,2,2,2] -> (((1/2)*(Pi^2)*(Zeta[3]^2))+(-11*Zeta[3]*Zeta[5])+((1193/1701000)*(Pi^8))+((-9/5)*mzv[3,5])),
  mzv[1,1,2,3,1] -> ((-13*Zeta[3]*Zeta[5])+((121/70875)*(Pi^8))+((-2/5)*mzv[3,5])),
  mzv[1,1,2,4] -> (((-1/3)*(Pi^2)*(Zeta[3]^2))+(8*Zeta[3]*Zeta[5])+((-14899/27216000)*(Pi^8))+((-3/5)*mzv[3,5])),
  mzv[1,1,3,1,1,1] -> ((-1/16200)*(Pi^8)),
  mzv[1,1,3,1,2] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+((-5/2)*Zeta[3]*Zeta[5])+((71/340200)*(Pi^8))+(-1*mzv[3,5])),
  mzv[1,1,3,2,1] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(6*Zeta[3]*Zeta[5])+((-739/567000)*(Pi^8))+((11/5)*mzv[3,5])),
  mzv[1,1,3,3] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-1919/5443200)*(Pi^8))+((3/2)*mzv[3,5])),
  mzv[1,1,4,1,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+((-15/2)*Zeta[3]*Zeta[5])+((283/226800)*(Pi^8))+((-3/2)*mzv[3,5])),
  mzv[1,1,4,2] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-3*Zeta[3]*Zeta[5])+((7457/27216000)*(Pi^8))+((-6/5)*mzv[3,5])),
  mzv[1,1,5,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-4301/9072000)*(Pi^8))+((3/10)*mzv[3,5])),
  mzv[1,1,6] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-3*Zeta[3]*Zeta[5])+((61/226800)*(Pi^8))),
  mzv[1,2,1,1,1,1,1] -> ((-1/450)*(Pi^8)),
  mzv[1,2,1,1,1,2] -> ((Zeta[3]*Zeta[5])+((-1/9450)*(Pi^8))+(-1*mzv[3,5])),
  mzv[1,2,1,1,2,1] -> ((-4*Zeta[3]*Zeta[5])+((1/2520)*(Pi^8))+(4*mzv[3,5])),
  mzv[1,2,1,1,3] -> (((-1/3)*(Pi^2)*(Zeta[3]^2))+((15/2)*Zeta[3]*Zeta[5])+((-13/27216)*(Pi^8))+((-3/2)*mzv[3,5])),
  mzv[1,2,1,2,1,1] -> (((-1/1800)*(Pi^8))+(6*Zeta[3]*Zeta[5])),
  mzv[1,2,1,2,2] -> (((3/4)*(Pi^2)*(Zeta[3]^2))+((-33/2)*Zeta[3]*Zeta[5])+((1187/1134000)*(Pi^8))+((-13/10)*mzv[3,5])),
  mzv[1,2,1,3,1] -> (((1/378000)*(Pi^8))+((-17/10)*mzv[3,5])),
  mzv[1,2,1,4] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-1919/5443200)*(Pi^8))+((3/2)*mzv[3,5])),
  mzv[1,2,2,1,1,1] -> ((-24*Zeta[3]*Zeta[5])+((37/12600)*(Pi^8))+(-4*mzv[3,5])),
  mzv[1,2,2,1,2] -> ((-1*(Pi^2)*(Zeta[3]^2))+((49/2)*Zeta[3]*Zeta[5])+((-7/4050)*(Pi^8))+(4*mzv[3,5])),
  mzv[1,2,2,2,1] -> ((-1*(Pi^2)*(Zeta[3]^2))+(17*Zeta[3]*Zeta[5])+((-1/1350)*(Pi^8))),
  mzv[1,2,2,3] -> (((3/2)*(Pi^2)*(Zeta[3]^2))+(-33*Zeta[3]*Zeta[5])+((19007/9072000)*(Pi^8))+((-18/5)*mzv[3,5])),
  mzv[1,2,3,1,1] -> (((1/2)*(Pi^2)*(Zeta[3]^2))+(11*Zeta[3]*Zeta[5])+((-829/378000)*(Pi^8))+((23/10)*mzv[3,5])),
  mzv[1,2,3,2] -> (((-1/2)*(Pi^2)*(Zeta[3]^2))+((27/2)*Zeta[3]*Zeta[5])+((-28087/27216000)*(Pi^8))+((27/10)*mzv[3,5])),
  mzv[1,2,4,1] -> (((1/2)*(Pi^2)*(Zeta[3]^2))+((-31/2)*Zeta[3]*Zeta[5])+((11651/9072000)*(Pi^8))+((-3/10)*mzv[3,5])),
  mzv[1,2,5] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(7*Zeta[3]*Zeta[5])+((-1133/1701000)*(Pi^8))+((-7/10)*mzv[3,5])),
  mzv[1,3,1,1,1,1] -> ((6*Zeta[3]*Zeta[5])+((-13/18900)*(Pi^8))+mzv[3,5]),
  mzv[1,3,1,1,2] -> (((1/4)*(Pi^2)*(Zeta[3]^2))+((-11/2)*Zeta[3]*Zeta[5])+((241/680400)*(Pi^8))),
  mzv[1,3,1,2,1] -> (((-1/2)*Zeta[3]*Zeta[5])+((1/25200)*(Pi^8))+(-1*mzv[3,5])),
  mzv[1,3,1,3] -> ((1/1814400)*(Pi^8)),
  mzv[1,3,2,1,1] -> ((-17*Zeta[3]*Zeta[5])+((2/875)*(Pi^8))+((-23/10)*mzv[3,5])),
  mzv[1,3,2,2] -> (((-3/4)*(Pi^2)*(Zeta[3]^2))+((33/2)*Zeta[3]*Zeta[5])+((-9491/9072000)*(Pi^8))+((9/5)*mzv[3,5])),
  mzv[1,3,3,1] -> (((23/3024000)*(Pi^8))+((-27/10)*mzv[3,5])),
  mzv[1,3,4] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-2*Zeta[3]*Zeta[5])+((29/226800)*(Pi^8))+((5/2)*mzv[3,5])),
  mzv[1,4,1,1,1] -> (((19/2)*Zeta[3]*Zeta[5])+((-29/22680)*(Pi^8))+((3/2)*mzv[3,5])),
  mzv[1,4,1,2] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-8*Zeta[3]*Zeta[5])+((3043/5443200)*(Pi^8))+((-3/2)*mzv[3,5])),
  mzv[1,4,2,1] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-5*Zeta[3]*Zeta[5])+((179/1296000)*(Pi^8))+((21/10)*mzv[3,5])),
  mzv[1,4,3] -> (((-1/3)*(Pi^2)*(Zeta[3]^2))+((15/2)*Zeta[3]*Zeta[5])+((-13/27216)*(Pi^8))+((-3/2)*mzv[3,5])),
  mzv[1,5,1,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(2*Zeta[3]*Zeta[5])+((-1/259200)*(Pi^8))),
  mzv[1,5,2] -> (((703/1134000)*(Pi^8))+((-17/2)*Zeta[3]*Zeta[5])+((-7/10)*mzv[3,5])+((1/3)*(Pi^2)*(Zeta[3]^2))),
  mzv[1,6,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(5*Zeta[3]*Zeta[5])+((-233/567000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[1,7] -> ((-1*Zeta[3]*Zeta[5])+((1/7560)*(Pi^8))),
  mzv[2,1,1,1,1,1,1] -> ((1/1350)*(Pi^8)),
  mzv[2,1,1,1,1,2] -> ((-2*Zeta[3]*Zeta[5])+((187/567000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[2,1,1,1,2,1] -> (((-163/113400)*(Pi^8))+(8*Zeta[3]*Zeta[5])),
  mzv[2,1,1,1,3] -> (((703/1134000)*(Pi^8))+((-17/2)*Zeta[3]*Zeta[5])+((-7/10)*mzv[3,5])+((1/3)*(Pi^2)*(Zeta[3]^2))),
  mzv[2,1,1,2,1,1] -> ((-12*Zeta[3]*Zeta[5])+((281/113400)*(Pi^8))+(-4*mzv[3,5])),
  mzv[2,1,1,2,2] -> (((-5/6)*(Pi^2)*(Zeta[3]^2))+(20*Zeta[3]*Zeta[5])+((-59/42525)*(Pi^8))+(4*mzv[3,5])),
  mzv[2,1,1,3,1] -> (((1/6)*(Pi^2)*(Zeta[3]^2))+(-1*Zeta[3]*Zeta[5])+((-233/1701000)*(Pi^8))+((9/5)*mzv[3,5])),
  mzv[2,1,1,4] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-3*Zeta[3]*Zeta[5])+((7457/27216000)*(Pi^8))+((-6/5)*mzv[3,5])),
  mzv[2,1,2,1,1,1] -> ((8*Zeta[3]*Zeta[5])+((-239/113400)*(Pi^8))+(4*mzv[3,5])),
  mzv[2,1,2,1,2] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-9*Zeta[3]*Zeta[5])+((793/1134000)*(Pi^8))+((-27/10)*mzv[3,5])),
  mzv[2,1,2,2,1] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-9*Zeta[3]*Zeta[5])+((19/28350)*(Pi^8))+(-4*mzv[3,5])),
  mzv[2,1,2,3] -> (((-1/2)*(Pi^2)*(Zeta[3]^2))+((27/2)*Zeta[3]*Zeta[5])+((-28087/27216000)*(Pi^8))+((27/10)*mzv[3,5])),
  mzv[2,1,3,1,1] -> (((-5/12)*(Pi^2)*(Zeta[3]^2))+(6*Zeta[3]*Zeta[5])+((-1/7560)*(Pi^8))+mzv[3,5]),
  mzv[2,1,3,2] -> (((Pi^2)*(Zeta[3]^2))+(-27*Zeta[3]*Zeta[5])+((56249/27216000)*(Pi^8))+((-27/5)*mzv[3,5])),
  mzv[2,1,4,1] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+((23/2)*Zeta[3]*Zeta[5])+((-31343/27216000)*(Pi^8))+((33/10)*mzv[3,5])),
  mzv[2,1,5] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+((5/2)*Zeta[3]*Zeta[5])+((157/3402000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[2,2,1,1,1,1] -> ((8*Zeta[3]*Zeta[5])+((-47/113400)*(Pi^8))),
  mzv[2,2,1,1,2] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-9*Zeta[3]*Zeta[5])+((803/1134000)*(Pi^8))+((-11/5)*mzv[3,5])),
  mzv[2,2,1,2,1] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-4*Zeta[3]*Zeta[5])+((-1/15120)*(Pi^8))+(4*mzv[3,5])),
  mzv[2,2,1,3] -> (((-3/4)*(Pi^2)*(Zeta[3]^2))+((33/2)*Zeta[3]*Zeta[5])+((-9491/9072000)*(Pi^8))+((9/5)*mzv[3,5])),
  mzv[2,2,2,1,1] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-4*Zeta[3]*Zeta[5])+((31/226800)*(Pi^8))),
  mzv[2,2,2,2] -> ((1/362880)*(Pi^8)),
  mzv[2,2,3,1] -> (((-1/2)*(Pi^2)*(Zeta[3]^2))+(6*Zeta[3]*Zeta[5])+((-997/27216000)*(Pi^8))+((-9/5)*mzv[3,5])),
  mzv[2,2,4] -> (((1/2)*(Pi^2)*(Zeta[3]^2))+(-11*Zeta[3]*Zeta[5])+((1193/1701000)*(Pi^8))+((-9/5)*mzv[3,5])),
  mzv[2,3,1,1,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(-10*Zeta[3]*Zeta[5])+((5119/3402000)*(Pi^8))+((-11/5)*mzv[3,5])),
  mzv[2,3,1,2] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+(8*Zeta[3]*Zeta[5])+((-2053/3024000)*(Pi^8))+((27/10)*mzv[3,5])),
  mzv[2,3,2,1] -> (((-1/2)*(Pi^2)*(Zeta[3]^2))+(11*Zeta[3]*Zeta[5])+((-3889/5443200)*(Pi^8))),
  mzv[2,3,3] -> (((3/4)*(Pi^2)*(Zeta[3]^2))+((-33/2)*Zeta[3]*Zeta[5])+((1187/1134000)*(Pi^8))+((-13/10)*mzv[3,5])),
  mzv[2,4,1,1] -> (((1/4)*(Pi^2)*(Zeta[3]^2))+((-9/2)*Zeta[3]*Zeta[5])+((6611/27216000)*(Pi^8))+((-21/10)*mzv[3,5])),
  mzv[2,4,2] -> (((-5/6)*(Pi^2)*(Zeta[3]^2))+(20*Zeta[3]*Zeta[5])+((-59/42525)*(Pi^8))+(4*mzv[3,5])),
  mzv[2,5,1] -> (((5/12)*(Pi^2)*(Zeta[3]^2))+((-23/2)*Zeta[3]*Zeta[5])+((997/1134000)*(Pi^8))+((-3/10)*mzv[3,5])),
  mzv[2,6] -> ((2*Zeta[3]*Zeta[5])+((-7/27000)*(Pi^8))+((-2/5)*mzv[3,5])),
  mzv[3,1,1,1,1,1] -> ((-4*Zeta[3]*Zeta[5])+((29/81000)*(Pi^8))+((-2/5)*mzv[3,5])),
  mzv[3,1,1,1,2] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-283/680400)*(Pi^8))+mzv[3,5]),
  mzv[3,1,1,2,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(-2*Zeta[3]*Zeta[5])+((841/3402000)*(Pi^8))+((-9/5)*mzv[3,5])),
  mzv[3,1,1,3] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-8*Zeta[3]*Zeta[5])+((3043/5443200)*(Pi^8))+((-3/2)*mzv[3,5])),
  mzv[3,1,2,1,1] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-107/283500)*(Pi^8))+((17/10)*mzv[3,5])),
  mzv[3,1,2,2] -> (((-1/4)*(Pi^2)*(Zeta[3]^2))+(8*Zeta[3]*Zeta[5])+((-2053/3024000)*(Pi^8))+((27/10)*mzv[3,5])),
  mzv[3,1,3,1] -> (((-1/2)*Zeta[3]*Zeta[5])+((17/362880)*(Pi^8))),
  mzv[3,1,4] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+((-5/2)*Zeta[3]*Zeta[5])+((71/340200)*(Pi^8))+(-1*mzv[3,5])),
  mzv[3,2,1,1,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(9*Zeta[3]*Zeta[5])+((-4483/3402000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[3,2,1,2] -> (((2/3)*(Pi^2)*(Zeta[3]^2))+(-18*Zeta[3]*Zeta[5])+((12713/9072000)*(Pi^8))+((-27/5)*mzv[3,5])),
  mzv[3,2,2,1] -> (((2/3)*(Pi^2)*(Zeta[3]^2))+(-13*Zeta[3]*Zeta[5])+((5849/9072000)*(Pi^8))+((9/5)*mzv[3,5])),
  mzv[3,2,3] -> ((-1*(Pi^2)*(Zeta[3]^2))+((49/2)*Zeta[3]*Zeta[5])+((-7/4050)*(Pi^8))+(4*mzv[3,5])),
  mzv[3,3,1,1] -> (((-1/12)*(Pi^2)*(Zeta[3]^2))+(4*Zeta[3]*Zeta[5])+((-9757/27216000)*(Pi^8))+((27/10)*mzv[3,5])),
  mzv[3,3,2] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-9*Zeta[3]*Zeta[5])+((793/1134000)*(Pi^8))+((-27/10)*mzv[3,5])),
  mzv[3,4,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+((9/2)*Zeta[3]*Zeta[5])+((-47/136080)*(Pi^8))+((-5/2)*mzv[3,5])),
  mzv[3,5] -> mzv[3,5],
  mzv[4,1,1,1,1] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+((-9/2)*Zeta[3]*Zeta[5])+((2/3375)*(Pi^8))+((-3/10)*mzv[3,5])),
  mzv[4,1,1,2] -> (((-1/3)*(Pi^2)*(Zeta[3]^2))+(9*Zeta[3]*Zeta[5])+((-3457/5443200)*(Pi^8))+(3*mzv[3,5])),
  mzv[4,1,2,1] -> (((-1/12)*(Pi^2)*(Zeta[3]^2))+(-1*Zeta[3]*Zeta[5])+((1763/27216000)*(Pi^8))+((-33/10)*mzv[3,5])),
  mzv[4,1,3] -> (((1/4)*(Pi^2)*(Zeta[3]^2))+((-11/2)*Zeta[3]*Zeta[5])+((241/680400)*(Pi^8))),
  mzv[4,2,1,1] -> (((-1/3)*(Pi^2)*(Zeta[3]^2))+(7*Zeta[3]*Zeta[5])+((-4573/27216000)*(Pi^8))+((3/10)*mzv[3,5])),
  mzv[4,2,2] -> (((1/3)*(Pi^2)*(Zeta[3]^2))+(-9*Zeta[3]*Zeta[5])+((803/1134000)*(Pi^8))+((-11/5)*mzv[3,5])),
  mzv[4,3,1] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-3*Zeta[3]*Zeta[5])+((1/4536)*(Pi^8))+((5/2)*mzv[3,5])),
  mzv[4,4] -> ((1/113400)*(Pi^8)),
  mzv[5,1,1,1] -> (((1/6)*(Pi^2)*(Zeta[3]^2))+((-7/2)*Zeta[3]*Zeta[5])+((881/9072000)*(Pi^8))+((-3/10)*mzv[3,5])),
  mzv[5,1,2] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+((11/2)*Zeta[3]*Zeta[5])+((-283/680400)*(Pi^8))+mzv[3,5]),
  mzv[5,2,1] -> (((-1/6)*(Pi^2)*(Zeta[3]^2))+(4*Zeta[3]*Zeta[5])+((-13/30375)*(Pi^8))+((3/10)*mzv[3,5])),
  mzv[5,3] -> ((Zeta[3]*Zeta[5])+((-1/9450)*(Pi^8))+(-1*mzv[3,5])),
  mzv[6,1,1] -> (((1/12)*(Pi^2)*(Zeta[3]^2))+(-2*Zeta[3]*Zeta[5])+((281/1134000)*(Pi^8))+((-2/5)*mzv[3,5])),
  mzv[6,2] -> ((-2*Zeta[3]*Zeta[5])+((187/567000)*(Pi^8))+((2/5)*mzv[3,5])),
  mzv[7,1] -> (((-1/4200)*(Pi^8))+(Zeta[3]*Zeta[5])),
  mzv[8] -> ((1/9450)*(Pi^8))}
];


(* ::Section::Closed:: *)
(*Default Verbosity & Cache Setup*)


SetAttributes[MaybeQuiet,HoldAll]
MaybeQuiet[expr_]:=If[TrueQ[$QuietPrint],Block[{Print=(Null&)},expr],expr]


SetAttributes[flag6,HoldAll]
flag6[expr_]:=If[TrueQ[$QuietPrint],Block[{Print=(Null&)},expr],expr]

(* Debug-level print: only fires when $HyperVerbosity > 1 *)
DebugPrint[args___] := If[$HyperVerbosity > 1, Print[args]]


$HyperVerbosity = 1;
$HyperInticaCheckDivergences = True;
$HyperInticaAbortOnDivergence = True;
$HyperInticaDivergences = <||>;
$HyperSplittingField = {};
$HyperEvaluatePeriods = True;

$HyperAlgebraicRoots = False;
$HyperIgnoreNonlinearPolynomials = False;
$QuietPrint=True(*False*);
$NoAlgebraicRootsContributions=True;
$HyperWarnZeroed = True;

$HyperIntroduceAlgebraicLetters::usage = "If True, LinearFactors[p, var] introduces fresh symbolic letters Wm[i], Wp[i] for each degree-2 factor it encounters, rather than failing or zeroing. Metadata (roots, Vieta sum/product, discriminant) is stored in $HyperAlgebraicLetterTable[i]. Default: False.";
$HyperIntroduceAlgebraicLetters = False;
$HyperAlgebraicLetterCounter    = 0;
$HyperAlgebraicLetterTable      = <||>;
ClearAlgebraicLetters[] := ($HyperAlgebraicLetterCounter = 0; $HyperAlgebraicLetterTable = <||>;)

$UseFFPolynomialQuotient = False;
$PartialFractionsMaxWeight = {1, 100};

$ShuffleWordsCache = <||>;
$TransformWordCache = <||>;
$PartialFractionsCache = <||>;
$PoleDegreeCache = <||>;
$RatResidueCache = <||>;
$LinearFactorsCache = <||>;
$ReglimWordCache = <||>;
$RegzeroWordCache = <||>;
$ZeroExpansionCache = <||>;
$InfExpansionCache = <||>;
$SeriesExpansionCache = <||>;

$NonlinearFactorsCache = <||>;


ForgetAllMemo[] := (
  $ShuffleWordsCache = <||>; $TransformWordCache = <||>;
  $PartialFractionsCache = <||>; $PoleDegreeCache = <||>;
  $LinearFactorsCache = <||>; $ReglimWordCache = <||>;
  $RegzeroWordCache = <||>; $ZeroExpansionCache = <||>;
  $InfExpansionCache = <||>; $SeriesExpansionCache = <||>; $NonlinearFactorsCache = <||>;)

ForgetExpansions[] := ($ZeroExpansionCache = <||>; $InfExpansionCache = <||>; $SeriesExpansionCache = <||>;)


(* ::Section::Closed:: *)
(*Words Operations*)


WordQ[w_] := ListQ[w] && !AnyTrue[w, ListQ]

(* Optimized coefficient simplification: avoids full Together[] for single-term values.
   - Structurally 0: skip everything
   - Single term (Head =!= Plus): Cancel[] is sufficient (no sum combining needed)
   - Sum of terms: fall back to Together[] to combine fractions *)
QuickCancel[x_] := x(*Which[x === 0, 0, Head[x] =!= Plus, Cancel[x], True, Together[x]]*)

CollectWords[wordlist_List] := Module[{result = <||>, w},
  Do[result[w[[2]]] = Lookup[result, Key[w[[2]]], 0] + w[[1]], {w, wordlist}];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]]

ScalarMul[wordlist_List, c_] := Map[{#[[1]] * c, #[[2]]} &, wordlist]

ShuffleWords[{}, w_List] := {w}
ShuffleWords[v_List, {}] := {v}
ShuffleWords[v_List, w_List] := Module[{key, cached},
  key = {v, w};
  If[KeyExistsQ[$ShuffleWordsCache, key], Return[$ShuffleWordsCache[key]]];
  If[KeyExistsQ[$ShuffleWordsCache, {w, v}], Return[$ShuffleWordsCache[{w, v}]]];
  cached = Join[Map[Prepend[#, v[[1]]] &, ShuffleWords[Rest[v], w]],
                Map[Prepend[#, w[[1]]] &, ShuffleWords[v, Rest[w]]]];
  $ShuffleWordsCache[{v, w}] = cached; cached]

ShuffleProduct[a_List, b_List] := Module[{result = <||>, i, j, wrd},
  Do[Do[Do[result[wrd] = Lookup[result, Key[wrd], 0] + i[[1]] * j[[1]],
    {wrd, ShuffleWords[i[[2]], j[[2]]]}], {j, b}], {i, a}];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]]

ConcatMul[a_List, b_List] := Module[{result = <||>, i, j, w},
  Do[Do[w = Join[i[[2]], j[[2]]];
    result[w] = Lookup[result, Key[w], 0] + i[[1]] * j[[1]], {j, b}], {i, a}];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]]

(*ShuffleSymbolic: both inputs are {{coef, shuffleKey}, ...}*)
(*Note: shuffleKey is a list of words*)
ShuffleSymbolic[aList_List, bList_List] := Module[{result = <||>, i, j, L, iW, jW},
  Do[Do[
    iW = Which[i[[2]] === {}, {}, WordQ[i[[2]]], {i[[2]]}, True, i[[2]]];
    jW = Which[j[[2]] === {}, {}, WordQ[j[[2]]], {j[[2]]}, True, j[[2]]];
    L = Sort[Join[iW, jW]];
    result[L] = Lookup[result, Key[L], 0] + i[[1]] * j[[1]],
    {j, bList}], {i, aList}];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]]


ZeroExpansion[{}, _] := {{1}}
ZeroExpansion[word_List, minOrder_Integer] /; minOrder < 0 := {}
ZeroExpansion[word_List, minOrder_Integer] := Module[{key, cached},
  If[minOrder === 0 && !$HyperInticaCheckDivergences, Return[{}]];
  key = {word, minOrder};
  If[KeyExistsQ[$ZeroExpansionCache, key], cached = $ZeroExpansionCache[key];
    If[cached[[2]] >= minOrder, Return[cached[[1]]]]];
  cached = {ExpandZeroWord[word, minOrder], minOrder};
  $ZeroExpansionCache[key] = cached; cached[[1]]]
  
ExpandZeroWord[{}, _] := {{1}}
ExpandZeroWord[word_List, minOrder_Integer] /; minOrder < 0 := {}
ExpandZeroWord[word_List, minOrder_Integer] := Module[
  {sub, maxlog, maxpowers, result, power, logpower, p, ii, jj, w1, resultTable},
  sub = ZeroExpansion[Rest[word], minOrder];
  If[sub === {} || Length[sub] === 0, Return[{}]];
  maxlog = Length[sub];
  result = Table[Table[0, {minOrder + 1}], {maxlog + 1}];
  maxpowers = Table[-1, {maxlog + 1}];
  w1 = word[[1]];
  Do[If[logpower + 1 <= Length[sub] && Length[sub[[logpower + 1]]] >= 1 && w1 === 0,
      result[[logpower + 2, 1]] += sub[[logpower + 1, 1]];
      maxpowers[[logpower + 2]] = Max[maxpowers[[logpower + 2]], 0]];
    Do[If[w1 =!= 0,
        p = 0;
        Do[If[ii + 1 <= Length[sub[[logpower + 1]]], 
          p -= sub[[logpower + 1, ii + 1]] / w1^(power - ii + 1)],
          {ii, 0, Min[power, Length[sub[[logpower + 1]]] - 1]}],
        If[power + 2 <= Length[sub[[logpower + 1]]], 
          p = sub[[logpower + 1, power + 2]], p = 0]];
      If[p =!= 0, p = -p;
        Do[p = -p / (power + 1); result[[jj + 1, power + 2]] += p;
          maxpowers[[jj + 1]] = Max[maxpowers[[jj + 1]], power + 1], 
          {jj, logpower, 0, -1}]],
      {power, 0, minOrder - 1}], {logpower, 0, maxlog - 1}];
  
  (*Build result table keeping empty lists for intermediate log powers*)
  resultTable = Table[If[maxpowers[[logpower + 1]] >= 0,
    Take[result[[logpower + 1]], maxpowers[[logpower + 1]] + 1], {}],
    {logpower, 0, Length[maxpowers] - 1}];
  
  (*Remove only TRAILING empty lists ; single Drop instead of O(n) While loop*)
  resultTable = Drop[resultTable, -LengthWhile[Reverse[resultTable], # === {} &]];
  
  resultTable]
  
  InfExpansion[{}, _] := {{1}}
InfExpansion[word_List, minOrder_Integer] /; minOrder < 0 := {}
InfExpansion[word_List, minOrder_Integer] := Module[{key, cached},
  If[minOrder === 0 && !$HyperInticaCheckDivergences, Return[{}]];
  key = {word, minOrder};
  If[KeyExistsQ[$InfExpansionCache, key], cached = $InfExpansionCache[key];
    If[cached[[2]] >= minOrder, Return[cached[[1]]]]];
  cached = {ExpandInfWord[word, minOrder], minOrder};
  $InfExpansionCache[key] = cached; cached[[1]]]

ExpandInfWord[{}, _] := {{1}}
ExpandInfWord[word_List, minOrder_Integer] /; minOrder < 0 := {}
ExpandInfWord[word_List, minOrder_Integer] := Module[
  {sub, maxlog, maxpowers, result, power, logpower, p, ii, jj, w1, resultTable},
  sub = InfExpansion[Rest[word], minOrder];
  If[sub === {} || Length[sub] === 0, Return[{}]];
  maxlog = Length[sub];
  result = Table[Table[0, {minOrder + 1}], {maxlog + 1}];
  maxpowers = Table[-1, {maxlog + 1}];
  w1 = word[[1]];
  Do[If[logpower + 1 <= Length[sub] && Length[sub[[logpower + 1]]] >= 1,
      result[[logpower + 2, 1]] -= sub[[logpower + 1, 1]];
      maxpowers[[logpower + 2]] = Max[maxpowers[[logpower + 2]], 0]];
    Do[If[power + 2 <= Length[sub[[logpower + 1]]], 
        p = -sub[[logpower + 1, power + 2]], p = 0];
      If[w1 =!= 0,
        Do[If[ii + 1 <= Length[sub[[logpower + 1]]], 
          p -= sub[[logpower + 1, ii + 1]] * w1^(power - ii + 1)],
          {ii, 0, Min[power, Length[sub[[logpower + 1]]] - 1]}]];
      If[p =!= 0, p = -p;
        Do[p = -p / (power + 1); result[[jj + 1, power + 2]] += p;
          maxpowers[[jj + 1]] = Max[maxpowers[[jj + 1]], power + 1], 
          {jj, logpower, 0, -1}]],
      {power, 0, minOrder - 1}], {logpower, 0, maxlog - 1}];
  
  (*Build result table keeping empty lists for intermediate log powers*)
  resultTable = Table[If[maxpowers[[logpower + 1]] >= 0,
    Take[result[[logpower + 1]], maxpowers[[logpower + 1]] + 1], {}],
    {logpower, 0, Length[maxpowers] - 1}];
  
  (*Remove only TRAILING empty lists ; single Drop instead of O(n) While loop*)
  resultTable = Drop[resultTable, -LengthWhile[Reverse[resultTable], # === {} &]];
  
  resultTable]

Reg0[wordlist_List] := Module[{result = <||>, w, v},
  Do[Do[result[v[[2]]] = Lookup[result, Key[v[[2]]], 0] + v[[1]] * w[[1]],
    {v, RegzeroWord[w[[2]]]}], {w, wordlist}];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]]
  
  (*Convert words with only {0,-1} letters to periods*)
WordToPeriodCoef[word_List] := Module[{},
  If[word === {}, Return[{1, {}}]];
  If[Union[word] === {0} || Union[word] === {-1}, Return[{0, {}}]];
  If[SubsetQ[{-1, 0}, Union[word]], Return[{ZeroInfPeriod[word], {}}]];
  {1, word}]
  
  RegzeroWord[{}] := {{1, {}}}
RegzeroWord[word_List] := Module[{key, n, i, result},
  key = word;
  If[KeyExistsQ[$RegzeroWordCache, key], Return[$RegzeroWordCache[key]]];
  n = 0; For[i = Length[word], i >= 1, i--, If[word[[i]] === 0, n++, Break[]]];
  (*Word of all zeros: regularized to zero*)
  If[n === Length[word], $RegzeroWordCache[key] = {}; Return[{}]];
  If[n === 0,
    $RegzeroWordCache[key] = {{1, word}}; Return[{{1, word}}]];
  (*Handle trailing zeros via shuffle regularization*)
  result = Map[{#[[1]], Append[#[[2]], word[[Length[word] - n]]]} &,
    ShuffleProduct[{{(-1)^n, Table[0, {n}]}}, {{1, Take[word, Length[word] - n - 1]}}]];
  $RegzeroWordCache[key] = result;
  result]
  
RegTail[wordlist_List, letter_, substitute_: 0] := Module[
  {res = {}, w, n, i, wlen, ii, prefac},
  Do[
    wlen = Length[w[[2]]];
    If[wlen === 0, AppendTo[res, w]; Continue[]];
    n = 0; 
    For[i = wlen, i >= 1, i--, If[w[[2, i]] === letter, n++, Break[]]];
    If[n === wlen, 
      (*Handle 0^0: when n=0, substitute^n/n! = 1*)
      prefac = If[n === 0, 1, substitute^n / n!];
      AppendTo[res, {w[[1]] * prefac, {}}]; 
      Continue[]];
    Do[
      (*Handle 0^0: when n=ii, substitute^(n-ii)/(n-ii)! = 1*)
      prefac = If[n === ii, 1, substitute^(n - ii) / (n - ii)!];
      res = Join[res, ConcatMul[
        ShuffleProduct[{{(-1)^ii, Table[letter, {ii}]}}, 
          {{w[[1]], w[[2, 1 ;; wlen - n - 1]]}}],
        {{prefac, {w[[2, wlen - n]]}}}]], 
      {ii, 0, n}], 
    {w, wordlist}];
  CollectWords[res]]

RegHead[wordlist_List, letter_, substitute_: 0] := Module[
  {res = {}, w, n, i, ii, prefac},
  Do[
    If[Length[w[[2]]] === 0, AppendTo[res, w]; Continue[]];
    n = 0; 
    For[i = 1, i <= Length[w[[2]]], i++, If[w[[2, i]] === letter, n++, Break[]]];
    If[n === Length[w[[2]]], 
      (*Handle 0^0: when n=0, substitute^n/n! = 1*)
      prefac = If[n === 0, 1, substitute^n / n!];
      AppendTo[res, {w[[1]] * prefac, {}}]; 
      Continue[]];
    Do[
      (*Handle 0^0: when n=ii, substitute^(n-ii)/(n-ii)! = 1*)
      prefac = If[n === ii, 1, substitute^(n - ii) / (n - ii)!];
      res = Join[res, ConcatMul[
        {{prefac, {w[[2, n + 1]]}}},
        ShuffleProduct[{{(-1)^ii, Table[letter, {ii}]}}, 
          {{w[[1]], w[[2, n + 2 ;;]]}}]]], 
      {ii, 0, n}], 
    {w, wordlist}];
  CollectWords[res]]

EvaluatePeriods[wordlist_List] := Module[{result = 0, entry, key, prod, word},
  Do[key = entry[[2]];
    If[key === {}, result += entry[[1]]; Continue[]];
    If[WordQ[key], result += entry[[1]] * ZeroInfPeriod[key]; Continue[]];
    If[ListQ[key] && Length[key] > 0 && AllTrue[key, WordQ],
      prod = 1; Do[If[word =!= {}, prod *= ZeroInfPeriod[word]], {word, key}];
      result += entry[[1]] * prod; Continue[]];
    result += entry[[1]] * ZeroInfPeriod[key], {entry, wordlist}];
  Collect[result, _ZeroInfPeriod, Cancel]]


TransformShuffle[{}, _] := {{{{1, {}}}, {{1, {}}}}}

TransformShuffle[wordlist_List, var_] := MaybeQuiet[Module[
  {resultTable, word, count, tempFactor, pair, letter, sub,
   countTemp, temp, i, combinedWords, failed = False},
  
  DebugPrint["[TS] Called with ", Length[wordlist], " words"];
  
  count = <||>;
  combinedWords = {};
  tempFactor = 1;
  pair = False;
  
  Do[
    If[word === {}, 
      Print["Error: empty word in shuffle product list"]; 
      Continue[]
    ];
    letter = word[[1]];
    
    If[word === Table[letter, {Length[word]}],
      tempFactor = tempFactor / Factorial[Length[word]];
      If[KeyExistsQ[count, letter],
        count[letter] += Length[word];
        pair = True,
        count[letter] = Length[word]
      ],
      AppendTo[combinedWords, word]
    ],
    {word, wordlist}
  ];
  
  If[pair,
    combinedWords = Join[combinedWords, 
      KeyValueMap[Table[#1, {#2}] &, count]];
    tempFactor = tempFactor * Apply[Times, Factorial /@ Values[count]];
    sub = TransformShuffle[combinedWords, var];
    If[sub === $Failed, 
      DebugPrint["[TS] Recursive TransformShuffle returned $Failed"];
      Return[$Failed]
    ];
    Return[Map[
      {#[[1]], Map[{#[[1]] * tempFactor, #[[2]]} &, #[[2]]]} &, 
      sub
    ]]
  ];
  
  resultTable = <|1 -> {{{1, {}}}, {{1, {}}}}|>;
  count = 1;
  
  Do[
    DebugPrint["[TS] Processing word: ", Short[word], " failed=", failed];
    If[failed, DebugPrint["[TS] Breaking due to failed"]; Break[]];
    
    sub = TransformWord[word, var];
    DebugPrint["[TS] TransformWord returned: ", If[sub === $Failed, "$Failed", "OK with " <> ToString[Length[sub]] <> " entries"]];
    
    If[sub === $Failed,
      DebugPrint["[TS] *** DETECTED $Failed ***"];
      failed = True;
      Break[]
    ];
    
    If[sub === {}, 
      count = 0;
      Break[]
    ];
    
    countTemp = 0;
    temp = <||>;
    
    Do[
      Do[
        countTemp++;
        temp[countTemp] = {
          CollectWords[ShuffleProduct[resultTable[i][[1]], wordPair[[1]]]],
          ShuffleSymbolic[resultTable[i][[2]], wordPair[[2]]]
        },
        {wordPair, sub}
      ],
      {i, count}
    ];
    
    resultTable = temp;
    count = countTemp,
    {word, wordlist}
  ];
  
  DebugPrint["[TS] After Do loop: failed=", failed, " count=", count];
  
  If[failed, 
    DebugPrint["[TS] *** RETURNING $Failed ***"];
    Return[$Failed]
  ];
  
  If[count === 0, Return[{}]];
  
  DebugPrint["[TS] Returning result with count=", count];
  Table[resultTable[i], {i, count}]
]]

TransformWord[{}, _] := {{{{1, {}}}, {{1, {}}}}}

TransformWord[word_List, var_] := MaybeQuiet[Module[
  {key, resultTable, sub, subResult, i, p, facts, s, coef, wrd, pole, 
   newWord, finalResult, wordPairs, reginfsKey, failed = False},
  
  key = {word, var};
  If[KeyExistsQ[$TransformWordCache, key], 
    DebugPrint["[TW] Cache hit for ", Short[word]];
    Return[$TransformWordCache[key]]
  ];
  
  DebugPrint["[TW] Processing word: ", Short[word], " var: ", var];
  
  resultTable = <||>;
  sub = ReglimWord[word, var];
  
  If[Length[sub] > 0 && !FreeQ[word, var],
    If[!KeyExistsQ[resultTable, sub], resultTable[sub] = <||>];
    resultTable[sub][{}] = Lookup[resultTable[sub], Key[{}], 0] + 1
  ];
  
  If[FreeQ[word, var],
    If[Length[sub] > 0,
      $TransformWordCache[key] = {{{{1, {}}}, sub}};
      Return[{{{{1, {}}}, sub}}],
      $TransformWordCache[key] = {};
      Return[{}]
    ]
  ];
  
  If[Length[word] > 0 && word[[-1]] === 0, 
    DebugPrint["[TW] Trailing zero detected, returning $Failed"];
    $TransformWordCache[key] = $Failed; 
    Return[$Failed]
  ];
  
  Do[
    DebugPrint["[TW] i=", i, " failed=", failed];
    If[failed, DebugPrint["[TW] Breaking due to failed flag"]; Break[]];
    If[i === Length[word] && i > 1 && word[[i - 1]] === 0, Break[]];
    p = If[i < Length[word], word[[i]] - word[[i + 1]], word[[i]]];
    p = (*Together[*)p(*]*);
    If[p === 0, Continue[]];
    
    DebugPrint["[TW] Calling LinearFactors on: ", Short[p]];
    facts = LinearFactors[p, var];
    DebugPrint["[TW] LinearFactors returned: ", If[facts === $Failed, "$Failed", Short[facts]]];
    
    If[facts === $Failed,
      DebugPrint["[TW] *** DETECTED $Failed from LinearFactors ***"];
      failed = True;
      Break[]
    ];
    If[Length[facts] === 0, Continue[]];
    
    (*First recursive call*)
    If[i < Length[word],
      sub = Join[Take[word, i], Drop[word, i + 1]];
      If[Length[sub] > 0 && sub[[-1]] =!= 0,
        DebugPrint["[TW] First recursive call with sub=", Short[sub]];
        subResult = TransformWord[sub, var];
        DebugPrint["[TW] First recursive returned: ", If[subResult === $Failed, "$Failed", "OK"]];
        If[subResult === $Failed,
          DebugPrint["[TW] *** DETECTED $Failed from first recursive ***"];
          failed = True;
          Break[]
        ];
        Do[
          If[!KeyExistsQ[resultTable, s[[2]]], resultTable[s[[2]]] = <||>];
          Do[
            Do[
              newWord = Prepend[wrd[[2]], pole[[2]]];
              coef = pole[[1]] * wrd[[1]] * 1;
              resultTable[s[[2]]][newWord] = 
                Lookup[resultTable[s[[2]]], Key[newWord], 0] + coef,
              {pole, facts}],
            {wrd, s[[1]]}],
          {s, subResult}]
      ]
    ];
    
    If[failed, DebugPrint["[TW] Breaking after first recursive"]; Break[]];
    
    (*Second recursive call*)
    sub = Join[Take[word, i - 1], Drop[word, i]];
    If[Length[sub] === 0 || sub[[-1]] =!= 0,
      DebugPrint["[TW] Second recursive call with sub=", Short[sub]];
      subResult = TransformWord[sub, var];
      DebugPrint["[TW] Second recursive returned: ", If[subResult === $Failed, "$Failed", "OK"]];
      If[subResult === $Failed,
        DebugPrint["[TW] *** DETECTED $Failed from second recursive ***"];
        failed = True;
        Break[]
      ];
      Do[
        If[!KeyExistsQ[resultTable, s[[2]]], resultTable[s[[2]]] = <||>];
        Do[
          Do[
            newWord = Prepend[wrd[[2]], pole[[2]]];
            coef = pole[[1]] * wrd[[1]] * (-1);
            resultTable[s[[2]]][newWord] = 
              Lookup[resultTable[s[[2]]], Key[newWord], 0] + coef,
            {pole, facts}],
          {wrd, s[[1]]}],
        {s, subResult}]
    ],
    {i, Length[word]}
  ];
  
  DebugPrint["[TW] After Do loop: failed=", failed];
  
  If[failed,
    DebugPrint["[TW] *** RETURNING $Failed ***"];
    $TransformWordCache[key] = $Failed;
    Return[$Failed]
  ];
  
  finalResult = {};
  Do[
    wordPairs = KeyValueMap[{QuickCancel[#2], #1} &, resultTable[reginfsKey]];
    wordPairs = Select[wordPairs, #[[1]] =!= 0 &];
    If[Length[wordPairs] > 0,
      AppendTo[finalResult, {wordPairs, reginfsKey}]
    ],
    {reginfsKey, Keys[resultTable]}
  ];
  
  DebugPrint["[TW] Returning finalResult with ", Length[finalResult], " entries"];
  $TransformWordCache[key] = finalResult;
  finalResult
]]
 

CombineShuffleKeys[a_, b_] := Module[{aW, bW},
  aW = Which[a === {}, {}, WordQ[a], {a}, True, a];
  bW = Which[b === {}, {}, WordQ[b], {b}, True, b];
  Sort[Join[aW, bW]]]
  
(*Normalize shuffle key to always be a list of words*)
NormalizeShuffleKey[L_] := Which[
  L === {}, {},
  WordQ[L], {L},  (*Wrap single word to {{word}} format*)
  True, L  (*Already a list of words*)
]


(*Zero-to-one period evaluation*)
ZeroOnePeriod[{}] := 1

ZeroOnePeriod[word_List] := Module[{letters, sorted, ratio},
  letters = Complement[Union[word], {0}];
  
  Which[
    (*All zeros regularize to 0*)
    Union[word] === {0} && Length[word] > 0,
    0,
    
    (*Regularize trailing zeros*)
    word[[-1]] === 0,
    Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, Reg0[{{1, word}}]]],
    
    (*Regularize leading 1's*)
    word[[1]] === 1,
    Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, RegHead[{{1, word}}, 1]]],
   
    (*MZVs: letters in {-1, 0, 1}*)
    SubsetQ[{-1, 0, 1}, Union[word]],
    ToMZV[{{1, word}}] //. mzvAllReductions // Cancel,
    
    (*Necessary? Handle words with 1/2 letters (from z -> 1-z transformation)*)
    SubsetQ[{-1, 0, 1/2, 1}, Union[word]],
    (*Transform via z -> 1-z to get letters in {-1, 0, 1/2, 1} -> {0, 1/2, 1, 2}*)
    (*Then use the standard conversions*)
    Total[Map[#[[1]] * ZeroOnePeriodReduced[#[[2]]] &, 
      TransformZeroOne[{{1, word}}]]],
    
    (*General case: try to reduce via standard transformations*)
    True,
    Module[{result},
      result = TryReduceZeroOnePeriod[word];
      If[FreeQ[result, ZeroOnePeriod] && FreeQ[result, Hlog],
        result,
        (*If can't reduce, return symbolic but log a warning...*)
        If[$HyperVerbosity > 0,
          Print["Warning: Could not evaluate ZeroOnePeriod[", word, "]"]
        ];
        Hlog[1, word]
      ]
    ]
  ]
]


(* ::Section::Closed:: *)
(*Rational Functions Operations*)


HYleadingLaurentOrder[expr_,x_]:=Module[{s=Series[expr,{x,0,0}(*?x->0*)]},If[Head[s]===SeriesData,s[[4]],Exponent[Together[expr],x,Min](*Abort[];*)]]
PoleDegree[p_, var_] := Module[{key, f, num, den, degNum, degDen},
  If[!FreeQ[p, List], Return[0]];
  If[p === 0, Return[Infinity]];  
  key = {p, var};
  If[KeyExistsQ[$PoleDegreeCache, key], Return[$PoleDegreeCache[key]]];
  If[FreeQ[p, var], $PoleDegreeCache[key] = 0; Return[0]];
  $PoleDegreeCache[key] = HYleadingLaurentOrder[p,var]
  ]


(*PoleDegree[p_, var_] := Module[{key, f, num, den, degNum, degDen},
  If[!FreeQ[p, List], Return[0]];
  If[p === 0, Return[Infinity]];  
  key = {p, var};
  If[KeyExistsQ[$PoleDegreeCache, key], Return[$PoleDegreeCache[key]]];
  If[FreeQ[p, var], $PoleDegreeCache[key] = 0; Return[0]];
  f = Together[p];
  {num, den} = NumeratorDenominator[f];
  degNum = Exponent[num, var, Min];
  degDen = Exponent[den, var, Min];
  $PoleDegreeCache[key] = degNum - degDen]*)


RatResidue[f_, var_] := Module[{key, tog, p, q, minP, minQ, result},
  If[f === 0, Return[0]];
  If[FreeQ[f, var], Return[f]];
  key = {f, var};
  If[KeyExistsQ[$RatResidueCache, key], Return[$RatResidueCache[key]]];
  tog = Together[f];
  {p, q} = NumeratorDenominator[tog];
  If[p === 0, $RatResidueCache[key] = 0; Return[0]];
  minP = Exponent[p, var, Min];
  minQ = Exponent[q, var, Min];
  result = (*Together*)Cancel[Coefficient[p, var, minP] / Coefficient[q, var, minQ]];
  $RatResidueCache[key] = result]


LinearFactors[p_, var_] := MaybeQuiet[Module[
  {key, facts, result = {}, sub, zero, pureFunc, deg, failed = False},

  key = {p, var};
  If[KeyExistsQ[$LinearFactorsCache, key], Return[$LinearFactorsCache[key]]];
  If[FreeQ[p, var], $LinearFactorsCache[key] = {}; Return[{}]];
  facts = FactorList[p, Extension -> $HyperSplittingField];

  
  Do[
    If[failed, Break[]];
    If[Exponent[sub[[1]], var] === 0, Continue[]];
    deg = Exponent[sub[[1]], var];
    
    If[deg === 1,
      zero = -Coefficient[sub[[1]], var, 0] / Coefficient[sub[[1]], var, 1];
      AppendTo[result, {sub[[2]], Together[zero]}],

      (*Degree > 1*)
      If[deg === 2 && TrueQ[$HyperIntroduceAlgebraicLetters],
        Module[{lcAL, bAL, cAL, discAL, idxAL},
          lcAL   = Coefficient[sub[[1]], var, 2];
          bAL    = Coefficient[sub[[1]], var, 1];
          cAL    = Coefficient[sub[[1]], var, 0];
          discAL = bAL^2 - 4 lcAL cAL;
          $HyperAlgebraicLetterCounter++;
          idxAL = $HyperAlgebraicLetterCounter;
          $HyperAlgebraicLetterTable[idxAL] = <|
            "Polynomial"   -> sub[[1]],
            "Variable"     -> var,
            "LC"           -> lcAL,
            "Sum"          -> Together[-bAL/lcAL],
            "Product"      -> Together[cAL/lcAL],
            "Discriminant" -> discAL,
            "WmValue"      -> Together[(-bAL - Sqrt[discAL])/(2 lcAL)],
            "WpValue"      -> Together[(-bAL + Sqrt[discAL])/(2 lcAL)]
          |>;
          AppendTo[result, {sub[[2]], Wm[idxAL]}];
          AppendTo[result, {sub[[2]], Wp[idxAL]}];
        ];
        Continue[]
      ];

      If[$HyperAlgebraicRoots,
        pureFunc = Function @@ {{var}, sub[[1]]};
        Do[
          AppendTo[result, {sub[[2]], Root[pureFunc, k]}],
          {k, 1, deg}
        ],

        If[$HyperIgnoreNonlinearPolynomials,
          If[$HyperVerbosity > 0, 
            Print["discarding nonlinear ", sub[[1]]]];
          $NonlinearFactorsCache[{sub[[1]], var}] = <|
            "Polynomial" -> sub[[1]],
            "Variable" -> var,
            "Degree" -> deg,
            "Fatal" -> False
          |>;
          Continue[],
          (* If[$NoAlgebraicRootsContributions===True,
          Message[LinearFactors::nonlinear, sub[[1]], var],
          Message[LinearFactorsE::nonlinear, sub[[1]], var]
          ]; *)
          $NonlinearFactorsCache[{sub[[1]], var}] = <|
            "Polynomial" -> sub[[1]],
            "Variable" -> var,
            "Degree" -> deg,
            "Fatal" -> True
          |>;
          failed = True;
          Break[]
        ]
      ]
    ],
    {sub, If[Length[facts] > 0, Rest[facts], {}]}
  ];
  If[failed,
    (*Cache the result; 0 under $NoAlgebraicRootsContributions, $Failed otherwise.*)
    $LinearFactorsCache[key] = If[$NoAlgebraicRootsContributions===True,0,$Failed];
    If[$NoAlgebraicRootsContributions === True && TrueQ[$HyperWarnZeroed],
      Message[LinearFactors::zeroed, var]
    ];
    Return[If[$NoAlgebraicRootsContributions===True,0,$Failed]]
  ];

  $LinearFactorsCache[key] = result;
  result
]]
LinearFactorsE::nonlinear = "`1` does not factor linearly in `2`. Check that your integration order is linearly reducible and try rerunning with a different ordering.";
LinearFactors::nonlinear = "`1` does not factor linearly in `2`. Assuming the resulting contributions are spurious and cancel by compatibility graph consistency. As a check, verify your integration order is linearly reducible and try a different ordering if needed.";
LinearFactors::zeroed = "Non-linear factor(s) in variable `1` were set to zero because $NoAlgebraicRootsContributions is True. This is only correct if the ordering passed to HyperInt[] is linearly reducible; if you are not sure, double-check your ordering or call PrintNonlinearSummary[]. Set $HyperWarnZeroed = False to silence this reminder.";

(*To get information about non-linear factors*)
(*Get all cached nonlinear factors*)
GetNonlinearFactors[] := $NonlinearFactorsCache
(*Check if any nonlinear factors were encountered*)
HasNonlinearFactors[] := Length[$NonlinearFactorsCache] > 0
(*Get only the fatal (non-ignored) nonlinear factors*)
GetFatalNonlinearFactors[] := Select[
  $NonlinearFactorsCache, 
  Lookup[#, "Fatal", False] &
]
(*Print a summary of nonlinear factors encountered*)
PrintNonlinearSummary[] := Module[{},
  If[Length[$NonlinearFactorsCache] === 0,
    Print["No nonlinear factors encountered."],
    Print["Nonlinear factors encountered: ", Length[$NonlinearFactorsCache]];
    Do[
      Print["  ", key[[1]], " in variable ", key[[2]], 
        If[Lookup[$NonlinearFactorsCache[key], "Fatal", False], 
          " [FATAL]", " [ignored]"]],
      {key, Keys[$NonlinearFactorsCache]}
    ]
  ]
]
ClearNonlinearCache[] := ($NonlinearFactorsCache = <||>;)


(* === Accessors for algebraic letters introduced by LinearFactors === *)

(* Convention: raw `Wm[i]` / `Wp[i]` notation is preserved in every Mathematica
   output (StandardForm, TraditionalForm, OutputForm, InputForm). No MakeBoxes
   subscript rendering at the kernel level \[LongDash] subscripted `W^{\pm}_i` pills live
   only in the UI (ui/app.js via wDefinitions.kind = "algebraic"). *)

GetAlgebraicBackSubRules[] := Flatten @ KeyValueMap[
  {Wm[#1] -> #2["WmValue"], Wp[#1] -> #2["WpValue"]} &,
  $HyperAlgebraicLetterTable]

(* STShowAlgebraicLetters[]: pretty inspection of $HyperAlgebraicLetterTable.
   Usage: after STIntegrate[..., FindRoots -> True] returns a SeriesData that
   contains Wm[i]/Wp[i] atoms, call STShowAlgebraicLetters[] to see the
   per-letter polynomial, variable, discriminant, and the two roots.
   In notebooks this returns a Dataset; in scripted kernels it falls back
   to a TableForm so wolframscript output stays legible. *)
STShowAlgebraicLetters[] := Module[{table = $HyperAlgebraicLetterTable,
    rows, notebookQ},
  If[Length[table] == 0,
    Print["[SubTropica] No algebraic letters currently defined. ",
          "Run STIntegrate[..., FindRoots -> True] first."];
    Return[Null]];
  notebookQ = TrueQ[$Notebooks];
  rows = KeyValueMap[
    Function[{i, r},
      <|
        "Pair"         -> Row[{Wm[i], ",  ", Wp[i]}],
        "Variable"     -> r["Variable"],
        "Polynomial"   -> r["Polynomial"],
        "Discriminant" -> r["Discriminant"],
        "Wm[i]"        -> r["WmValue"],
        "Wp[i]"        -> r["WpValue"]
      |>],
    table];
  If[notebookQ,
    Dataset[rows],
    TableForm[
      Values /@ rows,
      TableHeadings -> {None,
        {"Pair", "Variable", "Polynomial", "Discriminant",
         "Wm[i]", "Wp[i]"}}]]];

GetAlgebraicVietaRules[] := Flatten @ KeyValueMap[
  {Wm[#1] Wp[#1]   -> #2["Product"],
   Wp[#1] Wm[#1]   -> #2["Product"],
   Wm[#1] + Wp[#1] -> #2["Sum"]} &,
  $HyperAlgebraicLetterTable]

(* Vieta simplifier: reduces expr to use Wm[i], Wp[i] only in at-most-linear
   form. Symmetric combinations collapse to rational expressions; antisymmetric
   residues keep Wm[i] - Wp[i].

   Algorithm: for each letter-index i present in expr,
     1. Apply Wm^n, Wp^n, Wm*Wp reductions until fixed point so every
        monomial in Wm[i], Wp[i] is linear.
     2. Decompose: expanded = a + b*Wm[i] + c*Wp[i], where a, b, c are
        Wm[i], Wp[i]-free. (Coefficient[_, Wm[i]] extracts b correctly when
        the expression is linear and expanded.)
     3. Rewrite as a + (b+c)/2 * sum_i + (b-c)/2 * (Wm[i] - Wp[i]).
        The symmetric part collapses; the antisymmetric part is preserved
        verbatim (per the convention: Wm, Wp only resolve through Vieta). *)
SimplifyWithVieta[expr_] := Module[{e = expr, indices, powerRules, sVal, pVal,
    a, b, c, expanded, reconstructed, den,
    t0, tFp, tDen, tCoef, tRec, tOK, tScan,
    instrument = TrueQ[$stPostStageInstrumentation]},
  If[instrument,
    WriteString["stdout", "  [post]   SimplifyWithVieta: ENTER (ByteCount(e)=",
      ToString[ByteCount[e]], ")\n"]];
  t0 = SessionTime[];
  indices = Union @ Cases[e, (Wm | Wp)[i_] :> i, Infinity];
  tScan = SessionTime[] - t0;
  If[instrument,
    WriteString["stdout",
      "  [post]   SimplifyWithVieta: indexScan=", ToString[Round[tScan,0.01]],
      "s  -> ", ToString[Length[indices]],
      " letter index(es): ", ToString[indices], "\n"]];
  Do[
    sVal = $HyperAlgebraicLetterTable[i]["Sum"];
    pVal = $HyperAlgebraicLetterTable[i]["Product"];
    powerRules = {
      Wm[i]^n_Integer /; n >= 2 :> sVal Wm[i]^(n-1) - pVal Wm[i]^(n-2),
      Wp[i]^n_Integer /; n >= 2 :> sVal Wp[i]^(n-1) - pVal Wp[i]^(n-2),
      Wm[i]  Wp[i]    -> pVal
    };
    t0 = SessionTime[];
    (* FixedPoint ensures we converge: Expand may re-expose monomials that
       powerRules can reduce further. *)
    expanded = FixedPoint[Expand[# //. powerRules] &, Expand[e], 10];
    tFp = SessionTime[] - t0;
    (* Vieta collapse is only valid if `expanded` is polynomial in {Wm[i], Wp[i]};
       when Wm[i]/Wp[i] appears inside a denominator (e.g. 1/(Wm[i] - Wp[i])
       factors that arise from partial-fraction decompositions of deg-2
       denominators), Coefficient[_, Wm[i], k] returns 0 for all k and the
       naive reconstruction zeroes the expression. Skip those letters here;
       the rational-in-Wm/Wp residue is left verbatim for the caller. *)
    t0 = SessionTime[];
    den = Denominator[Together[expanded]];
    tDen = SessionTime[] - t0;
    If[!FreeQ[den, Wm[i]] || !FreeQ[den, Wp[i]],
      If[instrument,
        WriteString["stdout",
          "  [post]     idx=", ToString[i], " FixedPoint=", ToString[Round[tFp,0.01]],
          "s  Together-Denominator=", ToString[Round[tDen,0.01]],
          "s  skipped (Wm/Wp in denominator)\n"]];
      Continue[]];
    (* Coefficient[_, Wm[i], k] does NOT descend into Hlog/Mpl/Root arguments,
       so occurrences of Wm[i]/Wp[i] inside Hlog[..] are preserved verbatim. *)
    t0 = SessionTime[];
    a = Coefficient[Coefficient[expanded, Wm[i], 0], Wp[i], 0];
    b = Coefficient[expanded, Wm[i], 1];
    c = Coefficient[expanded, Wp[i], 1];
    tCoef = SessionTime[] - t0;
    t0 = SessionTime[];
    reconstructed = a + (b + c)/2 * sVal + (b - c)/2 * (Wm[i] - Wp[i]);
    tRec = SessionTime[] - t0;
    (* Final exactness check: if expansion minus reconstructed isn't zero,
       the original is still not captured by (a, b, c) \[LongDash] keep it as-is. *)
    t0 = SessionTime[];
    If[PossibleZeroQ[Together[expanded - reconstructed]],
      e = reconstructed];
    tOK = SessionTime[] - t0;
    If[instrument,
      WriteString["stdout",
        "  [post]     idx=", ToString[i], " FixedPoint=", ToString[Round[tFp,0.01]],
        "s  Together-Den=", ToString[Round[tDen,0.01]],
        "s  Coef3=", ToString[Round[tCoef,0.01]],
        "s  Recon=", ToString[Round[tRec,0.01]],
        "s  ExactnessCheck=", ToString[Round[tOK,0.01]], "s\n"]];
  , {i, indices}];
  e
]


(* CanonicalizeAlgebraicLetters[results_:None]: prune unused, dedupe, renumber.
     - If `results` is given, drop any index i for which neither Wm[i] nor Wp[i]
       appears anywhere inside `results` (Fubini-intermediate letters that
       cancelled in the final expression).
     - Two survivors i, j are considered equivalent iff
         Sort[{Together[WmValue[i]], Together[WpValue[i]]}] ===
         Sort[{Together[WmValue[j]], Together[WpValue[j]]}]
       i.e. up to the mirror swap Wm <-> Wp. For each equivalence class, keep
       the smallest index; drop the rest.
     - Renumber survivors consecutively 1..k and MUTATE the table in place.
     - Return a rename-rule list {Wm[old] -> Wm[new] | Wp[new], ...} to apply
       to any expression carrying the letters. When the non-survivor's sign
       convention was flipped relative to the survivor, the rule maps
       Wm[old] -> Wp[new] (and Wp[old] -> Wm[new]) so values stay consistent. *)
CanonicalizeAlgebraicLetters[] := CanonicalizeAlgebraicLetters[None];
CanonicalizeAlgebraicLetters[results_] := Module[
  {usedIdx, indices, groups, survivors, renameMap, newTable, renameRules},

  (* 1. Prune unused indices (only when `results` is given). *)
  If[results =!= None,
    usedIdx = Union @ Cases[results, (Wm | Wp)[i_Integer] :> i, Infinity];
    KeyDropFrom[$HyperAlgebraicLetterTable,
                Complement[Keys[$HyperAlgebraicLetterTable], usedIdx]]];

  indices = Sort @ Keys[$HyperAlgebraicLetterTable];
  If[Length[indices] === 0,
    $HyperAlgebraicLetterCounter = 0;
    Return[{}]];

  (* 2. Group indices by unordered root-pair key (mirror-swap equivalence). *)
  groups = GatherBy[indices,
    With[{m = Together[$HyperAlgebraicLetterTable[#]["WmValue"]],
          p = Together[$HyperAlgebraicLetterTable[#]["WpValue"]]},
      Sort[{m, p}]] &];
  survivors = Min /@ groups;
  renameMap = AssociationThread[Sort[survivors], Range[Length[survivors]]];

  (* 3. For each non-survivor, choose aligned vs. swapped rename by comparing
     its WmValue to the survivor's WmValue. *)
  renameRules = Flatten @ Map[
    Function[grp, Module[{s = Min[grp], sWm, sIdx},
      sWm = Together[$HyperAlgebraicLetterTable[s]["WmValue"]];
      sIdx = renameMap[s];
      Map[
        Function[o,
          With[{oWm = Together[$HyperAlgebraicLetterTable[o]["WmValue"]]},
            If[PossibleZeroQ[oWm - sWm],
              {Wm[o] -> Wm[sIdx], Wp[o] -> Wp[sIdx]},
              {Wm[o] -> Wp[sIdx], Wp[o] -> Wm[sIdx]}]]],
        grp]]],
    groups];

  (* 4. Rebuild table with consecutive keys. *)
  newTable = AssociationThread[
    Range[Length[survivors]],
    $HyperAlgebraicLetterTable /@ Sort[survivors]];
  $HyperAlgebraicLetterTable = newTable;
  $HyperAlgebraicLetterCounter = Length[survivors];

  renameRules
]


(*Partial fractioning*)
Options[PartialFractions] = {"MaxWeight" -> Automatic};
PartialFractions[f_, var_, opts:OptionsPattern[]] := Module[
  {key, p, q, poles = {}, facs, zero, pre = 1, result, i, coef, fac, m, simp007, maxW},
  key = {f, var};
  If[KeyExistsQ[$PartialFractionsCache, key], Return[$PartialFractionsCache[key]]];
  If[f === 0, $PartialFractionsCache[key] = {0}; Return[{0}]];
  p = simp007 = Together[f];
  q = Denominator[p];
  p = Numerator[p];
  If[FreeQ[q, var], $PartialFractionsCache[key] = {p/q}; Return[{p/q}]];
  facs = FactorList[q, Extension -> $HyperSplittingField];
  If[Length[facs] > 0, pre = 1/facs[[1, 1]]];
  Do[
    If[Exponent[fac[[1]], var] === 0, pre = pre / fac[[1]]^fac[[2]]; Continue[]];
    If[Exponent[fac[[1]], var] > 1,
      (* Degree-2 denominator factor: if $HyperIntroduceAlgebraicLetters,
         introduce fresh Wm[i]/Wp[i] letters (mirroring LinearFactors[]),
         rewrite the factor as LC*(var-Wm)*(var-Wp) in simp007, and add
         both simple poles.  Higher-degree factors still fall through
         to the old nonlinear-handling branch below. *)
      If[Exponent[fac[[1]], var] === 2 && TrueQ[$HyperIntroduceAlgebraicLetters],
        Module[{lcAL, bAL, cAL, discAL, idxAL, wmSym, wpSym, factored},
          lcAL   = Coefficient[fac[[1]], var, 2];
          bAL    = Coefficient[fac[[1]], var, 1];
          cAL    = Coefficient[fac[[1]], var, 0];
          discAL = bAL^2 - 4 lcAL cAL;
          $HyperAlgebraicLetterCounter++;
          idxAL = $HyperAlgebraicLetterCounter;
          wmSym = Wm[idxAL]; wpSym = Wp[idxAL];
          $HyperAlgebraicLetterTable[idxAL] = <|
            "Polynomial"   -> fac[[1]],
            "Variable"     -> var,
            "LC"           -> lcAL,
            "Sum"          -> Together[-bAL/lcAL],
            "Product"      -> Together[cAL/lcAL],
            "Discriminant" -> discAL,
            "WmValue"      -> Together[(-bAL - Sqrt[discAL])/(2 lcAL)],
            "WpValue"      -> Together[(-bAL + Sqrt[discAL])/(2 lcAL)]
          |>;
          (* Rewrite the denominator factor in simp007.  We do
             simp007 -> simp007 * fac/factored, which keeps Together
             well-behaved because fac divides the original denominator. *)
          factored = lcAL (var - wmSym) (var - wpSym);
          simp007 = Together[simp007 * (fac[[1]]/factored)^fac[[2]]];
          AppendTo[poles, {wmSym, fac[[2]], var - wmSym}];
          AppendTo[poles, {wpSym, fac[[2]], var - wpSym}];
          Continue[]
        ]
      ];
      (* Fallback: legacy non-linear-factor handling (deg >= 3 or flag off) *)
      $NonlinearFactorsCache[{fac[[1]], var}] = <|
        "Polynomial" -> fac[[1]],
        "Variable"   -> var,
        "Degree"     -> Exponent[fac[[1]], var],
        "Fatal"      -> ($NoAlgebraicRootsContributions =!= True)
      |>;
      If[$NoAlgebraicRootsContributions === True && TrueQ[$HyperWarnZeroed],
        Message[LinearFactors::zeroed, var]
      ];
      If[$NoAlgebraicRootsContributions === True,
        $PartialFractionsCache[key] = {0};
        Return[{0}],
        (* Use Throw so $Failed propagates past any Module/Return boundaries *)
        $PartialFractionsCache[key] = $Failed;
        Throw[$Failed, "HyperNonlinear"]
      ]
    ];
    zero = -Coefficient[fac[[1]], var, 0] / Coefficient[fac[[1]], var, 1];
    AppendTo[poles, {Together[zero], fac[[2]], fac[[1]]}],
    {fac, If[Length[facs] > 0, Rest[facs], {}]}
  ];
  (*Extract the polynomial part via polynomial division; avoids the expensive Apart[] call.
    p and q are already the numerator/denominator of Together[f], so the polynomial part
    is exactly PolynomialQuotient[p, q, var]. No Apart[] needed.
    If $UseFFPolynomialQuotient is True, use the faster finite-field version instead.*)
  maxW = OptionValue["MaxWeight"];
  If[maxW === Automatic, maxW = $PartialFractionsMaxWeight];
  result = {If[$UseFFPolynomialQuotient && MatchQ[DownValues[SPQRPolynomialQuotient], {__}],
      SPQRPolynomialQuotient[p, q, var, "MaxWeight" -> maxW],
      PolynomialQuotient[p, q, var]
  ]};
  p =.; q =.; (* free numerator/denominator; no longer needed after polynomial division *)
  (*
     Coefficient extraction:
     OLD: Called SeriesCoefficient m times per pole (SLOW)
     NEW: Compute all derivatives once with NestList (FASTER)
     Idea:
       If f has a pole of order m at a, then:
       f = Sum_j c_j/(z-a)^j + (regular part)
       where c_j = (1/(m-j)!) * d^(m-j)/dz^(m-j) [(z-a)^m * f(z)] |_{z=a}
     *)
  Do[
      m = poles[[i, 2]];           (*Pole order*)
      coef = {poles[[i, 1]]};      (*Start with pole location*)
      
      Module[{expr, a, derivs, derivOrder},
        a = poles[[i, 1]];
        (*Remove the pole by multiplying by (var - a)^m*)
        expr = Cancel[(var - a)^m * simp007];
        (*Compute derivatives 0 through m-1 in a single pass*)
        (*This is O(m) instead of O(m^2) for repeated Series calls*)
        derivs = NestList[D[#, var] &, expr, m - 1];
        (*Extract coefficients for 1/(var-a)^1, 1/(var-a)^2, ..., 1/(var-a)^m*)
        Do[
          derivOrder = m - j;  (*j=1: order m-1; j=m: order 0*)
          AppendTo[coef, 
            flag6[(*Cancel[*)derivs[[derivOrder + 1]] / Factorial[derivOrder] /. var -> a(*]*)]
          ],
          {j, 1, m}
        ]
      ];
      AppendTo[result, coef],
      {i, Length[poles]}
    ];
  
  simp007 =.; facs =.; (* free large intermediates ; no longer needed after coefficient extraction *)
  $PartialFractionsCache[key] = result;
  result
]


(*(*myApart[f_,var_]:=Apart[f,var];*)

Get["LinApart`"];
myApart[f_,var_]:=Block[{tog,num,den,coeffrules},
tog=f//Together;
num=tog//Numerator;
den=tog//Denominator;
coeffrules=num//CoefficientRules[#,var]&;
Sum[cf[[2]] LinApart[var^cf[[1,1]] 1/den,var],{cf,coeffrules}]
]

PartialFractions[f_, var_] := Module[
  {key, p, q, poles = {}, facs, zero, pre = 1, result, appart, i, coef, fac, m, simp007,Apart=myApart},
  key = {f, var};
  If[KeyExistsQ[$PartialFractionsCache, key], Return[$PartialFractionsCache[key]]];
  If[f === 0, $PartialFractionsCache[key] = {0}; Return[{0}]];
  flag2[p = simp007 = Together[f]]; 
  q = Denominator[p]; 
  p = Numerator[p];
  If[FreeQ[q, var], $PartialFractionsCache[key] = {p/q}; Return[{p/q}]];
  facs = FactorList[q, Extension -> $HyperSplittingField];
  If[Length[facs] > 0, pre = 1/facs[[1, 1]]];
  Do[
    If[Exponent[fac[[1]], var] === 0, pre = pre / fac[[1]]^fac[[2]]; Continue[]];
    If[Exponent[fac[[1]], var] > 1, Return[$Failed]];
    zero = -Coefficient[fac[[1]], var, 0] / Coefficient[fac[[1]], var, 1];
    flag3[AppendTo[poles, {Together[zero], fac[[2]], fac[[1]]}]],
    {fac, If[Length[facs] > 0, Rest[facs], {}]}
  ];
  (* Extract polynomial part using Apart *)
  flag4[result = {Module[{terms, polyTerms},
    appart = Apart[simp007, var];(*<- improve with LinearApart[]?*)
    terms = If[Head[appart] === Plus, List @@ appart, {appart}];
    polyTerms = Select[terms, PolynomialQ[#, var] &]; 
    Total[polyTerms]
  ]}];
  (* ================================================================
     Coefficient extraction:
     OLD: Called SeriesCoefficient m times per pole (SLOW)
     NEW: Compute all derivatives once with NestList (FASTer)
     Idea:
       If f has a pole of order m at a, then:
       f = Subscript[\[CapitalSigma], j] Subscript[c, j]/(z-a)^j + (regular part)
       where Subscript[c, j] = (1/(m-j)!) \[CenterDot] d^(m-j)/dz^(m-j) [(z-a)^m \[CenterDot] f(z)] |_{z=a}
     ================================================================ *)
  flag5[Do[
      m = poles[[i, 2]];           (* Pole order *)
      coef = {poles[[i, 1]]};      (* Start with pole location *)
      
      Module[{expr, a, derivs, derivOrder},
        a = poles[[i, 1]];
        (* Remove the pole by multiplying by (var - a)^m *)
        expr = Cancel[(var - a)^m * simp007];
        (* Compute derivatives 0 through m-1 in a single pass *)
        (* This is O(m) instead of O(m^2) for repeated Series calls *)
        derivs = NestList[D[#, var] &, expr, m - 1];
        (* Extract coefficients for 1/(var-a)^1, 1/(var-a)^2, ..., 1/(var-a)^m *)
        Do[
          derivOrder = m - j;  (* j=1 \[RightArrow] order m-1; j=m \[RightArrow] order 0 *)
          AppendTo[coef, 
            flag6[(*Cancel[*)derivs[[derivOrder + 1]] / Factorial[derivOrder] /. var -> a(*]*)]
          ],
          {j, 1, m}
        ]
      ];
      AppendTo[result, coef],
      {i, Length[poles]}
    ]];
  
  $PartialFractionsCache[key] = result; 
  result
]*)


(*Series expansion*)
SeriesExpansion[ratPoly_, var_, maxOrder_: 0] := Module[{key, ser, poly, minDeg, result},
  If[ratPoly === 0, Return[0]];
  key = {ratPoly, var, maxOrder};
  If[KeyExistsQ[$SeriesExpansionCache, key], Return[$SeriesExpansionCache[key]]];
  ser = Series[(*Together[*)ratPoly(*]*), {var, 0, maxOrder (*+ 2*)}];
  poly = Normal[ser]; If[poly === 0, $SeriesExpansionCache[key] = 0; Return[0]];
  minDeg = Exponent[poly, var, Min]; If[!IntegerQ[minDeg], minDeg = 0];
  result = Sum[var^n * Coefficient[poly, var, n], {n, minDeg, maxOrder}];
  $SeriesExpansionCache[key] = result;
  result]


(* ::Section::Closed:: *)
(*Converters & Mapping to Periods*)


  ConvertToHlogRegInf[expr_] := Module[{result, i, var, word},
  Which[
    ListQ[expr] && Length[expr] > 0 && ListQ[expr[[1]]], expr,
    Head[expr] === Plus, 
      CollectWords[Flatten[Map[ConvertToHlogRegInf, List @@ expr], 1]],
    Head[expr] === Times, result = {{1, {}}}; 
      Do[result = ShuffleSymbolic[result, ConvertToHlogRegInf[Part[expr, i]]], 
        {i, Length[expr]}]; result,
    Head[expr] === Hlog, var = expr[[1]]; word = expr[[2]];
      If[word === {}, Return[{{1, {}}}]]; 
      If[var === 0, Return[{}]];
      If[AllTrue[word, # === 0 &], 
        If[var === 1, Return[{{1, {}}}]];
        Return[{{(-1)^Length[word] / Length[word]!, 
          {Table[-var, {Length[word]}]}}}]];
      If[word[[-1]] === 0, 
        Return[ConvertToHlogRegInf[Total[Map[#[[1]] * Hlog[var, #[[2]]] &,
          RegTail[{{1, word}}, 0, If[var === 1, 0, Hlog[var, {0}]]]]]]]];
      If[word[[1]] === var, Return[$Failed]];
      If[AllTrue[word, # === word[[1]] &],
        Return[{{(-1)^Length[word] / Length[word]!, 
          {Table[var/word[[1]] - 1, {Length[word]}]}}}]];
      result = {{1, {}}}; 
      Do[If[word[[i]] === 0, result = ConcatMul[{{1, {-1}}}, result],
        result = ConcatMul[{{1, {-1}}, {-1, {var/word[[i]] - 1}}}, result]], 
        {i, Length[word]}];
      Map[{#[[1]], {#[[2]]}} &, result],
    Head[expr] === Log, ConvertToHlogRegInf[Hlog[expr[[1]], {0}]],
    Head[expr] === PolyLog, 
      ConvertToHlogRegInf[MplAsHlog[{expr[[1]]}, {expr[[2]]}]],
    Head[expr] === Power, i = expr[[2]];
      If[IntegerQ[i] && i > 0, result = {{1, {}}}; 
        word = ConvertToHlogRegInf[expr[[1]]];
        Do[result = ShuffleSymbolic[result, word], {i}]; result, 
        {{expr, {}}}],
    NumericQ[expr], {{expr, {}}},
    True, {{expr, {}}}]]


MplAsHlog[ns_List, zs_List] := Module[{i, j},
  If[MemberQ[zs, 0], Return[0]];
  (-1)^Length[ns] * Hlog[1, 
    Flatten[Table[Join[Table[0, {ns[[-i]] - 1}], 
      {1/Product[zs[[j]], {j, -i, -1}]}], {i, 1, Length[ns]}]]]]


(*Contour deformation for positive letters*)
(*Convert integral from [A,B] to [0,\[Infinity])*)
(*Can handle "irregular" periods now.*)
ConvertABtoZeroInf[wordlist_List, A_, B_] := Module[{result = <||>, w, v, i, u},
  Do[
    v = {{w[[1]], {}}};
    Do[
      If[w[[2, i]] === B,
        v = Map[{-#[[1]], Append[#[[2]], -1]} &, v],
        (*else*)
        v = Flatten[Map[
          {{-#[[1]], Append[#[[2]], -1]}, 
           {#[[1]], Append[#[[2]], (w[[2, i]] - A)/(B - w[[2, i]])]}} &,
          v], 1]
      ],
      {i, Length[w[[2]]]}
    ];
    Do[
      result[u[[2]]] = Lookup[result, Key[u[[2]]], 0] + u[[1]],
      {u, v}
    ],
    {w, wordlist}
  ];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]
]

(*Convert zero-to-infinity integral to zero-to-one*)
ConvertZeroOne[wordlist_List] := Module[{result = <||>, w, v, i, term},
  Do[
    v = {{w[[1]], {}}};
    Do[
      If[w[[2, i]] === -1,
        v = ConcatMul[{{1, {0}}}, v],
        v = ConcatMul[{{1, {0}}, {-1, {1/(1 + w[[2, i]])}}}, v]
      ],
      {i, Length[w[[2]]]}
    ];
    Do[
      result[term[[2]]] = Lookup[result, Key[term[[2]]], 0] + term[[1]],
      {term, v}
    ],
    {w, wordlist}
  ];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]
]

(*Convert int_1^inf to int_0^1 via z->1/z then reverse*)
Convert1InfTo01[wordlist_List] := Module[{result = <||>, w, v, i, u},
  Do[
    v = {{w[[1]], {}}};
    Do[
      If[w[[2, i]] === 0,
        v = Map[{#[[1]], Prepend[#[[2]], 0]} &, v],
        v = Flatten[Map[
          {{#[[1]], Prepend[#[[2]], 0]}, 
           {-#[[1]], Prepend[#[[2]], 1/w[[2, i]]]}} &,
          v], 1]
      ],
      {i, Length[w[[2]]]}
    ];
    Do[
      result[u[[2]]] = Lookup[result, Key[u[[2]]], 0] + u[[1]],
      {u, v}
    ],
    {w, wordlist}
  ];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]
]


ToMZV[wordlist_List] := Module[{result = 0, w, counts, poles, i, sign},
  Do[
    Which[
      w[[2]] === {}, 
      result += w[[1]],
      
      w[[2, 1]] === 1 || w[[2, -1]] === 0,
      (*Divergent - should not happen after regularization!*)
      Print["Warning: Divergent word in ToMZV: ", w[[2]]]; 
      result += w[[1]] * Hlog[1, w[[2]]],
      
      True,
      counts = {}; poles = {};
      Do[
        If[w[[2, i]] === 0,
          If[Length[counts] > 0, counts[[-1]]++],
          AppendTo[counts, 1]; AppendTo[poles, w[[2, i]]]
        ],
        {i, Length[w[[2]]], 1, -1}
      ];
      If[Length[counts] > 0,
        AppendTo[poles, 1];
        sign = (-1)^Length[counts];
        result += w[[1]] * sign * 
          Apply[mzv, Table[counts[[j]] * poles[[j + 1]] / poles[[j]], 
            {j, Length[counts]}]]
      ]
    ],
    {w, wordlist}
  ];
  result
]


(*Evaluate zero-to-infinity period*)
ZeroInfPeriodEval[{}] := 1

ZeroInfPeriodEval[word_List] := Module[
  {letters, scale, sorted, ratio},
  
  letters = Complement[Union[word], {0}];
  
  (*Positive letters - error, should be handled by BreakUpContour*)
  If[AnyTrue[letters, Positive],
    Print["Error: Positive letter in ZeroInfPeriodEval: ", word];
    (*Return unevaluated but prevent recursion*)
    Return[Inactive[ZeroInfPeriod][word]]
  ];
  
  Which[
    (*Empty letters means all zeros - regularizes to 0*)
    letters === {}&& Length[word] > 0,  (*Note: add Length[word] > 0 ?*)
    0,
    
    (*Regularize trailing zeros*)
    word[[-1]] === 0,
    Total[Map[#[[1]] * ZeroInfPeriodEval[#[[2]]] &, Reg0[{{1, word}}]]],
    
    (*MZVs: only -1 letters*)
    letters === {-1},
    Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, ConvertZeroOne[{{1, word}}]]],
    
    (*Single letter: rescale to -1*)
    Length[letters] === 1,
    ZeroInfPeriodRescale[word, -First[letters]],
    
    (*Two letters forming alternating sums*)
    Length[letters] === 2,
    sorted = Sort[letters];
    ratio = sorted[[2]] / sorted[[1]];
    If[MemberQ[{2, 1/2, -1}, ratio],
      (*Determine scale factor*)
      scale = Which[
        ratio === 2, -sorted[[1]],
        ratio === 1/2, -sorted[[2]],
        ratio === -1, -Max[Abs /@ sorted],
        True, 1
      ];
      If[scale =!= 1,
        ZeroInfPeriodRescale[word, scale],
        (*Transform {-2,-1,0} to {-1,0,1} for alternating sums*)
        Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, 
          Convert1InfTo01[{{1, Map[# + 1 &, word]}}]]]
      ],
      (*General two-letter case*)
      Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, ConvertZeroOne[{{1, word}}]]]
    ],
    
    (*Polylogs at roots of unity: all letters have absolute value 1*)
    AllTrue[letters, Abs[#] === 1 &],
    Module[{a, b, result = 0},
      Do[
        result += ZeroOnePeriod[word[[a + 1 ;; Length[word]]]] *
          Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, 
            Convert1InfTo01[{{1, word[[1 ;; a]]}}]]],
        {a, 0, Length[word]}
      ];
      result
    ],
    
    (*General case*)
    True,
    Total[Map[#[[1]] * ZeroOnePeriod[#[[2]]] &, ConvertZeroOne[{{1, word}}]]]
  ]
]

ZeroInfPeriodRescale[word_List, scale_] := Module[
  {k, result, u, onAxis, above, below, sub, logFac},
  
  (*MorE robust check for scale = 1*)
  If[scale === 1 || Together[scale - 1] === 0,
    Print["Error: ZeroInfPeriodRescale called with scale=1"];
    Return[ZeroInfPeriod[word]]
  ];
  
  result = 0;
  u = word / scale;
  above = {}; below = {};
  
  Do[
    If[Positive[u[[k]]],
      If[Im[scale] === 0,
        Print["Error: Singularity on integration path: ", u[[k]] * scale];
        Return[ZeroInfPeriod[word]]
      ];
      If[Im[scale] > 0, 
        above = Union[above, {u[[k]]}],
        below = Union[below, {u[[k]]}]
      ]
    ],
    {k, Length[u]}
  ];
  
  onAxis = Map[{#, If[MemberQ[above, #], 1, -1]} &, Sort[Union[above, below]]];
  
  Do[
    (*Handle 0^0: when k=0, (-Log[scale])^k/k! = 1*)
    logFac = If[k === 0, 1, (-Log[scale])^k / k!];
    If[above === {} && below === {},
      result += logFac * ZeroInfPeriodEval[u[[k + 1 ;; Length[word]]]],
      result += Total[Map[
        #[[1]] * Apply[Times, Map[ZeroInfPeriodEval, #[[2]]]] &,
        BreakUpContour[{{logFac, u[[k + 1 ;; Length[word]]]}}, onAxis]
      ]]
    ],
    {k, 0, Length[word]}
  ];
  result
]


(*Convert expression containing Hlog to Mpl notation*)
(*Note: useful to run after fibrationBasis[] in order to find relations between MPLs.*)
ConvertToMpl[expr_] := Module[{},
  expr /. {
    Hlog[z_, word_List] :> HlogAsMpl[z, word]
  }
]

HlogAsMpl[z_, {}] := 1;

HlogAsMpl[z_, word_List] := Module[
  {w = word, i, r, sigma, nu, result, regResult},
  
  (*Handle trailing zeros via shuffle regularization*)
  If[Last[w] === 0,
    regResult = RegTail[{{1, w}}, 0, Log[z]];
    (*Map over each {coef, newWord} pair explicitly*)
    Return[Total[
      Map[#[[1]] * HlogAsMpl[z, #[[2]]] &, regResult]
    ]]
  ];
  
  (*Read word from right to left*)
  i = Length[w];
  sigma = {};
  nu = {};
  r = 0;
  
  While[i >= 1,
    r++;
    AppendTo[sigma, w[[i]]];
    AppendTo[nu, 1];
    i--;
    While[i >= 1 && w[[i]] === 0,
      nu[[r]]++;
      i--;
    ];
  ];
  
  (*Build MPL*)
  result = (-1)^r * Mpl[
    nu,
    Append[Table[Together[sigma[[j + 1]]/sigma[[j]]], {j, 1, r - 1}], 
           Together[z/sigma[[r]]]]
  ];
  
  Simplify[result]
]


(*Convert ZeroInfPeriod in MPLs:*)

PositiveRealQ[x_] := TrueQ[Simplify[Im[x] == 0]] && TrueQ[Simplify[Re[x] > 0]]

ZeroInfPeriodAsMpl::contour = "Cannot determine contour direction for ZeroInfPeriodAsMpl[`1`]: unable to evaluate Im[`2`]. The result depends on whether `2` approaches the real axis from above or below.";

ZeroInfPeriodAsMpl::singularity = "ZeroInfPeriodAsMpl[`1`]: After rescaling, positive letter(s) `2` lie on the integration path [0,\[Infinity]). This integral requires contour deformation (producing I\[CenterDot]\[Pi] terms). Returning unevaluated.";

ZeroInfPeriodAsMpl[{}] := 1

ZeroInfPeriodAsMpl[word_List] := Module[
  {letters, scale, u, k, result, sorted, ratio, converted,
   testScale, symbolicVar, imTest, a, innerResult, rescaledWord, 
   positiveAfterRescale},
  
  letters = Complement[Union[word], {0}];
  
  Which[
    (*All zeros: regularizes to 0*)
    letters === {} && Length[word] > 0,
    0,
    
    (*Trailing zeros: regularize first*)
    word[[-1]] === 0,
    Total[#[[1]] * ZeroInfPeriodAsMpl[#[[2]]] & /@ Reg0[{{1, word}}]],
    
    (*Only -1 letters: convert to 0\[RightArrow]1 integral, then to Mpl*)
    letters === {-1},
    converted = ConvertZeroOne[{{1, word}}];
    Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ converted] // Simplify,
    
    (*Polylogs at roots of unity - all letters have |letter| = 1*)
    AllTrue[letters, TrueQ[Simplify[Abs[#] == 1]] &],
    (*Check for positive letters using safe comparison*)
    If[AnyTrue[letters, PositiveRealQ],
      Message[ZeroInfPeriodAsMpl::singularity, word, 
        Select[letters, PositiveRealQ]];
      Return[ZeroInfPeriod[word]]
    ];
    (*Safe to evaluate?*)
    innerResult = 0;
    Do[
      converted = Convert1InfTo01[{{1, word[[1 ;; a]]}}];
      innerResult += HlogAsMplSafe[1, word[[a + 1 ;; Length[word]]]] *
        Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ converted],
      {a, 0, Length[word]}
    ];
    innerResult // Simplify,
    
    (*Single non-{-1} letter: rescale to get -1 letters*)
    Length[letters] === 1 && letters =!= {-1},
    scale = -First[letters];
    u = word / scale;
    Sum[
      If[k === 0, 1, (-Log[scale])^k / k!] * 
        ZeroInfPeriodAsMpl[u[[k + 1 ;; Length[word]]]],
      {k, 0, Length[word]}
    ] // Simplify,
    
    (*Two letters with special ratio*)
    Length[letters] === 2,
    sorted = Sort[letters];
    ratio = Together[sorted[[1]] / sorted[[2]]];
    Which[
      ratio === 2,
      scale = -sorted[[2]];
      If[scale =!= 1 && Together[scale - 1] =!= 0,
        ZeroInfPeriodRescaleAsMplSafe[word, scale],
        Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@
          Convert1InfTo01[{{1, Map[# + 1 &, word]}}]] // Simplify
      ],

      ratio === 1/2,
      scale = -sorted[[1]];
      If[scale =!= 1 && Together[scale - 1] =!= 0,
        ZeroInfPeriodRescaleAsMplSafe[word, scale],
        Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ 
          Convert1InfTo01[{{1, Map[# + 1 &, word]}}]] // Simplify
      ],
      
      ratio === -1,
      (*Check for symbolic variables*)
      symbolicVar = First[Select[letters, Not@NumericQ[#] &], None];
      If[symbolicVar =!= None,
        imTest = Simplify[Im[symbolicVar]];
        If[!NumericQ[imTest],
          Message[ZeroInfPeriodAsMpl::contour, word, symbolicVar];
          If[$STContourHandling === "Continue",
            Return[Hlog[Infinity, word]],
            Abort[]
          ]
        ]
      ];
      (*Check fo positive letters after rescaling*)
      testScale = -Max[Abs /@ sorted];
      rescaledWord = word / testScale;
      positiveAfterRescale = Select[Union[rescaledWord], PositiveRealQ];
      If[Length[positiveAfterRescale] > 0,
        Message[ZeroInfPeriodAsMpl::singularity, word, 
          positiveAfterRescale * testScale];
        Return[ZeroInfPeriod[word]]
      ];
      (*Check for unit absolute values*)
      If[TrueQ[Simplify[Max[Abs /@ letters] == 1]],
        innerResult = 0;
        Do[
          converted = Convert1InfTo01[{{1, word[[1 ;; a]]}}];
          innerResult += HlogAsMplSafe[1, word[[a + 1 ;; Length[word]]]] *
            Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ converted],
          {a, 0, Length[word]}
        ];
        innerResult // Simplify,
        ZeroInfPeriodRescaleAsMplSafe[word, testScale]
      ],
      
      True,
      converted = ConvertZeroOne[{{1, word}}];
      Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ converted] // Simplify
    ],
    
    (*General case*)
    True,
    converted = ConvertZeroOne[{{1, word}}];
    Total[#[[1]] * HlogAsMplSafe[1, #[[2]]] & /@ converted] // Simplify
  ]
]

(*Also update ZeroInfPeriodRescaleAsMplSafe to use PositiveRealQ*)
ZeroInfPeriodRescaleAsMplSafe[word_List, scale_] := Module[
  {u, k, positiveAfterRescale},
  
  If[scale === 1 || Together[scale - 1] === 0,
    Return[ZeroInfPeriod[word]]
  ];
  
  u = word / scale;
  
  (*Check if any letters become positive real (on integration path)*)
  positiveAfterRescale = Select[Union[u], PositiveRealQ];
  If[Length[positiveAfterRescale] > 0,
    Message[ZeroInfPeriodAsMpl::singularity, word, 
      positiveAfterRescale * scale];
    Return[ZeroInfPeriod[word]]
  ];
  
  Sum[
    If[k === 0, 1, (-Log[scale])^k / k!] * 
      ZeroInfPeriodAsMpl[u[[k + 1 ;; Length[word]]]],
    {k, 0, Length[word]}
  ] // Simplify
]

(*Safe wrapper for HlogAsMpl that handles "edge cases"*)
HlogAsMplSafe[zArg_, {}] := 1

HlogAsMplSafe[zArg_, word_List] := Module[
  {result},
  
  (*Handle all-zeros word*)
  If[Union[word] === {0},
    If[zArg === 1, Return[0]];
    Return[Log[zArg]^Length[word] / Length[word]!]
  ];
  
  (*Use the existing HlogAsMpl*)
  result = HlogAsMpl[zArg, word];
  
  (*Simplify Mpl[{n}, {1}] = Zeta[n] for integer n > 1*)
  result /. {
    Mpl[{n_Integer}, {1}] /; n > 1 :> mzv[n],
    Mpl[{1}, {w_}] :> -Log[1 - w]
  }
]


(*Convert 1 \[RightArrow] \[Infinity] integral to 0 \[RightArrow] 1 via z\[RightArrow]1/z then reverse*)
Convert1InfTo01[wordlist_List] := Module[{result = <||>, w, v, i, u},
  Do[
    v = {{w[[1]], {}}};
    Do[
      If[w[[2, i]] === 0,
        v = Map[{#[[1]], Prepend[#[[2]], 0]} &, v],
        v = Flatten[Map[
          {{#[[1]], Prepend[#[[2]], 0]}, 
           {-#[[1]], Prepend[#[[2]], 1/w[[2, i]]]}} &,
          v], 1]
      ],
      {i, Length[w[[2]]]}
    ];
    Do[
      result[u[[2]]] = Lookup[result, Key[u[[2]]], 0] + u[[1]],
      {u, v}
    ],
    {w, wordlist}
  ];
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]
]


(*Write all the even zeta values in terms of zeta[2]*)
simplifyMZVeven[arg_]:=arg//.mzv[n_Integer?EvenQ] :> Module[{k = n/2},
  (-1)^(k + 1)*
   BernoulliB[n]/(2*Factorial[n])*
   (2^n*6^k)*mzv[2]^k
]


(* ::Section:: *)
(*Differentiation of Hlog[] and Mpl[]*)


DiffHlog[arg_, {}, var_] := 0

DiffHlog[arg_, word_List, var_] := Module[
  {result = 0, i, f, n = Length[word]},
  
  (*d/dvar of upper limit arg*)
  result = D[arg, var]/(arg - word[[1]]) * 
           If[n === 1, 1, Hlog[arg, Rest[word]]];
  
  (*Contributions from differences of neighboring letters*)
  Do[
    f = word[[i]] - word[[i + 1]];
    If[Simplify[f] =!= 0,
      result += D[Log[f], var] * (
        Hlog[arg, Delete[word, i + 1]] - Hlog[arg, Delete[word, i]]
      )
    ],
    {i, n - 1}
  ];
  
  (*Contribution from last letter*)
  If[word[[n]] =!= 0,
    result -= D[Log[word[[n]]], var] * 
              If[n === 1, 1, Hlog[arg, Most[word]]]
  ];
  
  (*Contribution from first letter (if arg \[NotEqual] \[Infinity])*)
  If[arg =!= Infinity,
    result -= D[word[[1]], var]/(arg - word[[1]]) * 
              If[n === 1, 1, Hlog[arg, Rest[word]]]
  ];
  
  result /. Hlog[_, {}] -> 1
]

(*Core derivative computation for Mpl*)
DiffMpl[ns_List, zs_List, var_] := Module[
  {result = 0, j, dzj, sub, n = Length[ns]},
  
  Do[
    dzj = D[zs[[j]], var];
    If[dzj === 0, Continue[]];
    
    sub = Which[
      ns[[j]] > 1,
        Mpl[ReplacePart[ns, j -> ns[[j]] - 1], zs] / zs[[j]],
      
      !IntegerQ[ns[[j]]] || ns[[j]] < 1,
        Message[DiffMpl::posint, ns[[j]]]; Abort[],
      
      ns === {1},
        1/(1 - zs[[1]]),
      
      j === n,  (*last index, n_j = 1*)
        Mpl[Most[ns], ReplacePart[Most[zs], -1 -> zs[[-2]]*zs[[-1]]]] / (1 - zs[[n]]),
      
      j === 1,  (*first index, n_1 = 1*)
        Mpl[Rest[ns], ReplacePart[Rest[zs], 1 -> zs[[1]]*zs[[2]]]] / (zs[[1]]*(zs[[1]] - 1)) - 
        Mpl[Rest[ns], Rest[zs]] / (zs[[1]] - 1),
      
      True,  (*middle index, n_j = 1*)
        Mpl[Delete[ns, j], ReplacePart[Delete[zs, j], j -> zs[[j]]*zs[[j+1]]]] / (zs[[j]]*(zs[[j]] - 1)) - 
        Mpl[Delete[ns, j], ReplacePart[Delete[zs, j], j-1 -> zs[[j-1]]*zs[[j]]]] / (zs[[j]] - 1)
    ];
    
    result += dzj * sub,
    {j, n}
  ];
  
  result
]

DiffMpl::posint = "Only Mpl with positive integer indices supported; got `1`.";

(*Master differentiation function that properly handles Hlog and Mpl*)
HyperD[expr_, var_] := Module[{},
  Which[
    FreeQ[expr, var], 0,
    
    Head[expr] === Plus,
      HyperD[#, var] & /@ expr,
    
    Head[expr] === Times,
      Sum[MapAt[HyperD[#, var] &, expr, j], {j, Length[expr]}],
    
    Head[expr] === Power && FreeQ[expr[[2]], var],
      expr[[2]] * expr[[1]]^(expr[[2]] - 1) * HyperD[expr[[1]], var],
    
    Head[expr] === Power,
      expr * (HyperD[expr[[2]], var] * Log[expr[[1]]] + 
              expr[[2]] * HyperD[expr[[1]], var] / expr[[1]]),
    
    Head[expr] === Hlog,
      DiffHlog[expr[[1]], expr[[2]], var],
    
    Head[expr] === Mpl,
      DiffMpl[expr[[1]], expr[[2]], var],
    
    Head[expr] === Log,
      HyperD[expr[[1]], var] / expr[[1]],
    
    Head[expr] === Exp,
      expr * HyperD[expr[[1]], var],
    
    True,
      D[expr, var]
  ]
]

(*Also define rules to catch Derivative forms that D might produce*)
Derivative[1, 0][Hlog][arg_, word_] := 
  DiffHlog[arg, word, arg] /. D[arg, arg] -> 1

(*For Mpl, handle derivative w.r.t. each slot*)
Unprotect[Derivative];
Derivative[derivNs_List, derivZs_List][Mpl][ns_List, zs_List] /; 
  Total[derivNs] === 0 && Total[derivZs] > 0 := Module[
  {result = 0, j, mult},
  Do[
    mult = derivZs[[j]];
    If[mult > 0,
      result += mult * (DiffMpl[ns, zs, zs[[j]]] /. D[zs[[j]], zs[[j]]] -> 1)
    ],
    {j, Length[zs]}
  ];
  result
]
Protect[Derivative];


(*(*Derivative of Hlog*)
HlogDerivative[arg_, {}, var_] := 0

HlogDerivative[arg_, word_List, var_] := Module[
  {result, i, f, n = Length[word]},
  
  result = D[arg, var]/(arg - word[[1]]) * Hlog[arg, Drop[word, 1]];

  Do[
    f = word[[i]] - word[[i + 1]];
    If[Simplify[f] === 0, Continue[]];
    result += D[Log[f], var] * (
      Hlog[arg, Delete[word, i + 1]] - Hlog[arg, Delete[word, i]]
    ),
    {i, n - 1}
  ];
  
  If[word[[n]] =!= 0,
    result -= D[Log[word[[n]]], var] * Hlog[arg, Take[word, n - 1]]
  ];
  
  If[arg =!= Infinity,
    result -= D[word[[1]], var]/(arg - word[[1]]) * Hlog[arg, Drop[word, 1]]
  ];
  
  result /. Hlog[_, {}] -> 1
]

(*Derivative of Mpl*)
MplDerivative[ns_List, zs_List, var_] := Module[
  {result = 0, j, inner, sub, n = Length[ns]},
  
  Do[
    inner = D[zs[[j]], var];
    If[inner === 0, Continue[]];
    
    sub = Which[
      ns[[j]] > 1,
        Mpl[ReplacePart[ns, j -> ns[[j]] - 1], zs] / zs[[j]],
      
      !IntegerQ[ns[[j]]] || ns[[j]] < 1,
        Message[MplDerivative::posint]; Return[$Failed],
      
      ns === {1},
        1/(1 - zs[[1]]),
      
      j === n,
        Mpl[Delete[ns, j], 
            ReplacePart[Delete[zs, j], (j-1) -> zs[[j-1]] * zs[[j]]]] / (1 - zs[[j]]),
      
      j === 1,
        Mpl[Delete[ns, 1], ReplacePart[Delete[zs, 1], 1 -> zs[[1]]*zs[[2]]]] / 
          (zs[[1]]*(zs[[1]] - 1)) - 
        Mpl[Delete[ns, 1], Delete[zs, 1]] / (zs[[1]] - 1),
      
      True, 
        Mpl[Delete[ns, j], ReplacePart[Delete[zs, j], j -> zs[[j]]*zs[[j+1]]]] / 
          (zs[[j]]*(zs[[j]] - 1)) - 
        Mpl[Delete[ns, j], ReplacePart[Delete[zs, j], (j-1) -> zs[[j-1]]*zs[[j]]]] / 
          (zs[[j]] - 1)
    ];
    
    result += inner * sub,
    {j, n}
  ];
  result
]

MplDerivative::posint = "Only Mpl with all positive integer indices supported.";

Hlog /: D[Hlog[arg_, word_List], var_] := HlogDerivative[arg, word, var]
Mpl /: D[Mpl[ns_List, zs_List], var_] := MplDerivative[ns, zs, var]*)


(* ::Section:: *)
(*Series expansion of Hlog[] and Mpl[]*)


(*Partial sum for MPL defining series (...)*)
MplSum[{}, _, _] := 1

MplSum[ns_List, zs_List, maxN_Integer] /; maxN >= 1 := Module[
  {n = Length[ns], subns, subzs},
  subns = Most[ns];
  subzs = Most[zs];
  Sum[MplSum[subns, subzs, k - 1] * zs[[n]]^k / k^ns[[n]], {k, 1, maxN}]
]

MplSum[_, _, maxN_Integer] /; maxN < 1 := 0

(*main series expansion of Hlog using ZeroExpansion*)
HlogZeroExpand[arg_, word_List, order_Integer] := Module[
  {expa, result = 0, n, m, coef},
  
  If[word === {}, Return[1]];
  
  expa = ZeroExpansion[word, order];
  If[expa === {} || Length[expa] === 0, Return[0]];
  
  (*Build: \[CapitalSigma]_{n,m} a_{n,m} * Log[arg]^(n-1)/(n-1)! * arg^(m-1)*)
  Do[
    If[n <= Length[expa] && Length[expa[[n]]] > 0,
      Do[
        coef = expa[[n, m]];
        If[coef =!= 0,
          result += coef * Log[arg]^(n - 1) / Factorial[n - 1] * arg^(m - 1)
        ],
        {m, Length[expa[[n]]]}
      ]
    ],
    {n, Length[expa]}
  ];
  
  result
]

(*Taylor expansion for Hlog/Mpl when argument depends parametrically on var*)
TaylorExpandHyper[expr_, var_, order_Integer] := Module[
  {result, k, deriv, derivs},
  
  (*Compute derivatives up to required order*)
  derivs = NestList[HyperD[#, var] &, expr, order];
  (*Build Taylor series: f(0) + var*f'(0) + var^2/2!*f''(0) + ...*)
  result = Sum[
    (derivs[[k + 1]] /. var -> 0) * var^k / k!,
    {k, 0, order}
  ];
  
  result
]

(*Standalone HlogSeries for internal use.*)
HlogSeries[arg_, word_List, var_, order_Integer] := Module[
  {argLim, expanded},
  
  (*If var doesn't appear, return unevaluated*)
  If[FreeQ[{arg, word}, var],
    Return[Hlog[arg, word] + O[var]^(order + 1)]
  ];
  
  (*Check if argument approaches 0*)
  argLim = Quiet[Limit[arg, var -> 0]];
  
  If[argLim === 0,
    (*Use ZeroExpansion*)
    expanded = HlogZeroExpand[arg, word, order];
    Return[Series[expanded, {var, 0, order}]]
  ];
  
  (*Otherwise use Taylor expansion*)
  Series[TaylorExpandHyper[Hlog[arg, word], var, order], {var, 0, order}]
]

HlogSeries[Hlog[arg_, word_List], var_, order_Integer] := 
  HlogSeries[arg, word, var, order]

(*Standalone MplSeries for internal use.*)
MplSeries::lastarg = "Last Mpl argument does not approach 0; using Taylor expansion.";
MplSeries::lastarg1 = "Depth > 1 Mpl with last argument -> 1 and first argument fixed has a log-series singularity at var = 0; returning unevaluated. Full log-series expansion is not yet implemented.";

MplSeries[ns_List, zs_List, var_, order_Integer] := Module[
  {lastZ, lim, firstLim, j, partialSum},

  (*If var does not appear in zs, return unchanged.*)
  If[FreeQ[zs, var],
    Return[Mpl[ns, zs] + O[var]^(order + 1)]
  ];

  (*Check that last argument goes to zero as var -> 0*)
  lastZ = zs[[-1]];
  lim = Quiet[Limit[lastZ, var -> 0]];

  If[lim === 0,
    (*Use MplSum*)
    partialSum = MplSum[ns, zs, order];
    Return[Series[partialSum, {var, 0, order}]]
  ];

  (*Also use MplSum when the FIRST argument approaches 0.
    The defining sum has zs[[1]]^m1 in every term; when zs[[1]] -> 0,
    all terms with m1 > order contribute at O(var^(order+1)) or higher,
    so MplSum captures the full Taylor expansion correctly regardless
    of what zs[[-1]] does (e.g. 1-x/u -> 1 is fine).*)
  firstLim = Quiet[Limit[zs[[1]], var -> 0]];
  If[firstLim === 0,
    partialSum = MplSum[ns, zs, order];
    Return[Series[partialSum, {var, 0, order}]]
  ];

  (*For depth-1 Mpl[{n},{z}] = PolyLog[n,z]: TaylorExpandHyper can diverge at
    var=0 for any lim != 0 ; not only lim=1 (where 1/(1-z) poles appear) but
    also lim=Indeterminate/Infinity (e.g. z=1-1/var gives DiffMpl[{1},...] = 1/var).
    Since Mpl[{n},{z}] = PolyLog[n,z] exactly, always delegate to Mathematica's
    built-in Series for depth-1 when lim != 0.*)
  If[Length[ns] === 1,
    Return[Series[PolyLog[ns[[1]], zs[[1]]], {var, 0, order}]]
  ];

  (*Depth > 1 with last arg -> 1 and first arg not -> 0:
    The function has a log-series singularity at var=0; TaylorExpandHyper would
    produce 1/0 errors. Return unevaluated instead of crashing.*)
  If[lim === 1,
    If[$HyperVerbosity > 0, Message[MplSeries::lastarg1]];
    Return[Mpl[ns, zs] + O[var]^(order + 1)]
  ];

  (*Otherwise use Taylor expansion*)
  If[$HyperVerbosity > 1, Message[MplSeries::lastarg]];
  Series[TaylorExpandHyper[Mpl[ns, zs], var, order], {var, 0, order}]
]

MplSeries[Mpl[ns_List, zs_List], var_, order_Integer] := 
  MplSeries[ns, zs, var, order]

HyperSeries::info = "Using Taylor expansion for `1` since argument doesn't approach special value.";

(*Main entry point*)
HyperSeries[expr_, {var_, point_, order_Integer}] := Module[
  {shifted, result},

  If[point === 0,
    Quiet[HyperSeriesInternal[expr, var, order], {Power::infy, Infinity::indet}],
    (*Shift to expand around 0*)
    shifted = expr /. var -> (var + point);
    result = Quiet[HyperSeriesInternal[shifted, var, order], {Power::infy, Infinity::indet}];
    result /. var -> (var - point)
  ]
]

(*Internal implementation*)
HyperSeriesInternal[expr_, var_, order_Integer] := Module[
  {processed, terms, collected},
  
  (*First, expand the expression to expose all terms*)
  processed = Expand[expr];
  
  (*Process each term if it's a sum*)
  If[Head[processed] === Plus,
    terms = List @@ processed;
    collected = Total[HyperSeriesTerm[#, var, order] & /@ terms];
    Return[collected + O[var]^(order + 1)]
  ];
  
  (*Single term*)
  HyperSeriesTerm[processed, var, order] + O[var]^(order + 1)
]

(*Process a single term (product of factors).*)
HyperSeriesTerm[term_, var_, order_Integer] := Module[
  {factors, hyperFactors, otherFactors, hyperSeries, otherSeries, result},
  
  (*If no var dependence, return as-is*)
  If[FreeQ[term, var], Return[term]];
  
  (*Split into Hlog/Mpl factors and others*)
  If[Head[term] === Times,
    factors = List @@ term;
    {hyperFactors, otherFactors} = {
      Select[factors, MatchQ[#, _Hlog | _Mpl] && !FreeQ[#, var] &],
      Select[factors, !(MatchQ[#, _Hlog | _Mpl] && !FreeQ[#, var]) &]
    };
    
    (*Expand Hlog/Mpl factors*)
    hyperSeries = If[hyperFactors === {},
      1,
      Times @@ (HyperSeriesExpandOne[#, var, order] & /@ hyperFactors)
    ];
    
    (*Expand other factors using standard Series*)
    otherSeries = If[otherFactors === {},
      1,
      Normal[Series[Times @@ otherFactors, {var, 0, order}]]
    ];
    
    (*Multiply and truncate*)
    result = Expand[hyperSeries * otherSeries];
    Return[SeriesTruncate[result, var, order]]
  ];
  
  (*Single factor*)
  If[MatchQ[term, _Hlog | _Mpl],
    Return[HyperSeriesExpandOne[term, var, order]]
  ];
  
  (*Default: use standard Series*)
  Normal[Series[term, {var, 0, order}]]
]

(*Expand a single Hlog or Mpl*)
HyperSeriesExpandOne[expr_, var_, order_Integer] := Module[
  {arg, word, ns, zs, argLim, lastZLim, firstZLim},
  
  Which[
    Head[expr] === Hlog,
    arg = expr[[1]];
    word = expr[[2]];
    
    (*Check if argument approaches 0*)
    argLim = Quiet[Limit[arg, var -> 0]];
    If[argLim === 0,
      (*Use ZeroExpansion approach*)
      Return[HlogZeroExpand[arg, word, order]]
    ];
    
    (*Otherwise Taylor expand*)
    If[$HyperVerbosity > 1, 
      Message[HyperSeries::info, expr]
    ];
    TaylorExpandHyper[expr, var, order],
    
    Head[expr] === Mpl,
    ns = expr[[1]];
    zs = expr[[2]];
    
    (*Check if last argument approaches 0*)
    lastZLim = Quiet[Limit[zs[[-1]], var -> 0]];
    If[lastZLim === 0,
      (*Use MplSum approach*)
      Return[MplSum[ns, zs, order]]
    ];

    (*Also use MplSum when the first argument approaches 0; see MplSeries.*)
    firstZLim = Quiet[Limit[zs[[1]], var -> 0]];
    If[firstZLim === 0,
      Return[Normal[Series[MplSum[ns, zs, order], {var, 0, order}]]]
    ];

    (*For depth-1 Mpl[{n},{z}] = PolyLog[n,z]: TaylorExpandHyper can diverge
      for any lim != 0 (lim=1 gives 1/(1-z) poles; lim=Indeterminate/Infinity
      also fails via the derivative chain). Always use built-in Series.*)
    If[Length[ns] === 1,
      Return[Normal[Series[PolyLog[ns[[1]], zs[[1]]], {var, 0, order}]]]
    ];

    (*Depth > 1, last arg -> 1, first arg not -> 0: log-series singularity,
      TaylorExpandHyper would produce 1/0 errors. Return unevaluated.*)
    If[lastZLim === 1,
      If[$HyperVerbosity > 0, Message[MplSeries::lastarg1]];
      Return[expr]
    ];

    (*Otherwise Taylor expand*)
    If[$HyperVerbosity > 1,
      Message[HyperSeries::info, expr]
    ];
    TaylorExpandHyper[expr, var, order],

    True,
    expr
  ]
]

(*Truncate polynomial to given order*)
SeriesTruncate[poly_, var_, order_Integer] := Module[
  {expanded, minDeg, maxDeg},
  expanded = Expand[poly];
  If[FreeQ[expanded, var], Return[expanded]];
  minDeg = Exponent[expanded, var, Min];
  maxDeg = Min[Exponent[expanded, var, Max], order];
  Sum[Coefficient[expanded, var, k] * var^k, {k, minDeg, maxDeg}]
]



(* ::Section:: *)
(*Integration Routines*)


(*A function to find primitives to pass to IntegrationStep[]*)
IntegrateII[wordlist_List, var_] := Module[
  {
  result = <||>,
   queue,
   (*result: Accumulates the primitive as word \[RightArrow] coefficient pairs*)
   (*queue:  Terms awaiting processing. Integration by parts generates*)
   (*new terms that get appended here for recursive processing.*)
    w, parfr, p, i, n, v, m},
  queue = wordlist;
  (*1. Main processing loop for each term {coef, word}:*)
  (*Process each term {coef, word} from the queue*)
  (*We want to find \[Integral]_{0}^{var} coef(t) \[CenterDot] Hlog(t, word) dt*)
  While[Length[queue] > 0, w = First[queue]; queue = Rest[queue];
  (*2. Partial fractions decomposition: split coef into*)
  (*polynomial + Subscript[\[CapitalSigma], i] \[CapitalSigma]\:2c7c Subscript[c, ij]/(var - pole\:1d62)^j*)
  (*Format: {poly, {pole1, c11, c12, ...}, {pole2, c21, c22, ...}, ...}*)
  (*where Subscript[c, ij] is coefficient of 1/(var - pole\:1d62)^j*)
    parfr = PartialFractions[w[[1]], var]; If[parfr === $Failed, Return[$Failed]];
    (*3. Handle polynomial part:*)
    If[Length[parfr] > 0 && parfr[[1]] =!= 0,
     (*a: Direct integration of polynomial part.
       parfr[[1]] is guaranteed polynomial in var by construction
       (PolynomialQuotient, see PartialFractions). Calling Integrate[]
       on it leaked Wolfram's internal Integrate`V[i][j] placeholders
       when the coefficients contained symbolic W-letters; computing
       the antiderivative term-by-term is exact and avoids the leak. *)
      p = With[{cs = CoefficientList[parfr[[1]], var]},
        Sum[cs[[k]] var^k / k, {k, 1, Length[cs]}]];
      result[w[[2]]] = Lookup[result, Key[w[[2]]], 0] + p;
      (*b: Integration by parts contribution*)
      If[Length[w[[2]]] > 0, AppendTo[queue, {-p / (var - w[[2, 1]]), Rest[w[[2]]]}]]];
(*comments:
   
   We want to compute: \[Integral]_{0}^{var} poly(t) \[CenterDot] Hlog(t, word) dt
   where poly = parfr[[1]] is the polynomial part from partial fractions.
   
   -IBP:
   Let P(t) = \[Integral]_{0}^{t} poly(s) ds, so P'(t) = poly(t) and P(0) = 0.
   Let H(t) = Hlog(t, {Subscript[w, 1], Subscript[w, 2], ...})
   Then: \[Integral]_{0}^{var} P'(t)\[CenterDot]H(t) dt = [P(t)\[CenterDot]H(t)]|_{0}^{var} - \[Integral]_{0}^{var} P(t)\[CenterDot]H'(t) dt
   STEP a: Boundary term
       [P(t)\[CenterDot]H(t)]|_{0}^{var} = P(var)\[CenterDot]H(var) - P(0)\[CenterDot]H(0)
                        = P(var)\[CenterDot]Hlog(var, word) - 0
       \[RightArrow] Add P(var) to result[word]
       Note: The Hlog factor is implicit in the word index!
       result[word] accumulates the coefficient of Hlog(var, word).
   STEP b: Integration by parts correction term
       Using: d/dt Hlog(t; Subscript[w, 1],Subscript[w, 2], ...) = 1/(t - Subscript[w, 1]) \[CenterDot] Hlog(t; Subscript[w, 2], ...)
       The second term becomes:
       -\[Integral]_{0}^{var} P(t)/(t -Subscript[w, 1]) \[CenterDot] Hlog(t; Subscript[w, 2], ...) dt
       
       This is a NEW integral that must be processed!
       \[RightArrow] Append {-P/(var - Subscript[w, 1]), Rest[word]} to the queue
       
       The recursion continues until all polynomial parts are eliminated
       (leaving only simple poles, which are handled in the next step).
*)
	(*4. Handling the pole term:*)
    Do[m = Length[parfr[[i]]] - 1;
      Do[If[parfr[[i, n + 1]] === 0, Continue[]];
        If[n > 1, p = parfr[[i, n + 1]] / ((var - parfr[[i, 1]])^(n - 1) * (1 - n));
          result[w[[2]]] = Lookup[result, Key[w[[2]]], 0] + p;
          If[Length[w[[2]]] > 0, AppendTo[queue, {-p / (var - w[[2, 1]]), Rest[w[[2]]]}]]];
        If[n === 1, v = Prepend[w[[2]], parfr[[i, 1]]];
          result[v] = Lookup[result, Key[v], 0] + parfr[[i, n + 1]]], 
        {n, 1, m}], {i, 2, Length[parfr]}]];
  (*Comments:
   
   For each pole location a = parfr[[i,1]] with coefficients for powers 1,2,...,m
   
   CASE A: Higher-order poles (n > 1)
       \[Integral] c/(z-a)^n dz = c/((1-n)(z-a)^(n-1))
       
       This gives a rational function contribution to the primitive,
       plus an integration-by-parts term for the queue.
   
   CASE B: Simple poles (n = 1)
       
       \[Integral]_{0}^{z} c/(t-a) \[CenterDot] Hlog(t; Subscript[w, 2],...) dt = c \[CenterDot] Hlog(z; a, Subscript[w, 2],...)
       
       The pole location 'a' becomes a NEW LETTER prepended to the word.
       No integration by parts needed here; the integral is exact.
    *)
  
  Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &]
  ]


(*(*Integration step using contour deformation:*)
SelectRemove[list_List, pred_] := {Select[list, pred], Select[list, Not@*pred]}

IntegrationStep[wordlist_List, var_] := MaybeQuiet[Module[
  {startTime, poles, pair, sub, w, reginfs, primitive, minOrder, zeroExp, poly,
   logpower, power, coef, temp, q, L, vv, finalResult, allone, part, t, 
   transformResult, reginfsCoef,
   positiveLetters, keysToProcess, wordsWithPos, wordsWithoutPos, onAxis, 
   newKey, word, origCoef, processedKey, failed = False,
   series},
   
  startTime = AbsoluteTime[];
  If[$HyperVerbosity > 0, 
    Print["Integrating variable ", var, " from 0 to Infinity, integrand has ", 
      Length[wordlist], " terms"]];
  $LinearFactorsCache = <||>; ForgetExpansions[];
  poles = <||>;
  
  Do[
    If[failed, Break[]];
    
    transformResult = TransformShuffle[pair[[2]], var];
    
    If[transformResult === $Failed,
      If[$HyperVerbosity > 0,
        Print["IntegrationStep: TransformShuffle returned $Failed"]];
      failed = True;
      Break[]
    ];
    
    Do[
      primitive = IntegrateII[ScalarMul[sub[[1]], pair[[1]]], var];
      If[primitive === $Failed, failed = True; Break[]];
      reginfs = sub[[2]];
      
      Do[
        (*Zero expansion*)
        gPrint["zero expansion"];
        series=Series[w[[1]], var->0]//Normal;
        minOrder=-Exponent[series,var,Min];
        If[minOrder >= 0,
          
          zeroExp = ZeroExpansion[w[[2]], minOrder];
          If[Length[zeroExp] > 0,
            Do[If[logpower + 1 <= Length[zeroExp] && 
                  Length[zeroExp[[logpower + 1]]] > 0,
              poly = Sum[zeroExp[[logpower + 1, k]] * var^(k - 1), 
                {k, 1, Length[zeroExp[[logpower + 1]]]}] *
                series;
              Do[coef = -Coefficient[poly, var, -power];
                If[coef =!= 0,
                  Do[
              
                    L = NormalizeShuffleKey[vv[[2]]];
                    
                    reginfsCoef = vv[[1]];
                    poles[{logpower, power, L}] =
                      Lookup[poles, Key[{logpower, power, L}], 0] + Cancel[coef * reginfsCoef],
                    {vv, reginfs}]];
                If[!$HyperInticaCheckDivergences, Break[]], 
                {power, 0, minOrder}]];
              If[!$HyperInticaCheckDivergences, Break[]], 
              {logpower, 0, Length[zeroExp] - 1}]]];
        
        (*Infinity expansion*)
        gPrint["infinity expansion"];
        poly = w[[1]] /. var -> 1/var;
        series=Series[poly, var->0]//Normal;
        minOrder=-Exponent[series,var,Min];
        (*minOrder = -PoleDegree[poly, var];*)
        If[minOrder >= 0,
          If[minOrder === 0 && !$HyperInticaCheckDivergences,
            If[!(w[[2]] === Table[-1, {Length[w[[2]]]}] && Length[w[[2]]] > 0),
              temp = RegzeroWord[w[[2]]];
              If[Length[temp] > 0, 
                coef = Together[poly /. var -> 0];
                Do[Do[
                  L = NormalizeShuffleKey[CombineShuffleKeys[q[[2]], t[[2]]]];
                  reginfsCoef = q[[1]] * t[[1]];
                  poles[{0, 0, L}] =
                    Lookup[poles, Key[{0, 0, L}], 0] + Cancel[coef * reginfsCoef],
                  {q, reginfs}], {t, temp}]
                  ]],
            (*poly = SeriesExpansion[poly, var, minOrder];*)
            poly=series;
            allone = True;
            Do[
              If[i < Length[w[[2]]], 
                allone = allone && (w[[2, i + 1]] === -1); 
                If[allone, Continue[]]];
              part = InfExpansion[Take[w[[2]], i], minOrder];
              temp = RegzeroWord[Drop[w[[2]], i]];
              If[Length[temp] > 0 && Length[part] > 0,
                Do[If[logpower + 1 <= Length[part] && 
                      Length[part[[logpower + 1]]] > 0,
                  Do[coef = Coefficient[
                    Sum[part[[logpower + 1, k]] * var^(k - 1), 
                      {k, 1, Length[part[[logpower + 1]]]}] * poly, var, -power];
                    If[coef =!= 0,
                    Do[Do[
                    L = NormalizeShuffleKey[CombineShuffleKeys[q[[2]], t[[2]]]];
                    reginfsCoef = q[[1]] * t[[1]];
                    poles[{logpower, power, L}] =
                    Lookup[poles, Key[{logpower, power, L}], 0] +
                    Cancel[coef * reginfsCoef],
                    {q, reginfs}], {t, temp}]
                        ];
                    If[!$HyperInticaCheckDivergences, Break[]], 
                    {power, 0, minOrder}]];
                  If[!$HyperInticaCheckDivergences, Break[]], 
                  {logpower, 0, Length[part] - 1}]],
              {i, Length[w[[2]]], 0, -1}]]],
        {w, primitive}],
      {sub, transformResult}],
    {pair, wordlist}];
  
  If[failed,
    Return[$Failed]
  ];
  
  If[$HyperVerbosity > 0,
    If[$HyperInticaCheckDivergences,
      Print["checking divergences after integration of ", var],
      Print["Warning: not checking divergences"]]];
  
  positiveLetters = {};
  gPrint["continuation"];
  
  (*Analytic continuation for positive letters*)
  Do[
    keysToProcess = Select[Keys[poles], 
      MatchQ[#, {logpower, power, _}] && #[[3]] =!= {} &];
    Do[
      (*Use SelectRemove to split words with/without positive letters*)
      {wordsWithPos, wordsWithoutPos} = 
        SelectRemove[processedKey[[3]], 
          Function[wrd, Or @@ (TrueQ[# > 0] & /@ wrd)]];
      If[wordsWithPos =!= {},
        If[Length[wordsWithPos] > 1,
          Continue[]];
        word = wordsWithPos[[1]];
        origCoef = poles[processedKey];
        poles[processedKey] = 0;
        onAxis = Select[Union[word], TrueQ[# > 0] &];
        positiveLetters = Union[positiveLetters, onAxis];
        onAxis = Map[{#, delta[var]} &, Sort[onAxis]];
        Do[
          newKey = Sort[Join[wordsWithoutPos, temp[[2]]]];
          poles[{logpower, power, newKey}] =
            Lookup[poles, Key[{logpower, power, newKey}], 0] +
            Cancel[temp[[1]] * origCoef],
          {temp, BreakUpContour[{{1, word}}, onAxis]}]],
      {processedKey, keysToProcess}],
    {logpower, 0, 10}, {power, 0, 10}];
  
  If[positiveLetters =!= {} && $HyperVerbosity > 0,
    Print["Warning: Contour was deformed to avoid potential singularities at ", 
      positiveLetters]];
  
  (*Collect the finite part (logpower=0, power=0)*)
  finalResult = {};
  keysToProcess = Select[Keys[poles], MatchQ[#, {0, 0, _}] &];
  Do[
    coef = QuickCancel[poles[processedKey]];
    If[coef =!= 0,
      L = processedKey[[3]];
      AppendTo[finalResult, {coef, L}]],
    {processedKey, keysToProcess}];
  poles =.; keysToProcess =.; (* free the poles association ; no longer needed after finalResult is assembled *)

  If[$HyperVerbosity > 0,
    Print["finished integration of ", var, " in ",
      AbsoluteTime[] - startTime, " s, ", Length[finalResult], " terms"]];
  
  finalResult
]]*)


(*Integration step using contour deformation:*)
SelectRemove[list_List, pred_] := {Select[list, pred], Select[list, Not@*pred]}

IntegrationStep[wordlist_List, var_] := MaybeQuiet[Module[
  {startTime, polesZero, polesInf, pair, sub, w, reginfs, primitive, minOrder, zeroExp, poly,
   logpower, power, coef, temp, q, L, vv, finalResult, allone, part, t,
   transformResult, reginfsCoef,
   positiveLetters, keysToProcess, wordsWithPos, wordsWithoutPos, onAxis,
   newKey, word, origCoef, processedKey, failed = False, check, keysZero, keysInf},

  startTime = AbsoluteTime[];
  If[$HyperVerbosity > 0,
    Print["Integrating variable ", var, " from 0 to Infinity, integrand has ",
      Length[wordlist], " terms"]];
  $LinearFactorsCache = <||>; ForgetExpansions[];
  polesZero = <||>; polesInf = <||>;
  
  Do[
    If[failed, Break[]];
    
    transformResult = TransformShuffle[pair[[2]], var];
    
    If[transformResult === $Failed,
      If[$HyperVerbosity > 0,
        Print["IntegrationStep: TransformShuffle returned $Failed"]];
      failed = True;
      Break[]
    ];
    
    Do[
      primitive = IntegrateII[ScalarMul[sub[[1]], pair[[1]]], var];
      If[primitive === $Failed, failed = True; Break[]];
      reginfs = sub[[2]];
      
      Do[
        (*Zero expansion*)
        minOrder = -PoleDegree[w[[1]], var];
        If[minOrder >= 0,
          zeroExp = ZeroExpansion[w[[2]], minOrder];
          If[Length[zeroExp] > 0,
            Do[If[logpower + 1 <= Length[zeroExp] && 
                  Length[zeroExp[[logpower + 1]]] > 0,
              poly = Sum[zeroExp[[logpower + 1, k]] * var^(k - 1), 
                {k, 1, Length[zeroExp[[logpower + 1]]]}] *
                SeriesExpansion[w[[1]], var, (*minOrder*)0];
              Do[coef = -Coefficient[poly, var, -power];
                If[coef =!= 0,
                  Do[
                    L = NormalizeShuffleKey[vv[[2]]];
                    reginfsCoef = vv[[1]];
                    polesZero[{logpower, power, L}] =
                      Lookup[polesZero, Key[{logpower, power, L}], 0] + (*Cancel[*)coef * reginfsCoef(*]*),
                    {vv, reginfs}]];
                If[!$HyperInticaCheckDivergences, Break[]], 
                {power, 0, minOrder}]];
              If[!$HyperInticaCheckDivergences, Break[]], 
              {logpower, 0, Length[zeroExp] - 1}]]];
        
        (*Infinity expansion*)
        poly = w[[1]] /. var -> 1/var;
        minOrder = -PoleDegree[poly, var];
        If[minOrder >= 0,
          If[minOrder === 0 && !$HyperInticaCheckDivergences,
            If[!(w[[2]] === Table[-1, {Length[w[[2]]]}] && Length[w[[2]]] > 0),
              temp = RegzeroWord[w[[2]]];
              If[Length[temp] > 0, 
                coef = Together[poly] /. var -> 0;
                Do[Do[
                  L = NormalizeShuffleKey[CombineShuffleKeys[q[[2]], t[[2]]]];
                  reginfsCoef = q[[1]] * t[[1]];
                  polesInf[{0, 0, L}] =
                    Lookup[polesInf, Key[{0, 0, L}], 0] + (*Cancel[*)coef * reginfsCoef(*]*),
                  {q, reginfs}], {t, temp}]
                  ]],
            poly = SeriesExpansion[poly, var, minOrder];
            allone = True;
            Do[
              If[i < Length[w[[2]]], 
                allone = allone && (w[[2, i + 1]] === -1); 
                If[allone, Continue[]]];
              part = InfExpansion[Take[w[[2]], i], minOrder];
              temp = RegzeroWord[Drop[w[[2]], i]];
              If[Length[temp] > 0 && Length[part] > 0,
                Do[If[logpower + 1 <= Length[part] && 
                      Length[part[[logpower + 1]]] > 0,
                  Do[coef = Coefficient[
                    Sum[part[[logpower + 1, k]] * var^(k - 1), 
                      {k, 1, Length[part[[logpower + 1]]]}] * poly, var, -power];
                    If[coef =!= 0,
                    Do[Do[
                    L = NormalizeShuffleKey[CombineShuffleKeys[q[[2]], t[[2]]]];
                    reginfsCoef = q[[1]] * t[[1]];
                    polesInf[{logpower, power, L}] =
                    Lookup[polesInf, Key[{logpower, power, L}], 0] +
                    (*Cancel[*)coef * reginfsCoef(*]*),
                    {q, reginfs}], {t, temp}]
                        ];
                    If[!$HyperInticaCheckDivergences, Break[]], 
                    {power, 0, minOrder}]];
                  If[!$HyperInticaCheckDivergences, Break[]], 
                  {logpower, 0, Length[part] - 1}]],
              {i, Length[w[[2]]], 0, -1}]]],
        {w, primitive}],
      {sub, transformResult}],
    {pair, wordlist}];
  
  If[failed,
    Return[$Failed]
  ];
  
  If[$HyperVerbosity > 0,
    If[$HyperInticaCheckDivergences,
      Print["checking divergences after integration of ", var],
      Print["Warning: not checking divergences"]]];
  
  positiveLetters = {};

  (*Analytic continuation for positive letters \[LongDash] operates on polesInf only, mirroring Maple*)
  Do[
    keysToProcess = Select[Keys[polesInf],
      MatchQ[#, {logpower, power, _}] && #[[3]] =!= {} &];
    Do[
      (*Use SelectRemove to split words with/without positive letters*)
      {wordsWithPos, wordsWithoutPos} =
        SelectRemove[processedKey[[3]],
          Function[wrd, Or @@ (TrueQ[# > 0] & /@ wrd)]];
      If[wordsWithPos =!= {},
        If[Length[wordsWithPos] > 1,
          Continue[]];
        word = wordsWithPos[[1]];
        origCoef = polesInf[processedKey];
        polesInf[processedKey] = 0;
        onAxis = Select[Union[word], TrueQ[# > 0] &];
        positiveLetters = Union[positiveLetters, onAxis];
        onAxis = Map[{#, delta[var]} &, Sort[onAxis]];
        Do[
          newKey = Sort[Join[wordsWithoutPos, temp[[2]]]];
          polesInf[{logpower, power, newKey}] =
            Lookup[polesInf, Key[{logpower, power, newKey}], 0] +
            Cancel[temp[[1]] * origCoef],
          {temp, BreakUpContour[{{1, word}}, onAxis]}]],
      {processedKey, keysToProcess}],
    {logpower, 0, 10}, {power, 0, 10}];

  If[positiveLetters =!= {} && $HyperVerbosity > 0,
    Print["Warning: Contour was deformed to avoid potential singularities at ",
      positiveLetters]];

  (*Divergence check: verify pole terms cancel at each boundary separately*)
  If[$HyperInticaCheckDivergences,
    failed = Catch[
    Do[Do[
      If[logpower === 0 && power === 0, Continue[]];
      keysZero = Select[Keys[polesZero], MatchQ[#, {logpower, power, _}] &];
      If[Length[keysZero] > 0,
        check = TestZeroFunction[Map[{QuickCancel[polesZero[#]], #[[3]]} &, keysZero]];
        If[check =!= 0,
          $HyperInticaDivergences[{var, 0, Log[var]^logpower/var^power}] = check/logpower!;
          Message[IntegrationStep::divergence, var, 0, Log[var]^logpower/var^power];
          If[$HyperInticaAbortOnDivergence, Throw[$Failed, "HyperInticaDivergence"]]]];
      keysInf = Select[Keys[polesInf], MatchQ[#, {logpower, power, _}] &];
      If[Length[keysInf] > 0,
        check = TestZeroFunction[Map[{QuickCancel[polesInf[#]], #[[3]]} &, keysInf]];
        If[check =!= 0,
          $HyperInticaDivergences[{var, Infinity, Log[var]^logpower*var^power}] = check/logpower!;
          Message[IntegrationStep::divergence, var, Infinity, Log[var]^logpower*var^power];
          If[$HyperInticaAbortOnDivergence, Throw[$Failed, "HyperInticaDivergence"]]]],
      {power, 0, 10}],
    {logpower, 0, 10}],
    "HyperInticaDivergence"];
    If[failed === $Failed, Return[$Failed]]];

  (*Combine: add zero-boundary contributions into polesInf for the {0,0} keys, mirroring Maple*)
  Do[
    polesInf[k] = Lookup[polesInf, Key[k], 0] + polesZero[k],
    {k, Select[Keys[polesZero], MatchQ[#, {0, 0, _}] &]}];

  (*Collect the finite part (logpower=0, power=0)*)
  finalResult = {};
  keysToProcess = Select[Keys[polesInf], MatchQ[#, {0, 0, _}] &];
  Do[
    coef = QuickCancel[polesInf[processedKey]];
    If[coef =!= 0,
      L = processedKey[[3]];
      AppendTo[finalResult, {coef, L}]],
    {processedKey, keysToProcess}];
  polesZero =.; polesInf =.; keysToProcess =.; (* free memory *)

  If[$HyperVerbosity > 0,
    Print["finished integration of ", var, " in ",
      AbsoluteTime[] - startTime, " s, ", Length[finalResult], " terms"]];
  
  finalResult
]]


Options[HyperInt] = {"Monitor" -> False};

(*A function wrapping everything needed for integration + an option to monitor which integration variable is being processed:*)
HyperInt[f_, vars_, opts:OptionsPattern[]] := MaybeQuiet[Module[
  {F, i, var, a, b, varList, scaledVar, result, monitor},

  monitor = OptionValue["Monitor"];

  If[!ListQ[f],
    F = ConvertToHlogRegInf[f];
    If[F === $Failed, Return[$Failed]];
    Return[HyperInt[F, vars, opts]]
  ];

  If[!ListQ[vars], Return[HyperInt[f, {vars}, opts]]];

  F = f;
  varList = {};

  Do[
    Which[
      MatchQ[vars[[i]], _Symbol],
        AppendTo[varList, vars[[i]]],
      MatchQ[vars[[i]], _Rule],
        var = vars[[i, 1]];
        {a, b} = vars[[i, 2]];
        scaledVar = Unique["sv"];
        AppendTo[varList, scaledVar];
        Which[
          a === b, Return[{}],
          {a, b} === {-Infinity, Infinity},
            F = Join[F, (F /. var -> -scaledVar)],
          b === Infinity, F = F /. var -> a + scaledVar,
          b === -Infinity, F = ScalarMul[F /. var -> a - scaledVar, -1],
          a === Infinity, F = ScalarMul[F /. var -> b + scaledVar, -1],
          a === -Infinity, F = F /. var -> b - scaledVar,
          True, F = ScalarMul[
            F /. var -> (a + b * scaledVar)/(1 + scaledVar),
            (b - a)/(1 + scaledVar)^2]
        ],
      True,
        AppendTo[varList, vars[[i]]]
    ],
    {i, Length[vars]}
  ];

  Catch[
    Do[
      If[TrueQ[monitor],
        WriteString["stdout", "Processing variable " <> ToString[i] <> " of " <> ToString[Length[varList]] <> ": " <> ToString[varList[[i]]] <> "\n"]
      ];
      F = IntegrationStep[F, varList[[i]]];
      If[F === $Failed, Return[$Failed]],
      {i, Length[varList]}
    ],
    "HyperNonlinear",
    (F = $Failed)&
  ];
  If[F === $Failed, Return[$Failed]];
  
  If[$HyperEvaluatePeriods, 
    result = EvaluatePeriods[F]; 
    Return[result]
  ];
  
  F
]];

(* HyperIntica: user-facing wrapper around HyperInt.
   Accepts Mathematica Integrate-style syntax:
     HyperIntica[f, {x, 0, 1}, {y, 0, Infinity}, ...]
   as well as the original HyperInt syntax:
     HyperIntica[f, {x -> {0, 1}, y -> {0, Infinity}}]
     HyperIntica[f, {x, y}]
*)
Clear[HyperIntica];

(* Integrate-style: HyperIntica[f, {x,0,1}, {y,0,Infinity}, ...] *)
HyperIntica[f_, limits__List, opts:OptionsPattern[]] /;
    AllTrue[{limits}, MatchQ[#, {_Symbol, _, _}]&] :=
    HyperInt[f,
        (#[[1]] -> {#[[2]], #[[3]]})& /@ {limits},
        opts
    ];

(* Fallthrough: all other calls delegate directly to HyperInt *)
HyperIntica[args___] := HyperInt[args];


(* ::Section::Closed:: *)
(*Fibration Bases*)


(*fibrationBasis and dependencies*)
(*Global variables for singularity/restrictions*)
$HyperRestrictSingularities = False;
$HyperAllowedSingularities = {};

(*TestZeroFunction: check if a wordlist represents the zero function.
  Mirrors Maple's testZeroFunction. Returns 0 if zero, nonzero otherwise.*)
TestZeroFunction[wordlist_List] := Module[{vars, result},
  vars = DeleteDuplicates[
    Select[Variables[Flatten[Map[#[[2]] &, wordlist]]], MatchQ[#, _Symbol] &]];
  Block[{$HyperAlgebraicRoots = True, $HyperRestrictSingularities = False},
    result = Simplify[FibrationBasis[wordlist, vars]]];
  Simplify[result /. mzv[2] -> Pi^2/6]]

(* FibrationBasis*)
Options[FibrationBasis] = {
  "ProjectOnto" -> <||>
};

FibrationBasis[wordlist_, vars_List: {}, OptionsPattern[]] := 
 MaybeQuiet[Block[{$FibResult = <||>},
  Module[{projectOnto, internalWordlist},
   
   internalWordlist = 
    If[! ListQ[wordlist], ConvertToHlogRegInf[wordlist], wordlist];
   
   projectOnto = OptionValue["ProjectOnto"];
   If[! AssociationQ[projectOnto], projectOnto = <||>];
   
   FibrationBasisRecurse[internalWordlist, vars, projectOnto, {}, 1];
   
   (*Return result*)
   If[Length[vars] === 0,
    Total[Values[$FibResult]] /. {} -> 0,
    Total[KeyValueMap[
       Function[{key, val}, 
        val*Product[
          If[i > Length[key] || key[[i]] === {}, 1, 
           Hlog[vars[[i]], key[[i]]]], {i, Length[vars]}]], 
       $FibResult]] /. {} -> 0
    ]
   ]
  ]]

(*Replacm FibrationBasisRecurse with this debugged (?) version*)

FibrationBasisRecurse[wordlist_List, vars_List, projectOnto_Association,
   prefix_List, valueFactor_] := 
 Module[{val, leftoverVars, sizes, prefixes, counter, key, i, pair, w,
    u, shuffleKey, constContrib},
  
  If[Length[vars] === 0,
   (*Base case: no more variables to process*)
   leftoverVars = 
    DeleteDuplicates[
     Flatten[Map[Function[entry, Variables[Flatten[{entry[[2]]}]]], 
       wordlist]]];
   leftoverVars = Select[leftoverVars, MatchQ[#, _Symbol] &];
   If[Length[leftoverVars] > 0,
    Message[FibrationBasis::leftover, leftoverVars]];

   (*Evaluate to periods*)
   val = Total[Map[Function[w,
        shuffleKey = w[[2]];
        w[[1]]*Which[
          shuffleKey === {} || shuffleKey === {{}}, 1,
          ListQ[shuffleKey] && Length[shuffleKey] > 0 &&
           AllTrue[shuffleKey, ListQ],
          Product[If[u === {}, 1, ZeroInfPeriod[u]], {u, shuffleKey}],
          ListQ[shuffleKey], ZeroInfPeriod[shuffleKey],
          True, ZeroInfPeriod[shuffleKey]
        ]
        ], wordlist]]*valueFactor;

   prefixes = Length[prefix];

   If[prefixes === 0,
    $FibResult[{}] = Lookup[$FibResult, Key[{}], 0] + val;
    Return[]
   ];

   sizes = Map[Length, prefix];
   If[MemberQ[sizes, 0], Return[]];

   counter = ConstantArray[1, prefixes];

   While[True,
    key = Table[prefix[[i, counter[[i]], 2]], {i, prefixes}];
    constContrib = val*Product[prefix[[i, counter[[i]], 1]], {i, prefixes}];

    $FibResult[key] = Lookup[$FibResult, Key[key], 0] + constContrib;
    
    i = 1;
    While[i <= prefixes,
     If[counter[[i]] >= sizes[[i]],
      counter[[i]] = 1;
      i++,
      counter[[i]]++;
      Break[]
      ]
     ];
    If[i > prefixes, Break[]]
   ],
   
   (*Recursive case: process the first variable*)
   Do[
    $HyperRestrictSingularities = KeyExistsQ[projectOnto, vars[[1]]];
    $HyperAllowedSingularities = Lookup[projectOnto, vars[[1]], {}];

    Do[
     FibrationBasisRecurse[
      pair[[2]],
      Drop[vars, 1],
      projectOnto,
      Append[prefix, pair[[1]]],
      valueFactor*w[[1]]
      ],
     {pair, TransformShuffle[w[[2]], vars[[1]]]}
     ],
    {w, wordlist}
    ]
   ]
  ]

FibrationBasis::leftover = 
  "Not reduced to a basis (leftover variables: `1`); undetected relations may remain.";


(* ::Section:: *)
(*Breaking Contours*)


(*Break up contour for words with positive letters*)
BreakUpContour[wordList_List, onAxis_List] := Module[
  {smallest, imPart, newAxis, result, word, wlen, temp, i, 
   constPart, varPart, constVal, brk, b, t, L, bW, tW,
   processedWordList, tailWord, headWord, shiftedLetter},
  
  (*Start with the input wordList*)
  processedWordList = wordList;
  
  (*Base case: no positive letters on axis*)
  If[Length[onAxis] === 0,
    (*Separate words that can be evaluated as periods vs those that cannot*)
    {constPart, varPart} = {
      Select[processedWordList, SubsetQ[{-2, -1, 0}, Union[#[[2]]]] &],
      Select[processedWordList, ! SubsetQ[{-2, -1, 0}, Union[#[[2]]]] &]
    };
    constVal = Total[Map[#[[1]] * ZeroInfPeriodEval[#[[2]]] &, constPart]];
    varPart = Map[{#[[1]], If[#[[2]] === {}, {}, {#[[2]]}]} &, varPart];
    If[constVal =!= 0,
      Return[Join[{{constVal, {}}}, varPart]],
      Return[varPart]
    ]
  ];
  
  result = <||>;
  {smallest, imPart} = First[onAxis];
  newAxis = Map[{#[[1]] - smallest, #[[2]]} &, Rest[onAxis]];
  
  Do[
    wlen = Length[word[[2]]];
    
    Do[
      (*Tail part: letters from i+1 to end*)
      tailWord = If[i < wlen, word[[2, i + 1 ;; wlen]], {}];
      
      (*Process the tail*)
      If[TrueQ[Simplify[smallest == 1]] && 
         (tailWord === {} || AllTrue[tailWord, IntegerQ] || 
          AllTrue[Simplify /@ tailWord, NumericQ]),
        (*Can evaluate as a ZeroOnePeriod*)
        temp = {{ZeroOnePeriod[tailWord] //. mzvAllReductions (*// Simplify*), {}}},
        (*else: convert integral from 0 to smallest*)
        temp = ConvertABtoZeroInf[RegHead[{{1, tailWord}}, smallest], 0, smallest];
        temp = Map[{#[[1]], If[#[[2]] === {}, {}, {#[[2]]}]} &, temp]
      ];
      
      (*Head part: letters from 1 to i, shifted by -smallest*)
      headWord = Map[
        Function[letter,
          If[letter === 0 || NumericQ[letter],
            (*Literal number: just subtract smallest*)
            letter - smallest,
            (*Symbolic expression: use normal subtraction*)
            (*Together*)Cancel[letter - smallest]
          ]
        ],
        word[[2, 1 ;; i]]
      ];
      
      (*Apply regtail to the head part*)
      brk = BreakUpContour[
        RegTail[{{word[[1]], headWord}}, 0, Pi * I * imPart - Log[smallest]],
        newAxis
      ];
      
      (*Shuffle product of head and tail contributions*)
      Do[
        Do[
          (*Normalize shuffle keys*)
          bW = Which[
            b[[2]] === {}, {},
            ListQ[b[[2]]] && Length[b[[2]]] > 0 && ListQ[b[[2, 1]]], b[[2]],
            True, {b[[2]]}
          ];
          tW = Which[
            t[[2]] === {}, {},
            ListQ[t[[2]]] && Length[t[[2]]] > 0 && ListQ[t[[2, 1]]], t[[2]],
            True, {t[[2]]}
          ];
          L = Sort[Join[bW, tW]];
          result[L] = Lookup[result, Key[L], 0] + b[[1]] * t[[1]],
          {t, temp}
        ],
        {b, brk}
      ],
      {i, 0, wlen}
    ],
    {word, processedWordList}
  ];
  
  (*Clean up and return*)
  Select[
    KeyValueMap[{QuickCancel[#2] //. mzvAllReductions // Cancel, #1} &, result],
    #[[1]] =!= 0 &
  ]
]


(* ::Section:: *)
(*Handling Endpoints*)


ReglimWord[{}] := {{1, {}}}

ReglimWord[word_List, var_] := Module[
  {key, zeroOrders, minOrder, n, i, result, w, onAxis, scaled, origin, temp, z,
   positiveLetters, idx, nextCoef, imVal, reVal, numericIm, numericRe, finalOrigin,
   remainingVars, assumptions, testIm, testRe, signRe},
  
  key = {word, var};
  If[KeyExistsQ[$ReglimWordCache, key], Return[$ReglimWordCache[key]]];
  
  (* Word not depending on var *)
  If[FreeQ[word, var],
    If[Union[word] === {0} || Union[word] === {-1}, 
      $ReglimWordCache[key] = {}; Return[{}]];
    
    If[SubsetQ[{-1, 0}, Union[word]],
      Module[{evalResult},
        evalResult = BreakUpContour[{{1, word}}, {}];
        evalResult = Map[{#[[1]] //. mzvAllReductions (*// Simplify*), #[[2]]} &, evalResult];
        evalResult = Select[evalResult, #[[1]] =!= 0 &];
        $ReglimWordCache[key] = evalResult;
        Return[evalResult]
      ]
    ];
    
    If[SubsetQ[{-2, -1, 0}, Union[word]],
      $ReglimWordCache[key] = {{ZeroInfPeriod[word], {}}}; 
      Return[{{ZeroInfPeriod[word], {}}}]];
    
    $ReglimWordCache[key] = {{1, word}}; Return[{{1, word}}]
  ];
  
  zeroOrders = Map[PoleDegree[#, var] &, word];
  minOrder = Min[zeroOrders];
  result = <||>; 
  w = Length[word]; 
  n = 0;
  While[n < w && zeroOrders[[w - n]] > minOrder, n++];
  
  (* Handle trailing letters approaching zero *)
  If[n > 0,
    Do[
      temp = {};
      Do[temp = Join[temp, ScalarMul[ReglimWord[sc[[2]], var], sc[[1]]]],
        {sc, RegzeroWord[Join[Take[word, w - n], Table[0, {ii}]]]}];
      Do[
        Do[
          Module[{tW, rW, L, tCoef, rCoef},
            tW = If[t[[2]] === {}, {}, If[WordQ[t[[2]]], {t[[2]]}, t[[2]]]];
            rW = If[r[[2]] === {}, {}, If[WordQ[r[[2]]], {r[[2]]}, r[[2]]]];
            L = Sort[Join[tW, rW]];
            tCoef = t[[1]]; rCoef = r[[1]];
            result[L] = Lookup[result, Key[L], 0] + tCoef * rCoef
          ],
          {r, ReglimWord[word[[w - n + 1 + ii ;; w]], var]}
        ],
        {t, CollectWords[temp]}
      ],
      {ii, 0, n}
    ];
    $ReglimWordCache[key] = Select[KeyValueMap[{QuickCancel[#2], #1} &, result], #[[1]] =!= 0 &];
    Return[$ReglimWordCache[key]]
  ];
  
  (* Compute scaled word *)
  scaled = Table[
    If[zeroOrders[[i]] > minOrder, 
      0,
      If[FreeQ[word[[i]], var], word[[i]], RatResidue[word[[i]], var]]
    ], 
    {i, w}
  ];
  
  (* Check for positive letters *)
  positiveLetters = Select[Union[scaled], Positive];
  
  DebugPrint["[DEBUG-ReglimWord] ========================================"];
  DebugPrint["[DEBUG-ReglimWord] word = ", word];
  DebugPrint["[DEBUG-ReglimWord] var = ", var];
  DebugPrint["[DEBUG-ReglimWord] minOrder = ", minOrder];
  DebugPrint["[DEBUG-ReglimWord] scaled = ", scaled];
  DebugPrint["[DEBUG-ReglimWord] positiveLetters = ", positiveLetters];
  
  If[Length[positiveLetters] > 0,
    (* Get remaining variables (other than var) for assumptions *)
    (* These are Schwinger parameters: real and positive *)
    remainingVars = Complement[
      Cases[scaled, _Symbol, Infinity], 
      {var}
    ];
    assumptions = And @@ Map[# > 0 &, remainingVars];
    
    DebugPrint["[DEBUG-ReglimWord] remainingVars = ", remainingVars];
    DebugPrint["[DEBUG-ReglimWord] assumptions = ", assumptions];
    
    (* Build onAxis list with delta[var] for contour direction *)
    onAxis = Map[
      Function[letter,
        DebugPrint["[DEBUG-ReglimWord] Processing positive letter: ", letter];
        
        origin = If[minOrder > 0,
          DebugPrint["[DEBUG-ReglimWord]   minOrder > 0, using delta[var]"];
          delta[var],
          
          If[minOrder < 0,
            DebugPrint["[DEBUG-ReglimWord]   minOrder < 0, using -delta[var]"];
            -delta[var],
            
            (* minOrder = 0: compute from next-to-leading term *)
            DebugPrint["[DEBUG-ReglimWord]   minOrder = 0, computing from next-to-leading term"];
            
            idx = FirstPosition[scaled, letter][[1]];
            DebugPrint["[DEBUG-ReglimWord]   idx (position of letter in scaled) = ", idx];
            DebugPrint["[DEBUG-ReglimWord]   word[[idx]] = ", word[[idx]]];
            
            nextCoef = RatResidue[word[[idx]] * var^(-minOrder) - letter, var];
            nextCoef = Together[(*Simplify[*)nextCoef(*]*)];
            DebugPrint["[DEBUG-ReglimWord]   nextCoef (raw) = ", nextCoef];
            DebugPrint["[DEBUG-ReglimWord]   nextCoef (simplified) = ", Simplify[nextCoef]];
            
            (* Evaluate Im and Re with assumptions that remaining vars are real positive: Subscript[x, i] in [0,\[Infinity]]] *)
            imVal = Quiet[Refine[ComplexExpand[Im[nextCoef]], assumptions]];
            reVal = Quiet[Refine[ComplexExpand[Re[nextCoef]], assumptions]];
            
            DebugPrint["[DEBUG-ReglimWord]   Im[nextCoef] after Refine+ComplexExpand = ", imVal];
            DebugPrint["[DEBUG-ReglimWord]   Re[nextCoef] after Refine+ComplexExpand = ", reVal];
            
            (* If Refine didn't fully evaluate, try Simplify with assumptions *)
            If[!FreeQ[imVal, Im] || !FreeQ[imVal, Re],
              DebugPrint["[DEBUG-ReglimWord]   Im still contains Im/Re, trying Simplify with assumptions"];
              imVal = Quiet[Simplify[Im[nextCoef], Assumptions -> assumptions]]
            ];
            If[!FreeQ[reVal, Im] || !FreeQ[reVal, Re],
              DebugPrint["[DEBUG-ReglimWord]   Re still contains Im/Re, trying Simplify with assumptions"];
              reVal = Quiet[Simplify[Re[nextCoef], Assumptions -> assumptions]]
            ];
            
            DebugPrint["[DEBUG-ReglimWord]   Final imVal = ", imVal];
            DebugPrint["[DEBUG-ReglimWord]   Final reVal = ", reVal];
            DebugPrint["[DEBUG-ReglimWord]   NumericQ[imVal] = ", NumericQ[imVal]];
            DebugPrint["[DEBUG-ReglimWord]   NumericQ[reVal] = ", NumericQ[reVal]];
            
            (* Determine the origin based on Im/Re values *)
            finalOrigin = Which[
              (* Case 1: Im is a nonzero number *)
              NumericQ[imVal] && imVal != 0,
                DebugPrint["[DEBUG-ReglimWord]   -> Case 1: Im is nonzero number, Sign[imVal] = ", Sign[imVal]];
                Sign[imVal],
              
              (* Case 2: Im evaluates to zero - use Re with delta *)
              NumericQ[imVal] && imVal == 0,
                DebugPrint["[DEBUG-ReglimWord]   -> Case 2: Im is zero"];
                If[NumericQ[reVal] && reVal != 0,
                  DebugPrint["[DEBUG-ReglimWord]      Re is nonzero number, Sign[reVal]*delta[var] = ", Sign[reVal] * delta[var]];
                  Sign[reVal] * delta[var],
                  DebugPrint["[DEBUG-ReglimWord]      Re is zero or non-numeric, defaulting to delta[var]"];
                  delta[var]  (* Default if Re also problematic *)
                ],
              
              (* Case 3: Im is exactly 0 symbolically *)
              imVal === 0,
                DebugPrint["[DEBUG-ReglimWord]   -> Case 3: Im === 0 symbolically"];
                If[NumericQ[reVal] && reVal != 0,
                  DebugPrint["[DEBUG-ReglimWord]      Re is nonzero number"];
                  Sign[reVal] * delta[var],
                  If[reVal === 0,
                    DebugPrint["[DEBUG-ReglimWord]      Re === 0, defaulting to delta[var]"];
                    delta[var],
                    (* reVal is symbolic but nonzero - try to get sign *)
                    DebugPrint["[DEBUG-ReglimWord]      Re is symbolic, trying Refine[Sign[reVal]]"];
                    signRe = Quiet[Refine[Sign[reVal], assumptions]];
                    DebugPrint["[DEBUG-ReglimWord]      signRe = ", signRe];
                    If[NumericQ[signRe],
                      signRe * delta[var],
                      DebugPrint["[DEBUG-ReglimWord]      Could not determine sign, defaulting to delta[var]"];
                      delta[var]  (* Fallback *)
                    ]
                  ]
                ],
              
              (* Case 4: Im remained symbolic but should be 0 for real positive vars *)
              (* This is the KEY case that was failing *)
              True,
                DebugPrint["[DEBUG-ReglimWord]   -> Case 4: Im remained symbolic"];
                (* For real positive variables, Im should be 0 *)
                (* nextCoef is a rational function of real vars, so Im = 0 *)
                testIm = Quiet[Simplify[imVal /. Map[# -> 1 &, remainingVars]]];
                DebugPrint["[DEBUG-ReglimWord]      testIm (at vars=1) = ", testIm];
                If[testIm === 0 || (NumericQ[testIm] && testIm == 0),
                  (* Yes, Im is 0 for real vars: use Re branch *)
                  DebugPrint["[DEBUG-ReglimWord]      Im is 0 for real vars, using Re branch"];
                  testRe = Quiet[Refine[Sign[reVal], assumptions]];
                  DebugPrint["[DEBUG-ReglimWord]      Sign[reVal] with assumptions = ", testRe];
                  If[NumericQ[testRe] && testRe != 0,
                    testRe * delta[var],
                    (* Try evaluating Re at a test point *)
                    Module[{testReNum},
                      testReNum = Quiet[reVal /. Map[# -> 1 &, remainingVars]];
                      DebugPrint["[DEBUG-ReglimWord]      reVal at vars=1 = ", testReNum];
                      If[NumericQ[testReNum] && testReNum != 0,
                        Sign[testReNum] * delta[var],
                        DebugPrint["[DEBUG-ReglimWord]      Could not determine sign, defaulting to delta[var]"];
                        delta[var]
                      ]
                    ]
                  ],
                  (* Im might actually be nonzero: shouldn't happen for real vars *)
                  If[$HyperVerbosity > 0, Print["[ReglimWord] WARNING: Im[nextCoef] may be nonzero; check your variables are real"]];
                  delta[var]
                ]
            ];
            
            DebugPrint["[DEBUG-ReglimWord]   finalOrigin = ", finalOrigin];
            finalOrigin
          ]
        ];
        
        DebugPrint["[DEBUG-ReglimWord]   Final origin for letter ", letter, " = ", origin];
        {letter, origin}
      ],
      Sort[positiveLetters]
    ];
    
    DebugPrint["[DEBUG-ReglimWord] onAxis = ", onAxis];
    DebugPrint["[DEBUG-ReglimWord] Calling BreakUpContour with scaled = ", scaled];
    
    (* Use BreakUpContour to handle positive letters *)
    $ReglimWordCache[key] = BreakUpContour[{{1, scaled}}, onAxis];
    DebugPrint["[DEBUG-ReglimWord] BreakUpContour returned: ", Short[$ReglimWordCache[key], 3]];
    Return[$ReglimWordCache[key]]
  ];
  
  (* No positive letters - check if evaluable as period *)
  If[SubsetQ[{-2, -1, 0}, Union[scaled]],
    If[Union[scaled] === {0} || Union[scaled] === {-1},
      $ReglimWordCache[key] = {}; Return[{}]];

    Module[{evalResult},
      evalResult = BreakUpContour[{{1, scaled}}, {}];
      evalResult = Map[{#[[1]] //. mzvAllReductions (*// Simplify*), #[[2]]} &, evalResult];
      evalResult = Select[evalResult, #[[1]] =!= 0 &];
      $ReglimWordCache[key] = evalResult;
      Return[evalResult]
    ]
  ];
  
  $ReglimWordCache[key] = {{1, scaled}}; 
  $ReglimWordCache[key]
]


(* ::Section::Closed:: *)
(*Symbology*)


(*List notation & symbol related functions:*)

ShuffleCompress[wordlist_List]:=Module[{result=<||>,w,L,i,term},Do[L={{w[[1]],{}}};
Do[L=ShuffleProduct[L,{{1,i}}],{i,w[[2]]}];
Do[result[term[[2]]]=Lookup[result,Key[term[[2]]],0]+term[[1]],{term,L}],{w,wordlist}];
Select[KeyValueMap[{QuickCancel[#2],#1}&,result],#[[1]]=!=0&]]

DifferentiateWordlist[wordlist_List,var_]:=Module[{result={},w,coef,word,dcoef,a},
Do[coef=w[[1]];word=w[[2]];
dcoef=D[coef,var];
If[dcoef=!=0,AppendTo[result,{dcoef,word}]];
(*Derivative of Hlog:prepend contribution*)
If[Length[word]>0,a=word[[1]];
AppendTo[result,{coef/(var-a),Rest[word]}]],{w,wordlist}];
CollectWords[result]]

(*Symbol:*)
Options[ConvertToSymbol]={"Expand"->True,"AsList"->False,"DropConstants"->False};

ConvertToSymbol[expr_,OptionsPattern[]]:=Module[{result,expand,asList,dropConst},expand=OptionValue["Expand"];
asList=OptionValue["AsList"];
dropConst=OptionValue["DropConstants"];
(*Get the raw symbol as a list*)
result=ConvertToSymbolInternal[expr];
(*Handle failed conversion*)
If[!ListQ[result],Return[result]];
(*Optionally expand letters into irreducible factors*)If[expand&&Length[result]>0,result=SymbolExpand[result]];
(*Optionally drop constant entries?*)
If[dropConst,result=Select[result,!AllTrue[#[[2]],NumericQ]&]];
(*Collect terms*)
result=CollectWords[result];
(*Return as list or as symbolic sum*)If[asList,result,If[result==={},0,Total[Map[If[#[[2]]==={},#[[1]],#[[1]]*Sym[#[[2]]]]&,result]]]]]

(*Internal recursive conversion:*)
ConvertToSymbolInternal[expr_]:=Module[{result,i,var,word,f,temp,reduced,simplified},Which[
(*Null or zero*)
expr===0,{},
(*Already a wordlist-error*)
ListQ[expr]&&Length[expr]>0&&ListQ[expr[[1]]]&&Length[expr[[1]]]==2,Print["Error: List supplied to ConvertToSymbol. Already in symbol format?"];
expr,
(*Sum:convert each term and collect*)
Head[expr]===Plus,result={};
Do[temp=ConvertToSymbolInternal[expr[[i]]];
If[ListQ[temp],result=Join[result,temp]],{i,Length[expr]}];
CollectWords[result],
(*Product:shuffle product of symbols*)
Head[expr]===Times,result={{1,{}}};
Do[temp=ConvertToSymbolInternal[expr[[i]]];
If[ListQ[temp],result=ShuffleProduct[result,temp]],{i,Length[expr]}];
result,
(*Power with positive integer exponent*)
Head[expr]===Power&&IntegerQ[expr[[2]]]&&expr[[2]]>0,result={{1,{}}};
temp=ConvertToSymbolInternal[expr[[1]]];
If[!ListQ[temp],Return[{{expr,{}}}]];
Do[result=ShuffleProduct[result,temp],{expr[[2]]}];
result,
(*Hyperlogarithm-the main case...*)
Head[expr]===Hlog,var=expr[[1]];
word=expr[[2]];
If[word==={}||!ListQ[word],Return[{{1,{}}}]];
result={};
Do[
(*Compute the symbol letter from dropping letter i*)
f=1;
(*First letter contribution:(z-w1) when i=1 and z!=infinity*)
If[var=!=Infinity&&i===1,simplified=Simplify[var-word[[1]]];
If[simplified===0,Print["Error: ill-defined Hlog with z = w1: ",expr];
Return[{{expr,{}}}]];
f=f*(var-word[[1]])];
(*Last letter contribution:1/wn if wn!=0*)
If[i===Length[word]&&word[[i]]=!=0,f=f/word[[i]]];
(*Contribution from (wi-w_{i-1}) when i>1*)
If[i>1,simplified=Simplify[word[[i]]-word[[i-1]]];
If[simplified=!=0,f=f*(word[[i]]-word[[i-1]])]];
(*Contribution from 1/(wi-w_{i+1}) when i<n*)
If[i<Length[word],simplified=Simplify[word[[i]]-word[[i+1]]];
If[simplified=!=0,f=f/(word[[i]]-word[[i+1]])]];
(*Simplify the letter*)
f=Simplify[f];
(*Skip if letter is trivial (\[PlusMinus]1)*)
If[f===1||f===-1,Continue[]];
(*Get symbol of reduced Hlog (with letter i deleted)*)
reduced=ConvertToSymbolInternal[Hlog[var,Delete[word,i]]];
If[!ListQ[reduced],Continue[]];
(*Concatenate:prepend letter f to each word in reduced*)
temp=Map[{#[[1]],Prepend[#[[2]],f]}&,reduced];
result=Join[result,temp],{i,Length[word]}];
CollectWords[result],
(*Logarithm*)
Head[expr]===Log,{{1,{expr[[1]]}}},
(*Classical polylogarithm:Li_n(z)->symbol*)
Head[expr]===PolyLog&&IntegerQ[expr[[1]]]&&expr[[1]]>0,Module[{n,z},n=expr[[1]];
z=expr[[2]];
(*Symbol of Li_n(z) is -[z\[CircleTimes]z\[CircleTimes]... (n-1 times)\[CircleTimes](1-z)]*)
{{-1,Join[Table[z,{n-1}],{1-z}]}}],
(*Mpl:convert to Hlog first*)
Head[expr]===Mpl,ConvertToSymbolInternal[MplAsHlog[expr[[1]],expr[[2]]]],
(*MZV and Zeta:symbols are zero (constants)*)
Head[expr]===mzv||Head[expr]===Zeta||MatchQ[expr,_mzv],{},(*Pi is a constant*)expr===Pi,{},
(*Numeric constants*)
NumericQ[expr],If[expr===0,{},{{expr,{}}}],
(*Default:treat as constant coefficient*)
True,{{expr,{}}}]]

(*Expand symbol letters into irreducible factors*)
SymbolExpand[symbolList_List]:=Module[{result=<||>,word,accumulated,i,f,factList,term,newTerms},If[symbolList==={},Return[{}]];
Do[
(*Start with coefficient and empty word*)
accumulated={{word[[1]],{}}};
(*Process each letter in the word*)
Do[f=word[[2,i]];
(*Factor the letter*)
factList=Which[
(*Numeric:handle specially*)
NumericQ[f],If[IntegerQ[f]&&Abs[f]>1,
(*Factor integer into primes*)
Map[{#[[2]],{#[[1]]}}&,FactorInteger[f]],
(*Keep as is if not factorable*)
If[f===1||f===-1||f===I||f===-I,{{1,{}}},
(*Trivial-contributes empty*)
{{1,{f}}}]],
(*Symbolic expression:use FactorList*)
True,Module[{fl,constant,factors},fl=FactorList[f];
If[fl==={}||!ListQ[fl],Return[{{1,{f}}}]];
constant=fl[[1,1]];
factors=Rest[fl];
(*Build list of {exponent,{factor}}*)
factors=Map[{#[[2]],{#[[1]]}}&,factors];
(*Add constant if non-trivial*)
If[!MemberQ[{1,-1,I,-I},constant],factors=Prepend[factors,{1,{constant}}]];
If[factors==={},{{1,{}}},factors]]];
(*Remove trivial factors*)
factList=Select[factList,!(#[[2]]==={}||(#[[2]]=!={}&&MemberQ[{1,-1,I,-I},#[[2,1]]]))&];
If[factList==={},factList={{1,{}}}];
(*Concatenate with accumulated result*)
newTerms={};
Do[Do[AppendTo[newTerms,{acc[[1]]*fac[[1]],Join[acc[[2]],fac[[2]]]}],{fac,factList}],{acc,accumulated}];
accumulated=newTerms,{i,Length[word[[2]]]}];
(*Add to result*)
Do[result[term[[2]]]=Lookup[result,Key[term[[2]]],0]+term[[1]],{term,accumulated}],{word,symbolList}];
Select[KeyValueMap[{#2,#1}&,result],#[[1]]=!=0&]]
(*Simplify symbol by collecting terms*)
SymbolSimplify[symbolList_List]:=Module[{result},result=CollectWords[symbolList];
Map[{Simplify[#[[1]]],Map[Simplify,#[[2]]]}&,result]]
(*
(*Print symbol in tensor notation*)
SymbolPrint[symbolList_List]:=Column[Map[Row[{#[[1]]," \[CircleTimes] ",If[#[[2]]==={},"1",Row[Riffle[Map[ToString,#[[2]]]," \[CircleTimes] "]]]}]&,symbolList]]
*)

(*Weight of symbol*)
SymbolWeight[symbolList_List]:=If[symbolList==={},0,Max[Map[Length[#[[2]]]&,symbolList]]]

(*Operations for tensor product of two symbols [TODO?]:*)
(*SymbolTensorProduct[sym1_List,sym2_List]:=ConcatMul[sym1,sym2]*)
(*Coproduct (for checking integrability)*)
(*SymbolCoproduct[symbolList_List,position_Integer]:=Module[{left,right},Map[{#[[1]],{Take[#[[2]],position],Drop[#[[2]],position]}}&,symbolList]]*)
(*Weight of symbol (length of tensor)*)
(*SymbolWeight[symbolList_List]:=If[symbolList==={},0,Max[Map[Length[#[[2]]]&,symbolList]]]*)


(* ::Section:: *)
(*Other Functions*)


(*These are routines to factor a polynomial in a variable completely even if it gives rise to algebraic roots:*)
(*Standard version:*)
factorCompletely[poly_,x_]:=Module[{solns,lcoeff},solns=Solve[poly==0,x,Cubics->False,Quartics->False];
lcoeff=Coefficient[poly,x^Exponent[poly,x]];
lcoeff*(Times@@(x-(x/.solns)))]
(*Version with saved abstract coefficients:*)
factorCompletely2[poly_,x_,label_]:=Module[{lcoeff},
solns=Solve[poly==0,x,Cubics->False,Quartics->False];
solnsSym=solns/.Rule[a_,b_]:>Rule[a,Unique[abc]];
explicit[label]=Thread[Flatten[solnsSym//.Rule[a_,b_]:>b]->Flatten[solns//.Rule[a_,b_]:>b]];
lcoeff=Coefficient[poly,x^Exponent[poly,x]];
lcoeff*(Times@@(x-(x/.(*solns*)solnsSym)))
]

STFactorAndTrackRoots = factorCompletely2;


(* ::Section::Closed:: *)
(*End*)


(*Print["HyperIntica is ready."];*)

End[]
EndPackage[]
