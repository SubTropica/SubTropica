// Phase 7-i: AlgebraicLetterTable implementation.

#include "hyperflint/algebra/algebraic_letters.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_set>

namespace hyperflint {

// -------- AlgebraicLetterEntry name accessors --------

std::string AlgebraicLetterEntry::wm_name() const {
    return "Wm_" + std::to_string(idx);
}
std::string AlgebraicLetterEntry::wp_name() const {
    return "Wp_" + std::to_string(idx);
}
std::string AlgebraicLetterEntry::wm_over_wp_name() const {
    return "WmOverWp_" + std::to_string(idx);
}

// -------- AlgebraicLetterTable singleton --------
//
// Thread-safety: single static mutex guards every public method
// plus the content-based dedup map inside `allocate`. The OpenMP
// outer-loop parallelism plan requires that two threads factoring
// the same deg-2 poly get the same idx back — otherwise the
// al_alloc regression (which asserts idx == 1 on first use) fails
// non-deterministically.

static std::mutex g_alg_letter_mu;

AlgebraicLetterTable& AlgebraicLetterTable::global() {
    static AlgebraicLetterTable instance;
    return instance;
}

long AlgebraicLetterTable::allocate(Poly polynomial, long var_idx) {
    // Compute Vieta sum / product / discriminant from the polynomial.
    //   polynomial = lc * var^2 + b * var + c
    // (downstream callers guarantee deg-in-var(polynomial) == 2).
    if (polynomial.degree_in_var(static_cast<size_t>(var_idx)) != 2) {
        throw std::runtime_error(
            "AlgebraicLetterTable::allocate: polynomial must be degree 2 in "
            "var_idx, got degree " + std::to_string(
                polynomial.degree_in_var(static_cast<size_t>(var_idx))));
    }

    // Content-based dedup key: canonical poly string plus var_idx.
    // Built outside the mutex since to_string can be expensive.
    std::string dedup_key =
        std::to_string(var_idx) + "|" + polynomial.to_string();

    std::lock_guard<std::mutex> lk(g_alg_letter_mu);
    auto it = content_index_.find(dedup_key);
    if (it != content_index_.end()) return it->second;

    const long idx = static_cast<long>(entries_.size()) + 1;

    Poly lc_poly = polynomial.coefficient_of_var(
        static_cast<size_t>(var_idx), 2);
    Poly b_poly = polynomial.coefficient_of_var(
        static_cast<size_t>(var_idx), 1);
    Poly c_poly = polynomial.coefficient_of_var(
        static_cast<size_t>(var_idx), 0);

    // sum = -b/lc, product = c/lc, disc = b² - 4·lc·c.
    Rat lc_rat(lc_poly);
    Rat b_rat(b_poly);
    Rat c_rat(c_poly);
    Rat sum_value     = -b_rat / lc_rat;
    Rat product_value = c_rat / lc_rat;

    // disc = b·b - (4·lc)·c. Stay in Poly for the discriminant since
    // it's typically what gets squared back out by the user; Rat
    // wrapping happens at back-sub time.
    Poly four_lc = lc_poly + lc_poly + lc_poly + lc_poly;
    Poly four_lc_c = four_lc * c_poly;
    Poly b_squared = b_poly * b_poly;
    Poly disc = b_squared - four_lc_c;

    AlgebraicLetterEntry entry{
        idx,
        std::move(polynomial),
        var_idx,
        std::move(lc_rat),
        std::move(sum_value),
        std::move(product_value),
        std::move(disc)
    };
    entries_.push_back(std::move(entry));
    content_index_.emplace(std::move(dedup_key), idx);
    return idx;
}

const AlgebraicLetterEntry& AlgebraicLetterTable::at(long idx) const {
    std::lock_guard<std::mutex> lk(g_alg_letter_mu);
    if (idx < 1 || idx > static_cast<long>(entries_.size())) {
        throw std::out_of_range(
            "AlgebraicLetterTable::at: idx " + std::to_string(idx)
            + " out of range [1, " + std::to_string(entries_.size()) + "]");
    }
    return entries_[static_cast<size_t>(idx - 1)];
}

long AlgebraicLetterTable::size() const {
    std::lock_guard<std::mutex> lk(g_alg_letter_mu);
    return static_cast<long>(entries_.size());
}

void AlgebraicLetterTable::clear() {
    std::lock_guard<std::mutex> lk(g_alg_letter_mu);
    entries_.clear();
    content_index_.clear();
}

std::vector<long> AlgebraicLetterTable::indices() const {
    std::lock_guard<std::mutex> lk(g_alg_letter_mu);
    std::vector<long> out;
    out.reserve(entries_.size());
    for (const auto& e : entries_) out.push_back(e.idx);
    return out;
}

// -------- build_algebraic_letter_var_list --------

std::vector<std::string> build_algebraic_letter_var_list(
    const std::vector<std::string>& vars,
    long pool_size) {
    if (pool_size < 0) {
        throw std::invalid_argument(
            "build_algebraic_letter_var_list: negative pool_size");
    }
    std::unordered_set<std::string> seen(vars.begin(), vars.end());
    std::vector<std::string> out = vars;
    auto add = [&](const std::string& name) {
        if (seen.insert(name).second) out.push_back(name);
    };
    for (long i = 1; i <= pool_size; ++i) {
        add("Wm_" + std::to_string(i));
        add("Wp_" + std::to_string(i));
        add("WmOverWp_" + std::to_string(i));
        // Phase 7-v: pre-allocate the sqrt_disc_<i> atom so
        // back_substitute can produce its replacement Rat in the
        // existing ctx (no ctx-extension dance per call).
        add("sqrt_disc_" + std::to_string(i));
    }
    return out;
}

std::vector<std::string> build_full_var_list(
    const MzvReductionTable& table,
    const std::vector<std::string>& vars,
    long pool_size) {
    auto with_mzv = build_mzv_var_list(table, vars);
    return build_algebraic_letter_var_list(with_mzv, pool_size);
}

// -------- simplify_with_vieta --------

namespace {

// Locate the PolyCtx variable index for a name. Returns -1 if absent.
long find_var_idx(const PolyCtx& ctx, const std::string& name) {
    const auto& vars = ctx.vars();
    for (size_t i = 0; i < vars.size(); ++i) {
        if (vars[i] == name) return static_cast<long>(i);
    }
    return -1;
}

// Reduce `numerator` so its degree in `var_idx` is ≤ 1, using the
// recurrence  V^n = sum * V^(n-1) - product * V^(n-2)  for n ≥ 2.
//
// The recurrence makes each V^n  =  U_n  ·  V  +  L_n  with U_n, L_n
// in the (sum, product) ring. Concretely:
//   U_0 = 0, L_0 = 1
//   U_1 = 1, L_1 = 0
//   U_k = sum * U_{k-1} - product * U_{k-2}
//   L_k = sum * L_{k-1} - product * L_{k-2}
// So V^k = U_k · V + L_k. The final reduced numerator is
//   sum_{k=0}^{maxDeg} c_k · V^k = sum c_k·(U_k·V + L_k)
//                               = (sum c_k·U_k) · V + (sum c_k·L_k).
Rat reduce_var_via_recurrence(const PolyCtx& ctx,
                               const Rat& numerator,
                               long var_idx,
                               const Rat& sum_val,
                               const Rat& product_val) {
    long max_deg = numerator.num().degree_in_var(static_cast<size_t>(var_idx));
    if (max_deg < 2) return numerator;

    // Strip the denominator: by the contract of simplify_with_vieta the
    // denominator is var-free. We process num as a Rat (so coefs of
    // each V^k are Rats / numerator.den()), and reattach den at the end.
    // Equivalently, work entirely on num and divide once.
    const Poly& num_poly = numerator.num();
    const Poly& den_poly = numerator.den();

    // Pre-compute U_k and L_k for k = 0..max_deg as Rats over ctx.
    // U_k, L_k are rational expressions in (sum_val, product_val).
    Rat zero{Poly::zero_of(ctx)};
    Rat one {Poly::one_of(ctx)};
    std::vector<Rat> U; U.reserve(static_cast<size_t>(max_deg + 1));
    std::vector<Rat> L; L.reserve(static_cast<size_t>(max_deg + 1));
    U.push_back(zero); L.push_back(one);     // V^0 = 1 = 0·V + 1
    if (max_deg >= 1) {
        U.push_back(one); L.push_back(zero); // V^1 = V = 1·V + 0
    }
    for (long k = 2; k <= max_deg; ++k) {
        U.push_back(sum_val * U.back() - product_val * U[U.size() - 2]);
        L.push_back(sum_val * L.back() - product_val * L[L.size() - 2]);
    }

    // Accumulate sum_k c_k · (U_k V + L_k)   where c_k = num_poly.coef(V, k) / den.
    Rat acc_V    = zero;   // coefficient of V in result
    Rat acc_free = zero;   // V-free part
    for (long k = 0; k <= max_deg; ++k) {
        Poly ck = num_poly.coefficient_of_var(
            static_cast<size_t>(var_idx), k);
        if (ck.is_zero()) continue;
        Rat ck_rat(ck, den_poly);
        acc_V    = acc_V    + ck_rat * U[static_cast<size_t>(k)];
        acc_free = acc_free + ck_rat * L[static_cast<size_t>(k)];
    }

    // Build  result = acc_free + acc_V * V .
    Rat var_rat{Poly::gen(ctx, static_cast<size_t>(var_idx))};
    return acc_free + acc_V * var_rat;
}

}  // namespace

// Sum of Wm_i, Wp_i, and WmOverWp_i degrees in num+den across every
// allocated pair. Used as a proxy "atom count" to decide whether
// combine_wm_wp_ratios's substitution actually shrinks the expression.
namespace {
long algebraic_atom_total(const Rat& r) {
    const PolyCtx& ctx = r.ctx();
    auto& T = AlgebraicLetterTable::global();
    long total = 0;
    for (long idx : T.indices()) {
        const auto& e = T.at(idx);
        long wm_idx    = find_var_idx(ctx, e.wm_name());
        long wp_idx    = find_var_idx(ctx, e.wp_name());
        long ratio_idx = find_var_idx(ctx, e.wm_over_wp_name());
        for (long v : {wm_idx, wp_idx, ratio_idx}) {
            if (v < 0) continue;
            long dn = r.num().degree_in_var(static_cast<size_t>(v));
            long dd = r.den().degree_in_var(static_cast<size_t>(v));
            if (dn > 0) total += dn;
            if (dd > 0) total += dd;
        }
    }
    return total;
}
}  // namespace

Rat combine_wm_wp_ratios(const Rat& r) {
    const PolyCtx& ctx = r.ctx();
    auto& T = AlgebraicLetterTable::global();
    if (T.size() == 0) return r;

    Rat current = r;
    for (long idx : T.indices()) {
        const auto& e = T.at(idx);
        long wm_idx    = find_var_idx(ctx, e.wm_name());
        long wp_idx    = find_var_idx(ctx, e.wp_name());
        long ratio_idx = find_var_idx(ctx, e.wm_over_wp_name());
        if (wm_idx < 0 || wp_idx < 0 || ratio_idx < 0) continue;

        // Quick gate: if there's no Wm_i in num or no Wp_i in den,
        // the substitution can't shrink anything.
        long num_wm_deg = current.num().degree_in_var(
            static_cast<size_t>(wm_idx));
        long den_wp_deg = current.den().degree_in_var(
            static_cast<size_t>(wp_idx));
        if (num_wm_deg <= 0 || den_wp_deg <= 0) continue;

        // Substitute Wm_i = WmOverWp_i · Wp_i in the whole Rat. The Rat
        // constructor reduces to lowest terms, cancelling the
        // newly-introduced Wp_i factor against the existing den's
        // Wp_i factor.
        Rat replacement = Rat::parse(ctx,
            e.wm_over_wp_name() + "*" + e.wp_name());
        Rat candidate = substitute_var_rat(ctx, current,
                                            static_cast<size_t>(wm_idx),
                                            replacement);

        // Accept only if total Wm/Wp/WmOverWp atom count shrinks.
        if (algebraic_atom_total(candidate) < algebraic_atom_total(current)) {
            current = candidate;
        }
    }
    return current;
}

Rat back_substitute(const Rat& r) {
    const PolyCtx& ctx = r.ctx();
    auto& T = AlgebraicLetterTable::global();
    if (T.size() == 0) return r;

    Rat half = Rat::parse(ctx, "1/2");
    Rat current = r;
    for (long idx : T.indices()) {
        const auto& e = T.at(idx);
        long wm_idx = find_var_idx(ctx, e.wm_name());
        long wp_idx = find_var_idx(ctx, e.wp_name());
        long sd_idx = find_var_idx(ctx, "sqrt_disc_" + std::to_string(idx));
        if (wm_idx < 0 || wp_idx < 0 || sd_idx < 0) {
            throw std::runtime_error(
                "back_substitute: ctx is missing Wm_/Wp_/sqrt_disc_ atoms "
                "for pair " + std::to_string(idx)
                + " — call build_algebraic_letter_var_list at ctx setup");
        }
        Rat sqrt_disc = Rat::parse(ctx, "sqrt_disc_" + std::to_string(idx));
        Rat sum_half = e.sum_value * half;
        Rat sqrt_over_2lc = sqrt_disc * half / e.lc;

        Rat wm_replacement = sum_half - sqrt_over_2lc;
        Rat wp_replacement = sum_half + sqrt_over_2lc;

        current = substitute_var_rat(ctx, current,
                                      static_cast<size_t>(wm_idx),
                                      wm_replacement);
        current = substitute_var_rat(ctx, current,
                                      static_cast<size_t>(wp_idx),
                                      wp_replacement);
    }
    return current;
}

Rat simplify_with_vieta(const Rat& r) {
    const PolyCtx& ctx = r.ctx();
    auto& T = AlgebraicLetterTable::global();
    if (T.size() == 0) return r;

    // Per Mma's SimplifyWithVieta (HyperIntica.wl:3110): bail out if
    // the denominator contains any algebraic atom. The user can
    // pre-clear via partial fractions.
    for (long idx : T.indices()) {
        const auto& e = T.at(idx);
        long wm_idx = find_var_idx(ctx, e.wm_name());
        long wp_idx = find_var_idx(ctx, e.wp_name());
        if (wm_idx < 0 || wp_idx < 0) continue;
        if (r.den().degree_in_var(static_cast<size_t>(wm_idx)) > 0
         || r.den().degree_in_var(static_cast<size_t>(wp_idx)) > 0) {
            return r;
        }
    }

    // Per-pair processing matches Mma's Do[..., {i, indices}]:
    // for each pair, fully reduce the current `e` (power rules + Wm·Wp
    // collapse) to get `expanded`, then check whether it matches the
    // antisymmetric reconstruction `a + (b+c)/2·sum + (b-c)/2·(Wm-Wp)`
    // exactly. Mma's PossibleZeroQ check passes iff b + c = 0
    // (otherwise the residual `(b+c)/2·(Wm + Wp − sum)` is non-zero
    // polynomially even though it's algebraically zero modulo Vieta).
    // When the check fails, `e` is kept unchanged for this pair —
    // subsequent pairs continue from that state.
    Rat e = r;
    Rat half  = Rat::parse(ctx, "1/2");

    for (long idx : T.indices()) {
        const auto& entry = T.at(idx);
        long wm_idx = find_var_idx(ctx, entry.wm_name());
        long wp_idx = find_var_idx(ctx, entry.wp_name());
        if (wm_idx < 0 || wp_idx < 0) continue;

        // Reduce powers ≥ 2 of Wm and Wp via the Chebyshev-like
        // recurrence; this leaves degree ≤ 1 in each.
        Rat expanded = reduce_var_via_recurrence(
            ctx, e, wm_idx, entry.sum_value, entry.product_value);
        expanded = reduce_var_via_recurrence(
            ctx, expanded, wp_idx, entry.sum_value, entry.product_value);

        // Collapse the Wm·Wp bilinear coefficient into the constant
        // part: extract a, b, c, d, then folder d·product into a.
        const Poly& num_poly = expanded.num();
        const Poly& den_poly = expanded.den();
        Poly num_no_wm = num_poly.coefficient_of_var(
            static_cast<size_t>(wm_idx), 0);
        Poly num_wm    = num_poly.coefficient_of_var(
            static_cast<size_t>(wm_idx), 1);
        Poly a_poly = num_no_wm.coefficient_of_var(
            static_cast<size_t>(wp_idx), 0);
        Poly c_poly = num_no_wm.coefficient_of_var(
            static_cast<size_t>(wp_idx), 1);
        Poly b_poly = num_wm   .coefficient_of_var(
            static_cast<size_t>(wp_idx), 0);
        Poly d_poly = num_wm   .coefficient_of_var(
            static_cast<size_t>(wp_idx), 1);

        Rat a_rat(a_poly, den_poly);
        Rat b_rat(b_poly, den_poly);
        Rat c_rat(c_poly, den_poly);
        Rat d_rat(d_poly, den_poly);
        // Fold d·product into a (the Wm·Wp collapse).
        Rat a_collapsed = a_rat + d_rat * entry.product_value;

        // PossibleZeroQ-equivalent check: Mma's reconstruction succeeds
        // iff b + c = 0. (Algebraically, the residue is
        // (b+c)/2·(Wm+Wp−sum), which is zero modulo Vieta but not
        // polynomially unless b+c=0.)
        if (!(b_rat + c_rat).is_zero()) {
            // Per-pair give-up: keep `e` unchanged and continue.
            continue;
        }

        // b + c = 0:  reconstructed = a + (b−c)/2 · (Wm − Wp).
        Rat wm_rat{Poly::gen(ctx, static_cast<size_t>(wm_idx))};
        Rat wp_rat{Poly::gen(ctx, static_cast<size_t>(wp_idx))};
        Rat reconstructed = a_collapsed
                          + (b_rat - c_rat) * half * (wm_rat - wp_rat);
        e = reconstructed;
    }
    return e;
}

}  // namespace hyperflint
