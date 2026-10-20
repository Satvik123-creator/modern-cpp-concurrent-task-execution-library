# Phase 7 — Technical Interview Preparation Guide

All answers are grounded in the actual implementation of this project.

---

## 7.1 Project Explanation Scripts

### 30-Second Introduction
> "I built a concurrent task execution library in C++20. At its core is a `ThreadPool` class that manages a pool of worker threads. You submit callables — lambdas, function pointers, or pre-built `Task` objects — and get back a `std::future` to retrieve the result asynchronously. The pool uses a priority heap for scheduling, supports task dependencies, retry on failure, per-task timeouts, and auto-scales the thread count based on queue depth. It also has a `Scheduler` for time-based dispatch, a thread-safe metrics system, and a REST API that exposes pool internals to a React dashboard."

### 1-Minute Explanation
> "The library has six main components. The `Task` class wraps any callable with metadata — a UUID, priority, status, retry count, timeout, and dependency list. Tasks go into a `TaskQueue`, which is a vector-backed max-heap ordered by priority, then FIFO timestamp, then UUID tiebreaker. Worker threads block on a `condition_variable_any`, pop the highest-priority ready task, and execute it. Results travel through `std::promise`/`std::future` pairs — the worker sets the promise, the caller's `future.get()` retrieves the value or rethrows the exception.
>
> The `ThreadPool` auto-scales: when the queue depth hits a configurable threshold and the worker count is below the max, it spawns a new `jthread` immediately. When the queue empties and there are excess workers, it marks one for stop — it drains remaining tasks, then exits. Stopping workers are reaped on the next `submit()` call (never from a worker thread) to avoid join-deadlock.
>
> A separate `Scheduler` runs a timer thread that maintains a min-heap of time-stamped entries. It sleeps until the next deadline using `condition_variable_any::wait_until`, fires the task into the pool, and re-queues recurring tasks. On top of this runs a `cpp-httplib` REST server that lets you submit, cancel, and monitor tasks from a browser."

### 2–3 Minute Technical Explanation
> "Let me walk through the architecture from the ground up.
>
> **The task representation:** Every unit of work is a `Task` — move-only, UUID-identified, holding a `std::function<void()>`. It carries priority (4 levels), retry and timeout configuration, and a dependency list of other task IDs.
>
> **The queue:** `detail::TaskQueue` is a `std::vector` used as a max-heap via `std::push_heap`/`pop_heap`. The ordering is defined by `Task::operator<` — higher priority comes first; equal priorities are FIFO by creation timestamp; ties broken by UUID. `pop()` is O(n) in the worst case because it checks dependency satisfaction: it scans from the top, deferring tasks whose dependencies haven't completed, and returns the first eligible task.
>
> **The workers:** Each `Worker` owns a `std::jthread` — C++20's joinable, stoppable thread. The worker loop blocks on `condition_variable_any::wait` with a stop token, pops a task, executes it, and calls a completion callback. For tasks with a timeout, execution happens on a detached thread; the worker waits via `shared_future::wait_for` and reports `TimedOut` if the deadline passes.
>
> **The pool:** `ThreadPool` is the coordinator. `submit()` builds a `std::promise`/`std::future` pair, wraps the callable in a `Task`, and pushes it under the pool's single `std::mutex`. The same lock covers worker management, queue operations, promise maps, retry state, and scaling decisions. Scale-up happens inline in `submit()`. Scale-down is triggered in the completion callback — a waiting worker is told to stop, and it's joined on the next `submit()` call.
>
> **Result delivery:** For the template `submit(F&&, Args&&...)` overload, the callable's return value is captured in a lambda that stores a `setter` function. The `completion_callbacks_` map fires this setter when the task finishes, which calls `promise->set_value()`. A `std::once_flag` prevents both the cancel handler and the completion callback from setting the promise simultaneously.
>
> **Retry:** If a `Task` has `max_retries > 0` and fails, `on_task_complete()` re-enqueues a fresh task with the same UUID, incrementing a retry counter. The promise is deferred until the final attempt. The `RetryInfo` struct preserves the callable and metadata between attempts.
>
> **Metrics:** A two-tier system — `std::atomic` counters for the hot path (submit/complete), a `timing_mutex_` for average wait/exec times, and a 60-entry `deque` of `MetricsSnapshot` for time-series charting. The `Metrics` object is `shared_ptr`-owned and exposed to the HTTP layer.
>
> **The REST layer:** An `HTTPServer` (wrapping `cpp-httplib`) exposes 15 endpoints — submit, cancel, pause/resume, metrics, history, worker status. A `RouteHandler` bridges HTTP requests to pool methods. A `TaskRegistry` maintains server-side task metadata separate from the pool's internal `TaskHistory`."

