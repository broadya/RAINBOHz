// SPDX-License-Identifier: Apache-2.0

#pragma once

// Fork-join parallelism: the only place the engine creates threads (ADR 0026).
//
// forEachIndex() runs one task per index on a few threads and returns only
// when every call has finished, so a task may refer to the caller's locals.
// There is no queue, no pool and no state between calls. Rendering uses it to
// render a frame's partial sets at once (docs/rendering.md section 7.5).
//
// A task must make each call independent of every other. It may read data
// that nothing writes during the call, and write only to what belongs to its
// own index: distinct elements of a vector sized beforehand, never resized
// during the call, and never a std::vector<bool>, whose elements share words.
//
// The floating-point environment (rounding mode, flush-to-zero) is per
// thread, and a new thread starts with the default one. The engine never
// changes it, so every thread computes alike. Code that changes it must
// apply the caller's environment in each worker too, or the thread count
// would change the output.

#include <cstddef>
#include <functional>

namespace rainbohz::rendering_engine {

/// The most threads forEachIndex() uses, whatever is asked for.
inline constexpr std::size_t kMaxThreads = 256;

/// Resolves a requested thread count.
///
/// @param requested  0 for one per hardware thread, otherwise the count wanted.
/// @return `std::thread::hardware_concurrency()` for 0, or 1 if that is
///         unknown; otherwise `requested`; clamped to [1, kMaxThreads].
std::size_t resolveThreadCount(std::size_t requested) noexcept;

/// Calls `task(i)` once for every i in [0, count), on up to `threads`
/// threads including the caller's, and returns when every call has finished.
///
/// Indices are claimed in increasing order from a shared counter, so a slow
/// call does not hold up the others. With `threads` 1, or `count` at most 1,
/// no thread is created and every call runs on the caller's thread in index
/// order. If fewer threads can be created than asked for, the ones that were
/// and the caller's finish the work.
///
/// Every write a call makes is visible to the caller when this returns.
///
/// @param threads  How many threads may run calls, the caller's included;
///                 0 counts as 1, and more than kMaxThreads as kMaxThreads.
/// @param count    How many indices.
/// @param task     Called once per index. May throw.
/// @throws The exception thrown by the call with the lowest index that
///         threw, once every call that started has finished. Indices above
///         a failure that no thread had claimed yet are not called. Every
///         index below it was called and finished.
/// @throws std::logic_error  If called from inside a call of `task`:
///         nesting would multiply the threads.
void forEachIndex(
    std::size_t threads, std::size_t count, const std::function<void(std::size_t)>& task);

} // namespace rainbohz::rendering_engine
