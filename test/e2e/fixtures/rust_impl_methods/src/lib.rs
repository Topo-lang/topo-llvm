// rust_impl_methods — struct with declared+implemented methods (see
// topo/main.topo for the declarations this implements).

pub struct Cart {
    pub n: i32,
}

impl Cart {
    pub fn new() -> Cart {
        Cart { n: 0 }
    }

    pub fn total(&self) -> i32 {
        self.n
    }

    pub fn add(&self, other: i32) -> i32 {
        self.n + other
    }
}
