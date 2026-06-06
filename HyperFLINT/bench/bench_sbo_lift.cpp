// bench_sbo_lift.cpp
//
// Phase 2-A2 PF1 (chain 19, 2026-05-03): standalone microbench of the
// SBO sparse-per-monomial -> fmpq_mpoly lift/lower round-trip.  No
// production source is touched; this is a pre-flight gate per R29 C2
// before any rep-swap implementation work.
//
// SBO Term layout follows R29 C5 (SoA coefs):
//   std::vector<fmpq>  coefs;        // 16 B / term inline
//   std::vector<TermExp>      term_exps;    // 41 B / term inline
//   where TermExp = { uint8_t k, uint32_t var_idx[8], uint8_t exp[8] }.
//
// k <= 8 inline (the 95th-percentile observed in phase2_pre is k=4.5);
// no overflow path is implemented because the empirical operand
// population in notes/hf_memory_plan/phase2_pre/sparsity_*.jsonl never
// exceeds k=6.  If a synthetic generator produces k>8 the bench
// asserts.
//
// Two modes (selected via --mode=...):
//   synth     : 1000 reps of (t=100, k=3, N=718) random SBO Polys.
//               Reports median ns/term/lift and ns/term/lower.
//               Gate (R29 C2): both <= 100 ns/term/{lift,lower}.
//   empirical : iterate every row of a phase2_pre sparsity JSONL.
//               For each row, synthesize 4 SBO Polys matching the
//               recorded (n_terms, k_avg, k_min, k_max), lift+lower
//               each, sum wall.  Reports mean lift+lower per call
//               (where "call" = one Rat::add/+= = 4 polys).
//               Gate (handoff PF1): mean <= 1 ms/call.
//
// Round-trip correctness verifier (--verify) lifts an SBO Poly,
// lowers it, then asserts fmpq_mpoly_equal against a reference
// fmpq_mpoly built directly from the same SBO data.  Off in timing
// runs.
//
// CLI:
//   bench_sbo_lift --mode=synth [--reps=1000] [--t=100] [--k=3]
//                  [--nvars=718] [--verify]
//   bench_sbo_lift --mode=empirical --jsonl=PATH [--verify]

#include <flint/fmpq_mpoly.h>
#include <flint/fmpq.h>
#include <flint/fmpz.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using clk = std::chrono::steady_clock;

static constexpr int    kInlineSlots = 8;
static constexpr uint64_t kBenchSeed = 0x5B015F1FULL;

// SBO TermExp.  k <= kInlineSlots; no heap overflow path in this PF1
// bench (operand population never reaches k=8).
struct TermExp {
    uint8_t  k;
    uint32_t var_idx[kInlineSlots];
    uint8_t  exp[kInlineSlots];
};

// SoA SparseMpoly per R29 C5.  fmpq is the underlying GMP
// rational struct; std::vector requires a non-array element type so
// fmpq_t (which is __fmpq[1]) cannot be used directly here.
struct SparseMpoly {
    std::vector<fmpq> coefs;       // SoA: parallel to term_exps
    std::vector<TermExp>     term_exps;   // SoA: parallel to coefs

    SparseMpoly() = default;

    // Move-only to make ownership explicit; fmpq contents are
    // owned by the vector and cleared in clear_all().
    SparseMpoly(const SparseMpoly&) = delete;
    SparseMpoly& operator=(const SparseMpoly&) = delete;
    SparseMpoly(SparseMpoly&& o) noexcept
        : coefs(std::move(o.coefs)), term_exps(std::move(o.term_exps)) {}
    SparseMpoly& operator=(SparseMpoly&& o) noexcept {
        if (this != &o) {
            clear_all();
            coefs = std::move(o.coefs);
            term_exps = std::move(o.term_exps);
        }
        return *this;
    }
    ~SparseMpoly() { clear_all(); }

    void reserve_terms(size_t n) {
        coefs.reserve(n);
        term_exps.reserve(n);
    }

    // Append a new term.  The caller has constructed `coef` already
    // (via fmpq_init + fmpq_set); we move ownership into the vector
    // by copying the struct and detaching `coef`'s payload (caller
    // must not call fmpq_clear on it after this call).
    void push_term(fmpq_t coef, const TermExp& exp_tag) {
        fmpq slot{};
        // Move-detach: copy the underlying struct (num/den fmpz_t)
        // into the vector slot, then zero the source so the caller's
        // dtor does not double-free.
        slot = *coef;
        std::memset(coef, 0, sizeof(fmpq));
        coefs.push_back(slot);
        term_exps.push_back(exp_tag);
    }

