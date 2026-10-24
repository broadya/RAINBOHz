// SPDX-License-Identifier: Apache-2.0

/// @file
/// @brief The SIMD render kernel: lanes across partials, with Highway.
///
/// Implements renderChunkSimd() of render_kernel.hpp (docs/rendering.md
/// section 7.5, ADR 0027). Highway compiles the body below once per target
/// (NEON, SVE, SVE2, AVX2, AVX-512 and the rest it supports) and picks the
/// best one for the machine at the first call.
///
/// The work per lane per sample is the reference kernel's, in the same
/// order: sine of the phase, roll-off of the frequency, the dB sum, the
/// conversion to linear with its floor and clip, then the capped phase step,
/// the wrap, and the frequency and amplitude steps. What differs is how it
/// is laid out:
///
/// - **Tiles.** The chunk is taken kTile samples at a time. For each vector
///   of partials, its state stays in registers for the whole tile, and its
///   contribution to each sample is added into that sample's accumulator,
///   one vector per sample. After every vector of the block, each
///   accumulator is reduced horizontally once and added into the output. A
///   partial's state is loaded and stored once per tile, not once per
///   sample.
/// - **Branch-free.** The roll-off is two clamped ramps, the floor and clip
///   are masks, and the wrap is one masked ±2π. Only a step of more than
///   2π, which leaves a lane outside [−π, π] after one wrap, takes a branch;
///   the whole vector takes it together, and reduces by the nearest whole
///   number of cycles.
///
/// The sum over partials is in a different order from the reference, and
/// the sine and exponential are Highway's polynomials rather than libm, so
/// the output agrees with the reference within a bound (docs/rendering.md
/// section 10), not bit for bit. For given inputs on a given machine it is
/// always the same, which is what keeps the output independent of the
/// thread count (ADR 0026).
///
/// @see render_kernel.hpp

// Highway's interface: foreach_target.h includes this file again, once per
// target, under this name.
#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "render_kernel_simd.cpp" // NOLINT(cppcoreguidelines-macro-usage)
#include <hwy/foreach_target.h>                     // IWYU pragma: keep

#include <hwy/contrib/math/math-inl.h>
#include <hwy/highway.h>

#include "rainbohz/rendering_engine/render_kernel.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>

HWY_BEFORE_NAMESPACE();
namespace rainbohz::rendering_engine::HWY_NAMESPACE {
namespace {

namespace hn = hwy::HWY_NAMESPACE;

/// Samples per tile. Its accumulators, one vector per sample, stay in level
/// one cache at every vector width: 64 samples of 2048-bit vectors is 16 KiB.
constexpr std::int32_t kTile = 64;

/// The most doubles in one vector of any target: kBlockLanePadding.
constexpr std::size_t kMaxLanes = kBlockLanePadding;

static_assert(HWY_MAX_BYTES / sizeof(double) <= kMaxLanes,
    "a Highway target is wider than kBlockLanePadding lanes of double");

/// A dB value below the floor: the amplitude of a silent padding lane.
constexpr double kSilentDecibels = -1000.0;

/// A tile's accumulators: one vector of the widest target per sample.
using Accumulators = std::array<double, static_cast<std::size_t>(kTile) * kMaxLanes>;

/// ln(10) / 20: toLinear's 10^(dB/20) as exp(dB · this).
constexpr double kDecibelsToNaturalLog = std::numbers::ln10 / 20.0;

/// The constants of the loop, as vectors, built once per call.
template <class D>
struct Constants {
    using V = hn::Vec<D>;

    V pi;
    V minusPi;
    V twoPi;
    V inverseTwoPi;
    V maxStep;
    V floor;
    V fullScale;
    V toNatural;
    V zero;
    V one;
    V lowInner;
    V lowScale;
    V highInner;
    V highScale;

