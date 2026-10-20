# Phase 2 Part B — Infrastructure & Server Layer Files

---

## File 11 — `include/ctee/common/logger.h` + `src/common/logger.cpp`

### A. Identity
- **Type:** Singleton-capable, async background logger.
- **Purpose:** Provides thread-safe `debug/info/warn/error` logging to stdout and/or a file, without ever blocking the calling thread.
- **Depends on:** Standard library only.
- **Used by:** `config.cpp`, `http_server.cpp`, `main.cpp`, and any component that needs logging.
- **Essential?** Yes — the whole application is wired through it.

### B. Architecture — Why Async Logging?

Synchronous logging calls `std::cout << message` inline with the calling thread. On a pool with dozens of concurrent worker threads, this means:
- The mutex protecting `cout` becomes a bottleneck.
- Task execution stalls waiting for the console/file write.

The async approach: calling threads only push entries into a `deque` (protected by a lightweight mutex), then signal a background thread. The background thread does all actual I/O. **The calling thread never waits for I/O to complete.**

### C. The Singleton

```cpp
static Logger& instance() {
    static Logger logger;
    return logger;
}
```
**C++ Concept — Function-local static (Meyers Singleton):**
`static Logger logger` inside a function is initialized exactly once — on the first call to `instance()`. In C++11+, this initialization is **thread-safe** (guaranteed by the standard). Subsequent calls return the same object. The logger is destroyed when the program exits (static storage duration).

`Logger::instance().info("message")` is how every component accesses the global logger without needing a `Logger*` member or constructor injection.

### D. The LogEntry + Queue

```cpp
struct LogEntry {
    Level level;
    std::string message;
    LogContext context;       // worker_id + task_id (optional structured fields)
    std::chrono::system_clock::time_point timestamp;
};

std::deque<LogEntry> queue_;   // protected by mutex_
```
`system_clock` (not `steady_clock`) is used here — `system_clock` gives wall-clock time suitable for human-readable timestamps.

### E. `log()` — The Fast Path

```cpp
void Logger::log(Level l, const string& message, const LogContext& ctx) {
    if (l < level_.load(memory_order_relaxed)) { return; }   // level check — lock-free

    LogEntry entry;
    entry.level = l;
    entry.message = message;
    entry.context = ctx;
    entry.timestamp = ch::system_clock::now();

    {
        lock_guard lock(mutex_);
        queue_.push_back(move(entry));
    }
    cv_.notify_one();
}
```
The level check uses a `relaxed` atomic load — no lock, minimal overhead. If the message level is below the threshold (e.g., `Debug` when level is `Info`), it returns immediately.

The lock is held only long enough to push into the deque. The format/write work happens in the background thread.

### F. `worker_loop()` — The Background Thread

```cpp
void Logger::worker_loop() {
    while (true) {
        deque<LogEntry> batch;
        vector<shared_ptr<std::promise<void>>> flush_batch;

        {
            unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty() || !flush_promises_.empty()
                    || stop_.load(memory_order_acquire);
            });

            batch.swap(queue_);                // O(1) — swap ownership of the deque
            flush_batch.swap(flush_promises_);
        }

        for (const auto& entry : batch) {
            output_line(format_entry(entry));
        }

        for (auto& p : flush_batch) {
            p->set_value();   // unblocks whoever called flush()
        }

        if (stop_.load(memory_order_acquire) && queue_.empty()) break;
    }
}
```
**`batch.swap(queue_)`** — Swaps the internal data of the two deques in O(1). This "drains" the shared queue into a private `batch`, releasing the lock immediately. All formatting and I/O then happens outside the lock — so new log calls can queue up while the background thread is writing.

**`flush_promises_`** — When `flush()` is called, a `promise<void>` is pushed into this vector. After the batch is written, the background thread calls `p->set_value()` on each pending promise, unblocking the caller's `future.get()`. This is the same `promise/future` sync pattern used in `ThreadPool::submit()`.

### G. `flush()` — Synchronous Drain

