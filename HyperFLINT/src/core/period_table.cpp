#include "hyperflint/core/period_table.hpp"

#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace hyperflint {

struct PeriodTable::Impl {
    mutable std::mutex mu;
    std::unordered_map<std::string, std::uint32_t> ids;
    std::vector<std::string> keys;
};

PeriodTable& PeriodTable::instance() {
    // Leaked singleton: immortal by design (ids must outlive every
    // SymMonomial; HyperInt keeps period lookups permanent likewise).
    static PeriodTable* t = new PeriodTable();
    return *t;
}

PeriodTable::Impl& PeriodTable::impl() const {
    static Impl* i = new Impl();
    return *i;
}

std::uint32_t PeriodTable::id_for(const std::string& canonical_key) {
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mu);
    auto it = im.ids.find(canonical_key);
    if (it != im.ids.end()) return it->second;
    const std::uint32_t id = static_cast<std::uint32_t>(im.keys.size());
    im.keys.push_back(canonical_key);
    im.ids.emplace(canonical_key, id);
    return id;
}

std::string PeriodTable::key_for(std::uint32_t id) const {
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mu);
    if (id >= im.keys.size())
        throw std::out_of_range("PeriodTable::key_for: unknown id");
    return im.keys[id];
}

std::size_t PeriodTable::size() const {
    Impl& im = impl();
    std::lock_guard<std::mutex> lk(im.mu);
    return im.keys.size();
}

}  // namespace hyperflint
