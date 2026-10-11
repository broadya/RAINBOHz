// SPDX-License-Identifier: Apache-2.0

#pragma once

// The transform stage: the canonical model, in musical time, becomes the
// rendering model, in physical time, in place in engine-db.
//
// This is responsibility 3 of docs/design.md section 1. The design is
// docs/transform.md; section numbers below refer to it. This first version
// converts each PartialSet's PartialSetMusicalTimeline into one
// FrequencyPhysicalTimeline, one AmplitudePhysicalTimeline and one
// PhasePhysicalTimeline per partial, and each SoundSource's
// SpatialPositionMusicalTimeline into one AzimuthPhysicalTimeline,
// ElevationPhysicalTimeline and DistancePhysicalTimeline, through one
// TempoMap per Score. Modulators and synchronised tempo maps are not yet
// converted (§2).
//
// The stage is a module of the engine library, not a separate target: the
// namespace rainbohz::rendering_engine::transform is the boundary between
// musical and physical time (docs/design.md section 7). Nothing outside it
// includes musical_units.hpp or tempo_map.hpp.
//
// Like precomputation, each partial goes through pure stages over in-memory
// data, declared here so that they can be tested without a database; reading
// and writing happen only inside transformAll().

#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/units.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace rainbohz::rendering_engine::transform {

/// Thrown when the transform cannot run at all: the configuration or a
/// TempoMap is unusable, or the score uses a feature this version does not
/// convert. Nothing has been written.
class TransformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when one PartialSet cannot be transformed: its timeline, its
/// POINT_AT edges or its states are invalid. None of its partials is
/// written; transformAll() records it and carries on with the next set.
class PartialSetTransformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when one partial cannot be transformed. transformAll() records it
/// against the partial and carries on with the next.
///
/// The message says what is wrong in terms of the input, naming the timeline
/// point involved by its position in the list, counting from 0, where there
/// is one; it does not name the partial, which the caller knows.
class PartialTransformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Thrown when one SoundSource's spatial position cannot be transformed: its
/// SpatialPositionMusicalTimeline, its POINT_AT edges or its states are
/// invalid (§6.8). Nothing is written for the source; transformAll() records
/// it and carries on with the next.
///
/// The message names the timeline point involved by its position in the
/// list, counting from 0; it does not name the source, which the caller
/// knows.
class SoundSourceTransformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// One defined value of one property of one partial: the point of the set's
/// timeline it sits on, the value, and the curve from here to the next envelope node.
///
/// A partial's envelope for a property is its envelope nodes in point order (§6.2).
/// Points where the property is NULL, or where the partial has no state, are
/// not envelope nodes.
struct EnvelopeNode {
    /// Index of the point in the set's timeline, from 0.
    std::size_t point = 0;

    /// The value in the canonical model's unit: Hz for frequency, dB for
    /// amplitude.
    double value = 0.0;

    /// The set point's curve type for this property, describing the path to
    /// the next envelope node.
    std::int64_t curveType = 0;

    /// The set point's curve amount for this property.
    double curveAmount = 0.0;
};

/// A point of a physical timeline as it will be written.
struct PhysicalPoint {
    /// When the envelope passes through this point.
    SampleTime time;

    /// The value; in the unit the caller put into the envelope nodes it came from.
    double value = 0.0;

    /// Curve type from here to the next point.
    std::int64_t curveType = 0;

    /// Curve amount from here to the next point.
    double curveAmount = 0.0;

    /// Memberwise equality: true when every field is equal.
    friend bool operator==(const PhysicalPoint&, const PhysicalPoint&) = default;
};

/// The phase fields of one PartialState of one partial, as read (§6.6).
///
/// Three combinations are valid (author, 2026-10-03): a target (`degrees`
/// set, `freeRun` false), a free-running point (`degrees` NULL, `freeRun`
/// true), and the default (both NULL), which plays no role in phase. Any
/// other combination is invalid (requireValidPhase()).
struct StatePhase {
    /// Index of the point in the set's timeline, from 0.
    std::size_t point = 0;

    /// PartialState.phase, degrees; nullopt when NULL.
    std::optional<double> degrees;

