// engine.cpp — mixed_lang benchmark workloads.

#include "engine.h"
#include <cstdio>

namespace engine {

void helper() {
    // internal helper — private visibility
}

int compute(int x, int y) {
    // Delegate to Rust implementation via extern "C"
    return rust_compute(x, y);
}

void run() {
    helper();
    int result = compute(10, 20);
    std::printf("mixed_lang: compute(10, 20) = %d\n", result);
}

// ---------------------------------------------------------------------------
// Friendly workload: tiny Rust function called in tight loop.
// With LLVM IR merge, rust_add() is inlined into the C++ loop body,
// eliminating per-call overhead. Without merge, each iteration pays
// a cross-module call cost.
// ---------------------------------------------------------------------------

int bench_friendly(int n) {
    int acc = 0;
    for (int i = 0; i < n; ++i) {
        acc = rust_add(acc, i);
    }
    return acc;
}

// ---------------------------------------------------------------------------
// Unfriendly workload: single call to a heavy Rust function.
// The function body dominates runtime; cross-language call overhead
// is negligible regardless of inlining.
// ---------------------------------------------------------------------------

int bench_unfriendly(int n) {
    return rust_heavy_compute(n);
}

} // namespace engine
