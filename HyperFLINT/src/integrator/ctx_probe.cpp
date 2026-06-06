#include "hyperflint/integrator/ctx_probe.hpp"

#include "hyperflint/integrator/env_flags.hpp"  // iter-77 Track-probe-ctx

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace hyperflint {
namespace {

bool init_enabled() {
    const char* s = HF_FLAG_PROBE_CTX_USAGE;
    return s && *s && (s[0] != '0');
}

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<std::vector<uint8_t>*>& seen_registry() {
    static std::vector<std::vector<uint8_t>*> r;
    return r;
}

thread_local std::vector<uint8_t> tls_seen;
thread_local bool tls_registered = false;

void register_tls() {
    if (tls_registered) return;
    std::lock_guard<std::mutex> lk(registry_mutex());
    seen_registry().push_back(&tls_seen);
    tls_registered = true;
}

}  // namespace

bool ctx_probe_enabled() {
    static const bool e = init_enabled();
    return e;
}

void ctx_probe_record(const fmpq_mpoly_struct* p, const PolyCtx& ctx) {
    if (!ctx_probe_enabled()) return;
    const long nvars = static_cast<long>(ctx.vars().size());
    if (nvars <= 0) return;
    if (tls_seen.size() < static_cast<size_t>(nvars)) {
        tls_seen.assign(nvars, 0);
        register_tls();
    }
    const long nterms = fmpq_mpoly_length(p, ctx.raw());
    if (nterms == 0) return;
    std::vector<unsigned long> exp(nvars);
    for (long t = 0; t < nterms; ++t) {
        fmpq_mpoly_get_term_exp_ui(exp.data(), p,
                                   static_cast<slong>(t), ctx.raw());
        for (long v = 0; v < nvars; ++v) {
            if (exp[v]) tls_seen[v] = 1;
        }
    }
}

std::string ctx_probe_dump_and_clear(const PolyCtx& ctx) {
    if (!ctx_probe_enabled()) return {};
    const long nvars = static_cast<long>(ctx.vars().size());
    std::vector<uint8_t> merged(nvars, 0);
    {
        std::lock_guard<std::mutex> lk(registry_mutex());
        for (auto* v : seen_registry()) {
            const size_t lim =
                std::min<size_t>(v->size(), static_cast<size_t>(nvars));
            for (size_t i = 0; i < lim; ++i) merged[i] |= (*v)[i];
            std::fill(v->begin(), v->end(), 0);
        }
    }
    long used = 0;
    std::ostringstream idxs;
    bool first = true;
    for (long i = 0; i < nvars; ++i) {
        if (merged[i]) {
            ++used;
            if (!first) idxs << ",";
            idxs << i;
            first = false;
        }
    }
    std::ostringstream out;
    out << "HF_PROBE_CTX_USAGE: pid=" << getpid()
        << " used=" << used
        << " total=" << nvars
        << " ratio="
        << (nvars > 0 ? (double)used / (double)nvars : 0.0)
        << " indices=[" << idxs.str() << "]";
    std::string s = out.str();
    // Optional: also append one line to the file at HF_PROBE_CTX_USAGE_PATH
    // for SubTropica/Mma in-dylib calls whose stderr is not captured.
    if (const char* path = HF_FLAG_PROBE_CTX_USAGE_PATH) {
        if (path && *path) {
            int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                std::string line = s + "\n";
                ssize_t written = write(fd, line.data(), line.size());
                (void)written;
                close(fd);
            }
        }
    }
    return s;
}

}  // namespace hyperflint
