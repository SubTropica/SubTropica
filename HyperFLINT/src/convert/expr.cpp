// Expr AST implementation — see expr.hpp for the node-kind map.

#include "hyperflint/convert/expr.hpp"

#include <algorithm>
#include <cassert>
#include <sstream>
#include <stdexcept>

namespace hyperflint {
namespace convert {

// ---- Factory methods ----

Expr Expr::leaf(Rat r) {
    Expr e(ExprKind::Leaf);
    e.leaf_rat_ = std::make_shared<Rat>(std::move(r));
    return e;
}

Expr Expr::plus(std::vector<Expr> summands) {
    Expr e(ExprKind::Plus);
    e.children_.reserve(summands.size());
    for (auto& c : summands) {
        e.children_.push_back(std::make_shared<const Expr>(std::move(c)));
    }
    return e;
}

Expr Expr::times(std::vector<Expr> factors) {
    Expr e(ExprKind::Times);
    e.children_.reserve(factors.size());
    for (auto& c : factors) {
        e.children_.push_back(std::make_shared<const Expr>(std::move(c)));
    }
    return e;
}

Expr Expr::power(Expr base, long n) {
    if (n < 1) {
        throw std::invalid_argument(
            "Expr::power: exponent must be >= 1 (negative/zero exponents "
            "must be folded into a Leaf by the parser)");
    }
    Expr e(ExprKind::Power);
    e.children_.push_back(std::make_shared<const Expr>(std::move(base)));
    e.power_n_ = n;
    return e;
}

Expr Expr::hlog(Rat var, std::vector<Rat> word) {
    Expr e(ExprKind::Hlog);
    e.hlog_var_ = std::make_shared<Rat>(std::move(var));
    e.hlog_word_ = std::move(word);
    return e;
}

// ---- Accessors ----

const Rat& Expr::leaf_rat() const {
    assert(kind_ == ExprKind::Leaf && "Expr::leaf_rat(): not a Leaf");
    return *leaf_rat_;
}

std::vector<Expr> Expr::children() const {
    assert((kind_ == ExprKind::Plus || kind_ == ExprKind::Times
            || kind_ == ExprKind::Power)
           && "Expr::children(): not a compound node");
    std::vector<Expr> out;
    out.reserve(children_.size());
    for (const auto& p : children_) {
        out.push_back(*p);
    }
    return out;
}

const Expr& Expr::child(size_t i) const {
    assert((kind_ == ExprKind::Plus || kind_ == ExprKind::Times
            || kind_ == ExprKind::Power)
           && "Expr::child(): not a compound node");
    assert(i < children_.size() && "Expr::child(): index out of range");
    return *children_[i];
}

long Expr::power_n() const {
    assert(kind_ == ExprKind::Power && "Expr::power_n(): not a Power");
    return power_n_;
}

const Rat& Expr::hlog_var() const {
    assert(kind_ == ExprKind::Hlog && "Expr::hlog_var(): not an Hlog");
    return *hlog_var_;
}

const std::vector<Rat>& Expr::hlog_word() const {
    assert(kind_ == ExprKind::Hlog && "Expr::hlog_word(): not an Hlog");
    return hlog_word_;
}

// ---- Canonical-string emission ----

std::string Expr::to_canonical_string() const {
    std::ostringstream o;
    switch (kind_) {
        case ExprKind::Leaf:
            o << leaf_rat_->to_string();
            break;
        case ExprKind::Plus:
        case ExprKind::Times: {
            // Collect child canonical strings, sort for Orderless match.
            std::vector<std::string> parts;
            parts.reserve(children_.size());
            for (const auto& p : children_) {
                parts.push_back(p->to_canonical_string());
            }
            std::sort(parts.begin(), parts.end());
            o << (kind_ == ExprKind::Plus ? "Plus[" : "Times[");
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i) o << ",";
                o << parts[i];
            }
            o << "]";
            break;
        }
        case ExprKind::Power:
            o << "Power[" << children_[0]->to_canonical_string()
              << "," << power_n_ << "]";
            break;
        case ExprKind::Hlog: {
            o << "Hlog[" << hlog_var_->to_string() << ",[";
            for (size_t i = 0; i < hlog_word_.size(); ++i) {
                if (i) o << ",";
                o << hlog_word_[i].to_string();
            }
            o << "]]";
            break;
        }
    }
    return o.str();
}

bool Expr::equals(const Expr& other) const {
    return to_canonical_string() == other.to_canonical_string();
}

}  // namespace convert
}  // namespace hyperflint
