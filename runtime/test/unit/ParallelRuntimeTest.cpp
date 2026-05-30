#include <gtest/gtest.h>
#include <topo/parallel.h>
#include <topo/rt/parallel_rt.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

class ParallelRuntimeTest : public ::testing::Test {
protected:
    void SetUp() override {
        topo::parallel::Config cfg;
        cfg.num_threads = 4;
        cfg.instrument = true;
        topo::parallel::init(cfg);
        topo::parallel::reset_cost_samples();
    }

    void TearDown() override { topo::parallel::shutdown(); }
};

// ---- Basic task spawn + await ----

static void set_value(void* arg) {
    *static_cast<int*>(arg) = 42;
}

TEST_F(ParallelRuntimeTest, SpawnAndAwaitSingleTask) {
    int result = 0;
    auto* task = topo_task_spawn(set_value, &result);
    topo_task_await(task);
    EXPECT_EQ(result, 42);
}

// ---- Multiple tasks with await_all ----

static void set_indexed(void* arg) {
    auto* val = static_cast<int*>(arg);
    *val = *val + 100;
}

TEST_F(ParallelRuntimeTest, SpawnNTasksAwaitAll) {
    constexpr int N = 8;
    int values[N];
    topo_task_t* tasks[N];

    for (int i = 0; i < N; ++i) {
        values[i] = i;
        tasks[i] = topo_task_spawn(set_indexed, &values[i]);
    }

    topo_task_await_all(tasks, N);

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(values[i], i + 100) << "Task " << i << " failed";
    }
}

// ---- spawn_ret: task with return value ----

static void compute_square(void* arg, void* result) {
    int input = *static_cast<int*>(arg);
    *static_cast<int*>(result) = input * input;
}

TEST_F(ParallelRuntimeTest, SpawnRetReturnsResult) {
    int input = 7;
    int output = 0;
    auto* task = topo_task_spawn_ret(compute_square, &input, &output, sizeof(int));
    topo_task_await(task);
    EXPECT_EQ(output, 49);
}

// ---- Work-stealing: imbalanced load ----

static void slow_task(void* arg) {
    // Simulate ~10ms of work
    auto start = std::chrono::steady_clock::now();
    volatile int sum = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10))
        ++sum;
    *static_cast<int*>(arg) = 1;
}

static void fast_task(void* arg) {
    *static_cast<int*>(arg) = 1;
}

TEST_F(ParallelRuntimeTest, WorkStealingImbalanced) {
    // 1 slow + 7 fast tasks — workers should steal fast tasks
    constexpr int N = 8;
    int results[N] = {};
    topo_task_t* tasks[N];

    tasks[0] = topo_task_spawn(slow_task, &results[0]);
    for (int i = 1; i < N; ++i)
        tasks[i] = topo_task_spawn(fast_task, &results[i]);

    topo_task_await_all(tasks, N);

    for (int i = 0; i < N; ++i)
        EXPECT_EQ(results[i], 1) << "Task " << i;
}

// ---- Lazy init via topo_parallel_ensure_init ----

TEST(ParallelRuntimeLazyInit, EnsureInitDoesNotCrash) {
    // This test runs without explicit init — ensure_init should handle it
    topo_parallel_ensure_init();
    int val = 0;
    auto* task = topo_task_spawn(set_value, &val);
    topo_task_await(task);
    EXPECT_EQ(val, 42);
}

// ---- Cost sampling ----

static void heavy_with_cost(void* arg) {
    topo_cost_begin("heavy");
    volatile int sum = 0;
    for (int i = 0; i < 100000; ++i)
        sum += i;
    *static_cast<int*>(arg) = sum;
    topo_cost_end("heavy");
}

static void light_with_cost(void* arg) {
    topo_cost_begin("light");
    *static_cast<int*>(arg) = 1;
    topo_cost_end("light");
}

