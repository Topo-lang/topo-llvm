#include "topo/parallel.h"
#include "topo/rt/parallel_rt.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================
// Internal types
// ============================================================

struct topo_task {
    std::function<void()> work;
    std::atomic<bool> done{false};
    std::mutex mtx;
    std::condition_variable cv;
    int priority = 2; // 0=Critical, 1=High, 2=Normal, 3=Low, 4=Background

    // Fire-and-forget ownership handoff (see topo_task_detach). When a caller
    // detaches a spawned task instead of awaiting it, ownership of the heap
    // allocation transfers to whichever side finishes last: if the body is
    // still running, the worker frees the task in run_task_safely after the
    // body completes; if the body already finished, the detaching caller frees
    // it. Both sides arbitrate under `mtx` so exactly one free happens. Without
    // this, a spawned-but-never-awaited task leaks (await was the only path
    // that freed the handle), contradicting the header's "fire-and-forget".
    bool detached = false;

    // If task->work() throws, the worker / stealer captures the exception
    // here, marks done=true, and notifies awaiters — instead of letting
    // the exception escape the worker loop (which previously left `done`
    // permanently false and `topo_task_await` spinning forever). The
    // awaiter rethrows the captured exception on the awaiting thread.
    // Exception-safety fix: keeps a thrown body from deadlocking awaiters.
    std::exception_ptr exception;
};

// ============================================================
// Cost sampling — thread-local ring buffer
// ============================================================

namespace {

struct CostSample {
    std::string name;
    uint64_t total_ns = 0;
    uint64_t count = 0;
};

// Thread-local pending begin timestamps
struct PendingBegin {
    std::string name;
    std::chrono::steady_clock::time_point tp;
};

static thread_local std::vector<PendingBegin> tls_pending_begins;

using SampleMap = std::unordered_map<std::string, CostSample>;

static std::mutex g_samples_mutex;
static std::vector<SampleMap*> g_tls_registrations;
// g_instrument is written under g_lifecycle_mutex in do_init() but read
// lock-free on every worker thread in topo_parallel_cost_{begin,end}_impl.
// A plain bool read concurrently with the write is a data race (UB); make it
// atomic. Release on the writer / acquire on the readers so a worker that sees
// the new value also sees the matching g_config publication.
static std::atomic<bool> g_instrument{true};

// Thread-local registration guard. The sample map lives INSIDE this object so
// a single thread_local owns both the map and its registration, eliminating the
// cross-object teardown window: previously `tls_samples` and `tls_registration`
// were independent thread_locals, and the standard destroys thread_locals in
// reverse order of construction completion. A worker enrols by touching
// `tls_registration` first (its enrol() then takes the address of the separate
// `tls_samples`, constructing it second), so `tls_samples` was destroyed BEFORE
// `tls_registration`'s destructor ran — leaving a dangling `&tls_samples`
// registered in g_tls_registrations for a window during which a concurrent
// aggregator (get_cost_samples / reset_cost_samples) could dereference a
// destroyed map. With the map embedded, the destructor first removes `&samples`
// from the registry under g_samples_mutex and only then is `samples` itself torn
// down, so no aggregator can ever observe a registered-but-destroyed map.
struct TlsSampleRegistration {
    SampleMap samples;
    bool registered = false;

    void enrol() {
        if (registered) return;
        std::lock_guard<std::mutex> lock(g_samples_mutex);
        g_tls_registrations.push_back(&samples);
        registered = true;
    }

    ~TlsSampleRegistration() {
        if (registered) {
            std::lock_guard<std::mutex> lock(g_samples_mutex);
            auto& v = g_tls_registrations;
            v.erase(std::remove(v.begin(), v.end(), &samples), v.end());
        }
        // `samples` is destroyed AFTER this body returns, i.e. after the
        // registration is removed under the lock — so the unregister
        // happens-before the map teardown.
    }
};

static thread_local TlsSampleRegistration tls_registration;

void register_tls_samples() {
    tls_registration.enrol();
}

} // anonymous namespace

