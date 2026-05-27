/// Original correctness function — small loop.
#[no_mangle]
pub extern "C" fn rust_compute(x: i32, y: i32) -> i32 {
    let mut result = x + y;
    for i in 0..x {
        result += i % 3;
    }
    result
}

/// Small function — benefits from cross-language inlining.
/// With LLVM IR merge the function body is inlined into the C++ caller,
/// eliminating per-call overhead.  The XOR prevents SCEV from reducing
/// the accumulation loop to a closed-form formula.
#[no_mangle]
pub extern "C" fn rust_add(x: i32, y: i32) -> i32 {
    x.wrapping_add(y) ^ (y & 0xFF)
}

/// Heavy computation where cross-language call overhead is negligible.
/// Iterates `iterations` times with arithmetic that prevents trivial
/// constant-folding while keeping the hot loop in Rust.
#[no_mangle]
pub extern "C" fn rust_heavy_compute(iterations: i32) -> i32 {
    let mut acc: i32 = 0;
    for i in 0..iterations {
        acc = acc.wrapping_add(i.wrapping_mul(7));
        acc ^= acc >> 3;
    }
    acc
}