---

## 7.2 File-Specific Interview Questions

### On `task_priority.h`
**Q: Why is `TaskPriority` declared as `enum class` with underlying type `uint8_t`?**  
A: `enum class` prevents name leakage — you must write `TaskPriority::High`, not just `High`, which avoids collisions with other enums. The `: uint8_t` explicitly chooses 1-byte storage instead of the default `int` (4 bytes). Every `Task` object stores a `TaskPriority`, so this saves 3 bytes per task — meaningful at scale.

### On `task.h` / `task.cpp`
**Q: Why is `Task` move-only (copy constructor `= delete`)?**  
A: A `Task` has a UUID. Copying would create two tasks with the same ID, breaking the promise/cancel-handler maps keyed by that ID. Additionally, `std::function` involves heap allocation — cheap to move, expensive to copy at scale. Making copy illegal prevents accidental expensive copies.

**Q: Explain the 3-level comparator in `Task::operator<`.**  
A: Level 1 — `priority_ < other.priority_`: lower enum value = "less important" = further from the heap top. Level 2 — `created_at_ > other.created_at_`: a later timestamp = "less important" = earlier task goes first (FIFO). Level 3 — `id_ > other.id_`: UUID lexicographic tiebreaker. This third level is required because C++ heap operations require a **strict weak ordering** — if two elements compare equal, the behavior is undefined. The UUID guarantees no two tasks ever compare equal.

**Q: Why does `generate_uuid()` use `thread_local` storage for the RNG?**  
A: `mt19937` is not thread-safe. Multiple threads calling `generate_uuid()` simultaneously would corrupt the shared RNG state, producing duplicate UUIDs or crashes. `thread_local` gives each thread its own engine instance, seeded from its own `random_device`. No mutex needed.

### On `task_queue.h` / `task_queue.cpp`
**Q: Why is `TaskQueue` backed by a `std::vector` instead of `std::priority_queue`?**  
A: `std::priority_queue` hides the container, preventing: (1) iterating all tasks for cancellation, (2) reading `cancelled_ids_` during pop, (3) implementing O(n) dependency checking. The raw vector + heap algorithms give full control at the cost of slightly more boilerplate.

**Q: What is the time complexity of `pop()` in the worst case?**  
A: O(n log n). Each `pop_heap` is O(log n). In the worst case, all n tasks have unsatisfied dependencies, so we pop all n, defer all n, then push all n back. Each push is also O(log n). Total: O(n log n).

**Q: How does dependency tracking work?**  
A: `TaskQueue` maintains two `unordered_set<string>`: `cancelled_ids_` and `completed_ids_`. When `mark_completed(id)` is called, the ID goes into `completed_ids_`. During `pop()`, for each task, all IDs in `task.depends_on()` are checked against `completed_ids_`. If any are missing, the task is deferred and pushed back.

### On `worker.h` / `worker.cpp`
**Q: What is `std::jthread` and why is it used instead of `std::thread`?**  
A: `std::jthread` (C++20) adds two features: (1) automatic `join()` in its destructor — no risk of `std::terminate()` if you forget to join, and (2) a built-in `stop_source`/`stop_token` for cooperative cancellation. In this project, the stop token is used in the `cv_.wait()` overload to make the condition variable aware of stop requests — the worker wakes immediately when `jthread::request_stop()` is called, even if no task is available.

**Q: Why is `condition_variable_any` used instead of `condition_variable`?**  
A: `condition_variable` only works with `std::unique_lock<std::mutex>`. `condition_variable_any` works with any lockable type and additionally provides the C++20 overload `wait(lock, stop_token, predicate)`, which atomically combines stop-token checking with the condition variable wait. This is essential for `jthread`-based code.

**Q: Explain the timeout mechanism — why detached thread + `shared_future`?**  
A: A regular `std::future` from `std::async` has a destructor that **blocks** until the async task completes — defeating the timeout. `std::shared_future` does NOT block on destruction. So: launch the task on a detached thread, wait on the `shared_future` for `timeout_ms`, and on timeout, report `TimedOut` and move on. The detached thread continues running — C++ cannot interrupt it. The task pointer is kept alive by a `shared_ptr` in the detached thread's closure.

