#ifndef TOPO_RT_JIT_ENGINE_RT_H
#define TOPO_RT_JIT_ENGINE_RT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version — bump when function signatures change.
/// v2: added topo_jit_engine_specialize_bytes + topo_jit_engine_dump_ir_bytes
///     so callers without an embedded .topo_ir section (primarily the
///     runtime unit tests) can feed bitcode directly.  The bytes
///     entrypoints are optional — older engines without them still
///     load via the v1 path.
#define TOPO_JIT_ENGINE_ABI_VERSION 2

/// DLL export/import macro for the engine shared library.
#if defined(TOPO_JIT_ENGINE_BUILDING)
#ifdef _WIN32
#define TOPO_JIT_ENGINE_API __declspec(dllexport)
#else
#define TOPO_JIT_ENGINE_API __attribute__((visibility("default")))
#endif
#else
#define TOPO_JIT_ENGINE_API
#endif

/// Return the engine ABI version. Used by topo-jit-api to verify compatibility.
TOPO_JIT_ENGINE_API uint32_t topo_jit_engine_version(void);

/// Check if the JIT engine is operational (LLVM initialized, OrcJIT ready).
/// Returns 1 if available, 0 otherwise.
TOPO_JIT_ENGINE_API int topo_jit_engine_available(void);

/// Specialize a pipeline function.
/// @param name       Pipeline qualified name (e.g. "sim::tick")
/// @param ctx_json   JSON-serialized Context constraints
/// @param ctx_len    Length of ctx_json
/// @param costs_json JSON-serialized cost samples map
/// @param costs_len  Length of costs_json
/// @return Function pointer to the specialized version, or NULL on failure.
TOPO_JIT_ENGINE_API void* topo_jit_engine_specialize(
    const char* name, const char* ctx_json, size_t ctx_len, const char* costs_json, size_t costs_len);

/// Dump the embedded IR as a text string.
/// @param name       Pipeline qualified name
/// @param ctx_json   JSON-serialized Context constraints
/// @param ctx_len    Length of ctx_json
/// @return Heap-allocated IR string (caller must free via topo_jit_engine_free_string)
TOPO_JIT_ENGINE_API char* topo_jit_engine_dump_ir(const char* name, const char* ctx_json, size_t ctx_len);

/// Free a string returned by topo_jit_engine_dump_ir.
/// Must be called to free memory allocated by the engine DLL.
TOPO_JIT_ENGINE_API void topo_jit_engine_free_string(char* str);

/// Specialize a pipeline function from caller-provided bitcode, bypassing
/// the .topo_ir / .tp_meta section reads.  Used by the runtime unit tests
/// (which do not link through topo-build and thus have no embedded IR).
/// @param name        Pipeline qualified name.
/// @param ir_data     LLVM bitcode bytes.
/// @param ir_size     Size of ir_data.
/// @param meta_json   JSON-serialized metadata (may be empty/NULL when no
///                    pipeline edge/stage metadata is needed).
/// @param meta_len    Length of meta_json.
/// @param ctx_json    JSON-serialized Context constraints.
/// @param ctx_len     Length of ctx_json.
/// @param costs_json  JSON-serialized cost samples map.
/// @param costs_len   Length of costs_json.
/// @return Function pointer to the specialized version, or NULL on failure.
TOPO_JIT_ENGINE_API void* topo_jit_engine_specialize_bytes(
    const char* name,
    const void* ir_data, size_t ir_size,
    const char* meta_json, size_t meta_len,
    const char* ctx_json, size_t ctx_len,
    const char* costs_json, size_t costs_len);

/// Dump caller-provided bitcode as a textual IR string.  Mirror of
/// topo_jit_engine_dump_ir but does not read any process section.
/// @return Heap-allocated IR string (caller frees via topo_jit_engine_free_string)
TOPO_JIT_ENGINE_API char* topo_jit_engine_dump_ir_bytes(
    const void* ir_data, size_t ir_size);

#ifdef __cplusplus
}
#endif

#endif // TOPO_RT_JIT_ENGINE_RT_H
