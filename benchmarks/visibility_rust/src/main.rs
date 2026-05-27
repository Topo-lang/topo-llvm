// Benchmark: TopoInlinePass + TopoFlattenPass — visibility-based inlining.
//
// Friendly:   deep call chain through protected/private functions (in lib.rs).
// Unfriendly: only call public functions — no extra inlining opportunity.

use visibility_rust::*;

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
    const ITERS: usize = 500;

    // Correctness (public API only — private symbols are optimized away by Topo)
    {
        let sum = add(3, 4);
        assert_eq!(sum, 7);
        assert_eq!(get_last_result(), 7);
        let diff = subtract(10, 3);
        assert_eq!(diff, 7);
        assert_eq!(get_last_result(), 7);
        println!("visibility_rust: all assertions passed");
    }

    for _ in 0..WARMUP {
        std::hint::black_box(friendly_work(0));
        std::hint::black_box(unfriendly_work());
    }

    let friendly_us = benchmark(ROUNDS, ITERS, || friendly_work(0));
    println!("RESULT_US_FRIENDLY={}", friendly_us);

    let unfriendly_us = benchmark(ROUNDS, ITERS, unfriendly_work);
    println!("RESULT_US_UNFRIENDLY={}", unfriendly_us);
}
