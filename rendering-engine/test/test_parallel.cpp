// SPDX-License-Identifier: Apache-2.0

// Tests for forEachIndex(), the engine's one fork-join primitive.
//
// Threading faults depend on timing, so passing here is weak evidence on its
// own. These tests are also run under ThreadSanitizer (the macos-tsan
// preset), which reports a race whether or not the run's timing exposed it.

#include "rainbohz/rendering_engine/parallel.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using rainbohz::rendering_engine::forEachIndex;
using rainbohz::rendering_engine::kMaxThreads;
using rainbohz::rendering_engine::resolveThreadCount;

/// An exception that names the index that threw it.
class IndexError : public std::runtime_error {
public:
    explicit IndexError(std::size_t index)
        : std::runtime_error{"index " + std::to_string(index)}, index_{index} {}
    std::size_t index() const noexcept { return index_; }

private:
    std::size_t index_;
};

TEST(ParallelTest, EveryIndexExactlyOnce) {
    for (const std::size_t count : {0U, 1U, 2U, 7U, 1000U}) {
        for (const std::size_t threads : {1U, 2U, 3U, 8U, 64U}) {
            std::vector<std::atomic<int>> calls(count);
            forEachIndex(threads, count, [&](std::size_t i) { calls[i].fetch_add(1); });
            for (std::size_t i = 0; i < count; ++i) {
                EXPECT_EQ(calls[i].load(), 1)
                    << "count " << count << ", threads " << threads << ", index " << i;
            }
        }
    }
}

TEST(ParallelTest, ZeroThreadsCountsAsOne) {
    std::vector<std::size_t> order;
    forEachIndex(0, 4, [&](std::size_t i) { order.push_back(i); });
    EXPECT_EQ(order, (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(ParallelTest, SerialPathRunsOnCallerInOrder) {
    const std::thread::id caller = std::this_thread::get_id();
    std::vector<std::size_t> order;
    bool allOnCaller = true;
    forEachIndex(1, 50, [&](std::size_t i) {
        order.push_back(i);
        allOnCaller = allOnCaller && std::this_thread::get_id() == caller;
    });
    EXPECT_TRUE(allOnCaller);
    ASSERT_EQ(order.size(), 50U);
    for (std::size_t i = 0; i < order.size(); ++i) {
        EXPECT_EQ(order[i], i);
    }
}

TEST(ParallelTest, ParallelPathUsesSeveralThreads) {
    std::mutex mutex;
    std::set<std::thread::id> seen;
    forEachIndex(4, 64, [&](std::size_t /*i*/) {
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
        const std::scoped_lock lock{mutex};
        seen.insert(std::this_thread::get_id());
    });
    EXPECT_GT(seen.size(), 1U);
    EXPECT_LE(seen.size(), 4U);
}

TEST(ParallelTest, ResultsVisibleAfterReturn) {
    // Plain (non-atomic) writes to distinct elements: correct without any
    // synchronisation but the join, which ThreadSanitizer checks.
    constexpr std::size_t kCount = 10'000;
    std::vector<double> out(kCount, 0.0);
    forEachIndex(8, kCount, [&](std::size_t i) { out[i] = static_cast<double>(i) * 0.5; });
    for (std::size_t i = 0; i < kCount; ++i) {
        ASSERT_EQ(out[i], static_cast<double>(i) * 0.5) << i;
    }
}

TEST(ParallelTest, LowestFailureRethrown) {
    for (int repeat = 0; repeat < 200; ++repeat) {
        try {
            forEachIndex(8, 1000, [](std::size_t i) {
                if (i == 5 || i == 900) {
                    throw IndexError{i};
                }
            });
            FAIL() << "nothing thrown";
        } catch (const IndexError& e) {
            ASSERT_EQ(e.index(), 5U) << "repeat " << repeat;
        }
    }
}

TEST(ParallelTest, SerialPathRethrowsFirstFailure) {
    std::vector<std::size_t> ran;
    try {
        forEachIndex(1, 10, [&](std::size_t i) {
            ran.push_back(i);
            if (i == 3 || i == 6) {
                throw IndexError{i};
            }
        });
        FAIL() << "nothing thrown";
    } catch (const IndexError& e) {
        EXPECT_EQ(e.index(), 3U);
    }
    EXPECT_EQ(ran, (std::vector<std::size_t>{0, 1, 2, 3}));
}

TEST(ParallelTest, EveryIndexBelowFailureRan) {
    constexpr std::size_t kCount = 500;
    constexpr std::size_t kFailing = 300;
    for (int repeat = 0; repeat < 50; ++repeat) {
        std::vector<std::atomic<bool>> ran(kCount);
        EXPECT_THROW(forEachIndex(8, kCount,
                         [&](std::size_t i) {
                             ran[i].store(true);
                             if (i == kFailing) {
                                 throw IndexError{i};
                             }
                         }),
            IndexError);
        for (std::size_t i = 0; i <= kFailing; ++i) {
            ASSERT_TRUE(ran[i].load()) << "index " << i << ", repeat " << repeat;
        }
    }
}

TEST(ParallelTest, NestedCallThrowsOnSerialPath) {
    bool refused = false;
    forEachIndex(1, 1, [&](std::size_t /*i*/) {
        try {
            forEachIndex(1, 1, [](std::size_t /*j*/) {});
        } catch (const std::logic_error&) {
            refused = true;
        }
    });
    EXPECT_TRUE(refused);
}

TEST(ParallelTest, NestedCallThrowsOnParallelPath) {
    std::atomic<int> refused{0};
    forEachIndex(4, 16, [&](std::size_t /*i*/) {
        try {
            forEachIndex(4, 4, [](std::size_t /*j*/) {});
        } catch (const std::logic_error&) {
            refused.fetch_add(1);
        }
    });
    EXPECT_EQ(refused.load(), 16);
}

TEST(ParallelTest, CallableAgainAfterFailureAndNesting) {
    EXPECT_THROW(forEachIndex(4, 8, [](std::size_t i) { throw IndexError{i}; }), IndexError);
    std::atomic<int> calls{0};
    forEachIndex(4, 8, [&](std::size_t /*i*/) { calls.fetch_add(1); });
    EXPECT_EQ(calls.load(), 8);
}

TEST(ParallelTest, ResolveThreadCount) {
    EXPECT_GE(resolveThreadCount(0), 1U);
    EXPECT_LE(resolveThreadCount(0), kMaxThreads);
    EXPECT_EQ(resolveThreadCount(1), 1U);
    EXPECT_EQ(resolveThreadCount(5), 5U);
    EXPECT_EQ(resolveThreadCount(10'000), kMaxThreads);
}

TEST(ParallelTest, StressSumMatchesSerial) {
    constexpr std::size_t kCount = 257;
    std::vector<double> serial(kCount);
    forEachIndex(1, kCount, [&](std::size_t i) { serial[i] = 1.0 / static_cast<double>(i + 1); });
    for (int repeat = 0; repeat < 2000; ++repeat) {
        std::vector<double> parallel(kCount);
        forEachIndex(1 + static_cast<std::size_t>(repeat % 9), kCount,
            [&](std::size_t i) { parallel[i] = 1.0 / static_cast<double>(i + 1); });
        ASSERT_EQ(parallel, serial) << "repeat " << repeat;
    }
}

} // namespace
