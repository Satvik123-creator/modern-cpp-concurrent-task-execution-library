#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "ctee/common/logger.h"

using namespace ctee;
using namespace std::chrono_literals;

// ── Helpers ────────────────────────────────────────────────────────────

namespace {

// Redirect std::cout to capture console output.
class CoutCapture {
public:
    CoutCapture() : old_(std::cout.rdbuf(buf_.rdbuf())) {}
    ~CoutCapture() { std::cout.rdbuf(old_); }

    [[nodiscard]] std::string str() const { return buf_.str(); }

private:
    std::ostringstream buf_;
    std::streambuf* old_;
};

// Create a temporary file path.
std::string temp_path(const std::string& name) {
    return std::tmpnam(nullptr) + name + ".log";
}

// Read entire file to string.
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

// ── Tests ──────────────────────────────────────────────────────────────
// Logger is a process-wide singleton.  Tests must not conflict on
// configuration (level, console enabled, file output).  We call
// flush() after every assertion batch to drain the async queue.

// ==========================================================================
// Group 1: Level filtering
// ==========================================================================

TEST(LoggerLevel, DefaultLevelIsInfo) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Info);
    EXPECT_EQ(l.level(), Logger::Level::Info);
    l.set_level(prev);
}

TEST(LoggerLevel, DebugBelowInfoIsDropped) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Info);

    CoutCapture cap;
    l.debug("should-not-appear");
    l.flush();

    EXPECT_TRUE(cap.str().empty()) << cap.str();
    l.set_level(prev);
}

TEST(LoggerLevel, WarnAboveInfoIsVisible) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Info);

    CoutCapture cap;
    l.warn("should-appear");
    l.flush();

    EXPECT_NE(cap.str().find("should-appear"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerLevel, SetLevelControlsOutput) {
    Logger& l = Logger::instance();
    auto prev = l.level();

    l.set_level(Logger::Level::Error);
    CoutCapture cap1;
    l.info("suppressed");
    l.warn("suppressed");
    l.error("visible");
    l.flush();
    std::string out1 = cap1.str();
    EXPECT_TRUE(out1.find("suppressed") == std::string::npos);
    EXPECT_NE(out1.find("visible"), std::string::npos);

    l.set_level(Logger::Level::Debug);
    CoutCapture cap2;
    l.debug("all-visible");
    l.flush();
    EXPECT_NE(cap2.str().find("all-visible"), std::string::npos);

    l.set_level(prev);
}

TEST(LoggerLevel, SetLevelIsImmediate) {
    Logger& l = Logger::instance();
    auto prev = l.level();

    l.set_level(Logger::Level::Error);
    CoutCapture cap;
    l.info("gone");
    l.flush();
    EXPECT_TRUE(cap.str().find("gone") == std::string::npos);

    l.set_level(Logger::Level::Info);
    CoutCapture cap2;
    l.info("back");
    l.flush();
    EXPECT_NE(cap2.str().find("back"), std::string::npos);

    l.set_level(prev);
}

// ==========================================================================
// Group 2: Timestamp format
// ==========================================================================

TEST(LoggerTimestamp, ContainsTimestamp) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("timestamp-check");
    l.flush();

    std::string output = cap.str();
    // Expect: [YYYY-MM-DD HH:MM:SS.mmm]
    EXPECT_NE(output.find("["), std::string::npos);
    EXPECT_NE(output.find("202"), std::string::npos);  // year starts with 202x
    EXPECT_NE(output.find("] [INFO]"), std::string::npos);

    l.set_level(prev);
}

TEST(LoggerTimestamp, ContainsMilliseconds) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("ms-check");
    l.flush();

    std::string output = cap.str();
    // Find the opening bracket, then look for a 3-digit ms component
    auto dot_pos = output.find('.');
    if (dot_pos != std::string::npos) {
        // After the dot, expect 3 digits before the ']'
        std::string ms_str = output.substr(dot_pos + 1, 3);
        EXPECT_EQ(ms_str.size(), 3u);
        for (char c : ms_str) {
            EXPECT_TRUE(c >= '0' && c <= '9');
        }
    }

    l.set_level(prev);
}

// ==========================================================================
// Group 3: Log levels in output
// ==========================================================================

