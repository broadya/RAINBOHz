// SPDX-License-Identifier: Apache-2.0

#pragma once

// The hot loop: given a block of partials with their state in Structure-of-
// Arrays form, a range of samples and an output span, add the partials'
// samples into the span (docs/rendering.md section 7).
//
// This is the boundary at which unit types stop. anchorPartial() takes
// unit-typed values and unwraps them into the block's arrays; renderChunk()
// works on raw doubles and SampleValues and never sees a unit type (AGENTS.md,
// C++ Conventions). None of these functions allocates, locks, throws or
// performs I/O, and none has access to engine-db. Everything that reaches them
// has been validated.
//
// There are two kernels with the same inputs and outputs (ADR 0027). The
// scalar kernel is the reference: libm's sine and exponential, the partials
// summed one at a time in lane order. The SIMD kernel (render_kernel_simd.cpp)
// runs lanes across partials with Highway, polynomial sine and exponential,
// and tiles the samples; its output agrees with the reference within the
// bound of docs/rendering.md section 10, not bit for bit, and its bits depend
// on the machine's vector width. The engine uses SIMD unless --render-kernel
// scalar asks for the reference.
//
// Per partial, per sample, the loop does one sine, one exponential, and a
// handful of additions: the phase advances by the exact one-sample increment
// of the segment's antiderivative, freq + g/2 (ADR 0007), plus the slope of
// the phase compensation in force (ADR 0019), the frequency by
// its gradient, the amplitude by its gradient; the roll-off is a function of
// the frequency at the sample (ADR 0008). No accumulated value survives a
// chunk boundary: the caller re-anchors every active partial from the closed
// form at the start of every chunk.
//
// The second kernel, spatialiseChunk(), runs once per SoundSource rather than
// once per partial: it spreads the source's mono sum over the output channels
// with the gains of the source's position at each sample (section 8.5). It
// costs a few transcendentals per sample per source, independent of how many
// partials the source has.

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/channel_mode.hpp"
#include "rainbohz/rendering_engine/sample_value.hpp"
#include "rainbohz/rendering_engine/segment_maths.hpp"
#include "rainbohz/rendering_engine/spatial_maths.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <string_view>
#include <vector>

namespace rainbohz::rendering_engine {

/// The largest one-sample phase step the kernel takes: 1000 periods,
/// radians per sample (author, 2026-10-11).
///
/// Frequencies have no limit in the model, and modulation can carry a partial
/// far beyond the audio band, where the roll-off silences it but the maths
/// still runs. This is where that stops: a larger step, or one that is not a
/// number, leaves the phase where it was for that sample. The limit is the
/// kernel's own, in its own units, because what it bounds is the work of
/// wrapping the phase. At 20 kHz sampling it is 20 MHz; at 96 kHz, 96 MHz.
/// The phase that is held can make a discontinuity before the next anchor,
/// which the roll-off makes inaudible in all but pathological cases.
inline constexpr double kMaxPhaseStep = 1000.0 * 2.0 * std::numbers::pi;

/// Which kernel renders a chunk (ADR 0027).
enum class RenderKernel : std::uint8_t {
    Simd,   ///< Lanes across partials, Highway; the default.
    Scalar, ///< The reference: one partial at a time, libm.
};

/// The kernel's name, as --render-kernel takes it and the report prints it.
///
/// @param kernel  The kernel.
/// @return "simd" or "scalar".
constexpr std::string_view renderKernelName(RenderKernel kernel) noexcept {
    return kernel == RenderKernel::Simd ? "simd" : "scalar";
}

/// The multiple a block's lane count is rounded up to: the widest vector of
/// doubles any Highway target has, 2048-bit SVE. The SIMD kernel works in
/// whole vectors and never needs a tail loop.
inline constexpr std::size_t kBlockLanePadding = 32;

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

    /// How many times the lane's phase was held rather than advanced: once
    /// per sample whose step exceeded kMaxPhaseStep or was not a number, and
    /// once per anchor whose phase was not finite. Zero for any partial whose
    /// frequency stays within the limit. Never reset by anchorPartial(); it
    /// counts over the block's whole life.
    std::vector<std::int64_t> cappedPhaseSteps;

    /// A block of at least `partials` lanes, all zero.
    ///
    /// @param partials  Number of lanes wanted, rounded up to a multiple of
    ///                  kBlockLanePadding. The one allocation the kernel's
    ///                  state ever makes; it happens in the preparation
    ///                  layer, once per partial set per frame.
    explicit BlockState(std::size_t partials);

    /// The number of lanes.
    ///
    /// @return The length of every array: the count asked for, rounded up
    ///         to a multiple of kBlockLanePadding.
    std::size_t size() const noexcept { return phase.size(); }
};

