// HF_FLAG_NAME_* macros for Track-cache-op-memo environment flags.
//
// Track scope: cache-op-memo (docs/env_flags.md §2 Track-cache-op-memo,
// 14 entries total; iter-7, iter-60, iter-73, iter-76 vintage). This
// header defines NAME-as-macro string-literal macros for all 14 entries.
//
// Why NAME-as-macro (HF_FLAG_NAME_*) and not VALUE-as-macro (HF_FLAG_*):
//   Track-cache-op-memo is the first cluster reached through helper
//   wrappers env_truthy(name) and env_size(name, fallback). Those
//   wrappers are static-local to src/core/operator_memo.cpp and take
//   const char* by parameter. A VALUE-as-macro shape like
//       #define HF_FLAG_OPERATOR_MEMO_TRUTHY env_truthy("HF_OPERATOR_MEMO")
//   would require env_truthy to be visible at every macro expansion
//   site (i.e., promote env_truthy / env_size from static-local to
//   header-inline). That is a larger linkage refactor than the macro
//   layer warrants. NAME-as-macro lets the call site choose the access
//   pattern (env_truthy / env_size / std::getenv / strtoll) while the
//   macro only consolidates the string literal in one place.
//
// Macro shape (uniform across all 14 entries):
//   #define HF_FLAG_NAME_<NAME> <env var literal as quoted string>
// Each macro expands to a C string literal (const char[]) that is
// passed as the const char* argument to env_truthy / env_size, or to
// std::getenv directly.
//
// Family-of-two convention with HF_FLAG_*:
//   - HF_FLAG_<NAME>      (iter-62/63/64): expands to the *value*
//     that std::getenv returns for the wrapped name (const char*,
//     NULL when unset). Used at plain std::getenv call sites that
//     have no wrapper.
//   - HF_FLAG_NAME_<NAME> (iter-65, this header): expands to the
//     *string literal* of the env var name itself. Used where a
//     wrapper such as env_truthy or env_size takes the name as an
//     argument, and at plain std::getenv sites within this cluster
//     (uniform shape across the whole Track-cache-op-memo cluster).
// Both shapes preserve the string literal verbatim in the macro body,
// so the hf-env-flag-registry-coverage ctest's parser-B finds the
// literal in this header rather than at each call site. Set S_src is
// unchanged before and after this refactor; only the file in which
// each literal appears changes (source -> header).
//
// Call sites (refactored to use these macros):
//   src/core/operator_memo.cpp : 15 sites covering 14 unique names
//                                (HF_OP_MEMO_EVICT_ON_RSS appears at
//                                two sites, lines 186 + 242).
//
// Of the 15 sites:
//   9 wrapper-based: env_truthy(<NAME>) or env_size(<NAME>, default)
//   6 plain getenv : std::getenv(<NAME>) where non-truthy semantics
//                    are needed (strtoll, strcmp, "0 means off"
//                    sentinel, two-branch dual-knob, paired-pointer
//                    fail-fast check).
//                    [iter-74 F4-tail clarification: the "paired-pointer
//                    fail-fast check" referenced here is the GENUINE
//                    std::abort()-on-conflict pattern at
//                    src/core/operator_memo.cpp:185-198 pairing
//                    HF_MI_COLLECT_OPTION_M_C with HF_OP_MEMO_EVICT_ON_RSS;
//                    DO NOT confuse it with the iter-65 F6-corrected
//                    silent-conjunction pattern documented at
//                    include/hyperflint/algebra/env_flags.hpp:82-94
//                    pairing HF_ENABLE_KNOWN_BROKEN_PF_CACHE with
//                    HF_I_KNOW_THIS_IS_BROKEN, which has NO abort and
//                    NO diagnostic.]
//
// IMPORTANT (iter-63 / iter-64 lesson): do NOT write any HF_ env-var
// name as a quoted literal in this file's comments, or parser-B will
// count it as an extra source-side entry. The prose above refers to
// names unquoted precisely for that reason.
//
// SILENT-DEFAULT-OFF HAZARD (iter-65 POST-LAND reviewer F2): each
// HF_FLAG_NAME_<NAME> macro expands to the *string literal of the
// name*, which is a non-NULL non-empty C string. If a caller writes
//   const char* x = HF_FLAG_NAME_OP_MEMO_EVICT_ON_RSS;
// (forgetting the std::getenv / env_truthy / env_size wrapping) then
// x points at the literal name, every `if (x)` check passes, every
// `x[0] == '1'` check fails (the leading byte is 'H'), and the flag
// silently appears default-off in all configurations. The bug
// compiles, links, and may pass unit tests where the env var is
// intentionally unset or set to a non-'1' truthy value. Always wrap
// HF_FLAG_NAME_* macros in one of:
//   std::getenv(HF_FLAG_NAME_<NAME>)
//   env_truthy (HF_FLAG_NAME_<NAME>)
//   env_size   (HF_FLAG_NAME_<NAME>, fallback)
// or call a higher-level helper that does so internally. Never bind
// the macro to a `const char*` variable directly without one of those
// wraps.
//
// Track-cache-op-memo rationale (HF-internal LRU eviction + master
// switch family): the flags gate operator-result memoization across
// the rat-add / LF / PF / transform / reduce ops, plus RSS-pressure
// driven LRU eviction (§E iter-7). The macros are scoped to core/
// because operator_memo lives at src/core/operator_memo.cpp and is
// the sole TU consuming the flags.

