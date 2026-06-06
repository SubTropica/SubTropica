// HF basis-ctx campaign — PHASE_1 unit tests for MzvExpansionTable.
//
// Iter 4: test 1a (production table flatness + load-time invariants
//   + basic shape of expansion map).  [DONE]
// Iter 5: tests 1b-benign + 1b-adversarial (synthetic chained
//   fixtures exercising Rat-level substitution; R-4 LOCK).  [DONE]
// Iter 6: test 1c (algebraic spot-checks against Euler / Zagier
//   identities; A-12 fold). Reference table at
//   test/data/mzv_basis_reference_values.json.  [DONE]
//
// Design: notes/hf_mzv_weight_cap_2026-05-28/design.md §7.2 gate 1.

#include "hyperflint/reduce/mzv_expansion.hpp"
#include "hyperflint/reduce/mzv_reduce.hpp"
#include "hyperflint/reduce/periods.hpp"
#include "hyperflint/symbols/word.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace hyperflint;

static int g_failures = 0;

static void check(bool cond, const char* label) {
    if (cond) {
        std::printf("[PASS] %s\n", label);
    } else {
        std::printf("[FAIL] %s\n", label);
        ++g_failures;
    }
}

// CMake-injected paths:
//   HF_PROD_DATA_DIR -> HyperFLINT/data/    (production tables)
//   HF_TEST_DATA_DIR -> HyperFLINT/test/data/ (synthetic chained fixtures)
#ifndef HF_PROD_DATA_DIR
#  define HF_PROD_DATA_DIR ""
#endif
#ifndef HF_TEST_DATA_DIR
#  define HF_TEST_DATA_DIR ""
#endif

