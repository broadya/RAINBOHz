#pragma once

// The preparation layer of rendering: from the validated, in-memory data of
// one frame's partials to plain per-partial-set state, and the driver that
// renders one partial set into its buffer chunk by chunk (docs/rendering.md
// sections 6.4 and 7.2).
//
// Everything here is a pure function over in-memory data, testable without a
// database. The gather (rendering.hpp) produces PartialData; this file turns
// it into RenderSegments and chunk cuts, then anchors and calls the kernel.
// The source clock of a partial set lives here and nowhere else: in this
// version it is the identity map, and Doppler will replace it (section 6.4,
// step 1).

#include "rainbohz/rendering_engine/amplitude_maths.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/sample_value.hpp"
#include "rainbohz/rendering_engine/segment_maths.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rainbohz::rendering_engine {

/// One validated point of a partial's amplitude timeline, as rendering needs
/// it. The curve from this point to the next is linear: the only curve this
/// version renders, checked at gather time.
struct AmplitudePoint {
    /// AmplitudePhysicalTimelinePoint.time.
    SampleTime time;

    /// AmplitudePhysicalTimelinePoint.amplitude, dB.
    Decibels amplitude;
};

/// One partial's inputs for one frame, gathered and validated (docs/rendering.md
/// section 6.3).
struct PartialData {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Partial.name; nullopt when NULL.
    std::optional<std::string> partialName;

    /// PartialSet.id of the one set aggregating the partial.
    std::int64_t partialSetId = 0;

    /// The integrable segments from the frame's entry segment onward:
    /// contiguous, in time order, covering at least
    /// `[max(b₀, fs), min(b_last, fe))`. The first may start before the
    /// frame and the last may end after it. Not empty.
    std::vector<Segment> segments;

    /// The amplitude points from the frame's entry point onward: strictly
    /// increasing in time, bracketing the same span. At least 2.
    std::vector<AmplitudePoint> amplitude;
};

/// A span of the frame during which one partial's behaviour is described by
/// one set of parameters, or by nothing at all (docs/rendering.md section
/// 6.4.1).
///
/// The parameters are re-based to the segment's own start, so that the hot
/// loop never refers back to the integrable segment or amplitude piece it came
/// from.
struct RenderSegment {
    /// Start, in source time. `[start, end)`.
    SampleTime start;

    /// End, in source time: the next render segment's start, or the frame's
    /// end.
    SampleTime end;

    /// Whether the partial exists during this span: `b₀ ≤ start < b_last`.
    /// When false, the fields below are meaningless.
    bool present = false;

    /// φ_s: the unwrapped phase at `start`, from phaseAt() on the covering
    /// integrable segment's stored values.
    Phase startPhase;

    /// f_s and g: the covering integrable segment's path, re-based to `start`.
    NominalFrequency nominal;

    /// a_s and g_a: the covering amplitude piece, re-based to `start`.
    NominalAmplitude amplitude;
};

/// Builds one partial's render segments for a frame.
///
/// The boundaries are the frame's start and end, the lifeline's start and end
/// where they fall inside the frame, and every integrable segment boundary
/// and amplitude point time strictly inside the frame; sorted and distinct,
/// one render segment between each consecutive pair, covering the frame
/// contiguously.
///
/// @param partial  The partial's gathered, validated data.
/// @param frame    The frame.
/// @param size     Frame size. Positive.
/// @return The render segments, in time order, from the frame's start to its
///         end. At least one.
/// @throws std::invalid_argument  If a present segment has no covering
///         integrable segment or amplitude piece in `partial`, which the
///         gather's validation rules out.
std::vector<RenderSegment> buildRenderSegments(
    const PartialData& partial, FrameNumber frame, FrameSize size);

/// One partial's render segments within a partial set's plan.
struct PartialPlan {
    /// Partial.id, for the report.
    std::int64_t partialId = 0;