// ============================================================
// Work-stealing thread pool
// ============================================================

namespace {

struct WorkerQueue {
    std::deque<topo_task*> tasks;
    std::mutex mtx;
};

struct ThreadPool {
    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<WorkerQueue>> queues;
    std::atomic<bool> shutdown_flag{false};
    std::atomic<uint64_t> submit_counter{0};
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    int num_workers = 0;

    void start(int n) {
        num_workers = n;
        queues.reserve(n);
        for (int i = 0; i < n; ++i)
            queues.push_back(std::make_unique<WorkerQueue>());
        workers.reserve(n);
        for (int i = 0; i < n; ++i) {
            workers.emplace_back([this, i]() { worker_loop(i); });
        }
    }

    void stop() {
        shutdown_flag.store(true, std::memory_order_release);
        wake_cv.notify_all();
        for (auto& w : workers) {
            if (w.joinable()) w.join();
        }
        workers.clear();
        queues.clear();
    }

    void submit(topo_task* task) {
        // Round-robin assignment
        uint64_t idx = submit_counter.fetch_add(1, std::memory_order_relaxed);
        int target = static_cast<int>(idx % num_workers);
        {
            std::lock_guard<std::mutex> lock(queues[target]->mtx);
            // High-priority tasks (Critical=0, High=1) go to front;
            // Normal and below go to back.
            if (task->priority < 2) {
                queues[target]->tasks.push_front(task);
            } else {
                queues[target]->tasks.push_back(task);
            }
        }
        wake_cv.notify_all();
    }

    // Try to pop a task from any worker queue. Used by `topo_task_await` to
    // implement work-helping: when a worker thread is awaiting a task from
    // inside another task body, parking on cv would deadlock if the target
    // task's completion depends on other queued tasks that only have this
    // worker to run them. Instead, help drain the pool.
    //
    // Steal order: LIFO from the back (matches worker_loop's stealing
    // discipline for cache locality). Starting index is pseudo-randomized so
    // concurrent helpers don't all hammer queue 0.
    topo_task* try_steal_any() {
        if (num_workers <= 0) return nullptr;
        uint64_t seed = submit_counter.fetch_add(1, std::memory_order_relaxed);
        int start = static_cast<int>(seed % num_workers);
        for (int i = 0; i < num_workers; ++i) {
            int victim = (start + i) % num_workers;
            std::lock_guard<std::mutex> lock(queues[victim]->mtx);
            if (!queues[victim]->tasks.empty()) {
                topo_task* t = queues[victim]->tasks.back();
                queues[victim]->tasks.pop_back();
                return t;
            }
        }
        return nullptr;
    }

    // Run a task body and *always* flip done+notify, even if the body
    // throws. Capturing the exception_ptr lets the awaiting thread
    // rethrow it (so a failure surfaces as a real error instead of a
    // silent hang). Exception-safety fix: prior to this,
    // an exception escaping work() left done==false forever, so
    // topo_task_await spun in its work-helping loop until the process
    // was killed — turning any unhandled task exception into a DoS on
    // the whole pipeline.
    static void run_task_safely(topo_task* task) {
        std::exception_ptr pending;
        try {
            task->work();
        } catch (...) {
            pending = std::current_exception();
        }

        // Complete + notify under task->mtx, and arbitrate detach ownership in
        // the SAME critical section: if the task was detached while we were
        // running, no awaiter exists, so the worker is the last owner and must
        // free it. topo_task_detach takes this same lock, so exactly one side
        // observes the other's flag and frees. We read `detached` before
        // releasing the lock; if we free, no awaiter can be parked on this task
        // (detach is mutually exclusive with await on a given handle).
        bool freeHere;
        {
            std::lock_guard<std::mutex> lock(task->mtx);
            if (pending) task->exception = std::move(pending);
            task->done.store(true, std::memory_order_release);
            task->cv.notify_all();
            freeHere = task->detached;
        }
        if (freeHere) delete task;
    }

