#[no_mangle]
pub extern "C" fn rust_add(a: i32, b: i32) -> i32 {
    helper(a) + helper(b)
}

fn helper(x: i32) -> i32 {
    x * 3 + 1
}
