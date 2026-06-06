// Phase 1 (poles streaming, 2026-05-03): Task 1.3 — PolesBucket
// flush_threshold + drain hook behavioural test.
//
// Verifies four contracts on the new flush callback machinery:
//   1. Default-off bit-identity (SIZE_MAX threshold + null callback ⇒
//      bump() never invokes the callback, regardless of bump count).
//   2. Threshold fires on every entry past terms.size() > threshold,
//      provided the callback does NOT clear the bucket.
//   3. drain() invokes the callback exactly once when registered.
//   4. R28 C1 atomic-clear contract: a callback that DOES clear
//      terms/pending/index must observe terms.size() == pending.size()
//      and an index consistent with terms at the moment it fires; bumps
//      following the clear must land cleanly in the now-empty bucket.

#include "hyperflint/integrator/poles_bucket.hpp"

#include "hyperflint/algebra/poly_struct_hash.hpp"  // regkey_struct_hash sanity
#include "hyperflint/core/poly.hpp"
#include "hyperflint/core/symcoef.hpp"
#include "hyperflint/integrator/transform.hpp"  // Word, RegKey, RegTermSym

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace hyperflint;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(const std::string& tag, bool ok, const std::string& detail = "") {
    if (ok) {
        std::cout << "[PASS] " << tag << "\n";
        ++g_pass;
    } else {
        std::cout << "[FAIL] " << tag;
        if (!detail.empty()) std::cout << "  " << detail;
        std::cout << "\n";
        ++g_fail;
    }
}

// Helper: build a distinct RegKey for index i. We use a single Word
// containing one Rat letter built from the literal "i+1" (positive
// integer) so each i produces a structurally-distinct RegKey. The new
// bucket key is the 128-bit struct-hash of canonicalize_regkey(.).
RegKey make_distinct_key(const PolyCtx& ctx, std::size_t i) {
    Word w;
    w.letters.push_back(Rat::parse(ctx, std::to_string(i + 1)));
    RegKey k;
    k.push_back(std::move(w));
    return k;
}

// Test 1: default-off bit-identity. Push 100 distinct keys; assert
// callback never fires when threshold == SIZE_MAX and callback ==
// nullptr. Counter remains 0.
int test_default_off_bit_identity() {
    PolyCtx ctx({"x"});
    PolesBucket b;
    int call_count = 0;
    // No flush_threshold / flush_callback set. Default state.
    check("default flush_threshold == SIZE_MAX",
          b.flush_threshold == SIZE_MAX);
    check("default flush_callback == nullptr",
          b.flush_callback == nullptr);
    check("default flush_user_data == nullptr",
          b.flush_user_data == nullptr);

    SymCoef coef = SymCoef::pi_factor(ctx).mul_rat(Rat::parse(ctx, "1"));
    for (std::size_t i = 0; i < 100; ++i) {
        b.bump(make_distinct_key(ctx, i), coef);
    }
    check("default-off: 100 distinct entries land in terms",
          b.terms.size() == 100,
          "got terms.size()=" + std::to_string(b.terms.size()));
    check("default-off: terms.size() == pending.size()",
          b.terms.size() == b.pending.size());
    check("default-off: index.size() == terms.size()",
          b.index.size() == b.terms.size());
    check("default-off: callback never invoked (counter == 0)",
          call_count == 0,
          "got call_count=" + std::to_string(call_count));
    return 0;
}

// Test 2: threshold fires. flush_threshold = 4; callback only records
// the call (does NOT clear the bucket). 10 distinct keys ⇒ callback
// must fire on bumps 5..10 (one call per bump that pushes terms.size()
// past 4). That is 6 calls.
struct RecordOnly {
    int calls = 0;
    std::vector<std::size_t> sizes_at_call;
};
int test_threshold_fires() {
    PolyCtx ctx({"x"});
    PolesBucket b;
    RecordOnly rec;
    b.flush_threshold = 4;
    b.flush_callback  = [](PolesBucket& self, void* ud) {
        auto* r = static_cast<RecordOnly*>(ud);
        ++r->calls;
        r->sizes_at_call.push_back(self.terms.size());
    };
    b.flush_user_data = &rec;

    SymCoef coef = SymCoef::pi_factor(ctx).mul_rat(Rat::parse(ctx, "1"));
    for (std::size_t i = 0; i < 10; ++i) {
        b.bump(make_distinct_key(ctx, i), coef);
    }
    // After 10 bumps with threshold=4, callback fires when
    // terms.size() > 4, i.e. for terms.size() ∈ {5,6,7,8,9,10}.
    check("threshold-fires: 10 distinct entries all land in terms",
          b.terms.size() == 10,
          "got terms.size()=" + std::to_string(b.terms.size()));
    check("threshold-fires: callback fired 6 times",
          rec.calls == 6,
          "got calls=" + std::to_string(rec.calls));
    bool sizes_ok = (rec.sizes_at_call.size() == 6);
    for (std::size_t k = 0; sizes_ok && k < rec.sizes_at_call.size(); ++k) {
        if (rec.sizes_at_call[k] != k + 5) sizes_ok = false;
    }
    check("threshold-fires: callback observed sizes 5..10 in order",
          sizes_ok);
    return 0;
}

