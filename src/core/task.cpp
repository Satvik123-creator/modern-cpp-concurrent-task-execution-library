#include "ctee/core/task.h"

#include <random>

using namespace std;
namespace ch = std::chrono;

namespace ctee {

namespace {

string generate_uuid() {
    // thread_local: mt19937 is NOT thread-safe.  Each thread gets
    // its own engine, seeded from random_device (once per thread).
    thread_local random_device rd;
    thread_local mt19937 gen(rd());
    thread_local uniform_int_distribution<uint32_t> dis(0, 0xFFFFFFFF);

    uint32_t d[4] = {dis(gen), dis(gen), dis(gen), dis(gen)};
    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
             d[0], (d[1] >> 16) & 0xFFFF, d[1] & 0xFFFF,
             (d[2] >> 16) & 0xFFFF, d[2] & 0xFFFF, d[3]);
    return buf;
}

}  // namespace

const char* to_string(TaskPriority priority) {
    switch (priority) {
        case TaskPriority::Low:      return "low";
        case TaskPriority::Normal:   return "normal";
        case TaskPriority::High:     return "high";
        case TaskPriority::Critical: return "critical";
    }
    return "unknown";
}

const char* to_string(TaskStatus status) {
    switch (status) {
        case TaskStatus::Pending:   return "pending";
        case TaskStatus::Queued:    return "queued";
        case TaskStatus::Running:   return "running";
        case TaskStatus::Completed: return "completed";
        case TaskStatus::Failed:    return "failed";
        case TaskStatus::Cancelled: return "cancelled";
        case TaskStatus::TimedOut:  return "timed_out";
    }
    return "unknown";
}

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

const string& Task::id() const noexcept { return id_; }
const string& Task::name() const noexcept { return name_; }
TaskPriority Task::priority() const noexcept { return priority_; }
TaskStatus Task::status() const noexcept { return status_; }
const Task::FuncType& Task::func() const noexcept { return func_; }
ch::steady_clock::time_point Task::created_at() const noexcept { return created_at_; }
ch::steady_clock::time_point Task::scheduled_at() const noexcept { return scheduled_at_; }
bool Task::has_schedule() const noexcept { return has_schedule_; }
bool Task::is_recurring() const noexcept { return repeat_interval_.count() > 0; }
ch::milliseconds Task::repeat_interval() const noexcept { return repeat_interval_; }
size_t Task::repeat_count() const noexcept { return repeat_count_; }
size_t Task::max_repeats() const noexcept { return max_repeats_; }
const TaskResult& Task::result() const noexcept { return result_; }

void Task::set_status(TaskStatus status) { status_ = status; }
void Task::set_scheduled_at(ch::steady_clock::time_point tp) {
    scheduled_at_ = tp;
    has_schedule_ = true;
}
void Task::set_recurring(ch::milliseconds interval, size_t max) {
    repeat_interval_ = interval;
    max_repeats_ = max;
}
void Task::increment_repeat_count() { ++repeat_count_; }
void Task::set_result(TaskResult result) { result_ = move(result); }

size_t Task::retry_count() const noexcept { return retry_count_; }
size_t Task::max_retries() const noexcept { return max_retries_; }
ch::milliseconds Task::retry_delay() const noexcept { return retry_delay_; }
const vector<string>& Task::depends_on() const noexcept { return depends_on_; }
ch::milliseconds Task::timeout_ms() const noexcept { return timeout_ms_; }

void Task::set_max_retries(size_t n) { max_retries_ = n; }
void Task::set_retry_delay(ch::milliseconds delay) { retry_delay_ = delay; }
void Task::set_depends_on(vector<string> deps) { depends_on_ = move(deps); }
void Task::set_timeout(ch::milliseconds ms) { timeout_ms_ = ms; }
void Task::increment_retry_count() { ++retry_count_; }
void Task::set_id(const string& id) { id_ = id; }

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

bool Task::operator<(const Task& other) const noexcept {
    // Three-level comparator for std::push_heap / std::pop_heap (max-heap).
    //
    // A < B means "A has lower effective priority than B", so B sits
    // closer to the root of the max-heap and is popped first.
    //
    // Level 1 – Priority (descending):
    //   Higher enum value → higher priority → goes above.
    //   Critical (3) > High (2) > Normal (1) > Low (0).
    //
    // Level 2 – Timestamp (ascending → FIFO within same priority):
    //   Earlier created_at → goes above → executed first.
    //   This guarantees FIFO ordering among tasks of equal priority.
    //
    // Level 3 – Task ID (lexicographic, for determinism):
    //   Two tasks can share the same priority AND the same timestamp
    //   (steady_clock resolution).  The UUID tiebreaker ensures a
    //   strict weak ordering with no ambiguous comparisons, which is
    //   required by the C++ standard for std::heap operations.

    if (priority_ != other.priority_) {
        return priority_ < other.priority_;
    }
    if (created_at_ != other.created_at_) {
        return created_at_ > other.created_at_;
    }
    return id_ > other.id_;
}

}  // namespace ctee