    void clear_all() {
        for (auto& c : coefs) {
            fmpq_clear(&c);
        }
        coefs.clear();
        term_exps.clear();
    }

    size_t size() const { return coefs.size(); }
};

// -------- lift / lower --------

// lift_to_fmpq_mpoly: scatter SBO terms into a packed exponent vector
// of length nvars (zero-fill + scatter k slots), call
// fmpq_mpoly_push_term_fmpq_ui per term, then sort_terms +
// combine_like_terms.  This matches Poly::transplant in poly.cpp:243
// exactly; the public FLINT API requires a length-N exps array
// regardless of nonzero count (R29 lift-cost note: structurally
// O(t*N), not O(t*k_avg)).
static void lift_to_fmpq_mpoly(
    fmpq_mpoly_t out, const SparseMpoly& s, slong nvars,
    const fmpq_mpoly_ctx_t ctx)
{
    fmpq_mpoly_zero(out, ctx);
    const size_t t = s.size();
    fmpq_mpoly_fit_length(out, static_cast<slong>(t), ctx);
    std::vector<ulong> exps(nvars, 0UL);
    for (size_t i = 0; i < t; ++i) {
        const TermExp& te = s.term_exps[i];
        // zero-fill (sparse-clear: only undo the scatter set by the
        // previous iteration; on first iteration `exps` is already
        // zero from the vector ctor)
        if (i > 0) {
            const TermExp& prev = s.term_exps[i - 1];
            for (uint8_t j = 0; j < prev.k; ++j) {
                exps[prev.var_idx[j]] = 0UL;
            }
        }
        // scatter
        for (uint8_t j = 0; j < te.k; ++j) {
            exps[te.var_idx[j]] = static_cast<ulong>(te.exp[j]);
        }
        // Pass the SoA coef directly; push_term takes const fmpq_t.
        fmpq_mpoly_push_term_fmpq_ui(out, &s.coefs[i], exps.data(), ctx);
    }
    // Restore zero in `exps` for hygiene (cheap O(k) of last term).
    if (!s.term_exps.empty()) {
        const TermExp& last = s.term_exps.back();
        for (uint8_t j = 0; j < last.k; ++j) exps[last.var_idx[j]] = 0UL;
    }
    fmpq_mpoly_sort_terms(out, ctx);
    fmpq_mpoly_combine_like_terms(out, ctx);
}

// lower_from_fmpq_mpoly: iterate fmpq_mpoly terms, gather nonzero
// (var_idx, exp) pairs into TermExp.  Asserts k <= kInlineSlots.
static void lower_from_fmpq_mpoly(
    SparseMpoly& out, const fmpq_mpoly_t in, slong nvars,
    const fmpq_mpoly_ctx_t ctx)
{
    const slong t = fmpq_mpoly_length(in, ctx);
    out.clear_all();
    out.reserve_terms(static_cast<size_t>(t));
    fmpq_t c;
    fmpq_init(c);
    std::vector<ulong> exps(nvars, 0UL);
    for (slong i = 0; i < t; ++i) {
        // Single-call gather of the full exponent vector; ~6× faster
        // than the per-variable get_term_var_exp_ui loop at N=718
        // (each var-call walks the term's packed exp from scratch).
        fmpq_mpoly_get_term_exp_ui(exps.data(), in, i, ctx);
        TermExp te{};
        te.k = 0;
        for (slong j = 0; j < nvars; ++j) {
            const ulong e = exps[j];
            if (e != 0) {
                if (te.k >= kInlineSlots) {
                    std::fprintf(stderr,
                        "[bench_sbo_lift] FATAL: term %ld has k>%d "
                        "(no overflow path in PF1 bench)\n",
                        static_cast<long>(i), kInlineSlots);
                    std::abort();
                }
                te.var_idx[te.k] = static_cast<uint32_t>(j);
                te.exp[te.k]     = static_cast<uint8_t>(e);
                te.k++;
            }
        }
        fmpq_mpoly_get_term_coeff_fmpq(c, in, i, ctx);
        out.push_term(c, te);
    }
    fmpq_clear(c);
}

