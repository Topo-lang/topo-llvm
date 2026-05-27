// loop_reduction — exercises LoopParallelizePass reduction-combine codegen.
//
// reduce_sum carries a single associative '+' reduction; reduce_combo
// carries two independent reductions ('+' and '*') in one loop. Under
// forced loop partitioning each partition accumulates a local partial
// from the operator identity and the parent serially folds the partials,
// applying the original init exactly once. Integer reductions make the
// parallel result bit-identical to the serial build, so vanilla and
// forced stdout must match exactly.

#include <cstdio>

static constexpr int NA = 256;
static constexpr int NB = 64;
static int arr_a[NA];
static int arr_sa[NB];
static int arr_sb[NB];

int init_data() {
    for (int i = 0; i < NA; ++i)
        arr_a[i] = i % 7;
    for (int i = 0; i < NB; ++i) {
        arr_sa[i] = i % 7;
        arr_sb[i] = (i % 5 == 0) ? 2 : 1; // bounded product: 2^13
    }
    return 0;
}

// Single '+' reduction over a partitionable loop.
int reduce_sum() {
    int s = 0;
    for (int i = 0; i < NA; ++i)
        s += arr_a[i];
    return s;
}

// Two independent reductions ('+' and '*') carried by one loop.
int reduce_combo() {
    int s = 0;
    int p = 1;
    for (int i = 0; i < NB; ++i) {
        s += arr_sa[i];
        p *= arr_sb[i];
    }
    return s * 31 + p;
}

int run_test() {
    init_data();
    int a = reduce_sum();
    int b = reduce_combo();
    return a * 1000003 + b;
}

int main() {
    std::printf("loop_reduction: result=%d\n", run_test());
    std::printf("loop_reduction: ok\n");
    return 0;
}
