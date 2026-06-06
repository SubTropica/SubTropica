# The Doppio algorithm: Fubini reduction with Euler-discriminant certification

*Status note (2026-06-05): this document describes the algorithm as
implemented in `doppio_lib.wl` (Mathematica, tests t01..t24) and in the
HyperFLINT port (`euler_chi`, `euler_filter`, `lr_scan`; ctest batteries
with exact cross-engine pins). It is written for collaborators who know
parametric Feynman integrals but have not followed the implementation.*

## 1. The question the algorithm answers

A Feynman integral in the Lee-Pomeransky representation is a twisted
period: with $G = U + F$ the sum of the Symanzik polynomials, or more
generally with a list of polynomial factors $P_1,\dots,P_m$,

$$ I \;=\; \int_{x_i > 0} \prod_{i} P_i(x;\,s)^{e_i}\; d^n x, $$

with exponents $e_i$ depending linearly on the dimensional regulator
$\varepsilon$. One efficient way to evaluate such integrals is to
integrate out one Feynman parameter at a time in terms of
hyperlogarithms (iterated integrals with rational letters). This works
precisely when, at every step, the singularities of the partial
integral are rational in the next integration variable: the property
called **linear reducibility**. The algorithm decides, before any
integration is attempted, whether a linearly reducible order of the
variables exists, which orders are admissible, and what the alphabet of
the answer will be, including controlled square-root extensions.

Three classical difficulties motivate the design:

1. The standard reduction (Brown; Panzer's HyperInt; our production
   engine "Lungo") propagates an **upper bound** on the singularities:
   it can produce *fictitious* letters, polynomials that never appear
   as actual singularities. A fictitious non-linear letter can wrongly
   veto an integration order (a false "not linearly reducible"
   verdict), and fictitious letters pollute the predicted alphabet.
