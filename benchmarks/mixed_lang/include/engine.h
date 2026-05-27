#pragma once

// Forward declare Rust functions (extern "C" from Rust side)
extern "C" int rust_compute(int x, int y);
extern "C" int rust_add(int x, int y);
extern "C" int rust_heavy_compute(int iterations);

namespace engine {
void run();
int compute(int x, int y);
void helper();

// Benchmark workloads
int bench_friendly(int n);
int bench_unfriendly(int n);
} // namespace engine
