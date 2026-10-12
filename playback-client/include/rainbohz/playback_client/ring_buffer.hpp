// SPDX-License-Identifier: Apache-2.0

#pragma once

// A single-producer, single-consumer ring buffer of samples, lock-free.
//
// The samples are interleaved values: for a session of C channels, one
// sample frame is C of them, and the device sink pushes whole frames only.
//
// The bridge between the feeder, which may block, and the audio callback,
// which must not (docs/design.md §5). One thread pushes, one other thread
// pops; neither ever waits for the other, and neither allocates after
// construction. Each side owns one index and only reads the other's, with
// acquire/release ordering, so a sample is always written before the
// consumer can see it and read before the producer can overwrite it.

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rainbohz::playback_client {

/// A lock-free SPSC ring of floats.
class SampleRing {
public:
    /// A ring that holds up to `capacity` samples.
    ///
    /// @param capacity  The most samples it holds; positive. A ring of 0
    ///                  would accept nothing, and a producer waiting for
    ///                  room would wait for ever.
    explicit SampleRing(std::size_t capacity) : buffer_(capacity) { assert(capacity > 0); }

    /// The most samples the ring holds.
    ///
    /// @return The capacity given to the constructor.
    std::size_t capacity() const noexcept { return buffer_.size(); }

    /// Appends as many of `samples` as fit, in whole granules. Producer side
    /// only.
    ///
    /// A granule is the unit the consumer pops in: one interleaved sample
    /// frame, `channels` values. Pushing only whole ones means the ring always
    /// holds whole sample frames, so a consumer popping whole frames never
    /// gets part of one.
    ///
    /// @param samples  Values to append, in order.
    /// @param granule  Append a multiple of this many; positive.
    /// @return How many were appended, from the front of `samples`; a
    ///         multiple of `granule`, unless `samples` itself is not. Fewer
    ///         than all when the ring is full.
    /// @note Real-time safe: no lock, allocation or system call.
    std::size_t push(std::span<const float> samples, std::size_t granule = 1) noexcept {
        const std::uint64_t write = write_.load(std::memory_order_relaxed);
        const std::uint64_t read = read_.load(std::memory_order_acquire);
        std::size_t room = capacity() - static_cast<std::size_t>(write - read);
        room -= room % granule;
        const std::size_t count = samples.size() < room ? samples.size() : room;
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[static_cast<std::size_t>((write + i) % capacity())] = samples[i];
        }
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    /// Removes up to `out.size()` samples into `out`. Consumer side only.
    ///
    /// @param[out] out  Receives the samples, from its front.
    /// @return How many were removed; fewer than `out.size()` when the ring
    ///         held fewer. The rest of `out` is left as it was.
    /// @note Real-time safe: no lock, allocation or system call.
    std::size_t pop(std::span<float> out) noexcept {
        const std::uint64_t read = read_.load(std::memory_order_relaxed);
        const std::uint64_t write = write_.load(std::memory_order_acquire);
        const auto held = static_cast<std::size_t>(write - read);
        const std::size_t count = out.size() < held ? out.size() : held;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = buffer_[static_cast<std::size_t>((read + i) % capacity())];
        }
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    /// How many samples the ring holds.
    ///
    /// @return The count. Exact from either side's own point of view. From a
    ///         third thread, the count the ring really held at one instant
    ///         during the call, so never more than capacity(); it may be
    ///         stale by the time it is used.
    std::size_t size() const noexcept {
        // write_ is loaded between two loads of read_, and kept only if
        // read_ did not move, so the pair is the ring's state at one instant.
        // Loaded after a read_ it can never be behind, write_ cannot make the
        // difference wrap; and the producer only fills up to a read_ that the
        // second load is at least, so it cannot exceed the capacity. Either
        // single pair of loads can fail one or the other when both sides
        // move in between. read_ moves at most once per audio callback, so a
        // retry is rare.
        while (true) {
            const std::uint64_t read = read_.load(std::memory_order_acquire);
            const std::uint64_t write = write_.load(std::memory_order_acquire);
            if (read_.load(std::memory_order_acquire) == read) {
                return static_cast<std::size_t>(write - read);
            }
        }
    }

private:
    /// The samples; sample number n is at n modulo capacity().
    std::vector<float> buffer_;
    /// Samples ever pushed. Written by the producer only.
    std::atomic<std::uint64_t> write_{0};
    /// Samples ever popped. Written by the consumer only.
    std::atomic<std::uint64_t> read_{0};
};

} // namespace rainbohz::playback_client