    // Run a stolen task exactly as `worker_loop` would — body + done-notify
    // under `task->mtx`. Kept in sync with the completion path in
    // `worker_loop`; any change to that path MUST be mirrored here.
    void run_stolen(topo_task* task) {
        run_task_safely(task);
    }

    void worker_loop(int id) {
        // Register this thread's TLS samples for aggregation
        register_tls_samples();

        // Simple PRNG for steal target selection
        std::minstd_rand rng(static_cast<unsigned>(id) + 42);

        while (true) {
            topo_task* task = nullptr;

            // Try own queue first
            {
                std::lock_guard<std::mutex> lock(queues[id]->mtx);
                if (!queues[id]->tasks.empty()) {
                    task = queues[id]->tasks.front();
                    queues[id]->tasks.pop_front();
                }
            }

            // Work-stealing: try other queues
            if (!task && num_workers > 1) {
                int start = static_cast<int>(rng() % num_workers);
                for (int i = 0; i < num_workers; ++i) {
                    int victim = (start + i) % num_workers;
                    if (victim == id) continue;
                    std::lock_guard<std::mutex> lock(queues[victim]->mtx);
                    if (!queues[victim]->tasks.empty()) {
                        // Steal from back (LIFO steal for locality)
                        task = queues[victim]->tasks.back();
                        queues[victim]->tasks.pop_back();
                        break;
                    }
                }
            }

            if (task) {
                // `run_task_safely` wraps the body in try/catch so the
                // worker loop always reaches the done+notify path even
                // when the task throws. Without that wrapper an
                // exception escaping work() would unwind out of the
                // worker thread with done==false, leaving every awaiter
                // spinning forever. The
                // done+notify itself is performed inside the helper
                // under task->mtx for the same memory-safety reason the
                // previous inlined version held the lock:
                // notify_all/wait sequencing vs awaiter free.
                run_task_safely(task);
                continue;
            }

            // No work found — wait or exit
            if (shutdown_flag.load(std::memory_order_acquire)) break;

            std::unique_lock<std::mutex> lock(wake_mutex);
            wake_cv.wait_for(lock, std::chrono::milliseconds(1));

            if (shutdown_flag.load(std::memory_order_acquire)) break;
        }
    }
};

// g_pool is read lock-free (acquire) by topo_parallel_ensure_init's
// double-checked fast path, topo_task_spawn*, and topo_task_await; it is
// written only under g_lifecycle_mutex. Making it atomic with
// acquire/release is required for two reasons:
//   1. Double-checked locking on a plain pointer is a data race (UB).
//   2. Publication ordering — see do_init() below: the pool must be fully
//      started before any reader observes a non-null pointer, otherwise a
//      racing ensure_init reader sees non-null but calls submit() on an
//      unstarted pool (num_workers==0, empty queues), which is the 0.00s
//      SIGABRT seen intermittently on macOS (ParallelRuntimeLazyInit).
static std::atomic<ThreadPool*> g_pool{nullptr};
static std::mutex g_lifecycle_mutex;
static topo::parallel::Config g_config;

void do_init() {
    int n = g_config.num_threads;
    if (n <= 0) n = static_cast<int>(std::thread::hardware_concurrency());
    if (n <= 0) n = 2;

    g_instrument.store(g_config.instrument, std::memory_order_relaxed);
    // Build and FULLY START the pool in a local before publishing it. The
    // release store is the last step, so any thread that acquire-loads a
    // non-null g_pool is guaranteed to see a started pool (start() happens-
    // before the publication). Publishing before start() completes would let
    // a racing reader submit to an unstarted pool — use-before-init crash.
    ThreadPool* p = new ThreadPool();
    p->start(n);
    g_pool.store(p, std::memory_order_release);
}

} // anonymous namespace

// ============================================================
// Public C++ API
// ============================================================

