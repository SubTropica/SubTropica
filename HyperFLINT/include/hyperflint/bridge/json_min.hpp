// hyperflint/bridge/json_min.hpp -- minimal, strict, structural JSON access
// for the bridge request bodies (2026-09-21).
//
// Replaces the regex extractors of handlers.cpp, which stopped at the first
// `]` byte regardless of string quoting and so truncated every string array
// at an element containing an indexed symbol name (`mm[1]`): the order search
// and verify_order then ran on a mutilated polynomial set and certified
// orders the integrator refused, and the `hyperflint` op silently skipped a
// bracketed integration variable or range endpoint (INV-JSON-STRING-ARRAYS,
// docs/cross-subsystem-invariants.md).
//
// Contract:
//   - A member is looked up EXACTLY, at the top level of the request object
//     (key text inside a value, a nested object's member, or `vars` versus
//     `vars_int` never match).
//   - `json_str_array` returns the decoded strings of an array-valued member;
//     every element must be a string and the elements comma-separated; `""`
//     and `[]` are preserved; an ABSENT member yields an empty vector.  Any
//     malformation (non-array value, non-string element, missing comma,
//     invalid or unterminated escape, truncated document) throws
//     std::runtime_error -- never a parsed prefix.  Callers wrap the request
//     parse in their existing try/catch and answer with error_json_op.
//   - `json_member_present` distinguishes an absent `verify_order` from a
//     present-but-empty one (verify mode must not fall back to a search).
//   - `json_array_interior` / `json_subarrays` walk arrays of arrays
//     (`groups`, `exps`, `shuffle`) string-aware.
//   - `json_unescape` decodes the standard JSON escapes and rejects others;
//     `json_str_field` uses it (the older lenient convention -- backslash
//     stripped from unknown escapes -- is retired; no production request
//     carries such escapes).
// The request producers (SubTropica.wl, ExportString[..., "JSON"]) emit
// standard JSON; no unescaping beyond `\"` and `\\` occurs in practice.
// Duplicate top-level keys resolve FIRST-wins (no producer emits them);
// scalar fields (booleans, numbers) are read by the callers' own scanners.
#pragma once

#include <stdexcept>
#include <string>
#include <vector>