int main() {
    const std::string production_json =
        std::string(HF_PROD_DATA_DIR) + "/mzv_reductions.json";

    // --- Test 1a-1: weight_of_mzv_name spot-checks --------------------------
    check(weight_of_mzv_name("Log2") == 1, "weight(Log2) == 1");
    check(weight_of_mzv_name("mzv_2") == 2, "weight(mzv_2) == 2");
    check(weight_of_mzv_name("mzv_2_3") == 5, "weight(mzv_2_3) == 5");
    check(weight_of_mzv_name("mzv_m1") == 1, "weight(mzv_m1) == 1 (alt)");
    check(weight_of_mzv_name("mzv_m2_m3") == 5, "weight(mzv_m2_m3) == 5");
    check(weight_of_mzv_name("mzv_1_1_1_m3") == 6,
          "weight(mzv_1_1_1_m3) == 6");
    check(weight_of_mzv_name("mzv_m1_1_1_m1_m1_m1") == 6,
          "weight(mzv_m1_1_1_m1_m1_m1) == 6 (worst-case expansion LHS)");
    check(weight_of_mzv_name("foo") == -1, "weight(foo) == -1 (non-mzv)");
    check(weight_of_mzv_name("mzv_") == -1, "weight(mzv_) == -1 (no digits)");
    check(weight_of_mzv_name("mzv_x") == -1,
          "weight(mzv_x) == -1 (non-digit)");

    // --- Test 1a-2: looks_like_mzv ------------------------------------------
    check(looks_like_mzv("Log2"), "looks_like_mzv(Log2)");
    check(looks_like_mzv("mzv_2_3"), "looks_like_mzv(mzv_2_3)");
    check(!looks_like_mzv("foo"), "!looks_like_mzv(foo)");
    check(!looks_like_mzv("Log20"),
          "!looks_like_mzv(Log20)  -- pure substring of Log2 boundary");

    // --- Test 1a-3: tokens_in extraction ------------------------------------
    auto toks = tokens_in("3*mzv_2 + 1/2*Log2^2 - mzv_3");
    check(toks.size() == 3, "tokens_in(...) -> 3 unique tokens");
    check(std::find(toks.begin(), toks.end(), "mzv_2") != toks.end(),
          "tokens_in finds mzv_2");
    check(std::find(toks.begin(), toks.end(), "Log2") != toks.end(),
          "tokens_in finds Log2");
    check(std::find(toks.begin(), toks.end(), "mzv_3") != toks.end(),
          "tokens_in finds mzv_3");

    // --- Test 1a-4: load_mzv_expansion on production JSON -------------------
    // Per MF-2(i) production-flatness invariant, the production table
    // must load with allow_chained=false (default). If the upstream
    // mzv_reductions.json ever changes to chained form, this test
    // catches it.
    MzvExpansionTable exp;
    try {
        exp = load_mzv_expansion(production_json);
        check(true, "load_mzv_expansion(production) succeeds without throw");
    } catch (const std::exception& e) {
        std::printf("[FAIL] load_mzv_expansion(production) threw: %s\n",
                    e.what());
        ++g_failures;
        return g_failures;  // can't continue
    }

    // --- Test 1a-5: basis shape ---------------------------------------------
    // 10 basis names: Log2 + 9 MZVs.
    check(exp.basis_names.size() == 10,
          "basis has 10 names (Log2 + 9 MZVs)");
    check(exp.basis_names[0] == "Log2",
          "basis[0] == 'Log2' (contiguity-invariant tail anchor)");
    check(exp.basis_idx.size() == exp.basis_names.size(),
          "basis_idx has one entry per basis name");
    check(exp.basis_ctx != nullptr, "basis_ctx is non-null");
    check(exp.basis_ctx->vars().size() == exp.basis_names.size(),
          "basis_ctx width == 10");

    // --- Test 1a-6: expansion-map shape -------------------------------------
    // 700 rules in production table => 700 expansion entries.
    check(exp.expansion.size() == 700,
          "expansion has 700 entries (one per production rule)");

    // Divergent ζ(1,…,1) rules: 8 entries with zero expansion.
    int divergent_zero_count = 0;
    int rules_seen = 0;
    for (const auto& kv : exp.expansion) {
        const std::string& name = kv.first;
        // Match mzv_1_1_..._1 with all-1 indices
        if (name.compare(0, 4, "mzv_") == 0) {
            bool all_ones = true;
            size_t i = 4;
            while (i < name.size()) {
                if (name[i] != '1') { all_ones = false; break; }
                ++i;
                if (i < name.size() && name[i] == '_') ++i;
            }
            if (all_ones && i == name.size()) {
                // Check whether the expansion is zero.
                if (kv.second.num().is_zero()) {
                    ++divergent_zero_count;
                }
            }
        }
        ++rules_seen;
    }
    check(divergent_zero_count == 8,
          "8 divergent mzv_1...1 rules expand to zero Rat (R-1 fold)");
    check(rules_seen == 700, "iterated over all 700 expansion entries");

    // --- Test 1a-7: build_basis_var_list contiguity invariant ---------------
    std::vector<std::string> user_vars = {"t1", "t2", "t3", "t4", "t5"};
    auto vlist = build_basis_var_list(exp, user_vars);
    check(vlist.size() == user_vars.size() + exp.basis_names.size(),
          "build_basis_var_list: user_vars + basis_names");
    // basis sub-block must appear contiguously at the tail in the same
    // order as exp.basis_names.
    bool tail_contiguous = true;
    for (size_t i = 0; i < exp.basis_names.size(); ++i) {
        if (vlist[user_vars.size() + i] != exp.basis_names[i]) {
            tail_contiguous = false;
            break;
        }
    }
    check(tail_contiguous,
          "build_basis_var_list: basis sub-block contiguous at tail (A-1)");

    // --- Test 1b-benign: chained-rule fixture, no operator-precedence stress -
    // Fixture: mzv_4 = 5*mzv_2^2 (flat),
    //          mzv_6 = mzv_4 * mzv_2 (chained)
    // Expected expansion[mzv_6] = 5*mzv_2^3.
    {
        const std::string benign_path =
            std::string(HF_TEST_DATA_DIR) +
            "/mzv_reductions_chained_test.json";
        MzvExpansionTable benign;
        try {
            // allow_chained=true because the fixture is deliberately
            // non-flat per MF-2(ii).
            benign = load_mzv_expansion(benign_path, /*allow_chained=*/true);
            check(true, "load_mzv_expansion(benign chained) succeeds");
        } catch (const std::exception& e) {
            std::printf("[FAIL] load_mzv_expansion(benign) threw: %s\n",
                        e.what());
            ++g_failures;
        }
        check(benign.expansion.size() == 2,
              "benign expansion has 2 entries (mzv_4 + mzv_6)");
        auto mzv4_it = benign.expansion.find("mzv_4");
        auto mzv6_it = benign.expansion.find("mzv_6");
        check(mzv4_it != benign.expansion.end(),
              "benign: expansion has mzv_4");
        check(mzv6_it != benign.expansion.end(),
              "benign: expansion has mzv_6");
        // Build reference 5*mzv_2^3 in the same basis_ctx.
        if (mzv6_it != benign.expansion.end()) {
            Rat expected = Rat::parse(*benign.basis_ctx, "5*mzv_2^3");
            // String compare on canonical form (both Rats in same ctx).
            check(mzv6_it->second.to_string() == expected.to_string(),
                  "benign: expansion[mzv_6] == 5*mzv_2^3");
        }
        if (mzv4_it != benign.expansion.end()) {
            Rat expected = Rat::parse(*benign.basis_ctx, "5*mzv_2^2");
            check(mzv4_it->second.to_string() == expected.to_string(),
                  "benign: expansion[mzv_4] == 5*mzv_2^2 (flat path)");
        }
    }

    // --- Test 1b-adversarial: R-4 fixture; unary-minus + exponent ----------
    // Fixture: mzv_4 = mzv_2*Log2^2 - mzv_2^2 (flat, weight 4),
    //          mzv_8 = -mzv_4^2 (chained, weight 8, unary minus + exponent)
    // Expected expansion[mzv_8] =
    //   -(mzv_2*Log2^2 - mzv_2^2)^2
    //   = -mzv_2^2*Log2^4 + 2*mzv_2^3*Log2^2 - mzv_2^4
    //
    // Under R-4-REJECTED TEXTUAL substitution, the textual replacement of
    // mzv_4 -> "mzv_2*Log2^2 - mzv_2^2" into "-mzv_4^2" would produce
    // "-mzv_2*Log2^2 - mzv_2^2^2" which the parser reads as
    // -(mzv_2*Log2^2) - (mzv_2^2)^2 = -mzv_2*Log2^2 - mzv_2^4
    // — the wrong answer. The Rat-level substitution path produces the
    // correct expansion above. THIS TEST FAILS LOUDLY if the loader
    // regresses to textual substitution.
    {
        const std::string adv_path =
            std::string(HF_TEST_DATA_DIR) +
            "/mzv_reductions_chained_adversarial.json";
        MzvExpansionTable adv;
        try {
            adv = load_mzv_expansion(adv_path, /*allow_chained=*/true);
            check(true, "load_mzv_expansion(adversarial chained) succeeds");
        } catch (const std::exception& e) {
            std::printf("[FAIL] load_mzv_expansion(adversarial) threw: %s\n",
                        e.what());
            ++g_failures;
        }
        check(adv.expansion.size() == 2,
              "adversarial expansion has 2 entries (mzv_4 + mzv_8)");
        auto mzv4_it = adv.expansion.find("mzv_4");
        auto mzv8_it = adv.expansion.find("mzv_8");
        check(mzv4_it != adv.expansion.end(),
              "adversarial: expansion has mzv_4");
        check(mzv8_it != adv.expansion.end(),
              "adversarial: expansion has mzv_8");
        if (mzv4_it != adv.expansion.end()) {
            Rat expected = Rat::parse(*adv.basis_ctx,
                                       "mzv_2*Log2^2 - mzv_2^2");
            check(mzv4_it->second.to_string() == expected.to_string(),
                  "adversarial: expansion[mzv_4] == mzv_2*Log2^2 - mzv_2^2");
        }
        if (mzv8_it != adv.expansion.end()) {
            // Rat-level substitution must produce
            //   -(mzv_2*Log2^2 - mzv_2^2)^2
            // = -mzv_2^2*Log2^4 + 2*mzv_2^3*Log2^2 - mzv_2^4
            Rat expected = Rat::parse(*adv.basis_ctx,
                "-mzv_2^2*Log2^4 + 2*mzv_2^3*Log2^2 - mzv_2^4");
            const bool match =
                (mzv8_it->second.to_string() == expected.to_string());
            check(match,
                  "adversarial (R-4 LOCK): expansion[mzv_8] = "
                  "-(mzv_4)^2 expanded correctly via Rat-level substitution");
            if (!match) {
                std::printf("  got:      %s\n",
                            mzv8_it->second.to_string().c_str());
                std::printf("  expected: %s\n",
                            expected.to_string().c_str());
            }
        }
    }

    // --- Test 1c: algebraic spot-checks (Euler / Zagier identities) -------
    // A-12 fold: validate the production expansion against well-known
    // closed-form MZV identities. Algebraic (canonical-string) comparison
    // is dependency-free and stronger than numerical evaluation: any
    // mismatch indicates either the upstream table has a wrong RHS, or
    // the loader's expansion/canonicalization has a bug.
    //
    // Source identities documented in test/data/mzv_basis_reference_values.json
    // (committed alongside this test as audit/provenance record).
    struct Identity {
        const char* lhs;
        const char* expected_rhs;
        const char* label;
    };
    const Identity identities[] = {
        {"mzv_4",  "(2/5)*mzv_2^2",
         "Euler: zeta(4) = (2/5)*zeta(2)^2"},
        {"mzv_6",  "(8/35)*mzv_2^3",
         "Euler: zeta(6) = (8/35)*zeta(2)^3"},
        {"mzv_8",  "(24/175)*mzv_2^4",
         "Euler: zeta(8) = (24/175)*zeta(2)^4"},
        {"mzv_2_2", "(3/10)*mzv_2^2",
         "Stuffle: zeta(2,2) = (3/10)*zeta(2)^2"},
        {"mzv_2_3", "3*mzv_2*mzv_3 - (11/2)*mzv_5",
         "Zagier depth-2: zeta(2,3) = 3*zeta(2)*zeta(3) - (11/2)*zeta(5)"},
        {"mzv_3_2", "-2*mzv_2*mzv_3 + (9/2)*mzv_5",
         "Zagier depth-2: zeta(3,2) = -2*zeta(2)*zeta(3) + (9/2)*zeta(5)"},
    };
    for (const auto& id : identities) {
        auto it = exp.expansion.find(id.lhs);
        if (it == exp.expansion.end()) {
            std::printf("[FAIL] expansion missing for %s\n", id.lhs);
            ++g_failures;
            continue;
        }
        Rat expected = Rat::parse(*exp.basis_ctx, id.expected_rhs);
        const bool match = (it->second.to_string() == expected.to_string());
        std::string label = std::string(id.lhs) + " == " + id.expected_rhs +
                            "  (" + id.label + ")";
        check(match, label.c_str());
        if (!match) {
            std::printf("  got:      %s\n", it->second.to_string().c_str());
            std::printf("  expected: %s\n", expected.to_string().c_str());
        }
    }

    // --- Test 1b-flatness-invariant ----------------------------------------
    // Loading a chained fixture without allow_chained=true must throw
    // (MF-2(i) production-flatness invariant).
    {
        const std::string benign_path =
            std::string(HF_TEST_DATA_DIR) +
            "/mzv_reductions_chained_test.json";
        bool threw = false;
        try {
            (void)load_mzv_expansion(benign_path);  // allow_chained=false
        } catch (const std::exception&) {
            threw = true;
        }
        check(threw,
              "MF-2(i) flatness invariant: chained fixture without "
              "allow_chained=true throws");
    }

    // --- Test 1d: A/B mint-site equivalence (PHASE_2 iter 9) ---------------
    // The KEY semantic test for the slim-ctx migration. Two scenarios run
    // the same Word through zero_one_period:
    //   wide path: ctx = build_mzv_var_list(table, user_vars) (current
    //     production: includes all 700 LHS vars); expansion=nullptr;
    //     to_mzv_one_word uses arm 1 (Poly::gen) + apply_mzv_reductions
    //     post-pass substitutes LHS -> RHS.
    //   slim path: ctx = build_basis_var_list(exp, user_vars) (PHASE_2
    //     new: basis only, ~10+|user_vars| vars); expansion=&exp;
    //     to_mzv_one_word uses arm 1 for basis names OR arm 2 (cross-ctx
    //     transfer of expansion[name]) for LHS names. apply_mzv_reductions
    //     post-pass is a no-op on slim ctx (no LHS vars present).
    //
    // Expected: the two paths produce VALUE-EQUIVALENT Rats. The canonical
    // string forms should match if FLINT's LEX monomial ordering is
    // preserved (per design §7.1 byte-identity attempt).
    //
    // Test method: build both ctxes, project the slim result into the wide
    // ctx via cross_ctx_transfer_rat, compare canonical strings.
    {
        // Load the production table + expansion. The wide ctx uses
        // the legacy build_mzv_var_list; the slim ctx uses build_basis_var_list.
        MzvReductionTable wide_table = load_mzv_reductions(production_json);
        std::vector<std::string> user_vars = {};  // no Feynman params for this test
        std::vector<std::string> wide_vars =
            build_mzv_var_list(wide_table, user_vars);
        std::vector<std::string> slim_vars =
            build_basis_var_list(exp, user_vars);
        PolyCtx wide_ctx(wide_vars);
        PolyCtx slim_ctx(slim_vars);
        // Empirical wide-ctx size: 710 = 10 basis (incl Log2) + 700 LHS,
        // user_vars empty here. The "711" figure from symcoef.cpp:714
        // refers to the wide ctx during integration where 1 extra
        // variable is typically present (e.g., from user_vars).
        check(wide_vars.size() == 710,
              "wide ctx has 710 vars (10 basis incl Log2 + 700 LHS; user_vars=0)");
        check(slim_vars.size() == 10,
              "slim ctx has 10 vars (Log2 + 9 basis, no user_vars, no LHS)");

        // Build a couple of test words. Per to_mzv_one_word encoding:
        // walking the word backwards, runs of `0` letters give "counts"
        // before each "pole" letter (the nonzero terminator). Word
        // [0,0,1] mints mzv_2 (basis, exercises arm 1 in both ctxes).
        // Word [0,0,0,0,1] mints mzv_4 (LHS in wide, expansion-table-hit
        // in slim — EXERCISES ARM 2).
        auto make_word = [](const PolyCtx& ctx,
                             const std::vector<long>& ints) -> Word {
            Word w;
            for (long v : ints) {
                w.letters.push_back(Rat::from_int(ctx, v));
            }
            return w;
        };

        // Helper: run zero_one_period both ways, compare canonical strings.
        auto compare = [&](const std::vector<long>& word_ints,
                           const std::string& label) {
            Word wide_word = make_word(wide_ctx, word_ints);
            Word slim_word = make_word(slim_ctx, word_ints);
            Rat wide_result = zero_one_period(wide_ctx, wide_word, wide_table,
                                               nullptr);
            Rat slim_result = zero_one_period(slim_ctx, slim_word, wide_table,
                                               &exp);
            // Project slim result into wide ctx for direct comparison
            // (avoids LEX-ordering differences from different var index maps).
            Rat slim_in_wide = cross_ctx_transfer_rat(slim_result, wide_ctx);
            const bool match = (wide_result.to_string() == slim_in_wide.to_string());
            check(match, label.c_str());
            if (!match) {
                std::printf("  wide:           %s\n",
                            wide_result.to_string().c_str());
                std::printf("  slim->wide:     %s\n",
                            slim_in_wide.to_string().c_str());
            }
        };

        // Arm 1 only (basis): mzv_2 from word [0,0,1] should match exactly.
        compare({0, 0, 1},
                "1d: word [0,0,1] (mzv_2; arm 1 in slim_ctx)");

        // Arm 2: word [0,0,0,0,1] should mint mzv_4 in wide_ctx (LHS) and
        // dispatch to expansion lookup in slim_ctx ((2/5)*mzv_2^2 per Euler).
        compare({0, 0, 0, 0, 1},
                "1d: word [0,0,0,0,1] (mzv_4; arm 2 in slim_ctx; R-4 cross-ctx)");

        // Arm 2 on depth-2: word [0,0,1,0,1] (weight 4, depth 2). Walking
        // backwards [1,0,1,0,0]: poles=[1,1], gap before first=2 (0,0 then
        // 1), gap before second=1 (0 then 1). Indices: counts*poles[j+1]/poles[j]
        // = [2*1/1, 1*1/1] = [2,1] -> mzv_2_1. With sign (-1)^2=1.
        // mzv_2_1 is in production table as a chained-LHS; A/B should match.
        compare({0, 0, 1, 0, 1},
                "1d: word [0,0,1,0,1] (mzv_2_1; arm 2 multi-index)");
    }

    // --- Test 1e: bridge input scanner (PHASE_3 / MF-3 fold) --------------
    // assert_no_lhs_tokens rejects LHS / out-of-table tokens while accepting
    // basis names + non-MZV identifiers. Test matrix per design §5.5:
    //   (a) basis name in input  -> OK
    //   (b) reducible LHS in input -> REJECT
    //   (c) basis with exponent  -> OK (scanner doesn't substitute)
    //   (d) LHS in sign+exponent context -> REJECT (scanner still rejects)
    //   (e) user var beginning with 'W' (Wm_2_3) -> OK (regex doesn't match)
    //   (f) mzv_*-shaped but unknown name (mzv_synthetic_a) -> REJECT
    //   (g) Log20 (non-identifier-boundary substring of Log2) -> OK
    //   (h) xmzv_2 (prefixed, non-identifier-boundary) -> OK
    //   (i) mzv_2_3_3 (full LHS, identifier-boundary correct) -> REJECT
    //   (j) null expansion ptr -> NO-OP (legacy callers)
    {
        auto threw = [](auto fn) -> bool {
            try { fn(); return false; } catch (const std::exception&) { return true; }
        };

        // (a) basis name only.
        check(!threw([&]() {
            assert_no_lhs_tokens("3*mzv_2 + 1/2*Log2^2", &exp, "test_1e_a");
        }), "1e(a): basis 'mzv_2' + 'Log2' OK");

        // (b) reducible LHS rejected.
        check(threw([&]() {
            assert_no_lhs_tokens("5*mzv_4 + 1", &exp, "test_1e_b");
        }), "1e(b): reducible LHS 'mzv_4' REJECTED");

        // (c) basis with exponent.
        check(!threw([&]() {
            assert_no_lhs_tokens("mzv_2^4 + mzv_3", &exp, "test_1e_c");
        }), "1e(c): basis 'mzv_2^4' OK");

        // (d) LHS in sign + exponent context.
        check(threw([&]() {
            assert_no_lhs_tokens("-mzv_m1_1_m1^2", &exp, "test_1e_d");
        }), "1e(d): LHS '-mzv_m1_1_m1^2' REJECTED");

        // (e) user var 'Wm_2_3' (begins with W, not mzv_).
        check(!threw([&]() {
            assert_no_lhs_tokens("Wm_2_3 * t1 + Wp_2_3 * t2", &exp, "test_1e_e");
        }), "1e(e): user vars 'Wm_2_3' / 'Wp_2_3' OK (not mzv_)");

        // (f) mzv_*-shaped UNKNOWN name (weight > 8, not in production
        // table). mzv_99 matches the grammar mzv_<digit>+ with weight 99
        // and is neither in basis nor in expansion, so REJECTED as
        // out-of-table.
        check(threw([&]() {
            assert_no_lhs_tokens("mzv_99", &exp, "test_1e_f");
        }), "1e(f): unknown 'mzv_99' REJECTED (weight beyond table)");

        // (f') mzv_synthetic_a does NOT match the mzv_* grammar
        //  (mzv_<digit>+); 's' after mzv_ is neither 'm' nor digit.
        // Scanner treats it as a generic identifier and accepts.
        // Documents the grammar boundary explicitly.
        check(!threw([&]() {
            assert_no_lhs_tokens("mzv_synthetic_a", &exp, "test_1e_fprime");
        }), "1e(f'): 'mzv_synthetic_a' OK (no digit after mzv_; not an MZV)");

        // (g) 'Log20' should NOT match the Log2 regex (word boundary).
        check(!threw([&]() {
            assert_no_lhs_tokens("Log20 * t1 + Log21", &exp, "test_1e_g");
        }), "1e(g): 'Log20' / 'Log21' OK (no Log2 boundary match)");

        // (h) prefixed identifier 'xmzv_2' should not match.
        check(!threw([&]() {
            assert_no_lhs_tokens("xmzv_2 + ymzv_3", &exp, "test_1e_h");
        }), "1e(h): 'xmzv_2' / 'ymzv_3' OK (no leading boundary match)");

        // (i) full LHS 'mzv_2_3_3' should be detected as reducible.
        check(threw([&]() {
            assert_no_lhs_tokens("3*mzv_2_3_3 + 1", &exp, "test_1e_i");
        }), "1e(i): 'mzv_2_3_3' REJECTED (reducible LHS)");

        // (j) null expansion ptr is a no-op.
        check(!threw([&]() {
            assert_no_lhs_tokens("anything 5*mzv_4 + Log2 mzv_synthetic_a",
                                  nullptr, "test_1e_j");
        }), "1e(j): null expansion ptr -> NO-OP");

        // (k) empty input.
        check(!threw([&]() {
            assert_no_lhs_tokens("", &exp, "test_1e_k");
        }), "1e(k): empty payload OK");

        // (l) input with no MZV-shape identifiers at all.
        check(!threw([&]() {
            assert_no_lhs_tokens("3*t1*t2 + 5*Log[t3] - 7", &exp, "test_1e_l");
        }), "1e(l): pure Feynman / Log[poly] payload OK");
    }

    // --- Test 1f: apply_mzv_reductions no-op guard (PHASE_4 iter 14) ------
    // Per design §5.4: when ctx has no LHS from table, the function
    // returns its input unchanged. Verify on a slim ctx Rat. The
    // identity must be canonical-string-equal (the function returns the
    // same Rat object semantically; we re-check via to_string()).
    {
        MzvReductionTable raw_table = load_mzv_reductions(production_json);
        std::vector<std::string> user_vars = {"t1", "t2"};
        std::vector<std::string> slim_vars =
            build_basis_var_list(exp, user_vars);
        PolyCtx slim_ctx(slim_vars);

        // Build a non-trivial Rat in the slim ctx. Use a basis-only
        // polynomial that would be reduction-eligible if the ctx had
        // LHS vars, but is already in basis form here.
        Rat input = Rat::parse(slim_ctx, "(2/5)*mzv_2^2 + 7*mzv_3*Log2 - 3");

        Rat output = apply_mzv_reductions(slim_ctx, raw_table, input);
        check(output.to_string() == input.to_string(),
              "1f: apply_mzv_reductions on slim ctx -> identity (no-op guard)");

        // Cache hit: a second call with the same ctx + table should be
        // O(1). We don't measure timing here (would be flaky), but
        // confirm semantic correctness on the second call.
        Rat output2 = apply_mzv_reductions(slim_ctx, raw_table, input);
        check(output2.to_string() == input.to_string(),
              "1f: apply_mzv_reductions cache hit -> identity preserved");
    }

    // --- Test 1g: clear_ctx_has_no_lhs_cache contract (PHASE_4 round-3 BLOCKER fix)
    // The cache must be clearable to defuse the PolyCtx-address-reuse
    // hazard documented for the R24-rev2/chain-17 pattern. In production
    // the bridge entry handler calls clear_ctx_has_no_lhs_cache()
    // alongside clear_rhs_cache() etc. This test locks the API contract:
    //   1. clear is callable with an empty cache (no-op)
    //   2. populate the cache via apply_mzv_reductions
    //   3. clear succeeds without crashing
    //   4. subsequent apply_mzv_reductions still returns correct identity
    //      (fresh scan after clear; not poisoned by a stale entry)
    {
        // (1) Clear on empty cache: smoke test.
        clear_ctx_has_no_lhs_cache();

        // (2) Populate.
        MzvReductionTable raw_table = load_mzv_reductions(production_json);
        std::vector<std::string> user_vars = {"t1", "t2"};
        std::vector<std::string> slim_vars =
            build_basis_var_list(exp, user_vars);
        PolyCtx slim_ctx(slim_vars);
        Rat input = Rat::parse(slim_ctx, "(2/5)*mzv_2^2 + 7*mzv_3*Log2 - 3");
        Rat first = apply_mzv_reductions(slim_ctx, raw_table, input);
        check(first.to_string() == input.to_string(),
              "1g: pre-clear apply -> identity (no-op guard fired)");

        // (3) Clear succeeds.
        clear_ctx_has_no_lhs_cache();
        check(true, "1g: clear_ctx_has_no_lhs_cache callable; no crash");

        // (4) After clear, the predicate must be re-scanned. The result
        // is still no-op (same slim_ctx, same table), but the code path
        // exercised is the cold-scan branch, not the hit branch.
        Rat second = apply_mzv_reductions(slim_ctx, raw_table, input);
        check(second.to_string() == input.to_string(),
              "1g: post-clear apply -> identity (cold scan re-populates correctly)");
    }

    if (g_failures == 0) {
        std::printf("\nAll PHASE_5 iter 16 (round-3 BLOCKER fix) tests passed.\n");
        return 0;
    } else {
        std::printf("\n%d FAILURES.\n", g_failures);
        return 1;
    }
}
