// Benchmark: LifetimeArenaPass — arena allocation.
//
// Friendly:   many small allocations within lifetime scope (in lib.rs).
// Unfriendly: variable-size allocations — arena setup overhead dominates.

use lifetime_rust::*;

fn benchmark<F: Fn() -> i32>(rounds: usize, iters: usize, work: F) -> u64 {
    let mut samples = Vec::with_capacity(rounds);
    for _ in 0..rounds {
        let start = std::time::Instant::now();
        let mut sink: i32 = 0;
        for _ in 0..iters {
            sink = work();
        }
        std::hint::black_box(sink);
        samples.push(start.elapsed().as_micros() as u64);
    }
    samples.sort();
    samples[rounds / 2]
}

fn main() {
    const ROUNDS: usize = 7;
    const WARMUP: usize = 50;
    const ITERS: usize = 50000;

    // Correctness
    let result = run_test();
    println!("lifetime_rust: result={}", result);
    println!("lifetime_rust: all assertions passed");

    for _ in 0..WARMUP {
        std::hint::black_box(friendly_work(1));
        std::hint::black_box(unfriendly_work(1));
    }

    let friendly_us = benchmark(ROUNDS, ITERS, || friendly_work(1));
    println!("RESULT_US_FRIENDLY={}", friendly_us);

    let unfriendly_us = benchmark(ROUNDS, ITERS, || unfriendly_work(1));
    println!("RESULT_US_UNFRIENDLY={}", unfriendly_us);
}