namespace hyperflint {
namespace jsonmin {

namespace detail {

inline void skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() &&
           (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
}

// Four hex digits at s[i..i+3] -> v; leaves i just past them.
inline unsigned read_hex4(const std::string& s, size_t& i) {
    if (i + 4 > s.size()) throw std::runtime_error("json: bad \\u escape");
    unsigned v = 0;
    for (int k = 0; k < 4; ++k) {
        const char h = s[i++];
        v <<= 4;
        if (h >= '0' && h <= '9')      v |= static_cast<unsigned>(h - '0');
        else if (h >= 'a' && h <= 'f') v |= static_cast<unsigned>(h - 'a' + 10);
        else if (h >= 'A' && h <= 'F') v |= static_cast<unsigned>(h - 'A' + 10);
        else throw std::runtime_error("json: bad \\u escape");
    }
    return v;
}

// Parse the JSON string literal at s[i] == '"'; returns the decoded text
// and leaves i just past the closing quote.  A \u escape decodes to UTF-8;
// a surrogate pair is combined (a lone surrogate is a malformed document).
inline std::string parse_string(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '"')
        throw std::runtime_error("json: expected a string");
    std::string out;
    ++i;
    while (true) {
        if (i >= s.size()) throw std::runtime_error("json: unterminated string");
        const char c = s[i++];
        if (c == '"') return out;
        if (c == '\\') {
            if (i >= s.size()) throw std::runtime_error("json: bad escape");
            const char e = s[i++];
            switch (e) {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'u': {
                    unsigned v = read_hex4(s, i);
                    if (v >= 0xD800 && v <= 0xDBFF) {
                        // high surrogate: the low half must follow as \uDC00..\uDFFF
                        if (i + 2 > s.size() || s[i] != '\\' || s[i + 1] != 'u')
                            throw std::runtime_error("json: lone high surrogate");
                        i += 2;
                        const unsigned lo = read_hex4(s, i);
                        if (lo < 0xDC00 || lo > 0xDFFF)
                            throw std::runtime_error("json: bad low surrogate");
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                    } else if (v >= 0xDC00 && v <= 0xDFFF) {
                        throw std::runtime_error("json: lone low surrogate");
                    }
                    if (v < 0x80) {
                        out += static_cast<char>(v);
                    } else if (v < 0x800) {
                        out += static_cast<char>(0xC0 | (v >> 6));
                        out += static_cast<char>(0x80 | (v & 0x3F));
                    } else if (v < 0x10000) {
                        out += static_cast<char>(0xE0 | (v >> 12));
                        out += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (v & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (v >> 18));
                        out += static_cast<char>(0x80 | ((v >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((v >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (v & 0x3F));
                    }
                    break;
                }
                default:
                    throw std::runtime_error("json: invalid escape");
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            throw std::runtime_error("json: control character in string");
        } else {
            out += c;
        }
    }
}

// Skip one JSON value (string, number, literal, array or object), string-aware.
inline void skip_value(const std::string& s, size_t& i) {
    skip_ws(s, i);
    if (i >= s.size()) throw std::runtime_error("json: unexpected end of document");
    const char c = s[i];
    if (c == '"') { parse_string(s, i); return; }
    if (c == '[' || c == '{') {
        // Track the opener of every level so that a `]` cannot close a `{`
        // (and vice versa): a mismatched pair is a malformed document.
        std::string openers;
        while (i < s.size()) {
            const char d = s[i];
            if (d == '"') { parse_string(s, i); continue; }
            if (d == '[' || d == '{') {
                openers.push_back(d);
            } else if (d == ']' || d == '}') {
                if (openers.empty() ||
                    (d == ']' && openers.back() != '[') ||
                    (d == '}' && openers.back() != '{'))
                    throw std::runtime_error("json: mismatched bracket");
                openers.pop_back();
                if (openers.empty()) { ++i; return; }
            }
            ++i;
        }
        throw std::runtime_error("json: unterminated array or object");
    }
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' &&
           s[i] != ' ' && s[i] != '\n' && s[i] != '\t' && s[i] != '\r') ++i;
}

// Index of the first character of the value of top-level member `key`,
// or std::string::npos when the member is absent.
inline size_t find_member(const std::string& s, const std::string& key) {
    size_t i = 0;
    skip_ws(s, i);
    if (i >= s.size() || s[i] != '{') throw std::runtime_error("json: expected an object");
    ++i;
    while (true) {
        skip_ws(s, i);
        if (i < s.size() && s[i] == '}') return std::string::npos;
        const std::string k = parse_string(s, i);
        skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') throw std::runtime_error("json: expected ':'");
        ++i;
        skip_ws(s, i);
        if (k == key) return i;
        skip_value(s, i);
        skip_ws(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return std::string::npos;
        throw std::runtime_error("json: expected ',' or '}'");
    }
}

}  // namespace detail

// Decode one JSON string literal (with its quotes); strict.
inline std::string json_unescape_literal(const std::string& literal) {
    size_t i = 0;
    return detail::parse_string(literal, i);
}

inline bool json_member_present(const std::string& body, const std::string& key) {
    return detail::find_member(body, key) != std::string::npos;
}

// Decoded strings of the array-valued top-level member `key`; absent -> {}.
inline std::vector<std::string> json_str_array(const std::string& body,
                                               const std::string& key) {
    std::vector<std::string> out;
    size_t i = detail::find_member(body, key);
    if (i == std::string::npos) return out;
    if (body[i] != '[')
        throw std::runtime_error("json: member '" + key + "' is not an array");
    ++i;
    detail::skip_ws(body, i);
    if (i < body.size() && body[i] == ']') return out;
    while (true) {
        detail::skip_ws(body, i);
        if (i >= body.size() || body[i] != '"')
            throw std::runtime_error("json: array '" + key + "' element is not a string");
        out.push_back(detail::parse_string(body, i));
        detail::skip_ws(body, i);
        if (i < body.size() && body[i] == ',') { ++i; continue; }
        if (i < body.size() && body[i] == ']') return out;
        throw std::runtime_error("json: array '" + key + "' missing ',' or ']'");
    }
}

// Decoded string value of the string-valued top-level member `key`; absent -> "".
inline std::string json_str_field(const std::string& body, const std::string& key) {
    size_t i = detail::find_member(body, key);
    if (i == std::string::npos) return {};
    if (body[i] != '"')
        throw std::runtime_error("json: member '" + key + "' is not a string");
    return detail::parse_string(body, i);
}

// Raw interior text of the array-valued top-level member `key` (between the
// outer brackets), string-aware; absent -> "".
inline std::string json_array_interior(const std::string& body, const std::string& key) {
    size_t i = detail::find_member(body, key);
    if (i == std::string::npos) return {};
    if (body[i] != '[')
        throw std::runtime_error("json: member '" + key + "' is not an array");
    const size_t start = i;
    detail::skip_value(body, i);  // i is now just past the matching ']'
    return body.substr(start + 1, i - start - 2);
}

// Split the interior of an array into the raw texts of its top-level values
// (any JSON type, each text complete), string-aware.
inline std::vector<std::string> json_subvalues(const std::string& interior) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        detail::skip_ws(interior, i);
        if (i >= interior.size()) return out;
        const size_t start = i;
        detail::skip_value(interior, i);
        out.push_back(interior.substr(start, i - start));
        detail::skip_ws(interior, i);
        if (i < interior.size() && interior[i] == ',') { ++i; continue; }
        if (i >= interior.size()) return out;
        throw std::runtime_error("json: expected ',' between array values");
    }
}

// Split the interior of an array of arrays into the raw texts of its
// sub-arrays (each including its own brackets), string-aware.
inline std::vector<std::string> json_subarrays(const std::string& interior) {
    std::vector<std::string> out = json_subvalues(interior);
    for (const auto& v : out) {
        if (v.empty() || v[0] != '[') throw std::runtime_error("json: expected a nested array");
    }
    return out;
}

// Convenience for the group parsers: the decoded string lists of an array of
// string arrays (`groups`); an empty inner array is kept as an empty list.
inline std::vector<std::vector<std::string>> json_nested_str_arrays(const std::string& body,
                                                                    const std::string& key) {
    std::vector<std::vector<std::string>> out;
    const std::string inner = json_array_interior(body, key);
    if (!json_member_present(body, key)) return out;
    for (const auto& sub : json_subarrays(inner)) {
        out.push_back(json_str_array("{\"xs\":" + sub + "}", "xs"));
    }
    return out;
}

}  // namespace jsonmin
}  // namespace hyperflint
