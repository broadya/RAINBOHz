#include "rainbohz/rendering_engine/frame_plan.hpp"

#include "rainbohz/rendering_engine/render_kernel.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

/// @file
/// @brief The preparation layer of rendering and the per-set render driver.
///
/// Implements docs/rendering.md sections 6.4 and 7.2: the merge of boundaries
/// into render segments, their re-basing to their own start, the chunk cuts
/// of a partial set, and the driver that anchors every active partial at each
/// chunk and calls the kernel. Nothing here touches engine-db.
///
/// @see frame_plan.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Whether `t` lies strictly inside `(from, to)`.
///
/// @param t     A time.
/// @param from  The open lower bound.
/// @param to    The open upper bound.
/// @return True when `from < t < to`.
constexpr bool strictlyInside(SampleTime t, SampleTime from, SampleTime to) noexcept {
    return from < t && t < to;
}

/// The boundaries of a partial's render segments in a frame, sorted and
/// distinct (§6.4.1).
///
/// @param partial  The partial's data.
/// @param fs       The frame's start.
/// @param fe       The frame's end.
/// @return `fs`, `fe`, the lifeline's ends where they fall inside the frame,
///         and every integrable segment boundary, amplitude point time and
///         compensation segment boundary strictly inside it.
std::vector<SampleTime> renderBoundaries(const PartialData& partial, SampleTime fs, SampleTime fe) {
    std::vector<SampleTime> bounds{fs, fe};
    const auto add = [&](SampleTime t) {
        if (strictlyInside(t, fs, fe)) {
            bounds.push_back(t);
        }
    };
    add(partial.segments.front().startTime);
    add(partial.segments.back().endTime);
    for (const Segment& segment : partial.segments) {
        add(segment.startTime);
        add(segment.endTime);
    }
    for (const AmplitudePoint& point : partial.amplitude) {
        add(point.time);
    }
    for (const CompensationSegment& segment : partial.compensation) {
        add(segment.startTime);
        add(segment.endTime);
    }
    std::ranges::sort(bounds);
    const auto duplicates = std::ranges::unique(bounds);
    bounds.erase(duplicates.begin(), duplicates.end());
    return bounds;
}

/// The integrable segment covering `start`, advancing a forward-only cursor.
///
/// @param segments      The partial's segments, in time order.
/// @param start         A render segment's start.
/// @param[in,out] cursor  Index of the last covering segment; moves forward.
/// @return The segment with `startTime ≤ start < endTime`.
/// @throws std::invalid_argument  If there is none.
const Segment& coveringSegment(
    std::span<const Segment> segments, SampleTime start, std::size_t& cursor) {
    while (cursor + 1 < segments.size() && segments[cursor].endTime <= start) {
        ++cursor;
    }
    const Segment& covering = segments[cursor];
    if (!(covering.startTime <= start && start < covering.endTime)) {
        throw std::invalid_argument{
            "buildRenderSegments: no integrable segment covers a present render segment"};
    }
    return covering;
}

/// The amplitude piece covering `start`, advancing a forward-only cursor.
///
/// @param points        The partial's amplitude points, in time order.
/// @param start         A render segment's start.
/// @param[in,out] cursor  Index of the last covering piece; moves forward.
/// @return The piece's two points, `from` and `to`, with
///         `from.time ≤ start < to.time`.
/// @throws std::invalid_argument  If there is none.
std::pair<const AmplitudePoint&, const AmplitudePoint&> coveringPiece(
    std::span<const AmplitudePoint> points, SampleTime start, std::size_t& cursor) {
    while (cursor + 2 < points.size() && points[cursor + 1].time <= start) {
        ++cursor;
    }
    if (cursor + 1 >= points.size() ||
        !(points[cursor].time <= start && start < points[cursor + 1].time)) {
        throw std::invalid_argument{
            "buildRenderSegments: no amplitude piece covers a present render segment"};
    }
    return {points[cursor], points[cursor + 1]};
}

