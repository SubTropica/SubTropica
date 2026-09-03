#include "hyperflint/reduce/zero_one_atoms.hpp"

#include <cstdlib>
#include <mutex>
#include <unordered_set>

namespace hyperflint {

namespace {
std::mutex& registry_mutex() { static std::mutex m; return m; }
std::vector<std::string>& registry_words() { static std::vector<std::string> w; return w; }
}  // namespace

std::string zero_one_atom_name(int k) { return "zop_" + std::to_string(k); }

void append_zero_one_atoms(std::vector<std::string>& vars) {
    std::unordered_set<std::string> seen(vars.begin(), vars.end());
    for (int k = 1; k <= kZeroOneAtomPoolSize; ++k) {
        const std::string n = zero_one_atom_name(k);
        if (seen.insert(n).second) vars.push_back(n);
    }
}

namespace {
// HF_ZERO_ONE_ATOM_POOL=<n> (test hook, default = kZeroOneAtomPoolSize):
// lowers the usable pool so the exhaustion path (and the OMP exception
// transport that turns it into a structured failure) can be exercised
// on a small fixture.  Never raises it: the ring only has
// kZeroOneAtomPoolSize reserved names.
int usable_pool_size() {
    static const int n = [] {
        const char* e = std::getenv("HF_ZERO_ONE_ATOM_POOL");
        if (!e) return kZeroOneAtomPoolSize;
        const long v = std::strtol(e, nullptr, 10);
        if (v < 1) return 1;
        if (v > kZeroOneAtomPoolSize) return kZeroOneAtomPoolSize;
        return static_cast<int>(v);
    }();
    return n;
}
}  // namespace

std::string zero_one_atom_for(const std::string& word_json) {
    std::lock_guard<std::mutex> lk(registry_mutex());
    auto& words = registry_words();
    for (size_t i = 0; i < words.size(); ++i) {
        if (words[i] == word_json) return zero_one_atom_name(static_cast<int>(i) + 1);
    }
    if (static_cast<int>(words.size()) >= usable_pool_size()) {
        throw ZeroOneAtomsExhausted(word_json, usable_pool_size());
    }
    words.push_back(word_json);
    return zero_one_atom_name(static_cast<int>(words.size()));
}

bool zero_one_atoms_used() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    return !registry_words().empty();
}

std::vector<std::pair<std::string, std::string>> zero_one_atom_table() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    std::vector<std::pair<std::string, std::string>> out;
    const auto& words = registry_words();
    for (size_t i = 0; i < words.size(); ++i) {
        out.emplace_back(zero_one_atom_name(static_cast<int>(i) + 1), words[i]);
    }
    return out;
}

std::string zero_one_atoms_json() {
    std::string o = "{";
    bool first = true;
    for (const auto& kv : zero_one_atom_table()) {
        if (!first) o += ",";
        o += "\"" + kv.first + "\":" + kv.second;
        first = false;
    }
    return o + "}";
}

void zero_one_atoms_reset_for_tests() {
    std::lock_guard<std::mutex> lk(registry_mutex());
    registry_words().clear();
}

}  // namespace hyperflint
