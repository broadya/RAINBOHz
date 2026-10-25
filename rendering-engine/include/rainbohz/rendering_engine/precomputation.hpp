// SPDX-License-Identifier: Apache-2.0

#pragma once

// Precomputation: for every partial, the chain of integrable segments that
// makes its phase randomly accessible, and the frames it is in scope for.
//
// The design is rendering-engine/docs/precomputation.md; section numbers
// below refer to it. In outline, each partial goes through four stages:
//
//   1. read its inputs into memory;
//   2. gather boundaries -- every time at which an input changes;
//   3. build one segment between each pair of consecutive boundaries;
//   4. integrate through the segments with a phase accumulator, from the
//      initial phase its phase timeline gives;
//   5. build the phase compensation segments that make the phase meet every
//      target on its phase timeline (§6.8), then write the result in one
//      transaction.
//
// Stages 2 to 5 and frame scope are pure functions over in-memory data and
// are declared here so that they can be tested without a database. Reading
// and writing happen only inside precomputeAll().
//
// This version handles the core frequency envelope and phase control: linear
// curves, no modulators, no spatial position (§2). It also reads each
// partial's amplitude timeline, for its times only, so that it can write the
// FrameEntry nodes rendering starts from (§6.7).

#include "rainbohz/rendering_engine/segment_maths.hpp"
#include "rainbohz/rendering_engine/store_types.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rainbohz::rendering_engine {

class PrecomputationStore;

/// Thrown when precomputation cannot run at all: the configuration it needs
/// for every partial is missing or unusable. Nothing has been written.
class PrecomputationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when one partial cannot be precomputed. precomputeAll() catches it,
/// records it against the partial, and carries on with the next.
///
/// The message says what is wrong in terms of the input, naming the point
/// involved where there is one; it does not name the partial, which the
/// caller knows.
class PartialPrecomputationError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// The value of FrequencyPhysicalTimelinePoint.frequency_curve_type for a linear
/// curve: the only one this version supports.
inline constexpr std::int64_t kLinearCurve = 0;

/// A FrequencyPhysicalTimelinePoint exactly as read from engine-db, before any
/// validation. Every property is nullable in the schema, so every one is
/// optional here.
struct FrequencyPointRecord {
    /// FrequencyPhysicalTimelinePoint.id.
    std::int64_t id = 0;

    /// FrequencyPhysicalTimelinePoint.time, samples.
    std::optional<double> time;

    /// FrequencyPhysicalTimelinePoint.frequency, radians per sample.
    std::optional<double> frequency;

    /// FrequencyPhysicalTimelinePoint.frequency_curve_type. Describes the curve from
    /// this point to the next; ignored on the last point.
    std::optional<std::int64_t> curveType;
};

/// A validated point of a partial's frequency envelope.
struct FrequencyPoint {
    /// When the envelope passes through this point.
    SampleTime time;

    /// The envelope's value at that time.
    AngularFrequency frequency;
};

/// An AmplitudePhysicalTimelinePoint as read for precomputation, before validation.
///
/// Only its time is read here (§4): the amplitude and curve values are
/// rendering's concern. The list is validated whole by precomputation because
/// this is where lists are walked, and because the frame entries of §6.7 lead
/// into it.
struct AmplitudePointRecord {
    /// AmplitudePhysicalTimelinePoint.id.
    std::int64_t id = 0;

    /// AmplitudePhysicalTimelinePoint.time, samples.
    std::optional<double> time;
};

/// A PhasePhysicalTimelinePoint exactly as read from engine-db, before any
/// validation.
struct PhasePointRecord {
    /// PhasePhysicalTimelinePoint.id.
    std::int64_t id = 0;

    /// PhasePhysicalTimelinePoint.time, samples.
    std::optional<double> time;

    /// PhasePhysicalTimelinePoint.phase, wrapped radians. NULL is meaningful:
    /// the partial runs free up to this point (§6.8).
    std::optional<double> phase;
};

/// The three spatial timelines of a SoundSource (§7). Each is one
/// `<Kind>PhysicalTimeline` with `<Kind>PhysicalTimelinePoint`s, whose value
/// column is `azimuth`, `elevation` or `distance`.
enum class SpatialKind : std::uint8_t {
    Azimuth,   ///< radians, [−π, π]
    Elevation, ///< radians, [−π/2, π/2]
    Distance,  ///< metres, [1, 16384]
};