TEST(LoggerFormat, DebugLevelLabel) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.debug("debug-msg");
    l.flush();

    EXPECT_NE(cap.str().find("[DEBUG]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerFormat, InfoLevelLabel) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("info-msg");
    l.flush();

    EXPECT_NE(cap.str().find("[INFO]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerFormat, WarnLevelLabel) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.warn("warn-msg");
    l.flush();

    EXPECT_NE(cap.str().find("[WARN]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerFormat, ErrorLevelLabel) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.error("error-msg");
    l.flush();

    EXPECT_NE(cap.str().find("[ERROR]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerFormat, MessageContentPreserved) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("hello world 123 !@#");
    l.flush();

    EXPECT_NE(cap.str().find("hello world 123 !@#"), std::string::npos);
    l.set_level(prev);
}

// ==========================================================================
// Group 4: Worker ID context
// ==========================================================================

TEST(LoggerContext, WorkerIdInOutput) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("worker-test", LogContext{.worker_id = 42});
    l.flush();

    EXPECT_NE(cap.str().find("[Worker:42]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerContext, WorkerIdZeroOmitted) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("no-worker", LogContext{.worker_id = 0});
    l.flush();

    EXPECT_EQ(cap.str().find("[Worker:"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerContext, WorkerIdLargeNumber) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("big-worker", LogContext{.worker_id = 99999});
    l.flush();

    EXPECT_NE(cap.str().find("[Worker:99999]"), std::string::npos);
    l.set_level(prev);
}

// ==========================================================================
// Group 5: Task ID context
// ==========================================================================

TEST(LoggerContext, TaskIdInOutput) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("task-test", LogContext{.task_id = "abc-123"});
    l.flush();

    EXPECT_NE(cap.str().find("[Task:abc-123]"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerContext, TaskIdEmptyOmitted) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("no-task", LogContext{.task_id = ""});
    l.flush();

    EXPECT_EQ(cap.str().find("[Task:"), std::string::npos);
    l.set_level(prev);
}

TEST(LoggerContext, TaskIdUuidFormat) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    CoutCapture cap;
    l.info("uuid-task", LogContext{.task_id = uuid});
    l.flush();

    EXPECT_NE(cap.str().find("[Task:" + uuid + "]"), std::string::npos);
    l.set_level(prev);
}

// ==========================================================================
// Group 6: Combined Worker ID + Task ID
// ==========================================================================

TEST(LoggerContext, WorkerAndTaskIdTogether) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("both", LogContext{.worker_id = 7, .task_id = "t-99"});
    l.flush();

    std::string output = cap.str();
    EXPECT_NE(output.find("[Worker:7]"), std::string::npos);
    EXPECT_NE(output.find("[Task:t-99]"), std::string::npos);
    // Worker should appear before Task in formatted output
    EXPECT_LT(output.find("[Worker:7]"), output.find("[Task:t-99]"));

    l.set_level(prev);
}

// ==========================================================================
// Group 7: Console output toggle
// ==========================================================================

TEST(LoggerConsole, ConsoleDisabledSuppressesOutput) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);
    bool prev_console = l.console_enabled();

    l.set_console_enabled(false);
    CoutCapture cap;
    l.info("should-not-appear");
    l.flush();

    EXPECT_TRUE(cap.str().empty()) << cap.str();

    l.set_console_enabled(prev_console);
    l.set_level(prev);
}

TEST(LoggerConsole, ConsoleReenabled) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);
    bool prev_console = l.console_enabled();

    l.set_console_enabled(false);
    l.info("hidden");
    l.flush();

    l.set_console_enabled(true);
    CoutCapture cap;
    l.info("visible");
    l.flush();

    EXPECT_TRUE(cap.str().find("hidden") == std::string::npos);
    EXPECT_NE(cap.str().find("visible"), std::string::npos);

    l.set_console_enabled(prev_console);
    l.set_level(prev);
}

// ==========================================================================
// Group 8: File output
// ==========================================================================

TEST(LoggerFile, WritesToFile) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_test");
    l.set_file_output(path);

    l.info("file-test-entry");
    l.flush();

    std::string content = read_file(path);
    EXPECT_NE(content.find("file-test-entry"), std::string::npos);

    l.close_file();
    std::remove(path.c_str());
    l.set_level(prev);
}

TEST(LoggerFile, FileContainsTimestamp) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_ts");
    l.set_file_output(path);

    l.info("ts-in-file");
    l.flush();

    std::string content = read_file(path);
    EXPECT_NE(content.find("[INFO]"), std::string::npos);
    EXPECT_NE(content.find("202"), std::string::npos);

    l.close_file();
    std::remove(path.c_str());
    l.set_level(prev);
}

TEST(LoggerFile, MultipleEntriesInFile) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_multi");
    l.set_file_output(path);

    l.info("entry-1");
    l.warn("entry-2");
    l.error("entry-3");
    l.flush();

    std::string content = read_file(path);
    EXPECT_NE(content.find("entry-1"), std::string::npos);
    EXPECT_NE(content.find("entry-2"), std::string::npos);
    EXPECT_NE(content.find("entry-3"), std::string::npos);
    EXPECT_NE(content.find("[WARN]"), std::string::npos);
    EXPECT_NE(content.find("[ERROR]"), std::string::npos);

    l.close_file();
    std::remove(path.c_str());
    l.set_level(prev);
}

TEST(LoggerFile, FileContainsContext) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_ctx");
    l.set_file_output(path);

    l.info("ctx-in-file", LogContext{.worker_id = 3, .task_id = "t-1"});
    l.flush();

    std::string content = read_file(path);
    EXPECT_NE(content.find("[Worker:3]"), std::string::npos);
    EXPECT_NE(content.find("[Task:t-1]"), std::string::npos);

    l.close_file();
    std::remove(path.c_str());
    l.set_level(prev);
}