**Q: How does scale-down avoid the join-deadlock problem?**  
A: When `try_scale_down()` marks a worker for stop, the worker doesn't join itself — it exits its loop and sets `state_ = Stopped`. The actual join (`it->reset()` → `Worker::~Worker()`) happens in `reap_stopped_workers()`, which is only called from `submit()` — a user thread, never a worker thread. So a worker can never be in the call stack when its own join is called.

### On `thread_pool.h` / `thread_pool.cpp`
**Q: Explain `is_packaged_task_v` and why it exists.**  
A: The template `submit(F&&, Args&&...)` would match `std::packaged_task<R()>` if submitted directly, but that overload doesn't handle `packaged_task` correctly (it wraps it in another promise layer, creating a double-future problem). `is_packaged_task` is a template specialization that returns `true` for `packaged_task` types. The `requires (!is_packaged_task_v<decay_t<F>>)` constraint excludes `packaged_task` from the template overload, routing it to the dedicated `submit(packaged_task<R()>)` overload.

**Q: What does `if constexpr (std::is_void_v<R>)` do?**  
A: It's a compile-time branch. If the return type `R` is `void`, the branch calls `func()` and then sets `promise->set_value()` with no argument (void promise). If `R` is non-void, the branch stores the return value in a `shared_ptr<R>` and sets `promise->set_value(move(*result_ptr))`. Without `if constexpr`, you'd need two template specializations — this merges them. The non-taken branch is not compiled for that instantiation.

**Q: Why does `submit()` use `std::once_flag` and `std::call_once`?**  
A: Between the cancel handler and the completion callback, only one should set the promise. If a task is cancelled while it's being executed (it was already popped before cancel), both the cancel handler and the completion callback might try to call `promise->set_value()` or `promise->set_exception()`. The second call would throw `std::future_error: promise already satisfied`. `call_once` guarantees exactly one call wins.

### On `metrics.h` / `metrics.cpp`
**Q: Why are hot-path counters `std::atomic` while timing accumulators use a mutex?**  
A: Atomic RMW (`fetch_add`) costs ~1–5 ns and involves no lock contention between cores. A mutex lock/unlock costs ~20–100 ns. For `total_wait_time_ += wait_time` (a compound read-modify-write on a non-trivially-copyable type), atomics don't work — you can't atomically add `nanoseconds` to `nanoseconds` with a single CPU instruction. A mutex is the correct tool there.

**Q: Explain the CAS loop in `set_queue_length()` for peak tracking.**  
A: `compare_exchange_weak(prev, len)`: atomically read current `peak`, compare to `prev`, if equal replace with `len`. If another thread changed `peak` in the meantime, `prev` is updated with the new value and we retry. This is the lock-free way to update a shared maximum: only update if the new value exceeds the current, and retry if the current changed concurrently.

### On `scheduler.h` / `scheduler.cpp`
**Q: Why does the Scheduler store `std::function<void()>` instead of `Task` objects?**  
A: `Task` is move-only — once moved into the pool, it's gone. A recurring task needs to fire multiple times, each time creating a fresh `Task` object. If the `ScheduledEntry` stored a `Task`, the first firing would consume (move) it and nothing would remain for subsequent firings. `std::function` is copyable — the `ScheduledEntry` retains its `func` member across multiple firings, creating a new `Task` each time.

---

## 7.3 C++ and OS Concepts for Interviews

### Threads and Scheduling
- **OS thread vs `std::thread`:** `std::thread` is a thin wrapper over OS threads (pthreads on Linux, Win32 threads on Windows). The OS scheduler decides which thread runs on which CPU core and for how long (time-slicing).
- **Thread overhead:** Creating a thread involves a kernel call, stack allocation (~1 MB by default), and TLS setup — typically 1–100 µs. Thread pools amortize this cost.
- **`jthread` cooperative stop:** `stop_token::stop_requested()` checks an atomic flag. The thread must check it periodically — C++ has no preemptive thread kill.

