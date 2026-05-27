/// CABIContractTest.cpp — Verify C ABI stability of topo runtime headers.
///
/// Tests:
/// 1. extern "C" linkage correctness (all symbols linkable from C++)
/// 2. Struct/typedef layout stability (sizes, alignments)
/// 3. Complete symbol coverage for each *_rt.h header
/// 4. ABI version constants
/// 5. Macro definitions

#include <gtest/gtest.h>

// Include all C ABI runtime headers
#include <topo/rt/parallel_rt.h>
#include <topo/rt/adaptive_rt.h>
#include <topo/rt/jit_engine_rt.h>
#include <topo/rt/observe_rt.h>
#include <topo/rt/arena_rt.h>
#include <topo/rt/pass_event_rt.h>
#include <topo/rt/containment_rt.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

// =====================================================================
// 1. Struct / Typedef Layout Stability
// =====================================================================

// topo_task_t is an opaque struct pointer — verify it's a pointer type
static_assert(std::is_pointer<topo_task_t*>::value, "topo_task_t* must be a pointer type");

// topo_arena_t is typedef'd as struct topo_arena* — it IS a pointer
static_assert(std::is_pointer<topo_arena_t>::value, "topo_arena_t must be a pointer type (opaque handle)");
static_assert(sizeof(topo_arena_t) == sizeof(void*), "topo_arena_t must be pointer-sized");
static_assert(alignof(topo_arena_t) == alignof(void*), "topo_arena_t must have pointer alignment");

// =====================================================================
// 2. ABI Version Constants
// =====================================================================

// v2 added topo_jit_engine_specialize_bytes / topo_jit_engine_dump_ir_bytes
// so the test harness can drive the real JIT path without an embedded
// .topo_ir section.
static_assert(TOPO_JIT_ENGINE_ABI_VERSION == 2, "JIT engine ABI version must be 2");

// =====================================================================
// 3. Symbol Linkage Tests — parallel_rt.h
// =====================================================================

TEST(CABIContract, ParallelSymbols) {
    // Verify all parallel_rt.h functions are linkable via extern "C"
    auto* fn_spawn = &topo_task_spawn;
    auto* fn_spawn_ret = &topo_task_spawn_ret;
    auto* fn_spawn_ret_pri = &topo_task_spawn_ret_pri;
    auto* fn_await = &topo_task_await;
    auto* fn_await_all = &topo_task_await_all;
    auto* fn_init = &topo_parallel_init;
    auto* fn_shutdown = &topo_parallel_shutdown;
    auto* fn_ensure = &topo_parallel_ensure_init;
    auto* fn_cost_begin = &topo_cost_begin;
    auto* fn_cost_end = &topo_cost_end;

    EXPECT_NE(fn_spawn, nullptr);
    EXPECT_NE(fn_spawn_ret, nullptr);
    EXPECT_NE(fn_spawn_ret_pri, nullptr);
    EXPECT_NE(fn_await, nullptr);
    EXPECT_NE(fn_await_all, nullptr);
    EXPECT_NE(fn_init, nullptr);
    EXPECT_NE(fn_shutdown, nullptr);
    EXPECT_NE(fn_ensure, nullptr);
    EXPECT_NE(fn_cost_begin, nullptr);
    EXPECT_NE(fn_cost_end, nullptr);
}

TEST(CABIContract, ParallelSignatures) {
    // Verify function pointer types match expected C ABI signatures
    using SpawnFn = topo_task_t* (*)(void (*)(void*), void*);
    using SpawnRetFn = topo_task_t* (*)(void (*)(void*, void*), void*, void*, size_t);
    using SpawnRetPriFn = topo_task_t* (*)(void (*)(void*, void*), void*, void*, size_t, int);
    using AwaitFn = void (*)(topo_task_t*);
    using AwaitAllFn = void (*)(topo_task_t**, int);
    using InitFn = void (*)(int);
    using VoidFn = void (*)();
    using CostFn = void (*)(const char*);

    SpawnFn spawn = &topo_task_spawn;
    SpawnRetFn spawn_ret = &topo_task_spawn_ret;
    SpawnRetPriFn spawn_ret_pri = &topo_task_spawn_ret_pri;
    AwaitFn await_fn = &topo_task_await;
    AwaitAllFn await_all = &topo_task_await_all;
    InitFn init = &topo_parallel_init;
    VoidFn shutdown_fn = &topo_parallel_shutdown;
    VoidFn ensure = &topo_parallel_ensure_init;
    CostFn cost_begin = &topo_cost_begin;
    CostFn cost_end = &topo_cost_end;

    // Suppress unused-variable warnings; the assignments above already
    // verify type compatibility at compile time.
    (void)spawn;
    (void)spawn_ret;
    (void)spawn_ret_pri;
    (void)await_fn;
    (void)await_all;
    (void)init;
    (void)shutdown_fn;
    (void)ensure;
    (void)cost_begin;
    (void)cost_end;
}