namespace topo::parallel {

void init(const Config& cfg) {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    // Null the pointer out (release) BEFORE tearing the pool down so no
    // lock-free reader can observe a non-null pointer to a pool that is being
    // stopped/deleted. The store and the teardown both happen under the
    // lifecycle lock; readers only ever acquire-load.
    if (ThreadPool* old = g_pool.load(std::memory_order_acquire)) {
        g_pool.store(nullptr, std::memory_order_release);
        old->stop();
        delete old;
    }
    g_config = cfg;
    do_init();
}

void shutdown() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (ThreadPool* old = g_pool.load(std::memory_order_acquire)) {
        g_pool.store(nullptr, std::memory_order_release);
        old->stop();
        delete old;
    }
    // Each worker's TLS registration removes itself via the
    // TlsSampleRegistration destructor as the joined thread exits, so there is
    // nothing to clear here. A blanket clear() would also drop the main
    // thread's still-live registration while its thread-local `registered`
    // flag stays true — enrol() would then never re-add it, making the main
    // thread's cost samples invisible across an init/shutdown cycle.
}

std::unordered_map<std::string, uint64_t> get_cost_samples() {
    // Accumulate raw totals + counts across every worker's TLS map first,
    // then compute each function's average exactly once. Averaging an
    // already-averaged value (the previous "weighted merge") produced an
    // order-dependent, mathematically wrong result whenever one function
    // name spanned multiple worker threads (the common case under
    // round-robin task distribution).
    struct Accum {
        uint64_t total_ns = 0;
        uint64_t count = 0;
    };
    std::unordered_map<std::string, Accum> acc;

    // The same g_samples_mutex guards the worker writes in
    // topo_parallel_cost_end_impl, so iterating each registered map here is
    // race-free: no worker can rehash/insert into its tls_samples while we
    // hold this lock.
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    for (auto* tls : g_tls_registrations) {
        for (const auto& [name, sample] : *tls) {
            if (sample.count > 0) {
                auto& a = acc[name];
                a.total_ns += sample.total_ns;
                a.count += sample.count;
            }
        }
    }

    std::unordered_map<std::string, uint64_t> result;
    result.reserve(acc.size());
    for (const auto& [name, a] : acc) {
        if (a.count > 0)
            result[name] = a.total_ns / a.count;
    }
    return result;
}

void reset_cost_samples() {
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    for (auto* tls : g_tls_registrations) {
        tls->clear();
    }
}

// Scoped reset: erase only one pipeline's accumulated samples across every
// worker's TLS map, leaving all other pipelines' samples intact. Used by the
// adaptive monitor so re-measuring one specialized pipeline does not wipe the
// cost history of every sibling pipeline sharing these maps. Clears both the
// pipeline-level key (`name`) and every per-stage key (`name::<stage>`), which
// is exactly the subset a global reset would have cleared for this pipeline.
void reset_cost_samples(const std::string& name) {
    const std::string stagePrefix = name + "::";
    std::lock_guard<std::mutex> lock(g_samples_mutex);
    for (auto* tls : g_tls_registrations) {
        for (auto it = tls->begin(); it != tls->end();) {
            const std::string& key = it->first;
            if (key == name ||
                (key.size() >= stagePrefix.size() &&
                 key.compare(0, stagePrefix.size(), stagePrefix) == 0)) {
                it = tls->erase(it);
            } else {
                ++it;
            }
        }
    }
}

} // namespace topo::parallel

// ============================================================
// C ABI (called by generated code / users)
// ============================================================

