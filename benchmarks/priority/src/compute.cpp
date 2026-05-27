#include "compute.h"

namespace compute {

void internal_step() {
    volatile int acc = 0;
    for (int i = 0; i < 100; ++i)
        acc = acc ^ (acc << 1) ^ (i * 17 + 3);
}

void critical_helper(int n) {
    volatile int acc = 0;
    for (int i = 0; i < n; ++i)
        acc = acc ^ (acc << 1) ^ (i * 7 + 1);
}

void run_critical_path() {
    // Critical priority — calls critical_helper (propagation test)
    critical_helper(200);
    internal_step();
}

void process_data(int size) {
    volatile int acc = 0;
    for (int i = 0; i < size; ++i)
        acc = acc ^ (acc << 1) ^ (i * 11 + 5);
}

void run_pipeline() {
    run_critical_path();
    process_data(150);
    cleanup();
    log_metrics();
}

void cleanup() {
    volatile int acc = 0;
    for (int i = 0; i < 50; ++i)
        acc = acc ^ (acc << 1) ^ (i * 3 + 2);
}

void log_metrics() {
    volatile int acc = 0;
    for (int i = 0; i < 30; ++i)
        acc = acc ^ (acc << 1) ^ (i * 5 + 4);
}

} // namespace compute