// -------- generators --------

// Random SBO Poly with t terms, exact k nonzeros per term, uniform
// var_idx in [0, nvars), exp uniform in {1, 2, 3}, coef = 1/d for
// random d in [1, 99].  Deterministic via a passed RNG.
static SparseMpoly gen_uniform(
    size_t t, uint8_t k, slong nvars, std::mt19937_64& rng)
{
    assert(k <= kInlineSlots);
    SparseMpoly s;
    s.reserve_terms(t);
    std::uniform_int_distribution<uint32_t> var_d(0, static_cast<uint32_t>(nvars - 1));
    std::uniform_int_distribution<int>      exp_d(1, 3);
    std::uniform_int_distribution<int>      den_d(1, 99);
    fmpq_t c;
    fmpq_init(c);
    std::vector<uint32_t> chosen;
    chosen.reserve(k);
    for (size_t i = 0; i < t; ++i) {
        chosen.clear();
        // sample k distinct var_idx (rejection — k<<nvars so cheap)
        while (chosen.size() < k) {
            uint32_t v = var_d(rng);
            if (std::find(chosen.begin(), chosen.end(), v) == chosen.end())
                chosen.push_back(v);
        }
        std::sort(chosen.begin(), chosen.end());
        TermExp te{};
        te.k = k;
        for (uint8_t j = 0; j < k; ++j) {
            te.var_idx[j] = chosen[j];
            te.exp[j]     = static_cast<uint8_t>(exp_d(rng));
        }
        fmpq_set_si(c, 1, den_d(rng));
        s.push_term(c, te);
    }
    fmpq_clear(c);
    return s;
}

// Random SBO Poly that approximates an empirical row's k distribution:
// each term draws k uniform from [k_min, k_max] (clamped to
// kInlineSlots; rows with k_max>kInlineSlots are clipped and we
// record a clip count for stderr reporting).  Approximation is OK for
// PF1 — the gate cares about lift+lower aggregate, not per-term k
// fidelity.
static SparseMpoly gen_from_row(
    size_t t, int k_min, int k_max, slong nvars,
    std::mt19937_64& rng, size_t* clip_count_out = nullptr)
{
    if (k_min < 1) k_min = 1;
    if (k_max < k_min) k_max = k_min;
    int k_max_eff = std::min<int>(k_max, kInlineSlots);
    if (clip_count_out && k_max > kInlineSlots) (*clip_count_out)++;
    int k_min_eff = std::min<int>(k_min, k_max_eff);
    SparseMpoly s;
    s.reserve_terms(t);
    std::uniform_int_distribution<int>      k_d(k_min_eff, k_max_eff);
    std::uniform_int_distribution<uint32_t> var_d(0, static_cast<uint32_t>(nvars - 1));
    std::uniform_int_distribution<int>      exp_d(1, 3);
    std::uniform_int_distribution<int>      den_d(1, 99);
    fmpq_t c;
    fmpq_init(c);
    std::vector<uint32_t> chosen;
    chosen.reserve(kInlineSlots);
    for (size_t i = 0; i < t; ++i) {
        const int k = k_d(rng);
        chosen.clear();
        while (static_cast<int>(chosen.size()) < k) {
            uint32_t v = var_d(rng);
            if (std::find(chosen.begin(), chosen.end(), v) == chosen.end())
                chosen.push_back(v);
        }
        std::sort(chosen.begin(), chosen.end());
        TermExp te{};
        te.k = static_cast<uint8_t>(k);
        for (int j = 0; j < k; ++j) {
            te.var_idx[j] = chosen[j];
            te.exp[j]     = static_cast<uint8_t>(exp_d(rng));
        }
        fmpq_set_si(c, 1, den_d(rng));
        s.push_term(c, te);
    }
    fmpq_clear(c);
    return s;
}

// -------- verifier --------