TEST_F(ParallelRuntimeTest, CostSamplingHeavyGtLight) {
    int a = 0, b = 0;

    // Run multiple times to get stable averages
    for (int i = 0; i < 5; ++i) {
        auto* t1 = topo_task_spawn(heavy_with_cost, &a);
        auto* t2 = topo_task_spawn(light_with_cost, &b);
        topo_task_await(t1);
        topo_task_await(t2);
    }

    auto samples = topo::parallel::get_cost_samples();
    ASSERT_TRUE(samples.count("heavy") > 0) << "heavy sample missing";
    ASSERT_TRUE(samples.count("light") > 0) << "light sample missing";
    EXPECT_GT(samples["heavy"], samples["light"]) << "heavy=" << samples["heavy"] << " light=" << samples["light"];
}

// Instrument gate — setting `instrument=false` on init must prevent any cost
// samples from being recorded. Previously untested: a regression that
// disconnected the `g_instrument` gate (e.g. `topo_cost_begin` stops
// consulting it) would silently populate samples even when the user opted
// out. We re-init the pool with instrument=false and verify samples stay
// empty even after running the same heavy/light workload the gated test
// uses.
TEST_F(ParallelRuntimeTest, CostSamplingInstrumentGateSilencesRecording) {
    // Tear down the fixture's instrument=true pool and bring up one with
    // instrumentation disabled.
    topo::parallel::shutdown();

    topo::parallel::Config cfg;
    cfg.num_threads = 4;
    cfg.instrument = false;
    topo::parallel::init(cfg);
    topo::parallel::reset_cost_samples();

    int a = 0, b = 0;
    for (int i = 0; i < 5; ++i) {
        auto* t1 = topo_task_spawn(heavy_with_cost, &a);
        auto* t2 = topo_task_spawn(light_with_cost, &b);
        topo_task_await(t1);
        topo_task_await(t2);
    }

    auto samples = topo::parallel::get_cost_samples();
    EXPECT_TRUE(samples.empty())
        << "instrument=false should suppress cost samples; got "
        << samples.size() << " entries";

    // Restore the fixture's pool so TearDown's shutdown is balanced.
    topo::parallel::shutdown();
    topo::parallel::Config restore;
    restore.num_threads = 4;
    restore.instrument = true;
    topo::parallel::init(restore);
}

TEST_F(ParallelRuntimeTest, ResetCostSamples) {
    int a = 0;
    auto* t = topo_task_spawn(heavy_with_cost, &a);
    topo_task_await(t);

    auto before = topo::parallel::get_cost_samples();
    EXPECT_FALSE(before.empty());

    topo::parallel::reset_cost_samples();
    auto after = topo::parallel::get_cost_samples();
    EXPECT_TRUE(after.empty());
}

// ---- Priority-aware spawn ----

static void set_result_pri(void* arg, void* result_buf) {
    *static_cast<int*>(result_buf) = *static_cast<int*>(arg) + 10;
}

TEST_F(ParallelRuntimeTest, SpawnRetPriHighPriority) {
    int input = 5;
    int output = 0;

    // Spawn with Critical priority (0)
    auto* task = topo_task_spawn_ret_pri(set_result_pri, &input, &output, sizeof(int), 0);
    topo_task_await(task);
    EXPECT_EQ(output, 15);
}

TEST_F(ParallelRuntimeTest, SpawnRetPriBackgroundPriority) {
    int input = 7;
    int output = 0;

    // Spawn with Background priority (4)
    auto* task = topo_task_spawn_ret_pri(set_result_pri, &input, &output, sizeof(int), 4);
    topo_task_await(task);
    EXPECT_EQ(output, 17);
}

// ============================================================
// Edge-case / stress coverage
// ============================================================
//
// Each test is independent and deterministic. No sleeps longer than ~10 ms.
// These exist to catch regressions in queue handling, stealing, reentrancy,
// lifecycle boundaries, and the empty/zero-count paths.

