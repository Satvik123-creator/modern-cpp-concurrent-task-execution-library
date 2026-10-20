# Phase 2 — Every File Explained in Detail (Core Library)

Files are presented in **strict dependency order** — leaf files first, then the files that depend on them.
Each file follows the same structure: Identity → Line-by-Line → C++ Concepts → Connections → Example.

---

## File 1 — `include/ctee/core/task_priority.h`

### A. Identity
- **Type:** Public header (interface-only)
- **Purpose:** Defines the `TaskPriority` enum that controls which task runs first.
- **Depends on:** Nothing (pure C++ standard library).
- **Used by:** `task.h`, `task_queue.h`, `scheduler.h`, `thread_pool.h`, `task_registry.h`, all test files.
- **Essential?** Yes — every task has a priority.

### B. Line-by-Line

```cpp
#pragma once
```
**What it does:** Tells the compiler "include this file only once, no matter how many times it appears in `#include` chains."  
**Why:** Without it, if two files both include `task_priority.h` and then include each other, the compiler would see the same declarations twice and produce errors.  
**Alternative:** The older way uses `#ifndef TASK_PRIORITY_H / #define / #endif` guards. `#pragma once` is simpler and supported by every modern compiler.

```cpp
#include <cstdint>
#include <string>
```
`<cstdint>` gives you fixed-width integer types like `std::uint8_t`. `<string>` is included for the `to_string` declaration (though `to_string` takes a `TaskPriority` by value, not `std::string`).

```cpp
namespace ctee {
```
**What it does:** Wraps everything in the `ctee` namespace.  
**Why:** Without namespaces, `TaskPriority` would be a global name that could clash with any other library that defines its own `TaskPriority`. The namespace acts like a folder for names.

```cpp
enum class TaskPriority : std::uint8_t {
    Low      = 0,
    Normal   = 1,
    High     = 2,
    Critical = 3
};
```
**C++ Concept — `enum class`:**  
A regular `enum` in C++ leaks its values into the enclosing scope. `enum class` (a C++11 feature) prevents that. You must write `TaskPriority::High`, not just `High`. This prevents name collisions and makes code self-documenting.

**`: std::uint8_t`** — The underlying storage type. Instead of the compiler choosing `int` (4 bytes), you specify `uint8_t` (1 byte). Since there are only 4 values, 1 byte is more than enough. This matters when `TaskPriority` is stored inside every `Task` object.

**Values 0–3:** The integer values matter because they are used in `Task::operator<` as a comparison: higher number = higher priority = runs first.

```cpp
const char* to_string(TaskPriority priority);
```
**Declaration only** — the implementation lives in `task.cpp`. Returns a human-readable C-string like `"high"` or `"critical"`. Used by the HTTP layer for JSON serialization.

---

## File 2 — `include/ctee/core/task_result.h`

### A. Identity
- **Type:** Public header — two things in one: the `TaskStatus` lifecycle enum and the `TaskResult` struct.
- **Purpose:** Defines what a task can be in (its status) and what it produces (its result).
- **Depends on:** `<chrono>`, `<exception>`, `<optional>`, `<string>`.
- **Used by:** Almost every file in the project.

### B. Line-by-Line

```cpp
enum class TaskStatus : std::uint8_t {
    Pending   = 0,
    Queued    = 1,
    Running   = 2,
    Completed = 3,
    Failed    = 4,
    Cancelled = 5,
    TimedOut  = 6
};
```
This is a **7-state lifecycle machine** for a task:

| State | When set |
|---|---|
| `Pending` | Default when a `Task` is first constructed |
| `Queued` | When `submit()` calls `task.set_status(TaskStatus::Queued)` before pushing into the queue |
| `Running` | When `Task::execute()` begins |
| `Completed` | When `func_()` returns without throwing |
| `Failed` | When `func_()` throws any exception |
| `Cancelled` | When `ThreadPool::cancel()` is called |
| `TimedOut` | When the `shared_future::wait_for()` deadline expires in the Worker |

```cpp
struct TaskResult {
    bool success = false;
    std::optional<std::string> value;
    std::exception_ptr exception;
    std::chrono::milliseconds execution_time{0};
};
```
**`bool success`:** `true` only if the task completed without throwing.

**`std::optional<std::string> value`:** Optional string payload. In practice this field is **not written to** by `Task::execute()` in the current implementation — the actual result travels through `std::promise`/`std::future`, not here. This field exists as a potential future extension (e.g., serialized JSON output).

**C++ Concept — `std::exception_ptr`:**  
This is a special pointer that can hold any exception object across thread boundaries. When a task throws, `Task::execute()` calls `std::current_exception()` which captures the thrown exception into an `exception_ptr`. That pointer can later be re-thrown with `std::rethrow_exception()` on a different thread. This is how exceptions safely travel from a worker thread back to the caller via a `future`.

**`std::chrono::milliseconds execution_time{0}`:**  
`{}` initializes to zero milliseconds. Written by `Task::execute()` as `duration_cast<milliseconds>(end - start)`.

---

## File 3 — `include/ctee/core/task.h` + `src/core/task.cpp`

### A. Identity
- **Type:** Core class — the fundamental unit of work.
- **Purpose:** Encapsulates everything about one job: the callable, identity, priority, status, scheduling metadata, retry config, dependency list, timeout, and result.
- **Depends on:** `task_priority.h`, `task_result.h`.
- **Used by:** `TaskQueue`, `Worker`, `ThreadPool`, `Scheduler`, all tests.
- **Essential?** Yes — you cannot submit anything without a `Task`.

### B. Line-by-Line — `task.h`

```cpp
class Task {
    friend class detail::Worker;
    friend class ThreadPool;
```
**C++ Concept — `friend`:**  
`friend` grants another class access to `private` members. `Worker` needs to call `task.execute()` and read `task.result()`. `ThreadPool` needs to call `task.set_status()`. Without `friend`, these would have to be made `public`, which would let any code call them. `friend` is a surgical access grant — only these two classes get the privilege.

```cpp
public:
    using FuncType = std::function<void()>;
```
**C++ Concept — `std::function<void()>`:**  
`std::function` is a **type-erased callable wrapper**. It can hold a lambda, a function pointer, a functor, or a `std::bind` expression — anything that is callable with no arguments and returns `void`. "Type-erased" means you don't need to know the exact type of the callable at compile time; `std::function` boxes it up inside a heap-allocated wrapper.

**Why `void()`?** — Tasks are always executed as `func_()`. The result (if any) is captured by the lambda closure before being wrapped into a `Task`. The actual return value travels through `std::promise`, not through the `FuncType`.

```cpp
    Task(FuncType func, std::string name = "",
         TaskPriority priority = TaskPriority::Normal);
```
Constructor: takes a callable, an optional name (empty string by default), and an optional priority (Normal by default). Default arguments mean you can write `Task(my_lambda)` and it just works.

