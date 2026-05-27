#[no_mangle]
#[inline(never)]
pub fn parse(input: i32) -> i32 {
    let mut acc = input;
    for i in 0..1000i32 {
        acc = acc ^ (acc << 1) ^ i;
    }
    acc
}

#[no_mangle]
#[inline(never)]
pub fn validate(data: i32) -> i32 {
    let mut acc = data;
    for i in 0..1000i32 {
        acc = acc ^ (acc << 1) ^ i;
    }
    acc
}

#[no_mangle]
#[inline(never)]
pub fn transform(data: i32) -> i32 {
    let mut acc = data;
    for i in 0..1000i32 {
        acc = acc ^ (acc << 1) ^ i;
    }
    acc
}

#[no_mangle]
#[inline(never)]
pub fn execute(input: i32) -> i32 {
    let a = parse(input);
    let b = validate(a);
    transform(b)
}
