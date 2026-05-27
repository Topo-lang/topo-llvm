/// Pipeline entry — body will be replaced by PipelineCodeGenPass.
#[no_mangle]
#[inline(never)]
pub fn process(input: i32) -> i32 {
    let loaded = load(input);
    let decoded = decode(loaded);
    let enhanced = enhance(decoded);
    let detected = detect(decoded);
    compose(enhanced, detected)
}

#[no_mangle]
#[inline(never)]
pub fn load(input: i32) -> i32 {
    input * 2
}

#[no_mangle]
#[inline(never)]
pub fn decode(data: i32) -> i32 {
    data + 10
}

#[no_mangle]
#[inline(never)]
pub fn enhance(pixels: i32) -> i32 {
    pixels + 5
}

#[no_mangle]
#[inline(never)]
pub fn detect(pixels: i32) -> i32 {
    pixels * 3
}

#[no_mangle]
#[inline(never)]
pub fn compose(enhanced: i32, detected: i32) -> i32 {
    enhanced + detected
}
