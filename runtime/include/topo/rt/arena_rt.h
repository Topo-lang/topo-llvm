#ifndef TOPO_RT_ARENA_RT_H
#define TOPO_RT_ARENA_RT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// ABI version — bump when any signature in this header changes (add/remove
/// parameter, change parameter or return type, change calling convention).
/// Adding a NEW entry point without touching existing ones is backward
/// compatible and does NOT require a bump.
///
/// Consumers obtain the runtime's actual version via topo_arena_version()
/// at process start and compare against TOPO_ARENA_ABI_VERSION; mismatch is
/// a hard error. See topo-llvm/runtime/ABI-COMPAT.md for the compatibility
/// matrix and bump history.
///
/// v1: initial pinned baseline (create, alloc, reset, destroy, bytes_used,
///     capacity).
#define TOPO_ARENA_ABI_VERSION 1

/// Report the runtime's compiled-in TOPO_ARENA_ABI_VERSION. Consumers
/// (typically a one-time ctor injected by LifetimeArenaPass) call this
/// and abort on mismatch with their own TOPO_ARENA_ABI_VERSION.
uint32_t topo_arena_version(void);

/// Opaque arena handle.
typedef struct topo_arena* topo_arena_t;

/// Create a new arena with the given initial capacity (bytes).
/// Returns NULL on allocation failure.
topo_arena_t topo_arena_create(size_t initial_capacity);

/// Allocate `size` bytes with `alignment` from the arena.
/// Returns NULL if the arena cannot satisfy the request.
void* topo_arena_alloc(topo_arena_t arena, size_t size, size_t alignment);

/// Reset the arena, making all previous allocations invalid.
/// Does not free underlying memory — reuses the buffer.
void topo_arena_reset(topo_arena_t arena);

/// Destroy the arena, freeing all underlying memory.
void topo_arena_destroy(topo_arena_t arena);

/// Query total bytes allocated from the arena.
size_t topo_arena_bytes_used(topo_arena_t arena);

/// Query the arena's total capacity.
size_t topo_arena_capacity(topo_arena_t arena);

#ifdef __cplusplus
}
#endif

#endif // TOPO_RT_ARENA_RT_H
