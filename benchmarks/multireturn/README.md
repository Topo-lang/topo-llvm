# 04 — Multi-Return Values

Demonstrates Topo's multi-return value system with color space conversion.

## Features

- Multi-return declarations: `rgb_to_hsl(int r, int g, int b) -> (int h, int s, int l);`
- Arrow binding with destructuring: `rgb_to_hsl() -> (h, s, l);`
- `topo::tuple<int, int, int>` for C++ implementation
- Structured bindings in C++ code
- `clamp()` as protected helper with Rust-style return

## Build (pure C++)

```bash
cmake -B build && cmake --build build
./build/multi_return
```

## Build (with Topo)

```bash
topo-build
```
