#include "ctee/common/logger.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace ctee {

Logger& Logger::instance() {
    static Logger logger;
    return logger;
}

Logger::Logger() {
    worker_ = std::thread([this]() { worker_loop(); });
}

Logger::Logger(Level level, bool console_enabled)
    : level_(level),
      console_enabled_(console_enabled) {
    worker_ = std::thread([this]() { worker_loop(); });
}

Logger::~Logger() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_one();
    if (worker_.joinable()) {
        worker_.join();
    }
    close_file();
}

// ── Backward-compatible log methods (no context) ───────────────────────

void Logger::debug(const std::string& message) {
    log(Level::Debug, message, {});
}

void Logger::info(const std::string& message) {
    log(Level::Info, message, {});
}

void Logger::warn(const std::string& message) {
    log(Level::Warn, message, {});
}

void Logger::error(const std::string& message) {
    log(Level::Error, message, {});
}

// ── Context-aware log methods ──────────────────────────────────────────

void Logger::debug(const std::string& message, const LogContext& ctx) {
    log(Level::Debug, message, ctx);
}

void Logger::info(const std::string& message, const LogContext& ctx) {
    log(Level::Info, message, ctx);
}

void Logger::warn(const std::string& message, const LogContext& ctx) {
    log(Level::Warn, message, ctx);
}

void Logger::error(const std::string& message, const LogContext& ctx) {
    log(Level::Error, message, ctx);
}

// ── Configuration ──────────────────────────────────────────────────────

void Logger::set_level(Level l) {
    level_.store(l, std::memory_order_relaxed);
}

Logger::Level Logger::level() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

void Logger::set_console_enabled(bool enabled) {
    console_enabled_.store(enabled, std::memory_order_relaxed);
}

bool Logger::console_enabled() const noexcept {
    return console_enabled_.load(std::memory_order_relaxed);
}

void Logger::set_file_output(const std::string& filepath) {
    std::lock_guard lock(file_mutex_);
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
    file_stream_.open(filepath, std::ios::out | std::ios::trunc);
}

void Logger::close_file() {
    std::lock_guard lock(file_mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
        file_stream_.close();
    }
}

// ── Synchronization ────────────────────────────────────────────────────

void Logger::flush() {
    auto promise = std::make_shared<std::promise<void>>();
    auto future = promise->get_future();

    {
        std::lock_guard lock(mutex_);
        flush_promises_.push_back(promise);
    }
    cv_.notify_one();
    future.get();
}

// ── Core logging ───────────────────────────────────────────────────────

void Logger::log(Level l, const std::string& message, const LogContext& ctx) {
    if (l < level_.load(std::memory_order_relaxed)) {
        return;
    }

    LogEntry entry;
    entry.level = l;
    entry.message = message;
    entry.context = ctx;
    entry.timestamp = std::chrono::system_clock::now();

    {
        std::lock_guard lock(mutex_);
        queue_.push_back(std::move(entry));
    }
    cv_.notify_one();
}

// ── Background worker ──────────────────────────────────────────────────

void Logger::worker_loop() {
    while (true) {
        std::deque<LogEntry> batch;
        std::vector<std::shared_ptr<std::promise<void>>> flush_batch;

        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [this] {
                return !queue_.empty()
                    || !flush_promises_.empty()
                    || stop_.load(std::memory_order_acquire);
            });

            batch.swap(queue_);
            flush_batch.swap(flush_promises_);
        }

        for (const auto& entry : batch) {
            output_line(format_entry(entry));
        }

        for (auto& p : flush_batch) {
            p->set_value();
        }

        if (stop_.load(std::memory_order_acquire) && queue_.empty()) {
            break;
        }
    }
}

// ── Output ─────────────────────────────────────────────────────────────

void Logger::output_line(const std::string& line) {
    if (console_enabled_.load(std::memory_order_relaxed)) {
        std::cout << line << '\n';
    }

    std::lock_guard lock(file_mutex_);
    if (file_stream_.is_open()) {
        file_stream_ << line << '\n';
        file_stream_.flush();
    }
}

// ── Formatting ─────────────────────────────────────────────────────────

std::string Logger::format_entry(const LogEntry& entry) const {
    auto time_t_val = std::chrono::system_clock::to_time_t(entry.timestamp);
    auto since_epoch = entry.timestamp.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(since_epoch).count() % 1000;

    std::tm tm_buf;
#ifdef _WIN32
    localtime_s(&tm_buf, &time_t_val);
#else
    localtime_r(&time_t_val, &tm_buf);
#endif

    char ts_buf[32];
    std::strftime(ts_buf, sizeof(ts_buf), "%Y-%m-%d %H:%M:%S", &tm_buf);

    const char* level_str = "INFO";
    switch (entry.level) {
        case Level::Debug: level_str = "DEBUG"; break;
        case Level::Info:  level_str = "INFO";  break;
        case Level::Warn:  level_str = "WARN";  break;
        case Level::Error: level_str = "ERROR"; break;
    }

    std::ostringstream oss;
    oss << '[' << ts_buf << '.'
        << std::setfill('0') << std::setw(3) << millis << "]"
        << " [" << level_str << ']';

    if (entry.context.worker_id > 0) {
        oss << " [Worker:" << entry.context.worker_id << ']';
    }
    if (!entry.context.task_id.empty()) {
        oss << " [Task:" << entry.context.task_id << ']';
    }

    oss << ' ' << entry.message;
    return oss.str();
}

}  // namespace ctee