2. Square roots: many integrals are reducible only in an extended
   sense, where the final answer contains algebraic letters
   ($\sqrt{\text{K\"all\'en}}$ functions and relatives). Deciding
   *which* roots are admissible, and when, requires care.
3. Projective integrands: when the integrand is scale invariant, one
   of the $n$ integrations never happens, and a naive $n$-variable
   reduction reports an artificial obstruction.

## 2. The Fubini reduction (the generator)

The classical layer is unchanged from Brown and Panzer. For a subset
$S$ of the integration variables (the variables already integrated),
the engine maintains a set of letters $L_S$, polynomials in the
remaining variables and the kinematics. One step works as follows: to
pass from $L_{S\setminus\{v\}}$ to a candidate set for $L_S$, take, for
every letter $f$,

* the leading coefficient of $f$ in $v$,
* the discriminant of $f$ in $v$ (when $\deg_v f \ge 1$),

and for every pair $f, g$ with positive degree in $v$ their resultant
in $v$; factor everything over $\mathbb{Q}$ and keep one representative
per proportionality class. Because $S$ can be reached by removing any
of its elements last, the engine computes the candidate set for every
choice of the last-removed variable and **intersects** the results
(the Fubini, or compatibility-graph, refinement). The boundary
polynomials $x_j$ are included in the seed set; they generate the
trailing coefficients automatically as resultants and must never be
discarded.

This layer is fast and complete: the true singularities of the
$S$-marginal integral are always among the candidates. It is not
faithful: candidates can be fictitious, and on non-intersected
(single-order) reductions they frequently are.

## 3. Euler-discriminant certification (the filter)

The new layer decides, for every candidate letter, whether it is a
genuine singularity of the $S$-marginal integral. The criterion comes
from the theory of Euler discriminants: for a twisted integral, a
kinematic point lies on the singular locus exactly when the Euler
characteristic of the very affine variety attached to the integrand
**drops** relative to its generic value. For our setup the relevant
count is the number of critical points of
$\log \Psi = \sum_i e_i \log P_i$ on the complement of
$\prod_i P_i = 0$, summed over the coordinate sectors; this is the
Lee-Pomeransky critical-point count, and it is computable.

Concretely, for a candidate letter $\ell$ at subset $S$:

1. compute the generic count $\chi_{\rm gen}$: numericize all
   parameters to random values, count solutions of the **cleared dlog
   system**

   $$N_v = \sum_i e_i \Big(\prod_{j\neq i} P_j\Big)\,\partial_v P_i = 0
   \quad (v \in S), \qquad 1 - z\,\prod_i P_i = 0,$$

   as the number of standard monomials of a Gr\"obner basis modulo a
   prime (the twist exponents enter only as coefficients, so their
   values are free; this is what makes the count cheap);
2. compute the same count constrained to a generic point of
   $\{\ell = 0\}$;
3. the letter is **genuine** iff the constrained count is strictly
   smaller.

Robustness rules, all conservative in the keep direction: the generic
count is the maximum of two independent draws (an unlucky prime can
only undercount); a destructive verdict (dropping a letter) requires
two independent no-drop draws; a positive-dimensional constrained
count, or any failure, keeps the letter; boundary letters $x_j$ are
exempt (they are integration-region boundaries, not Landau loci).
When every factor is homogeneous in the sector variables the count
dies identically by the Euler relation; the implementation then passes
to the chart $x_{\rm last} = 1$ automatically.

The filter runs after the Fubini intersection at every subset. On all
one-loop single-group inputs we tried (box family, pentagons, and the
authentic per-face production inputs of the massless and three-mass
boxes), the intersection is already clean and the filter certifies
rather than corrects. Fictitious letters appear, and are removed, on
non-intersected sequential reductions and on multi-factor inputs; the
first natural catch is described in Section 6.

## 4. Projective inputs: automatic Cheng-Wu and the gauge scan

If every $P_i$ is homogeneous in the integration variables and

$$\sum_i e_i \deg P_i \;=\; -\,n \qquad \text{identically in } \varepsilon,$$

the integrand is scale invariant and the integral is a projective
period: only $n-1$ integrations happen. Crawling all $n$ variables
then produces a spurious obstruction. By the Cheng-Wu theorem one may
gauge-fix any single variable to $1$; the engine detects projectivity
from the supplied exponents and applies the gauge automatically.

Because the *crawl* (unlike the integral) is not gauge invariant, the
engine also implements a **gauge scan**: crawl the ungauged
$n$-variable system to depth $n-1$; the variable left un-integrated
*is* the gauge, and the best order selects it. One subset table serves
all $n$ gauges at about twice the cost of a single gauged run, because
the letters of the gauge-$j$ run are exactly the dehomogenizations at
$x_j = 1$ of the ungauged letters (dehomogenization commutes with the
leading-coefficient, discriminant, and resultant operations, and
preserves irreducibility of gauge-coprime letters). Admissibility is
judged per gauge on the dehomogenized letters. A "not linearly
reducible" verdict from the scan is therefore gauge exhaustive.

## 5. The keep-rules: what blocks an integration order

At a step "integrate $v$ from subset $S$", every surviving letter is
judged in the pivot variable $v$. Degrees are degrees in $v$, not
total degrees.

**Strict rule** (the rational class):

* $\deg_v \le 1$: pass.
* $\deg_v = 2$, say $q = A v^2 + B v + C$ with $A, B, C$ polynomials in
  the pending variables and kinematics: pass iff the conic $u^2 = q$
  has a rational point, operationally iff $A$ **and** $C$ are perfect
  squares as polynomials. Then an Euler substitution rationalizes
  $\sqrt{B^2 - 4AC}$ and the $v$-integration stays inside
  hyperlogarithms.
* $\deg_v \ge 3$: block (cubic roots leave the class).

**FindRoots rule** (the algebraic-letter class) adds two tiers for a
non-rationalizable quadratic. The guiding fact is that integrating a
dlog form against $q$ produces, up to rational pieces,
$\tfrac{1}{\sqrt{\Delta}} \log\frac{2Av+B-\sqrt{\Delta}}{2Av+B+\sqrt{\Delta}}$
with $\Delta = B^2 - 4AC$: the new functions carry $\sqrt{\Delta}$ as a
function of the *pending* variables, so the right question is whether
the remaining integrations can live with that root.

* **Terminal tier**: if $v$ is the last variable, $\Delta$ is purely
  kinematic and the roots of $q$ are algebraic kinematic numbers,
  letters of the answer rather than obstacles. Pass.
* **Carry tier**: otherwise factor $\Delta = k \prod_i f_i^{m_i}$ and
  take the odd-multiplicity part (even powers leave the root
  rationally; the numeric content contributes a constant
  $\sqrt{k}$, which changes no function class). If the odd part is
  empty, the roots are rational: pass. If it is purely kinematic,
  this is the familiar kinematic square-root letter: pass. If it
  involves pending variables, pass **and carry** the odd part as an
  obligation: it joins the letter set of every later step of the same
  path and is re-judged by these same tiers recursively, discharged
  once its variables are integrated. A zero discriminant (a double
  rational line) passes cleanly: its dlog produces no root at all.

Each completed order reports its profile
(carried roots, kinematic roots, terminal quadratics). The rule is
**necessary-only**: every tier is a necessary condition for remaining
in the (algebraic-letter) hyperlogarithm class, and the recursion does
not regenerate the discriminant and resultant letters of the
root-bearing integrand against the other letters. This is the same
epistemic status as the production FindRoots mechanism, slightly
tightened by the recursive re-judging. In practice one trusts the
score-minimal order and treats orders with many carried roots as
speculative candidates. A separate caveat: the terminal discharge
assumes one root per term (two distinct roots of the same variable in
a single term would be a genus obstruction); the engines count
obligations but do not check pairwise simultaneity, and the integrator
fails loudly there if it ever matters.

How the terminal step is actually integrated: each terminal quadratic
is handed to the integrator as a root polynomial; the integrator
introduces its two roots as formal letters (the $W_\mp$ mechanism),
every letter becomes linear in the last variable over the extension,
and the endpoint evaluation leaves hyperlogarithms at algebraic
kinematic arguments. Equivalently, per term, the Euler substitution
adapted to the single carried root rationalizes the prefactor
$1/\sqrt{\Delta}$ and all logarithm arguments simultaneously.

## 6. Worked example: uq5

The integrand (a collaborator-supplied five-variable quadruple; the
first genuinely multi-factor input the engine saw):

$$ P_1 = x_1 + x_2 + x_3, \qquad
   P_2 = -qq_1 x_1 x_2 - qq_2 x_1 x_3 + 2 wb_1 x_3 x_4 - x_4^2
         + 2 wb_2 x_2 x_5 - x_5^2 + 2 yb\, x_4 x_5, $$

with twist exponents $e_1 = 1 + 2\varepsilon$, $e_2 = -3 - \varepsilon$
and integration variables $x_1,\dots,x_5$.

**Projectivity.** Both factors are homogeneous, with
$\deg P_1 = 1$, $\deg P_2 = 2$, and

$$ e_1 \cdot 1 + e_2 \cdot 2
   = (1 + 2\varepsilon) + 2(-3 - \varepsilon) = -5 = -n $$

identically in $\varepsilon$. The integrand is projective; the
original five-variable crawl reported an artificial obstruction, which
disappeared under the automatic gauge $x_5 = 1$ (and the gauge scan
later confirmed every statement below gauge-exhaustively).

**A fictitious letter, caught.** At the intermediate subset
$S = \{x_2, x_3, x_4, x_5\}$ the Fubini generator produces, among
genuine letters, the bare letter $yb$. The certification of Section 3
finds no Euler-characteristic drop on $\{yb = 0\}$ under either twist
variant and under two independent counting backends: $yb$ is
fictitious and is removed. The control letter
$wb_1^2 + wb_2^2 - 2\, wb_1 wb_2\, yb$ at the same subset shows a
genuine drop and is kept. This was the filter's first natural catch;
it is now a regression fixture in both engines.

**Strict verdict.** After the gauge, the order
$(x_2, x_3)$ proceeds linearly, but at the $x_1$ step two chi-genuine
quadratic letters block:

$$ q_1 = qq_1 x_1^2 - 2 wb_2 x_1 - (1 + x_4^2 - 2 yb\, x_4), \qquad
   q_2 = qq_2 x_1^2 - 2 wb_1 x_4\, x_1 - (1 + x_4^2 - 2 yb\, x_4), $$

up to overall signs. Their leading coefficients $qq_1, qq_2$ are not
perfect squares, so the Euler conic has no rational point and the
strict rule rejects, at this and at every other gauge: uq5 is
genuinely not linearly reducible in the rational class. Every blocking
letter is certified genuine, so no amount of fictitious-letter removal
changes this.

**FindRoots verdict.** The discriminants of the blockers in $x_1$ are

$$ \Delta_1 \propto qq_1 + wb_2^2 + qq_1 x_4^2 - 2 qq_1 yb\, x_4, \qquad
   \Delta_2 \propto qq_2 + (qq_2 + wb_1^2) x_4^2 - 2 qq_2 yb\, x_4, $$

quadratics in the single pending variable $x_4$. The carry tier
accepts both blockers and carries $\Delta_1, \Delta_2$. At the
terminal $x_4$ step both obligations are quadratics with no pending
variables left: the terminal tier discharges them, their roots being
algebraic kinematic letters (their own $x_4$-discriminants reproduce
the certified terminal letters $qq_i\, yb^2 - qq_i - wb^2_{\,\cdot}$,
and the $1 + x_4^2 - 2 yb\, x_4$ family supplies $yb \pm 1$). The
order $(x_2, x_3, x_1, x_4)$ at gauge $x_5$ is admissible with exactly
two carried roots, reproducing the reduction found by hand. The full
scan finds 120 admissible orders across all five gauges; the
sixteen-letter terminal alphabet is chi-certified. Both engines agree
on every number above (the Mathematica t24 battery and the HyperFLINT
`lr_scan` battery pin them exactly).

The same pattern (projective input, genuine quadratic blockers whose
discriminants live in to-be-terminal variables) subsequently unlocked
five library integrals on which the production engine reports NOLR
even with its root mechanism enabled; the mechanism difference is
precisely the carry tier, since production accepts only purely
kinematic discriminants.

## 7. Implementations and validation

* **Mathematica** (`doppio_lib.wl`): the full engine. Variants: B
  (per-subset multivariate elimination via SPQR/msolve, the oracle),
  KinNumeric over $\mathbb{Q}$ (fast numeric cross-check), and C (the
  production candidate: the Fubini generator with the per-subset
  certification of Section 3, pure Mathematica). Wired into SubTropica
  as `STIntegrate[..., "MethodLR" -> "Doppio"]` since v1.2.2.2, with
  contract parity against the production dispatcher validated on
  authentic per-face inputs. Test suite t01..t24, all green in fresh
  kernels.
* **HyperFLINT** (C++/FLINT): `euler_chi` (the counting core; Gr\"obner
  bases from msolve), `euler_filter` (the certification filter, opt-in
  inside the production order scorer via `HF_EULER_FILTER=1`, default
  off and byte-identical off), `lr_scan` (projectivity detection, the
  gauge scan, and both keep-rules as a verdict engine). Oracle
  batteries pin the exact Mathematica values, including the uq5
  numbers; the certification workload runs roughly six to ten times
  faster than the Mathematica engine (uq5 table plus filter: 14.1 s
  versus 2.2 s).
* **Validation highlights**: the counting core reproduces the
  principal-Landau-determinant database values exactly on twelve
  diagrams; the staircase counter survived a fifty-thousand-case
  randomized fuzz against brute force; the per-face experiment shows
  the production engine's intersected per-face letters are all genuine
  on the box family (the filter certifies rather than corrects there);
  adversarial and physics reviews of every layer are on file, with all
  findings folded.

One implementation note worth knowing: msolve 0.9.4 silently computes
wrong Gr\"obner bases (or crashes) when fed input coefficients at or
above $2^{32}$ in multi-term systems. Every serialization in the port
reduces coefficients into $[0, p)$ first; anyone calling msolve
directly should do the same.

## 8. Provenance

The algorithm assembles five lineages: the hyperlogarithm reduction of
Brown (arXiv:0804.1660, 0910.0114) as made algorithmic by Panzer's
HyperInt (arXiv:1403.3385); the Euler-substitution treatment of square
roots in the tradition of Besier, van Straten, and Weinzierl
(arXiv:1809.10983) and HyperInt's root mechanism; the critical-point
counting of Lee and Pomeransky (arXiv:1308.6676) with the
Euler-characteristic identification of Bitoun, Bogner, Klausen, and
Panzer (arXiv:1712.09215); the Landau-discriminant and
principal-Landau-determinant program (Mizera, Telen, arXiv:2109.08036;
Fevola, Mizera, Telen, arXiv:2311.16219), with the theorem that the
Euler discriminant is the singular locus supplied by Matsubara-Heo
(arXiv:2505.13163); and, computationally, the DiscKosky counting
package of Crisanti, Lippstreu, McLeod, and Pola\v{c}kov\'a together
with the SPQR elimination machinery of Chestnov and Crisanti
(arXiv:2511.14875) and msolve. The synthesis (certified reduction,
the cleared-dlog factored interface, the carried-discriminant tier,
the projective gauge scan) is original to this codebase; the carry
tier was calibrated against a reduction of uq5 found by hand.

## 9. Current limitations, honestly stated

1. The FindRoots tier is necessary-only; admissibility under it is a
   candidate verdict, not a reduction theorem. Prefer score-minimal
   orders; integrate one candidate end to end before relying on a
   family-wide claim.
2. The per-term single-root assumption at terminal discharge is not
   checked by the scorer (the integrator fails loudly if violated).
3. The whole-quadruple route exceeds the practical budget for large
   inputs (in Mathematica, roughly seven or more propagators with
   nontrivial mass structure); the per-face route through the
   production pipeline is the indicated tool there, and the HyperFLINT
   port shifts the boundary outward by its constant factor.
4. The certification verdicts are probabilistic in the benign
   direction only: unlucky primes or points can keep a fictitious
   letter (never drop a genuine one under the two-draw destructive
   protocol, up to the stated residual probabilities).
5. The HyperFLINT scan is a verdict engine: its conic and carry
   acceptances are not yet executable by the HyperFLINT integrator
   itself (the production scorer therefore keeps its narrower
   quadratic rule), while the Mathematica engine hands root
   polynomials to the existing integration machinery.