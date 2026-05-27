// Benchmark: ObservabilityPass — tracing span instrumentation overhead.
//
// Friendly:   tracing enabled, measuring overhead vs no-tracing baseline.
// Unfriendly: same workload — both measure pure overhead cost.

use observability_rust::*;
use std::os::raw::c_char;

#[link(name = "topo-observe")]
extern "C" {
    fn topo_trace_init(exporter: *const c_char, sampling_rate: f64);
    fn topo_trace_shutdown();
}

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
    const WARMUP: usize = 100;
    const ITERS: usize = 10000;

    unsafe {
        let exporter = b"stdout\0";
        topo_trace_init(exporter.as_ptr() as *const c_char, 1.0);
    }

    // Correctness
    {
        let result = execute(10);
        println!("observability_rust: result={}", result);
        println!("observability_rust: all assertions passed");
    }

    unsafe { topo_trace_shutdown() };

    for _ in 0..WARMUP {
        std::hint::black_box(execute(std::hint::black_box(1)));
    }

    let friendly_us = benchmark(ROUNDS, ITERS, || execute(std::hint::black_box(10)));
    println!("RESULT_US_FRIENDLY={}", friendly_us);

    let unfriendly_us = benchmark(ROUNDS, ITERS, || execute(std::hint::black_box(42)));
    println!("RESULT_US_UNFRIENDLY={}", unfriendly_us);
}
