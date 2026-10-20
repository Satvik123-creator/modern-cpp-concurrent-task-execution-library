# Phase 4 — Concurrency and Thread-Safety Analysis

Every shared resource is identified, its protection mechanism analyzed, and every potential issue classified.

---

## 4.1 Shared Data Map — What Is Protected By What

### ThreadPool Members (`thread_pool.cpp`)

| Member | Type | Protected By | Accessed From |
|---|---|---|---|
| `queue_` | `detail::TaskQueue` | `mutex_` | All submit overloads (user thread) + `queue_.pop()` in Worker (worker thread via lock) + `on_task_complete()` |
| `workers_` | `vector<unique_ptr<Worker>>` | `mutex_` | `add_worker()`, `try_scale_down()`, `reap_stopped_workers()`, `shutdown()`, `get_worker_infos()` |
| `running_` | `bool` | `mutex_` | All submit overloads, `on_task_complete()`, `shutdown()`, `is_running()` |
| `paused_` | `bool` | `mutex_` | `pause()`, `resume()`, `is_paused()` |
| `void_promises_` | `unordered_map` | `mutex_` | `submit(Task)`, `on_task_complete()`, `cancel()` |
| `cancel_handlers_` | `unordered_map` | `mutex_` | All submit overloads, `cancel()`, `on_task_complete()` |
| `completion_callbacks_` | `unordered_map` | `mutex_` | Template submit, `on_task_complete()`, `set_completion_handler()`, `shutdown()` |
| `retry_info_` | `unordered_map` | `mutex_` | `submit(Task)`, `submit_batch()`, `on_task_complete()` |
| `retry_counts_` | `unordered_map` | `mutex_` | `on_task_complete()` |
| `stopping_count_` | `size_t` | `mutex_` | `try_scale_down()`, `reap_stopped_workers()` |
| `next_worker_id_` | `atomic<size_t>` | Lock-free | `add_worker()` (always under mutex, but atomic anyway) |
| `total_completed_` | `atomic<size_t>` | Lock-free | `on_task_complete()` (under mutex, but atomic for cheap RMW) |
| `total_failed_` | `atomic<size_t>` | Lock-free | `on_task_complete()` (under mutex) |
| `metrics_` | `shared_ptr<Metrics>` | Shared ownership | Multiple threads via `Metrics` internal protection |
| `history_` | `shared_ptr<TaskHistory>` | Shared ownership | Multiple threads via `TaskHistory` internal mutex |

### Worker Members (`worker.cpp`)

| Member | Type | Protected By | Accessed From |
|---|---|---|---|
| `state_` | `atomic<State>` | Lock-free | Worker thread (writes), Pool thread (reads in `try_scale_down`, `reap_stopped_workers`) |
| `should_stop_` | `atomic<bool>` | Lock-free | Pool thread (writes via `request_stop()`), Worker thread (reads in predicate) |
| `id_`, `on_complete_`, etc. | Non-atomic members | Written once at construction; read-only after | Safe: construction happens before thread starts |

### TaskQueue Members (`task_queue.cpp`)

| Member | Type | Protected By | Accessed From |
|---|---|---|---|
| `heap_` | `vector<Task>` | Pool's `mutex_` (external) | `push()`, `pop()` — both called while holding pool's mutex |
| `cancelled_ids_` | `unordered_set` | Pool's `mutex_` | `cancel()`, `pop()` |
| `completed_ids_` | `unordered_set` | Pool's `mutex_` | `mark_completed()`, `pop()` |

### Metrics Members (`metrics.cpp`)

| Member | Type | Protected By | Notes |
|---|---|---|---|
| `submitted_`, `completed_`, `failed_`, `cancelled_`, `running_`, `queue_length_`, `peak_queue_length_`, `active_threads_` | `atomic<size_t>` | Lock-free | Correct |
| `total_wait_time_`, `total_exec_time_`, `wait_samples_`, `exec_samples_` | Non-atomic | `timing_mutex_` | Correct |
| `history_` | `deque<MetricsSnapshot>` | `history_mutex_` | Correct |
| `total_busy_ns_` | `atomic<long long>` | Lock-free | Correct |
| `last_tick_time_` | Non-atomic `time_point` | **Not protected** | See Issue 4.4.1 |

---

## 4.2 Condition Variable Predicate Analysis

### Pool's `cv_` — used in `Worker::run()`

```cpp
cv_.wait(lock, stop_token, [this] {
    return (!queue_.empty() && !pool_paused_)
        || !pool_running_
        || should_stop_.load(memory_order_acquire);
});
```

**Why each condition is in the predicate:**
- `!queue_.empty() && !pool_paused_` — primary work condition: work available AND pool not paused.
- `!pool_running_` — shutdown signal: always wake workers on shutdown.
- `should_stop_` — scale-down signal: wake this specific worker to let it exit.

