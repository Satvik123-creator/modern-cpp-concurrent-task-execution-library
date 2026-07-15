#include <gtest/gtest.h>

#include <string>

#include "ctee/common/thread_naming.h"

TEST(ThreadNamingTest, SetThreadNameDoesNotCrash) {
    ctee::common::set_thread_name("test-worker");
}

TEST(ThreadNamingTest, SetThreadNameLongString) {
    ctee::common::set_thread_name("this-is-a-very-long-thread-name-that-exceeds-fifteen-chars");
}

TEST(ThreadNamingTest, SetThreadNameEmpty) {
    ctee::common::set_thread_name("");
}