### Synchronization Primitives
- **`std::mutex`:** Mutual exclusion lock. Only one thread holds it at a time. Implemented as a futex (fast userspace mutex) on Linux — uncontended lock costs ~10–20 ns; contended lock involves a kernel syscall.
- **`std::condition_variable`:** Sleeps a thread until a condition is true. The `wait()` call atomically releases the lock and sleeps. Woken by `notify_one()` (one thread) or `notify_all()`. Always check predicate in a loop (spurious wakeups).
- **`std::atomic<T>`:** Guarantees indivisible read/write. Backed by CPU instructions like `LOCK XADD` (x86). `memory_order_relaxed` = no reordering guarantees; `memory_order_acquire/release` = establishes happens-before relationships.

### Futures and Promises
- **`std::promise<T>`:** Write end of a channel. Call `set_value(v)` or `set_exception(ep)`. Can only be set once.
- **`std::future<T>`:** Read end. `get()` blocks until ready. Moves the value out (can be called once).
- **`std::shared_future<T>`:** Multiple threads can call `get()` concurrently. Does not block on destruction (unlike `future` from `std::async`).
- **`std::packaged_task<R(Args...)>`:** Wraps a callable with a built-in promise. Calling `operator()` sets the promise. Useful when you want to decouple task creation from execution.

### RAII
Every resource in this project is managed via RAII:
- `unique_lock` — releases mutex in its destructor.
- `unique_ptr<Worker>` — calls `Worker::~Worker()` (joins thread) in its destructor.
- `shared_ptr<Metrics>` — destroys `Metrics` when last owner dies.
- `jthread` — joins the thread in its destructor.
- `ofstream` — flushes and closes file in its destructor.

### Exception Safety
- `Task::execute()` catches all exceptions with `catch(...)`.
- Exceptions travel across threads via `exception_ptr`.
- `future.get()` re-throws them on the caller's thread.
- The pool never propagates exceptions from tasks to workers — workers are always in a valid state after execution.

---

## 7.4 Design Decisions — In-Depth

### Decision 1: Single Mutex vs. Fine-Grained Locking
**What:** One `std::mutex mutex_` protects all of: queue, workers vector, promises map, callbacks map, retry map, running/paused flags.  
**Why (inferred):** Simplicity and correctness. With a single mutex, there's no possible deadlock from lock ordering within the pool. Reasoning about thread safety is straightforward.  
**Alternative:** Separate mutexes for queue, workers, maps. Would reduce contention but require careful lock ordering.  
**Trade-off:** Lower concurrency under high submission rates. Acceptable for most workloads; a bottleneck at millions of tasks/second.

### Decision 2: Worker Callbacks Instead of Direct Pool Access
**What:** `Worker` receives `on_complete_` and `on_start_` as `std::function` rather than holding a `ThreadPool*`.  
**Why (inferred):** Testability and decoupling. `Worker` can be unit-tested independently with a mock callback. The compile-time dependency graph is simpler — `worker.h` doesn't include `thread_pool.h`.  
**Trade-off:** Slight overhead from `std::function` virtual dispatch. Runtime coupling instead of compile-time.

### Decision 3: Reaping Workers Only From `submit()`
**What:** Stopped workers are joined only in `reap_stopped_workers()`, called only from `submit()`.  
**Why:** Prevents join-deadlock. A worker cannot join itself.  
**Limitation:** If no tasks are submitted after a scale-down, stopped workers linger until `shutdown()`.

### Decision 4: `optional<Task>` Return from `pop()`
**What:** `pop()` returns `std::optional<Task>` rather than throwing or returning a sentinel.  
**Why:** Expressive and zero-overhead. `optional<Task>` with `has_value() = false` clearly means "nothing ready." No exception-throwing for a normal case. No null pointer.  
**Alternative:** Throw `std::out_of_range` if empty. More expensive, harder to use.

### Decision 5: Priority Heap Over Simple FIFO Queue
**What:** `std::vector` with heap algorithms instead of `std::queue`.  
**Why:** Supports priority levels (Critical/High/Normal/Low) and FIFO within each level. Supports dependency checking by scanning the heap.  
**Trade-off:** O(log n) push/pop vs O(1) for a simple FIFO. The dependency scan is O(n). Acceptable for typical queue sizes.

---

## 7.5 Difficult Follow-Up Questions

### Q1: What happens if multiple threads submit tasks simultaneously?

`submit()` acquires `mutex_` at the start. All concurrent submitters serialize on this lock. One thread enters at a time, pushes its task, notifies the CV, and releases the lock. The others wait, then proceed one by one.

