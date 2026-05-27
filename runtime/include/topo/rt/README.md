# C ABI Runtime Interface

These headers define the C ABI that compiler-generated code calls into.
They are **not** intended for direct use by application code.

- `parallel_rt.h` — Task spawn/await + cost sampling (used by TopoParallelPass)
- `adaptive_rt.h` — Adaptive dispatch registration (used by AdaptiveDispatchPass)

For the user-facing C++ API, use `topo/parallel.h`, `topo/jit.h`, and `topo/adaptive.h` instead.