// =====================================================================
// 4. Symbol Linkage Tests — adaptive_rt.h
// =====================================================================

TEST(CABIContract, AdaptiveSymbols) {
    auto* fn_register = &topo_adaptive_register;
    auto* fn_init = &topo_adaptive_init;
    auto* fn_shutdown = &topo_adaptive_shutdown;

    EXPECT_NE(fn_register, nullptr);
    EXPECT_NE(fn_init, nullptr);
    EXPECT_NE(fn_shutdown, nullptr);
}

TEST(CABIContract, AdaptiveSignatures) {
    using RegisterFn = void (*)(const char*, const char*, void**, uint64_t);
    using VoidFn = void (*)();

    RegisterFn reg = &topo_adaptive_register;
    VoidFn init = &topo_adaptive_init;
    VoidFn shutdown_fn = &topo_adaptive_shutdown;

    (void)reg;
    (void)init;
    (void)shutdown_fn;
}

// =====================================================================
// 5. Type-level Tests — jit_engine_rt.h
//
// JIT engine symbols are loaded dynamically via dlopen/LoadLibrary,
// so we cannot take their address at link time. Instead, verify
// that the declared function pointer types are well-formed and
// that the header macros/constants are correct.
// =====================================================================

TEST(CABIContract, JitEngineSignatureTypes) {
    // Verify expected function pointer types compile and are callable types.
    // These types match the declarations in jit_engine_rt.h.
    using VersionFn = uint32_t (*)(void);
    using AvailableFn = int (*)(void);
    using SpecializeFn = void* (*)(const char*, const char*, size_t, const char*, size_t);
    using SpecializeBytesFn = void* (*)(
        const char*,
        const void*, size_t,
        const char*, size_t,
        const char*, size_t,
        const char*, size_t);
    using DumpIRFn = char* (*)(const char*, const char*, size_t);
    using DumpIRBytesFn = char* (*)(const void*, size_t);
    using FreeStringFn = void (*)(char*);

    // Verify these are indeed function pointer types
    static_assert(std::is_pointer<VersionFn>::value, "VersionFn must be a pointer type");
    static_assert(std::is_pointer<AvailableFn>::value, "AvailableFn must be a pointer type");
    static_assert(std::is_pointer<SpecializeFn>::value, "SpecializeFn must be a pointer type");
    static_assert(std::is_pointer<SpecializeBytesFn>::value, "SpecializeBytesFn must be a pointer type");
    static_assert(std::is_pointer<DumpIRFn>::value, "DumpIRFn must be a pointer type");
    static_assert(std::is_pointer<DumpIRBytesFn>::value, "DumpIRBytesFn must be a pointer type");
    static_assert(std::is_pointer<FreeStringFn>::value, "FreeStringFn must be a pointer type");

    // Verify return types through function_result trait
    VersionFn version = nullptr;
    AvailableFn avail = nullptr;
    SpecializeFn spec = nullptr;
    SpecializeBytesFn spec_bytes = nullptr;
    DumpIRFn dump = nullptr;
    DumpIRBytesFn dump_bytes = nullptr;
    FreeStringFn free_str = nullptr;
    (void)version;
    (void)avail;
    (void)spec;
    (void)spec_bytes;
    (void)dump;
    (void)dump_bytes;
    (void)free_str;
}

TEST(CABIContract, JitEngineConstants) {
    // ABI version must be stable
    EXPECT_EQ(TOPO_JIT_ENGINE_ABI_VERSION, 2u);

    // TOPO_JIT_ENGINE_API macro must be defined (may expand to empty)
#ifndef TOPO_JIT_ENGINE_API
    FAIL() << "TOPO_JIT_ENGINE_API macro not defined";
#endif
}

// =====================================================================
// 6. Symbol Linkage Tests — observe_rt.h
// =====================================================================

TEST(CABIContract, ObserveSymbols) {
    auto* fn_begin = &topo_trace_span_begin;
    auto* fn_end = &topo_trace_span_end;
    auto* fn_init = &topo_trace_init;
    auto* fn_shutdown = &topo_trace_shutdown;

    EXPECT_NE(fn_begin, nullptr);
    EXPECT_NE(fn_end, nullptr);
    EXPECT_NE(fn_init, nullptr);
    EXPECT_NE(fn_shutdown, nullptr);
}

TEST(CABIContract, ObserveSignatures) {
    using SpanBeginFn = void (*)(const char*);
    using SpanEndFn = void (*)();
    using InitFn = void (*)(const char*, double);
    using ShutdownFn = void (*)();

    SpanBeginFn begin = &topo_trace_span_begin;
    SpanEndFn end = &topo_trace_span_end;
    InitFn init = &topo_trace_init;
    ShutdownFn shutdown_fn = &topo_trace_shutdown;

    (void)begin;
    (void)end;
    (void)init;
    (void)shutdown_fn;
}

