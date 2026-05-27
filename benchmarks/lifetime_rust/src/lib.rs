static mut G_ESCAPE: *mut u8 = std::ptr::null_mut();

#[no_mangle]
#[inline(never)]
pub fn allocate() -> i32 {
    42
}

#[no_mangle]
#[inline(never)]
pub fn process(input: i32) -> i32 {
    input * 3
}

#[no_mangle]
pub fn run_test() -> i32 {
    let a = allocate();
    let r = process(a);
    assert_eq!(r, 126);
    r
}

/// Friendly workload: many small fixed-size heap allocations.
/// Arena replaces individual Box::new (malloc) with bump allocation.
#[no_mangle]
#[inline(never)]
pub fn friendly_work(seed: i32) -> i32 {
    const N: usize = 100;
    let mut sum = seed;
    let mut ptrs: Vec<Box<[i32; 4]>> = Vec::with_capacity(N);
    for i in 0..N {
        let mut arr = Box::new([0i32; 4]);
        arr[0] = i as i32 + seed;
        arr[1] = i as i32 * 2;
        arr[2] = i as i32 * 3;
        arr[3] = i as i32 + 1;
        unsafe { G_ESCAPE = arr.as_ptr() as *mut u8 };
        ptrs.push(arr);
    }
    for p in &ptrs {
        sum += p[0] + p[3];
    }
    sum
}

/// Unfriendly workload: variable-size allocations — arena setup overhead dominates.
#[no_mangle]
#[inline(never)]
pub fn unfriendly_work(seed: i32) -> i32 {
    let mut sum = seed;
    for i in 1..=64usize {
        let mut v: Vec<i32> = vec![0; i];
        unsafe { G_ESCAPE = v.as_ptr() as *mut u8 };
        for j in 0..i {
            v[j] = j as i32 + seed;
        }
        sum += v[i - 1];
    }
    sum
}