**Spurious wakeup protection:** ✓ The predicate is rechecked on every wakeup. If no condition is true, the thread goes back to sleep.

**Lock held during predicate check:** ✓ `unique_lock<mutex>` is held. `queue_.empty()` and `pool_paused_` are read under the lock. `should_stop_` is atomic — safe to read under or without lock.

**Post-wait guards:**
```cpp
if (!pool_running_ && queue_.empty()) { break; }
if (stop_token.stop_requested() && queue_.empty()) { break; }
if (should_stop_.load(memory_order_acquire) && queue_.empty()) { break; }
if (pool_paused_ || queue_.empty()) { continue; }
```
These provide a second layer of filtering after the wait — important because the predicate uses OR, so a worker might wake for shutdown but still have tasks to drain.

### Scheduler's `cv_` — used in `timer_loop()`

```cpp
cv_.wait(lock, stop_token, [&] {
    return !schedule_heap_.empty() || !running_;
});
```
**Potential issue:** The predicate doesn't check `paused_`. A paused scheduler with tasks in its heap will still wake the timer thread. Inside the loop, there's a nested `while (paused_)` wait. This is correct but adds an extra check level. No correctness issue — just slightly less efficient.

---

## 4.3 Memory Ordering Analysis

### `Worker::request_stop()` / `should_stop_`

```cpp
// Writer (ThreadPool, under mutex_):
should_stop_.store(true, memory_order_release);

// Reader (Worker thread, in predicate under mutex_):
should_stop_.load(memory_order_acquire);
```
**Classification: ✅ No issue.**  
Release-acquire pair ensures the worker sees `true` and all preceding writes from the pool thread. Additionally, both are called under `mutex_`, providing an additional happens-before relationship.

### `state_` in Worker

```cpp
state_.store(State::Running, memory_order_release);  // worker thread
state_.load(memory_order_relaxed);                   // pool thread (try_scale_down)
```
**Classification: ⚠️ Potential Risk (Design Trade-off).**  
`relaxed` load in `try_scale_down()` means the pool might see a stale `state_` value (e.g., see `Waiting` when the worker just transitioned to `Running`). However, this is a benign race: if the pool incorrectly sees `Waiting` and calls `request_stop()`, the worker will finish its current task and then exit — no task is lost, and the pool reaps it on the next `submit()` call. The pool intentionally accepts eventual consistency for `state_` to avoid making `try_scale_down()` more expensive.

### `total_completed_` and `total_failed_` in ThreadPool

```cpp
total_completed_++;   // called in on_task_complete() — under mutex_
```
**Classification: ✅ No issue.**  
Already protected by `mutex_`. The `atomic` type provides extra safety but the mutex alone would be sufficient.

---

## 4.4 Confirmed Issues

### Issue 4.4.1 — `last_tick_time_` in `Metrics` is not thread-safe

**File:** `src/core/metrics.cpp`, `Metrics::tick()`  
**Code:**
```cpp
void Metrics::tick() {
    auto now = ch::steady_clock::now();
    auto elapsed = ch::duration_cast<ch::milliseconds>(now - last_tick_time_);
    if (elapsed.count() < 1000) return;
    last_tick_time_ = now;   // ← non-atomic write
    ...
}
```
`last_tick_time_` is a `std::chrono::steady_clock::time_point` — not an atomic type. `tick()` is called from `on_task_complete()` which is called from worker threads (all of which may be executing tasks simultaneously). Multiple workers could call `tick()` concurrently.

**Race scenario:** Worker-0 and Worker-1 both call `tick()` within 1 second. Worker-0 reads `last_tick_time_`, computes `elapsed >= 1000ms`, and begins writing `last_tick_time_ = now`. Worker-1 simultaneously reads the partially-written `last_tick_time_`. This is a **data race — undefined behavior** in C++.

**Severity:** High conceptually, but low in practice. The only effect is a spurious extra snapshot in the 60-entry history — not a crash-level bug. The race window is tiny.

**Fix:** Protect `last_tick_time_` with `history_mutex_`, or make `tick()` a no-op if `history_mutex_` cannot be acquired (try-lock).

---

### Issue 4.4.2 — `retry_delay` is stored but not enforced

**File:** `src/core/thread_pool.cpp`, `on_task_complete()`  
**Code:**
```cpp
Task retry_task(info.func, info.name, info.priority);
retry_task.set_retry_delay(info.retry_delay);
...
queue_.push(move(retry_task));   // immediate re-enqueue
cv_.notify_one();
```
`retry_delay` is stored in `RetryInfo` and passed to the new `retry_task`, but the task is pushed **immediately** with no delay. The `retry_delay` field in `Task` has no effect in the current implementation.

**Severity:** Behavioral gap — not a crash or thread-safety issue. Tests like `RetryTest.RetriesOnFailure` still pass because they don't depend on timing.

