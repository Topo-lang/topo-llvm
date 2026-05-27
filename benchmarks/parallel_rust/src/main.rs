// Benchmark: TopoParallelPass — parallel task execution.
//
// Friendly:   many independent tasks → parallel speedup on multi-core.
// Unfriendly: sequential dependency chain → no parallelism possible.

use std::ffi::c_void;
use parallel_rust::*;

fn friendly_work() -> i32 {
    const NTASKS: usize = 8;
    let mut results = [0i32; NTASKS];
    let mut tasks = [std::ptr::null_mut::<c_void>(); NTASKS];

    for i in 0..NTASKS {
        tasks[i] = unsafe {
            topo_task_spawn(heavy_work, &mut results[i] as *mut i32 as *mut c_void)
        };
    }
    for i in 0..NTASKS {
        unsafe { topo_task_await(tasks[i]) };
    }

    results.iter().sum()
}

fn unfriendly_work() -> i32 {
    let mut a = 0i32;
    heavy_work(&mut a as *mut i32 as *mut c_void);
    let mut b = 0i32;
    light_work(&mut b as *mut i32 as *mut c_void);
    a + b
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
    unsafe { topo_parallel_init(0) };

    // Correctness
    {
        let mut a = 0i32;
        let mut b = 0i32;
        unsafe {
            let t1 = topo_task_spawn(heavy_work, &mut a as *mut i32 as *mut c_void);
            let t2 = topo_task_spawn(light_work, &mut b as *mut i32 as *mut c_void);
            topo_task_await(t1);
            topo_task_await(t2);
        }
        println!("results: a={}, b={}", a, b);
        assert_eq!(b, 42);
    }

    const ROUNDS: usize = 5;
    const WARMUP: usize = 5;
    const ITERS: usize = 10;

    for _ in 0..WARMUP {
        std::hint::black_box(friendly_work());
        std::hint::black_box(unfriendly_work());
    }

    let friendly_us = benchmark(ROUNDS, ITERS, friendly_work);
    println!("RESULT_US_FRIENDLY={}", friendly_us);

    let unfriendly_us = benchmark(ROUNDS, ITERS, unfriendly_work);
    println!("RESULT_US_UNFRIENDLY={}", unfriendly_us);

    unsafe { topo_parallel_shutdown() };
}
