// Phase 3-a: Expression AST for ConvertToHlogRegInf.
//
// Discriminated-union node type carrying symbolic expressions that
// arise in integrand inputs to `hyperflint`. The five node kinds
// mirror the heads HyperIntica.wl:3421 dispatches on:
//
//   Leaf(Rat)       — a rational function in the ambient PolyCtx
//                     (numeric literal, bare variable, or any
//                     sub-expression that lowers into a single Rat).
//   Plus(children)  — variadic sum.
//   Times(children) — variadic product.
//   Power(base, n)  — integer exponent; n < 0 routed through Rat
//                     division at parse time, so Power nodes in a
//                     finalized AST always have n >= 1.
//   Hlog(var, word) — symbolic iterated integral with a Rat var
//                     and a Rat-letter word.
//
// The `Log` head is NOT a node kind — it rewrites to
// `Hlog[arg, {0}]` at parse time (Mma's ConvertToHlogRegInf treats
// Log the same way at the dispatch level).
//
// Indirection: Plus/Times/Power need to hold child Exprs, and Expr
// is the outer type, so children use shared_ptr<const Expr>. Expr
// values are logically immutable after construction — the factory
// methods return a completed Expr and there are no mutators.

#pragma once

#include "hyperflint/core/rat.hpp"

#include <memory>
#include <string>
#include <vector>

namespace hyperflint {
namespace convert {

enum class ExprKind { Leaf, Plus, Times, Power, Hlog };

class Expr {
public:
    // ---- Factory methods ----

    static Expr leaf(Rat r);

    // Variadic node. Single-child lists are allowed (the parser may
    // produce a Plus[x] for a redundantly-parenthesized sum); the
    // canonical-string emitter keeps the wrapper visible.
    static Expr plus(std::vector<Expr> summands);
    static Expr times(std::vector<Expr> factors);

    // Power(base, n) with n >= 1. Negative exponents must be folded
    // into a Leaf by the parser (Rat handles x^(-k) natively).
    static Expr power(Expr base, long n);

    // Hlog[var, word]. Both var and each letter are Rats in the
    // caller-supplied PolyCtx.
    static Expr hlog(Rat var, std::vector<Rat> word);

    // ---- Accessors ----

    ExprKind kind() const { return kind_; }

    const Rat& leaf_rat() const;                 // precondition: kind == Leaf
    // Children accessors return by value (internal storage uses
    // shared_ptr indirection to break the recursive-type cycle).
    std::vector<Expr> children() const;          // precondition: kind in {Plus, Times, Power}
    size_t num_children() const { return children_.size(); }
    const Expr& child(size_t i) const;           // cheap single-child access
    long power_n() const;                        // precondition: kind == Power
    const Rat& hlog_var() const;                 // precondition: kind == Hlog
    const std::vector<Rat>& hlog_word() const;   // precondition: kind == Hlog

    // ---- Emission ----

    // Canonical string form for cross-validation:
    //   Leaf:  rat.to_string()
    //   Plus:  "Plus[c1,c2,...]"  with children emitted in the order
    //          they appear after a stable alphabetic sort (Mma's
    //          Plus/Times are Orderless so sorting both sides keeps
    //          cross-validation deterministic).
    //   Times: "Times[c1,c2,...]"  same sort.
    //   Power: "Power[base,n]"
    //   Hlog:  "Hlog[var,[l1,l2,...]]"  letters NOT sorted — word
    //          order is significant.
    std::string to_canonical_string() const;

    // Structural equality (post-canonical-form).
    bool equals(const Expr& other) const;

private:
    // Expr uses shared_ptr indirection for children — both to break
    // the recursive-type cycle and to keep copies cheap. Fields
    // irrelevant to the node's kind are default-initialized
    // (empty-vector, zero-long, empty rat_cache).
    ExprKind kind_;
    std::shared_ptr<Rat> leaf_rat_;         // Leaf only
    std::shared_ptr<Rat> hlog_var_;         // Hlog only
    std::vector<Rat> hlog_word_;            // Hlog only
    std::vector<std::shared_ptr<const Expr>> children_;   // Plus, Times, Power
    long power_n_ = 0;                       // Power only

    // Private ctor; use the factory methods.
    explicit Expr(ExprKind k) : kind_(k) {}
};

}  // namespace convert
}  // namespace hyperflint