The `promise`, `future`, `once_flag`, and `setter` are all created on the calling thread's stack before the lock is acquired — so no contention for those. Only the queue push and map insertions are under the lock. Lock hold time is very short (O(log n) for heap insert).

### Q2: How do you prevent two workers from taking the same task?

`queue_.pop()` is called while holding `mutex_`. Only one worker can hold `mutex_` at a time. The first worker to acquire the lock calls `pop()` and removes the task from the heap. When the second worker acquires the lock, the task is gone. There is no way for two workers to get the same task.

This is the key role of the mutex in this design — it serializes all queue access.

### Q3: What happens if a task throws an exception?

In `Task::execute()`, the exception is caught by `catch(...)`. `std::current_exception()` captures an `exception_ptr`. `result_.exception = current_exception()`, `status_ = Failed`.

In the no-timeout path: the exception_ptr is not directly used; instead `promise->set_exception()` is called in `on_task_complete()` with a new `runtime_error("Task execution failed")` — not the original exception. **This means the original exception type and message are lost for the `submit(Task)` overload.** For the template overload, the original exception IS preserved via the `setter` lambda: `*setter = [promise, ep](){ promise->set_exception(ep); }`.

The caller's `future.get()` rethrows the exception.

### Q4: What happens if the queue is full?

In all `submit()` overloads:
```cpp
if (queue_.is_full()) {
    promise->set_exception(make_exception_ptr(runtime_error("Task queue is full")));
    return future;
}
```
The task is rejected immediately. The future is returned with an exception already set. `future.get()` throws immediately. No blocking.

`submit_batch()` stops adding tasks when the queue becomes full and returns only the IDs that were successfully enqueued.

### Q5: How does a future receive a return value?

For the template overload: the user's return value is captured inside the Task's closure (via `result_ptr = make_shared<R>(move(result))`). A "setter" lambda is stored that calls `promise->set_value(move(*result_ptr))`. When `on_task_complete()` fires the `completion_callbacks_[id]`, it calls the setter, which calls `set_value`. The future's internal shared state becomes ready. `future.get()` moves the value out and returns it.

The `shared_ptr<R>` is needed because the value is computed on the worker thread and must survive until the setter is called (also on the worker thread, but slightly later in `on_task_complete()`).

### Q6: What happens when shutdown starts while tasks are running?

1. `running_` is set to `false` under `mutex_`.
2. `cv_.notify_all()` wakes all workers.
3. Workers check: `!pool_running_ && queue_.empty()` — if there are still tasks queued, workers will pick them up until the queue is empty, then exit.
4. Running tasks continue to completion (C++ cannot stop a running thread).
5. `worker.reset()` is called — `Worker::~Worker()` requests stop and joins. Since the worker already exited its loop, the join returns immediately.

**Tasks still in the queue (not yet picked up) are abandoned.** Their futures remain permanently pending (broken promise).

### Q7: How do you prevent deadlocks?

Three key rules:
1. **No worker calls `join()` on itself.** `reap_stopped_workers()` is only called from `submit()` (user thread).
2. **Lock ordering is consistent.** Inside the pool: only `mutex_` → `timing_mutex_`. `timing_mutex_` is never held when acquiring `mutex_`.
3. **Callbacks that might acquire locks are called with care.** The `completion_callbacks_` are called while `mutex_` is held — they must not try to re-acquire `mutex_` (which would deadlock). In the template overload, the setter calls `promise->set_value()` — this accesses the promise's internal shared state, not the pool mutex. ✓

The Scheduler unlocks its own mutex before calling `pool_.submit()` to avoid holding two mutexes simultaneously.

### Q8: How is thread safety ensured throughout the library?

Three complementary mechanisms:
- **`std::mutex`** for compound state: all mutable pool state (queue, workers, maps, flags) is protected by one mutex. Any read-modify-write on these must be under the lock.
- **`std::atomic`** for simple counters: metrics counters, worker state, should_stop flag. Lock-free, cheap, no contention.
- **Thread confinement:** Some data is only ever accessed from one thread. `Timer_thread_` in Scheduler is the only writer to `schedule_heap_` — but since callers also write to it under `mutex_`, the scheduler mutex protects it.

### Q9: What is the complexity of important operations?