**Fix:** Use the `Scheduler` to re-enqueue after the delay: `scheduler.schedule(retry_task, now + retry_delay)`. However, `ThreadPool` doesn't currently hold a reference to `Scheduler`.

---

### Issue 4.4.3 — Unstarted tasks' futures are permanently broken on shutdown

**File:** `src/core/thread_pool.cpp`, `shutdown()`  
**Code:**
```cpp
void ThreadPool::shutdown() {
    { lock_guard lock(mutex_); running_ = false; completion_callbacks_.clear(); }
    cv_.notify_all();
    for (auto& worker : workers_) { worker.reset(); }
    workers_.clear();
    // queue_ is NOT cleared. void_promises_ is NOT cleared.
}
```
If tasks remain in `queue_` (submitted but not yet picked up by a worker), their `void_promises_` entries remain in the map. When the pool destructs, the `unordered_map<string, shared_ptr<promise<void>>> void_promises_` destructs → the `shared_ptr` ref count drops → if no other reference, the `promise` object destructs without `set_value()` or `set_exception()` → any `future.get()` blocking on it receives `std::future_error: broken promise`.

**Severity:** User-facing behavioral issue. Users who submit tasks and then call `shutdown()` quickly may get cryptic `broken_promise` exceptions.

**Fix:** In `shutdown()`, iterate `void_promises_` and call `set_exception(runtime_error("pool shut down"))` on each before clearing.

---

### Issue 4.4.4 — `TaskHistory::record_complete()` is O(n) under lock

**File:** `src/core/task_history.cpp`  
**Code:**
```cpp
void TaskHistory::record_complete(...) {
    lock_guard lock(mutex_);
    for (auto& e : entries_) {    // O(n) linear scan
        if (e.task_id == task_id) { ... return; }
    }
}
```
Called from `on_task_complete()` which is called by every worker on every task completion. For a pool with N tasks in history and K workers, this is O(N × K) lock contention under the history mutex.

**Severity:** Performance issue at scale (>10,000 tasks in history). Not a thread-safety issue.

**Fix:** Add an `unordered_map<string, size_t>` index mapping task_id → deque position (or iterator). O(1) lookup.

---

## 4.5 Potential Risks

### Risk 4.5.1 — `stats()` mixes locked and lock-free reads

**File:** `src/core/thread_pool.cpp`, `ThreadPool::stats()`  
```cpp
ThreadPoolStats ThreadPool::stats() const {
    lock_guard lock(mutex_);
    ThreadPoolStats s;
    s.active_threads = workers_.size();           // under lock
    s.pending_tasks  = queue_.size();             // under lock
    s.completed_tasks = total_completed_;         // atomic (but under lock too)
    s.total_submitted = total_completed_ + total_failed_ + queue_.size();
    auto snap = metrics_->snapshot();             // acquires timing_mutex_ internally
    ...
}
```
`metrics_->snapshot()` acquires `timing_mutex_` while `mutex_` is already held. **Lock ordering:** `mutex_` → `timing_mutex_`. If any code path holds `timing_mutex_` and then tries to acquire `mutex_`, there would be a deadlock. Examining the code: `timing_mutex_` is only ever acquired inside `Metrics` methods (`record_complete`, `snapshot`). `Metrics` never calls back into `ThreadPool`. So the ordering is always `mutex_` → `timing_mutex_`. **Classification: No issue identified in examined code**, but this is a fragile assumption — future changes that call pool methods from Metrics could introduce a deadlock.

### Risk 4.5.2 — Detached thread after timeout may access destroyed objects

**File:** `src/core/worker.cpp`, timeout path  
```cpp
auto task_ptr = make_shared<Task>(move(*task_opt));
thread([task_ptr, promise = move(p)]() mutable {
    task_ptr->execute();
    promise.set_value();
}).detach();
```
The detached thread holds `task_ptr` (a `shared_ptr`) — the `Task` stays alive as long as the thread runs. ✅  
The detached thread holds `promise p` by move — the promise is owned by the thread. ✅  
The `on_complete_` callback is not called from the detached thread — it's called by the worker (the `shared_future::wait_for` path). ✅  

**Potential risk:** If the program exits while the detached thread is still running, the thread is killed with undefined behavior (destructors may not run). This is a standard C++ issue with `detach()` in general — not specific to this project. In production, every thread should be joined before process exit.

**Classification: Design trade-off** — `detach()` was chosen to avoid blocking the worker. A safer approach is a thread pool for timeout tasks.

### Risk 4.5.3 — `cancel()` race with task execution start