    /// PartialState.phase_free_run; nullopt when NULL.
    std::optional<bool> freeRun;
};

/// One phase point of one partial: the point of the set's timeline it sits
/// on, and the phase there (§6.6).
///
/// Unlike an EnvelopeNode, a NULL value is still a point: "free-running up to here"
/// (precomputation.md §6.8). Targets and free-running states give one; a
/// default state, or a point where the partial has no state, gives none
/// (phaseNodesOf()).
struct PhaseEnvelopeNode {
    /// Index of the point in the set's timeline, from 0.
    std::size_t point = 0;

    /// The target phase in degrees, in [−180, 180]; nullopt for a NULL
    /// (free-running) point.
    std::optional<double> degrees;

    /// Memberwise equality: true when every field is equal.
    friend bool operator==(const PhaseEnvelopeNode&, const PhaseEnvelopeNode&) = default;
};

/// A point of a PhasePhysicalTimeline as it will be written.
struct PhasePoint {
    /// When the phase must be reached.
    SampleTime time;

    /// The target phase, in [−π, π]; nullopt for a NULL (free-running) point.
    std::optional<Phase> phase;

    /// Memberwise equality: true when every field is equal.
    friend bool operator==(const PhasePoint&, const PhasePoint&) = default;
};

/// The SpatialPositionState of one point of a source's timeline, as read
/// (§6.8). A NULL property is "no envelope node here" for that property,
/// except on the first point, where all three are required.
struct SpatialState {
    /// Index of the point in the source's timeline, from 0.
    std::size_t point = 0;

    /// SpatialPositionState.azimuth, degrees; nullopt when NULL.
    std::optional<double> azimuth;

    /// SpatialPositionState.elevation, degrees; nullopt when NULL.
    std::optional<double> elevation;

    /// SpatialPositionState.distance, metres; nullopt when NULL.
    std::optional<double> distance;
};

/// The curve fields of one point of a source's timeline, validated, each
/// array in kSpatialKinds order (azimuth, elevation, distance).
struct SpatialCurves {
    /// `<kind>_curve_type`, 0 to 4.
    std::array<std::int64_t, 3> curveType{};

    /// `<kind>_curve_amount`, 0 to 1.
    std::array<double, 3> curveAmount{};
};

/// A source's three spatial envelopes, or their placed points, in
/// kSpatialKinds order (azimuth, elevation, distance).
template <typename T>
using SpatialTriple = std::array<std::vector<T>, 3>;

/// Checks that a source's position is fully defined at its timeline's first
/// point (§6.8).
///
/// Supplying a default position is the compiler's responsibility (author,
/// 2026-10-05); here a missing value is an integrity error.
///
/// @param states  The states of the source's timeline, in point order.
/// @throws SoundSourceTransformError  If point 0 has no state, or its state
///         leaves azimuth, elevation or distance NULL.
void requireCompletePosition(std::span<const SpatialState> states);

/// Checks that every value a source's states define is in the canonical
/// model's range (§6.8).
///
/// Azimuth must be finite and in [−180, 180] degrees, elevation in [−90, 90]
/// degrees, distance in [1, 16384] metres. Out-of-range values are refused,
/// not wrapped: wraparound is for modulators only (engine-schema.cypher).
///
/// @param states  The states of the source's timeline.
/// @throws SoundSourceTransformError  If a value is out of range.
void requireSpatialValues(std::span<const SpatialState> states);

/// Builds a source's three envelopes from its states (§6.8).
///
/// Each property is its own envelope: a state contributes an envelope node
/// for each property it does not leave NULL, carrying that property's curve
/// fields from its point.
///
/// @param states  The states of the source's timeline, in point order, at
///                most one per point.
/// @param curves  The curve fields of every point of the timeline, by point
///                index.
/// @return The envelope nodes of azimuth (degrees), elevation (degrees) and
///         distance (metres), each in point order.
SpatialTriple<EnvelopeNode> spatialNodesOf(
    std::span<const SpatialState> states, std::span<const SpatialCurves> curves);