```cpp
    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    Task() = delete;
```
**C++ Concept — Move semantics and deleted functions:**

- `= default` on the move constructor/assignment tells the compiler to generate the default move for you (transfer ownership, leave source in valid-but-unspecified state).
- `= delete` on copy constructor/assignment makes copying illegal at compile time. Why? A `Task` owns a `std::function` (potentially holding resources) and a UUID — copying would create two tasks with the same ID, which breaks the entire identity system.
- `= delete` on the default constructor prevents creating a `Task` with no function.
- `noexcept` on the move means "this operation will never throw", which lets `std::vector` use the efficient move path when resizing.

```cpp
    [[nodiscard]] const std::string& id() const noexcept;
```
**`[[nodiscard]]`:** C++17 attribute. If you call `task.id()` and discard the return value (i.e., don't store it anywhere), the compiler emits a warning. This guards against accidental misuse.

**`const noexcept`:** The method doesn't modify the object (`const`) and won't throw (`noexcept`). Both are important for callers that hold a `const Task&`.

```cpp
    bool operator<(const Task& other) const noexcept;
```
The priority comparator — used by the heap in `TaskQueue`. Explained fully below in `task.cpp`.

```cpp
private:
    void set_status(TaskStatus status);
    void execute();
```
Private — only accessible to `detail::Worker` and `ThreadPool` (the friends). This prevents any caller from executing a task directly or changing its status externally.

```cpp
private:
    std::string id_;
    std::string name_;
    FuncType func_;
    TaskPriority priority_;
    TaskStatus status_;
    std::chrono::steady_clock::time_point created_at_;
    std::chrono::steady_clock::time_point scheduled_at_;
    bool has_schedule_;
    std::chrono::milliseconds repeat_interval_;
    std::size_t repeat_count_;
    std::size_t max_repeats_;
    TaskResult result_;
    std::size_t retry_count_ = 0;
    std::size_t max_retries_ = 0;
    std::chrono::milliseconds retry_delay_{0};
    std::vector<std::string> depends_on_;
    std::chrono::milliseconds timeout_ms_{0};
```
Each field annotated:

| Field | Type | Purpose |
|---|---|---|
| `id_` | `string` | UUID — unique identity for this task across the whole pool |
| `name_` | `string` | Human-readable label for logging/history |
| `func_` | `FuncType` | The actual work to do |
| `priority_` | `TaskPriority` | Determines queue ordering |
| `status_` | `TaskStatus` | Current lifecycle state |
| `created_at_` | `time_point` | When the Task was constructed — used for FIFO ordering within same priority |
| `scheduled_at_` | `time_point` | When a scheduled task should run (set by Scheduler) |
| `has_schedule_` | `bool` | Whether `scheduled_at_` is meaningful |
| `repeat_interval_` | `milliseconds` | If > 0, task is recurring with this period |
| `repeat_count_` | `size_t` | How many times it has run so far |
| `max_repeats_` | `size_t` | 0 = unlimited; N = stop after N runs |
| `result_` | `TaskResult` | Filled by `execute()` |
| `retry_count_` | `size_t` | How many retries have already happened |
| `max_retries_` | `size_t` | 0 = no retry; N = retry up to N times |
| `retry_delay_` | `milliseconds` | Wait before re-enqueue (currently stored but delay not actively waited — see Phase 4) |
| `depends_on_` | `vector<string>` | IDs of tasks that must complete first |
| `timeout_ms_` | `milliseconds` | 0 = no timeout; > 0 = detached-thread execution with deadline |

### C. Line-by-Line — `task.cpp`

```cpp
namespace {
string generate_uuid() {
    thread_local random_device rd;
    thread_local mt19937 gen(rd());
    thread_local uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);
    ...
}
}  // namespace
```
**C++ Concept — `thread_local`:**  
`thread_local` means "one copy of this variable per thread". `mt19937` (a Mersenne Twister random number generator) is **not thread-safe** — if two threads shared one instance, the internal state could be corrupted. By making it `thread_local`, each thread has its own private generator seeded from its own `random_device`. The variable is initialized the first time that thread reaches this line, and destroyed when the thread exits.

**C++ Concept — anonymous namespace `namespace {}`:**  
Anything inside `namespace {}` has **internal linkage** — it's visible only within this translation unit (this `.cpp` file). It's the modern C++ replacement for `static` at file scope. `generate_uuid` is an implementation detail; it should not be accessible from other files.

```cpp
char buf[37];
snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x", ...);
```
Formats 128 bits (four `uint32_t` values = 4 × 32 = 128 bits) into a UUID-like string: `xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx`. This is not a proper RFC-4122 UUID (no version bits), but it's sufficient for uniquely identifying tasks within a process run.

```cpp
Task::Task(FuncType func, string name, TaskPriority priority)
    : id_(generate_uuid()),
      name_(move(name)),
      func_(move(func)),
      priority_(priority),
      status_(TaskStatus::Pending),
      created_at_(ch::steady_clock::now()),
      has_schedule_(false),
      repeat_interval_(0),
      repeat_count_(0),
      max_repeats_(0) {}
```
**C++ Concept — Member Initializer List (`: field_(val), ...`):**  
This is the correct way to initialize class members. It runs before the constructor body. Using `move(name)` and `move(func)` here means the strings and functions are moved into the members (ownership transferred), not copied — this is much cheaper than copying.

**`std::chrono::steady_clock::now()`:**  
`steady_clock` is a monotonic clock — it never goes backward. Used for measuring durations and for the task's `created_at_` timestamp (used in the priority comparator for FIFO ordering).

```cpp
void Task::execute() {
    if (func_) {
        status_ = TaskStatus::Running;
        auto start = ch::steady_clock::now();
        try {
            func_();
            auto end = ch::steady_clock::now();
            result_.success = true;
            result_.execution_time = ch::duration_cast<ch::milliseconds>(end - start);
            status_ = TaskStatus::Completed;
        } catch (...) {
            auto end = ch::steady_clock::now();
            result_.success = false;
            result_.exception = current_exception();
            result_.execution_time = ch::duration_cast<ch::milliseconds>(end - start);
            status_ = TaskStatus::Failed;
        }
    }
}
```
**`if (func_)`** — Checks whether the `std::function` holds a callable. An empty `std::function` is falsy; calling it would throw `std::bad_function_call`.

**`catch (...)`** — Catches **every possible exception** — `std::exception` subclasses, integers, strings, anything. This is essential because task code is user-supplied and could throw anything.

**`std::current_exception()`** — Captures the currently active exception into an `exception_ptr`. The worker thread can then pass this pointer to `ThreadPool::on_task_complete()` which puts it into a `std::promise` via `set_exception()`. When the caller calls `future.get()`, the exception is automatically re-thrown.

```cpp
bool Task::operator<(const Task& other) const noexcept {
    if (priority_ != other.priority_) {
        return priority_ < other.priority_;   // Higher enum → closer to top
    }
    if (created_at_ != other.created_at_) {
        return created_at_ > other.created_at_; // Earlier time → closer to top (FIFO)
    }
    return id_ > other.id_;                     // UUID tiebreaker (determinism)
}
```
**C++ Concept — Max-Heap Comparator:**  
`std::push_heap`/`pop_heap` build a **max-heap**: the element for which no other element `x < element` sits at the top. So `operator<(A, B)` meaning "A is less important than B" means B goes to the top.

**Three levels of comparison:**
1. **Priority:** `priority_ < other.priority_` → lower enum value = "less important" → higher enum value bubbles to top. `Critical(3) > High(2) > Normal(1) > Low(0)`. ✓
2. **Timestamp (FIFO):** `created_at_ > other.created_at_` → later creation time = "less important" → earlier task goes first. This enforces FIFO within the same priority tier. ✓
3. **UUID tiebreaker:** Two tasks could have the exact same priority AND the exact same `created_at_` timestamp (if `steady_clock` resolution is coarser than the task creation rate). Without a tiebreaker, the comparator would not be a **strict weak ordering** — required by the C++ standard for heap operations. The UUID comparison makes it strictly deterministic. ✓

---

## File 4 — `include/ctee/core/task_queue.h` + `src/core/task_queue.cpp`

### A. Identity
- **Type:** Internal class (`ctee::detail` namespace).
- **Purpose:** A bounded, priority-ordered queue of pending tasks. Also maintains dependency tracking.
- **Depends on:** `task.h`
- **Used by:** `ThreadPool` (owns it as a member), `Worker` (references it).
- **Essential?** Yes — this is the heart of scheduling.

### B. Key Design — Vector-Backed Heap

```cpp
private:
    std::vector<Task> heap_;
    std::unordered_set<std::string> cancelled_ids_;
    std::unordered_set<std::string> completed_ids_;
    std::size_t capacity_;
```
**C++ Concept — `std::push_heap` / `std::pop_heap` on a `std::vector`:**  
The C++ standard library does not have a standalone priority queue that lets you access internals. Instead, it provides **heap algorithms** that operate on any random-access range:
- `std::push_heap(begin, end)` — assumes `[begin, end-1)` is already a heap; re-heapifies after inserting at `end-1`.
- `std::pop_heap(begin, end)` — moves the top element to `end-1` and re-heapifies `[begin, end-1)`.

You do the `push_back` / `pop_back` on the vector yourself; the algorithms rearrange elements in-place. This uses `Task::operator<` to determine ordering. The result: the highest-priority task is always at `heap_[0]`.

**Why not `std::priority_queue`?**  
`std::priority_queue` wraps these same algorithms but hides the container, preventing things like: iterating all tasks, cancelling a specific task, or implementing dependency checking. The raw vector + heap approach gives full control.

**`cancelled_ids_` and `completed_ids_`:**  
`std::unordered_set<std::string>` — O(1) average lookup. When a task is cancelled, its ID is inserted into `cancelled_ids_`. When a task completes, its ID is inserted into `completed_ids_`. These sets are consulted during `pop()`.

### C. Line-by-Line — `task_queue.cpp`

```cpp
void TaskQueue::push(Task task) {
    if (heap_.size() >= capacity_) { return; }
    heap_.push_back(move(task));
    push_heap(heap_.begin(), heap_.end());
}
```
**`move(task)`** — `Task` is not copyable (copy constructor is `= delete`). You must move it into the vector. `std::move` casts `task` to an rvalue reference, enabling the move constructor.

**`push_heap`** uses `Task::operator<` to sift the new element up into its correct position. O(log n).

```cpp
optional<Task> TaskQueue::pop() {
    vector<Task> deferred;
    optional<Task> result;

    while (!heap_.empty()) {
        pop_heap(heap_.begin(), heap_.end());
        Task task = move(heap_.back());
        heap_.pop_back();

        if (cancelled_ids_.count(task.id()) > 0) { continue; }

        bool deps_met = true;
        for (const auto& dep_id : task.depends_on()) {
            if (completed_ids_.count(dep_id) == 0) {
                deps_met = false;
                break;
            }
        }

        if (!deps_met) { deferred.push_back(move(task)); continue; }

        result = move(task);
        break;
    }

    for (auto& t : deferred) {
        heap_.push_back(move(t));
        push_heap(heap_.begin(), heap_.end());
    }

    return result;
}
```
**C++ Concept — `std::optional<T>`:**  
Represents a value that may or may not be present. `std::optional<Task>` either contains a `Task` or is empty (`std::nullopt`). The caller checks `if (!task_opt)` to know if the queue had nothing ready. This is safer than returning a pointer (raw pointers add lifetime risks) or throwing an exception (expensive for a common case like "queue momentarily empty").

**The dependency-checking loop:**
1. Pop the highest-priority task from the heap.
2. If cancelled → skip (discard it).
3. For each ID in `task.depends_on()`: if that ID is not in `completed_ids_` → push into `deferred`.
4. If all deps met → this is our task; break.
5. After the loop, push all `deferred` tasks back into the heap.

**Important note:** This is **O(n) worst-case** if many high-priority tasks all have unsatisfied dependencies. The comment in the source explicitly acknowledges this: "O(n) in worst case but fine for typical queue sizes." For production use with thousands of interdependent tasks, a proper DAG scheduler would be needed.

**Why `optional<Task>` instead of returning by reference?**  
`Task` is move-only. Returning by value (`Task`) would require the caller to handle the "queue empty" case differently (maybe throw?). `optional<Task>` cleanly models "there might not be anything ready."

---

## File 5 — `include/ctee/core/task_history.h` + `src/core/task_history.cpp`

### A. Identity
- **Type:** Audit log — thread-safe, bounded FIFO record of task completions.
- **Purpose:** Lets the HTTP layer serve historical task data to the frontend dashboard.
- **Depends on:** `task_result.h`
- **Used by:** `ThreadPool` (owns a `shared_ptr<TaskHistory>`).

### B. Key Structures

```cpp
struct TaskRecord {
    std::string task_id;
    std::string task_name;
    TaskStatus status = TaskStatus::Pending;
    std::chrono::steady_clock::time_point submitted_at;
    std::chrono::steady_clock::time_point completed_at;
    std::chrono::milliseconds execution_time{0};
};
```
A snapshot of a single task's lifecycle — two timestamps (submitted + completed) plus status and execution time.

```cpp
class TaskHistory {
    std::size_t max_size_;
    mutable std::mutex mutex_;
    std::deque<TaskRecord> entries_;
};
```
**`mutable std::mutex`:** The mutex is `mutable` so that `const` methods like `recent()` and `count()` can still lock it. (A `const` method normally cannot modify members; `mutable` is the exception.)

**`std::deque<TaskRecord>`:** A double-ended queue. `push_back` at the tail, `pop_front` from the head when the size limit is exceeded. This gives O(1) insert + O(1) trim, which is better than `std::vector` (which would need O(n) to remove the front).

### C. The Two-Phase Record Pattern

```cpp
void TaskHistory::record_submit(const string& task_id, ...) {
    TaskRecord entry;
    entry.task_id = task_id;
    entry.status = TaskStatus::Queued;
    entry.submitted_at = at;
    record(entry);   // ← pushes into deque
}

void TaskHistory::record_complete(const string& task_id, TaskStatus status, ...) {
    lock_guard lock(mutex_);
    for (auto& e : entries_) {          // ← O(n) linear scan
        if (e.task_id == task_id) {
            e.status = status;
            e.completed_at = ch::steady_clock::now();
            e.execution_time = exec_time;
            return;
        }
    }
}
```
**Two-phase update:** First a record is inserted at submission time (with `status = Queued`). Then on completion, the same record is found and updated in-place. The linear scan in `record_complete` is O(n) — acceptable for the default `max_size = 10000`, but would be a bottleneck under very high throughput. A `std::unordered_map<string, size_t>` index would make this O(1).

---

## File 6 — `include/ctee/core/thread_pool_config.h`

### A. Identity
- **Type:** Plain data structures (no methods, no behavior).
- **Purpose:** All configuration knobs and output data structures for the thread pool.
- **Depends on:** Nothing (pure standard library).

### B. Line-by-Line

```cpp
struct ThreadPoolConfig {
    std::size_t min_threads = 2;
    std::size_t max_threads = std::max<std::size_t>(2, std::thread::hardware_concurrency());
    std::size_t max_queue_size = 10000;
    std::chrono::milliseconds task_timeout{30000};
    bool enable_metrics = true;

    std::chrono::milliseconds idle_timeout{5000};
    std::size_t scale_up_threshold = 10;

    std::size_t default_max_retries = 0;
    std::chrono::milliseconds default_retry_delay{1000};
    std::chrono::milliseconds default_task_timeout{0};
};
```
**`std::thread::hardware_concurrency()`** — Returns the number of logical CPU cores on the machine (e.g., 8 on a quad-core hyperthreaded CPU). May return 0 if the system can't determine it — hence `std::max<std::size_t>(2, ...)` ensures at least 2 threads even on exotic platforms.

**`scale_up_threshold = 10`** — If the queue has ≥ 10 pending tasks and the pool hasn't reached `max_threads`, a new worker is spawned immediately. Tune this lower for latency-sensitive workloads, higher for throughput-oriented ones.

**`default_task_timeout{0}`** — Zero means "no timeout." Non-zero enables the detached-thread timeout mechanism in `Worker::run()`.

**`struct ThreadPoolStats`** — A read-only snapshot returned by `pool.stats()`. This is a plain data transfer object; it has no methods. Distinct from `MetricsSnapshot` (which is owned by `Metrics`) — `ThreadPoolStats` is the public API shape.

**`struct WorkerInfo`** — Contains a worker's `id`, `state` (as a string like `"running"`), and `marked_stop` flag. Returned by `pool.get_worker_infos()`.

**`struct QueueInfo`** — Contains `size`, `capacity`, and `is_full`. Returned by `pool.queue_info()`.

---

## File 7 — `include/ctee/core/metrics.h` + `src/core/metrics.cpp`

### A. Identity
- **Type:** Thread-safe performance counter + time-series store.
- **Purpose:** Tracks submitted/completed/failed/cancelled tasks, queue length, thread utilization, timing averages, and stores snapshots for charting.
- **Two-tier design:** Atomic counters for the hot path; a mutex for timing accumulators and history.

### B. The Two-Tier Thread-Safety Model

```cpp
// Tier 1: Lock-free atomics (hot path — called on every submit/complete)
std::atomic<std::size_t> submitted_{0};
std::atomic<std::size_t> completed_{0};
std::atomic<std::size_t> failed_{0};
std::atomic<std::size_t> cancelled_{0};
std::atomic<std::size_t> running_{0};
std::atomic<std::size_t> queue_length_{0};
std::atomic<std::size_t> peak_queue_length_{0};
std::atomic<std::size_t> active_threads_{0};
std::atomic<long long> total_busy_ns_{0};

// Tier 2: Mutex-protected accumulators (cold path — less frequent)
mutable std::mutex timing_mutex_;
std::chrono::nanoseconds total_wait_time_{0};
std::chrono::nanoseconds total_exec_time_{0};
std::size_t wait_samples_{0};
std::size_t exec_samples_{0};

// Tier 2: History (cold path — updated ~1/sec)
mutable std::mutex history_mutex_;
std::deque<MetricsSnapshot> history_;
```
**C++ Concept — `std::atomic<T>`:**  
An atomic type guarantees that reads and writes are **indivisible** — no other thread can see a partially-written value. It also provides **memory ordering** guarantees. The two most important orderings used here:

- `memory_order_relaxed` — The operation is atomic (no torn reads/writes) but provides no ordering guarantees relative to other memory accesses. Used for simple counters where the exact interleaving doesn't matter.
- `memory_order_release` / `memory_order_acquire` — A release-store makes all prior writes visible to any thread that does an acquire-load of the same variable.

**Why atomics instead of a mutex for counters?**  
A `mutex` lock/unlock costs ~20–100 ns and causes cache-line contention between cores. An atomic `fetch_add` with `relaxed` ordering costs ~1–5 ns. On a pool with hundreds of tasks per second, this difference matters significantly.

### C. The Peak Queue Length — Compare-Exchange Loop

```cpp
void Metrics::set_queue_length(std::size_t len) {
    queue_length_.store(len, std::memory_order_relaxed);

    std::size_t prev = peak_queue_length_.load(std::memory_order_relaxed);
    while (len > prev) {
        if (peak_queue_length_.compare_exchange_weak(
                prev, len, std::memory_order_relaxed)) {
            break;
        }
    }
}
```
**C++ Concept — `compare_exchange_weak` (CAS — Compare And Swap):**  
This is the fundamental primitive for lock-free programming. It atomically:
1. Reads the current value of `peak_queue_length_`.
2. Compares it to `prev`.
3. If equal: stores `len` and returns `true`.
4. If not equal: updates `prev` with the actual value and returns `false`.

The `weak` variant may spuriously fail (return false even when equal) — this is acceptable here because we're in a `while` loop anyway. `compare_exchange_strong` never spuriously fails but is slightly slower.

**Why this pattern?** Because two threads could simultaneously read `peak_queue_length_ = 5` and both try to write `6`. Without CAS, both might succeed and write `6` — but one of them might actually have had the real max. CAS ensures only one wins the race.

### D. `tick()` — Time-Series Snapshot

```cpp
void Metrics::tick() {
    auto now = ch::steady_clock::now();
    auto elapsed = ch::duration_cast<ch::milliseconds>(now - last_tick_time_);
    if (elapsed.count() < 1000) return;  // guard: only once per second
    last_tick_time_ = now;
    auto snap = snapshot();
    // compute tasks_per_sec from delta with previous snapshot...
    record_snapshot(snap);
}
```
Called from `ThreadPool::on_task_complete()` after every task. The 1-second guard means a snapshot is recorded at most once per second. The rolling history holds 60 snapshots = ~1 minute of data for the frontend chart.

**`tasks_per_sec`** is a derived metric: `(completed_now - completed_prev) / elapsed_seconds`. This gives a moving rate, not a lifetime average.

---

## File 8 — `include/ctee/core/worker.h` + `src/core/worker.cpp`

### A. Identity
- **Type:** Internal class (`ctee::detail`) — one execution thread.
- **Purpose:** Owns a `std::jthread`, runs a loop that pops tasks and executes them.
- **Depends on:** `task_queue.h`, `task_result.h`.
- **Used by:** `ThreadPool` (creates workers via `add_worker()`).

### B. Line-by-Line — `worker.h`

```cpp
enum class State : std::uint8_t {
    Idle, Waiting, Running, Stopped
};
```
The 4-state lifecycle:
- **Idle** — Initial state, set at construction.
- **Waiting** — Worker is blocked in `cv_.wait(...)`, waiting for a task.
- **Running** — Worker is actively executing a task.
- **Stopped** — Worker has exited its `run()` loop and the `jthread` is about to return.

```cpp
using TaskCompleteCallback = std::function<void(
    const std::string& task_id,
    TaskStatus status,
    std::chrono::milliseconds wait_time,
    std::chrono::milliseconds exec_time)>;
using TaskStartCallback = std::function<void()>;
```
Two callbacks injected from `ThreadPool::add_worker()`. These decouple `Worker` from `ThreadPool` at compile time — the worker doesn't need to `#include "thread_pool.h"`.

```cpp
Worker(std::size_t id,
       std::mutex& mutex,
       std::condition_variable_any& cv,
       TaskQueue& queue,
       bool& pool_running,
       bool& pool_paused,
       TaskCompleteCallback on_complete = nullptr,
       TaskStartCallback on_start = nullptr);
```
The Worker takes **references** to the pool's shared state: the mutex, the condition variable, the queue, and the two bool flags. These are the exact same objects owned by `ThreadPool`. The worker never copies them — it just uses the pool's originals.

**Why `std::condition_variable_any` instead of `std::condition_variable`?**  
`std::condition_variable` only works with `std::unique_lock<std::mutex>`. `std::condition_variable_any` works with **any** lockable type. This is needed because `cv_.wait(lock, stop_token, predicate)` — the C++20 stop-token overload — requires `condition_variable_any`. Without it, you cannot integrate stop tokens with condition variable waits.

```cpp
std::atomic<State> state_{State::Idle};
std::atomic<bool> should_stop_{false};
```
Both are `atomic` because they are read from two different threads simultaneously: the worker thread (writes `state_`) and the pool's main thread (reads `state_` in `try_scale_down()` and `reap_stopped_workers()`). Without `atomic`, this would be a data race — undefined behavior in C++.

```cpp
private:
    void run(std::stop_token stop_token);
    std::jthread thread_;
```
**C++ Concept — `std::jthread` (C++20):**  
`std::jthread` is an upgrade of `std::thread`. It adds two things:
1. **Automatic join on destruction** — `std::thread` requires you to manually `join()` or `detach()` before destruction or the program calls `std::terminate()`. `std::jthread` joins automatically in its destructor.
2. **Built-in stop token** — `std::jthread` carries a `std::stop_source`. You can call `thread.request_stop()` to ask it to stop, and the thread's function receives a `std::stop_token` argument to check `stop_token.stop_requested()`.

### C. Line-by-Line — `worker.cpp`

```cpp
Worker::Worker(...) : id_(id), mutex_(mutex), ..., on_complete_(move(on_complete)), ... {
    thread_ = jthread([this](stop_token token) { run(token); });
}
```
The `jthread` is started in the constructor body (after the member init list). The lambda captures `this` (the `Worker` pointer) and the `stop_token` passed by `jthread` automatically. The thread immediately begins executing `run(token)`.

**Lifetime safety:** The `Worker` object must outlive the thread. Since `ThreadPool` owns `unique_ptr<Worker>` objects, and the pool's `shutdown()` calls `worker.reset()` (which calls `Worker::~Worker()` which joins the thread), the lifetime is correctly managed.

```cpp
Worker::~Worker() {
    if (thread_.joinable()) {
        thread_.request_stop();
        cv_.notify_all();
    }
}
```
`jthread` auto-joins on destruction — but only after its internal stop source has been signaled. `request_stop()` sets the stop token. `cv_.notify_all()` wakes the worker from its `cv_.wait()` so it can check the stop token and exit. Without `notify_all()`, the worker could be asleep indefinitely, and the destructor would block forever waiting for the join.

```cpp
void Worker::run(stop_token stop_token) {
    { ostringstream oss; oss << "ctee-w" << id_;
      ctee::common::set_thread_name(oss.str()); }
```
Names the thread `"ctee-w0"`, `"ctee-w1"`, etc. in debuggers and profilers. Purely a debugging aid.

```cpp
    state_.store(State::Waiting, memory_order_release);

    while (true) {
        optional<Task> task_opt;
        {
            unique_lock lock(mutex_);

            cv_.wait(lock, stop_token, [this] {
                return (!queue_.empty() && !pool_paused_)
                    || !pool_running_
                    || should_stop_.load(memory_order_acquire);
            });
```
**C++ Concept — `cv_.wait(lock, stop_token, predicate)`:**  
This is the C++20 stop-token–aware overload of `condition_variable_any::wait`. It atomically:
1. Releases `lock` and puts the thread to sleep.
2. Wakes when: (a) `cv_.notify_one()/notify_all()` is called AND the predicate returns `true`, OR (b) the stop token is signaled.

**The predicate:** `[this] { return (!queue_.empty() && !pool_paused_) || !pool_running_ || should_stop_; }`  
The predicate guards against **spurious wakeups** — `condition_variable::wait` can randomly wake up with no notification. The predicate is rechecked every time the thread wakes. If it returns `false`, the thread goes back to sleep immediately.

**Why check `should_stop_` with `memory_order_acquire`?**  
`ThreadPool::try_scale_down()` calls `worker->request_stop()` which does `should_stop_.store(true, memory_order_release)`. The acquire in the predicate ensures that once the worker sees `should_stop_ == true`, it also sees all writes that happened before the release-store in `request_stop()`.

```cpp
            if (!pool_running_ && queue_.empty()) { break; }
            if (stop_token.stop_requested() && queue_.empty()) { break; }
            if (should_stop_.load(memory_order_acquire) && queue_.empty()) { break; }
            if (pool_paused_ || queue_.empty()) { continue; }

            task_opt = queue_.pop();
        }
```
**Three exit conditions:**
1. Pool is shut down AND queue is empty → break (normal shutdown).
2. Stop token was requested AND queue is empty → break (jthread's built-in stop mechanism).
3. `should_stop_` flag is set AND queue is empty → break (scale-down).

Note the `&& queue_.empty()` on conditions 1–3. This means: even if told to stop, the worker **drains remaining tasks first**. This prevents task loss during scale-down.

**`if (pool_paused_ || queue_.empty()) { continue; }`** — If paused or nothing available yet, loop back to `cv_.wait`.

**`task_opt = queue_.pop()`** — Pops the highest-priority ready task. Returns `nullopt` if all tasks have unsatisfied dependencies. The lock is still held during this call, ensuring no other worker can pop the same task.

**The lock is released immediately after the closing `}`** — the `unique_lock lock(mutex_)` destructor unlocks it. From this point, the task is held only by this worker.

```cpp
        state_.store(State::Running, memory_order_release);
        auto start_time = ch::steady_clock::now();
        auto wait_time = ch::duration_cast<ch::milliseconds>(
            start_time - task_opt->created_at());
        if (on_start_) on_start_();
```
`wait_time` = time from task creation to worker picking it up. This metric shows queue congestion. `on_start_()` calls `metrics_->record_start()` which increments `running_` atomic counter.

```cpp
        // === TIMEOUT PATH ===
        if (task_opt->timeout_ms().count() > 0) {
            auto task_ptr = make_shared<Task>(move(*task_opt));

            shared_future<void> done;
            {
                promise<void> p;
                done = p.get_future().share();
                thread([task_ptr, promise = move(p)]() mutable {
                    task_ptr->execute();
                    promise.set_value();
                }).detach();
            }

            if (done.wait_for(task_ptr->timeout_ms()) == future_status::ready) {
                if (on_complete_) {
                    on_complete_(task_ptr->id(), task_ptr->status(),
                                 wait_time, task_ptr->result().execution_time);
                }
            } else {
                if (on_complete_) {
                    on_complete_(task_ptr->id(), TaskStatus::TimedOut,
                                 wait_time, task_ptr->timeout_ms());
                }
            }
            state_.store(State::Waiting, memory_order_release);
            continue;
        }
```
**C++ Concept — `std::shared_future<void>`:**  
A `std::future` can only be `get()`-ted once. `std::shared_future` can be waited on by multiple threads. Here only one thread waits, but the reason for using `shared_future` is that its **destructor does not block** — a regular `std::future` returned by `std::async` does block. If we used a regular `future`, destroying it before it's ready would wait for the task to complete, making the "timeout" meaningless.

**The timeout mechanism:**
1. Move the task into a `shared_ptr<Task>` so it lives beyond the worker's scope.
2. Create a `std::promise<void>` + `shared_future`.
3. Launch a new raw `std::thread` that executes the task and then signals the promise.
4. `.detach()` the thread — it runs independently.
5. `done.wait_for(timeout)` — blocks the **worker** thread for at most `timeout_ms`.
6. If `future_status::ready` → the task finished in time → read its result and report.
7. If timeout → report `TimedOut`. **Do not read `task_ptr->` fields** — the detached thread may still be writing them (data race). Just report the timeout.

**Important limitation:** The detached thread **cannot be interrupted**. C++ threads have no kill mechanism. On timeout, the worker moves on, but the task continues running in the background until it naturally completes. This is documented in the header comment.

```cpp
        // === NORMAL PATH (no timeout) ===
        task_opt->execute();
        auto exec_time = task_opt->result().execution_time;
        if (on_complete_) {
            on_complete_(task_opt->id(), task_opt->status(), wait_time, exec_time);
        }
        state_.store(State::Waiting, memory_order_release);
    }

    state_.store(State::Stopped, memory_order_release);
}
```
After calling `on_complete_`, the worker transitions back to `Waiting` and loops. When the loop breaks (any of the three exit conditions), `Stopped` is stored with `release` ordering — this ensures that `ThreadPool::reap_stopped_workers()` which checks `is_stopped()` with `relaxed` load sees the final value correctly (because `relaxed` is fine when there's an external synchronization point — the thread join — between the store and the read).

---

## File 9 — `include/ctee/core/thread_pool.h` + `src/core/thread_pool.cpp`

### A. Identity
- **Type:** The central public API class.
- **Purpose:** Coordinates workers, queue, metrics, history, retry logic, auto-scaling. The primary interface for all callers.
- **Depends on:** All core headers.
- **Essential?** Yes — this is the library.

### B. The Type Trait — `is_packaged_task`

```cpp
template<typename T>
struct is_packaged_task : std::false_type {};

template<typename R, typename... Args>
struct is_packaged_task<std::packaged_task<R(Args...)>> : std::true_type {};

template<typename T>
inline constexpr bool is_packaged_task_v = is_packaged_task<T>::value;
```
**C++ Concept — Template Specialization for Type Detection:**  
The primary template says "nothing is a `packaged_task` by default." The specialization says "if `T` is specifically `std::packaged_task<R(Args...)>`, then yes it is." This is a **compile-time type check** — it runs during compilation, not at runtime.

**Why is this needed?** — `ThreadPool` has three `submit` overloads. The main template overload (`submit(F&&, Args&&...)`) uses `requires (!is_packaged_task_v<...>)` to exclude `std::packaged_task` arguments, ensuring they go to the dedicated `packaged_task` overload instead.

### C. The Three `submit` Overloads

**Overload 1: Generic callable + arguments (the template overload)**

```cpp
template<typename F, typename... Args>
    requires std::invocable<F, Args...>
          && (!is_packaged_task_v<std::decay_t<F>>)
[[nodiscard]] auto submit(F&& f, Args&&... args)
    -> std::future<std::invoke_result_t<F, Args...>>;
```

**C++ Concepts — Let's break this apart:**

`template<typename F, typename... Args>` — `F` is the callable type, `Args...` is a variadic pack of argument types (zero or more).

`requires std::invocable<F, Args...>` — C++20 **concept constraint**. At compile time, this checks: "Can I call an object of type `F` with arguments of types `Args...`?" If not, this overload is excluded from overload resolution (SFINAE, but cleaner). This gives a readable error message instead of a cryptic template failure.

`std::decay_t<F>` — Strips references and CV qualifiers from `F`. If you pass a lambda as `F = MyLambda&`, `decay_t<F> = MyLambda`. Needed so `is_packaged_task_v` matches the bare type.

`std::invoke_result_t<F, Args...>` — Computes the return type of calling `F` with `Args...` at compile time. If you pass `[]() { return 42; }`, this is `int`. If you pass `[]() {}`, this is `void`.

`[[nodiscard]]` — Compiler warning if you don't save the returned `future`.

**Inside the implementation:**

```cpp
using R = std::invoke_result_t<F, Args...>;

auto promise = std::make_shared<std::promise<R>>();
auto future = promise->get_future();
auto once = std::make_shared<std::once_flag>();
auto setter = std::make_shared<std::function<void()>>([](){});
```
**C++ Concept — `std::promise<R>` and `std::future<R>`:**  
A `promise` is the **write end** of a one-shot communication channel. A `future` is the **read end**. They share internal state via a `shared_ptr` allocated inside.
- `promise->set_value(result)` — Stores the result and marks the state as ready.
- `promise->set_exception(ep)` — Stores an exception.
- `future.get()` — Blocks until the state is ready, then returns the value (or re-throws the exception).

`std::make_shared<>` allocates the promise on the heap (via shared_ptr) so it can be safely shared between the submitting thread and the worker thread.

**`std::once_flag` + `std::call_once`:** Guarantees a callable is called **exactly once**, even if multiple threads try simultaneously. Here it ensures that between the cancel handler and the completion callback, only one of them sets the promise. Without this, a race between cancellation and completion could call `set_value` or `set_exception` twice on the same promise — which throws `std::future_error`.

```cpp
Task task([setter, once, promise, fn = std::forward<F>(f),
           ...capture = std::forward<Args>(args)]() mutable {
    try {
        if constexpr (std::is_void_v<R>) {
            std::invoke(std::move(fn), std::move(capture)...);
            *setter = [promise]() { promise->set_value(); };
        } else {
            auto result = std::invoke(std::move(fn), std::move(capture)...);
            auto result_ptr = std::make_shared<R>(std::move(result));
            *setter = [promise, result_ptr]() {
                promise->set_value(std::move(*result_ptr));
            };
        }
    } catch (...) {
        auto ep = std::current_exception();
        *setter = [promise, ep]() { promise->set_exception(ep); };
    }
});
```
**C++ Concepts — all in one lambda:**

`std::forward<F>(f)` — **Perfect forwarding**. If `f` is an lvalue (named variable), forward as lvalue. If rvalue (temporary), forward as rvalue. This preserves the value category through the template, enabling move semantics wherever possible.

`...capture = std::forward<Args>(args)` — Pack expansion inside a lambda capture (C++20 feature). Captures all `args...` by forwarding them into the lambda's closure. Each argument is moved/copied as appropriate.

`mutable` — Lambda captures are `const` by default. `mutable` allows the lambda to modify its captured copies (needed to `std::move` from them during execution).

`if constexpr (std::is_void_v<R>)` — **Compile-time branching**. At compile time, checks if `R` is `void`. If so, calls the function and sets `promise->set_value()` with no argument. Otherwise, stores the return value. Without `if constexpr`, you'd need two separate templates — `if constexpr` merges them. The branch not taken is **not compiled** for that instantiation.

`std::invoke(std::move(fn), std::move(capture)...)` — Calls `fn` with all `capture` arguments, using the most general callable invocation syntax. Handles function pointers, member function pointers, and callable objects uniformly.

**The `setter` indirection:** The lambda doesn't call `promise->set_value()` directly. Instead, it stores a closure in `*setter`. The actual promise fulfillment happens in the completion callback (`completion_callbacks_[id]`). This allows the promise to be set under proper synchronization — but note that in the current implementation, `(*setter)()` is called from within `on_task_complete()` which holds `mutex_`. The `setter` itself is `shared_ptr<std::function<void()>>` — heap-allocated so it can be shared between the task closure and the completion callback.

**Overload 2: `std::packaged_task<R()>`**

```cpp
template<typename R>
std::future<R> ThreadPool::submit(std::packaged_task<R()> task) {
    auto pt = std::make_shared<std::packaged_task<R()>>(std::move(task));
    auto future = pt->get_future();
    Task wrapper([pt]() mutable { (*pt)(); });
    ...
}
```
**C++ Concept — `std::packaged_task<R()>`:**  
A `packaged_task` wraps a callable and provides a built-in promise/future pair. Calling `(*pt)()` executes the callable and stores its result in the internal promise. `pt->get_future()` gets the future end. This overload lets users who already have a `packaged_task` submit it directly, without going through the general template machinery.

**Why `make_shared`?** — `packaged_task` is move-only. To capture it in the Task's `std::function` (which requires copyable captures), it's first moved into a `shared_ptr`. The lambda captures the `shared_ptr` by value — `shared_ptr` is copyable. So when the `std::function` is copied (which it isn't in this codebase, but is required by the type), the shared_ptr's copy constructor runs.

**Overload 3: `submit(Task task)` (defined in `thread_pool.cpp`)**  
Takes a fully constructed `Task` object (with name, priority, retry config, etc. already set by the caller). This is the "full-featured" overload — it also stores retry metadata and records submission in `TaskHistory`.

### D. Auto-Scaling Implementation

**Scale-Up** (inside every `submit()` overload):
```cpp
if (queue_.size() >= config_.scale_up_threshold &&
    workers_.size() < config_.max_threads) {
    add_worker();
}
```
Done **under the same mutex lock** as the push — no window for a race.

```cpp
void ThreadPool::add_worker() {
    auto id = next_worker_id_.fetch_add(1, memory_order_relaxed);
    auto worker = make_unique<detail::Worker>(
        id, mutex_, cv_, queue_, running_, paused_,
        [this](...) { on_task_complete(...); },
        [this]() { metrics_->record_start(); });
    workers_.push_back(move(worker));
    metrics_->set_active_threads(workers_.size());
}
```
`next_worker_id_` is `std::atomic<size_t>` — generates unique IDs without a lock. `fetch_add` atomically increments and returns the previous value. `memory_order_relaxed` suffices because ID uniqueness is all that matters; no ordering with respect to other variables is needed.

**Scale-Down:**
```cpp
void ThreadPool::try_scale_down() {
    std::size_t live = workers_.size() - stopping_count_;
    if (queue_.empty() && live > config_.min_threads) {
        std::size_t excess = live - config_.min_threads;
        for (auto& w : workers_) {
            if (excess == 0) break;
            if (w && w->state() == detail::Worker::State::Waiting && !w->is_marked_stop()) {
                w->request_stop();
                ++stopping_count_;
                --excess;
            }
        }
    }
}
```
`stopping_count_` tracks how many workers have been marked for stop but not yet reaped. `live = workers_.size() - stopping_count_` gives the true live worker count. Only `Waiting` workers are marked (never `Running` ones). `request_stop()` sets `should_stop_` atomic + notifies the CV. The worker sees this at its next wait cycle and exits after draining.

**Reaping:**
```cpp
void ThreadPool::reap_stopped_workers() {
    auto it = workers_.begin();
    while (it != workers_.end()) {
        if ((*it)->is_stopped()) {
            it->reset();     // calls Worker::~Worker() → jthread auto-joins
            it = workers_.erase(it);
            if (stopping_count_ > 0) --stopping_count_;
        } else { ++it; }
    }
    metrics_->set_active_threads(workers_.size());
}
```
Called **only from `submit()`** — which is only called from the user/submitter thread (never from a worker thread). This is critical: `it->reset()` destroys the `Worker`, which joins the `jthread`. If reaping happened inside `on_task_complete()` (which runs on a worker thread), a worker would be trying to `join()` itself — instant deadlock.

### E. Shutdown

```cpp
void ThreadPool::shutdown() {
    {
        lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        paused_ = false;
        completion_callbacks_.clear();
    }
    cv_.notify_all();

    for (auto& worker : workers_) {
        worker.reset();
    }
    workers_.clear();
    metrics_->set_active_threads(0);
}
```
**Sequence:**
1. Set `running_ = false` under `mutex_`.
2. `cv_.notify_all()` — wakes every worker.
3. Each worker: sees `!pool_running_ && queue_.empty()` → breaks its loop → sets `State::Stopped`.
4. `worker.reset()` → `Worker::~Worker()` → `jthread` destructor → auto-join.

**`completion_callbacks_.clear()`** before joining ensures no callback fires during shutdown. If a task completes during shutdown, `on_task_complete()` finds the callback map empty and does nothing.

**What about queued tasks?** — The queue is **not** cleared by `shutdown()`. Queued tasks that haven't been picked up yet are simply abandoned. Futures for those tasks will **never be set** — callers blocking on `future.get()` will block forever unless the `future` is destroyed. This is a documented design trade-off (not a bug per se, but a limitation to be aware of).

### F. The `submit(Task)` Retry Storage

```cpp
if (task.max_retries() > 0) {
    RetryInfo info;
    info.func = task.func();
    info.name = task.name();
    info.priority = task.priority();
    info.max_retries = task.max_retries();
    info.retry_delay = task.retry_delay();
    info.timeout_ms = task.timeout_ms();
    info.depends_on = task.depends_on();
    retry_info_[id] = move(info);
}
```
The `RetryInfo` struct preserves the function + metadata needed to reconstruct a fresh `Task` with the same ID when retrying. The task is pushed into `queue_` with `set_id(task_id)` so its ID is reused — promise resolution deferred until final completion.

---

## File 10 — `include/ctee/core/scheduler.h` + `src/core/scheduler.cpp`

### A. Identity
- **Type:** Independent time-based dispatcher that sits on top of `ThreadPool`.
- **Purpose:** Fires tasks into the pool at a specified future time, or repeatedly on a fixed interval.
- **Depends on:** `thread_pool.h`, `task.h`.
- **Used by:** `main.cpp`, `HTTPServer`/`RouteHandler`, tests.
- **Optional?** Yes — the pool works without a scheduler; the scheduler just adds time-based triggering.

### B. The `ScheduledEntry` and Min-Heap

```cpp
struct ScheduledEntry {
    std::string id;
    std::string name;
    TaskPriority priority;
    std::function<void()> func;
    std::chrono::steady_clock::time_point next_execution;
    std::chrono::milliseconds interval{0};
    std::size_t max_repeats = 0;
    std::size_t repeat_count = 0;
    bool is_recurring = false;
};

struct CompareByTime {
    bool operator()(const ScheduledEntry& a, const ScheduledEntry& b) const {
        return a.next_execution > b.next_execution;  // min-heap: earliest first
    }
};

std::vector<ScheduledEntry> schedule_heap_;
```
Note that `ScheduledEntry` stores a `std::function<void()>` — not a `Task`. A `Task` is move-only; storing it here would prevent rescheduling recurring tasks (you'd use up the task on the first firing). By storing the raw `func`, you can create a fresh `Task` each time the entry fires.

The heap uses a custom `CompareByTime` with `>` — this makes `std::push_heap`/`pop_heap` produce a **min-heap** (earliest deadline at top), opposite to the max-heap in `TaskQueue`.

### C. The Timer Loop

```cpp
void Scheduler::timer_loop(stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        unique_lock lock(mutex_);

        // Wait until the heap is non-empty or shutdown
        cv_.wait(lock, stop_token, [&] {
            return !schedule_heap_.empty() || !running_;
        });

        if (!running_) break;

        // Handle pause
        while (paused_ && running_) {
            cv_.wait(lock, stop_token, [&] { return !paused_ || !running_; });
        }

        // Sleep until the next deadline (or a new earlier task arrives)
        auto deadline = schedule_heap_.front().next_execution;
        cv_.wait_until(lock, stop_token, deadline, [&] {
            return (!schedule_heap_.empty() &&
                    schedule_heap_.front().next_execution < deadline) ||
                   !running_;
        });

        auto now = ch::steady_clock::now();
        if (schedule_heap_.front().next_execution > now) continue; // not yet
```
**`cv_.wait_until(lock, stop_token, deadline, predicate)`:**  
Sleeps until either: the `deadline` time point arrives, OR the predicate becomes true (a new task with an earlier deadline was added), OR the stop token fires. This is more efficient than a polling loop — the thread consumes no CPU while sleeping.

**Why wake early if a new earlier task arrives?**  
If the heap's front deadline is `T+10s` and you call `schedule(task, T+2s)`, you push the new entry, re-heapify, and `notify_one()` the CV. The timer thread wakes, re-reads `schedule_heap_.front()`, sees `T+2s`, and re-sleeps until that new deadline.

```cpp
        pop_heap(schedule_heap_.begin(), schedule_heap_.end(), CompareByTime{});
        ScheduledEntry entry = move(schedule_heap_.back());
        schedule_heap_.pop_back();

        if (cancelled_ids_.count(entry.id)) {
            cancelled_ids_.erase(entry.id);
            continue;
        }

        bool should_reschedule = entry.is_recurring &&
            (entry.max_repeats == 0 || entry.repeat_count + 1 < entry.max_repeats);

        lock.unlock();  // ← unlock before calling pool_.submit()

        if (!paused_) {
            Task t(entry_func, entry_name, entry_priority);
            [[maybe_unused]] auto fut = pool_.submit(move(t));
        }

        if (should_reschedule) {
            lock.lock();
            // create next entry with repeat_count + 1 and next_execution += interval
            schedule_heap_.push_back(move(next));
            push_heap(...);
            lock.unlock();
        }
    }
}
```
**`lock.unlock()` before `pool_.submit()`:**  
`pool_.submit()` also acquires `mutex_` (the pool's mutex — not the scheduler's `mutex_`). These are two different mutexes. But releasing the scheduler's lock before calling into the pool prevents holding two locks simultaneously, which reduces deadlock risk and contention.

**`[[maybe_unused]] auto fut = pool_.submit(move(t))`:**  
The future is obtained but immediately discarded. The scheduler fires tasks and doesn't track their results. `[[maybe_unused]]` suppresses the "unused variable" compiler warning.

**Recurring task re-scheduling:**  
After submitting, if `should_reschedule`, the entry is re-pushed into the heap with `next_execution = now + interval` and `repeat_count + 1`. If `max_repeats > 0` and `repeat_count + 1 >= max_repeats`, `should_reschedule` is false and the entry is not re-pushed — it fires once more and disappears.