**Scenario:**
1. Worker pops the task from the queue (lock released).
2. Meanwhile, user calls `pool.cancel(task_id)` (acquires lock).
3. `cancel()` finds `cancel_handlers_[task_id]` and calls it → sets exception "Task was cancelled".
4. Worker calls `task.execute()` (task is no longer in queue, cancel_handlers_ has been cleared).
5. Worker calls `on_complete_(id, Completed, ...)`.
6. `on_task_complete()` tries to resolve `void_promises_[id]` — but `cancel()` already called `set_exception()` on it. Calling `set_value()` on an already-set promise throws `std::future_error`.

**Examination:** The `std::once_flag` / `std::call_once` mechanism in the **template `submit`** overload specifically prevents this: only the first of (cancel, complete) to call `call_once` wins. ✅  

For the **`submit(Task)`** overload: `cancel()` calls `cancel_handlers_[id]()` which directly calls `promise->set_exception()` without `call_once`. Then `on_task_complete()` tries `promise->set_value()`. The `void_promises_` entry is still there (cancel only calls the handler + erases from `cancel_handlers_`, but the `void_promises_` erase happens in `on_task_complete()` only for the non-cancelled case). Wait — examining `cancel()`:
```cpp
cancel_handlers_.erase(it);
void_promises_.erase(task_id);   // ← erases the promise!
completion_callbacks_.erase(task_id);
```
And in `on_task_complete()`:
```cpp
auto pit = void_promises_.find(task_id);
if (pit != void_promises_.end()) { ... }  // ← not found → skipped
```
So the promise is erased by `cancel()` before `on_task_complete()` tries to access it. ✅

**Classification: No issue identified** for the `Task` overload. The `once_flag` mechanism covers the template overload.

---

## 4.6 Design Trade-offs

### Trade-off 1: Single Global Mutex for Everything

All pool state — queue, workers, running, paused, promises, callbacks, retry info — is protected by a single `std::mutex mutex_`. This is simple and easy to reason about.

**Pro:** Impossible to deadlock within the pool itself. All mutations are serialized.  
**Con:** Under high load, many threads contend for one lock. For a 32-core machine with 32 workers all calling `on_task_complete()` simultaneously, only one can proceed at a time.

**Alternative:** Fine-grained locking — separate mutexes for queue, workers, promises. Higher throughput but much harder to reason about ordering.

### Trade-off 2: Reap Only From `submit()` (Never from Workers)

Stopped workers are only joined from `submit()` (user thread), never from `on_task_complete()` (worker thread). This prevents a worker from joining itself.

**Pro:** Eliminates an entire class of deadlocks.  
**Con:** Stopped workers are not immediately joined — they linger in `workers_` until the next `submit()` call. If no more tasks are ever submitted, stopped workers are only joined in `shutdown()`.

### Trade-off 3: Workers Drain Before Stopping

When a worker is told to stop (scale-down), it continues processing remaining tasks until the queue is empty. This prevents task loss during scale-down.

**Pro:** No tasks are silently discarded.  
**Con:** A worker may process many tasks after being told to stop, delaying the scale-down.

### Trade-off 4: Timeout via Detached Thread

Using a detached thread + `shared_future::wait_for` to implement timeouts means the "timed out" task continues running in the background.

**Pro:** The worker thread is unblocked immediately at the deadline.  
**Con:** Detached threads cannot be interrupted. A task that runs forever will live forever (until process exit). This could exhaust system thread counts.

---

## 4.7 Summary Table

| Finding | Location | Classification |
|---|---|---|
| `last_tick_time_` non-atomic in `tick()` | `metrics.cpp` | ✅ Confirmed issue (benign race) |
| `retry_delay` not enforced | `thread_pool.cpp` | ✅ Confirmed behavioral gap |
| Shutdown doesn't resolve queued futures | `thread_pool.cpp:shutdown()` | ✅ Confirmed behavioral issue |
| `record_complete()` O(n) under lock | `task_history.cpp` | ✅ Confirmed performance issue |
| `stats()` acquires two locks | `thread_pool.cpp:stats()` | ⚠️ Potential risk (currently safe) |
| Detached thread after timeout | `worker.cpp` | ⚠️ Design trade-off |
| cancel() + complete() race | `thread_pool.cpp` | ✅ No issue (once_flag + void_promises erase) |
| state_ relaxed read in scale-down | `worker.cpp` / `thread_pool.cpp` | ⚠️ Design trade-off (benign) |
| Single global mutex | `thread_pool.cpp` | ⚠️ Design trade-off (scalability) |
| Lock ordering (mutex_ → timing_mutex_) | `thread_pool.cpp:stats()` | ⚠️ Potential risk (fragile assumption) |
| Same task executing twice | `task_queue.cpp:pop()` | ✅ No issue (pop under mutex, move semantics) |
| Metrics atomic counters | `metrics.cpp` | ✅ No issue |
| Worker state machine | `worker.cpp` | ✅ No issue |
| cv_.wait() predicate | `worker.cpp` | ✅ No issue |