/// Places a source's three envelopes in physical time and converts them to
/// the rendering model's units (§6.3, §6.8).
///
/// Each envelope is squashed on its own, as a partial's are (placeNodes()).
/// There is no lifeline to fit: past the last point the value holds.
/// Azimuth and elevation become radians (toAzimuth(), toElevation());
/// distance stays in metres.
///
/// @param envelopes   The envelopes from spatialNodesOf(), values in range,
///                    each with at least one envelope node.
/// @param pointTimes  The computed physical time of every point of the
///                    source's timeline, by point index. Every value finite.
/// @return The points of the azimuth, elevation and distance timelines.
SpatialTriple<PhysicalPoint> placeSpatialNodes(
    const SpatialTriple<EnvelopeNode>& envelopes, std::span<const SampleTime> pointTimes);

/// Checks that frequency and amplitude share the partial's first and last
/// points (§6.2).
///
/// A partial exists from the first point where any of its properties is
/// defined to the last. Both properties must be defined at those two points;
/// in between they are independent.
///
/// @param frequency  The partial's frequency envelope nodes, in point order. At least
///                   two.
/// @param amplitude  The partial's amplitude envelope nodes, in point order. At least
///                   two.
/// @throws PartialTransformError  If either property has fewer than two
///         envelope nodes, or the first or last envelope nodes are on different points.
void requireSharedEndPoints(
    std::span<const EnvelopeNode> frequency, std::span<const EnvelopeNode> amplitude);

/// Places one property's envelope nodes in physical time (§6.3).
///
/// Each envelope node's computed time is its point's time; the envelope nodes are squashed
/// (squash.hpp) so that the result is strictly increasing and never negative,
/// with the user's order kept and later states winning a collision.
///
/// @param nodes       The property's envelope nodes, in point order.
/// @param pointTimes  The computed physical time of every point of the set's
///                    timeline, by point index. Every value finite.
/// @return One point per surviving envelope node, in order, carrying that envelope node's
///         value and curve. Fewer than the envelope nodes when any collided.
std::vector<PhysicalPoint> placeNodes(
    std::span<const EnvelopeNode> nodes, std::span<const SampleTime> pointTimes);

/// Fits a placed amplitude envelope to the partial's lifeline (§6.4).
///
/// The frequency envelope defines the lifeline (author, 2026-10-02). The
/// amplitude envelope's last point is moved to the lifeline's end, and any
/// other amplitude point at or after that end is dropped, so that the two
/// timelines end together exactly, as precomputation requires.
///
/// @param[in,out] amplitude  Placed amplitude points, strictly increasing,
///                           at least two. On return, strictly increasing,
///                           at least two, ending exactly at `end`.
/// @param end                The frequency envelope's last time. Later than
///                           the first amplitude point.
void fitToLifeline(std::vector<PhysicalPoint>& amplitude, SampleTime end);

/// Checks that a partial's states lie within its lifeline (§6.6).
///
/// The partial's first and last frequency envelope nodes sit on points where it has a
/// state. A state before the first or after the last, whatever its phase,
/// has no frequency to sound at.
///
/// @param frequency  The partial's frequency envelope nodes, in point order. At least
///                   two.
/// @param states     The phase fields of every state of the partial, in
///                   point order.
/// @throws PartialTransformError  If the first or last state is not on the
///         point of the first or last frequency envelope node.
void requireStatesWithinLifeline(
    std::span<const EnvelopeNode> frequency, std::span<const StatePhase> states);

/// Checks that every state's phase fields are one of the three valid
/// combinations, and that every target is in range (§6.6).
///
/// @param states  The phase fields of every state of the partial.
/// @throws PartialTransformError  If a state has a phase with
///         phase_free_run true or NULL, or phase_free_run false with no
///         phase, or a phase that is not finite in [−180, 180] degrees.
void requireValidPhase(std::span<const StatePhase> states);

/// Selects a partial's phase points from its states (§6.6).
///
/// A target gives a point with its phase, a free-running state a NULL point,
/// and a default state nothing. Precomputation needs a point at the start of
/// the lifeline, so a default first state gives a NULL point after all: the
/// partial runs free to its first target (author, 2026-10-03). It is
/// selected here, before squashing, so that a later state colliding with it
/// wins as any later state does.
///
/// @param states  The phase fields of every state of the partial, in point
///                order, valid (requireValidPhase()) and at least one, the
///                first on the lifeline's first point.
/// @return The phase points, in point order; at least one, the first on the
///         first state's point.
std::vector<PhaseEnvelopeNode> phaseNodesOf(std::span<const StatePhase> states);