// Test 3: drain() fires. Push 3 entries (below threshold=4); call
// drain(); assert callback fires exactly once (drain itself).
int test_drain_fires() {
    PolyCtx ctx({"x"});
    PolesBucket b;
    RecordOnly rec;
    b.flush_threshold = 4;
    b.flush_callback  = [](PolesBucket& self, void* ud) {
        (void)self;
        auto* r = static_cast<RecordOnly*>(ud);
        ++r->calls;
    };
    b.flush_user_data = &rec;

    SymCoef coef = SymCoef::pi_factor(ctx).mul_rat(Rat::parse(ctx, "1"));
    for (std::size_t i = 0; i < 3; ++i) {
        b.bump(make_distinct_key(ctx, i), coef);
    }
    check("drain-fires: 3 bumps below threshold, no calls yet",
          rec.calls == 0,
          "got calls=" + std::to_string(rec.calls));
    b.drain();
    check("drain-fires: drain() invoked callback once",
          rec.calls == 1,
          "got calls=" + std::to_string(rec.calls));
    // Idempotency note: drain() is documented as "idempotent" in the
    // sense that it's safe to call when there's nothing to flush. With
    // a non-clearing callback, calling drain() twice fires twice (each
    // call delegates to the callback). The production callback clears
    // the bucket; a second drain() with an empty bucket still calls
    // back, but the host's wired callback is responsible for handling
    // empty input safely. This test only asserts the once-per-call
    // contract.
    b.drain();
    check("drain-fires: second drain() also fires (delegates to cb)",
          rec.calls == 2);
    // Drain with no callback: safe no-op.
    PolesBucket b2;
    b2.drain();  // must not crash; no callback registered.
    check("drain-fires: drain() with no callback is a safe no-op",
          true);
    return 0;
}

// Test 4: R28 C1 atomic clear contract. A realistic production
// callback clears terms/pending/index in one block. Push 8 entries
// with threshold=4. Bump #5 triggers the callback (terms.size()==5 >
// 4); callback validates invariants then clears the bucket. Bump #6
// must land on an empty bucket as terms[0]. Repeat until 8 bumps
// done. Final state: bucket should hold whatever entries arrived
// after the last flush (with threshold=4 and clear-on-flush, that's
// 0 entries if the last bump triggered a clear, otherwise the count
// since the last clear).
struct ClearingState {
    int calls = 0;
    bool invariants_ok = true;
    std::string fail_reason;
};
int test_atomic_clear_contract() {
    PolyCtx ctx({"x"});
    PolesBucket b;
    ClearingState st;
    b.flush_threshold = 4;
    b.flush_callback  = [](PolesBucket& self, void* ud) {
        auto* s = static_cast<ClearingState*>(ud);
        ++s->calls;
        // R28 C1: state invariants at the moment the callback fires.
        if (self.terms.size() != self.pending.size()) {
            s->invariants_ok = false;
            s->fail_reason = "terms.size() != pending.size() at flush";
        }
        if (self.index.size() != self.terms.size()) {
            s->invariants_ok = false;
            s->fail_reason = "index.size() != terms.size() at flush";
        }
        // Atomic clear: terms, pending, index in one block.
        self.terms.clear();
        self.pending.clear();
        self.index.clear();
    };
    b.flush_user_data = &st;

    SymCoef coef = SymCoef::pi_factor(ctx).mul_rat(Rat::parse(ctx, "1"));
    // 8 bumps with distinct keys. With threshold=4 and atomic clear:
    //   bump 1..4: terms grows to {1,2,3,4}, no flush.
    //   bump 5:    terms grows to 5, flush fires (size=5 at flush),
    //              callback clears → terms is empty after.
    //   bump 6:    terms = 1 (lands as terms[0] in empty bucket).
    //   bump 7:    terms = 2.
    //   bump 8:    terms = 3.
    // Final: terms.size() == 3, calls == 1.
    for (std::size_t i = 0; i < 8; ++i) {
        b.bump(make_distinct_key(ctx, i), coef);
        // After every bump, invariants must hold.
        if (b.terms.size() != b.pending.size()) {
            st.invariants_ok = false;
            st.fail_reason = "post-bump terms.size() != pending.size()";
        }
        if (b.index.size() != b.terms.size()) {
            st.invariants_ok = false;
            st.fail_reason = "post-bump index.size() != terms.size()";
        }
    }
    check("atomic-clear: callback fired exactly once",
          st.calls == 1,
          "got calls=" + std::to_string(st.calls));
    check("atomic-clear: invariants held across flush",
          st.invariants_ok,
          st.fail_reason);
    check("atomic-clear: 3 entries remain after final flush",
          b.terms.size() == 3,
          "got terms.size()=" + std::to_string(b.terms.size()));
    check("atomic-clear: terms.size() == pending.size() final",
          b.terms.size() == b.pending.size());
    check("atomic-clear: index.size() == terms.size() final",
          b.index.size() == b.terms.size());
    return 0;
}

}  // namespace

int main() {
    test_default_off_bit_identity();
    test_threshold_fires();
    test_drain_fires();
    test_atomic_clear_contract();

    std::cout << "\n=== PolesBucket flush_threshold + drain self-test ==="
              << "\n  passed: " << g_pass
              << "\n  failed: " << g_fail << "\n";
    return g_fail == 0 ? 0 : 1;
}