extern "C" {

uint32_t topo_parallel_version(void) {
    return TOPO_PARALLEL_ABI_VERSION;
}

void topo_parallel_ensure_init() {
    // Double-checked locking: the fast-path read MUST be an acquire-load so it
    // pairs with do_init()'s release store (a non-null pointer implies a fully
    // started pool). The recheck under the lock is also an acquire-load.
    if (g_pool.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (g_pool.load(std::memory_order_acquire)) return;
    do_init();
}

topo_task_t* topo_task_spawn(void (*fn)(void*), void* arg) {
    topo_parallel_ensure_init();
    // Snapshot g_pool once (acquire) after ensure_init guarantees a started
    // pool; never dereference the atomic directly. ensure_init just published
    // it, so it is non-null here.
    ThreadPool* pool = g_pool.load(std::memory_order_acquire);
    auto* task = new topo_task();
    task->work = [fn, arg]() {
        fn(arg);
    };
    pool->submit(task);
    return task;
}

topo_task_t* topo_task_spawn_ret(void (*fn)(void*, void*), void* arg, void* result_buf, size_t /*result_size*/) {
    topo_parallel_ensure_init();
    ThreadPool* pool = g_pool.load(std::memory_order_acquire);
    auto* task = new topo_task();
    task->work = [fn, arg, result_buf]() {
        fn(arg, result_buf);
    };
    pool->submit(task);
    return task;
}

topo_task_t* topo_task_spawn_ret_pri(
    void (*fn)(void*, void*), void* arg, void* result_buf, size_t /*result_size*/, int priority) {
    topo_parallel_ensure_init();
    ThreadPool* pool = g_pool.load(std::memory_order_acquire);
    auto* task = new topo_task();
    task->priority = priority;
    task->work = [fn, arg, result_buf]() {
        fn(arg, result_buf);
    };
    pool->submit(task);
    return task;
}

void topo_task_await(topo_task_t* task) {
    if (!task) return;
    // Work-helping await. If we parked on the cv unconditionally, a worker
    // thread calling await from inside a task body could deadlock the pool
    // whenever reentrant depth × fanout exceeds the worker count: all workers
    // would sit in cv.wait, and the tasks they are awaiting would have no
    // thread left to run them. Instead, we try to pop + execute any queued
    // task while waiting. Only park (briefly) when no helpable work remains.
    //
    // Non-worker callers (e.g. the main thread) go through the same path —
    // helping drain the pool speeds up the common case at negligible cost.
    while (!task->done.load(std::memory_order_acquire)) {
        // Snapshot g_pool once per iteration (acquire). A concurrent
        // shutdown() may null it out between our load and use, but the
        // local copy stays valid: shutdown joins all workers under the
        // lifecycle lock before delete, and run_stolen runs the task body
        // on this thread, so a snapshotted-then-deleted pool is the
        // shutdown-races-await TOCTOU the atomic + snapshot closes.
        ThreadPool* pool = g_pool.load(std::memory_order_acquire);
        topo_task* helper = pool ? pool->try_steal_any() : nullptr;
        if (helper) {
            pool->run_stolen(helper);
            continue;
        }
        // No work to help with — park with a short timeout so we re-check
        // for newly-arrived stealable work periodically, and so the target
        // task's completion notify wakes us promptly.
        std::unique_lock<std::mutex> lock(task->mtx);
        task->cv.wait_for(lock, std::chrono::milliseconds(1),
                          [&]() { return task->done.load(std::memory_order_acquire); });
    }
    // Barrier acquire of task->mtx: a worker that set done=true under the
    // lock may still be inside `notify_all` (i.e. still holding task->mtx)
    // when our wait-free outer predicate sees done. Acquiring the mutex
    // here waits for that lock_guard to destruct, establishing
    // "worker releases lock" happens-before "we destroy task" — otherwise
    // `delete task` tears down task->cv/mtx while pthread_cond_broadcast is
    // still executing on them, surfacing as `_os_unfair_lock_unowned_abort`
    // under high-throughput workloads.
    std::exception_ptr captured;
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        // Move out the exception while still holding the lock so the
        // body-running worker has fully committed the store.
        captured = std::move(task->exception);
    }
    delete task;
    // Rethrow on the awaiting thread. The body threw an exception; the
    // contract of topo_task_await is to surface that to the caller
    // rather than hide it. Exception-safety fix: pre-fix this branch
    // never existed and the awaiter spun forever.
    if (captured) std::rethrow_exception(captured);
}

void topo_task_await_all(topo_task_t** tasks, int count) {
    // If any task body throws, we still drain the remaining tasks (so
    // no work-helping spin loop is left running and no task handle is
    // leaked) and surface the first captured exception to the caller.
    // Exception-safety fix: prior to
    // this change a single throwing task would propagate out mid-loop,
    // leaving siblings undrained and their handles leaked.
    std::exception_ptr first;
    for (int i = 0; i < count; ++i) {
        try {
            topo_task_await(tasks[i]);
        } catch (...) {
            if (!first) first = std::current_exception();
        }
    }
    if (first) std::rethrow_exception(first);
}

void topo_task_detach(topo_task_t* task) {
    if (!task) return;
    // Fire-and-forget: relinquish the handle without blocking. Ownership of the
    // heap allocation is handed to whichever side finishes last (see the
    // `detached` field on topo_task and run_task_safely):
    //   * If the body already finished (done==true), the worker ran
    //     run_task_safely while `detached` was still false, so it did NOT free
    //     the task — the detacher is the last owner and frees it now.
    //   * If the body is still running (done==false), set `detached` so the
    //     worker frees the task when run_task_safely completes.
    // Both paths take task->mtx (the same lock run_task_safely uses for its
    // done+notify), so exactly one side frees. After this call the caller MUST
    // NOT touch `task` again, and MUST NOT pass it to topo_task_await*.
    bool freeNow;
    {
        std::lock_guard<std::mutex> lock(task->mtx);
        if (task->done.load(std::memory_order_acquire)) {
            // Worker already completed without freeing (it saw detached==false).
            freeNow = true;
        } else {
            task->detached = true;
            freeNow = false;
        }
    }
    if (freeNow) delete task;
}

// Internal impls; the C ABI entry points topo_cost_begin / topo_cost_end
// live in topo_cost.cpp so the linker can dead-strip them (and therefore
// this .o's pull-in via the pass-emitted reference chain) when no pass
// instrumented any pipeline. Splitting lets "mode = off" binaries contain
// zero `topo_cost_*` symbols instead of leaking them via topo-parallel's
// scheduler colocation.
extern "C" void topo_parallel_cost_begin_impl(const char* func_name) {
    if (!g_instrument.load(std::memory_order_acquire)) return;
    // Non-worker threads (e.g. main) may execute task bodies via work-helping
    // in topo_task_await. Register their TLS samples lazily on first use so
    // get_cost_samples() sees them.
    register_tls_samples();
    tls_pending_begins.push_back({std::string(func_name), std::chrono::steady_clock::now()});
}

extern "C" void topo_parallel_cost_end_impl(const char* func_name) {
    if (!g_instrument.load(std::memory_order_acquire)) return;
    auto end_tp = std::chrono::steady_clock::now();
    std::string name(func_name);

    // Find matching begin (search from back for nested calls)
    for (auto it = tls_pending_begins.rbegin(); it != tls_pending_begins.rend(); ++it) {
        if (it->name == name) {
            auto duration_ns =
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(end_tp - it->tp).count());

            // tls_registration.samples is published into g_tls_registrations
            // and read / cleared cross-thread by get_cost_samples() /
            // reset_cost_samples() under g_samples_mutex. The owning worker must
            // take the SAME lock for its writes — otherwise an insert here can
            // rehash buckets while an aggregator iterates this map (UB / torn
            // reads), or race a concurrent clear()/erase(). register_tls_samples
            // above guarantees the map is enrolled before we write it. The
            // critical section is just the map mutation; tls_pending_begins
            // stays thread-local and unlocked.
            {
                std::lock_guard<std::mutex> lock(g_samples_mutex);
                auto& sample = tls_registration.samples[name];
                sample.name = name;
                sample.total_ns += duration_ns;
                sample.count++;
            }

            tls_pending_begins.erase(std::next(it).base());
            return;
        }
    }
}

void topo_parallel_init(int num_threads) {
    topo::parallel::Config cfg;
    cfg.num_threads = num_threads;
    topo::parallel::init(cfg);
}

void topo_parallel_shutdown(void) {
    topo::parallel::shutdown();
}

} // extern "C"