/// The compensation segment covering `start`, advancing a forward-only
/// cursor.
///
/// @param segments      The partial's compensation segments, in time order.
/// @param start         A render segment's start.
/// @param[in,out] cursor  Index of the last covering segment; moves forward.
/// @return The segment with `startTime ≤ start < endTime`.
/// @throws std::invalid_argument  If there is none.
const CompensationSegment& coveringCompensation(
    std::span<const CompensationSegment> segments, SampleTime start, std::size_t& cursor) {
    while (cursor + 1 < segments.size() && segments[cursor].endTime <= start) {
        ++cursor;
    }
    if (segments.empty() ||
        !(segments[cursor].startTime <= start && start < segments[cursor].endTime)) {
        throw std::invalid_argument{
            "buildRenderSegments: no compensation segment covers a present render segment"};
    }
    return segments[cursor];
}

/// The piece of a spatial timeline covering `start`, as a value and gradient
/// re-based to `start`, advancing a forward-only cursor.
///
/// Before the first point, or from the last point on, the value is held at
/// that point's with a zero gradient (frame_plan.hpp, buildSpatialSegments).
///
/// @param points        A timeline's points, in time order. Not empty.
/// @param start         A spatial segment's start.
/// @param[in,out] cursor  Index of the last point at or before `start`, or 0;
///                      moves forward.
/// @param valueOf       Reads a point's value.
/// @param gradientOf    The gradient of the piece between two points.
/// @param valueAt       The value `tau` along a piece from a start value and
///                      gradient.
/// @return The value at `start` and the gradient that holds from there.
template <typename Point, typename ValueOf, typename GradientOf, typename ValueAt>
auto coveringSpatialPiece(std::span<const Point> points, SampleTime start, std::size_t& cursor,
    ValueOf valueOf, GradientOf gradientOf, ValueAt valueAt) {
    while (cursor + 1 < points.size() && points[cursor + 1].time <= start) {
        ++cursor;
    }
    const Point& from = points[cursor];
    using Gradient = decltype(gradientOf(from, from));
    if (start < from.time || cursor + 1 == points.size()) {
        return std::pair{valueOf(from), Gradient{}};
    }
    const Gradient gradient = gradientOf(from, points[cursor + 1]);
    return std::pair{valueAt(valueOf(from), gradient, start - from.time), gradient};
}

} // namespace

std::vector<RenderSegment> buildRenderSegments(
    const PartialData& partial, FrameNumber frame, FrameSize size) {
    const SampleTime fs = frameStart(frame, size);
    const SampleTime fe = fs + SampleDuration{static_cast<double>(size.value())};
    const SampleTime lifelineStart = partial.segments.front().startTime;
    const SampleTime lifelineEnd = partial.segments.back().endTime;
    const std::vector<SampleTime> bounds = renderBoundaries(partial, fs, fe);

    std::vector<RenderSegment> result;
    result.reserve(bounds.size() - 1);

    // The cursors only move forward: the boundaries are sorted, and so are
    // the segments, the amplitude points and the compensation segments.
    std::size_t segment = 0;
    std::size_t piece = 0;
    std::size_t ramp = 0;
    for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
        RenderSegment render{.start = bounds[i],
            .end = bounds[i + 1],
            .present = lifelineStart <= bounds[i] && bounds[i] < lifelineEnd,
            .startPhase = Phase{0.0},
            .nominal = {},
            .amplitude = {},
            .compensation = {}};
        if (!render.present) {
            result.push_back(render);
            continue;
        }
        const Segment& covering = coveringSegment(partial.segments, render.start, segment);
        const auto [from, to] = coveringPiece(partial.amplitude, render.start, piece);
        const CompensationSegment& compensation =
            coveringCompensation(partial.compensation, render.start, ramp);

        // Re-base to the render segment's own start (§6.4.1). φ_s comes from
        // the same phaseAt on the same stored values as precomputation uses,
        // so it is bit-identical to what a boundary there would have stored.
        const SampleDuration tau = render.start - covering.startTime;
        render.startPhase = phaseAt(covering.startPhase, covering.nominal, tau);
        render.nominal = NominalFrequency{
            .startFrequency = frequencyAt(covering.nominal, tau),
            .linearGradient = covering.nominal.linearGradient,
        };
        const DecibelGradient ampGradient = (to.amplitude - from.amplitude) / (to.time - from.time);
        render.amplitude = NominalAmplitude{
            .startDecibels = from.amplitude + ampGradient * (render.start - from.time),
            .gradient = ampGradient,
        };
        render.compensation = compensationFrom(compensation, render.start);
        result.push_back(render);
    }
    return result;
}

