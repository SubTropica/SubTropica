// Phase 6b: period evaluators restricted to the MZV branch.

#include <algorithm>
#include "hyperflint/reduce/periods.hpp"

#include "hyperflint/reduce/period_scratch.hpp"  // period-tuples (binding fold)

#include "hyperflint/algebra/convert.hpp"        // convert_zero_one
#include "hyperflint/algebra/shuffle.hpp"        // collect_words
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/zw_table.hpp"          // iter-52 C0c.1 Increment β: ZWTable for transform_shuffle transient
#include "hyperflint/integrator/regularize.hpp"  // reg0, reg_head
#include "hyperflint/integrator/transform.hpp"   // transform_shuffle (6d-v-v)
#include "hyperflint/reduce/mzv_expansion.hpp"   // PHASE_2 iter 9: three-arm mint-site lookup
#include "hyperflint/runtime/narrow_ctx_flag.hpp"  // R24v2 tolerance branch

#include <atomic>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace hyperflint {

namespace {

std::string encode_index(long n) {
    if (n < 0) return "m" + std::to_string(-n);
    return std::to_string(n);
}

// Parse a letter's canonical string as an integer. Throws if the
// letter isn't a literal integer.
long letter_as_integer(const Rat& letter) {
    std::string s = letter.to_string();
    try {
        return std::stol(s);
    } catch (...) {
        throw std::runtime_error(
            "to_mzv: non-integer letter " + s);
    }
}

bool letter_is_integer(const Rat& letter) {
    std::string s = letter.to_string();
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') i = 1;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

bool word_is_all_zero(const Word& w) {
    if (w.empty()) return false;
    for (const auto& l : w.letters) {
        if (!l.is_zero()) return false;
    }
    return true;
}

bool letter_equals_int(const Rat& l, long k) {
    return l.to_string() == std::to_string(k);
}

// Convert a single word to its MZV-reduced Rat contribution per the
// ToMZV formula at HyperIntica.wl:3537.
Rat to_mzv_one_word(const PolyCtx& ctx, const Rat& coef, const Word& word,
                     const MzvExpansionTable* expansion) {
    // PHASE_2 iter 10: if explicit `expansion` parameter is nullptr,
    // fall back to the process-global active expansion (set by the
    // bridge entry RAII guard when HF_USE_BASIS_CTX=1). This avoids
    // the deep parameter cascade through break_up_contour → transform
    // → integration_step → hyper_int.
    if (expansion == nullptr) {
        expansion = get_active_mzv_expansion();
    }
    if (word.empty()) return coef;

    // Divergence guards.
    if (letter_equals_int(word[0], 1) ||
        word[word.size() - 1].is_zero()) {
        // Divergent — ToMZV prints a warning and emits
        // coef * Hlog[1, word]; we simply return 0 here and let the
        // caller's regularization handle it (Phase 6b's
        // zero_one_period regularizes first).
        return Rat::zero_of(ctx);
    }

    // Walk letters backwards, collapsing runs of zero into multiplicity
    // and non-zero letters into poles.
    std::vector<long> counts;
    std::vector<long> poles;
    for (long i = static_cast<long>(word.size()) - 1; i >= 0; --i) {
        if (word[static_cast<size_t>(i)].is_zero()) {
            if (!counts.empty()) counts.back()++;
        } else {
            if (!letter_is_integer(word[static_cast<size_t>(i)])) {
                throw std::runtime_error(
                    "to_mzv: non-integer letter in word");
            }
            counts.push_back(1);
            poles.push_back(letter_as_integer(word[static_cast<size_t>(i)]));
        }
    }
    if (counts.empty()) return Rat::zero_of(ctx);
    // Append 1 to the pole list for the index-shifting step.
    poles.push_back(1);

    // Indices: n_j = counts[j] * poles[j+1] / poles[j]  (all integer
    // given the {-1,0,1} scope).
    std::vector<long> indices;
    indices.reserve(counts.size());
    for (size_t j = 0; j < counts.size(); ++j) {
        long num = counts[j] * poles[j + 1];
        long den = poles[j];
        if (den == 0) {
            throw std::runtime_error("to_mzv: zero pole encountered");
        }
        if (num % den != 0) {
            // Non-integer index — happens for letters outside {-1,0,1}.
            throw std::runtime_error(
                "to_mzv: non-integer mzv index (letters outside {-1,0,1})");
        }
        indices.push_back(num / den);
    }

    // sign = (-1)^|counts|
    long sign = (counts.size() % 2 == 0) ? 1 : -1;

    // Build mzv_<enc> variable.
    std::ostringstream name;
    name << "mzv_";
    for (size_t j = 0; j < indices.size(); ++j) {
        if (j) name << "_";
        name << encode_index(indices[j]);
    }
    // Resolve mzv_<enc>. Three-arm lookup per design.md v2.1 §5.3:
    //
    //   arm 1 — basis-name in ctx: Poly::gen(ctx, basis_idx). The fast
    //     path; works for both wide-ctx (basis names live in ctx
    //     alongside LHS) and slim-ctx (basis names ARE the ctx vars).
    //   arm 2 — expansion-table hit: when arm 1 misses AND `expansion`
    //     is provided AND the mzv_name has an expansion, transfer the
    //     expansion's basis Rat into `ctx` via cross_ctx_transfer_rat.
    //     PHASE_2 iter 9 activation: this is the slim-ctx path.
    //   arm 3 — legacy fallback: Rat::parse(ctx, mzv_name) (with
    //     HF_PARSE_TOLERANT tolerance branch); preserves wide-ctx
    //     behaviour for any caller that hasn't migrated.
    //
    // When `expansion` is nullptr (default), arms 1+3 reproduce the
    // pre-iter-9 path bit-identically.
    const std::string mzv_name = name.str();
    const size_t mzv_idx = ctx.index_of(mzv_name);
    Rat mzv_var = [&]() -> Rat {
        // Arm 1: basis var present in ctx.
        if (mzv_idx != SIZE_MAX) {
            return Rat(Poly::gen(ctx, mzv_idx));
        }
        // Arm 2: expansion-table lookup (slim-ctx path; PHASE_2 iter 9).
        if (expansion != nullptr) {
            auto it = expansion->expansion.find(mzv_name);
            if (it != expansion->expansion.end()) {
                return cross_ctx_transfer_rat(it->second, ctx);
            }
            // expansion provided but mzv_name not in basis AND not in
            // expansion table: out-of-table mint. Cleaner failure than
            // the wide-ctx Rat::parse stacktrace.
            throw std::runtime_error(
                "to_mzv_one_word: MZV symbol '" + mzv_name +
                "' not in basis and not in expansion table "
                "(out-of-table mint; weight may exceed table coverage)");
        }
        // Arm 3: legacy fallback (wide-ctx callers without expansion).
        // R24 rev 2 / chain 17 tolerance branch.  Under
        // HF_PARSE_TOLERANT=1 a missing mzv var is converted to a
        // safe-failure zero + global narrow-ctx flag set, instead of
        // a `std::runtime_error` that escapes the OMP region as
        // `std::terminate`.  Default-off path is bit-identical to
        // pre-chain-17 behaviour (Rat::parse).
        if (tolerance_enabled()) {
            std::optional<Rat> parsed = Rat::parse_or_none(ctx, mzv_name);
            if (!parsed.has_value()) {
                g_narrow_ctx_too_narrow.store(true,
                                                std::memory_order_relaxed);
                return Rat::zero_of(ctx);
            }
            return std::move(*parsed);
        }
        return Rat::parse(ctx, mzv_name);
    }();
    Rat out = coef * mzv_var;
    if (sign < 0) out = -out;
    return out;
}

}  // namespace

Rat to_mzv(const PolyCtx& ctx, const Wordlist& wl,
           const MzvExpansionTable* expansion) {
    Rat acc{Poly::zero_of(ctx)};
    for (const auto& t : wl.terms) {
        acc = acc + to_mzv_one_word(ctx, t.coef, t.word, expansion);
    }
    return acc;
}

Rat zero_one_period(const PolyCtx& ctx,
                    const Word& word,
                    const MzvReductionTable& table,
                    const MzvExpansionTable* expansion) {
    if (word.empty()) return Rat::one_of(ctx);
    if (word_is_all_zero(word)) return Rat::zero_of(ctx);

    // Trailing-zero regularization via reg0.
    if (word[word.size() - 1].is_zero()) {
        Wordlist seed;
        seed.terms.push_back(WordlistTerm{Rat::one_of(ctx), word});
        Wordlist reg = reg0(seed);
        Rat acc{Poly::zero_of(ctx)};
        for (const auto& t : reg.terms) {
            acc = acc + t.coef * zero_one_period(ctx, t.word, table, expansion);
        }
        return acc;
    }

    // Leading-1 regularization via reg_head(letter=1).
    if (letter_equals_int(word[0], 1)) {
        Wordlist seed;
        seed.terms.push_back(WordlistTerm{Rat::one_of(ctx), word});
        Letter one{Poly::one_of(ctx)};
        Letter zero{Poly::zero_of(ctx)};
        Wordlist reg = reg_head(seed, one, zero);
        Rat acc{Poly::zero_of(ctx)};
        for (const auto& t : reg.terms) {
            acc = acc + t.coef * zero_one_period(ctx, t.word, table, expansion);
        }
        return acc;
    }

    // MZV branch (letters in {-1, 0, 1} — verified by letter check).
    for (const auto& l : word.letters) {
        if (!letter_is_integer(l)) {
            throw std::runtime_error(
                "zero_one_period: non-integer letter (Phase 6b scope)");
        }
        long v = letter_as_integer(l);
        if (v != -1 && v != 0 && v != 1) {
            throw std::runtime_error(
                "zero_one_period: letter outside {-1,0,1} "
                "(Phase 6b scope)");
        }
    }

    Wordlist seed;
    seed.terms.push_back(WordlistTerm{Rat::one_of(ctx), word});
    Rat mzv_expr = to_mzv(ctx, seed, expansion);
    return apply_mzv_reductions(ctx, table, mzv_expr);
}

Rat zero_inf_period(const PolyCtx& ctx,
                    const Word& word,
                    const MzvReductionTable& table,
                    const MzvExpansionTable* expansion) {
    if (word.empty()) return Rat::one_of(ctx);
    if (word_is_all_zero(word)) return Rat::zero_of(ctx);

    // Gather non-zero letters to classify the word.
    std::vector<long> nonzero;
    nonzero.reserve(word.size());
    bool any_zero = false;
    for (const auto& l : word.letters) {
        if (l.is_zero()) { any_zero = true; continue; }
        if (!letter_is_integer(l)) {
            throw std::runtime_error(
                "zero_inf_period: non-integer letter (Phase 6c scope)");
        }
        nonzero.push_back(letter_as_integer(l));
    }
    (void)any_zero;

    // All-(-1) word regularizes to 0 (HyperIntica:5249). This requires
    // both that there are no zero letters AND every letter equals -1.
    if (!any_zero && !nonzero.empty()) {
        bool all_minus_one = true;
        for (long v : nonzero) if (v != -1) { all_minus_one = false; break; }
        if (all_minus_one) return Rat::zero_of(ctx);
    }

    // Trailing-zero regularization.
    if (word[word.size() - 1].is_zero()) {
        Wordlist seed;
        seed.terms.push_back(WordlistTerm{Rat::one_of(ctx), word});
        Wordlist reg = reg0(seed);
        Rat acc{Poly::zero_of(ctx)};
        for (const auto& t : reg.terms) {
            acc = acc + t.coef * zero_inf_period(ctx, t.word, table, expansion);
        }
        return acc;
    }

    // Letters in {-1, 0}: use ConvertZeroOne -> ZeroOnePeriod.
    bool letters_in_m1_0 = true;
    for (long v : nonzero) {
        if (v != -1) { letters_in_m1_0 = false; break; }
    }
    if (letters_in_m1_0) {
        Wordlist seed;
        seed.terms.push_back(WordlistTerm{Rat::one_of(ctx), word});
        Wordlist converted = convert_zero_one(seed);
        Rat acc{Poly::zero_of(ctx)};
        for (const auto& t : converted.terms) {
            acc = acc + t.coef * zero_one_period(ctx, t.word, table, expansion);
        }
        return acc;
    }

    // Single distinct nonzero letter: rescale path
    // (ZeroInfPeriodRescale; HyperIntica.wl:3643).  We support only
    // scale=2 here (the transcendental Log[scale] must live in our
    // basis; only Log2 does). Other scales throw.
    {
        std::vector<long> unique_nonzero = nonzero;
        std::sort(unique_nonzero.begin(), unique_nonzero.end());
        unique_nonzero.erase(std::unique(unique_nonzero.begin(),
                                          unique_nonzero.end()),
                              unique_nonzero.end());
        if (unique_nonzero.size() == 1) {
            long letter = unique_nonzero[0];
            long scale = -letter;
            if (scale != 2) {
                throw std::runtime_error(
                    "zero_inf_period: single-letter rescale requires "
                    "scale=2 (letter=-2); got letter=" +
                    std::to_string(letter));
            }
            // u = word / scale: 0->0, -2->-1.
            Word u;
            u.letters.reserve(word.size());
            for (const auto& l : word.letters) {
                if (l.is_zero()) {
                    u.letters.push_back(Rat::zero_of(ctx));
                } else {
                    long v = letter_as_integer(l);
                    long uv = v / scale;
                    u.letters.push_back(Rat::from_int(ctx, uv));
                }
            }
            const size_t log2_idx = ctx.index_of("Log2");
            Rat neg_log2 = (log2_idx == SIZE_MAX)
                ? -Rat::parse(ctx, "Log2")
                : -Rat(Poly::gen(ctx, log2_idx));
            Rat acc{Poly::zero_of(ctx)};
            Rat logFac{Poly::one_of(ctx)};   // (-Log2)^0 / 0! = 1
            const long n = static_cast<long>(word.size());
            for (long k = 0; k <= n; ++k) {
                Word tail;
                tail.letters.assign(u.letters.begin() + k, u.letters.end());
                Rat period = zero_inf_period(ctx, tail, table, expansion);
                acc = acc + logFac * period;
                if (k < n) {
                    Rat divisor = Rat::from_int(ctx, static_cast<long>(k + 1));
                    logFac = (logFac * neg_log2) / divisor;
                }
            }
            return acc;
        }

        // Two-letter case: nonzero ⊆ {-2, -1}, ratio = 1/2, scale = 1
        // branch (HyperIntica.wl:3617). Shift each letter by +1 and
        // route through Convert1InfTo01 + ZeroOnePeriod.
        if (unique_nonzero.size() == 2 &&
            unique_nonzero[0] == -2 && unique_nonzero[1] == -1) {
            Word shifted;
            shifted.letters.reserve(word.size());
            for (const auto& l : word.letters) {
                if (l.is_zero()) {
                    shifted.letters.push_back(Rat::one_of(ctx));
                } else {
                    long v = letter_as_integer(l);
                    shifted.letters.push_back(Rat{Poly(ctx,
                        std::to_string(v + 1))});
                }
            }
            Wordlist seed;
            seed.terms.push_back(
                WordlistTerm{Rat::one_of(ctx), shifted});
            Wordlist converted = convert_1inf_to_01(seed);
            Rat acc{Poly::zero_of(ctx)};
            for (const auto& t : converted.terms) {
                acc = acc + t.coef * zero_one_period(ctx, t.word, table, expansion);
            }
            return acc;
        }
    }

    throw std::runtime_error(
        "zero_inf_period: letters outside Phase-6d scope "
        "({0, -1, -2} with limited combinations)");
}

Regulator evaluate_periods(const PolyCtx& ctx,
                            const Regulator& r,
                            const MzvReductionTable& table) {
    Rat const_val{Poly::zero_of(ctx)};
    Regulator passthrough;
    for (const auto& term : r) {
        if (term.key.empty()) {
            const_val = const_val + term.coef;
            continue;
        }
        bool all_evaluable = true;
        Rat prod{Poly::one_of(ctx)};
        for (const auto& word : term.key) {
            try {
                Rat val = zero_inf_period(ctx, word, table);
                prod = prod * val;
            } catch (const std::exception&) {
                all_evaluable = false;
                break;
            }
        }
        if (all_evaluable) {
            const_val = const_val + term.coef * prod;
        } else {
            passthrough.push_back(term);
        }
    }

    Regulator out;
    if (!const_val.is_zero()) {
        out.push_back(RegTerm{const_val, RegKey{}});
    }
    for (auto& t : passthrough) {
        out.push_back(std::move(t));
    }
    return canonicalize_regulator(out);
}

// Phase 6d-v-v: FibrationBasis.
//
// Port of HyperIntica.wl:5051-5127 (FibrationBasisRecurse) +
// HyperInt.mpl:1692-1744.
//
// The algorithm is a recursive descent on the `vars` list. At each
// level we split the input Regulator (a sum over "shuffle keys" of
// Words) by integrating out one variable via `transform_shuffle`,
// which produces (shuffle-part, regulator-part) pairs. The shuffle-
// part becomes a prefix list; the regulator-part recurses with
// one fewer variable. At the bottom (vars == []) we evaluate the
// remaining words as periods via zero_inf_period and unfold the
// accumulated prefix Cartesian product.
//
// Data shape: the internal FibBasisMap maps a key (vector<Word> of
// length vars.size()) to a Rat coefficient; we fold additions via
// canonical string equality since Rat doesn't implement hashing.
namespace {

// Rat factories in ctx.
Rat fb_rat_zero(const PolyCtx& ctx) { return Rat::zero_of(ctx); }
Rat fb_rat_one (const PolyCtx& ctx) { return Rat::one_of(ctx); }

// String canonical key for a vector<Word>.
std::string fib_key_string(const std::vector<Word>& key) {
    std::ostringstream o;
    for (const auto& w : key) {
        o << "[";
        for (size_t i = 0; i < w.size(); ++i) {
            if (i) o << ",";
            o << w[i].to_string();
        }
        o << "]\x01";
    }
    return o.str();
}

// Add (coef, key) to the accumulator (maintaining key→index and
// key→coef), folding duplicate keys.
struct FibBasisAcc {
    std::vector<std::pair<std::vector<Word>, Rat>> entries;
    std::unordered_map<std::string, size_t>        index;
    void add(std::vector<Word> key, const Rat& coef) {
        if (coef.is_zero()) return;
        std::string k = fib_key_string(key);
        auto it = index.find(k);
        if (it == index.end()) {
            index[k] = entries.size();
            entries.emplace_back(std::move(key), coef);
        } else {
            entries[it->second].second = entries[it->second].second + coef;
        }
    }
};

// Compute `val` = sum_{w in wordlist} w.coef * product_{u in w.key}
// ZeroInfPeriod(u), or throw if any u is unevaluable.
// Returns nullopt-equivalent when the product doesn't reduce cleanly;
// we signal via a `evaluable` out-param so the caller can fall back.
Rat base_case_val(const PolyCtx& ctx, const Regulator& wordlist,
                   const MzvReductionTable& table, bool& evaluable) {
    Rat val = fb_rat_zero(ctx);
    evaluable = true;
    for (const auto& w : wordlist) {
        Rat prod = fb_rat_one(ctx);
        for (const auto& u : w.key) {
            if (u.empty()) continue;
            try {
                Rat p = zero_inf_period(ctx, u, table);
                prod = prod * p;
            } catch (const std::exception&) {
                evaluable = false;
                return fb_rat_zero(ctx);
            }
        }
        val = val + w.coef * prod;
    }
    return val;
}

// The recursive worker. Base case when `vars_rem` is empty.
// `prefix` is a list of Wordlist (one entry per var already
// processed); each Wordlist's terms are the shuffle-key alternatives
// for that var.
void fib_recurse(const PolyCtx& ctx,
                 const Regulator& wordlist,
                 const std::vector<size_t>& vars_rem,
                 const std::vector<Wordlist>& prefix,
                 const Rat& value_factor,
                 const MzvReductionTable& table,
                 FibBasisAcc& acc) {
    if (vars_rem.empty()) {
        bool evaluable;
        Rat val = base_case_val(ctx, wordlist, table, evaluable);
        if (!evaluable) {
            // Keep the term symbolically by pinning it into a
            // single key whose Words are the full regulator keys'
            // concatenation. For our Phase 6d-v-v scope this can't
            // happen on the fixtures we've chosen; flag loudly.
            throw std::runtime_error(
                "fibration_basis: base-case period not evaluable under "
                "Phase-6d zero_inf_period; caller must lift the "
                "variable through `var_indices` or pre-reduce");
        }
        val = val * value_factor;

        const size_t n_pref = prefix.size();
        if (n_pref == 0) {
            acc.add({}, val);
            return;
        }

        std::vector<size_t> sizes(n_pref);
        for (size_t i = 0; i < n_pref; ++i) {
            sizes[i] = prefix[i].terms.size();
            if (sizes[i] == 0) return;
        }

        std::vector<size_t> counter(n_pref, 0);
        while (true) {
            std::vector<Word> key;
            key.reserve(n_pref);
            Rat contrib = val;
            for (size_t i = 0; i < n_pref; ++i) {
                key.push_back(prefix[i].terms[counter[i]].word);
                contrib = contrib * prefix[i].terms[counter[i]].coef;
            }
            acc.add(std::move(key), contrib);

            // Increment Cartesian counter.
            size_t i = 0;
            while (i < n_pref) {
                if (counter[i] + 1 >= sizes[i]) {
                    counter[i] = 0;
                    ++i;
                } else {
                    ++counter[i];
                    break;
                }
            }
            if (i >= n_pref) break;
        }
        return;
    }

    // Recursive case: integrate out vars_rem[0] via transform_shuffle.
    size_t first = vars_rem[0];
    std::vector<size_t> rest(vars_rem.begin() + 1, vars_rem.end());
    // Iter-52 C0c.1 Increment β: caller-side fresh transient ZWTable for
    // the new mandatory transform_shuffle `zw_tab` parameter (Option A).
    // The fibration-basis driver runs serially in the divergence-check
    // pass (post-OMP-merge), and its ZWTable does not need to persist
    // across the wrapping integration_step's main pass — a per-call
    // transient suffices.
    auto _lf_zw = std::make_shared<ZWTable>(ctx);
    for (const auto& w : wordlist) {
        // w.key is a vector<Word> interpreted as a shuffle product.
        TransformResultSym tr = transform_shuffle(ctx, w.key, first, table,
                                                    _lf_zw);
        for (const auto& pair : tr) {
            // pair.shuffle is a Wordlist (a sum of coef * Word).
            // pair.regulator is a RegulatorSym (Bug #6 lift); demote
            // to Regulator for the inner recursion since
            // fibration_basis only evaluates Rat-pure periods.
            Regulator inner;
            inner.reserve(pair.regulator.size());
            for (const auto& t : pair.regulator) {
                if (!t.coef.is_rat()) {
                    throw std::runtime_error(
                        "fibration_basis: transform_shuffle emitted a "
                        "non-Rat SymCoef (I*Pi*delta residue). The "
                        "fibration-basis path currently only supports "
                        "Rat-pure regulators.");
                }
                inner.push_back(RegTerm{t.coef.as_rat(), t.key});
            }
            std::vector<Wordlist> new_prefix = prefix;
            new_prefix.push_back(pair.shuffle);
            Rat new_vf = value_factor * w.coef;
            fib_recurse(ctx, inner, rest, new_prefix, new_vf,
                        table, acc);
        }
    }
}

}  // namespace

FibrationBasisResult
fibration_basis(const PolyCtx& ctx,
                 const Regulator& input,
                 const std::vector<size_t>& var_indices,
                 const MzvReductionTable& table) {
    FibBasisAcc acc;
    fib_recurse(ctx, input, var_indices, {}, fb_rat_one(ctx), table, acc);

    FibrationBasisResult out;
    out.vars.reserve(var_indices.size());
    for (size_t i : var_indices) {
        out.vars.push_back(ctx.vars()[i]);
    }
    for (auto& e : acc.entries) {
        if (e.second.is_zero()) continue;
        out.terms.emplace_back(std::move(e.first), std::move(e.second));
    }
    return out;
}

Rat test_zero_function(const PolyCtx& ctx,
                        const Regulator& r,
                        const MzvReductionTable& table) {
    // Phase 6e stub: reduce via evaluate_periods, then sum all
    // surviving coefs. Distinct non-empty regulator keys are assumed
    // independent over the basis — a simplification that misses
    // fibration-basis-level identities (those would require the full
    // FibrationBasis port).
    Regulator reduced = evaluate_periods(ctx, r, table);
    Rat total{Poly::zero_of(ctx)};
    for (const auto& t : reduced) {
        total = total + t.coef;
    }
    return total;
}

// --- Phase 6d-v-v-ii: SymCoef-valued fibration_basis and zero-test ---

namespace {

// SymCoef analog of FibBasisAcc. Keys are vector<Word>; coef is
// SymCoef (accumulated via SymCoef addition, which auto-canonicalizes
// and drops zero prefactors).
struct FibBasisAccSym {
    std::vector<std::pair<std::vector<Word>, SymCoef>> entries;
    std::unordered_map<std::string, size_t>            index;
    void add(std::vector<Word> key, const SymCoef& coef) {
        if (coef.is_zero()) return;
        std::string k = fib_key_string(key);
        auto it = index.find(k);
        if (it == index.end()) {
            index[k] = entries.size();
            entries.emplace_back(std::move(key), coef);
        } else {
            entries[it->second].second =
                entries[it->second].second + coef;
        }
    }
};

// Base-case evaluator for fibration_basis_sym: attempt to reduce
// each remaining regulator entry's key (list of Words) via
// zero_inf_period. If every leaf reduces, accumulate the product as
// a Rat and multiply into the entry's SymCoef; otherwise emit the
// term symbolically (pin as an unreducible Word-key with its
// original SymCoef).
//
// Per adversarial review gap 1: the sym version MUST NOT rethrow —
// it is the passthrough layer for fibration_basis's Rat-path throw,
// so it has to produce a symbolic output when periods can't
// evaluate.
void base_case_sym(const PolyCtx& ctx,
                    const RegulatorSym& wordlist,
                    const SymCoef& value_factor,
                    const std::vector<Wordlist>& prefix,
                    const MzvReductionTable& table,
                    FibBasisAccSym& acc) {
    const size_t n_pref = prefix.size();

    // Precompute prefix sizes once for the Cartesian-counter loop.
    std::vector<size_t> sizes(n_pref);
    for (size_t i = 0; i < n_pref; ++i) {
        sizes[i] = prefix[i].terms.size();
        if (sizes[i] == 0) return;  // empty alternative set => zero contrib
    }

    auto emit = [&](std::vector<Word> extra_key,
                     const SymCoef& extra_coef) {
        if (n_pref == 0) {
            acc.add(std::move(extra_key), extra_coef);
            return;
        }
        std::vector<size_t> counter(n_pref, 0);
        while (true) {
            std::vector<Word> key = extra_key;
            key.reserve(extra_key.size() + n_pref);
            SymCoef contrib = extra_coef;
            for (size_t i = 0; i < n_pref; ++i) {
                key.push_back(prefix[i].terms[counter[i]].word);
                contrib = contrib.mul_rat(prefix[i].terms[counter[i]].coef);
            }
            acc.add(std::move(key), contrib);
            // Increment Cartesian counter.
            size_t i = 0;
            while (i < n_pref) {
                if (counter[i] + 1 >= sizes[i]) { counter[i] = 0; ++i; }
                else                            { ++counter[i]; break; }
            }
            if (i >= n_pref) break;
        }
    };

    for (const auto& w : wordlist) {
        // w.coef is SymCoef; combine with value_factor once.
        SymCoef entry_sym = value_factor.mul(w.coef);

        // Try to reduce every Word in the regulator key to a period.
        // If all reduce, fold into a single Rat product and emit at
        // the empty-extra-key slot. Else, emit the entry symbolically
        // with its full key (shifted to the "right" of the prefix).
        // Period-tuples Phase 2 fix (binding review fold, 2026-06-04):
        // under the slim ctx, zero_inf_period cannot reduce into a Rat
        // (atom vars absent -> arm-3 parse throw would mis-pin the term
        // symbolically and could change the divergence verdict). Mint
        // via the scratch ring and fold with SymCoef::mul instead --
        // exact parity with the wide path's reduce-and-fold semantics.
        if (period_tuples_enabled()) {
            SymCoef folded = entry_sym;
            bool evaluable = true;
            for (const auto& u : w.key) {
                if (u.empty()) continue;
                try {
                    folded = folded.mul(
                        mint_period_sym(ctx, u, table, /*zero_one=*/false));
                } catch (const std::exception&) {
                    evaluable = false;
                    break;
                }
            }
            if (evaluable) {
                emit(std::vector<Word>{}, folded);
            } else {
                emit(w.key, entry_sym);
            }
            continue;
        }
        Rat period_prod = Rat::one_of(ctx);
        bool evaluable = true;
        for (const auto& u : w.key) {
            if (u.empty()) continue;
            try {
                Rat p = zero_inf_period(ctx, u, table);
                period_prod = period_prod * p;
            } catch (const std::exception&) {
                evaluable = false;
                break;
            }
        }
        if (evaluable) {
            SymCoef folded = entry_sym.mul_rat(period_prod);
            emit(std::vector<Word>{}, folded);
        } else {
            // Pin the regulator key's Words into the result key
            // (append after the prefix slots so the shape stays
            // `prefix-cross-product` × `passthrough-key`). Different
            // regulator entries produce different symbolic keys.
            emit(w.key, entry_sym);
        }
    }
}

void fib_recurse_sym(const PolyCtx& ctx,
                     const RegulatorSym& wordlist,
                     const std::vector<size_t>& vars_rem,
                     const std::vector<Wordlist>& prefix,
                     const SymCoef& value_factor,
                     const MzvReductionTable& table,
                     FibBasisAccSym& acc) {
    if (vars_rem.empty()) {
        base_case_sym(ctx, wordlist, value_factor, prefix, table, acc);
        return;
    }

    const size_t first = vars_rem[0];
    const std::vector<size_t> rest(vars_rem.begin() + 1, vars_rem.end());
    // Iter-52 C0c.1 Increment β: caller-side fresh transient ZWTable
    // (same rationale as fib_recurse above).
    auto _lf_zw = std::make_shared<ZWTable>(ctx);
    for (const auto& w : wordlist) {
        TransformResultSym tr = transform_shuffle(ctx, w.key, first, table,
                                                    _lf_zw);
        for (const auto& pair : tr) {
            // Accumulate: prefix += [pair.shuffle], value_factor *= w.coef.
            std::vector<Wordlist> new_prefix = prefix;
            new_prefix.push_back(pair.shuffle);
            SymCoef new_vf = value_factor.mul(w.coef);
            fib_recurse_sym(ctx, pair.regulator, rest, new_prefix,
                             new_vf, table, acc);
        }
    }
}

}  // namespace

FibrationBasisResultSym
fibration_basis_sym(const PolyCtx& ctx,
                     const RegulatorSym& input,
                     const std::vector<size_t>& var_indices,
                     const MzvReductionTable& table) {
    // Performance guard (adversarial-review flag): fibration on deep
    // multi-variable inputs is combinatorially expensive. This is a
    // warning only; the user asked to run it, we honor that.
    if (var_indices.size() >= 5 && input.size() > 100) {
        std::cerr << "fibration_basis_sym: warning — var_indices.size()="
                  << var_indices.size() << " with input.size()="
                  << input.size() << "; runtime may be combinatorial.\n";
    }

    FibBasisAccSym acc;
    const SymCoef unit = SymCoef::from_rat(Rat::one_of(ctx));
    fib_recurse_sym(ctx, input, var_indices, {}, unit, table, acc);

    FibrationBasisResultSym out;
    out.vars.reserve(var_indices.size());
    for (size_t i : var_indices) {
        out.vars.push_back(ctx.vars()[i]);
    }
    out.terms.reserve(acc.entries.size());
    for (auto& e : acc.entries) {
        if (e.second.is_zero()) continue;
        out.terms.emplace_back(std::move(e.first), std::move(e.second));
    }
    return out;
}

bool test_zero_function_sym(const PolyCtx&                  ctx,
                             const RegulatorSym&             r,
                             const std::vector<size_t>&      var_indices,
                             const MzvReductionTable&        table) {
    // A regulator-sym is algebraically zero iff, after projecting to
    // the full fibration basis, every term's SymCoef coefficient is
    // the zero SymCoef. Distinct fibration-basis keys are linearly
    // independent over the transcendental basis, so the per-term
    // check is both necessary and sufficient.
    FibrationBasisResultSym fb = fibration_basis_sym(ctx, r, var_indices, table);
    for (const auto& t : fb.terms) {
        if (!t.second.is_zero()) return false;
    }
    return true;
}

}  // namespace hyperflint
