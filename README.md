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

**On macOS, prefer Homebrew** (`brew install llvm@<major>`).
`cmake/llvm-detect.cmake` finds it preferentially because Homebrew ships a
shared `libLLVM.dylib` that drops relink time and per-binary size. The
prebuilt bundled tarball is **not verified on macOS** and can fail at link
time with `dyld: Symbol not found: __ZdaPv` / `Abort trap: 6` (a libc++ ABI
mismatch). On macOS `setup-llvm.sh` detects a Homebrew `llvm@<major>` and
skips the bundled download; if none is found it warns before falling back to
the unverified tarball. On Linux and Windows the bundled tarball is the
default path.

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

### Windows notes

**ABI / compiler requirement.** The installed runtime libraries
(`topo::llvm::topo-arena` and the others) are built with the **MSVC ABI**
(bundled clang, or MSVC `cl.exe`). Your consumer build must use an
ABI-compatible compiler — otherwise the linker reports unresolved
`operator new` / `operator delete` symbols. If a MinGW/GNU linker sits
earlier on `PATH`, CMake may select it silently, so pin the bundled clang
before `find_package`:

```cmake
set(CMAKE_C_COMPILER   "<llvm-dev>/bin/clang.exe")
set(CMAKE_CXX_COMPILER "<llvm-dev>/bin/clang++.exe")
find_package(topo-llvm CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE topo::llvm::topo-arena)
```

The installed `topo-llvmConfig.cmake` emits a warning when it is loaded on
Windows with a non-Clang / non-MSVC compiler.

**Test / JIT coverage.** The full unit + integration test suite and the JIT
engine (`topo-jit-engine`) build and run on all three CI platforms, **Windows
included**. The earlier `lld-link` failure — it could not expand the response
file referencing imported `topo::core::*` targets — was an ABI + CRT mismatch:
the upstream `topo-core` / `topo-lang` / `topo-lang-cpp` builds defaulted to the
GNU ABI and the dynamic CRT, while `topo-llvm` links MSVC-ABI, static-CRT
objects. CI pins those upstream builds to the same bundled clang and
`CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`; with that, `lld-link` resolves the
`topo::core::*` symbols and the ~400 cases — including the JIT-engine
integration tests — pass on `windows-2022`. Apply the same bundled-clang +
static-CRT pin to reproduce the Windows test build locally.

## License

See [LICENSE](./LICENSE).
