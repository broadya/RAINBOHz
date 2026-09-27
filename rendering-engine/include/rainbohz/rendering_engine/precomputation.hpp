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
//   4. integrate through the segments with a phase accumulator, then write
//      the result in one transaction.
//
// Stages 2 and 3, integration and frame scope are pure functions over
// in-memory data and are declared here so that they can be tested without a
// database. Reading and writing happen only inside precomputeAll().
//
// This version handles the core frequency envelope only: linear curves, no
// modulators, no phase control, no spatial position (§2). It also reads each
// partial's amplitude timeline, for its times only, so that it can write the
// FrameEntry nodes rendering starts from (§6.7).

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/segment_maths.hpp"
#include "rainbohz/rendering_engine/units.hpp"

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

/// The value of FrequencyTimelinePoint.frequency_curve_type for a linear
/// curve: the only one this version supports.
inline constexpr std::int64_t kLinearCurve = 0;

/// A FrequencyTimelinePoint exactly as read from engine-db, before any
/// validation. Every property is nullable in the schema, so every one is
/// optional here.
struct FrequencyPointRecord {
    /// FrequencyTimelinePoint.id.
    std::int64_t id = 0;

    /// FrequencyTimelinePoint.time, samples.
    std::optional<double> time;

    /// FrequencyTimelinePoint.frequency, radians per sample.
    std::optional<double> frequency;

    /// FrequencyTimelinePoint.frequency_curve_type. Describes the curve from
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

/// An AmplitudeTimelinePoint as read for precomputation, before validation.
///
/// Only its time is read here (§4): the amplitude and curve values are
/// rendering's concern. The list is validated whole by precomputation because
/// this is where lists are walked, and because the frame entries of §6.7 lead
/// into it.
struct AmplitudePointRecord {
    /// AmplitudeTimelinePoint.id.
    std::int64_t id = 0;

    /// AmplitudeTimelinePoint.time, samples.
    std::optional<double> time;
};

/// Where the rendering of one frame starts for one partial: the in-memory
/// form of a FrameEntry node and its FRAME_ENTRY_TIMELINE edges (§6.7).
struct FrameEntryTarget {
    /// The frame.
    FrameNumber frame{0};

    /// Index, into the partial's segments, of the segment covering
    /// `max(b₀, k·F)`: the entry segment.
    std::size_t segment = 0;

    /// Index, into the partial's amplitude points, of the last point at or
    /// before that time: the start of the amplitude piece covering it.
    std::size_t point = 0;
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
///              "FrequencyTimeline".
/// @return The node ids in list order, head first. Empty when `head` is
///         nullopt.
/// @throws PartialPrecomputationError  If the list returns to a node it has
///         already visited.
std::vector<std::int64_t> walkChain(std::optional<std::int64_t> head,
    const std::unordered_map<std::int64_t, std::int64_t>& next, std::string_view what);

/// Checks a partial's frequency points and converts them to typed values
/// (§7).
///
/// @param points  The points of the partial's FrequencyTimeline, in
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
/// @param points    The points of the partial's AmplitudeTimeline, in
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
/// The accumulator starts at 0 at the first segment's start. Each segment's
/// startPhase is the accumulator's value at its start; the phase it gains is
/// phaseIncrement() over its length, from its own stored parameters.
///
/// @param[in,out] segments  Contiguous segments in time order. Each one's
///                          startPhase is overwritten.
/// @return The unwrapped phase at the end of the last segment, which is not
///         stored anywhere; 0 when `segments` is empty.
/// @throws PartialPrecomputationError  If the phase stops being finite. The
///         segments before the one that overflowed have been updated.
Phase integrate(std::span<Segment> segments);

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
/// `start_time ≤ t*ₖ < end_time`; the entry point is the last amplitude point
/// with `time ≤ t*ₖ`. Both exist for every frame in scope, because
/// `t*ₖ ∈ [b₀, b_last)` there and the amplitude timeline starts and ends at
/// `b₀` and `b_last`.
///
/// One forward pass over each list, advancing as the frame starts increase:
/// the cost is the length of the lists plus the number of frames, not their
/// product.
///
/// @param segments        The partial's segments, contiguous, in time order,
///                        covering `[b₀, b_last)`. Not empty.
/// @param amplitudeTimes  The partial's amplitude point times, strictly
///                        increasing, from `b₀` to `b_last`. At least 2.
/// @param frames          The frames in scope, from frameScope() over the
///                        same lifeline.
/// @param size            Frame size. Positive.
/// @return One target per frame from `frames.first` to `frames.last`, in
///         frame order.
/// @pre The lists satisfy the conditions above; `frames` was computed from
///      them. Not checked.
std::vector<FrameEntryTarget> frameEntries(std::span<const Segment> segments,
    std::span<const SampleTime> amplitudeTimes, FrameRange frames, FrameSize size);

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

    /// The frames it is in scope for; nullopt on failure.
    std::optional<FrameRange> frames;

    /// How many FrameEntry nodes were written: one per frame in scope. Zero
    /// on failure.
    std::size_t entryCount = 0;

    /// True when the partial was precomputed and written.
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
    bool ok() const noexcept { return failureCount() == 0; }
};

/// Precomputes every partial in `db` (§6.1).
///
/// Reads the EngineConfig and every partial's inputs, then precomputes the
/// partials one at a time, in Partial.id order, each in its own transaction.
/// Any output a partial already has -- its PartialSegmentTimeline, segments,
/// their nominal frequencies, its PARTIAL_IN_FRAME edges and its FrameEntry
/// nodes -- is deleted and rebuilt. Frame nodes are created as needed and
/// never deleted.
///
/// A partial that fails is recorded in the report and the run carries on.
/// Its transaction is rolled back, so any output from an earlier run is left
/// as it was.
///
/// @param[in,out] db  Database holding the score. Modified: every partial
///                    that succeeds has its output replaced, and missing
///                    Frame nodes are created.
/// @return One outcome per partial.
/// @throws PrecomputationError  If there is not exactly one EngineConfig, or
///         its frame size is unusable. Nothing has been written.
/// @throws EngineDbError  If reading the inputs fails. Nothing has been
///         written. A database failure while writing one partial is recorded
///         against that partial instead.
PrecomputationReport precomputeAll(EngineDb& db);

} // namespace rainbohz::rendering_engine
