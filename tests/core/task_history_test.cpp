#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "ctee/core/task_history.h"
#include "ctee/core/task_result.h"

namespace ctee {
namespace {

// ==========================================================================
// TaskHistory tests
// ==========================================================================

TEST(TaskHistoryTest, RecordAndRetrieve) {
    TaskHistory history(100);

    TaskRecord rec;
    rec.task_id = "task-1";
    rec.task_name = "test";
    rec.status = TaskStatus::Completed;
    rec.submitted_at = std::chrono::steady_clock::now();
    rec.completed_at = std::chrono::steady_clock::now();
    rec.execution_time = std::chrono::milliseconds(10);
    history.record(rec);

    auto recent = history.recent(10);
    ASSERT_EQ(recent.size(), 1u);
    EXPECT_EQ(recent[0].task_id, "task-1");
    EXPECT_EQ(history.count(), 1u);
}

TEST(TaskHistoryTest, MaxSizeRespected) {
    TaskHistory history(5);

    for (int i = 0; i < 10; i++) {
        TaskRecord rec;
        rec.task_id = "task-" + std::to_string(i);
        rec.status = TaskStatus::Completed;
        rec.completed_at = std::chrono::steady_clock::now();
        history.record(rec);
    }

    EXPECT_EQ(history.count(), 5u);
    auto recent = history.recent(100);
    EXPECT_EQ(recent.size(), 5u);
    EXPECT_EQ(recent[0].task_id, "task-5");
}

TEST(TaskHistoryTest, RecordSubmitAndUpdate) {
    TaskHistory history(100);

    history.record_submit("t1", "my_task", std::chrono::steady_clock::now());
    EXPECT_EQ(history.count(), 1u);

    auto recent = history.recent(10);
    EXPECT_EQ(recent[0].status, TaskStatus::Queued);

    history.record_complete("t1", TaskStatus::Completed,
                           std::chrono::milliseconds(5));
    recent = history.recent(10);
    EXPECT_EQ(recent[0].status, TaskStatus::Completed);
    EXPECT_EQ(recent[0].execution_time.count(), 5);
}

TEST(TaskHistoryTest, ClearResets) {
    TaskHistory history(100);
    history.record_submit("t1", "a", std::chrono::steady_clock::now());
    history.record_submit("t2", "b", std::chrono::steady_clock::now());
    EXPECT_EQ(history.count(), 2u);

    history.clear();
    EXPECT_EQ(history.count(), 0u);
}

}  // namespace
}  // namespace ctee