/// The three kinds, in the order the schema declares them.
inline constexpr std::array<SpatialKind, 3> kSpatialKinds{
    SpatialKind::Azimuth, SpatialKind::Elevation, SpatialKind::Distance};

/// The timeline table of a kind, e.g. "AzimuthPhysicalTimeline".
///
/// @param kind  The kind.
/// @return The table name; static storage.
std::string_view spatialTimelineName(SpatialKind kind) noexcept;

/// The value column of a kind's points, e.g. "azimuth".
///
/// @param kind  The kind.
/// @return The column name; static storage.
std::string_view spatialValueName(SpatialKind kind) noexcept;

/// Whether a spatial value is one the model accepts, and why not.
///
/// The ranges are the schema's: azimuth [−π, π] and elevation [−π/2, π/2]
/// radians, distance [1, 16384] metres. Shared by precomputation and
/// rendering, which report the problem in their own errors.
///
/// @param kind   Which value it is.
/// @param value  The value, as read.
/// @return nullopt when `value` is finite and in range; otherwise the reason,
///         such as "azimuth 4 is outside [-π, π]".
std::optional<std::string> spatialValueProblem(SpatialKind kind, double value);

/// A point of a SoundSource's azimuth, elevation or distance timeline exactly
/// as read from engine-db, before any validation.
struct SpatialPointRecord {
    /// The point's id.
    std::int64_t id = 0;

    /// The point's time, samples.
    std::optional<double> time;

    /// Its azimuth, elevation or distance.
    std::optional<double> value;

    /// Its `<kind>_curve_type`: the curve to the next point.
    std::optional<std::int64_t> curveType;
};

/// A validated point of a partial's phase timeline: a target the rendered
/// phase must meet, or a point up to which it runs free.
struct PhaseTarget {
    /// When the target applies.
    SampleTime time;

    /// The phase to meet at `time`, wrapped radians in [-π, π]; nullopt for
    /// "free-running up to here". Not "no envelope node here": a NULL point still
    /// starts a new compensation segment.
    std::optional<Phase> phase;
};

/// One phase compensation segment: the in-memory form of a
/// PartialPhaseCompensationSegment (§6.8).
///
/// Covers `[startTime, endTime)`. Over it, the compensation added to the
/// phase from the frequency envelope ramps linearly from `startDelta` to
/// `endDelta`. `startDelta` is in [-π, π]; `endDelta` is `startDelta` plus
/// the ramp's whole rise, which is not wrapped, so it may lie outside.
struct CompensationSegment {
    /// PartialPhaseCompensationSegment.start_time.
    SampleTime startTime;

    /// PartialPhaseCompensationSegment.end_time: the next segment's
    /// startTime.
    SampleTime endTime;

    /// PartialPhaseCompensationSegment.start_phase_delta, radians.
    Phase startDelta;

    /// PartialPhaseCompensationSegment.end_phase_delta, radians.
    Phase endDelta;
};

/// The compensation ramp of `segment`, re-based to `at` (§6.8).
///
/// The delta at `at` is interpolated from the stored start and end, so that it
/// is exactly `startDelta` at the segment's start; the slope is
/// `(endDelta − startDelta) / (endTime − startTime)`.
///
/// @param segment  A compensation segment; `endTime > startTime`.
/// @param at       A time within it.
/// @return The ramp from `at` onward.
NominalCompensation compensationFrom(const CompensationSegment& segment, SampleTime at) noexcept;

/// Where the rendering of one frame starts for one partial: the in-memory
/// form of a FrameEntry node and its FRAME_ENTRY_PHYSICAL_TIMELINE edges (§6.7).
struct FrameEntryTarget {
    /// The frame.
    FrameNumber frame{0};

    /// Index, into the partial's segments, of the segment covering
    /// `max(b₀, k·F)`: the entry segment.
    std::size_t segment = 0;

    /// Index, into the partial's amplitude points, of the last point at or
    /// before that time: the start of the amplitude piece covering it.
    std::size_t point = 0;

    /// Index, into the partial's compensation segments, of the one covering
    /// the same time.
    std::size_t compensation = 0;

