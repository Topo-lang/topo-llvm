#ifndef TOPO_PARALLEL_RT_H
#define TOPO_PARALLEL_RT_H

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
/// Consumers obtain the runtime's actual version via topo_parallel_version()
/// at process start and compare against TOPO_PARALLEL_ABI_VERSION; mismatch
/// is a hard error. See topo-llvm/runtime/ABI-COMPAT.md for the compatibility
/// matrix and bump history.
///
/// v1: initial pinned baseline (topo_task_spawn/await, topo_parallel_init/
///     shutdown/ensure_init, topo_cost_begin/end).
#define TOPO_PARALLEL_ABI_VERSION 1

/// Report the runtime's compiled-in TOPO_PARALLEL_ABI_VERSION. Consumers
/// (typically a one-time ctor injected by TopoParallelPass) call this and
/// abort on mismatch with their own TOPO_PARALLEL_ABI_VERSION.
uint32_t topo_parallel_version(void);

/// Opaque task handle
typedef struct topo_task topo_task_t;

/// Spawn a fire-and-forget task.
/// fn(arg) is executed on a worker thread.
/// The returned handle owns a heap allocation that MUST be released exactly
/// once: either by awaiting it (topo_task_await / topo_task_await_all) or, for
/// genuine fire-and-forget use where the result is never awaited, by
/// topo_task_detach(). Dropping the handle without either leaks the task.
topo_task_t* topo_task_spawn(void (*fn)(void*), void* arg);

/// Spawn a task that writes a result.
/// fn(arg, result_buf) is executed on a worker thread.
/// result_buf must point to at least result_size bytes, valid until await.
topo_task_t* topo_task_spawn_ret(void (*fn)(void*, void*), void* arg, void* result_buf, size_t result_size);

/// Spawn a task with scheduling priority.
/// priority: 0=Critical, 1=High, 2=Normal, 3=Low, 4=Background.
/// Higher-priority tasks are dequeued first by worker threads.
topo_task_t* topo_task_spawn_ret_pri(
    void (*fn)(void*, void*), void* arg, void* result_buf, size_t result_size, int priority);

/// Block until task completes. Frees the task handle.
void topo_task_await(topo_task_t* task);

/// Block until all tasks complete. Frees all task handles.
void topo_task_await_all(topo_task_t** tasks, int count);

/// Relinquish a spawned task handle without blocking ("fire-and-forget").
/// Ownership of the handle's allocation transfers to the runtime: the task is
/// freed when its body completes (or immediately, if it already has). After
/// this call the handle is invalid — do not touch it or pass it to
/// topo_task_await*. Use this instead of dropping a handle you will never
/// await, which would leak the task.
void topo_task_detach(topo_task_t* task);

/// Initialize the parallel runtime with the given number of threads.
/// Pass 0 to use hardware concurrency.
void topo_parallel_init(int num_threads);

/// Shut down the parallel runtime and release resources.
void topo_parallel_shutdown(void);

/// Ensure the runtime is initialized (call_once lazy init).
void topo_parallel_ensure_init();

/// Cost sampling: record begin timestamp for a named function.
void topo_cost_begin(const char* func_name);

/// Cost sampling: record end timestamp and accumulate duration.
void topo_cost_end(const char* func_name);

#ifdef __cplusplus
}
#endif

#endif // TOPO_PARALLEL_RT_H
