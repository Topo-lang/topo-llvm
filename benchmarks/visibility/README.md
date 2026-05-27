# 01 — Hello Visibility

Demonstrates Topo's three-level visibility system using a simple calculator.

## Features

- `public` / `protected` / `private` visibility sections
- Nested namespaces (`calc::core::impl`)
- C++ style declarations (`int add(int a, int b);`)
- Rust style declarations (`subtract(int a, int b) -> int;`)
- `const` return type (`const int& get_last_result();`)
- Pointer parameters (`void set(int* out, int val);`)

## Build (pure C++)

```bash
cmake -B build && cmake --build build
./build/hello_visibility
```

## Build (with Topo)

```bash
topo-build
```
