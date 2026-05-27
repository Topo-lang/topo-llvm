// Benchmark: PipelineCodeGenPass — DAG-optimized pipeline code generation.
//
// Friendly:   pipeline with fork/join at scale.
// Unfriendly: manual sequential calls (no pipeline DAG benefit).

use pipeline_rust::*;

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

fn friendly_work() -> i32 {
    let mut sink = 0i32;
    for i in 0..10000 {
        sink = process(std::hint::black_box(i));
        std::hint::black_box(sink);
    }
    sink
}

fn unfriendly_work() -> i32 {
    // Manual sequential replication of the pipeline logic.
    // Protected symbols (load/decode/enhance/detect/compose) are inlined by Topo,
    // so we replicate their arithmetic here as the non-optimized baseline.
    let mut sink = 0i32;
    for i in 0..10000 {
        let input = std::hint::black_box(i);
        let loaded = input * 2;
        let decoded = loaded + 10;
        let enhanced = decoded + 5;
        let detected = decoded * 3;
        sink = enhanced + detected;
        std::hint::black_box(sink);
    }
    sink
}

fn main() {
    const ROUNDS: usize = 7;
    const WARMUP: usize = 50;
    const ITERS: usize = 100;

    // Correctness
    {
        let result = process(10);
        assert_eq!(result, 125);
        println!("pipeline_rust: result={}", result);
        println!("pipeline_rust: all assertions passed");
    }

    for _ in 0..WARMUP {
        std::hint::black_box(friendly_work());
        std::hint::black_box(unfriendly_work());
    }

    let friendly_us = benchmark(ROUNDS, ITERS, friendly_work);
    println!("RESULT_US_FRIENDLY={}", friendly_us);

    let unfriendly_us = benchmark(ROUNDS, ITERS, unfriendly_work);
    println!("RESULT_US_UNFRIENDLY={}", unfriendly_us);
}