    /// The constants for `rollOff`.
    ///
    /// @param d        The tag.
    /// @param rollOff  The roll-off's band edges.
    Constants(D d, const RollOffRaw& rollOff)
        : pi(hn::Set(d, std::numbers::pi)), minusPi(hn::Set(d, -std::numbers::pi)),
          twoPi(hn::Set(d, 2.0 * std::numbers::pi)),
          inverseTwoPi(hn::Set(d, 1.0 / (2.0 * std::numbers::pi))),
          maxStep(hn::Set(d, kMaxPhaseStep)), floor(hn::Set(d, raw::kSilenceFloorDecibels)),
          fullScale(hn::Set(d, kFullScale.value())), toNatural(hn::Set(d, kDecibelsToNaturalLog)),
          zero(hn::Zero(d)), one(hn::Set(d, 1.0)), lowInner(hn::Set(d, rollOff.lowInner)),
          lowScale(hn::Set(d, 1.0 / (rollOff.lowInner - rollOff.lowOuter))),
          highInner(hn::Set(d, rollOff.highInner)),
          highScale(hn::Set(d, 1.0 / (rollOff.highOuter - rollOff.highInner))) {}
};

/// One vector of partials over one tile of samples.
///
/// Two cases are decided once per vector and tile, not once per sample, and
/// select a specialisation. Each gives the same values as the general form
/// for the lanes it is chosen for.
///
/// @tparam kGeneral  False when no lane's step can exceed π over the tile:
///                   then every step is taken, no lane is capped, and one
///                   conditional ±2π wraps it exactly. True otherwise: the
///                   step cap, the count, and the reduction by whole cycles
///                   for a step beyond 2π.
/// @tparam kRollOff  False when every lane stays inside the audio band over
///                   the tile, where the roll-off is 0 dB exactly.
/// @param d          The tag.
/// @param k          The constants.
/// @param[in,out] ph The phases; advanced over the tile.
/// @param[in,out] f  The frequencies; advanced.
/// @param[in,out] a  The amplitudes, dB; advanced.
/// @param g          The frequency gradients.
/// @param offset     The phase step offsets.
/// @param ga         The amplitude gradients.
/// @param[in,out] held  Gains one per lane per sample whose step was not taken.
/// @param tile       Samples in the tile.
/// @param[in,out] acc   The tile's accumulators, one vector per sample.
template <bool kGeneral, bool kRollOff, class D, class V = hn::Vec<D>>
HWY_INLINE void renderTile(D d, const Constants<D>& k, V& ph, V& f, V& a, V g, V offset, V ga,
    V& held, std::size_t tile, Accumulators& acc) {
    const std::size_t width = hn::Lanes(d);
    for (std::size_t t = 0; t < tile; ++t) {
        const V sine = hn::Sin(d, ph);
        V decibels = a;
        if constexpr (kRollOff) {
            // The roll-off (section 8.2) as two clamped ramps, each 0 at its
            // inner edge and 1 at and beyond its outer edge, scaled to the
            // floor. The bands do not overlap, so at most one is non-zero;
            // below the low outer edge, at negative frequency and above the
            // high outer edge the sum is the floor exactly.
            const V low =
                hn::Min(hn::Max(hn::Mul(hn::Sub(k.lowInner, f), k.lowScale), k.zero), k.one);
            const V high =
                hn::Min(hn::Max(hn::Mul(hn::Sub(f, k.highInner), k.highScale), k.zero), k.one);
            decibels = hn::Add(a, hn::Mul(k.floor, hn::Add(low, high)));
        }
        // The exponential is taken on every lane, clamped into its range; the
        // floor's mask then discards the lanes at or below it, as the
        // reference never evaluates them.
        const V clamped = hn::Min(hn::Max(decibels, k.floor), k.fullScale);
        const V linear = hn::IfThenElseZero(
            hn::Gt(decibels, k.floor), hn::Exp(d, hn::Mul(clamped, k.toNatural)));
        double* HWY_RESTRICT sum = &acc[t * width];
        hn::Store(hn::MulAdd(sine, linear, hn::Load(d, sum)), d, sum);

        // The exact one-sample increment (section 7.3): the same sums as the
        // reference, so that the phase agrees with it to the bit wherever a
        // step needs at most one wrap.
        V next = hn::Add(hn::Add(ph, f), offset);
        next = hn::IfThenElse(hn::Gt(next, k.pi), hn::Sub(next, k.twoPi), next);
        next = hn::IfThenElse(hn::Lt(next, k.minusPi), hn::Add(next, k.twoPi), next);
        if constexpr (kGeneral) {
            const auto take = hn::Le(hn::Abs(hn::Add(f, offset)), k.maxStep);
            const auto outside = hn::Or(hn::Gt(next, k.pi), hn::Lt(next, k.minusPi));
            if (!hn::AllFalse(d, hn::And(take, outside))) {
                // A step of more than 2π: reduce by the nearest whole number
                // of cycles, then wrap once more for the rounding.
                next = hn::NegMulAdd(hn::Round(hn::Mul(next, k.inverseTwoPi)), k.twoPi, next);
                next = hn::IfThenElse(hn::Gt(next, k.pi), hn::Sub(next, k.twoPi), next);
                next = hn::IfThenElse(hn::Lt(next, k.minusPi), hn::Add(next, k.twoPi), next);
            }
            ph = hn::IfThenElse(take, next, ph);
            held = hn::Add(held, hn::IfThenZeroElse(take, k.one));
        } else {
            ph = next;
        }

        f = hn::Add(f, g);
        a = hn::Add(a, ga);
    }
}

/// The kernel for one target. Arguments as renderChunkSimd().
///
/// @param[in,out] block  The block.
/// @param lanes          Active lanes, `[0, lanes)`.
/// @param first          First sample of the chunk.
/// @param last           One past the last sample.
/// @param rollOff        The roll-off's band edges.
/// @param[in,out] out    The frame buffer.
/// @note Not declared noexcept, which Highway's dispatch table does not take
///       in a function type; nothing in it throws, and renderChunkSimd() is.
void renderChunkTarget(BlockState& block, std::size_t lanes, std::int32_t first, std::int32_t last,
    const RollOffRaw& rollOff, std::span<SampleValue> out) {
    using D = hn::ScalableTag<double>;
    const D d;
    const hn::RebindToSigned<D> di;
    using VecD = hn::Vec<D>;
    const std::size_t width = hn::Lanes(d);
    const std::size_t padded = (lanes + width - 1) / width * width;

    const std::span<double> phase{block.phase};
    const std::span<double> freq{block.freq};
    const std::span<double> amp{block.amp};
    const std::span<double> gradient{block.gradient};
    const std::span<double> phaseStepOffset{block.phaseStepOffset};
    const std::span<double> ampGradient{block.ampGradient};
    const std::span<std::int64_t> capped{block.cappedPhaseSteps};

    // The lanes that complete the last vector are scratch: make them silent
    // and still, so that they add nothing and are never capped.
    for (std::size_t i = lanes; i < padded; ++i) {
        phase[i] = 0.0;
        freq[i] = 0.0;
        amp[i] = kSilentDecibels;
        gradient[i] = 0.0;
        phaseStepOffset[i] = 0.0;
        ampGradient[i] = 0.0;
    }

    const Constants<D> k{d, rollOff};
    const VecD lowLimit = hn::Set(d, rollOff.lowInner);
    const VecD highLimit = hn::Set(d, rollOff.highInner);

    HWY_ALIGN Accumulators acc;

    for (std::int32_t tileStart = first; tileStart < last; tileStart += kTile) {
        const auto tile = static_cast<std::size_t>(std::min(kTile, last - tileStart));
        const VecD span = hn::Set(d, static_cast<double>(tile));
        for (std::size_t t = 0; t < tile; ++t) {
            hn::Store(k.zero, d, &acc[t * width]);
        }

        for (std::size_t v = 0; v < padded; v += width) {
            VecD ph = hn::LoadU(d, &phase[v]);
            VecD f = hn::LoadU(d, &freq[v]);
            VecD a = hn::LoadU(d, &amp[v]);
            const VecD g = hn::LoadU(d, &gradient[v]);
            const VecD offset = hn::LoadU(d, &phaseStepOffset[v]);
            const VecD ga = hn::LoadU(d, &ampGradient[v]);
            VecD held = k.zero;

            // Over the tile the frequency moves by at most |g| per sample, so
            // |g| · tile bounds how far it goes, with room for the rounding of
            // its accumulation. A comparison with a NaN is false, so a lane
            // that is not a number always takes the general form.
            const VecD reach = hn::Mul(hn::Abs(g), span);
            const bool smallSteps =
                hn::AllTrue(d, hn::Le(hn::Add(hn::Abs(hn::Add(f, offset)), reach), k.pi));
            const bool inBand = hn::AllTrue(d,
                hn::And(hn::Ge(hn::Sub(f, reach), lowLimit), hn::Le(hn::Add(f, reach), highLimit)));
            if (smallSteps && inBand) [[likely]] {
                renderTile<false, false>(d, k, ph, f, a, g, offset, ga, held, tile, acc);
            } else if (smallSteps) {
                renderTile<false, true>(d, k, ph, f, a, g, offset, ga, held, tile, acc);
            } else {
                renderTile<true, true>(d, k, ph, f, a, g, offset, ga, held, tile, acc);
            }

            hn::StoreU(ph, d, &phase[v]);
            hn::StoreU(f, d, &freq[v]);
            hn::StoreU(a, d, &amp[v]);
            hn::StoreU(hn::Add(hn::LoadU(di, &capped[v]), hn::ConvertTo(di, held)), di, &capped[v]);
        }

        for (std::size_t t = 0; t < tile; ++t) {
            const double sum = hn::ReduceSum(d, hn::Load(d, &acc[t * width]));
            out[static_cast<std::size_t>(tileStart) + t] += static_cast<SampleValue>(sum);
        }
    }
}

} // namespace
} // namespace rainbohz::rendering_engine::HWY_NAMESPACE
HWY_AFTER_NAMESPACE();

#if HWY_ONCE

namespace rainbohz::rendering_engine {

HWY_EXPORT(renderChunkTarget);

void renderChunkSimd(BlockState& block, std::size_t lanes, SampleIndex first, SampleIndex last,
    const RollOffRaw& rollOff, std::span<SampleValue> out) noexcept {
    HWY_DYNAMIC_DISPATCH(renderChunkTarget)
    (block, lanes, first.value(), last.value(), rollOff, out);
}

} // namespace rainbohz::rendering_engine

#endif // HWY_ONCE
