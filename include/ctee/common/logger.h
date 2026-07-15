#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ctee {

// ---------------------------------------------------------------------------
// LogContext — optional structured fields attached to every log entry.
//
// worker_id:  Non-zero when the log originates from a pool worker thread.
// task_id:    Non-empty when a task is being processed.
// ---------------------------------------------------------------------------
struct LogContext {
    std::size_t worker_id = 0;
    std::string task_id;
};

// ---------------------------------------------------------------------------
// Logger — thread-safe, asynchronous logger with file and console output.
//
// Architecture:
//   Callers push LogEntry structs into a lock-free deque and notify a
//   background worker thread.  The worker drains the queue in batches and
//   writes formatted lines to stdout and/or a log file.
//
// Thread safety:
//   - log() / debug() / info() / warn() / error() are safe to call from
//     any thread concurrently.
//   - set_level(), set_console_enabled(), set_file_output() are safe to
//     call from any thread (they use relaxed atomics / mutex-protected).
//   - flush() blocks until all pending entries have been written.
//
// Format:
//   [2024-07-14 23:16:38.123] [INFO] [Worker:2] [Task:abc-123] message
// ---------------------------------------------------------------------------
class Logger {
public:
    enum class Level : std::uint8_t {
        Debug = 0,
        Info  = 1,
        Warn  = 2,
        Error = 3
    };

    static Logger& instance();

    Logger();
    explicit Logger(Level level, bool console_enabled = true);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) = delete;
    Logger& operator=(Logger&&) = delete;

    // ── Logging (backward-compatible, no context) ──────────────────────

    void debug(const std::string& message);
    void info(const std::string& message);
    void warn(const std::string& message);
    void error(const std::string& message);

    // ── Logging (with structured context) ──────────────────────────────

    void debug(const std::string& message, const LogContext& ctx);
    void info(const std::string& message, const LogContext& ctx);
    void warn(const std::string& message, const LogContext& ctx);
    void error(const std::string& message, const LogContext& ctx);

    // ── Configuration ──────────────────────────────────────────────────

    void set_level(Level level);
    [[nodiscard]] Level level() const noexcept;

    void set_console_enabled(bool enabled);
    [[nodiscard]] bool console_enabled() const noexcept;

    void set_file_output(const std::string& filepath);
    void close_file();

    // ── Synchronization ────────────────────────────────────────────────

    void flush();

private:
    struct LogEntry {
        Level level;
        std::string message;
        LogContext context;
        std::chrono::system_clock::time_point timestamp;
    };

    void log(Level level, const std::string& message, const LogContext& ctx);
    void worker_loop();
    std::string format_entry(const LogEntry& entry) const;
    void output_line(const std::string& line);

    // Background worker
    std::thread worker_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<LogEntry> queue_;
    std::atomic<bool> stop_{false};

    // Flush synchronization
    std::vector<std::shared_ptr<std::promise<void>>> flush_promises_;

    // Configuration
    std::atomic<Level> level_{Level::Info};
    std::atomic<bool> console_enabled_{true};
    std::mutex file_mutex_;
    std::ofstream file_stream_;
};

}  // namespace ctee
