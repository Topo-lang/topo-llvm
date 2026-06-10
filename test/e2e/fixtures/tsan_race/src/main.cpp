// tsan_race — host code that VIOLATES the .topo parallel declaration.
//
// The .topo declares bump_a / bump_b as same-stage siblings (parallel-safe);
// both bodies below do unsynchronized read-modify-writes on the same global
// counter. Built with [parallel] mode = "force" + check = "off", the
// TopoParallelPass spawns them concurrently and the race is real — a
// ThreadSanitizer build must report it. Built with mode = "off" the calls
// stay sequential on one thread and the same binary runs TSan-clean.

#include <cstdio>

#ifdef _MSC_VER
#define TSAN_RACE_NOINLINE __declspec(noinline)
#else
#define TSAN_RACE_NOINLINE __attribute__((noinline))
#endif

// Shared mutable state — the declaration violation.
static long g_counter = 0;

// Opaque per-step RMW: noinline keeps the loop a real sequence of
// load-add-store on g_counter (the optimizer cannot collapse it to a single
// store, nor prove the pointer thread-local), so the two tasks overlap on many
// genuinely-racy accesses rather than a single write the detector might miss.
TSAN_RACE_NOINLINE
static void bump_once(long* p) {
    *p = *p + 1;
}

namespace racy {

int seed(int iters) {
    return iters;
}

int bump_a(int iters) {
    for (int i = 0; i < iters; ++i)
        bump_once(&g_counter);
    return static_cast<int>(g_counter);
}

int bump_b(int iters) {
    for (int i = 0; i < iters; ++i)
        bump_once(&g_counter);
    return static_cast<int>(g_counter);
}

int total(int a, int b) {
    return a + b;
}

// Pipeline orchestrator — separate statements so every ABI emits the calls
// in declaration order (call-argument evaluation order is unspecified).
int run_pair(int iters) {
    int n = seed(iters);
    int a = bump_a(n);
    int b = bump_b(n);
    return total(a, b);
}

} // namespace racy

int main() {
    // Many rounds of 400k unsynchronized RMWs per side. The wide, repeated
    // overlap window makes the forced-parallel build race deterministically
    // under TSan even against the runtime's work-helping await (which can
    // occasionally drain both tasks on one thread for a single round); the
    // serial build computes the exact total. Verified 80/80 race detections
    // on macos-14 arm64 with brew llvm@22 (single longest run ~1.1s).
    constexpr int kIters = 400000;
    constexpr int kRounds = 40;
    long last = 0;
    for (int r = 0; r < kRounds; ++r)
        last = racy::run_pair(kIters);
    std::printf("tsan_race: final counter=%ld last=%ld\n", g_counter, last);
    std::printf("tsan_race: done\n");
    return 0;
}