/// Places a partial's phase points in physical time and converts them to
/// radians (§6.3, §6.6).
///
/// The points are squashed on their own, exactly as envelope nodes are:
/// the user's order is kept and a later state wins a collision, including a
/// later NULL over an earlier target.
///
/// @param nodes       The phase points, in point order, each NULL or in
///                    [−180, 180] degrees.
/// @param pointTimes  The computed physical time of every point of the set's
///                    timeline, by point index. Every value finite.
/// @return One point per surviving envelope node, in order, with its phase in radians
///         (toPhase()) or NULL.
std::vector<PhasePoint> placePhaseNodes(
    std::span<const PhaseEnvelopeNode> nodes, std::span<const SampleTime> pointTimes);

/// Fits placed phase points to the partial's lifeline (§6.6).
///
/// The last point is moved back to the lifeline's end if it lies at or after
/// it, and any other point at or after that end is dropped. Unlike amplitude
/// (§6.4), the last point may lie before the end and then stays where it
/// is: the last state that plays a role in phase need not be the partial's
/// last state.
///
/// @param[in,out] phase  Placed phase points, strictly increasing, at least
///                       one. On return, strictly increasing, at least one,
///                       none after `end`.
/// @param end            The frequency envelope's last time. Later than the
///                       first phase point.
void fitToLifeline(std::vector<PhasePoint>& phase, SampleTime end);

/// Removes the redundant middle of every run of NULL phase points (§6.6).
///
/// A NULL point ends the span before it and starts the next; the spans
/// between consecutive NULL points are flat. In a run of three or more, only
/// the first and the last mean anything, so the others are dropped (author,
/// 2026-10-03). Such runs come from consecutive free-running states, the
/// first possibly the NULL point a default first state gives. Precomputation gives the same
/// compensation either way, with fewer segments.
///
/// @param[in,out] phase  Phase points in time order. On return, no NULL point
///                       has a NULL point both before and after it. The first
///                       and last points are never removed.
void collapseFreeRuns(std::vector<PhasePoint>& phase);

/// What happened to one partial.
struct PartialTransformOutcome {
    /// Partial.id.
    std::int64_t partialId = 0;

    /// Partial.name; nullopt when NULL.
    std::optional<std::string> partialName;

    /// Why the partial failed; nullopt when it succeeded. A failed partial
    /// has nothing written for it by this run.
    std::optional<std::string> error;

    /// Points written to its FrequencyPhysicalTimeline. Zero on failure.
    std::size_t frequencyPoints = 0;

    /// Points written to its AmplitudePhysicalTimeline. Zero on failure.
    std::size_t amplitudePoints = 0;

    /// Points written to its PhasePhysicalTimeline. Zero on failure.
    std::size_t phasePoints = 0;

    /// True when the partial was transformed and written.
    ///
    /// @return Whether `error` is unset.
    bool ok() const noexcept { return !error.has_value(); }
};

/// What happened to one PartialSet with a musical timeline.
struct PartialSetTransformOutcome {
    /// PartialSet.id.
    std::int64_t setId = 0;

    /// PartialSet.name; nullopt when NULL.
    std::optional<std::string> setName;

    /// Why the whole set failed; nullopt when its partials were attempted.
    /// A failed set has no partial outcomes and nothing written.
    std::optional<std::string> error;

    /// One outcome per partial of the set, in Partial.id order. Empty when
    /// the set failed.
    std::vector<PartialTransformOutcome> partials;

    /// How many of its partials failed.
    ///
    /// @return The number of entries in `partials` whose error is set.
    std::size_t failureCount() const noexcept;

    /// True when the set and every partial in it succeeded.
    ///
    /// @return Whether `error` is unset and failureCount() is 0.
    bool ok() const noexcept { return !error.has_value() && failureCount() == 0; }
};

/// What happened to one SoundSource with a spatial musical timeline.
struct SoundSourceTransformOutcome {
    /// SoundSource.id.
    std::int64_t sourceId = 0;

