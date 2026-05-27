use std::ffi::c_void;

#[no_mangle]
pub extern "C" fn heavy_work(arg: *mut c_void) {
    let name = b"heavy_work\0";
    unsafe { topo_cost_begin(name.as_ptr() as *const i8) };
    let mut sum: i32 = 0;
    for i in 0..1_000_000i32 {
        sum = sum.wrapping_add(i);
    }
    unsafe { *(arg as *mut i32) = sum };
    unsafe { topo_cost_end(name.as_ptr() as *const i8) };
}

#[no_mangle]
pub extern "C" fn light_work(arg: *mut c_void) {
    let name = b"light_work\0";
    unsafe { topo_cost_begin(name.as_ptr() as *const i8) };
    unsafe { *(arg as *mut i32) = 42 };
    unsafe { topo_cost_end(name.as_ptr() as *const i8) };
}

extern "C" {
    pub fn topo_parallel_init(num_threads: i32);
    pub fn topo_parallel_shutdown();
    pub fn topo_task_spawn(func: extern "C" fn(*mut c_void), arg: *mut c_void) -> *mut c_void;
    pub fn topo_task_await(task: *mut c_void);
    pub fn topo_cost_begin(func_name: *const i8);
    pub fn topo_cost_end(func_name: *const i8);
}