| Operation | Complexity | Notes |
|---|---|---|
| `submit()` | O(log n) | Heap insert; plus O(k) for reap where k = stopped workers |
| `queue_.pop()` | O(n log n) worst case | O(log n) average when no deps |
| `on_task_complete()` | O(n) for `history_.record_complete` | Linear scan |
| `stats()` | O(k) workers | k = worker count |
| `metrics_->snapshot()` | O(1) | Atomic reads + short mutex hold |
| `scheduler.schedule()` | O(log m) | m = scheduled entries |
| `cancel()` | O(1) | Map lookup + insert |

### Q10: What would you improve for millions of tasks per second?

1. **Fine-grained locking or lock-free queue:** Replace the single `mutex_` with a lock-free MPMC queue (e.g., using `std::atomic<Node*>` or a Michael-Scott queue). This removes the submission bottleneck.
2. **Work-stealing:** Each worker has its own local deque. When idle, workers steal from other workers' deques. Reduces lock contention to near zero.
3. **Fix `record_complete()` in TaskHistory:** Replace the linear scan with an `unordered_map` index — O(1) lookup instead of O(n).
4. **Lazy reaping:** Instead of checking all workers in `reap_stopped_workers()`, maintain a separate "stopped" list.
5. **Batch wakeup:** `cv_.notify_all()` on batch submit, not `notify_one()` per task.
6. **Enforce `retry_delay`:** Use the Scheduler to re-enqueue retry tasks after the delay, avoiding the busy-queue problem.
7. **Fix `last_tick_time_` race** in Metrics — use an atomic or add it to `history_mutex_`.
8. **Resolve queued futures on shutdown** — call `set_exception` on all pending `void_promises_` in `shutdown()`.

### Q11: Can the same task execute more than once accidentally?

Not in the normal flow. `pop()` removes the task from the heap — a moved-from `Task` cannot be re-popped. For retries, a **new** `Task` object is created with the same UUID — conceptually the "same" task but a different object.

Could a task execute twice due to a race? No: `pop()` is called under `mutex_`, and `move(heap_.back())` takes ownership. Only one worker can hold the mutex at a time.

### Q12: How could you extend this to support task groups / fan-out-fan-in?

The existing dependency system (`depends_on`) already provides the building block. For fan-in (wait for all tasks A, B, C before starting D), set `task_d.set_depends_on({a_id, b_id, c_id})`. The `pop()` function checks all three IDs in `completed_ids_` before returning `task_d`.

For a cleaner API, you'd add a `when_all(vector<string> ids)` method that returns a `future<void>` that resolves when all listed IDs complete.

---

## 7.6 Quick-Reference Cheat Sheet for Interviews

```
CTEE = Concurrent Task Execution Engine

Key classes:
  Task             → move-only unit of work, UUID, priority, func_, retry, timeout, deps
  TaskQueue        → vector heap, push_heap/pop_heap, dep gating, cancel tracking
  Worker           → jthread, cv_.wait(stop_token), State{Idle,Waiting,Running,Stopped}
  ThreadPool       → single mutex, 3 submit overloads, auto-scale, retry, promise maps
  Scheduler        → jthread timer, min-heap by time, fires tasks into pool
  Metrics          → atomics (hot) + mutex (timing avg) + deque history (60 snaps)
  TaskHistory      → mutex deque of TaskRecord, bounded at 10000
  Logger           → async background thread, deque<LogEntry>, promise-based flush

Key C++20 features used:
  jthread           → auto-join + stop_token
  condition_variable_any::wait(lock, stop_token, pred) → stop-aware wait
  requires + std::invocable                            → concept constraints
  if constexpr(std::is_void_v<R>)                      → compile-time branch
  ...capture = std::forward<Args>(args)                → pack in lambda capture

Key synchronization patterns:
  mutex_          → all pool state (queue, workers, maps, flags)
  atomic state_   → worker state (read from different thread, benign relaxed)
  atomic counters → metrics (lock-free, relaxed)
  once_flag       → cancel vs complete race prevention
  CAS loop        → peak queue length tracking
  shared_future   → non-blocking timeout wait

Known limitations:
  retry_delay not enforced  → re-enqueue is immediate
  shutdown() → queued futures broken_promise
  record_complete() → O(n) scan under lock
  detached thread → cannot be interrupted on timeout
  circular deps → would hang pop() forever
```
