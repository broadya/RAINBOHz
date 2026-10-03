#pragma once

// Unit types for the physical quantities and identifiers of the rendering
// model.
//
// Every quantity has its own type rather than a bare double or int32, so that
// mixing units -- a frame size passed as a frame number, an absolute time
// where a segment-local one belongs -- fails to compile. See AGENTS.md, C++
// Conventions, and rendering-engine/docs/precomputation.md section 3.1, which
// lists the types and the operations they allow.
//
// Each type is a class with one member, explicitly constructed and read back
// with value(). None converts implicitly to or from its representation, or to
// any other unit type. Only operations that make physical sense are defined;
// anything else does not compile. The types are trivially copyable with
// inline operators, and cost nothing at run time.
//
// Units are those of the rendering model (engine-db/schema/engine-schema.cypher,
// "Units and limits"): time in samples, continuous; frequency in radians per
// sample; phase in radians, unwrapped. The amplitude and output types that
// rendering adds are listed in docs/rendering.md section 3.

#include <cmath>
#include <compare>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>

namespace rainbohz::rendering_engine {

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

/// A length of time, or an offset between two points in time, in samples.
///
/// Continuous: it need not be a whole number of samples. May be negative.
/// The time local to a segment, τ, is a SampleDuration measured from the
/// segment's start.
class SampleDuration {
public:
    /// A zero duration.
    constexpr SampleDuration() noexcept = default;

    /// A duration of `samples` samples.
    ///
    /// @param samples  Length in samples; any value, including fractional,
    ///                 negative and non-finite ones.
    constexpr explicit SampleDuration(double samples) noexcept : value_{samples} {}

    /// The duration in samples.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleDuration, SampleDuration) noexcept = default;