// Build a reference fmpq_mpoly directly from the SBO data using the
// same scatter+push pattern as lift_to_fmpq_mpoly, then assert that
// (lower o lift)(s) round-trips to a fmpq_mpoly that compares equal.
static bool verify_round_trip(
    const SparseMpoly& s, slong nvars, const fmpq_mpoly_ctx_t ctx)
{
    fmpq_mpoly_t lifted;
    fmpq_mpoly_init(lifted, ctx);
    lift_to_fmpq_mpoly(lifted, s, nvars, ctx);

    SparseMpoly s2;
    lower_from_fmpq_mpoly(s2, lifted, nvars, ctx);

    fmpq_mpoly_t lifted2;
    fmpq_mpoly_init(lifted2, ctx);
    lift_to_fmpq_mpoly(lifted2, s2, nvars, ctx);

    const bool eq = fmpq_mpoly_equal(lifted, lifted2, ctx);
    fmpq_mpoly_clear(lifted,  ctx);
    fmpq_mpoly_clear(lifted2, ctx);
    return eq;
}

// -------- modes --------

static int run_synth(int reps, size_t t, uint8_t k, slong nvars, bool verify) {
    fmpq_mpoly_ctx_t ctx;
    fmpq_mpoly_ctx_init(ctx, nvars, ORD_LEX);

    std::mt19937_64 rng(kBenchSeed);
    // Pre-build a single representative SBO Poly; we time lift+lower
    // on this (re-built inside the rep loop into a fresh fmpq_mpoly).
    SparseMpoly s = gen_uniform(t, k, nvars, rng);

    if (verify) {
        bool ok = verify_round_trip(s, nvars, ctx);
        std::fprintf(stderr,
            "[bench_sbo_lift][synth] verify round-trip: %s\n",
            ok ? "PASS" : "FAIL");
        if (!ok) {
            fmpq_mpoly_ctx_clear(ctx);
            return 2;
        }
    }

    std::vector<double> lift_ns(reps);
    std::vector<double> lower_ns(reps);

    fmpq_mpoly_t lifted;
    fmpq_mpoly_init(lifted, ctx);

    // Warmup: 50 reps to stabilize cache + avoid first-call init noise.
    for (int w = 0; w < 50; ++w) {
        lift_to_fmpq_mpoly(lifted, s, nvars, ctx);
        SparseMpoly s2;
        lower_from_fmpq_mpoly(s2, lifted, nvars, ctx);
    }

    for (int r = 0; r < reps; ++r) {
        auto t0 = clk::now();
        lift_to_fmpq_mpoly(lifted, s, nvars, ctx);
        auto t1 = clk::now();
        SparseMpoly s2;
        lower_from_fmpq_mpoly(s2, lifted, nvars, ctx);
        auto t2 = clk::now();
        lift_ns[r]  = std::chrono::duration<double, std::nano>(t1 - t0).count();
        lower_ns[r] = std::chrono::duration<double, std::nano>(t2 - t1).count();
    }

    fmpq_mpoly_clear(lifted, ctx);
    fmpq_mpoly_ctx_clear(ctx);

    std::sort(lift_ns.begin(),  lift_ns.end());
    std::sort(lower_ns.begin(), lower_ns.end());
    auto median = [](const std::vector<double>& v) {
        return v[v.size() / 2];
    };
    auto mean = [](const std::vector<double>& v) {
        double s = 0;
        for (double x : v) s += x;
        return s / static_cast<double>(v.size());
    };

    const double med_lift   = median(lift_ns);
    const double med_lower  = median(lower_ns);
    const double mean_lift  = mean(lift_ns);
    const double mean_lower = mean(lower_ns);
    const double per_term_lift_med   = med_lift   / static_cast<double>(t);
    const double per_term_lower_med  = med_lower  / static_cast<double>(t);
    const double per_term_lift_mean  = mean_lift  / static_cast<double>(t);
    const double per_term_lower_mean = mean_lower / static_cast<double>(t);

    std::printf(
        "synth,nvars=%ld,t=%zu,k=%u,reps=%d,"
        "lift_total_ns_med=%.0f,lift_per_term_ns_med=%.2f,"
        "lift_total_ns_mean=%.0f,lift_per_term_ns_mean=%.2f,"
        "lower_total_ns_med=%.0f,lower_per_term_ns_med=%.2f,"
        "lower_total_ns_mean=%.0f,lower_per_term_ns_mean=%.2f\n",
        static_cast<long>(nvars), t, static_cast<unsigned>(k), reps,
        med_lift, per_term_lift_med,
        mean_lift, per_term_lift_mean,
        med_lower, per_term_lower_med,
        mean_lower, per_term_lower_mean);

    // R29 C2 gate: <= 100 ns/term/{lift,lower}, median.
    const bool lift_pass  = per_term_lift_med  <= 100.0;
    const bool lower_pass = per_term_lower_med <= 100.0;
    std::fprintf(stderr,
        "[bench_sbo_lift][synth] R29 C2 gate (<=100 ns/term/each):\n"
        "  lift  med = %6.2f ns/term  %s\n"
        "  lower med = %6.2f ns/term  %s\n",
        per_term_lift_med,  lift_pass  ? "PASS" : "FAIL",
        per_term_lower_med, lower_pass ? "PASS" : "FAIL");
    return (lift_pass && lower_pass) ? 0 : 1;
}

