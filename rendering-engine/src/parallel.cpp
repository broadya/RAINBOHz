// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/parallel.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

/// @file
/// @brief forEachIndex(): fork, claim indices from an atomic counter, join.
///
/// No locks and no condition variables: a thread claims the next index with
/// one fetch_add, and the caller waits only in join(). Nothing waits on
/// anything else, so nothing can deadlock.
///
/// Memory ordering: the counter only has to hand out each index once, which
/// any atomic read-modify-write does whatever its order, so it is relaxed. A
/// thread's completion synchronizes with the return of its join(), so every
/// write a call made happens before the caller's code after the join.
///
/// @see parallel.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Whether this thread is inside a call of a task. Set on the caller's
/// thread on the serial path too, so that a nesting fault shows up in a
/// single-threaded run and does not wait for load.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables): per thread, by design.
thread_local bool insideTask = false;

/// Marks this thread as inside a task for its lifetime.
class TaskScope {
public:
    TaskScope() noexcept { insideTask = true; }
    ~TaskScope() { insideTask = false; }
    TaskScope(const TaskScope&) = delete;
    TaskScope& operator=(const TaskScope&) = delete;
    TaskScope(TaskScope&&) = delete;
    TaskScope& operator=(TaskScope&&) = delete;
};

/// What the threads of one forEachIndex() call share.
struct Shared {
    /// The next index no thread has claimed.
    std::atomic<std::size_t> next{0};

    /// The lowest index whose call threw, or the count when none has.
    std::atomic<std::size_t> lowestFailure;

    /// One slot per index, written only by the thread that claimed it.
    std::vector<std::exception_ptr> errors;

    /// The calls.
    const std::function<void(std::size_t)>* task = nullptr;
};

/// Threads that are joined when this goes out of scope, however it does: the
/// role std::jthread would play, which Apple's libc++ offers only as an
/// experimental feature. Never detaches.
class JoinedThreads {
public:
    JoinedThreads() = default;
    ~JoinedThreads() {
        for (std::thread& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }
    JoinedThreads(const JoinedThreads&) = delete;
    JoinedThreads& operator=(const JoinedThreads&) = delete;
    JoinedThreads(JoinedThreads&&) = delete;
    JoinedThreads& operator=(JoinedThreads&&) = delete;

    /// Reserves room for `count` threads, so that starting them cannot
    /// reallocate.
    ///
    /// @param count  How many.
    void reserve(std::size_t count) { threads_.reserve(count); }

    /// Starts a thread running `body`.
    ///
    /// @param body  What the thread runs.
    /// @throws std::system_error  If the thread cannot be started.
    template <typename Body>
    void start(Body&& body) {
        threads_.emplace_back(std::forward<Body>(body));
    }

private:
    std::vector<std::thread> threads_;
};

/// Lowers `target` to `value` if `value` is lower.
///
/// @param[in,out] target  The atomic to lower.
/// @param value           The candidate.
void lowerTo(std::atomic<std::size_t>& target, std::size_t value) noexcept {
    std::size_t current = target.load(std::memory_order_relaxed);
    while (value < current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}

/// Claims and runs indices until none is left, or every one left is above a
/// failure. Never throws: a call's exception goes to its slot.
///
/// @param[in,out] shared  The call's shared state.
void claimLoop(Shared& shared) noexcept {
    const TaskScope scope;
    const std::size_t count = shared.errors.size();
    for (;;) {
        const std::size_t i = shared.next.fetch_add(1, std::memory_order_relaxed);
        // Reading lowestFailure relaxed only risks running a call whose
        // result is then ignored; the final value is read after the join.
        if (i >= count || i > shared.lowestFailure.load(std::memory_order_relaxed)) {
            return;
        }
        try {
            (*shared.task)(i);
        } catch (...) {
            shared.errors[i] = std::current_exception();
            lowerTo(shared.lowestFailure, i);
        }
    }
}

} // namespace

std::size_t resolveThreadCount(std::size_t requested) noexcept {
    std::size_t threads = requested;
    if (threads == 0) {
        threads = std::thread::hardware_concurrency();
    }
    return std::clamp<std::size_t>(threads, 1, kMaxThreads);
}

void forEachIndex(
    std::size_t threads, std::size_t count, const std::function<void(std::size_t)>& task) {
    if (insideTask) {
        throw std::logic_error{"forEachIndex() called from inside a task"};
    }
    threads = std::clamp<std::size_t>(threads, 1, kMaxThreads);
    if (threads == 1 || count <= 1) {
        const TaskScope scope;
        for (std::size_t i = 0; i < count; ++i) {
            task(i);
        }
        return;
    }

    // Declared before the threads, so that it outlives them: the threads
    // are joined first, even when this function unwinds.
    Shared shared;
    shared.lowestFailure.store(count, std::memory_order_relaxed);
    shared.errors.resize(count);
    shared.task = &task;

    {
        const std::size_t workers = std::min(threads, count) - 1;
        JoinedThreads started;
        started.reserve(workers);
        try {
            for (std::size_t t = 0; t < workers; ++t) {
                started.start([&shared] { claimLoop(shared); });
            }
            // NOLINTNEXTLINE(bugprone-empty-catch): handled by carrying on.
        } catch (const std::system_error&) {
            // Fewer threads than asked for. The ones started and the caller's
            // still claim every index, so only the speed changes.
        }
        claimLoop(shared);
    } // Every thread joined here.

    const std::size_t failed = shared.lowestFailure.load(std::memory_order_relaxed);
    if (failed < count) {
        std::rethrow_exception(shared.errors[failed]);
    }
}

} // namespace rainbohz::rendering_engine