    /// The sum of the two operands.
    friend constexpr SampleDuration operator+(SampleDuration a, SampleDuration b) noexcept {
        return SampleDuration{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr SampleDuration operator-(SampleDuration a, SampleDuration b) noexcept {
        return SampleDuration{a.value_ - b.value_};
    }
    /// The negation of this value.
    friend constexpr SampleDuration operator-(SampleDuration a) noexcept {
        return SampleDuration{-a.value_};
    }
    /// The product of the two operands.
    friend constexpr SampleDuration operator*(SampleDuration a, double scale) noexcept {
        return SampleDuration{a.value_ * scale};
    }
    /// The product of the two operands.
    friend constexpr SampleDuration operator*(double scale, SampleDuration a) noexcept {
        return SampleDuration{scale * a.value_};
    }
    /// The first operand divided by the second.
    friend constexpr SampleDuration operator/(SampleDuration a, double divisor) noexcept {
        return SampleDuration{a.value_ / divisor};
    }
    /// The ratio of two durations, which is unitless.
    friend constexpr double operator/(SampleDuration a, SampleDuration b) noexcept {
        return a.value_ / b.value_;
    }

    /// Adds `other` to this value in place.
    constexpr SampleDuration& operator+=(SampleDuration other) noexcept {
        value_ += other.value_;
        return *this;
    }
    /// Subtracts `other` from this value in place.
    constexpr SampleDuration& operator-=(SampleDuration other) noexcept {
        value_ -= other.value_;
        return *this;
    }

private:
    double value_ = 0.0;
};

/// An absolute point in time, in samples from the start of the score.
///
/// Continuous: a point may fall between samples. This is source time in the
/// sense of the schema header, section 2; with no spatial position yet, source
/// time and receiver time coincide.
///
/// Modelled on std::chrono's time_point: subtracting two times gives a
/// SampleDuration, and adding two times does not compile.
class SampleTime {
public:
    /// The start of the score, sample 0.
    constexpr SampleTime() noexcept = default;

    /// The point `samples` samples after the start of the score.
    ///
    /// @param samples  Position in samples; any value, including fractional,
    ///                 negative and non-finite ones. Validation is the
    ///                 caller's concern.
    constexpr explicit SampleTime(double samples) noexcept : value_{samples} {}

    /// The position in samples from the start of the score.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleTime, SampleTime) noexcept = default;

    /// The duration from `b` to `a`.
    friend constexpr SampleDuration operator-(SampleTime a, SampleTime b) noexcept {
        return SampleDuration{a.value_ - b.value_};
    }
    /// The sum of the two operands.
    friend constexpr SampleTime operator+(SampleTime t, SampleDuration d) noexcept {
        return SampleTime{t.value_ + d.value()};
    }
    /// The sum of the two operands.
    friend constexpr SampleTime operator+(SampleDuration d, SampleTime t) noexcept {
        return SampleTime{d.value() + t.value_};
    }
    /// The first operand less the second.
    friend constexpr SampleTime operator-(SampleTime t, SampleDuration d) noexcept {
        return SampleTime{t.value_ - d.value()};
    }

    /// Adds `d` to this value in place.
    constexpr SampleTime& operator+=(SampleDuration d) noexcept {
        value_ += d.value();
        return *this;
    }
    /// Subtracts `d` from this value in place.
    constexpr SampleTime& operator-=(SampleDuration d) noexcept {
        value_ -= d.value();
        return *this;
    }

private:
    double value_ = 0.0;
};

/// A position within a frame, in whole samples: sample `n` of frame `k` is
/// the instant `k * F + n`.
///
/// Discrete, unlike SampleTime and SampleDuration. Adding one to a frame's
/// start time gives the instant of that sample (docs/rendering.md section
/// 3.1); it is not a duration and cannot be used as one.
class SampleIndex {
public:
    /// Sample `index` of a frame.
    ///
    /// @param index  Position within the frame, from 0.
    constexpr explicit SampleIndex(std::int32_t index) noexcept : value_{index} {}

    /// The position within the frame.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleIndex, SampleIndex) noexcept = default;

    /// The instant of sample `n` of a frame that starts at `start`.
    friend constexpr SampleTime operator+(SampleTime start, SampleIndex n) noexcept {
        return SampleTime{start.value() + static_cast<double>(n.value_)};
    }
    /// The instant of sample `n` of a frame that starts at `start`.
    friend constexpr SampleTime operator+(SampleIndex n, SampleTime start) noexcept {
        return start + n;
    }

private:
    std::int32_t value_;
};

// ---------------------------------------------------------------------------
// Seconds
// ---------------------------------------------------------------------------
//
// Physical time in seconds exists only where the engine meets the canonical
// model: a TempoMap converts musical time to seconds, and POINT_AT carries an
// offset in seconds. The transform stage turns seconds into samples at the
// configured sample rate (toSampleTime) and nothing downstream of it sees a
// second again. See docs/transform.md section 3.

/// A length of time, or an offset between two points in time, in seconds.
///
/// Continuous. May be negative: POINT_AT.offset_seconds is allowed to pull a
/// point earlier than its musical position.
class SecondsDuration {
public:
    /// A zero duration.
    constexpr SecondsDuration() noexcept = default;

    /// A duration of `seconds` seconds.
    ///
    /// @param seconds  Length in seconds; any value, including negative and
    ///                 non-finite ones.
    constexpr explicit SecondsDuration(double seconds) noexcept : value_{seconds} {}

    /// The duration in seconds.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SecondsDuration, SecondsDuration) noexcept = default;

    /// The sum of the two operands.
    friend constexpr SecondsDuration operator+(SecondsDuration a, SecondsDuration b) noexcept {
        return SecondsDuration{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr SecondsDuration operator-(SecondsDuration a, SecondsDuration b) noexcept {
        return SecondsDuration{a.value_ - b.value_};
    }
    /// The negation of this value.
    friend constexpr SecondsDuration operator-(SecondsDuration a) noexcept {
        return SecondsDuration{-a.value_};
    }
    /// The product of the two operands.
    friend constexpr SecondsDuration operator*(SecondsDuration a, double scale) noexcept {
        return SecondsDuration{a.value_ * scale};
    }
    /// The product of the two operands.
    friend constexpr SecondsDuration operator*(double scale, SecondsDuration a) noexcept {
        return SecondsDuration{scale * a.value_};
    }
    /// The first operand divided by the second.
    friend constexpr SecondsDuration operator/(SecondsDuration a, double divisor) noexcept {
        return SecondsDuration{a.value_ / divisor};
    }
    /// The ratio of two durations, which is unitless.
    friend constexpr double operator/(SecondsDuration a, SecondsDuration b) noexcept {
        return a.value_ / b.value_;
    }

    /// Adds `other` to this value in place.
    constexpr SecondsDuration& operator+=(SecondsDuration other) noexcept {
        value_ += other.value_;
        return *this;
    }

private:
    double value_ = 0.0;
};

/// An absolute point in physical time, in seconds from the start of the
/// score.
///
/// Continuous. Modelled on SampleTime: subtracting two gives a
/// SecondsDuration, and adding two does not compile.
class SecondsTime {
public:
    /// The start of the score.
    constexpr SecondsTime() noexcept = default;

    /// The point `seconds` seconds after the start of the score.
    ///
    /// @param seconds  Position in seconds; any value. Validation is the
    ///                 caller's concern.
    constexpr explicit SecondsTime(double seconds) noexcept : value_{seconds} {}

    /// The position in seconds from the start of the score.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SecondsTime, SecondsTime) noexcept = default;

    /// The duration from `b` to `a`.
    friend constexpr SecondsDuration operator-(SecondsTime a, SecondsTime b) noexcept {
        return SecondsDuration{a.value_ - b.value_};
    }
    /// The sum of the two operands.
    friend constexpr SecondsTime operator+(SecondsTime t, SecondsDuration d) noexcept {
        return SecondsTime{t.value_ + d.value()};
    }
    /// The sum of the two operands.
    friend constexpr SecondsTime operator+(SecondsDuration d, SecondsTime t) noexcept {
        return SecondsTime{d.value() + t.value_};
    }
    /// The first operand less the second.
    friend constexpr SecondsTime operator-(SecondsTime t, SecondsDuration d) noexcept {
        return SecondsTime{t.value_ - d.value()};
    }

    /// Adds `d` to this value in place.
    constexpr SecondsTime& operator+=(SecondsDuration d) noexcept {
        value_ += d.value();
        return *this;
    }

private:
    double value_ = 0.0;
};

// ---------------------------------------------------------------------------
// Phase and frequency
// ---------------------------------------------------------------------------

/// A phase, or a difference in phase, in radians.
///
/// Unwrapped: never reduced modulo 2π, and it may be negative. Accumulated
/// phase grows without bound over a partial's lifeline; see
/// docs/precomputation.md section 8 for why double precision is enough.
class Phase {
public:
    /// A phase of zero.
    constexpr Phase() noexcept = default;

    /// A phase of `radians` radians.
    ///
    /// @param radians  Unwrapped phase; any value.
    constexpr explicit Phase(double radians) noexcept : value_{radians} {}

    /// The phase in radians, unwrapped.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Phase, Phase) noexcept = default;

    /// The sum of the two operands.
    friend constexpr Phase operator+(Phase a, Phase b) noexcept {
        return Phase{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr Phase operator-(Phase a, Phase b) noexcept {
        return Phase{a.value_ - b.value_};
    }
    /// The negation of this value.
    friend constexpr Phase operator-(Phase a) noexcept { return Phase{-a.value_}; }
    /// The product of the two operands.
    friend constexpr Phase operator*(Phase a, double scale) noexcept {
        return Phase{a.value_ * scale};
    }
    /// The product of the two operands.
    friend constexpr Phase operator*(double scale, Phase a) noexcept {
        return Phase{scale * a.value_};
    }

    /// Adds `other` to this value in place.
    constexpr Phase& operator+=(Phase other) noexcept {
        value_ += other.value_;
        return *this;
    }
    /// Subtracts `other` from this value in place.
    constexpr Phase& operator-=(Phase other) noexcept {
        value_ -= other.value_;
        return *this;
    }

private:
    double value_ = 0.0;
};

/// Reduces a phase to the range [-π, π].
///
/// The rendering accumulator keeps its phase wrapped so that the sine keeps
/// full precision indefinitely (docs/rendering.md section 7.3). A wrapped
/// phase keeps the Phase type: it is still radians, and nothing needs the
/// compiler to distinguish wrapped from unwrapped.
///
/// @param phase  Any phase; a non-finite value gives NaN.
/// @return The same angle, in [-π, π].
inline Phase wrap(Phase phase) noexcept {
    return Phase{std::remainder(phase.value(), 2.0 * std::numbers::pi)};
}

/// Reduces a phase difference to the half-open range [-π, π): the smallest
/// adjustment that reaches the same angle, with an exact half cycle taken as
/// −π (author, 2026-10-03).
///
/// Unlike wrap(), whose result may be either end of [-π, π], this one has a
/// single answer for every angle. Phase compensation needs that: which way an
/// exact half cycle goes must not depend on rounding (docs/precomputation.md
/// section 6.8).
///
/// @param phase  Any phase; a non-finite value gives NaN.
/// @return The same angle, in [-π, π).
inline Phase wrapHalfOpen(Phase phase) noexcept {
    double reduced = std::remainder(phase.value(), 2.0 * std::numbers::pi);
    if (reduced >= std::numbers::pi) {
        reduced -= 2.0 * std::numbers::pi;
    }
    return Phase{reduced};
}

/// A frequency in radians per sample.
///
/// Unbounded in both directions: the model allows negative frequencies and
/// frequencies outside the audio range (engine-schema.cypher, "Units and
/// limits"). At constant frequency f, one sample advances the phase by f.
class AngularFrequency {
public:
    /// A frequency of zero.
    constexpr AngularFrequency() noexcept = default;

    /// A frequency of `radiansPerSample` radians per sample.
    ///
    /// @param radiansPerSample  Any value, including negative ones.
    constexpr explicit AngularFrequency(double radiansPerSample) noexcept
        : value_{radiansPerSample} {}

    /// The frequency in radians per sample.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(AngularFrequency, AngularFrequency) noexcept = default;

    /// The sum of the two operands.
    friend constexpr AngularFrequency operator+(AngularFrequency a, AngularFrequency b) noexcept {
        return AngularFrequency{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr AngularFrequency operator-(AngularFrequency a, AngularFrequency b) noexcept {
        return AngularFrequency{a.value_ - b.value_};
    }
    /// The negation of this value.
    friend constexpr AngularFrequency operator-(AngularFrequency a) noexcept {
        return AngularFrequency{-a.value_};
    }
    /// The product of the two operands.
    friend constexpr AngularFrequency operator*(AngularFrequency a, double scale) noexcept {
        return AngularFrequency{a.value_ * scale};
    }
    /// The product of the two operands.
    friend constexpr AngularFrequency operator*(double scale, AngularFrequency a) noexcept {
        return AngularFrequency{scale * a.value_};
    }

    /// The phase accumulated at this constant frequency over `d`.
    friend constexpr Phase operator*(AngularFrequency f, SampleDuration d) noexcept {
        return Phase{f.value_ * d.value()};
    }
    /// The phase accumulated at this constant frequency over `d`.
    friend constexpr Phase operator*(SampleDuration d, AngularFrequency f) noexcept {
        return Phase{d.value() * f.value_};
    }

private:
    double value_ = 0.0;
};

/// The constant frequency that accumulates `p` over `d`: the rate of a linear
/// phase ramp, such as a phase compensation segment's.
///
/// A free function rather than a hidden friend: neither operand is an
/// AngularFrequency, so argument-dependent lookup would not find a friend.
///
/// @param p  The phase gained.
/// @param d  The duration over which it is gained. Not zero.
/// @return `p / d`, radians per sample.
constexpr AngularFrequency operator/(Phase p, SampleDuration d) noexcept {
    return AngularFrequency{p.value() / d.value()};
}

/// A rate of change of frequency, in radians per sample².
class FrequencyGradient {
public:
    /// A gradient of zero: constant frequency.
    constexpr FrequencyGradient() noexcept = default;

    /// A gradient of `radiansPerSampleSquared` radians per sample².
    ///
    /// @param radiansPerSampleSquared  Any value; negative for a falling
    ///                                 frequency.
    constexpr explicit FrequencyGradient(double radiansPerSampleSquared) noexcept
        : value_{radiansPerSampleSquared} {}

    /// The gradient in radians per sample².
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(FrequencyGradient, FrequencyGradient) noexcept = default;

    /// The change in frequency over `d` at this gradient.
    friend constexpr AngularFrequency operator*(FrequencyGradient g, SampleDuration d) noexcept {
        return AngularFrequency{g.value_ * d.value()};
    }
    /// The change in frequency over `d` at this gradient.
    friend constexpr AngularFrequency operator*(SampleDuration d, FrequencyGradient g) noexcept {
        return AngularFrequency{d.value() * g.value_};
    }

private:
    double value_ = 0.0;
};

/// The gradient that changes frequency by `change` over `d`.
///
/// @param change  Change in frequency, radians per sample.
/// @param d       Duration over which it changes. Not checked: a zero
///                duration gives an infinite or NaN gradient.
/// @return `change / d`, in radians per sample².
constexpr FrequencyGradient operator/(AngularFrequency change, SampleDuration d) noexcept {
    return FrequencyGradient{change.value() / d.value()};
}

// ---------------------------------------------------------------------------
// Amplitude
// ---------------------------------------------------------------------------

/// An amplitude, or a difference between amplitudes, in decibels.
///
/// Every amplitude term in the model is expressed in dB, and terms compose by
/// addition (docs/rendering.md section 8.1). A nominal amplitude is at most 0
/// dB, full scale; a sum is clipped to that when converted. Nothing here
/// enforces the range: validation and toLinear() are where it is applied.
class Decibels {
public:
    /// Zero dB: full scale, or no change.
    constexpr Decibels() noexcept = default;

    /// An amplitude of `decibels` dB.
    ///
    /// @param decibels  Any value.
    constexpr explicit Decibels(double decibels) noexcept : value_{decibels} {}

    /// The amplitude in dB.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Decibels, Decibels) noexcept = default;

    /// The sum of the two operands: composition of two amplitude terms.
    friend constexpr Decibels operator+(Decibels a, Decibels b) noexcept {
        return Decibels{a.value_ + b.value_};
    }
    /// The first operand less the second.
    friend constexpr Decibels operator-(Decibels a, Decibels b) noexcept {
        return Decibels{a.value_ - b.value_};
    }
    /// The negation of this value.
    friend constexpr Decibels operator-(Decibels a) noexcept { return Decibels{-a.value_}; }

    /// Adds `other` to this value in place.
    constexpr Decibels& operator+=(Decibels other) noexcept {
        value_ += other.value_;
        return *this;
    }
    /// Subtracts `other` from this value in place.
    constexpr Decibels& operator-=(Decibels other) noexcept {
        value_ -= other.value_;
        return *this;
    }

private:
    double value_ = 0.0;
};

/// A rate of change of amplitude, in dB per sample.
///
/// A fade is a straight line in dB, and so a geometric progression in linear
/// amplitude; that is what interpolating in dB means.
class DecibelGradient {
public:
    /// A gradient of zero: constant amplitude.
    constexpr DecibelGradient() noexcept = default;

    /// A gradient of `decibelsPerSample` dB per sample.
    ///
    /// @param decibelsPerSample  Any value; negative for a fade out.
    constexpr explicit DecibelGradient(double decibelsPerSample) noexcept
        : value_{decibelsPerSample} {}

    /// The gradient in dB per sample.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(DecibelGradient, DecibelGradient) noexcept = default;

    /// The change in amplitude over `d` at this gradient.
    friend constexpr Decibels operator*(DecibelGradient g, SampleDuration d) noexcept {
        return Decibels{g.value_ * d.value()};
    }
    /// The change in amplitude over `d` at this gradient.
    friend constexpr Decibels operator*(SampleDuration d, DecibelGradient g) noexcept {
        return Decibels{d.value() * g.value_};
    }

private:
    double value_ = 0.0;
};

/// The gradient that changes amplitude by `change` over `d`.
///
/// @param change  Change in amplitude, dB.
/// @param d       Duration over which it changes. Not checked: a zero
///                duration gives an infinite or NaN gradient.
/// @return `change / d`, in dB per sample.
constexpr DecibelGradient operator/(Decibels change, SampleDuration d) noexcept {
    return DecibelGradient{change.value() / d.value()};
}

/// An amplitude as a multiplier on a unit-amplitude signal: linear, unitless,
/// from 0 (silence) to 1 (full scale).
///
/// Produced only by converting a dB sum (amplitude_maths.hpp, toLinear), once
/// per partial per sample; never added to a dB value.
class LinearAmplitude {
public:
    /// Silence.
    constexpr LinearAmplitude() noexcept = default;

    /// A multiplier of `multiplier`.
    ///
    /// @param multiplier  Linear amplitude; 0 to 1 when it came from
    ///                    toLinear().
    constexpr explicit LinearAmplitude(double multiplier) noexcept : value_{multiplier} {}

    /// The multiplier.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(LinearAmplitude, LinearAmplitude) noexcept = default;

private:
    double value_ = 0.0;
};

/// A wall-clock frequency, in hertz.
///
/// Used only where the rendering model meets the outside world: the band
/// edges of the frequency-range roll-off are specified in Hz and converted to
/// radians per sample once per run (toAngular). Nothing in the render path
/// carries a Hertz value.
class Hertz {
public:
    /// A frequency of `hertz` Hz.
    ///
    /// @param hertz  Any value.
    constexpr explicit Hertz(double hertz) noexcept : value_{hertz} {}

    /// The frequency in Hz.
    constexpr double value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(Hertz, Hertz) noexcept = default;

private:
    double value_;
};

// ---------------------------------------------------------------------------
// Sample rate and frames
// ---------------------------------------------------------------------------

/// A sample rate, in samples per second.
///
/// Used only where the rendering model meets wall-clock seconds: the default
/// frame size is one second.
class SampleRate {
public:
    /// A sample rate of `samplesPerSecond`.
    ///
    /// @param samplesPerSecond  Samples per second. Not checked here;
    ///                          a usable rate is positive.
    constexpr explicit SampleRate(std::int32_t samplesPerSecond) noexcept
        : value_{samplesPerSecond} {}

    /// The rate in samples per second.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(SampleRate, SampleRate) noexcept = default;

private:
    std::int32_t value_;
};

/// The length of a frame, in samples. Frame *k* covers
/// `[k * size, (k + 1) * size)`.
///
/// Configured by EngineConfig.frameSize, defaulting to one second
/// (oneSecond()). Frame size and frame number share a representation and are
/// deliberately not interchangeable; they meet only in frameContaining(),
/// lastFrameBefore() and frameStart().
class FrameSize {
public:
    /// A frame size of `samples` samples.
    ///
    /// @param samples  Frame length in samples. Not checked here; a usable
    ///                 frame size is positive.
    constexpr explicit FrameSize(std::int32_t samples) noexcept : value_{samples} {}

    /// The frame size that makes a frame one second long at `rate`.
    ///
    /// This is the frame size when EngineConfig.frameSize is NULL.
    ///
    /// @param rate  Sample rate of the score.
    /// @return A frame size of `rate.value()` samples.
    static constexpr FrameSize oneSecond(SampleRate rate) noexcept {
        return FrameSize{rate.value()};
    }

    /// The frame size in samples.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(FrameSize, FrameSize) noexcept = default;

private:
    std::int32_t value_;
};

/// The index of a frame. Frame 0 is the first frame of the score.
///
/// Its representation is the INT32 of Frame.frame_number, which crosses
/// component boundaries.
class FrameNumber {
public:
    /// Frame `index`.
    ///
    /// @param index  Frame index; 0 is the first frame of the score.
    constexpr explicit FrameNumber(std::int32_t index) noexcept : value_{index} {}

    /// The frame index.
    constexpr std::int32_t value() const noexcept { return value_; }

    /// Orders two values of this type by their representation.
    friend constexpr auto operator<=>(FrameNumber, FrameNumber) noexcept = default;

private:
    std::int32_t value_;
};

namespace detail {

/// Converts a whole-valued frame position to a FrameNumber, if it is one.
///
/// @param position  A frame index as a double, already floored or ceiled.
/// @return The FrameNumber, or nullopt if `position` is not finite or lies
///         outside the range of INT32.
inline std::optional<FrameNumber> toFrameNumber(double position) noexcept {
    constexpr auto kLowest = static_cast<double>(std::numeric_limits<std::int32_t>::min());
    constexpr auto kHighest = static_cast<double>(std::numeric_limits<std::int32_t>::max());
    if (!std::isfinite(position) || position < kLowest || position > kHighest) {
        return std::nullopt;
    }
    return FrameNumber{static_cast<std::int32_t>(position)};
}

} // namespace detail

/// Converts a wall-clock frequency to the rendering model's unit.
///
/// @param frequency  Frequency in Hz.
/// @param rate       Sample rate. Must be positive.
/// @return `2π · frequency / rate`, in radians per sample.
constexpr AngularFrequency toAngular(Hertz frequency, SampleRate rate) noexcept {
    return AngularFrequency{
        2.0 * std::numbers::pi * frequency.value() / static_cast<double>(rate.value())};
}

/// Converts a duration in seconds to samples at `rate`.
///
/// @param d     Duration in seconds.
/// @param rate  Sample rate. Must be positive.
/// @return `d · rate`, in samples, continuous.
constexpr SampleDuration toSampleDuration(SecondsDuration d, SampleRate rate) noexcept {
    return SampleDuration{d.value() * static_cast<double>(rate.value())};
}

/// Converts a point in time from seconds to samples at `rate`.
///
/// This is the one place physical time changes unit. Everything the transform
/// writes to engine-db has been through it, and nothing downstream sees a
/// second again.
///
/// @param t     Point in time, seconds from the start of the score.
/// @param rate  Sample rate. Must be positive.
/// @return `t · rate`, in samples, continuous.
constexpr SampleTime toSampleTime(SecondsTime t, SampleRate rate) noexcept {
    return SampleTime{t.value() * static_cast<double>(rate.value())};
}

/// The frame that contains the point `t`: ⌊t / size⌋.
///
/// @param t     A point in time.
/// @param size  Frame size. Must be positive.
/// @return The frame whose range `[k * size, (k + 1) * size)` contains `t`;
///         or nullopt if `t` is not finite or the frame number does not fit
///         INT32.
inline std::optional<FrameNumber> frameContaining(SampleTime t, FrameSize size) noexcept {
    return detail::toFrameNumber(std::floor(t.value() / static_cast<double>(size.value())));
}

/// The last frame that overlaps a range ending at `end`: ⌈end / size⌉ − 1.
///
/// Ranges are half-open, so a range ending exactly at `k * size` does not
/// reach frame *k*.
///
/// @param end   The exclusive end of a range.
/// @param size  Frame size. Must be positive.
/// @return The last frame that overlaps `[.., end)`; or nullopt if `end` is
///         not finite or the frame number does not fit INT32.
inline std::optional<FrameNumber> lastFrameBefore(SampleTime end, FrameSize size) noexcept {
    return detail::toFrameNumber(std::ceil(end.value() / static_cast<double>(size.value())) - 1.0);
}

/// The time at which frame `k` starts: `k * size`.
///
/// @param k     A frame.
/// @param size  Frame size.
/// @return The first point in time inside frame `k`. Exact: every product of
///         two INT32 values is representable as a double.
constexpr SampleTime frameStart(FrameNumber k, FrameSize size) noexcept {
    return SampleTime{static_cast<double>(k.value()) * static_cast<double>(size.value())};
}

} // namespace rainbohz::rendering_engine
