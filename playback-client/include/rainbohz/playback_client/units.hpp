// SPDX-License-Identifier: Apache-2.0

#pragma once

// Unit types for the quantities and identifiers playback-client handles.
//
// As in rendering-engine (AGENTS.md, C++ Conventions), each quantity is its
// own type rather than a bare integer, so that a frame size passed where a
// frame number belongs, or a sample rate where a sample count belongs, fails
// to compile. Each type has one member, is explicitly constructed and read
// back with value(), and defines only the operations that make physical
// sense. They are trivially copyable with inline operators.
//
// These are not shared with rendering-engine: components do not link each
// other's code. The client needs only integers, because it works in whole
// samples of a stream it is given; the engine's continuous time types have no
// counterpart here.

#include <compare>
#include <cstdint>

namespace rainbohz::playback_client {

/// Samples per second of the audio the server renders, in Hz.
class SampleRate {
public:
    /// A rate of `hertz` samples per second.
    ///
    /// @param hertz  Samples per second. Validation (positive) is the
    ///               caller's concern.
    constexpr explicit SampleRate(std::int32_t hertz) noexcept : value_{hertz} {}

    /// The rate.
    ///
    /// @return Samples per second, in Hz.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleRate, SampleRate) noexcept = default;

private:
    std::int32_t value_;
};

/// A number of samples: a length of audio, or a distance between two
/// samples. May be negative only as an intermediate.
class SampleCount {
public:
    /// No samples.
    constexpr SampleCount() noexcept = default;

    /// A count of `samples` samples.
    ///
    /// @param samples  The count.
    constexpr explicit SampleCount(std::int64_t samples) noexcept : value_{samples} {}

    /// The count.
    ///
    /// @return The number of samples.
    constexpr std::int64_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleCount, SampleCount) noexcept = default;

    /// The sum of the two operands.
    friend constexpr SampleCount operator+(SampleCount a, SampleCount b) noexcept {
        return SampleCount{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr SampleCount operator-(SampleCount a, SampleCount b) noexcept {
        return SampleCount{a.value_ - b.value_};
    }
    /// Adds `other` to this value in place.
    ///
    /// @param other  The count to add.
    /// @return This value, after the addition.
    constexpr SampleCount& operator+=(SampleCount other) noexcept {
        value_ += other.value_;
        return *this;
    }

    /// This length of audio in seconds at `rate`.
    ///
    /// @param rate  The sample rate; must be positive.
    /// @return value() ÷ rate, in seconds.
    constexpr double seconds(SampleRate rate) const noexcept {
        return static_cast<double>(value_) / static_cast<double>(rate.value());
    }

private:
    std::int64_t value_ = 0;
};

/// Samples per frame: the length of every frame the server renders.
class FrameSize {
public:
    /// A frame of `samples` samples.
    ///
    /// @param samples  Samples per frame. Validation (positive) is the
    ///                 caller's concern.
    constexpr explicit FrameSize(std::int32_t samples) noexcept : value_{samples} {}

    /// The frame size.
    ///
    /// @return Samples per frame.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// The frame size as a count of samples.
    ///
    /// @return value() as a SampleCount.
    constexpr SampleCount count() const noexcept { return SampleCount{value_}; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(FrameSize, FrameSize) noexcept = default;

private:
    std::int32_t value_;
};

/// A frame number, Frame.frame_number in engine-db: frame n holds samples
/// n × frame size to (n + 1) × frame size, exclusive.
class FrameNumber {
public:
    /// Frame `number`.
    ///
    /// @param number  The frame number, 0 or more.
    constexpr explicit FrameNumber(std::int32_t number) noexcept : value_{number} {}

    /// The frame number.
    ///
    /// @return Frame.frame_number.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(FrameNumber, FrameNumber) noexcept = default;

private:
    std::int32_t value_;
};

/// An absolute sample: its index in the score's sample stream, from sample 0
/// at time zero.
///
/// Modelled on std::chrono's time_point: two indexes subtract to a
/// SampleCount, and adding two indexes does not compile.
class SampleIndex {
public:
    /// Sample `index`.
    ///
    /// @param index  The index, 0 or more.
    constexpr explicit SampleIndex(std::int64_t index) noexcept : value_{index} {}

    /// The sample `offset` samples into `frame`, for frames of `size`.
    ///
    /// @param frame   The frame.
    /// @param offset  Index within the frame, from 0 to below `size`.
    /// @param size    Samples per frame.
    /// @return The index frame × size + offset.
    static constexpr SampleIndex inFrame(
        FrameNumber frame, std::int32_t offset, FrameSize size) noexcept {
        return SampleIndex{(static_cast<std::int64_t>(frame.value()) * size.value()) + offset};
    }

    /// The index.
    ///
    /// @return Samples from sample 0.
    constexpr std::int64_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleIndex, SampleIndex) noexcept = default;

    /// The number of samples from `b` to `a`.
    friend constexpr SampleCount operator-(SampleIndex a, SampleIndex b) noexcept {
        return SampleCount{a.value_ - b.value_};
    }

private:
    std::int64_t value_;
};

} // namespace rainbohz::playback_client
