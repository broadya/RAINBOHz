#pragma once

// The hot loop: given a block of partials with their state in Structure-of-
// Arrays form, a range of samples and an output span, add the partials'
// samples into the span (docs/rendering.md section 7).
//
// This is the one file the parallel version rewrites, and the boundary at
// which unit types stop. anchorPartial() takes unit-typed values and unwraps
// them into the block's arrays; renderChunk() works on raw doubles and
// SampleValues and never sees a unit type (AGENTS.md, C++ Conventions). Neither
// allocates, locks, throws or performs I/O, and neither has access to
// engine-db. Everything that reaches them has been validated.
//
// Per partial, per sample, the loop does one sine, one exponential, and a
// handful of additions: the phase advances by the exact one-sample increment
// of the segment's antiderivative, freq + g/2 (ADR 0007), plus the slope of
// the phase compensation in force (ADR 0019), the frequency by
// its gradient, the amplitude by its gradient; the roll-off is a function of
// the frequency at the sample (ADR 0008). No accumulated value survives a
// chunk boundary: the caller re-anchors every active partial from the closed
// form at the start of every chunk.

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/sample_value.hpp"
#include "rainbohz/rendering_engine/segment_maths.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace rainbohz::rendering_engine {

/// The per-partial state of a block, one array per quantity, all double.
///
/// The three that change every sample -- phase, freq, amp -- and the three
/// per-chunk constants -- gradient, phaseStepOffset, ampGradient. Lane `i` of
/// every array belongs to the same partial.
struct BlockState {
    /// Wrapped phase at the current sample, radians, in [-π, π].
    std::vector<double> phase;

    /// Instantaneous frequency at the current sample, radians per sample.
    std::vector<double> freq;

    /// Nominal amplitude at the current sample, dB.
    std::vector<double> amp;

    /// The render segment's frequency gradient g, radians per sample².
    std::vector<double> gradient;

    /// g / 2 + c: the constant in the one-sample phase increment. g / 2
    /// makes the increment the exact integral of a linear segment (ADR 0007);
    /// c is the slope of the phase compensation, radians per sample
    /// (ADR 0019). Folded into one value so that phase control costs the
    /// loop nothing.
    std::vector<double> phaseStepOffset;

    /// The render segment's amplitude gradient, dB per sample.
    std::vector<double> ampGradient;

    /// A block of `partials` lanes, all zero.
    ///
    /// @param partials  Number of lanes. The one allocation the kernel's
    ///                  state ever makes; it happens in the preparation
    ///                  layer, once per partial set per frame.
    explicit BlockState(std::size_t partials);

    /// The number of lanes.
    std::size_t size() const noexcept { return phase.size(); }
};

/// The closed-form state from which one partial's lane is set at the start of
/// a chunk (docs/rendering.md section 7.2).
struct Anchor {
    /// The render segment's start phase φ_s, unwrapped.
    Phase startPhase;

    /// The render segment's frequency path, re-based to its start.
    NominalFrequency nominal;

    /// The render segment's amplitude path, re-based to its start.
    NominalAmplitude amplitude;

    /// The render segment's phase compensation, re-based to its start.
    NominalCompensation compensation;

    /// Source-time offset of the chunk's first sample from the render
    /// segment's start: τ_a. Not negative.
    SampleDuration tau;
};

/// Sets lane `lane` of `block` from the closed form at `anchor.tau`, exactly.
///
/// phase = wrap(phaseAt(startPhase, nominal, τ) + compensationAt(compensation,
/// τ)), freq = frequencyAt(nominal, τ), amp = amplitudeAt(amplitude, τ); the
/// gradients are the segment's, and the phase step offset is g / 2 plus the
/// compensation's slope. This is the boundary where unit types are unwrapped
/// into the arrays.
///
/// @param[in,out] block  The block. Lane `lane` of every array is overwritten.
/// @param lane           Which lane. Less than `block.size()`; not checked.
/// @param anchor         The state to set it from.
void anchorPartial(BlockState& block, std::size_t lane, const Anchor& anchor) noexcept;

/// Adds the samples of the active lanes over `[first, last)` into `out`.
///
/// For each sample, in order: sum over the active lanes of
/// `sin(phase) · toLinear(amp + rollOff(freq))`, added into `out[n]` once
/// per sample; then each active lane's phase advances by the exact
/// one-sample increment `freq + phaseStepOffset` and is wrapped, its freq by
/// `gradient`, and its amp by `ampGradient`. Everything is computed in
/// double and narrowed to SampleValue once, at the add.
///
/// Serial and scalar in this version; docs/rendering.md section 7.5 says
/// what the parallel version changes, and that its inputs and outputs are
/// these.
///
/// @param[in,out] block   The block. The phase, freq and amp of every active
///                        lane are advanced to the state at sample `last`.
/// @param active          Lanes to render, each less than `block.size()`.
///                        Inactive lanes are neither read nor written.
/// @param first           First sample of the chunk, within the frame.
/// @param last            One past the last sample of the chunk. Not before
///                        `first`; at most `out.size()`.
/// @param rollOff         The roll-off's band edges, radians per sample.
/// @param[in,out] out     The frame buffer. Samples `[first, last)` have the
///                        chunk's contribution added to them.
/// @pre No allocation, locking, exception or I/O is reachable from here.
///      Preconditions on the indexes are not checked.
void renderChunk(BlockState& block, std::span<const std::size_t> active, SampleIndex first,
    SampleIndex last, const RollOffRaw& rollOff, std::span<SampleValue> out) noexcept;

} // namespace rainbohz::rendering_engine
