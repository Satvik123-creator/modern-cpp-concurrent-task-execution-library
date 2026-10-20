# Phase 6 — Tests and Examples Analysis

> **Important disclaimer:** These tests have NOT been executed during this analysis. All statements about pass/fail behavior are based on code reading, not observed test runs.

---

## 6.1 Test Infrastructure

**Framework:** GoogleTest (`#include <gtest/gtest.h>`)  
**Pattern:** Tests are inside `namespace ctee { namespace { ... } }` — anonymous namespace prevents symbol collisions across test files.  
**Config helpers:** Each test file defines a local `make_config()` helper that builds a `ThreadPoolConfig` tailored for that test group.

---

## 6.2 `tests/core/thread_pool_test.cpp` (32 KB — largest test file)

### Group 1: Lifecycle

**`ThreadPoolLifecycle::ConstructionStartsWorkers`**
- Setup: `ThreadPool pool(make_config(4))`
- Assert: `pool.is_running() == true`, `pool.is_paused() == false`, `pool.thread_count() == 4`
- What it proves: The constructor launches exactly `min_threads` workers.
- Failure would mean: Workers not started, or count is wrong.

**`ThreadPoolLifecycle::DestructorJoinsAllThreads`**
```cpp
{
    ThreadPool pool(make_config(2));
    for (int i = 0; i < 10; ++i)
        pool.submit(Task([&counter] { counter++; }));
    std::this_thread::sleep_for(50ms);
}  // destructor called here
EXPECT_EQ(counter.load(), 10);
```
- The 50ms sleep is a timing hack — it gives workers time to process tasks before the destructor fires.
- Failure would mean: Destructor returned before tasks completed → data race on `counter`.
- **Edge case not covered:** What if 50ms is not enough on a very slow CI machine? This test could be flaky.

**`ThreadPoolLifecycle::ShutdownIdempotent`**
- Calls `pool.shutdown()` three times.
- Asserts `!pool.is_running()`.
- Tests the `if (!running_) return;` guard in `shutdown()`.

**`ThreadPoolLifecycle::ShutdownDrainsQueue`**
```cpp
// 10 tasks each sleep 5ms. Pool has 2 workers. Total ~25ms of work.
pool.shutdown();
EXPECT_EQ(counter.load(), 10);
```
- Tests that `shutdown()` waits for all running tasks to complete.
- **Note:** This test's name says "drains queue" but what it really tests is that running tasks finish. Tasks that are in the queue but not yet picked up may not complete — the test works because 2 workers process 10 tasks before shutdown returns.

### Group 2: Submit Callable