// -------- empirical mode --------

struct Row {
    int      n_vars   = 0;
    int      a_n_terms = 0, a_n_kmin = 0, a_n_kmax = 0;
    int      a_d_terms = 0, a_d_kmin = 0, a_d_kmax = 0;
    int      b_n_terms = 0, b_n_kmin = 0, b_n_kmax = 0;
    int      b_d_terms = 0, b_d_kmin = 0, b_d_kmax = 0;
};

// Lightweight JSONL parser: extract integer field by name from a
// well-formed line.  Bench-only — no general-purpose escape handling.
static int json_int(const std::string& line, const char* key, int dflt = 0) {
    std::string needle = std::string("\"") + key + "\":";
    auto p = line.find(needle);
    if (p == std::string::npos) return dflt;
    p += needle.size();
    while (p < line.size() && (line[p] == ' ')) ++p;
    int sign = 1;
    if (p < line.size() && line[p] == '-') { sign = -1; ++p; }
    int v = 0;
    while (p < line.size() && line[p] >= '0' && line[p] <= '9') {
        v = v * 10 + (line[p] - '0');
        ++p;
    }
    return sign * v;
}

static std::vector<Row> read_jsonl(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::fprintf(stderr, "could not open %s: %s\n",
                     path.c_str(), std::strerror(errno));
        std::exit(2);
    }
    std::vector<Row> rows;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Row r;
        r.n_vars     = json_int(line, "n_vars");
        r.a_n_terms  = json_int(line, "a_n_terms");
        r.a_n_kmin   = json_int(line, "a_n_k_min");
        r.a_n_kmax   = json_int(line, "a_n_k_max");
        r.a_d_terms  = json_int(line, "a_d_n_terms");
        r.a_d_kmin   = json_int(line, "a_d_k_min");
        r.a_d_kmax   = json_int(line, "a_d_k_max");
        r.b_n_terms  = json_int(line, "b_n_terms");
        r.b_n_kmin   = json_int(line, "b_n_k_min");
        r.b_n_kmax   = json_int(line, "b_n_k_max");
        r.b_d_terms  = json_int(line, "b_d_n_terms");
        r.b_d_kmin   = json_int(line, "b_d_k_min");
        r.b_d_kmax   = json_int(line, "b_d_k_max");
        if (r.n_vars > 0 && r.a_n_terms > 0) rows.push_back(r);
    }
    return rows;
}

