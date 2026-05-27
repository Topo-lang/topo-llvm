# 02 — Stages

Demonstrates fn blocks and stage annotations for ordered initialization.

## Features

- `fn` blocks with `stage<N>` annotations
- Same-stage logical parallelism (`stage<1>` has two operations)
- Stage ordering guarantees (stage 1 -> stage 2 -> stage 3)
- `const` parameter in private function

## Build (pure C++)

```bash
cmake -B build && cmake --build build
./build/stages
```

## Build (with Topo)

```bash
topo-build
```