    /// Index, into the points of the partial's SoundSource's azimuth,
    /// elevation and distance timelines (kSpatialKinds order), of the last
    /// point at or before the frame's start `k·F`, or of the first point when
    /// the timeline starts inside the frame. Unlike the others it does not
    /// depend on the partial, so every partial of a source has the same.
    std::array<std::size_t, 3> spatial{};
};

/// One integrable segment: the in-memory form of a PartialIntegrableSegment
/// and its PartialSegmentNominalFrequency.
///
/// Covers `[startTime, endTime)`.
struct Segment {
    /// PartialIntegrableSegment.start_time.
    SampleTime startTime;

    /// PartialIntegrableSegment.end_time: the next segment's startTime.
    SampleTime endTime;

    /// The segment's nominal frequency path.
    NominalFrequency nominal;

    /// PartialIntegrableSegment.start_phase: unwrapped phase at startTime.
    /// Zero until integrate() has run.
    Phase startPhase;
};

/// An inclusive range of frames.
struct FrameRange {
    /// The first frame in the range.
    FrameNumber first;

    /// The last frame in the range; not before `first`.
    FrameNumber last;
};

/// Follows a NEXT_POINT linked list from its head.
///
/// The list is walked in C++ rather than by one variable-length Cypher
/// pattern, because Ladybug refuses variable-length patterns longer than 30
/// hops and a timeline can be longer than that (§10).
///
/// Lists are acyclic by construction upstream, not by a database constraint:
/// NEXT_POINT's ONE_ONE is counted per FROM/TO pair, so a list's head can
/// also be the successor of its last node. The cycle check is what stops such
/// a list from being walked forever. A list merged into another -- a head
/// that is the successor of another list's point -- is also an error, but is
/// not detected here: it walks as an ordinary list (§7).
///
/// @param head  Id of the first node, or nullopt for an empty list.
/// @param next  For each node id, the id of the node its NEXT_POINT leads to.
///              Nodes absent from it end the list.
/// @param what  What the list is, for the error message, e.g.
///              "FrequencyPhysicalTimeline".
/// @return The node ids in list order, head first. Empty when `head` is
///         nullopt.
/// @throws PartialPrecomputationError  If the list returns to a node it has
///         already visited.
std::vector<std::int64_t> walkChain(std::optional<std::int64_t> head,
    const std::unordered_map<std::int64_t, std::int64_t>& next, std::string_view what);

/// Checks a partial's frequency points and converts them to typed values
/// (§7).
///
/// @param points  The points of the partial's FrequencyPhysicalTimeline, in
///                NEXT_POINT order.
/// @return The envelope: one FrequencyPoint per input point, in order.
/// @throws PartialPrecomputationError  If there are fewer than 2 points; a
///         time or frequency is NULL or not finite; the first time is before
///         0; a time is not strictly later than the one before it; or any
///         point but the last has a curve type other than linear (including
///         NULL). The message names the point by its position in the list,
///         counting from 0.
std::vector<FrequencyPoint> validateFrequencyEnvelope(std::span<const FrequencyPointRecord> points);

/// Checks a partial's amplitude points against its frequency envelope, and
/// converts their times to typed values (§7).
///
/// The two timelines define the lifeline together: the first amplitude
/// point's time must equal the first frequency point's, and the last the
/// last, exactly (author, 2026-09-24). Interior points are independent.
///
/// @param points    The points of the partial's AmplitudePhysicalTimeline, in
///                  NEXT_POINT order.
/// @param envelope  The partial's validated frequency envelope.
/// @return One time per input point, in order, strictly increasing.
/// @throws PartialPrecomputationError  If there are fewer than 2 points; a
///         time is NULL or not finite; a time is not strictly later than the
///         one before it; or the first or last time differs from the
///         envelope's. The message names the point by its position in the
///         list, counting from 0.
std::vector<SampleTime> validateAmplitudeTimes(
    std::span<const AmplitudePointRecord> points, std::span<const FrequencyPoint> envelope);

/// Checks one of a SoundSource's spatial timelines on its own, without
/// reference to any partial (§7).
///
/// @param kind    Which timeline it is.
/// @param points  Its points, in NEXT_POINT order.
/// @return One time per point, in order, strictly increasing.
/// @throws PartialPrecomputationError  If there are no points; a time or a
///         value is NULL or not finite; a time is not strictly later than the
///         one before it; a value is out of range (spatialValueProblem()); or
///         a curve type other than the last point's is not 0 (linear). The
///         message names the timeline and the point by its position,
///         counting from 0.
std::vector<SampleTime> validateSpatialTimeline(
    SpatialKind kind, std::span<const SpatialPointRecord> points);