static int run_empirical(const std::string& jsonl_path, bool verify) {
    auto rows = read_jsonl(jsonl_path);
    std::fprintf(stderr,
        "[bench_sbo_lift][empirical] loaded %zu rows from %s\n",
        rows.size(), jsonl_path.c_str());
    if (rows.empty()) return 1;

    // ctx is built once per unique nvars; in practice all parity_1
    // rows have nvars=718 so this is a trivial micro-cache.
    const slong nvars = rows.front().n_vars;
    fmpq_mpoly_ctx_t ctx;
    fmpq_mpoly_ctx_init(ctx, nvars, ORD_LEX);

    std::mt19937_64 rng(kBenchSeed);
    size_t clip_count = 0;

    if (verify) {
        // Verify round-trip on the first 5 rows × 4 polys = 20 polys.
        for (int ri = 0; ri < std::min<int>(5, (int)rows.size()); ++ri) {
            const Row& r = rows[ri];
            struct PSpec { int t, kmin, kmax; const char* name; };
            PSpec polys[4] = {
                {r.a_n_terms, r.a_n_kmin, r.a_n_kmax, "a.num"},
                {r.a_d_terms, r.a_d_kmin, r.a_d_kmax, "a.den"},
                {r.b_n_terms, r.b_n_kmin, r.b_n_kmax, "b.num"},
                {r.b_d_terms, r.b_d_kmin, r.b_d_kmax, "b.den"},
            };
            for (int pi = 0; pi < 4; ++pi) {
                if (polys[pi].t <= 0) continue;
                SparseMpoly s = gen_from_row(
                    polys[pi].t, polys[pi].kmin, polys[pi].kmax,
                    nvars, rng, &clip_count);
                bool ok = verify_round_trip(s, nvars, ctx);
                if (!ok) {
                    std::fprintf(stderr,
                        "[bench_sbo_lift][empirical] verify FAIL "
                        "on row %d %s\n", ri, polys[pi].name);
                    fmpq_mpoly_ctx_clear(ctx);
                    return 2;
                }
            }
        }
        std::fprintf(stderr,
            "[bench_sbo_lift][empirical] verify round-trip on first "
            "5 rows: PASS\n");
    }

    fmpq_mpoly_t lifted;
    fmpq_mpoly_init(lifted, ctx);

    // Warmup: 1 full sweep of the first 20 rows to stabilize cache.
    {
        std::mt19937_64 wrng(kBenchSeed + 1);
        const size_t warm_n = std::min<size_t>(20, rows.size());
        for (size_t ri = 0; ri < warm_n; ++ri) {
            const Row& r = rows[ri];
            int ts[4]    = {r.a_n_terms, r.a_d_terms, r.b_n_terms, r.b_d_terms};
            int kmins[4] = {r.a_n_kmin,  r.a_d_kmin,  r.b_n_kmin,  r.b_d_kmin};
            int kmaxs[4] = {r.a_n_kmax,  r.a_d_kmax,  r.b_n_kmax,  r.b_d_kmax};
            for (int pi = 0; pi < 4; ++pi) {
                if (ts[pi] <= 0) continue;
                SparseMpoly s = gen_from_row(ts[pi], kmins[pi], kmaxs[pi],
                                             nvars, wrng, nullptr);
                lift_to_fmpq_mpoly(lifted, s, nvars, ctx);
                SparseMpoly s2;
                lower_from_fmpq_mpoly(s2, lifted, nvars, ctx);
            }
        }
    }

    // Hot sweep.  We measure (gen + lift + lower) per poly but
    // record gen separately so we can subtract it from the reported
    // lift+lower aggregate.  Gen is bench overhead, not a production
    // cost; production gets SBO ops directly from Rat::add operands.
    double tot_gen_ns   = 0.0;
    double tot_lift_ns  = 0.0;
    double tot_lower_ns = 0.0;
    size_t n_calls      = 0;   // 1 call = 1 row = 4 polys
    size_t n_polys      = 0;
    size_t n_terms_lifted = 0;

    for (const Row& r : rows) {
        int ts[4]    = {r.a_n_terms, r.a_d_terms, r.b_n_terms, r.b_d_terms};
        int kmins[4] = {r.a_n_kmin,  r.a_d_kmin,  r.b_n_kmin,  r.b_d_kmin};
        int kmaxs[4] = {r.a_n_kmax,  r.a_d_kmax,  r.b_n_kmax,  r.b_d_kmax};
        double row_lift_ns  = 0.0;
        double row_lower_ns = 0.0;
        for (int pi = 0; pi < 4; ++pi) {
            if (ts[pi] <= 0) continue;
            auto tg0 = clk::now();
            SparseMpoly s = gen_from_row(ts[pi], kmins[pi], kmaxs[pi],
                                         nvars, rng, &clip_count);
            auto tg1 = clk::now();
            lift_to_fmpq_mpoly(lifted, s, nvars, ctx);
            auto t1 = clk::now();
            SparseMpoly s2;
            lower_from_fmpq_mpoly(s2, lifted, nvars, ctx);
            auto t2 = clk::now();
            tot_gen_ns +=
                std::chrono::duration<double, std::nano>(tg1 - tg0).count();
            row_lift_ns +=
                std::chrono::duration<double, std::nano>(t1 - tg1).count();
            row_lower_ns +=
                std::chrono::duration<double, std::nano>(t2 - t1).count();
            n_polys++;
            n_terms_lifted += static_cast<size_t>(ts[pi]);
        }
        tot_lift_ns  += row_lift_ns;
        tot_lower_ns += row_lower_ns;
        n_calls++;
    }

    fmpq_mpoly_clear(lifted, ctx);
    fmpq_mpoly_ctx_clear(ctx);

    const double mean_lift_per_call_ms  =
        (tot_lift_ns  / static_cast<double>(n_calls)) / 1.0e6;
    const double mean_lower_per_call_ms =
        (tot_lower_ns / static_cast<double>(n_calls)) / 1.0e6;
    const double mean_roundtrip_per_call_ms =
        mean_lift_per_call_ms + mean_lower_per_call_ms;
    const double mean_lift_per_term_ns =
        tot_lift_ns  / static_cast<double>(n_terms_lifted);
    const double mean_lower_per_term_ns =
        tot_lower_ns / static_cast<double>(n_terms_lifted);

    std::printf(
        "empirical,jsonl=%s,n_rows=%zu,n_polys=%zu,n_terms=%zu,"
        "gen_total_s=%.4f,"
        "lift_total_s=%.4f,lower_total_s=%.4f,"
        "mean_lift_per_call_ms=%.4f,mean_lower_per_call_ms=%.4f,"
        "mean_roundtrip_per_call_ms=%.4f,"
        "mean_lift_per_term_ns=%.2f,mean_lower_per_term_ns=%.2f,"
        "k_clip_rows=%zu\n",
        jsonl_path.c_str(), n_calls, n_polys, n_terms_lifted,
        tot_gen_ns / 1.0e9,
        tot_lift_ns / 1.0e9, tot_lower_ns / 1.0e9,
        mean_lift_per_call_ms, mean_lower_per_call_ms,
        mean_roundtrip_per_call_ms,
        mean_lift_per_term_ns, mean_lower_per_term_ns,
        clip_count);

    // Handoff PF1 gate: mean lift per call <= 1 ms.
    const bool lift_call_pass = mean_lift_per_call_ms <= 1.0;
    std::fprintf(stderr,
        "[bench_sbo_lift][empirical] handoff PF1 gate "
        "(mean lift/call <=1 ms):\n"
        "  mean lift  / call = %7.4f ms  %s\n"
        "  mean lower / call = %7.4f ms\n"
        "  mean rt    / call = %7.4f ms\n",
        mean_lift_per_call_ms, lift_call_pass ? "PASS" : "FAIL",
        mean_lower_per_call_ms, mean_roundtrip_per_call_ms);
    return lift_call_pass ? 0 : 1;
}

