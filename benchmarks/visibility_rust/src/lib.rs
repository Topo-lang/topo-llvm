static mut LAST_RESULT: i32 = 0;

#[no_mangle]
#[inline(never)]
pub fn add(a: i32, b: i32) -> i32 {
    let r = a + b;
    log_operation(r);
    r
}

#[no_mangle]
#[inline(never)]
pub fn subtract(a: i32, b: i32) -> i32 {
    let r = a - b;
    log_operation(r);
    r
}

#[no_mangle]
#[inline(never)]
pub fn get_last_result() -> i32 {
    unsafe { LAST_RESULT }
}

#[no_mangle]
#[inline(never)]
pub fn log_operation(result: i32) {
    unsafe { LAST_RESULT = result };
}

#[no_mangle]
#[inline(never)]
pub fn reset_state() {
    set_value(0);
}

#[no_mangle]
#[inline(never)]
pub fn set_value(val: i32) {
    unsafe { LAST_RESULT = val };
}

/// Friendly: deep call chain through protected/private functions.
/// InlinePass gives internal linkage → enables cross-function inlining.
#[no_mangle]
#[inline(never)]
pub fn friendly_work(seed: i32) -> i32 {
    let mut sink = 0i32;
    for i in 0..10000 {
        let input = std::hint::black_box(i + seed);
        let r = add(input, input + 1);
        let r = subtract(r, 1);
        sink = r;
        std::hint::black_box(sink);
    }
    sink
}

/// Unfriendly: only public functions — no extra inlining opportunity.
#[no_mangle]
#[inline(never)]
pub fn unfriendly_work() -> i32 {
    let mut sink = 0i32;
    for _ in 0..10000 {
        sink = get_last_result();
        std::hint::black_box(sink);
    }
    sink
}