/// Checks that a source's spatial timeline covers a partial's lifeline: its
/// first point is at or before `b₀` (§7).
///
/// After its last point a timeline's value holds, so it need not reach
/// `b_last` (author, 2026-10-05).
///
/// @param kind           Which timeline it is.
/// @param times          Its validated times. Not empty.
/// @param lifelineStart  The partial's b₀.
/// @throws PartialPrecomputationError  If it starts after the lifeline.
void checkSpatialCoverage(
    SpatialKind kind, std::span<const SampleTime> times, SampleTime lifelineStart);

/// Checks a partial's phase points against its frequency envelope, and
/// converts them to typed values (§7, §6.8).
///
/// @param points    The points of the partial's PhasePhysicalTimeline, in
///                  NEXT_POINT order.
/// @param envelope  The partial's validated frequency envelope.
/// @return One target per input point, in order, strictly increasing in time.
/// @throws PartialPrecomputationError  If there are no points; a time is NULL
///         or not finite; the first time is not the envelope's first time
///         exactly; a time is not strictly later than the one before it; a
///         time is after the envelope's last time; or a phase that is not
///         NULL is not finite or is outside [-π, π]. The message names the
///         point by its position in the list, counting from 0.
std::vector<PhaseTarget> validatePhaseTimeline(
    std::span<const PhasePointRecord> points, std::span<const FrequencyPoint> envelope);

/// Stage 2: the sorted, distinct times at which any input changes (§6.3).
///
/// In this version the frequency envelope is the only input, so the result is
/// its point times. It is written as a merge so that frequency modulators and
/// phase regions can add their times later.
///
/// @param envelope  A validated frequency envelope.
/// @return Boundary times, strictly increasing. The first and last are the
///         envelope's first and last times: the partial's lifeline.
std::vector<SampleTime> gatherBoundaries(std::span<const FrequencyPoint> envelope);

/// Stage 3: one segment between each pair of consecutive boundaries, with its
/// nominal frequency (§6.4).
///
/// A segment continues the envelope piece that covers its start: the
/// gradient is that piece's, and the start frequency is the envelope's value
/// at the segment's start -- the point's own frequency when the boundary is
/// an envelope point.
///
/// @param boundaries  Strictly increasing, from gatherBoundaries().
/// @param envelope    The validated envelope the boundaries came from.
/// @return |boundaries| − 1 segments, contiguous, in time order. Their
///         startPhase is zero; integrate() fills it in.
/// @pre `boundaries` is strictly increasing, has at least 2 entries, and
///      starts and ends at the envelope's first and last times.
/// @throws std::invalid_argument  If the precondition does not hold.
std::vector<Segment> buildSegments(
    std::span<const SampleTime> boundaries, std::span<const FrequencyPoint> envelope);

/// Stage 4: runs the phase accumulator through the segments, setting each
/// one's start phase (§6.5).
///
/// The accumulator starts at `initialPhase` at the first segment's start:
/// 0 for the natural phase N₀ from which initialPhaseOf() works, φ₀ from it
/// for the segments that are written. Each segment's
/// startPhase is the accumulator's value at its start, wrapped to [-π, π];
/// the phase it gains is phaseIncrement() over its length, from its own
/// stored parameters. Wrapping at every start keeps phase local to the
/// segment: precision depends on a segment's frequency and length, not on
/// how long the piece has run (author, 2026-10-11).
///
/// @param[in,out] segments  Contiguous segments in time order. Each one's
///                          startPhase is overwritten.
/// @param initialPhase      The phase at the first segment's start.
/// @return The phase at the end of the last segment, wrapped to [-π, π],
///         which is not stored anywhere; `initialPhase` wrapped when
///         `segments` is empty.
/// @throws PartialPrecomputationError  If the phase stops being finite. The
///         segments before the one that overflowed have been updated.
Phase integrate(std::span<Segment> segments, Phase initialPhase = Phase{0.0});