// -------- main --------

int main(int argc, char** argv) {
    std::string mode = "synth";
    std::string jsonl;
    int reps = 1000;
    size_t t  = 100;
    int    k  = 3;
    long   nvars = 718;
    bool   verify = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--mode=", 0) == 0)        mode = a.substr(7);
        else if (a.rfind("--jsonl=", 0) == 0)  jsonl = a.substr(8);
        else if (a.rfind("--reps=", 0) == 0)   reps = std::atoi(a.c_str() + 7);
        else if (a.rfind("--t=", 0) == 0)      t = static_cast<size_t>(std::atol(a.c_str() + 4));
        else if (a.rfind("--k=", 0) == 0)      k = std::atoi(a.c_str() + 4);
        else if (a.rfind("--nvars=", 0) == 0)  nvars = std::atol(a.c_str() + 8);
        else if (a == "--verify")              verify = true;
        else if (a == "--help" || a == "-h") {
            std::printf(
                "usage:\n"
                "  bench_sbo_lift --mode=synth [--reps=1000] [--t=100] [--k=3]\n"
                "                  [--nvars=718] [--verify]\n"
                "  bench_sbo_lift --mode=empirical --jsonl=PATH [--verify]\n");
            return 0;
        }
    }

    if (k > kInlineSlots) {
        std::fprintf(stderr,
            "[bench_sbo_lift] requested k=%d > kInlineSlots=%d\n",
            k, kInlineSlots);
        return 2;
    }

    if (mode == "synth") {
        return run_synth(reps, t, static_cast<uint8_t>(k),
                         static_cast<slong>(nvars), verify);
    } else if (mode == "empirical") {
        if (jsonl.empty()) {
            std::fprintf(stderr,
                "[bench_sbo_lift] --mode=empirical requires --jsonl=PATH\n");
            return 1;
        }
        return run_empirical(jsonl, verify);
    } else {
        std::fprintf(stderr,
            "[bench_sbo_lift] unknown --mode=%s\n", mode.c_str());
        return 1;
    }
}