/// The closed-form state from which one partial's lane is set at the start of
/// a chunk (docs/rendering.md section 7.2).
struct Anchor {
    /// The render segment's start phase φ_s. Precomputation stores it
    /// wrapped to [-π, π]; any finite value gives the same result.
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
/// into the arrays. A phase that is not finite, from a frequency so large
/// that the closed form overflows, is set to 0 and counted in
/// cappedPhaseSteps, so that no NaN reaches the loop.
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
/// `gradient`, and its amp by `ampGradient`. An increment beyond
/// kMaxPhaseStep, or one that is not a number, is not taken: the phase holds
/// and the lane's cappedPhaseSteps goes up by one. Everything is computed in
/// double and narrowed to SampleValue once, at the add.
///
/// This is the scalar reference kernel, over any set of lanes. The engine
/// calls the dense overload below, which takes the kernel to use.
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

/// Adds the samples of lanes `[0, lanes)` over `[first, last)` into `out`,
/// with the kernel `kernel`.
///
/// The dense form the engine uses: the caller anchors a chunk's active
/// partials into the first lanes of the block, in order, so that every load
/// is contiguous. With RenderKernel::Scalar this is exactly the reference
/// renderChunk() with `active` = 0, 1, ..., lanes − 1; with RenderKernel::Simd
/// it is renderChunkSimd().
///
/// @param kernel          Which kernel.
/// @param[in,out] block   The block. Lanes `[0, lanes)` are advanced as by
///                        the reference; lanes from `lanes` on are scratch,
///                        and the SIMD kernel may overwrite them.
/// @param lanes           How many lanes are active. At most `block.size()`.
/// @param first           First sample of the chunk, within the frame.
/// @param last            One past the last sample. Not before `first`; at
///                        most `out.size()`.
/// @param rollOff         The roll-off's band edges, radians per sample.
/// @param[in,out] out     The frame buffer.
/// @pre As the reference renderChunk(). Not checked.
void renderChunk(RenderKernel kernel, BlockState& block, std::size_t lanes, SampleIndex first,
    SampleIndex last, const RollOffRaw& rollOff, std::span<SampleValue> out) noexcept;

/// The SIMD kernel: adds the samples of lanes `[0, lanes)` over
/// `[first, last)` into `out` (docs/rendering.md section 7.5, ADR 0027).
///
/// The same computation as the reference, with lanes across partials in
/// whole vectors, chosen at run time for the machine (Highway's dynamic
/// dispatch): the sine and exponential are Highway's polynomials, the
/// roll-off is branch-free, and the samples are taken in tiles, with each
/// vector's state held in registers across a tile and its contribution
/// summed into one accumulator per sample, reduced horizontally once per
/// sample. The phase step, the cap and the count are as the reference's,
/// lane by lane. Lanes from `lanes` up to the next whole vector are set
/// silent and rendered; they add nothing.
///
/// The output agrees with the reference within the bound of
/// docs/rendering.md section 10. For given inputs on a given machine it is
/// always the same.
///
/// @param[in,out] block   The block. Lanes `[0, lanes)` are advanced as by
///                        the reference; lanes from `lanes` on are scratch.
/// @param lanes           How many lanes are active. At most `block.size()`.
/// @param first           First sample of the chunk, within the frame.
/// @param last            One past the last sample. Not before `first`; at
///                        most `out.size()`.
/// @param rollOff         The roll-off's band edges, radians per sample.
/// @param[in,out] out     The frame buffer.
/// @pre As the reference renderChunk(). Not checked.
void renderChunkSimd(BlockState& block, std::size_t lanes, SampleIndex first, SampleIndex last,
    const RollOffRaw& rollOff, std::span<SampleValue> out) noexcept;

/// The closed-form position from which a spatial chunk starts
/// (docs/rendering.md section 8.5).
struct SpatialAnchor {
    /// The spatial segment's path, re-based to its start.
    NominalPosition position;

    /// Source-time offset of the chunk's first sample from the spatial
    /// segment's start. Not negative.
    SampleDuration tau;
};

/// Spreads a SoundSource's mono samples over `[first, last)` into the output
/// channels, adding them into `interleaved`.
///
/// For each sample n, the position is evaluated in closed form at
/// `anchor.tau + (n − first)` -- nothing is accumulated, so no error carries
/// from sample to sample -- and each channel c of the mode gets
/// `gain_c(position) · mono[n]` added into `interleaved[n · C + c]`, where C
/// is channelCount(mode). The gain is computed and applied in double and
/// narrowed to SampleValue once, at the add.
///
/// The source clock advances 1.0 per sample here; under Doppler the offset
/// becomes the source clock's.
///
/// @param anchor          The covering spatial segment and the offset of
///                        `first` into it.
/// @param mode            The channel mode.
/// @param mono            The source's summed mono buffer, one frame long.
/// @param first           First sample of the chunk, within the frame.
/// @param last            One past the last sample. Not before `first`; at
///                        most `mono.size()`.
/// @param[in,out] interleaved  The frame buffer, `mono.size() · C` samples,
///                        interleaved by channel. Samples of `[first, last)`
///                        have the source's contribution added to them.
/// @pre The position stays in range over the chunk, which validation of the
///      timelines guarantees. No allocation, locking, exception or I/O is
///      reachable from here. Preconditions on the indexes are not checked.
void spatialiseChunk(const SpatialAnchor& anchor, ChannelMode mode,
    std::span<const SampleValue> mono, SampleIndex first, SampleIndex last,
    std::span<SampleValue> interleaved) noexcept;

} // namespace rainbohz::rendering_engine