std::vector<SampleIndex> chunkCuts(std::span<const PartialPlan> partials,
    SampleTime sourceTimeAtFrameStart, double sourceRate, FrameSize size) {
    std::vector<std::int32_t> cuts{0, size.value()};
    for (const PartialPlan& partial : partials) {
        for (const RenderSegment& segment : partial.segments) {
            // ⌈(s − fs) / rate⌉, taken on the small difference rather than on
            // the absolute time (§10). The first sample at or after `s`.
            const double offset = (segment.start - sourceTimeAtFrameStart).value() / sourceRate;
            const double sample =
                std::clamp(std::ceil(offset), 0.0, static_cast<double>(size.value()));
            cuts.push_back(static_cast<std::int32_t>(sample));
        }
    }
    std::ranges::sort(cuts);
    const auto duplicates = std::ranges::unique(cuts);
    cuts.erase(duplicates.begin(), duplicates.end());

    std::vector<SampleIndex> result;
    result.reserve(cuts.size());
    for (const std::int32_t cut : cuts) {
        result.emplace_back(cut);
    }
    return result;
}

PartialSetPlan planPartialSet(std::int64_t partialSetId, std::span<const PartialData> partials,
    FrameNumber frame, FrameSize size) {
    PartialSetPlan plan{
        .partialSetId = partialSetId,
        // Step 1: the source clock. The identity map in this version; the
        // binary search over the set's distance path, and the rate from its
        // gradient, go here when Doppler arrives.
        .sourceTimeAtFrameStart = frameStart(frame, size),
        .sourceRate = 1.0,
        .partials = {},
        .cuts = {},
    };

    // Step 2: every partial's render segments.
    plan.partials.reserve(partials.size());
    for (const PartialData& partial : partials) {
        plan.partials.push_back(PartialPlan{
            .partialId = partial.partialId,
            .segments = buildRenderSegments(partial, frame, size),
        });
    }

    // Step 3: the chunk boundaries.
    plan.cuts = chunkCuts(plan.partials, plan.sourceTimeAtFrameStart, plan.sourceRate, size);
    return plan;
}

void renderPartialSet(
    const PartialSetPlan& plan, const RollOff& rollOff, std::span<SampleValue> out) {
    BlockState block{plan.partials.size()};
    std::vector<std::size_t> cursor(plan.partials.size(), 0);
    std::vector<std::size_t> active;
    active.reserve(plan.partials.size());
    const RollOffRaw edges = rollOff.raw();

    for (std::size_t c = 0; c + 1 < plan.cuts.size(); ++c) {
        const SampleIndex first = plan.cuts[c];
        const SampleIndex last = plan.cuts[c + 1];
        const SampleTime at = sourceTimeAt(plan, first);

        // Anchor every partial present at the chunk's first sample (§7.2).
        active.clear();
        for (std::size_t i = 0; i < plan.partials.size(); ++i) {
            const std::vector<RenderSegment>& segments = plan.partials[i].segments;
            std::size_t& cur = cursor[i];
            while (cur + 1 < segments.size() && segments[cur].end <= at) {
                ++cur;
            }
            const RenderSegment& segment = segments[cur];
            if (!segment.present) {
                continue;
            }
            anchorPartial(block, i,
                Anchor{.startPhase = segment.startPhase,
                    .nominal = segment.nominal,
                    .amplitude = segment.amplitude,
                    .compensation = segment.compensation,
                    .tau = at - segment.start});
            active.push_back(i);
        }

        renderChunk(block, active, first, last, edges, out);
    }
}