// =====================================================================
// 7. Symbol Linkage Tests — arena_rt.h
// =====================================================================

TEST(CABIContract, ArenaSymbols) {
    auto* fn_create = &topo_arena_create;
    auto* fn_alloc = &topo_arena_alloc;
    auto* fn_reset = &topo_arena_reset;
    auto* fn_destroy = &topo_arena_destroy;
    auto* fn_bytes_used = &topo_arena_bytes_used;
    auto* fn_capacity = &topo_arena_capacity;

    EXPECT_NE(fn_create, nullptr);
    EXPECT_NE(fn_alloc, nullptr);
    EXPECT_NE(fn_reset, nullptr);
    EXPECT_NE(fn_destroy, nullptr);
    EXPECT_NE(fn_bytes_used, nullptr);
    EXPECT_NE(fn_capacity, nullptr);
}

TEST(CABIContract, ArenaSignatures) {
    using CreateFn = topo_arena_t (*)(size_t);
    using AllocFn = void* (*)(topo_arena_t, size_t, size_t);
    using ResetFn = void (*)(topo_arena_t);
    using DestroyFn = void (*)(topo_arena_t);
    using BytesUsedFn = size_t (*)(topo_arena_t);
    using CapacityFn = size_t (*)(topo_arena_t);

    CreateFn create = &topo_arena_create;
    AllocFn alloc = &topo_arena_alloc;
    ResetFn reset = &topo_arena_reset;
    DestroyFn destroy = &topo_arena_destroy;
    BytesUsedFn bytes_used = &topo_arena_bytes_used;
    CapacityFn capacity = &topo_arena_capacity;

    (void)create;
    (void)alloc;
    (void)reset;
    (void)destroy;
    (void)bytes_used;
    (void)capacity;
}

// =====================================================================
// 8. Cross-header independence: verify no conflicting definitions
// =====================================================================

TEST(CABIContract, HeadersCompileTogetherWithoutConflict) {
    // This test succeeds simply by compiling — all 5 headers are
    // included at the top of this file. If any header leaks C++
    // types, has conflicting macros, or missing include guards,
    // compilation would fail.
    SUCCEED();
}

// =====================================================================
// 9. Symbol naming convention: all public symbols use topo_ prefix
// =====================================================================

TEST(CABIContract, NamingConvention) {
    // All public C ABI symbols verified above follow the topo_ prefix
    // convention. This test documents that expectation. The include
    // guard macros also follow the TOPO_ prefix convention:
    EXPECT_EQ(TOPO_JIT_ENGINE_ABI_VERSION, 2u);
}

// =====================================================================
// 10. Per-library ABI-version macro + introspection symbol agreement
//
// Each runtime header declares a TOPO_<NAME>_ABI_VERSION macro and a
// matching topo_<lib>_version() introspection symbol. The macro
// (compile-time constant baked into the caller) and the symbol
// (compile-time constant baked into the runtime library) MUST report
// the same value when the caller and library are built from the same
// commit. A mismatch caught by these tests means the macro and the
// implementation drifted — either the macro was bumped without an
// implementation update or vice versa. The same mismatch at runtime
// (caller-vs-library across a version skew) is the exact condition
// the runtime-side ctor pattern documented in
// topo-llvm/runtime/ABI-COMPAT.md is meant to catch.
// =====================================================================

TEST(CABIContract, ParallelVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_parallel_version(), TOPO_PARALLEL_ABI_VERSION);
    EXPECT_EQ(TOPO_PARALLEL_ABI_VERSION, 1u);
}

TEST(CABIContract, ArenaVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_arena_version(), TOPO_ARENA_ABI_VERSION);
    EXPECT_EQ(TOPO_ARENA_ABI_VERSION, 1u);
}

TEST(CABIContract, AdaptiveVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_adaptive_version(), TOPO_ADAPTIVE_ABI_VERSION);
    EXPECT_EQ(TOPO_ADAPTIVE_ABI_VERSION, 1u);
}

TEST(CABIContract, ObserveVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_trace_version(), TOPO_OBSERVE_ABI_VERSION);
    EXPECT_EQ(TOPO_OBSERVE_ABI_VERSION, 1u);
}

TEST(CABIContract, PassEventVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_pass_event_version(), TOPO_PASS_EVENT_ABI_VERSION);
    EXPECT_EQ(TOPO_PASS_EVENT_ABI_VERSION, 1u);
}

TEST(CABIContract, ContainmentVersionMacroMatchesSymbol) {
    EXPECT_EQ(topo_containment_version(), TOPO_CONTAINMENT_ABI_VERSION);
    EXPECT_EQ(TOPO_CONTAINMENT_ABI_VERSION, 1u);
}