    /// From buildRenderSegments().
    std::vector<RenderSegment> segments;
};

/// Everything needed to render one partial set for one frame, with no
/// reference to the database or to any other set (docs/rendering.md section
/// 6.4).
struct PartialSetPlan {
    /// PartialSet.id.
    std::int64_t partialSetId = 0;

    /// The set's source clock at the frame's first sample. The identity map
    /// in this version: the frame's start. Under Doppler, the result of the
    /// binary search over the set's distance path.
    SampleTime sourceTimeAtFrameStart;

    /// Source time consumed per receiver sample. 1.0 in this version; under
    /// Doppler, from the distance gradient.
    double sourceRate = 1.0;

    /// The set's partials in scope, in the order given.
    std::vector<PartialPlan> partials;

    /// The chunk boundaries, as receiver samples: 0, then every sample at
    /// which any partial moves to its next render segment, then the frame
    /// size. Strictly increasing.
    std::vector<SampleIndex> cuts;
};

/// The source clock's value at receiver sample `n` of the frame.
///
/// The one place the two timebases meet (docs/rendering.md section 3.1).
///
/// @param plan  The set's plan, for its clock.
/// @param n     A sample of the frame.
/// @return `sourceTimeAtFrameStart + n · sourceRate`.
constexpr SampleTime sourceTimeAt(const PartialSetPlan& plan, SampleIndex n) noexcept {
    return plan.sourceTimeAtFrameStart +
           SampleDuration{static_cast<double>(n.value()) * plan.sourceRate};
}

/// Finds the chunk boundaries of a partial set (docs/rendering.md section
/// 6.4.2).
///
/// A render segment starting at source time `s` begins at receiver sample
/// `⌈(s − fs) / rate⌉`, the first sample at or after it. The cut set is the
/// union of those over every render segment of every partial, plus 0 and the
/// frame size, clipped to `[0, size]`.
///
/// @param partials         The set's partials with their render segments.
/// @param sourceTimeAtFrameStart  The set's clock at sample 0.
/// @param sourceRate       Source time per receiver sample. Positive.
/// @param size             Frame size. Positive.
/// @return The cuts, strictly increasing, from 0 to `size`.
std::vector<SampleIndex> chunkCuts(std::span<const PartialPlan> partials,
    SampleTime sourceTimeAtFrameStart, double sourceRate, FrameSize size);

/// Prepares a partial set for a frame: the source clock, every partial's
/// render segments, and the chunk cuts (docs/rendering.md section 6.4, steps
/// 1 to 3). The buffer, step 4, is the caller's.
///
/// @param partialSetId  The set.
/// @param partials      The set's partials in scope for the frame, validated.
///                      Every one must belong to `partialSetId`; not checked.
/// @param frame         The frame.
/// @param size          Frame size. Positive.
/// @return The plan.
/// @throws std::invalid_argument  As buildRenderSegments().
PartialSetPlan planPartialSet(std::int64_t partialSetId, std::span<const PartialData> partials,
    FrameNumber frame, FrameSize size);

/// Renders a partial set into its buffer, chunk by chunk (docs/rendering.md
/// sections 6.4 and 7.2).
///
/// For each chunk, every partial whose render segment covering the chunk's
/// first sample is present is anchored from the closed form at that sample,
/// and the kernel adds the chunk's samples into `out`. No accumulated value
/// survives a chunk boundary.
///
/// @param plan     The set's plan, from planPartialSet().
/// @param rollOff  The roll-off's band edges for the run.
/// @param[in,out] out  The set's buffer, exactly one frame long. The set's
///                     samples are added into it; the caller zeroes it first.
/// @pre `out.size()` equals the frame size the plan was built for, and the
///      plan's last cut equals it. Not checked.
void renderPartialSet(
    const PartialSetPlan& plan, const RollOff& rollOff, std::span<SampleValue> out);

} // namespace rainbohz::rendering_engine