#pragma once

// Track-cache-op-memo (14 flags; see docs/env_flags.md §2 "Track-cache-op-memo").
// Each macro expands to the string literal for the env var name.
#define HF_FLAG_NAME_OPERATOR_MEMO                    "HF_OPERATOR_MEMO"
#define HF_FLAG_NAME_OPERATOR_MEMO_COLLISION_LOG      "HF_OPERATOR_MEMO_COLLISION_LOG"
#define HF_FLAG_NAME_OPERATOR_MEMO_ENABLE_REDUCE      "HF_OPERATOR_MEMO_ENABLE_REDUCE"
#define HF_FLAG_NAME_OPERATOR_MEMO_LRU_CAP_PER_OP     "HF_OPERATOR_MEMO_LRU_CAP_PER_OP"
#define HF_FLAG_NAME_OPERATOR_MEMO_OFF_LF             "HF_OPERATOR_MEMO_OFF_LF"
#define HF_FLAG_NAME_OPERATOR_MEMO_OFF_PF             "HF_OPERATOR_MEMO_OFF_PF"
#define HF_FLAG_NAME_OPERATOR_MEMO_OFF_RAT_ADD        "HF_OPERATOR_MEMO_OFF_RAT_ADD"
#define HF_FLAG_NAME_OPERATOR_MEMO_OFF_REDUCE         "HF_OPERATOR_MEMO_OFF_REDUCE"
#define HF_FLAG_NAME_OPERATOR_MEMO_OFF_TRANSFORM      "HF_OPERATOR_MEMO_OFF_TRANSFORM"
#define HF_FLAG_NAME_OP_MEMO_EVICT_LRU_BATCH          "HF_OP_MEMO_EVICT_LRU_BATCH"
#define HF_FLAG_NAME_OP_MEMO_EVICT_ON_RSS             "HF_OP_MEMO_EVICT_ON_RSS"
#define HF_FLAG_NAME_OP_MEMO_EVICT_RSS_THRESHOLD_MB   "HF_OP_MEMO_EVICT_RSS_THRESHOLD_MB"
#define HF_FLAG_NAME_OP_MEMO_EVICT_STRATEGY           "HF_OP_MEMO_EVICT_STRATEGY"
#define HF_FLAG_NAME_OP_MEMO_EVICT_TRACE              "HF_OP_MEMO_EVICT_TRACE"