```cpp
void Logger::flush() {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();
    {
        std::lock_guard lock(mutex_);
        flush_promises_.push_back(promise);
    }
    cv_.notify_one();
    future.get();   // blocks here until the background thread processes everything
}
```
Used in tests and shutdown to ensure all pending log messages have been written before the process exits.

### H. Formatting

```cpp
string Logger::format_entry(const LogEntry& entry) const {
    // Produces: [2024-07-14 23:16:38.123] [INFO] [Worker:2] [Task:abc-123] message
    ...
    if (entry.context.worker_id > 0)
        oss << " [Worker:" << entry.context.worker_id << ']';
    if (!entry.context.task_id.empty())
        oss << " [Task:" << entry.context.task_id << ']';
    oss << ' ' << entry.message;
}
```
`LogContext` (worker_id + task_id) is only appended if non-zero/non-empty — backward-compatible with the no-context overloads.

Platform-specific time formatting: `localtime_s` on Windows, `localtime_r` on Linux.

### I. Destructor — Graceful Shutdown

```cpp
Logger::~Logger() {
    stop_.store(true, memory_order_release);
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    close_file();
}
```
`stop_` is set, background thread is woken, its loop processes any remaining entries in the queue, then breaks and the `join()` completes. `close_file()` flushes and closes the `ofstream`.

---

## File 12 — `include/ctee/common/config.h` + `src/common/config.cpp`

### A. Identity
- **Type:** JSON-based configuration loader + aggregate config struct.
- **Purpose:** Loads `ThreadPoolConfig` + `ServerConfig` from a JSON file at startup.
- **Depends on:** `thread_pool_config.h`, `server/server_config.h`, `nlohmann/json` (3rd-party).

### B. `AppConfig`

```cpp
struct AppConfig {
    ThreadPoolConfig pool;
    ServerConfig server;
    std::size_t default_max_retries = 0;
    std::chrono::milliseconds default_retry_delay{1000};
    std::chrono::milliseconds default_task_timeout{0};
    std::size_t max_history_size = 1000;
    bool name_threads = true;
};
```
A single aggregate that wraps all configuration sections. Each section maps to a JSON key: `"pool"`, `"server"`, `"retry"`, `"timeout_ms"`, etc.

### C. `load_config()`

```cpp
AppConfig load_config(const std::string& path) {
    AppConfig config;
    std::ifstream file(path);
    if (!file.is_open()) {
        Logger::instance().info("No config file found, using defaults");
        return config;   // returns the default-constructed config
    }
    try {
        json j;
        file >> j;
        if (j.contains("pool")) {
            auto& p = j["pool"];
            if (p.contains("min_threads"))
                config.pool.min_threads = p["min_threads"].get<std::size_t>();
            ...
        }
        ...
    } catch (const std::exception& e) {
        Logger::instance().error("Failed to parse config: " + string(e.what()));
    }
    return config;
}
```
**Design:** "Fail gracefully" — if the JSON file is missing or malformed, return the default config and log the issue. Never throw to the caller. Each JSON key is checked with `.contains()` before reading, so partial configs work fine.

**`nlohmann/json`** — A header-only C++ JSON library fetched via CMake's `FetchContent`. The `>>` operator parses a JSON stream. `.get<T>()` converts a JSON value to a C++ type with type checking.

**Note:** `main.cpp` does not actually call `load_config()` — it constructs `ThreadPoolConfig` manually. `load_config()` is implemented and available but not wired to the application entry point currently. This is a partially-used feature.

---

## File 13 — `include/ctee/common/thread_naming.h`

### A. Identity
- **Type:** Header-only utility — platform-specific thread naming.
- **Purpose:** Names threads for debugger and profiler visibility (e.g., "ctee-w0").
- **Header-only:** No `.cpp` file; all code is in the header. The `inline` keyword prevents multiple-definition errors when the header is included in multiple translation units.

### B. Line-by-Line