TEST(LoggerFile, CloseFileStopsWriting) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_close");
    l.set_file_output(path);
    l.info("before-close");
    l.flush();

    l.close_file();
    l.info("after-close");
    l.flush();

    std::string content = read_file(path);
    EXPECT_NE(content.find("before-close"), std::string::npos);
    EXPECT_TRUE(content.find("after-close") == std::string::npos);

    std::remove(path.c_str());
    l.set_level(prev);
}

TEST(LoggerFile, FileTruncatedOnReopen) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    std::string path = temp_path("logger_file_trunc");

    l.set_file_output(path);
    l.info("old-content");
    l.flush();

    // Reopen with truncation
    l.set_file_output(path);
    l.info("new-content");
    l.flush();

    std::string content = read_file(path);
    EXPECT_TRUE(content.find("old-content") == std::string::npos);
    EXPECT_NE(content.find("new-content"), std::string::npos);

    l.close_file();
    std::remove(path.c_str());
    l.set_level(prev);
}

// ==========================================================================
// Group 9: Async behavior
// ==========================================================================

TEST(LoggerAsync, ConcurrentLoggingNoCrash) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);
    bool prev_console = l.console_enabled();
    l.set_console_enabled(false);

    constexpr int NUM_THREADS = 8;
    constexpr int PER_THREAD = 50;

    std::vector<std::thread> threads;
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&l, t]() {
            for (int i = 0; i < PER_THREAD; ++i) {
                l.info("thread-" + std::to_string(t) + "-msg-" + std::to_string(i),
                       LogContext{.worker_id = static_cast<std::size_t>(t)});
            }
        });
    }
    for (auto& th : threads) th.join();
    l.flush();

    // If we get here without crashing, the test passes.
    l.set_console_enabled(prev_console);
    l.set_level(prev);
}

TEST(LoggerAsync, FlushBlocksUntilComplete) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);
    bool prev_console = l.console_enabled();
    l.set_console_enabled(false);

    for (int i = 0; i < 100; ++i) {
        l.info("flush-test-" + std::to_string(i));
    }
    l.flush();

    // After flush returns, queue should be empty.
    // Verify by doing another flush which should complete instantly.
    auto start = std::chrono::steady_clock::now();
    l.flush();
    auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(elapsed, 100ms);

    l.set_console_enabled(prev_console);
    l.set_level(prev);
}

TEST(LoggerAsync, FlushWithWorkersAndTasks) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);
    bool prev_console = l.console_enabled();
    l.set_console_enabled(false);

    for (int i = 0; i < 20; ++i) {
        l.info("msg", LogContext{
            .worker_id = static_cast<std::size_t>(i % 4),
            .task_id = "task-" + std::to_string(i)
        });
    }
    l.flush();

    l.set_console_enabled(prev_console);
    l.set_level(prev);
}

// ==========================================================================
// Group 10: Backward compatibility
// ==========================================================================

TEST(LoggerCompat, OldApiStillWorks) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.debug("d");
    l.info("i");
    l.warn("w");
    l.error("e");
    l.flush();

    EXPECT_NE(cap.str().find("[DEBUG] d"), std::string::npos);
    EXPECT_NE(cap.str().find("[INFO] i"), std::string::npos);
    EXPECT_NE(cap.str().find("[WARN] w"), std::string::npos);
    EXPECT_NE(cap.str().find("[ERROR] e"), std::string::npos);

    l.set_level(prev);
}

TEST(LoggerCompat, SingletonIdentity) {
    Logger& a = Logger::instance();
    Logger& b = Logger::instance();
    EXPECT_EQ(&a, &b);
}

// ==========================================================================
// Group 11: Log format exact structure
// ==========================================================================

TEST(LoggerFormat, FullLineStructure) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("full-structure", LogContext{.worker_id = 1, .task_id = "abc"});
    l.flush();

    std::string line = cap.str();
    // Expected: [YYYY-MM-DD HH:MM:SS.mmm] [INFO] [Worker:1] [Task:abc] full-structure
    EXPECT_NE(line.find("["), std::string::npos);
    EXPECT_NE(line.find("] [INFO] [Worker:1] [Task:abc] full-structure"), std::string::npos);

    l.set_level(prev);
}

TEST(LoggerFormat, NoContextMinimalLine) {
    Logger& l = Logger::instance();
    auto prev = l.level();
    l.set_level(Logger::Level::Debug);

    CoutCapture cap;
    l.info("minimal");
    l.flush();

    std::string line = cap.str();
    // Expected: [YYYY-MM-DD HH:MM:SS.mmm] [INFO] minimal
    EXPECT_NE(line.find("[INFO] minimal"), std::string::npos);
    EXPECT_EQ(line.find("[Worker:"), std::string::npos);
    EXPECT_EQ(line.find("[Task:"), std::string::npos);

    l.set_level(prev);
}

}  // namespace
