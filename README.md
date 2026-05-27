# topo-llvm

LLVM backend for the [Topo](https://github.com/topo-lang) toolchain:

- **17 LLVM IR passes** under `lib/Transforms/` (TopoParallel,
  PipelineCodeGen, DataLayout, IndirectionPass, AdaptiveDispatch,
  LifetimeArena, Observability, SymbolObfuscator, ...).
- **Backend orchestration** under `lib/Backend/` (SymbolMapper, Verifier,
  PassPipeline, VariantBenchmark, LLVMTransformBackend).
- **C-ABI runtime libraries** under `runtime/` — the call targets of
  pass-generated calls (`topo-parallel`, `topo-jit-api`, `topo-adaptive`,
  `topo-arena`, `topo-observe`, `topo-containment`, `topo-pass-event`).
- **JIT engine** (`lib/JitEngine/`, `topo-jit-engine` shared library).
- **LLVMLifter** (`lib/Decompile/`) — IR → TranspileModel reverse path.
- **LLVM-bound `topo-prof`** pieces under `lib/Profile/Llvm/`.
- **Backend tools** under `tools/`: `topo-build-llvm-cpp`,
  `topo-build-llvm-rust`, `topo-build-llvm-mixed`, `topo-prof`
  (optional — gated, see below).
- **41 benchmark projects** under `benchmarks/` (optional — gated).

## Dependencies

Upstream (find_package):

- [topo-core](https://github.com/topo-lang/topo-core) — frontend
  (Lexer/Parser/Sema/Analysis/Check/Build/Transpile/Profile/Platform)
- [LLVM](https://llvm.org/) — auto-detected via Homebrew `llvm@<major>`
  or the bundled `llvm-dev/` tarball

Build tools (optional):

- [topo-lang-cpp](https://github.com/topo-lang/topo-lang-cpp) — only
  when `-DTOPO_LLVM_BUILD_TOOLS=ON`, and requires that package to be
  installed with its `TOPO_LANG_CPP_ENABLE_LLVM=ON` gate
- [topo-lang-rust](https://github.com/topo-lang/topo-lang-rust) — same
  as above with the Rust gate

Header-only / external:

- [nlohmann-json](https://github.com/nlohmann/json)
- [tomlplusplus](https://github.com/marzer/tomlplusplus) (vcpkg)

## Build options

| Option | Default | Effect |
|---|---|---|
| `TOPO_LLVM_BUILD_TESTS` | `ON` | Unit + integration GTest suites |
| `TOPO_LLVM_BUILD_TOOLS` | `OFF` | `topo-build-llvm-{cpp,rust,mixed}` |
| `TOPO_LLVM_BUILD_PROF` | `OFF` | `topo-prof` CLI |
| `TOPO_LLVM_BUILD_BENCHMARKS` | `OFF` | 41 benchmark projects |

## Bundled LLVM toolchain

The `llvm-dev/` directory holds a pinned LLVM/Clang dev distribution
(see `.llvm-version` for the version). It is **not committed to git**
(~few GB). To populate it:

```sh
bash scripts/setup-llvm.sh
```

Alternatively, install LLVM via Homebrew (`brew install llvm@<major>`);
`cmake/llvm-detect.cmake` finds it preferentially because Homebrew
ships a shared `libLLVM.dylib` that drops relink time and per-binary
size.

## Build

```sh
# Configure
cmake -S . -B build -G Ninja \
    -DCMAKE_PREFIX_PATH=<path with topo-core install> \
    -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake

# Build
cmake --build build

# Test
ctest --test-dir build --output-on-failure --timeout 120

# Install
cmake --install build --prefix /tmp/topo-llvm-install
```

## Downstream usage

```cmake
find_package(topo-llvm CONFIG REQUIRED)

add_executable(my_app main.cpp)
# Link a runtime lib (no LLVM transitive pull):
target_link_libraries(my_app PRIVATE topo::llvm::topo-arena)
# Or link a pass library (transitively requires LLVM):
# target_link_libraries(my_app PRIVATE topo::llvm::TopoTransforms)
```

## License

See [LICENSE](./LICENSE).