```cpp
#ifdef _WIN32
#include <windows.h>
inline void set_thread_name(const std::string& name) {
    int size = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    std::wstring wide(size, 0);
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, &wide[0], size);

    static auto pSetThreadDescription = reinterpret_cast<decltype(&SetThreadDescription)>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
    if (pSetThreadDescription) {
        HANDLE thread = GetCurrentThread();
        pSetThreadDescription(thread, wide.c_str());
    }
}
#elif defined(__linux__)
inline void set_thread_name(const std::string& name) {
    std::string truncated = name.substr(0, 15);
    pthread_setname_np(pthread_self(), truncated.c_str());
}
#else
inline void set_thread_name(const std::string&) { }  // no-op
#endif
```
**Windows path:**
- `SetThreadDescription` (Win10 1607+) is loaded **dynamically** via `GetProcAddress` rather than linked statically. This means the code compiles and runs on older Windows versions (where the function doesn't exist) without crashing — `pSetThreadDescription` will be `nullptr` and the call is skipped.
- `MultiByteToWideChar` converts UTF-8 to UTF-16 (Windows wide strings), required by `SetThreadDescription`.
- `static auto pSetThreadDescription` — loaded once (first call), cached in the function-local static.

**Linux path:**
- `pthread_setname_np` requires the name to be ≤ 15 characters (kernel limit). The code truncates with `substr(0, 15)`.

**Why name threads?**  
In Visual Studio's debugger or perf tools like `perf`/`VTune`, named threads appear as `"ctee-w0"` instead of `"Thread 1234"`. Makes debugging concurrent issues far easier.

---

## File 14 — `include/ctee/common/non_copyable.h`

```cpp
class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;
    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
    NonCopyable(NonCopyable&&) noexcept = default;
    NonCopyable& operator=(NonCopyable&&) noexcept = default;
};
```
A **mixin base class** that any class can inherit from to instantly become non-copyable but still movable.

**Usage (intended):**
```cpp
class ThreadPool : public NonCopyable { ... };
```
However, `ThreadPool` in the actual code does not inherit from `NonCopyable` — it manually declares `= delete` on its copy operations. `NonCopyable` is defined but **not actually used** by any class in the current codebase. It is available as a utility for future use.

---

## File 15 — `include/ctee/common/result.h`

```cpp
template <typename T>
class Result {
public:
    static Result success(T value) { ... }
    static Result error(std::string message) { ... }

    [[nodiscard]] bool is_success() const noexcept { return success_; }
    [[nodiscard]] const T& value() const& { return std::get<T>(data_); }
    [[nodiscard]] const std::string& error() const& { return std::get<std::string>(data_); }

private:
    bool success_ = false;
    std::variant<T, std::string> data_;
};
```
**C++ Concept — `std::variant<T, std::string>`:**  
`std::variant` is a type-safe union that holds exactly one of its listed types at a time. `std::get<T>(data_)` extracts the value if it holds a `T`; throws `std::bad_variant_access` if it holds a `std::string`. This is the C++ way to model "either a success value OR an error message."

**Usage status:** `Result<T>` is defined but **not used** by any class in the current project. The HTTP layer uses `nlohmann::json` for serialization directly. This is a utility class available for future use or external callers who want a typed error-or-value return type.

---

## File 16 — `include/ctee/server/http_server.h` + `src/server/http_server.cpp`

### A. Identity
- **Type:** HTTP server wrapper around `cpp-httplib`.
- **Purpose:** Exposes the `ThreadPool` and `Scheduler` over a REST API on port 8080.
- **Depends on:** `thread_pool.h`, `scheduler.h`, `route_handler.h`, `cpp-httplib` (3rd-party).

### B. Key Design

```cpp
class HTTPServer {
    ThreadPool& pool_;
    Scheduler& scheduler_;
    std::uint16_t port_;
    RouteHandler handler_;              // owns the route handler
    std::unique_ptr<httplib::Server> server_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
};
```
`httplib::Server` is forward-declared in the header (`namespace httplib { class Server; }`) and defined via `#include <httplib.h>` only in the `.cpp`. This is the **PIMPL-lite** pattern — keeps the heavy httplib header out of anything that includes `http_server.h`.

### C. `start()` and the Server Thread

```cpp
void HTTPServer::start() {
    setup_routes();
    if (!server_->bind_to_port("0.0.0.0", port_)) { ... return; }
    running_ = true;
    server_thread_ = std::thread([this]() {
        server_->listen_after_bind();
    });
}
```
`bind_to_port` + `listen_after_bind` is `cpp-httplib`'s non-blocking bind: bind first, then listen in a separate thread. The main thread returns from `start()` immediately; the `server_thread_` runs the event loop.

### D. REST Route Map

```cpp
server_->Post("/tasks",          [...] { h.submit_task(...); });
server_->Post("/tasks/priority", [...] { h.submit_task_priority(...); });
server_->Post("/tasks/schedule", [...] { h.schedule_task(...); });
server_->Delete(R"(/tasks/([^/]+))", [...] { h.cancel_task(...); });
server_->Get("/workers",         [...] { h.get_workers(...); });
server_->Get("/queue",           [...] { h.get_queue(...); });
server_->Get("/metrics",         [...] { h.get_metrics(...); });
server_->Get("/health",          [...] { h.get_health(...); });
server_->Post("/tasks/batch",    [...] { h.submit_batch(...); });
server_->Get("/tasks/history",   [...] { h.get_task_history(...); });
server_->Get("/reports/performance", [...] { h.get_performance_report(...); });
server_->Post("/pool/pause",     [...] { h.pause_pool(...); });
server_->Post("/pool/resume",    [...] { h.resume_pool(...); });
server_->Get("/config",          [...] { h.get_config(...); });
server_->Post("/shutdown",       [...] { h.shutdown_pool(...); });
server_->Options(R"(/.*)", [](...)  { /* CORS preflight */ });
```
15 routes total. CORS headers (`Access-Control-Allow-Origin: *`) are set both in the OPTIONS handler and as default headers, enabling the React frontend to call the API from a different port.

### E. `stop()` — Atomic Exchange Pattern

```cpp
void HTTPServer::stop() {
    if (running_.exchange(false)) {  // returns old value; sets to false
        server_->stop();
        if (server_thread_.joinable()) server_thread_.join();
    }
}
```
`atomic::exchange(false)` atomically sets `running_` to `false` and returns the old value. If the old value was `true` (server was running), proceed with shutdown. If already `false`, do nothing. This prevents double-stop races.

---

## File 17 — `include/ctee/server/route_handler.h` + `src/server/route_handler.cpp`

### A. Identity
- **Type:** REST endpoint handler — the bridge between HTTP requests and the pool/scheduler.
- **Purpose:** Parses JSON from HTTP requests, calls pool/scheduler methods, and serializes responses to JSON.
- **Depends on:** `thread_pool.h`, `scheduler.h`, `task_registry.h`, `metrics.h`.

### B. Key Endpoints

**POST `/tasks`** → `submit_task()`:
- Parses JSON body: `{ "name": "...", "duration_ms": 100 }`.
- Creates a `Task` whose function sleeps for `duration_ms` (simulated work).
- Calls `pool_.submit(move(task))` (the `Task` overload).
- Registers the task in `registry_` with status `Queued`.
- Sets a completion handler via `pool_.set_completion_handler()` to update `registry_` when done.
- Returns `{ "task_id": "...", "status": "queued" }`.

**GET `/metrics`** → `get_metrics()`:
- Calls `pool_.metrics()->snapshot()` and `pool_.metrics()->history()`.
- Serializes to JSON with all counter fields + time-series array.
- The React frontend polls this endpoint to power the real-time dashboard charts.

**GET `/workers`** → `get_workers()`:
- Calls `pool_.get_worker_infos()`.
- Returns an array: `[{ "id": 0, "state": "running", "marked_stop": false }, ...]`.

**DELETE `/tasks/{id}`** → `cancel_task()`:
- Extracts the task ID from the URL path using a regex capture group.
- Calls `pool_.cancel(id)`.
- Updates `registry_` with `Cancelled` status.

**POST `/tasks/batch`** → `submit_batch()`:
- Parses an array of task specs.
- Creates multiple `Task` objects.
- Calls `pool_.submit_batch(tasks)`.
- Returns an array of task IDs.

### C. `TaskRegistry` — Server-Side Task Tracking

The `registry_` member of `RouteHandler` is a `TaskRegistry` — a separate, server-only metadata store for HTTP-submitted tasks. It is **distinct** from the pool's internal `task_history_` (which tracks all tasks including programmatically submitted ones).

The registry stores `TaskInfo` structs with task type, HTTP metadata, and status. It is updated via `pool_.set_completion_handler()` callbacks that fire when tasks complete.

---

## File 18 — `include/ctee/server/task_registry.h` + `src/server/task_registry.cpp`

### A. Identity
- **Type:** Thread-safe key-value store (task ID → TaskInfo).
- **Purpose:** Tracks HTTP-submitted tasks with their metadata and cancel handlers.
- **Depends on:** `task_priority.h`, `task_result.h`.

### B. Key Implementation Details

```cpp
bool TaskRegistry::cancel(const std::string& id) {
    std::function<bool()> handler;
    {
        std::lock_guard lock(mutex_);
        auto it = cancel_handlers_.find(id);
        if (it == cancel_handlers_.end()) return false;
        handler = it->second;
        cancel_handlers_.erase(it);
    }                               // ← lock released BEFORE calling handler
    bool result = handler();        // calls pool_.cancel(id)
    if (result) update_status(id, TaskStatus::Cancelled);
    return result;
}
```
**Why release the lock before calling `handler()`?**  
`handler()` calls `pool_.cancel(id)` which acquires the pool's `mutex_`. If the registry's `mutex_` were still held, any code path that holds the pool's mutex and then tries to call a registry method would deadlock. By releasing the registry lock first, only one lock is held at a time — no deadlock possible.

**`update_status_if_not_terminal()`:**  
Checks if the status is already `Completed`, `Failed`, or `Cancelled` before updating. Prevents a race where a completion callback and a cancel call both try to set the final status — the first one wins.

---

## File 19 — `src/main.cpp`

```cpp
int main() {
    ctee::Logger::instance().info("Concurrent Task Execution Engine starting...");

    ctee::ThreadPoolConfig config;
    config.min_threads = std::thread::hardware_concurrency();
    config.max_queue_size = 10000;

    ctee::ThreadPool pool(config);
    ctee::Scheduler scheduler(pool);
    ctee::HTTPServer server(pool, scheduler, 8080);

    server.start();
    Logger::instance().info("Server running on http://0.0.0.0:8080");
    Logger::instance().info("Press Enter to stop...");
    std::cin.get();          // blocks here — user presses Enter to stop

    Logger::instance().info("Shutting down...");
    server.stop();
    scheduler.shutdown();
    pool.shutdown();

    Logger::instance().info("Shutdown complete.");
    return 0;
}
```

**Wiring order (construction):**
1. `Logger` — singleton, already exists.
2. `ThreadPoolConfig` — configured manually (not via `load_config()`).
3. `ThreadPool pool(config)` — starts `min_threads` workers immediately.
4. `Scheduler scheduler(pool)` — starts the timer thread, holds a reference to `pool`.
5. `HTTPServer server(pool, scheduler, 8080)` — creates `RouteHandler` + `httplib::Server`.
6. `server.start()` — binds port + starts server thread.

**Shutdown order (destruction, manually called):**
1. `server.stop()` — stops accepting HTTP requests.
2. `scheduler.shutdown()` — stops timer thread; pending scheduled tasks are abandoned.
3. `pool.shutdown()` — signals workers, waits for running tasks, joins threads.

**Object lifetimes are correct:** The `Scheduler` holds a `ThreadPool&` reference. The `HTTPServer` holds `ThreadPool&` and `Scheduler&`. All three are stack-allocated in `main()`, so they exist for the same lifetime. No dangling references.

**Note:** `load_config()` is not called. Thread naming is enabled by default in `Worker` (calls `set_thread_name`) but `AppConfig.name_threads` is not checked — the naming always happens.