Tests cover: lambda returning value, lambda with arguments, free function pointer, member function pointer (via lambda wrapper), struct with callable `operator()`, nested lambdas, moving non-copyable types, returning `std::string`, returning `std::vector`, chained futures (one future's result feeds the next submit call). Each tests that `future.get()` returns the correct value.

### Group 3: Submit Task

Tests cover: submitting a pre-built `Task` with name and priority, verifying the future resolves to void, exception propagation via `EXPECT_THROW(future.get(), std::runtime_error)`.

### Group 4: Submit `packaged_task`

Tests the third `submit` overload. Creates a `std::packaged_task<int()>`, submits it, verifies result.

### Group 5: Pause / Resume

- Pause pool → submit tasks → tasks queue up but don't execute → resume → tasks execute.
- Asserts counter is 0 after pause period and equals N after resume.

### Group 6: Cancel

Tests `pool.cancel(task_id)` returns `true` for existing tasks and `false` for unknown IDs. Tests that the future throws after cancellation.

### Group 7: Completion Handler

Tests `pool.set_completion_handler(id, callback)` — verifies the callback fires with the correct `TaskStatus`.

### Group 8: Stats

Tests `pool.stats()` reports correct `active_threads`, `completed_tasks`, etc.

---

## 6.3 `tests/core/concurrency_test.cpp` (15 KB)

### `ConcurrencyTest::ConcurrentSubmit`
```cpp
// 8 submitter threads, each submitting 100 tasks
// All tasks increment an atomic<int> counter
std::this_thread::sleep_for(500ms);
EXPECT_EQ(counter.load(), 800);
```
- Tests that concurrent submission from multiple threads doesn't corrupt the queue or lose tasks.
- The 500ms sleep is a timing assumption — on a very loaded machine, this could be flaky.
- **Better approach:** Collect all 800 futures and call `.get()` on each. No timing assumption needed.

### `ConcurrencyTest::ConcurrentSubmitAndCancel`
- Thread A submits 100 tasks (each sleeps 10ms).
- Thread B cancels them by ID concurrently.
- Tests: No crash, no data race, counter ≤ 100 (some may have already started).

### Additional concurrency tests (from file structure):
- Multiple threads reading `pool.stats()` concurrently.
- Worker scaling under concurrent load.
- `std::barrier` usage for synchronized multi-thread start.

---

## 6.4 `tests/core/deadlock_test.cpp` (15 KB)

**Strategy:** Each test wraps the scenario in `std::async` and uses `wait_for(5s)` to detect deadlocks:
```cpp
auto f = std::async(std::launch::async, []() { ... });
auto status = f.wait_for(kDeadlockTimeout);
ASSERT_NE(status, std::future_status::timeout)
    << "Deadlock detected: ...";
```
If the pool deadlocks, `wait_for` returns `timeout` and the test fails with a clear message.

**Scenarios tested:**
1. `ShutdownWhileTasksRunning` — tasks are mid-execution when `shutdown()` is called.
2. `ShutdownWhileTasksQueued` — pool is paused, tasks are queued, then shutdown.
3. `CancelAllThenShutdown` — all queued tasks cancelled, then shutdown.
4. Multiple other deadlock-prone scenarios.

**What deadlock would mean here:** A worker thread is waiting for the pool mutex, while the pool mutex holder is waiting for the worker thread to join. The test framework detects this by timing out at 5 seconds.

---

## 6.5 `tests/core/retry_test.cpp` (143 lines — fully read)

### `RetryTest::RetriesOnFailure`
```cpp
std::atomic<int> attempts{0};
Task task([&attempts]() {
    attempts++;
    if (attempts.load() < 3) throw std::runtime_error("fail");
});
task.set_max_retries(3);
auto future = pool.submit(move(task));
future.get();                     // should not throw — succeeds on attempt 3
EXPECT_EQ(attempts.load(), 3);
```
**Walkthrough:**
1. Attempt 1: throws → `Failed` → `retry_counts_[id] = 0 < 3` → re-enqueue.
2. Attempt 2: throws → `retry_counts_[id] = 1 < 3` → re-enqueue.
3. Attempt 3: succeeds → `Completed` → `promise->set_value()` → `future.get()` returns.

**What failure means:** Retry mechanism not working — either re-enqueue doesn't happen, or the ID association with the promise breaks.

### `RetryTest::RetriesExhausted`
- Task always throws, `max_retries = 2`.
- Expects `EXPECT_THROW(future.get(), std::runtime_error)` after 3 attempts (1 + 2 retries).
- `EXPECT_EQ(attempts.load(), 3)`.

### `RetryTest::RetryPreservesTaskName`
- Verifies that after retry, `task_history()` contains a record with the correct name and `Completed` status.
- Important: tests that `RetryInfo::name` is correctly stored and applied to the retry task.

### **Gap:** No test for `retry_delay` actually causing a delay.  
The `retry_delay` field is never tested for timing behavior because it's not actually enforced (as noted in Phase 4). No test checks that N milliseconds elapsed between retries.

---

## 6.6 `tests/core/timeout_test.cpp` (119 lines — fully read)

### `TimeoutTest::TaskTimesOut`
```cpp
Task task([]() { std::this_thread::sleep_for(5s); });
task.set_timeout(100ms);
auto future = pool.submit(move(task));
EXPECT_THROW(future.get(), std::runtime_error);

// Verify TimedOut status in history
std::this_thread::sleep_for(200ms);
auto history = pool.task_history()->recent(10);
// check for TimedOut entry
```
**Walkthrough:** Task sleeps 5s, timeout is 100ms. Worker's `wait_for(100ms)` expires → reports `TimedOut` → promise exception set → `future.get()` throws.

**Note:** The 200ms sleep before checking history is because `on_task_complete()` may be called asynchronously. This is a timing assumption.

### `TimeoutTest::TaskCompletesBeforeTimeout`
- Task sleeps 10ms, timeout is 5s.
- `future.get()` should NOT throw.
- Tests the happy path of the timeout mechanism.

### **Gap:** No test verifies that the detached thread eventually finishes (after the "timed out" worker moves on). No test checks for resource leaks from many timed-out tasks.

---

## 6.7 `tests/core/dependencies_test.cpp` (136 lines — fully read)

### `DependenciesTest::DependentTaskWaits`
```cpp
Task task_a([&]() { a_order = order.fetch_add(1); });
std::string a_id = task_a.id();
Task task_b([&]() { b_order = order.fetch_add(1); });
task_b.set_depends_on({a_id});
auto fa = pool.submit(move(task_a));
auto fb = pool.submit(move(task_b));
fb.get();
EXPECT_LT(a_order, b_order);  // a ran before b
```
**Walkthrough:** `task_b` is in the heap, but `pop()` returns `task_a` first (deps of `task_b` not met). After `task_a` completes, `mark_completed(a_id)` is called. Next `pop()` for `task_b` finds `a_id` in `completed_ids_` → returns `task_b`.

### `DependenciesTest::MarkCompletedUnblocks` (tests TaskQueue directly)
- Pushes task_a and task_b (depends on a) into the queue.
- `pop()` returns task_a (first).
- `pop()` returns `nullopt` (task_b blocked).
- `mark_completed(a_id)`.
- `pop()` now returns task_b.

### **Gap:** No test for circular dependencies (A depends on B, B depends on A). This would cause both tasks to be stuck in the "deferred" list forever, causing the `pop()` loop to become infinite and the queue to appear permanently empty. **This is an unhandled edge case that could hang the pool.**

---

## 6.8 `tests/core/task_queue_test.cpp` (11 KB)

Tests TaskQueue directly (not through ThreadPool):
- Priority ordering: Critical tasks pop before High, High before Normal, Low last.
- FIFO within same priority: earlier task pops first.
- Cancellation: cancelled tasks are skipped on `pop()`.
- Capacity: `is_full()` returns true at capacity, `push()` silently drops at capacity.
- Dependency gating: as described above.

---

## 6.9 `tests/core/metrics_test.cpp` (12 KB)

Tests:
- `record_submit()` increments `submitted`.
- `record_complete(Completed)` increments `completed`, decrements `running`.
- `record_complete(Failed)` increments `failed`.
- `set_queue_length()` updates `queue_length` and `peak_queue_length`.
- `snapshot()` returns consistent values.
- `history()` grows up to `history_size` entries, then rolls.
- `tick()` produces snapshots no more than once per second.
- `reset()` zeros all counters.
- Thread-safety: multiple threads calling `record_submit` / `record_complete` concurrently.

---

## 6.10 `tests/core/scheduler_test.cpp` (9 KB)

Tests:
- `schedule_now()` executes immediately.
- `schedule(task, now + 200ms)` executes after ~200ms.
- `schedule_recurring(task, 100ms, max_repeats=3)` fires 3 times with ~100ms intervals.
- `cancel()` prevents a scheduled task from firing.
- `reschedule()` changes the execution time.
- `pause()` / `resume()` stops/starts the timer loop.

---

## 6.11 `tests/core/stress_test.cpp` and `load_test.cpp`

### `stress_test.cpp` (8 KB):
- Submits thousands of tasks as fast as possible.
- Mixes priorities, cancellations, and concurrency.
- Checks no deadlock, all futures resolve, counters consistent.

### `load_test.cpp` (11 KB):
- Sustained high load over multiple seconds.
- Verifies auto-scaling: pool expands under load and contracts when idle.
- Measures throughput (tasks/sec) against expected minimums.

---

## 6.12 `tests/core/benchmark_test.cpp` (13 KB)

- Measures time to submit N tasks and wait for all to complete.
- Reports throughput in tasks/second.
- Compares different configurations (thread counts, queue sizes).
- **Note:** These are not correctness tests — they don't assert correctness, they print performance numbers. They don't fail unless the pool deadlocks.

---

## 6.13 `tests/core/edge_case_test.cpp` (13 KB)

Tests unusual inputs and boundary conditions:
- Submit to a shut-down pool → future throws immediately.
- Submit when queue is full → future throws immediately.
- Cancel a non-existent ID → returns false, no crash.
- Submit an empty lambda (`[](){}`).
- Submit tasks that immediately throw.
- Submit after pause/resume cycle.

---

## 6.14 `tests/core/worker_test.cpp` (7 KB)

Tests `Worker` directly:
- Worker starts in `Idle` state, transitions to `Waiting`.
- Worker transitions to `Running` during task execution.
- `request_stop()` causes worker to stop after draining.
- `is_stopped()` returns true after thread exits.
- `state_to_string()` returns correct strings.

---

## 6.15 `tests/core/task_test.cpp` (4 KB)

Tests `Task` directly:
- UUID is generated (non-empty, unique across tasks).
- Priority defaults to Normal.
- Status starts as Pending.
- `execute()` sets status to Completed on success.
- `execute()` sets status to Failed and stores exception on throw.
- `operator<` correctly orders tasks by priority, then timestamp, then ID.
- Move constructor/assignment work correctly.

---

## 6.16 `tests/core/task_history_test.cpp` (2 KB)

Tests `TaskHistory`:
- `record_submit()` adds a record with `Queued` status.
- `record_complete()` updates the existing record's status and timestamps.
- `recent(N)` returns the N most recent records.
- `since(time_point)` filters by completion time.
- `count()` returns correct total.
- Bounded at `max_size` — oldest records evicted.

---

## 6.17 `tests/core/batch_test.cpp` (1.6 KB)

Tests `submit_batch()`:
- Returns vector of task IDs.
- All tasks execute.
- IDs are unique.
- Stops accepting if queue becomes full mid-batch.

---

## 6.18 `tests/core/config_test.cpp` (3 KB)

Tests `ThreadPoolConfig` defaults and `load_config()`:
- Default `min_threads = 2`, `max_threads >= 2`.
- `load_config("nonexistent.json")` returns default config without throwing.
- Valid JSON file is parsed correctly.

---

## 6.19 `tests/core/thread_naming_test.cpp` (426 bytes)

- Calls `set_thread_name("test-thread")` — verifies no crash.
- Does not verify the name is actually set (that would require platform-specific inspection).

---

## 6.20 `tests/common/logger_test.cpp` (18 KB)

Tests:
- `info()`, `debug()`, `warn()`, `error()` with and without `LogContext`.
- Level filtering: `debug` message not written when level is `Info`.
- File output: messages written to a temp file.
- `flush()` blocks until pending messages are written.
- Background thread drains all messages before destructor returns.
- Multiple threads logging concurrently — no crash, no corruption.

---

## 6.21 `tests/server/http_server_test.cpp` (17 KB)

Tests the HTTP layer:
- Server starts and listens on a port.
- `POST /tasks` returns a task ID.
- `GET /metrics` returns JSON with expected fields.
- `GET /health` returns `{"status": "ok"}`.
- `DELETE /tasks/{id}` cancels a task.
- `POST /pool/pause` and `POST /pool/resume` work.
- CORS headers are present on responses.

---

## 6.22 `examples/basic_pool.cpp` — Walkthrough

```cpp
ctee::ThreadPoolConfig config;
config.min_threads = 4;
config.max_threads = 8;
config.max_queue_size = 1000;
ctee::ThreadPool pool(config);
```
Creates a pool with 4–8 workers and queue capacity 1000.

```cpp
auto future_sum = pool.submit([]() { return 42; });
std::cout << "Sum: " << future_sum.get() << "\n";
```
**Line by line:**
- `pool.submit([]() { return 42; })` → `R = int` → `future<int>`.
- A worker executes the lambda → returns 42 → `promise->set_value(42)`.
- `future_sum.get()` → returns `42`.
- Output: `Sum: 42`.

```cpp
auto future_product = pool.submit([](int a, int b) { return a * b; }, 6, 7);
std::cout << "Product: " << future_product.get() << "\n";
```
Arguments `6` and `7` are captured via `...capture = std::forward<Args>(args)`. Inside the task lambda: `std::invoke(fn, move(capture)...)` = `fn(6, 7)` = `42`.

```cpp
for (int i = 0; i < 10; ++i)
    futures.push_back(pool.submit([i]() { return i * i; }));
```
Fan-out pattern. 10 tasks submitted. `[i]` captures by value — each task has its own copy of `i`. Results: 0, 1, 4, 9, 16, 25, 36, 49, 64, 81.

```cpp
ctee::Task task([]() { ... }, "my_task", ctee::TaskPriority::High);
auto task_future = pool.submit(std::move(task));
task_future.get();
```
Uses the `submit(Task)` overload — returns `future<void>`. `task_future.get()` blocks until the task completes or throws.

---

## 6.23 Important Gaps in Test Coverage

| Missing Test | Risk Level | Why It Matters |
|---|---|---|
| Circular dependency (A→B, B→A) | High | Would cause permanent deadlock in `pop()` |
| `retry_delay` timing | Medium | Feature exists but untested; known to be broken |
| Unstarted futures after `shutdown()` | Medium | Callers may block forever |
| Detached thread resource leak on timeout | Medium | Could exhaust thread handles |
| `TaskHistory::record_complete()` performance at scale | Low | O(n) under lock, not measured |
| `Metrics::last_tick_time_` race | Low | Benign but technically UB |
| `submit_batch()` with retry tasks | Medium | Batch doesn't store retry info correctly |
| Scheduler + retry interaction | Medium | Scheduler fires tasks without retry metadata |
| `pool.stats()` consistency under concurrent mutations | Low | Mixes locked and unlocked reads |
