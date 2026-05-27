# 05 — Pipeline

Demonstrates Topo's pipeline data flow orchestration for image processing.

## Features

- Pipeline fn block with DAG edges: `load -> decode; decode -> enhance; ...`
- Fork: `decode` outputs to both `enhance` and `detect`
- Join: both `enhance` and `detect` feed into `compose`
- Terminal edge: `compose -> std::cpp17::int;`
- `TOPO_PIPELINE` macro for stub function declaration
- `topo/pipeline.h` runtime header

## Build (pure C++)

```bash
cmake -B build && cmake --build build
./build/pipeline
```

## Build (with Topo)

```bash
topo-build
```