/// The phase the frequency envelope gives at `at` (§6.8), not wrapped:
/// the covering segment's wrapped start phase plus what it gains up to `at`.
///
/// `phaseAt()` on the integrable segment covering `at`, from its stored
/// values: the same computation rendering makes there, so the two agree to
/// the last bit. At the lifeline's end, the last segment evaluated at its
/// length.
///
/// @param segments  Integrated segments, contiguous, in time order. Not empty.
/// @param at        A time in `[b₀, b_last]`.
/// @return N(at).
/// @throws std::invalid_argument  If `at` is outside the segments.
Phase naturalPhaseAt(std::span<const Segment> segments, SampleTime at);

/// The partial's initial phase φ₀ (§6.8).
///
/// The first target's phase when it is not NULL. When it is NULL, φ₀ is
/// worked backwards so that the partial, running free from `b₀`, meets the
/// first target that is not NULL exactly:
/// `φ₀ = wrapHalfOpen(θₖ − N₀(tₖ))`. With no target at all, 0.
///
/// @param fromZero  The segments, integrated from 0: N₀.
/// @param targets   The validated phase timeline. Not empty.
/// @return φ₀, in [-π, π].
Phase initialPhaseOf(std::span<const Segment> fromZero, std::span<const PhaseTarget> targets);

/// Stage 5: the phase compensation segments (§6.8).
///
/// The boundaries are `b₀`, every target time inside `(b₀, b_last)`, and
/// `b_last`. A running delta D starts at 0 and is wrapped to [-π, π] at the
/// start of every segment, never inside one. The target
/// that set φ₀ -- the first that is not NULL -- and every point before it
/// are met already, so their segments are flat at 0. After that, in time
/// order, a target θᵢ at tᵢ moves D by `wrapHalfOpen(θᵢ − (N(tᵢ) + D))`
/// and the segment ending at tᵢ ramps from the old D to the new one; a NULL
/// point leaves D alone, so the segment ending there is flat. The segment
/// after the last point, if it ends before `b_last`, is flat. Wrapping only
/// at a start keeps each ramp's rise, and so its slope, exact, while keeping
/// every value local (author, 2026-10-11); a segment's startDelta may differ
/// from the previous segment's endDelta by a multiple of 2π.
///
/// @param segments  The segments, integrated from φ₀: N. Not empty.
/// @param targets   The validated phase timeline, the first at `b₀`.
/// @return The compensation segments, contiguous, covering `[b₀, b_last)`.
///         At least one.
std::vector<CompensationSegment> buildCompensation(
    std::span<const Segment> segments, std::span<const PhaseTarget> targets);

/// The frames that a lifeline `[start, end)` overlaps (§6.6).
///
/// @param start  Start of the lifeline. Not before 0.
/// @param end    End of the lifeline, exclusive. After `start`.
/// @param size   Frame size. Positive.
/// @return Frames ⌊start / size⌋ to ⌈end / size⌉ − 1, inclusive.
/// @throws PartialPrecomputationError  If either frame number does not fit
///         INT32.
FrameRange frameScope(SampleTime start, SampleTime end, FrameSize size);

/// Where each frame's rendering starts, for every frame in scope (§6.7).
///
/// For frame *k* the entry time is `t*ₖ = max(b₀, k·F)`, the frame's first
/// instant inside the lifeline. The entry segment is the one with
/// `start_time ≤ t*ₖ < end_time`, and so is the entry compensation segment;
/// the entry point is the last amplitude point with `time ≤ t*ₖ`. Both exist for every frame in
/// scope, because `t*ₖ ∈ [b₀, b_last)` there and the amplitude timeline starts and ends at `b₀` and
/// `b_last`. The spatial entry points are the last point at or before the
/// frame's start `k·F` (not `t*ₖ`), or the first point when the timeline
/// starts inside the frame; they depend on the frame and the source only.
///
/// One forward pass over each list, advancing as the frame starts increase:
/// the cost is the length of the lists plus the number of frames, not their
/// product.
///
/// @param segments        The partial's segments, contiguous, in time order,
///                        covering `[b₀, b_last)`. Not empty.
/// @param amplitudeTimes  The partial's amplitude point times, strictly
///                        increasing, from `b₀` to `b_last`. At least 2.
/// @param compensation    The partial's compensation segments, contiguous,
///                        covering `[b₀, b_last)`. Not empty.
/// @param spatialTimes    The times of the partial's SoundSource's azimuth,
///                        elevation and distance points, in kSpatialKinds
///                        order, each strictly increasing and covering the
///                        lifeline (checkSpatialCoverage()). The first entry
///                        in each is found by binary search, because the
///                        lists span the whole source rather than one
///                        partial, and the rest by a forward cursor.
/// @param frames          The frames in scope, from frameScope() over the
///                        same lifeline.
/// @param size            Frame size. Positive.
/// @return One target per frame from `frames.first` to `frames.last`, in
///         frame order.
/// @pre The lists satisfy the conditions above; `frames` was computed from
///      them. Not checked.
std::vector<FrameEntryTarget> frameEntries(std::span<const Segment> segments,
    std::span<const SampleTime> amplitudeTimes, std::span<const CompensationSegment> compensation,
    const std::array<std::span<const SampleTime>, 3>& spatialTimes, FrameRange frames,
    FrameSize size);