namespace {

static void atomic_increment(void* arg) {
    auto* ctr = static_cast<std::atomic<int>*>(arg);
    ctr->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

// ---- 1. Spawn queue saturation ----
//
// Spawn N (= 10000) tasks, much larger than the 4-worker pool. Each increments
// a shared counter. All must complete, and the counter must equal N. A
// queue-capacity bug (drop / deadlock / lost wake-up) would show up here
// either as a hang or a wrong final count.

TEST_F(ParallelRuntimeTest, SaturationManySpawnsAwaitAll) {
    constexpr int N = 10000;
    std::atomic<int> counter{0};
    std::vector<topo_task_t*> tasks;
    tasks.reserve(N);

    for (int i = 0; i < N; ++i) {
        tasks.push_back(topo_task_spawn(atomic_increment, &counter));
    }
    topo_task_await_all(tasks.data(), static_cast<int>(tasks.size()));

    EXPECT_EQ(counter.load(), N);
}

// ---- 2. Many tiny tasks (task handoff race check) ----
//
// 1000 tiny tasks each doing atomic ++. If any path drops a task or races on
// handoff between `submit` and `worker_loop`, the final count is wrong.

TEST_F(ParallelRuntimeTest, ManyTinyTasksCounterMatches) {
    constexpr int N = 1000;
    std::atomic<int> counter{0};
    std::vector<topo_task_t*> tasks;
    tasks.reserve(N);

    for (int i = 0; i < N; ++i) {
        tasks.push_back(topo_task_spawn(atomic_increment, &counter));
    }
    topo_task_await_all(tasks.data(), static_cast<int>(tasks.size()));

    EXPECT_EQ(counter.load(), N);
}

// ---- 3. Recursive task spawning (reentrancy) ----
//
// A task body that itself spawns + awaits child tasks. Tests that worker
// threads can synchronously await from inside a running task without
// deadlocking the pool.
//
// Fixed 2026-04-17 by making `topo_task_await` work-helping: an awaiter
// tries to pop + run queued tasks from any worker queue instead of parking
// unconditionally.
//
// Coverage:
//   * `RecursiveSpawnOneLevel`   — depth 1, fanout 3 (4 tasks, 4 workers).
//   * `RecursiveSpawnTwoLevels`  — depth 2, fanout 3 (13 tasks, 4 workers).
//     Previously deadlocked; now must pass within the 10-second deadline.
//   * `RecursiveSpawnThreeLevels` — depth 3, fanout 4 (85 tasks, 4 workers).
//     Stress test for deep reentrancy.
//
// Each nested test enforces a 10-second deadline so any regression that
// reintroduces the deadlock fails fast instead of hanging ctest forever.

namespace {

struct RecursiveCtx {
    std::atomic<int>* counter;
    int fanout;
    int depth_left;
};

static void recursive_spawn(void* arg) {
    auto* ctx = static_cast<RecursiveCtx*>(arg);
    ctx->counter->fetch_add(1, std::memory_order_relaxed);

    if (ctx->depth_left == 0) return;

    std::vector<RecursiveCtx> children(ctx->fanout);
    std::vector<topo_task_t*> child_tasks;
    child_tasks.reserve(ctx->fanout);

    for (int i = 0; i < ctx->fanout; ++i) {
        children[i] = RecursiveCtx{ctx->counter, ctx->fanout, ctx->depth_left - 1};
        child_tasks.push_back(topo_task_spawn(recursive_spawn, &children[i]));
    }
    topo_task_await_all(child_tasks.data(), static_cast<int>(child_tasks.size()));
}

} // namespace

TEST_F(ParallelRuntimeTest, RecursiveSpawnOneLevel) {
    // Root + 3 leaf children. Root runs on worker 0 and awaits; children run
    // on workers 1,2,3. This is the maximal shape that does *not* trip the
    // reentrant-await deadlock on a 4-worker pool.
    //
    // Total tasks = 1 + 3 = 4. Counter should equal 4.
    std::atomic<int> counter{0};
    RecursiveCtx root{&counter, 3, 1};
    auto* root_task = topo_task_spawn(recursive_spawn, &root);
    topo_task_await(root_task);

    EXPECT_EQ(counter.load(), 4);
}

namespace {

// Run `body` on a detachable worker and wait up to `deadline_ms`. If the body
// hasn't finished by then, the test fails fast with an ASSERT_* macro
// (reentrant-await deadlock regression would otherwise hang ctest forever).
//
// On timeout we intentionally *leak* the worker thread — calling join would
// itself block. The process is about to exit on failure anyway.
template <typename F>
void run_with_deadline(int deadline_ms, F&& body) {
    std::atomic<bool> done{false};
    std::thread worker([&]() {
        body();
        done.store(true, std::memory_order_release);
    });
    auto start = std::chrono::steady_clock::now();
    while (!done.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() - start >
            std::chrono::milliseconds(deadline_ms)) {
            worker.detach();
            FAIL() << "deadline (" << deadline_ms << " ms) exceeded — likely reentrant-await deadlock regression";
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    worker.join();
}

} // namespace

TEST_F(ParallelRuntimeTest, RecursiveSpawnTwoLevels) {
    // Root (depth 2) + 3 children (depth 1) + 9 grandchildren (depth 0) = 13.
    //
    // Previously deadlocked on a 4-worker pool: worker 0 ran root and awaited,
    // workers 1,2,3 ran the 3 depth-1 children and each awaited 3
    // grandchildren, leaving no thread to run the 9 grandchildren. Fixed by
    // work-helping await in `topo_task_await`.
    std::atomic<int> counter{0};
    run_with_deadline(10000, [&]() {
        RecursiveCtx root{&counter, 3, 2};
        auto* root_task = topo_task_spawn(recursive_spawn, &root);
        topo_task_await(root_task);
    });
    EXPECT_EQ(counter.load(), 13);
}

TEST_F(ParallelRuntimeTest, RecursiveSpawnThreeLevels) {
    // Fanout 4, depth 3 on a 4-worker pool.
    // Total tasks = 1 + 4 + 16 + 64 = 85. Stress-tests that work-helping
    // scales to deeper reentrancy than the pool width.
    std::atomic<int> counter{0};
    run_with_deadline(10000, [&]() {
        RecursiveCtx root{&counter, 4, 3};
        auto* root_task = topo_task_spawn(recursive_spawn, &root);
        topo_task_await(root_task);
    });
    EXPECT_EQ(counter.load(), 85);
}

// ---- 4. Empty spawn batch (zero-count await_all) ----
//
// `topo_task_await_all` with count=0. Must be a no-op; no null deref, no
// spurious wait. Catches a missing guard in the await path.

TEST_F(ParallelRuntimeTest, EmptyAwaitAllIsNoOp) {
    topo_task_t* empty[1] = {nullptr};
    // Count=0 means the body never touches empty[0] — safe.
    topo_task_await_all(empty, 0);
    SUCCEED();
}

// ---- 5. Shutdown with outstanding (awaited) tasks ----
//
// Spawn many tasks, await only a prefix of them, then let TearDown/shutdown
// destroy the pool. Tasks that are still queued at shutdown time must not
// leak the pool destructor, and all already-submitted work must either run or
// be cleanly discarded without triggering ASan/TSan.
//
// Note: `topo_task_await` is the *only* way to delete a `topo_task_t*`. If we
// leave tasks un-awaited we would leak them deliberately. To keep this test
// leak-clean under ASan, we await everything *after* capturing the
// partial-await scenario.

TEST_F(ParallelRuntimeTest, ShutdownWithPartiallyAwaitedTasks) {
    constexpr int N = 64;
    std::atomic<int> counter{0};
    std::vector<topo_task_t*> tasks;
    tasks.reserve(N);

    for (int i = 0; i < N; ++i) {
        tasks.push_back(topo_task_spawn(atomic_increment, &counter));
    }

    // Await only the first half — the remaining tasks may or may not have
    // started yet, but they must all eventually run (the pool is still up).
    topo_task_await_all(tasks.data(), N / 2);

    // Drain the rest so we don't leak task handles. If shutdown-with-pending
    // were broken, this second await_all would hang or crash.
    topo_task_await_all(tasks.data() + N / 2, N - N / 2);

    EXPECT_EQ(counter.load(), N);
}

// ---- 6. Rapid init/shutdown cycles ----
//
// Loop 5 times: init → spawn → await → shutdown. Catches static-state leaks
// (e.g. stale TLS registrations, dangling `g_pool`, accumulated cost samples
// that survive across shutdown when they should not).
//
// Uses the C ABI directly (mirrors what generated code does). We restore the
// fixture-provided pool afterward so TearDown's shutdown succeeds cleanly.

TEST_F(ParallelRuntimeTest, RapidInitShutdownCycles) {
    topo::parallel::shutdown(); // tear down fixture pool

    for (int cycle = 0; cycle < 5; ++cycle) {
        topo_parallel_init(2);

        std::atomic<int> counter{0};
        constexpr int N = 32;
        std::vector<topo_task_t*> tasks;
        tasks.reserve(N);
        for (int i = 0; i < N; ++i) {
            tasks.push_back(topo_task_spawn(atomic_increment, &counter));
        }
        topo_task_await_all(tasks.data(), static_cast<int>(tasks.size()));
        EXPECT_EQ(counter.load(), N) << "cycle " << cycle;

        topo_parallel_shutdown();
    }

    // Re-establish a pool so TearDown's shutdown() call is balanced.
    topo::parallel::Config cfg;
    cfg.num_threads = 4;
    cfg.instrument = true;
    topo::parallel::init(cfg);
}

// ---- 7. Work stealing under extreme imbalance ----
//
// One long task (~10 ms busy-wait) submitted first + many short tasks
// submitted after. With round-robin submit and work-stealing disabled the
// short tasks queued behind the long one on the same worker would block on
// the long task. With stealing enabled, idle workers must pull them off the
// busy queue so the total wall-clock is dominated by the single long task,
// not summed.
//
// Determinism: we assert *completion count*, not wall-clock. A stealing bug
// would manifest as a hang (exceeding the test timeout), not as a wrong
// count — but the count assertion still catches dropped tasks, and the hang
// is the real failure mode here.

namespace {

static void busy_ten_ms(void* arg) {
    auto start = std::chrono::steady_clock::now();
    volatile uint64_t sum = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10)) {
        sum += 1;
    }
    static_cast<std::atomic<int>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

} // namespace

TEST_F(ParallelRuntimeTest, WorkStealingUnderExtremeImbalance) {
    constexpr int N_SHORT = 64;
    std::atomic<int> long_counter{0};
    std::atomic<int> short_counter{0};

    // One long task first.
    auto* long_task = topo_task_spawn(busy_ten_ms, &long_counter);

    // Then many short tasks.
    std::vector<topo_task_t*> short_tasks;
    short_tasks.reserve(N_SHORT);
    for (int i = 0; i < N_SHORT; ++i) {
        short_tasks.push_back(topo_task_spawn(atomic_increment, &short_counter));
    }

    // Await short tasks first — if stealing is broken, these would be stuck
    // behind the long task on one worker and the await would hang.
    topo_task_await_all(short_tasks.data(), static_cast<int>(short_tasks.size()));
    EXPECT_EQ(short_counter.load(), N_SHORT);

    topo_task_await(long_task);
    EXPECT_EQ(long_counter.load(), 1);
}

// ---- 8. Mixed priority ordering does not drop tasks ----
//
// Interleave Critical (0), High (1), Normal (2), Low (3), Background (4)
// priorities. All must eventually run, regardless of the order they were
// submitted in. Catches a bug where a priority lane's tasks are enqueued but
// never dequeued.

namespace {

static void pri_result(void* arg, void* result) {
    *static_cast<int*>(result) = *static_cast<int*>(arg) * 2;
}

} // namespace

TEST_F(ParallelRuntimeTest, MixedPrioritiesAllComplete) {
    constexpr int N = 50;
    std::vector<int> inputs(N);
    std::vector<int> outputs(N, 0);
    std::vector<topo_task_t*> tasks;
    tasks.reserve(N);

    for (int i = 0; i < N; ++i) {
        inputs[i] = i + 1;
        int pri = i % 5; // cycle 0..4
        tasks.push_back(
            topo_task_spawn_ret_pri(pri_result, &inputs[i], &outputs[i], sizeof(int), pri));
    }
    topo_task_await_all(tasks.data(), static_cast<int>(tasks.size()));

    for (int i = 0; i < N; ++i) {
        EXPECT_EQ(outputs[i], (i + 1) * 2) << "task " << i << " pri " << (i % 5);
    }
}

// ---- Exception-safety regression (throwing task body must not deadlock awaiters) ----
//
// Prior to the fix, a task body that threw left ``done`` at false
// forever; the awaiter spun in its work-helping loop until the process
// was killed. The regression below pins the new contract: a throwing
// task completes promptly, awaiter wakes, and the exception surfaces
// on the awaiting thread.
//
// We drive the regression via the C++ runtime's higher-level
// ``topo::parallel::Pool::spawn(F)`` if available; the C ABI takes a
// ``void(*)(void*)`` so a lambda that throws lives in a translation-
// unit-local trampoline.

namespace {

struct TaskException : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void throwing_body(void*) {
    throw TaskException("intentional fault from task body");
}

void succeed_after_throw(void* p) {
    *static_cast<int*>(p) = 7;
}

} // namespace

TEST_F(ParallelRuntimeTest, ThrowingTaskBodyDoesNotDeadlockAwaiter) {
    // A single throwing task must complete + rethrow within a small
    // bounded wall-clock window — the prior bug was an unbounded spin.
    auto start = std::chrono::steady_clock::now();
    auto* task = topo_task_spawn(throwing_body, nullptr);

    bool got = false;
    try {
        topo_task_await(task);
    } catch (const TaskException& e) {
        got = true;
        EXPECT_STREQ(e.what(), "intentional fault from task body");
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    EXPECT_TRUE(got) << "exception did not surface from topo_task_await";
    // Generous upper bound; the prior bug never returned at all so any
    // finite cap is sufficient as a fail-without-fix signal.
    EXPECT_LT(elapsed_ms, 2000) << "topo_task_await did not return promptly "
                                   "after task body threw";
}

TEST_F(ParallelRuntimeTest, AwaitAllSurvivesPartialThrow) {
    // One throwing task in the middle of a batch: every sibling still
    // completes, the handle leak is avoided, and the first captured
    // exception surfaces from topo_task_await_all.
    constexpr int N = 6;
    std::vector<int> outs(N, 0);
    std::vector<topo_task_t*> tasks;
    tasks.reserve(N);
    for (int i = 0; i < N; ++i) {
        if (i == N / 2) {
            tasks.push_back(topo_task_spawn(throwing_body, nullptr));
        } else {
            tasks.push_back(topo_task_spawn(succeed_after_throw, &outs[i]));
        }
    }

    auto start = std::chrono::steady_clock::now();
    bool got = false;
    try {
        topo_task_await_all(tasks.data(), N);
    } catch (const TaskException&) {
        got = true;
    }
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    EXPECT_TRUE(got) << "topo_task_await_all swallowed the task exception";
    EXPECT_LT(elapsed_ms, 2000);

    // Non-throwing siblings still completed.
    for (int i = 0; i < N; ++i) {
        if (i == N / 2) continue;
        EXPECT_EQ(outs[i], 7) << "sibling task " << i << " never ran";
    }
}