    /// SoundSource.name; nullopt when NULL.
    std::optional<std::string> sourceName;

    /// Why the source failed; nullopt when it succeeded. A failed source has
    /// nothing written for it by this run.
    std::optional<std::string> error;

    /// Points written to its AzimuthPhysicalTimeline, ElevationPhysicalTimeline
    /// and DistancePhysicalTimeline, in kSpatialKinds order. Zero on failure.
    std::array<std::size_t, 3> points{};

    /// True when the source was transformed and written.
    ///
    /// @return Whether `error` is unset.
    bool ok() const noexcept { return !error.has_value(); }
};

/// A Score's tempo map, as the run found it.
struct TempoMapSummary {
    /// Score.id.
    std::int64_t scoreId = 0;

    /// TempoMap.id.
    std::int64_t tempoMapId = 0;

    /// TempoMap.name; nullopt when NULL.
    std::optional<std::string> name;

    /// TempoMap.ticks_per_beat.
    std::int32_t ticksPerBeat = 0;

    /// How many TempoMapPoints it has.
    std::size_t pointCount = 0;
};

/// The result of a transform run.
struct TransformReport {
    /// The sample rate every time was converted at.
    SampleRate sampleRate{0};

    /// Every Score's tempo map, in Score.id order. A Score without one is
    /// absent.
    std::vector<TempoMapSummary> tempoMaps;

    /// One outcome per PartialSet with a musical timeline, in PartialSet.id
    /// order. Sets without one are not transformed and do not appear.
    std::vector<PartialSetTransformOutcome> sets;

    /// One outcome per SoundSource with a SpatialPositionMusicalTimeline, in
    /// SoundSource.id order. Sources without one are not transformed and do
    /// not appear.
    std::vector<SoundSourceTransformOutcome> sources;

    /// How many partials were attempted, over every set that did not fail.
    ///
    /// @return The total number of entries in every set's `partials`.
    std::size_t partialCount() const noexcept;

    /// How many sets failed whole, plus how many partials failed, plus how
    /// many sources failed.
    ///
    /// @return The count; 0 when everything succeeded.
    std::size_t failureCount() const noexcept;

    /// True when everything succeeded, including when there was nothing to
    /// transform.
    ///
    /// @return Whether failureCount() is 0.
    bool ok() const noexcept { return failureCount() == 0; }
};

/// Transforms every PartialSet and SoundSource with a musical timeline in
/// `db` (§6).
///
/// Reads the EngineConfig, every Score's TempoMap, every musical set's
/// timeline, points, POINT_AT edges and states, and every spatial musical
/// timeline with the same; then transforms the sets one at a time in
/// PartialSet.id order, and within a set the partials in Partial.id order,
/// each partial in its own transaction; then the sources in SoundSource.id
/// order, each in its own transaction. Any FrequencyPhysicalTimeline,
/// AmplitudePhysicalTimeline or PhasePhysicalTimeline a transformed partial
/// already has, and any AzimuthPhysicalTimeline, ElevationPhysicalTimeline or
/// DistancePhysicalTimeline a transformed source already has, is deleted and
/// rebuilt. Partial sets and sources with no musical timeline are left
/// exactly as they are.
///
/// A set, partial or source that fails is recorded in the report and the run
/// carries on. A failed partial's or source's transaction is rolled back, so
/// any timelines from an earlier run are left as they were.
///
/// @param[in,out] db  Database holding the score. Modified: every partial
///                    that succeeds has its three physical timelines
///                    replaced, and so does every source that succeeds.
/// @return What was found and what happened to each set, partial and source.
/// @throws TransformError  If there is not exactly one EngineConfig or its
///         sampleRate is unusable; if any TempoMap is invalid; or if any
///         TEMPO_MAP_SYNCHRONIZATION_POINT edge exists, which this version
///         does not support. Nothing has been written.
/// @throws EngineDbError  If reading the inputs fails. Nothing has been
///         written. A database failure while writing one partial is recorded
///         against that partial instead.
TransformReport transformAll(EngineDb& db);

} // namespace rainbohz::rendering_engine::transform