/// The frame size from EngineConfig: `frameSize`, or one second at
/// `sampleRate` when `frameSize` is NULL.
///
/// @param frameSize   EngineConfig.frameSize, samples; nullopt for NULL.
/// @param sampleRate  EngineConfig.sampleRate, samples per second; nullopt for
///                    NULL.
/// @return The frame size to use.
/// @throws PrecomputationError  If the value used is NULL, not positive, or
///         does not fit INT32.
FrameSize resolveFrameSize(
    std::optional<std::int64_t> frameSize, std::optional<std::int64_t> sampleRate);

/// What happened to one partial.
struct PartialOutcome {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Partial.name; nullopt when NULL.
    std::optional<std::string> partialName;

    /// Why the partial failed; nullopt when it succeeded. A failed partial
    /// has nothing written for it by this run.
    std::optional<std::string> error;

    /// How many segments were written. Zero on failure.
    std::size_t segmentCount = 0;

    /// How many phase compensation segments were written. Zero on failure.
    std::size_t compensationCount = 0;

    /// The frames it is in scope for; nullopt on failure.
    std::optional<FrameRange> frames;

    /// How many FrameEntry nodes were written: one per frame in scope. Zero
    /// on failure.
    std::size_t entryCount = 0;

    /// True when the partial was precomputed and written.
    ///
    /// @return True when `error` is nullopt.
    bool ok() const noexcept { return !error.has_value(); }
};

/// The result of a precomputation run.
struct PrecomputationReport {
    /// The frame size every partial's frame scope was computed with.
    FrameSize frameSize{0};

    /// One outcome per partial, in Partial.id order.
    std::vector<PartialOutcome> partials;

    /// How many partials failed.
    ///
    /// @return The number of entries in `partials` whose error is set.
    std::size_t failureCount() const noexcept;

    /// True when every partial succeeded, including when there are none.
    ///
    /// @return True when failureCount() is zero.
    bool ok() const noexcept { return failureCount() == 0; }
};

/// Precomputes every partial in `db` (§6.1).
///
/// Reads the EngineConfig and every partial's inputs, then precomputes the
/// partials one at a time, in Partial.id order. Each partial's output is
/// written as one unit, 64 partials per call to the store, which the Ladybug
/// backend writes in one transaction, each table's rows as one statement
/// (§10; @see ADR 0022, ADR 0028). Any output a partial already has -- its
/// PartialSegmentPhysicalTimeline, segments, their nominal frequencies, its
/// compensation segments, its PARTIAL_IN_FRAME edges and its FrameEntry
/// nodes -- is deleted and rebuilt. Frame nodes are created as needed and
/// never deleted.
///
/// A partial that fails is recorded in the report and the run carries on.
/// Nothing of it is written, so any output from an earlier run is left as it
/// was. A failure while the store writes is confined to the partial that
/// caused it.
///
/// @param[in,out] store  engine-db holding the score. Modified: every partial
///                       that succeeds has its output replaced, and missing
///                       Frame nodes are created.
/// @return One outcome per partial.
/// @throws PrecomputationError  If there is not exactly one EngineConfig, or
///         its frame size is unusable. Nothing has been written.
/// @throws EngineDbError  If reading the inputs fails, in which case nothing
///         has been written, or if the store does not answer for every unit
///         it was given. A failure while writing one partial is recorded
///         against that partial instead.
PrecomputationReport precomputeAll(PrecomputationStore& store);

} // namespace rainbohz::rendering_engine
