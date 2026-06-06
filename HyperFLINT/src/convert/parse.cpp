// Phase 3-a parser implementation — see parse.hpp for the grammar.

#include "hyperflint/convert/parse.hpp"

#include <cctype>
#include <sstream>

namespace hyperflint {
namespace convert {

namespace {

// ---------- Tokenizer ----------

enum class TokKind {
    End,
    Number,     // integer literal
    Ident,      // alphanumeric identifier (also function names)
    Plus, Minus, Star, Slash, Caret,
    LBracket, RBracket,  // '[' ']' (HyperInt.mpl list syntax, also Mma call)
    LBrace, RBrace,      // '{' '}' (Mma list syntax)
    LParen, RParen, Comma,
};

struct Tok {
    TokKind     kind = TokKind::End;
    std::string text;    // for Number / Ident
    size_t      pos = 0; // character offset in the source (diagnostics)
};

// Forward declaration so the Tokenizer can defer the
// "is this Hlog/Log/PolyLog?" decision when merging Mma-style
// integer subscripts (`mm[1]`, `m[1,2]`) into a single Ident.
bool is_function_name(const std::string& id);

class Tokenizer {
public:
    explicit Tokenizer(const std::string& s) : s_(s) {}

    std::vector<Tok> tokenize() {
        std::vector<Tok> out;
        while (pos_ < s_.size()) {
            skip_ws();
            if (pos_ >= s_.size()) break;
            char c = s_[pos_];
            if (std::isdigit(static_cast<unsigned char>(c))) {
                out.push_back(lex_number());
            } else if (std::isalpha(static_cast<unsigned char>(c))
                       || c == '_') {
                out.push_back(lex_ident());
            } else if (c == '+') { out.push_back({TokKind::Plus,     "+", pos_++}); }
            else if (c == '-') { out.push_back({TokKind::Minus,    "-", pos_++}); }
            else if (c == '*') { out.push_back({TokKind::Star,     "*", pos_++}); }
            else if (c == '/') { out.push_back({TokKind::Slash,    "/", pos_++}); }
            else if (c == '^') { out.push_back({TokKind::Caret,    "^", pos_++}); }
            else if (c == '[') { out.push_back({TokKind::LBracket, "[", pos_++}); }
            else if (c == ']') { out.push_back({TokKind::RBracket, "]", pos_++}); }
            else if (c == '{') { out.push_back({TokKind::LBrace,   "{", pos_++}); }
            else if (c == '}') { out.push_back({TokKind::RBrace,   "}", pos_++}); }
            else if (c == '(') { out.push_back({TokKind::LParen,   "(", pos_++}); }
            else if (c == ')') { out.push_back({TokKind::RParen,   ")", pos_++}); }
            else if (c == ',') { out.push_back({TokKind::Comma,    ",", pos_++}); }
            else {
                std::ostringstream o;
                o << "unexpected character '" << c << "' at position " << pos_;
                throw ParseError(o.str());
            }
        }
        out.push_back({TokKind::End, "", pos_});
        return out;
    }

private:
    void skip_ws() {
        while (pos_ < s_.size()
               && std::isspace(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
    }

    Tok lex_number() {
        size_t start = pos_;
        while (pos_ < s_.size()
               && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            ++pos_;
        }
        return {TokKind::Number, s_.substr(start, pos_ - start), start};
    }

    Tok lex_ident() {
        size_t start = pos_;
        while (pos_ < s_.size()
               && (std::isalnum(static_cast<unsigned char>(s_[pos_]))
                   || s_[pos_] == '_')) {
            ++pos_;
        }
        std::string name = s_.substr(start, pos_ - start);

        // Mma-style integer subscripts: merge `name[i]`, `name[i,j,...]`
        // into a single Ident whose text contains the brackets verbatim
        // so the round-trip preserves the source syntax.  FLINT's
        // fmpq_mpoly accepts arbitrary character strings as variable
        // names (verified roundtrip with "mm[1]" + "mm_2"), so the
        // bracketed name flows through PolyCtx unchanged.
        //
        // Only attempted when:
        //   - the identifier is NOT a function name (Hlog/Log/PolyLog
        //     use `[...]` for their argument-list semantics), and
        //   - the bracketed content is integers + commas + whitespace
        //     and at least one digit was seen.
        // On any non-numeric content inside the brackets we leave the
        // tokens unmerged and let the parser report the original
        // "expected ')' to close grouping" error.
        if (!is_function_name(name)
            && pos_ < s_.size() && s_[pos_] == '[') {
            size_t scan = pos_ + 1;        // first char after '['
            bool   ok = true;
            bool   sawDigit = false;
            while (scan < s_.size() && s_[scan] != ']') {
                char c = s_[scan];
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    sawDigit = true;
                    ++scan;
                } else if (c == ',' || c == ' ' || c == '\t') {
                    ++scan;
                } else {
                    ok = false;
                    break;
                }
            }
            if (ok && sawDigit
                && scan < s_.size() && s_[scan] == ']') {
                ++scan;  // consume ']'
                std::string compound = s_.substr(start, scan - start);
                pos_ = scan;
                return {TokKind::Ident, std::move(compound), start};
            }
        }
        return {TokKind::Ident, std::move(name), start};
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool is_function_name(const std::string& id) {
    return id == "Hlog" || id == "Log" || id == "PolyLog";
}

// ---------- Pass 1: identifier collection ----------

// Walk the token stream; any IDENT that is NOT a function name is
// treated as a free variable. Recognized function names are
// excluded from the variable list (no new ctx var for "Hlog").
std::vector<std::string>
collect_free_vars(const std::vector<Tok>& toks,
                   const std::vector<std::string>& user_vars) {
    std::vector<std::string> out = user_vars;
    for (const auto& t : toks) {
        if (t.kind != TokKind::Ident) continue;
        if (is_function_name(t.text)) continue;
        bool seen = false;
        for (const auto& v : out) {
            if (v == t.text) { seen = true; break; }
        }
        if (!seen) out.push_back(t.text);
    }
    return out;
}

// ---------- Pass 2: recursive-descent parser ----------

class Parser {
public:
    Parser(const std::vector<Tok>& toks, const PolyCtx& ctx)
        : toks_(toks), ctx_(ctx) {}

    Expr parse() {
        Expr e = parse_sum();
        if (peek().kind != TokKind::End) {
            std::ostringstream o;
            o << "unexpected token '" << peek().text
              << "' at position " << peek().pos << " (expected end of input)";
            throw ParseError(o.str());
        }
        return e;
    }

private:
    const Tok& peek(size_t offset = 0) const {
        size_t i = idx_ + offset;
        if (i >= toks_.size()) return toks_.back();
        return toks_[i];
    }

    Tok consume() {
        Tok t = toks_[idx_];
        if (t.kind != TokKind::End) ++idx_;
        return t;
    }

    void expect(TokKind k, const std::string& what) {
        if (peek().kind != k) {
            std::ostringstream o;
            o << "expected " << what << " at position " << peek().pos
              << " but got '" << peek().text << "'";
            throw ParseError(o.str());
        }
        consume();
    }

    // Leaf factory helper.
    Expr make_leaf_from_string(const std::string& rat_str) const {
        return Expr::leaf(Rat::parse(ctx_, rat_str));
    }

    // Convenience: zero and one literals in ctx_.
    Rat rat_zero() const { return Rat::parse(ctx_, "0"); }
    Rat rat_one()  const { return Rat::parse(ctx_, "1"); }

    // Fold a Plus/Times/Power if all children are Leaves.
    bool all_leaves(const std::vector<Expr>& xs) const {
        for (const auto& x : xs) {
            if (x.kind() != ExprKind::Leaf) return false;
        }
        return true;
    }

    // sum := product (("+" | "-") product)*
    Expr parse_sum() {
        std::vector<Expr> terms;
        terms.push_back(parse_product());
        while (peek().kind == TokKind::Plus || peek().kind == TokKind::Minus) {
            bool negate = (peek().kind == TokKind::Minus);
            consume();
            Expr rhs = parse_product();
            if (negate) {
                // rhs <- -1 * rhs (folded into Leaf if possible)
                if (rhs.kind() == ExprKind::Leaf) {
                    rhs = Expr::leaf(rat_zero() - rhs.leaf_rat());
                } else {
                    Expr minus_one = Expr::leaf(rat_zero() - rat_one());
                    rhs = Expr::times({minus_one, rhs});
                }
            }
            terms.push_back(std::move(rhs));
        }
        if (terms.size() == 1) return std::move(terms[0]);
        if (all_leaves(terms)) {
            Rat acc = rat_zero();
            for (const auto& t : terms) acc = acc + t.leaf_rat();
            return Expr::leaf(std::move(acc));
        }
        return Expr::plus(std::move(terms));
    }

    // product := unary (("*" | "/") unary)*
    Expr parse_product() {
        std::vector<Expr> factors;
        factors.push_back(parse_unary());
        while (peek().kind == TokKind::Star || peek().kind == TokKind::Slash) {
            bool invert = (peek().kind == TokKind::Slash);
            consume();
            Expr rhs = parse_unary();
            if (invert) {
                if (rhs.kind() == ExprKind::Leaf) {
                    rhs = Expr::leaf(rat_one() / rhs.leaf_rat());
                } else {
                    throw ParseError(
                        "division by a non-Leaf expression "
                        "(symbolic reciprocal of Hlog/Log unsupported)");
                }
            }
            factors.push_back(std::move(rhs));
        }
        if (factors.size() == 1) return std::move(factors[0]);
        if (all_leaves(factors)) {
            Rat acc = rat_one();
            for (const auto& f : factors) acc = acc * f.leaf_rat();
            return Expr::leaf(std::move(acc));
        }
        return Expr::times(std::move(factors));
    }

    // power := call ("^" unary)?
    //
    // PRECEDENCE FIX (2026-06-04): '^' binds TIGHTER than unary minus,
    // matching Mathematica/Maple: -f^n == -(f^n). The previous grammar
    // (power := unary ("^" unary)?) folded a leading minus into the BASE,
    // computing (-f)^n, which silently equals +f^n for every EVEN n and
    // so dropped the sign of any bare-unary-minus even power. Plain
    // benchmark integrands always carry rational coefficients (the -1/18*
    // Times path) and sums use binary minus, both unaffected; but the
    // graph pipeline's subtraction counterterms emit exactly the exposed
    // shape -(F)^(-2). Cost: a -4*zeta3/eps defect on the 2-loop
    // triangle-ladder (faces ord_-1_face_3/4), caught against the library
    // entry + pySecDec on 2026-06-04. The exponent side still parses via
    // parse_unary so x^-2 / x^(-2) keep working.
    Expr parse_power() {
        Expr base = parse_call();
        if (peek().kind != TokKind::Caret) return base;
        consume();
        Expr exp = parse_unary();
        if (exp.kind() != ExprKind::Leaf) {
            throw ParseError(
                "exponent must be an integer literal; symbolic "
                "exponents are outside Phase-3 scope");
        }
        // Require exp to be an integer. Rat::to_string() for an
        // integer prints as "N" or "-N" without a slash.
        std::string s = exp.leaf_rat().to_string();
        if (s.find('/') != std::string::npos
            || s.find('(') != std::string::npos
            || s.find_first_of("xyzabcd") != std::string::npos) {
            throw ParseError(
                "non-integer exponent '" + s + "'; only integer "
                "exponents are supported");
        }
        long n = 0;
        try { n = std::stol(s); }
        catch (...) {
            throw ParseError("cannot interpret exponent '" + s + "' as long");
        }
        if (n == 0) {
            return Expr::leaf(rat_one());
        }
        if (n < 0) {
            // Rat handles x^(-k) natively via reciprocal power.
            // Require a Leaf base; non-Leaf^(-k) is symbolic and
            // unsupported in Phase 3.
            if (base.kind() != ExprKind::Leaf) {
                throw ParseError(
                    "negative exponent on a non-Leaf expression "
                    "(e.g. Hlog^(-1)) is outside Phase-3 scope");
            }
            return Expr::leaf(base.leaf_rat().pow(n));
        }
        // n >= 1.
        if (base.kind() == ExprKind::Leaf) {
            return Expr::leaf(base.leaf_rat().pow(n));
        }
        return Expr::power(std::move(base), n);
    }

    // unary := ("-" | "+") unary | power
    // The sign wraps the whole power (see parse_power precedence note).
    Expr parse_unary() {
        if (peek().kind == TokKind::Plus) {
            consume();
            return parse_unary();
        }
        if (peek().kind == TokKind::Minus) {
            consume();
            Expr inner = parse_unary();
            if (inner.kind() == ExprKind::Leaf) {
                return Expr::leaf(rat_zero() - inner.leaf_rat());
            }
            Expr minus_one = Expr::leaf(rat_zero() - rat_one());
            return Expr::times({minus_one, std::move(inner)});
        }
        return parse_power();
    }

    // call := "Hlog[" expr "," "[" letterlist "]" "]"
    //       | "Log[" expr "]"
    //       | "PolyLog[" expr "," expr "]"
    //       | atom
    Expr parse_call() {
        if (peek().kind == TokKind::Ident && is_function_name(peek().text)
            && peek(1).kind == TokKind::LBracket) {
            std::string name = consume().text;
            consume();   // '['
            if (name == "Log") {
                Expr arg = parse_sum();
                expect(TokKind::RBracket, "']' to close Log[...]");
                // Rewrite Log[arg] → Hlog[arg, {0}]. Requires arg
                // to be a Leaf (Phase-3 scope).
                if (arg.kind() != ExprKind::Leaf) {
                    throw ParseError(
                        "Log[arg] requires a rational (Leaf) argument "
                        "in Phase 3 — Log of Hlog/Log is out of scope");
                }
                std::vector<Rat> word;
                word.push_back(rat_zero());
                return Expr::hlog(arg.leaf_rat(), std::move(word));
            }
            if (name == "Hlog") {
                Expr var = parse_sum();
                expect(TokKind::Comma, "',' between Hlog's var and word");
                if (var.kind() != ExprKind::Leaf) {
                    throw ParseError(
                        "Hlog's first argument must be a Leaf (rational) "
                        "— symbolic Hlog-of-Hlog is out of scope");
                }
                // Letter list uses either `[...]` (HyperInt.mpl
                // convention) or `{...}` (Mma convention). Accept both.
                TokKind open = peek().kind;
                TokKind close;
                if (open == TokKind::LBracket) {
                    close = TokKind::RBracket;
                    consume();
                } else if (open == TokKind::LBrace) {
                    close = TokKind::RBrace;
                    consume();
                } else {
                    throw ParseError(
                        "expected '[' or '{' to open Hlog's letter list");
                }
                std::vector<Rat> word;
                if (peek().kind != close) {
                    Expr letter = parse_sum();
                    if (letter.kind() != ExprKind::Leaf) {
                        throw ParseError(
                            "Hlog letter must be a Leaf (rational)");
                    }
                    word.push_back(letter.leaf_rat());
                    while (peek().kind == TokKind::Comma) {
                        consume();
                        Expr l2 = parse_sum();
                        if (l2.kind() != ExprKind::Leaf) {
                            throw ParseError(
                                "Hlog letter must be a Leaf (rational)");
                        }
                        word.push_back(l2.leaf_rat());
                    }
                }
                expect(close, (close == TokKind::RBracket
                               ? "']' to close Hlog's letter list"
                               : "'}' to close Hlog's letter list"));
                expect(TokKind::RBracket, "']' to close Hlog[...]");
                return Expr::hlog(var.leaf_rat(), std::move(word));
            }
            if (name == "PolyLog") {
                // Accepted syntactically; Phase 3 defers the
                // semantics to Phase 3-X (needs MplAsHlog).
                (void)parse_sum();
                expect(TokKind::Comma, "',' between PolyLog args");
                (void)parse_sum();
                expect(TokKind::RBracket, "']' to close PolyLog[...]");
                throw ParseError(
                    "PolyLog is outside Phase-3 scope (requires "
                    "MplAsHlog; deferred)");
            }
        }
        return parse_atom();
    }

    // atom := number | ident | "(" expr ")"
    Expr parse_atom() {
        if (peek().kind == TokKind::LParen) {
            consume();
            Expr e = parse_sum();
            expect(TokKind::RParen, "')' to close grouping");
            return e;
        }
        if (peek().kind == TokKind::Number) {
            Tok t = consume();
            return make_leaf_from_string(t.text);
        }
        if (peek().kind == TokKind::Ident) {
            if (is_function_name(peek().text)) {
                // Function name in atom position is always followed
                // by '['; if it isn't, something is wrong.
                throw ParseError(
                    "function name '" + peek().text + "' not followed by '['");
            }
            Tok t = consume();
            return make_leaf_from_string(t.text);
        }
        std::ostringstream o;
        o << "unexpected token '" << peek().text
          << "' at position " << peek().pos;
        throw ParseError(o.str());
    }

    const std::vector<Tok>& toks_;
    const PolyCtx& ctx_;
    size_t idx_ = 0;
};

}  // namespace

ParseResult parse_expression(const std::string& input,
                              const std::vector<std::string>& user_vars) {
    // Pass 1: tokenize, collect identifiers.
    Tokenizer lex(input);
    std::vector<Tok> toks = lex.tokenize();
    std::vector<std::string> augmented = collect_free_vars(toks, user_vars);

    // Pass 2: build ctx on the heap (PolyCtx is non-movable), parse.
    auto ctx = std::make_unique<PolyCtx>(augmented);
    Parser p(toks, *ctx);
    Expr e = p.parse();
    return ParseResult{std::move(e), std::move(ctx), std::move(augmented)};
}

}  // namespace convert
}  // namespace hyperflint