std::vector<SpatialSegment> buildSpatialSegments(
    const SourceData& source, FrameNumber frame, FrameSize size) {
    const SampleTime fs = frameStart(frame, size);
    const SampleTime fe = fs + SampleDuration{static_cast<double>(size.value())};
    std::vector<SampleTime> bounds{fs, fe};
    const auto add = [&](SampleTime t) {
        if (strictlyInside(t, fs, fe)) {
            bounds.push_back(t);
        }
    };
    for (const AzimuthPoint& point : source.azimuth) {
        add(point.time);
    }
    for (const ElevationPoint& point : source.elevation) {
        add(point.time);
    }
    for (const DistancePoint& point : source.distance) {
        add(point.time);
    }
    std::ranges::sort(bounds);
    const auto duplicates = std::ranges::unique(bounds);
    bounds.erase(duplicates.begin(), duplicates.end());

    std::vector<SpatialSegment> result;
    result.reserve(bounds.size() - 1);
    std::size_t azimuthCursor = 0;
    std::size_t elevationCursor = 0;
    std::size_t distanceCursor = 0;
    for (std::size_t i = 0; i + 1 < bounds.size(); ++i) {
        const SampleTime start = bounds[i];
        const auto [azimuth, azimuthSlope] = coveringSpatialPiece(
            std::span<const AzimuthPoint>{source.azimuth}, start, azimuthCursor,
            [](const AzimuthPoint& p) { return p.azimuth; },
            [](const AzimuthPoint& a, const AzimuthPoint& b) {
                return azimuthGradient(a.azimuth, b.azimuth, b.time - a.time);
            },
            azimuthAt);
        const auto [elevation, elevationSlope] = coveringSpatialPiece(
            std::span<const ElevationPoint>{source.elevation}, start, elevationCursor,
            [](const ElevationPoint& p) { return p.elevation; },
            [](const ElevationPoint& a, const ElevationPoint& b) {
                return elevationGradient(a.elevation, b.elevation, b.time - a.time);
            },
            elevationAt);
        const auto [distance, distanceSlope] = coveringSpatialPiece(
            std::span<const DistancePoint>{source.distance}, start, distanceCursor,
            [](const DistancePoint& p) { return p.distance; },
            [](const DistancePoint& a, const DistancePoint& b) {
                return distanceGradient(a.distance, b.distance, b.time - a.time);
            },
            distanceAt);
        result.push_back(SpatialSegment{.start = start,
            .end = bounds[i + 1],
            .position = NominalPosition{.startAzimuth = azimuth,
                .azimuthGradient = azimuthSlope,
                .startElevation = elevation,
                .elevationGradient = elevationSlope,
                .startDistance = distance,
                .distanceGradient = distanceSlope}});
    }
    return result;
}

SoundSourcePlan planSoundSource(const SourceData& source, FrameNumber frame, FrameSize size) {
    SoundSourcePlan plan{
        .soundSourceId = source.soundSourceId,
        // The source clock: the identity map in this version (§6.4, step 1).
        .sourceTimeAtFrameStart = frameStart(frame, size),
        .sourceRate = 1.0,
        .spatial = buildSpatialSegments(source, frame, size),
        .cuts = {},
    };
    std::vector<std::int32_t> cuts{0, size.value()};
    for (const SpatialSegment& segment : plan.spatial) {
        const double offset =
            (segment.start - plan.sourceTimeAtFrameStart).value() / plan.sourceRate;
        cuts.push_back(static_cast<std::int32_t>(
            std::clamp(std::ceil(offset), 0.0, static_cast<double>(size.value()))));
    }
    std::ranges::sort(cuts);
    const auto duplicates = std::ranges::unique(cuts);
    cuts.erase(duplicates.begin(), duplicates.end());
    plan.cuts.reserve(cuts.size());
    for (const std::int32_t cut : cuts) {
        plan.cuts.emplace_back(cut);
    }
    return plan;
}

void spatialiseSource(const SoundSourcePlan& plan, ChannelMode mode,
    std::span<const SampleValue> mono, std::span<SampleValue> interleaved) {
    std::size_t cursor = 0;
    for (std::size_t c = 0; c + 1 < plan.cuts.size(); ++c) {
        const SampleIndex first = plan.cuts[c];
        const SampleTime at = plan.sourceTimeAtFrameStart +
                              SampleDuration{static_cast<double>(first.value()) * plan.sourceRate};
        while (cursor + 1 < plan.spatial.size() && plan.spatial[cursor].end <= at) {
            ++cursor;
        }
        const SpatialSegment& segment = plan.spatial[cursor];
        spatialiseChunk(SpatialAnchor{.position = segment.position, .tau = at - segment.start},
            mode, mono, first, plan.cuts[c + 1], interleaved);
    }
}

} // namespace rainbohz::rendering_engine
