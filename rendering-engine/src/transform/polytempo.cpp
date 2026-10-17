// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/transform/polytempo.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

/// @file
/// @brief Resolution of synchronised tempo maps to physical time.
///
/// Section numbers in comments refer to docs/polytempo.md.
///
/// @see polytempo.hpp

namespace rainbohz::rendering_engine::transform {

namespace {

/// The schema's valid tempo range, BPM (engine-schema.cypher, "tempo").
constexpr double kMinTempo = 1.0;
/// The schema's highest valid tempo, BPM.
constexpr double kMaxTempo = 1000.0;
/// Seconds in a minute, which turns BPM into beats per second.
constexpr double kSecondsPerMinute = 60.0;
/// How many halvings or doublings a bracket search may take before the
/// region is declared impossible. 2^64 in either direction is far beyond the
/// tempo range, so a region that needs more cannot be valid.
constexpr int kMaxBracketSteps = 64;

/// Formats a double for an error message, with enough digits to identify it.
///
/// @param value  Any value, including non-finite ones.
/// @return The value in the shortest form that round-trips.
std::string formatNumber(double value) {
    std::ostringstream out;
    out.precision(std::numeric_limits<double>::max_digits10);
    out << value;
    return out.str();
}

/// Formats a tick position, whole or fractional, for an error message.
///
/// @param ticks  A position in ticks.
/// @return The integer when it is whole, else the shortest round-tripping form.
std::string formatTick(double ticks) {
    if (std::isfinite(ticks) && ticks == std::floor(ticks) && std::fabs(ticks) < 1e15) {
        return std::to_string(static_cast<std::int64_t>(ticks));
    }
    return formatNumber(ticks);
}

/// Finds where a monotone function crosses a target, to machine precision.
///
/// Plain bisection: the bracket halves until it closes to two adjacent
/// doubles, which takes at most about 64 steps for any bracket. No derivative
/// is needed, and the answer is as exact as the arithmetic allows.
///
/// @param f           The function. Monotone on [lo, hi].
/// @param lo          Lower end of the bracket.
/// @param hi          Upper end of the bracket, greater than `lo`.
/// @param target      The value to reach.
/// @param decreasing  Whether `f` falls as its argument rises.
/// @return The argument at which `f` crosses `target`.
/// @pre `f(lo)` and `f(hi)` lie on opposite sides of `target`.
double bisect(
    const std::function<double(double)>& f, double lo, double hi, double target, bool decreasing) {
    for (;;) {
        const double mid = std::midpoint(lo, hi);
        if (mid <= lo || mid >= hi) {
            return mid;
        }
        const bool above = f(mid) > target;
        if (above == decreasing) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
}

/// One stretch of a resolved map while it is being built: like a
/// ResolvedSegment, but without its start time, which is assigned at the end.
struct Piece {
    double startTicks = 0.0;  ///< Where the piece starts, in ticks.
    double lengthTicks = 0.0; ///< Its length in ticks; may be +infinity.
    Tempo start{0.0};         ///< Tempo at its start.
    Tempo end{0.0};           ///< Tempo at its end.
};

/// One way of fitting a single ramp between two interior sync points
/// (§5.10): an extension at one end that continues a gradient, meeting the
/// ramp at a virtual point.
struct Candidate {
    double h = 0.0;              ///< The extension's duration, seconds.
    bool atStart = false;        ///< Which end is extended.
    double virtualTempo = 0.0;   ///< The tempo where the extension meets the ramp.
    double extensionTicks = 0.0; ///< The extension's length in ticks.
};

/// What a single ramp between two interior sync points has to fit (§5.10).
struct RampFit {
    double start = 0.0;  ///< Tempo at the earlier sync point, BPM.
    double end = 0.0;    ///< Tempo at the later sync point, BPM.
    double beats = 0.0;  ///< Beats between them.
    double target = 0.0; ///< Seconds between them.
};

/// Whether a candidate may be used: its extension is a proper part of the
/// region and its virtual tempo is in the schema's range.
///
/// @param candidate  The candidate.
/// @param target     The region's duration in seconds.
/// @return True when valid.
bool isValid(const Candidate& candidate, double target) {
    return std::isfinite(candidate.h) && candidate.h > 0.0 && candidate.h < target &&
           std::isfinite(candidate.virtualTempo) && candidate.virtualTempo >= kMinTempo &&
           candidate.virtualTempo <= kMaxTempo;
}

/// The two pieces of a single ramp fitted with a candidate.
///
/// @param startTicks  Where the region starts.
/// @param ticks       The region's length in ticks.
/// @param start       Tempo at its earlier sync point.
/// @param end         Tempo at its later sync point.
/// @param chosen      The candidate, valid.
/// @return The extension and the ramp, in order.
std::vector<Piece> splitRamp(
    double startTicks, double ticks, Tempo start, Tempo end, const Candidate& chosen) {
    const Tempo virtualTempo{chosen.virtualTempo};
    const double rampTicks = ticks - chosen.extensionTicks;
    if (chosen.atStart) {
        return {Piece{.startTicks = startTicks,
                    .lengthTicks = chosen.extensionTicks,
                    .start = start,
                    .end = virtualTempo},
            Piece{.startTicks = startTicks + chosen.extensionTicks,
                .lengthTicks = rampTicks,
                .start = virtualTempo,
                .end = end}};
    }
    return {Piece{.startTicks = startTicks,
                .lengthTicks = rampTicks,
                .start = start,
                .end = virtualTempo},
        Piece{.startTicks = startTicks + rampTicks,
            .lengthTicks = chosen.extensionTicks,
            .start = virtualTempo,
            .end = end}};
}

/// The child map under resolution: the nominal map, its sync points, and the
/// quantities derived from them that every case needs.
class Resolver {
public:
    /// Prepares the resolution: validates the anchors against the map (§6)
    /// and computes the nominal time of every point.
    ///
    /// @param nominal  The child's validated map.
    /// @param anchors  Its sync points; at least one.
    /// @throws TempoMapError  As resolveSynchronised() documents for the
    ///         anchors.
    Resolver(const TempoMap& nominal, std::span<const SyncAnchor> anchors);

    /// Resolves the map (§7).
    ///
    /// @return The resolved map.
    /// @throws TempoMapError  As resolveSynchronised() documents.
    ResolvedTempoMap resolve();

private:
    /// A region between two consecutive sync points (§3).
    struct Region {
        std::size_t first = 0;    ///< Index of its first segment (the point opening it).
        std::size_t last = 0;     ///< Index of its last segment, inclusive.
        double ticks = 0.0;       ///< Its length in ticks.
        SecondsDuration duration; ///< Its anchored duration, D.
        double k0 = 0.0;          ///< Its uncompensated stretch factor, nominal ÷ anchored.

        /// True for a region of one ramp segment between sync points when the
        /// map has three or more sync points (§5.9 to §5.11). With two sync
        /// points a single ramp is the uniform stretch of §5.3.
        bool singleRamp = false;

        /// The tempo its first segment must start at, when compensation or a
        /// neighbouring hold fixes it (§5.6, §5.7). Unset: uniform, k·T.
        std::optional<Tempo> fixedStart;

        /// The tempo its last segment must end at, likewise.
        std::optional<Tempo> fixedEnd;

        /// Its pieces once resolved; empty until then.
        std::vector<Piece> pieces;
    };

    /// Nominal end tempo of a segment.
    ///
    /// @param i  Index of the point that opens the segment.
    /// @return The point's own tempo when it holds or is the last, else the
    ///         next point's.
    Tempo endTempo(std::size_t i) const noexcept {
        const auto& points = points_;
        return (points[i].hold || i + 1 == points.size()) ? points[i].tempo : points[i + 1].tempo;
    }

    /// Nominal length of a segment.
    ///
    /// @param i  Index of the point that opens the segment.
    /// @return Its length in ticks; +infinity for the last segment.
    double lengthTicks(std::size_t i) const noexcept {
        const auto& points = points_;
        return i + 1 == points.size()
                   ? std::numeric_limits<double>::infinity()
                   : static_cast<double>((points[i + 1].position - points[i].position).value());
    }

    /// A piece's duration.
    ///
    /// @param piece  A piece of finite length.
    /// @return Its duration in seconds, in the map's ramp shape.
    SecondsDuration durationOf(const Piece& piece) const noexcept {
        return secondsThrough(
            piece.start, piece.end, piece.lengthTicks, piece.lengthTicks, resolution_, ramp_);
    }

    /// A piece's tempo gradient (§5.10).
    ///
    /// @param piece  A piece of finite length.
    /// @return BPM per second by physical time, BPM per beat by musical
    ///         position; 0 for a hold.
    double gradientOf(const Piece& piece) const noexcept {
        if (piece.start == piece.end) {
            return 0.0;
        }
        const double rise = piece.end.value() - piece.start.value();
        if (ramp_ == TempoRamp::PhysicalTime) {
            return rise / durationOf(piece).value();
        }
        return rise / (piece.lengthTicks / static_cast<double>(resolution_.value()));
    }

    /// The duration of a ramp, in the map's shape.
    ///
    /// @param from   Tempo at its start, BPM.
    /// @param to     Tempo at its end, BPM.
    /// @param beats  Its length in beats.
    /// @return Seconds.
    double seconds(double from, double to, double beats) const noexcept {
        const double ticks = beats * static_cast<double>(resolution_.value());
        return secondsThrough(Tempo{from}, Tempo{to}, ticks, ticks, resolution_, ramp_).value();
    }

    /// Names a region for a message.
    ///
    /// @param region  The region.
    /// @return e.g. "the region from tick 0 to tick 3840".
    std::string describe(const Region& region) const;

    /// Checks a tempo against the schema's range.
    ///
    /// @param tempo  BPM.
    /// @param where  What the tempo is, for the message.
    /// @throws TempoMapError  If `tempo` is not finite or outside [1, 1000].
    static void requireInRange(double tempo, const std::string& where);

    /// Builds the resolved map of a single sync point (§5.2).
    ///
    /// @return The nominal segments, every start time shifted so that the
    ///         sync point lands on its anchor.
    ResolvedTempoMap shift() const;

    /// Partitions the synced span into regions and computes each one's
    /// uncompensated stretch factor (§5.3, §5.5).
    ///
    /// @return One region per pair of consecutive sync points, unresolved.
    std::vector<Region> makeRegions() const;

    /// Sets the shared tempo at interior sync points where ramps meet (§5.6).
    ///
    /// @param[in,out] regions  The regions from makeRegions(). Where ramps
    ///                         meet, the earlier region's `fixedEnd` and the
    ///                         later's `fixedStart` are set to the geometric
    ///                         mean of their stretched tempi.
    void compensate(std::vector<Region>& regions) const;

    /// Fixes a stretch region's ends from neighbouring holds (§5.7), once
    /// those neighbours are resolved.
    ///
    /// @param[in,out] regions  The regions; `regions[r]` may gain a
    ///                         `fixedStart` or `fixedEnd`.
    /// @param r                The region to pin.
    /// @return False when a neighbouring hold the region depends on is not
    ///         resolved yet, so the region must wait.
    bool pinFromNeighbours(std::vector<Region>& regions, std::size_t r) const;

    /// Solves a stretch region's factor and fills its pieces (§5.3, §5.6).
    ///
    /// @param[in,out] region  The region, with its fixed ends set; receives
    ///                        its pieces.
    /// @throws TempoMapError  If the region cannot fill, or be compressed
    ///         into, its anchored duration.
    void solveStretch(Region& region) const;

    /// Resolves every region that is not a single ramp, holds before the
    /// ramps that meet them (§5.6 to §5.8).
    ///
    /// @param[in,out] regions  The compensated regions. On return every
    ///                         region that is not a single ramp has its
    ///                         pieces, and its `fixedStart` or `fixedEnd` is
    ///                         set where a neighbouring hold fixes it.
    /// @throws TempoMapError  If a region cannot fill, or be compressed
    ///         into, its anchored duration.
    void resolveStretches(std::vector<Region>& regions) const;

    /// The pieces of a stretch region at one stretch factor.
    ///
    /// @param region  The region, with any fixed ends set.
    /// @param k       The factor every unfixed tempo is multiplied by.
    /// @return One piece per segment, in order, honouring the fixed ends.
    std::vector<Piece> stretchPieces(const Region& region, double k) const;

    /// Resolves the single-ramp regions in tick order (§5.9 to §5.11).
    ///
    /// @param[in,out] regions  The regions, every other kind already
    ///                         resolved. On return every region has pieces.
    /// @throws TempoMapError  If a region cannot be fitted (§5.9, §5.10).
    void resolveSingleRamps(std::vector<Region>& regions) const;

    /// Resolves a single ramp at the edge of the synced span (§5.9).
    ///
    /// @param[in,out] region  The region; receives its one piece.
    /// @param atStart         True when the free end is the earlier one.
    /// @param inner           The tempo at the sync point the region shares
    ///                        with the rest of the span.
    /// @throws TempoMapError  If the free end would be outside [1, 1000].
    void resolveEdgeRamp(Region& region, bool atStart, Tempo inner) const;

    /// A candidate by physical time: closed form, the h² terms cancel (§5.10).
    ///
    /// @param fit      What the ramp has to fit.
    /// @param atStart  Which end is extended.
    /// @param g        The gradient continued there, BPM per second.
    /// @return The candidate, or nullopt when the extension would be
    ///         negative, longer than the region, or undefined.
    std::optional<Candidate> physicalCandidate(const RampFit& fit, bool atStart, double g) const;

    /// A candidate by musical position: a bracketed solve for the extension's
    /// length in beats (§5.10).
    ///
    /// @param fit      What the ramp has to fit.
    /// @param atStart  Which end is extended.
    /// @param g        The gradient continued there, BPM per beat.
    /// @return The candidate, or nullopt when no extension in range fits.
    std::optional<Candidate> positionCandidate(const RampFit& fit, bool atStart, double g) const;

    /// The message for a single ramp that no candidate fits.
    ///
    /// @param region          The region.
    /// @param fit             What it had to fit.
    /// @param gradientBefore  As given to resolveInteriorRamp().
    /// @param gradientAfter   As given to resolveInteriorRamp().
    /// @return The message.
    std::string unfittable(const Region& region, const RampFit& fit, double gradientBefore,
        std::optional<double> gradientAfter) const;

    /// Resolves a single ramp between two interior sync points by moving its
    /// change point (§5.10).
    ///
    /// @param[in,out] region   The region; receives one piece when the plain
    ///                         ramp fits, else two, meeting at a virtual point.
    /// @param start            The tempo at its earlier sync point.
    /// @param end              The tempo at its later sync point.
    /// @param gradientBefore   The gradient of the piece ending at the earlier
    ///                         sync point, as gradientOf() measures it.
    /// @param gradientAfter    The gradient of the piece starting at the later
    ///                         sync point; nullopt when that piece belongs to a
    ///                         single ramp not yet resolved (§5.11).
    /// @throws TempoMapError  If no candidate is valid.
    void resolveInteriorRamp(Region& region, Tempo start, Tempo end, double gradientBefore,
        std::optional<double> gradientAfter) const;

    /// Assembles the regions and the material outside the synced span into
    /// the resolved map (§5.4, §7 steps 6 to 9).
    ///
    /// @param regions  Every region, resolved.
    /// @return The map, every region starting at its anchor exactly.
    /// @throws TempoMapError  If any segment's tempo is outside [1, 1000].
    ResolvedTempoMap assemble(const std::vector<Region>& regions) const;

    std::vector<TempoMapPoint> points_;  ///< The nominal map's points.
    TicksPerBeat resolution_;            ///< Its ticks per beat.
    TempoRamp ramp_;                     ///< The Score's ramp shape.
    std::vector<std::size_t> syncIndex_; ///< Point index of each sync point, in tick order.
    std::vector<SecondsTime> anchor_;    ///< Each sync point's anchor, in the same order.
    std::vector<SecondsTime> nominalAt_; ///< Nominal seconds at every point, from tick 0.
};

Resolver::Resolver(const TempoMap& nominal, std::span<const SyncAnchor> anchors)
    : points_{nominal.points()}, resolution_{nominal.ticksPerBeat()}, ramp_{nominal.ramp()} {
    if (anchors.empty()) {
        throw TempoMapError{"it has a synchronisation edge but no sync point"};
    }
    std::vector<SyncAnchor> sorted{anchors.begin(), anchors.end()};
    std::ranges::sort(sorted, {}, &SyncAnchor::childTick);
    const auto& points = points_;
    for (std::size_t j = 0; j < sorted.size(); ++j) {
        const MusicalTime tick = sorted[j].childTick;
        const auto found = std::ranges::lower_bound(points, tick, {}, &TempoMapPoint::position);
        if (found == points.end() || found->position != tick) {
            throw TempoMapError{"sync point at tick " + std::to_string(tick.value()) +
                                " is not the position of one of its TempoMapPoints"};
        }
        if (j > 0 && sorted[j - 1].childTick == tick) {
            throw TempoMapError{"two sync points are at tick " + std::to_string(tick.value()) +
                                "; each must be at a different TempoMapPoint"};
        }
        if (j > 0 && sorted[j].anchor <= sorted[j - 1].anchor) {
            throw TempoMapError{"sync point at tick " + std::to_string(tick.value()) +
                                " is anchored at " + formatNumber(sorted[j].anchor.value()) +
                                " s, not after the sync point at tick " +
                                std::to_string(sorted[j - 1].childTick.value()) + " anchored at " +
                                formatNumber(sorted[j - 1].anchor.value()) +
                                " s; the anchors must strictly increase along the map"};
        }
        syncIndex_.push_back(static_cast<std::size_t>(std::distance(points.begin(), found)));
        anchor_.push_back(sorted[j].anchor);
    }
    nominalAt_.reserve(points.size());
    SecondsTime at;
    nominalAt_.push_back(at);
    for (std::size_t i = 1; i < points.size(); ++i) {
        const double length = lengthTicks(i - 1);
        at += secondsThrough(
            points[i - 1].tempo, endTempo(i - 1), length, length, resolution_, ramp_);
        nominalAt_.push_back(at);
    }
}

std::string Resolver::describe(const Region& region) const {
    const auto& points = points_;
    return "the region from tick " + std::to_string(points[region.first].position.value()) +
           " to tick " + std::to_string(points[region.last + 1].position.value());
}

void Resolver::requireInRange(double tempo, const std::string& where) {
    if (!std::isfinite(tempo) || tempo < kMinTempo || tempo > kMaxTempo) {
        throw TempoMapError{where + " would be " + formatNumber(tempo) +
                            " BPM; every tempo must be in [" + formatNumber(kMinTempo) + ", " +
                            formatNumber(kMaxTempo) + "]"};
    }
}

ResolvedTempoMap Resolver::shift() const {
    const auto& points = points_;
    const SecondsDuration offset = anchor_.front() - nominalAt_[syncIndex_.front()];
    std::vector<ResolvedSegment> segments;
    segments.reserve(points.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        segments.push_back(ResolvedSegment{
            .startTicks = static_cast<double>(points[i].position.value()),
            .lengthTicks = lengthTicks(i),
            .startSeconds = nominalAt_[i] + offset,
            .startTempo = points[i].tempo,
            .endTempo = endTempo(i),
        });
    }
    return ResolvedTempoMap{resolution_, ramp_, std::move(segments)};
}

std::vector<Resolver::Region> Resolver::makeRegions() const {
    const auto& points = points_;
    std::vector<Region> regions;
    for (std::size_t r = 0; r + 1 < syncIndex_.size(); ++r) {
        Region region;
        region.first = syncIndex_[r];
        region.last = syncIndex_[r + 1] - 1;
        region.ticks = static_cast<double>(
            (points[region.last + 1].position - points[region.first].position).value());
        region.duration = anchor_[r + 1] - anchor_[r];
        const SecondsDuration nominal = nominalAt_[region.last + 1] - nominalAt_[region.first];
        region.k0 = nominal.value() / region.duration.value();
        region.singleRamp =
            region.first == region.last && !points[region.first].hold && syncIndex_.size() > 2;
        regions.push_back(region);
    }
    return regions;
}

void Resolver::compensate(std::vector<Region>& regions) const {
    const auto& points = points_;
    for (std::size_t j = 1; j + 1 < syncIndex_.size(); ++j) {
        const std::size_t at = syncIndex_[j];
        const bool leftHolds = points[at - 1].hold;
        const bool rightHolds = points[at].hold;
        if (!leftHolds && !rightHolds) {
            // Ramps on both sides: one shared tempo, the geometric mean of the
            // two uniformly stretched tempi (§5.6). The regions then re-solve
            // against it independently.
            const Tempo shared{
                points[at].tempo.value() * std::sqrt(regions[j - 1].k0 * regions[j].k0)};
            regions[j - 1].fixedEnd = shared;
            regions[j].fixedStart = shared;
        }
        // A hold on one side (§5.7) is taken from the hold's region once that
        // region is resolved: resolveStretches() and resolveSingleRamps().
    }
}

std::vector<Piece> Resolver::stretchPieces(const Region& region, double k) const {
    const auto& points = points_;
    std::vector<Piece> pieces;
    pieces.reserve(region.last - region.first + 1);
    for (std::size_t i = region.first; i <= region.last; ++i) {
        Piece piece{.startTicks = static_cast<double>(points[i].position.value()),
            .lengthTicks = lengthTicks(i),
            .start = Tempo{k * points[i].tempo.value()},
            .end = Tempo{k * endTempo(i).value()}};
        if (i == region.first && region.fixedStart.has_value() && !points[i].hold) {
            piece.start = *region.fixedStart;
        }
        if (i == region.last && region.fixedEnd.has_value() && !points[i].hold) {
            piece.end = *region.fixedEnd;
        }
        pieces.push_back(piece);
    }
    return pieces;
}

bool Resolver::pinFromNeighbours(std::vector<Region>& regions, std::size_t r) const {
    // A ramp at this region's boundary that meets a hold of the neighbouring
    // region takes the hold's tempo (§5.7), so the neighbour must be resolved
    // first. Dependencies between two adjacent regions run one way at most,
    // so the wait always ends.
    Region& region = regions[r];
    const bool needsLeft = r > 0 && !points_[region.first].hold && points_[region.first - 1].hold &&
                           !region.fixedStart.has_value();
    const bool needsRight = r + 1 < regions.size() && !points_[region.last].hold &&
                            points_[region.last + 1].hold && !region.fixedEnd.has_value();
    if ((needsLeft && regions[r - 1].pieces.empty()) ||
        (needsRight && regions[r + 1].pieces.empty())) {
        return false;
    }
    if (needsLeft) {
        region.fixedStart = regions[r - 1].pieces.back().end;
    }
    if (needsRight) {
        region.fixedEnd = regions[r + 1].pieces.front().start;
    }
    return true;
}

void Resolver::solveStretch(Region& region) const {
    if (!region.fixedStart.has_value() && !region.fixedEnd.has_value()) {
        // Nothing pins an end: the uniform stretch (§5.3, §5.5, §5.8).
        region.pieces = stretchPieces(region, region.k0);
        return;
    }
    // The region's duration falls steadily as k rises (§5.6). By physical
    // time a region whose both ends are pinned keeps a finite duration as
    // k → 0, so the bracket search is bounded and a region that cannot fill
    // its time is rejected.
    const double target = region.duration.value();
    const auto duration = [&](double k) {
        double total = 0.0;
        for (const Piece& piece : stretchPieces(region, k)) {
            total += durationOf(piece).value();
        }
        return total;
    };
    double lo = region.k0;
    double hi = region.k0;
    for (int step = 0; duration(lo) <= target; ++step) {
        if (step == kMaxBracketSteps) {
            throw TempoMapError{describe(region) + " cannot be stretched to fill its " +
                                formatNumber(target) +
                                " s: its boundary tempi are fixed by its neighbours, and even "
                                "with its interior stopped it would take only " +
                                formatNumber(duration(lo)) + " s"};
        }
        lo *= 0.5;
    }
    for (int step = 0; duration(hi) >= target; ++step) {
        if (step == kMaxBracketSteps) {
            throw TempoMapError{
                describe(region) + " cannot be compressed into its " + formatNumber(target) + " s"};
        }
        hi *= 2.0;
    }
    region.pieces = stretchPieces(region, bisect(duration, lo, hi, target, true));
}

void Resolver::resolveStretches(std::vector<Region>& regions) const {
    std::size_t pending = 0;
    for (const Region& region : regions) {
        pending += region.singleRamp ? 0 : 1;
    }
    while (pending > 0) {
        bool progressed = false;
        for (std::size_t r = 0; r < regions.size(); ++r) {
            Region& region = regions[r];
            if (region.singleRamp || !region.pieces.empty() || !pinFromNeighbours(regions, r)) {
                continue;
            }
            solveStretch(region);
            progressed = true;
            --pending;
        }
        if (!progressed) {
            throw TempoMapError{"its regions depend on each other in a cycle, which the "
                                "design says cannot happen; please report this map"};
        }
    }
}

void Resolver::resolveEdgeRamp(Region& region, bool atStart, Tempo inner) const {
    const auto& points = points_;
    const double beats = region.ticks / static_cast<double>(resolution_.value());
    const double target = region.duration.value();
    const std::string where = "the free end of the single ramp of " + describe(region) +
                              " at the edge of the synced span";
    double outer = 0.0;
    if (ramp_ == TempoRamp::PhysicalTime) {
        // The ramp covers its beats at the mean of its end tempi (§5.9).
        outer = (2.0 * kSecondsPerMinute * beats / target) - inner.value();
    } else {
        // The duration falls as the free end's tempo rises; the schema's
        // range brackets it, or the ramp needs a tempo outside the range.
        const auto duration = [&](double candidate) {
            return atStart ? seconds(candidate, inner.value(), beats)
                           : seconds(inner.value(), candidate, beats);
        };
        if (duration(kMinTempo) < target) {
            throw TempoMapError{where + " would be below " + formatNumber(kMinTempo) + " BPM"};
        }
        if (duration(kMaxTempo) > target) {
            throw TempoMapError{where + " would be above " + formatNumber(kMaxTempo) + " BPM"};
        }
        outer = bisect(duration, kMinTempo, kMaxTempo, target, true);
    }
    requireInRange(outer, where);
    region.pieces = {Piece{.startTicks = static_cast<double>(points[region.first].position.value()),
        .lengthTicks = region.ticks,
        .start = atStart ? Tempo{outer} : inner,
        .end = atStart ? inner : Tempo{outer}}};
}

std::optional<Candidate> Resolver::physicalCandidate(
    const RampFit& fit, bool atStart, double g) const {
    const double rise = fit.end - fit.start;
    const double rampBeats = fit.target * (fit.start + fit.end) / (2.0 * kSecondsPerMinute);
    const double denominator = atStart ? rise - (g * fit.target) : (g * fit.target) - rise;
    if (denominator == 0.0) {
        return std::nullopt;
    }
    const double h = (rampBeats - fit.beats) * 2.0 * kSecondsPerMinute / denominator;
    if (!std::isfinite(h) || h <= 0.0 || h >= fit.target) {
        return std::nullopt;
    }
    const double from = atStart ? fit.start : fit.end;
    const double virtualTempo = atStart ? from + (g * h) : from - (g * h);
    const double extensionBeats = h * (from + virtualTempo) / (2.0 * kSecondsPerMinute);
    return Candidate{.h = h,
        .atStart = atStart,
        .virtualTempo = virtualTempo,
        .extensionTicks = extensionBeats * static_cast<double>(resolution_.value())};
}

std::optional<Candidate> Resolver::positionCandidate(
    const RampFit& fit, bool atStart, double g) const {
    // The extension may not take a tempo out of range, which bounds the
    // bracket on its length.
    const double from = atStart ? fit.start : fit.end;
    const double slope = atStart ? g : -g;
    double maxBeats = fit.beats;
    if (slope < 0.0) {
        maxBeats = std::min(maxBeats, (from - kMinTempo) / -slope);
    } else if (slope > 0.0) {
        maxBeats = std::min(maxBeats, (kMaxTempo - from) / slope);
    }
    if (!std::isfinite(maxBeats) || maxBeats <= 0.0) {
        return std::nullopt;
    }
    const auto duration = [&](double extension) {
        const double virtualTempo = from + (slope * extension);
        return atStart ? seconds(fit.start, virtualTempo, extension) +
                             seconds(virtualTempo, fit.end, fit.beats - extension)
                       : seconds(fit.start, virtualTempo, fit.beats - extension) +
                             seconds(virtualTempo, fit.end, extension);
    };
    const double atZero = duration(0.0);
    const double atMax = duration(maxBeats);
    if ((atZero > fit.target) == (atMax > fit.target)) {
        return std::nullopt;
    }
    const double extension = bisect(duration, 0.0, maxBeats, fit.target, atZero > atMax);
    if (!std::isfinite(extension) || extension <= 0.0 || extension >= fit.beats) {
        return std::nullopt;
    }
    const double virtualTempo = from + (slope * extension);
    return Candidate{.h = atStart ? seconds(fit.start, virtualTempo, extension)
                                  : seconds(virtualTempo, fit.end, extension),
        .atStart = atStart,
        .virtualTempo = virtualTempo,
        .extensionTicks = extension * static_cast<double>(resolution_.value())};
}

std::string Resolver::unfittable(const Region& region, const RampFit& fit, double gradientBefore,
    std::optional<double> gradientAfter) const {
    std::string reason;
    if (fit.start == fit.end && gradientBefore == 0.0 && gradientAfter.value_or(0.0) == 0.0) {
        reason = ": its end tempi are both " + formatNumber(fit.start) +
                 " BPM and no gradient is available to continue, so nothing can move, and a "
                 "ramp at that tempo covers " +
                 formatNumber(seconds(fit.start, fit.end, fit.beats)) + " s, not " +
                 formatNumber(fit.target) + " s";
    } else {
        reason = ": no extension of either end fits " + formatNumber(fit.beats) + " beats into " +
                 formatNumber(fit.target) + " s between " + formatNumber(fit.start) + " and " +
                 formatNumber(fit.end) + " BPM within [" + formatNumber(kMinTempo) + ", " +
                 formatNumber(kMaxTempo) + "]";
    }
    return "the single ramp of " + describe(region) + " cannot be fitted between its sync points" +
           reason;
}

void Resolver::resolveInteriorRamp(Region& region, Tempo start, Tempo end, double gradientBefore,
    std::optional<double> gradientAfter) const {
    const auto startTicks = static_cast<double>(points_[region.first].position.value());
    const RampFit fit{.start = start.value(),
        .end = end.value(),
        .beats = region.ticks / static_cast<double>(resolution_.value()),
        .target = region.duration.value()};

    // The plain ramp: if it already covers the region's beats, nothing moves.
    if (seconds(fit.start, fit.end, fit.beats) == fit.target) {
        region.pieces = {Piece{
            .startTicks = startTicks, .lengthTicks = region.ticks, .start = start, .end = end}};
        return;
    }

    // The candidates (§5.10): the earlier end continuing its neighbour's
    // gradient or a fixed tempo, and the later end likewise, except that a
    // later single-ramp neighbour offers no gradient (§5.11).
    std::vector<std::pair<bool, double>> tries{{true, gradientBefore}, {true, 0.0}};
    if (gradientAfter.has_value()) {
        tries.emplace_back(false, *gradientAfter);
    }
    tries.emplace_back(false, 0.0);
    std::vector<Candidate> valid;
    for (const auto& [atStart, g] : tries) {
        const std::optional<Candidate> candidate = ramp_ == TempoRamp::PhysicalTime
                                                       ? physicalCandidate(fit, atStart, g)
                                                       : positionCandidate(fit, atStart, g);
        if (candidate.has_value() && isValid(*candidate, fit.target)) {
            valid.push_back(*candidate);
        }
    }
    if (valid.empty()) {
        throw TempoMapError{unfittable(region, fit, gradientBefore, gradientAfter)};
    }
    // The least forced candidate wins (§5.10).
    const Candidate& chosen = *std::ranges::min_element(valid, {}, &Candidate::h);
    region.pieces = splitRamp(startTicks, region.ticks, start, end, chosen);
}

void Resolver::resolveSingleRamps(std::vector<Region>& regions) const {
    const std::size_t count = regions.size();
    for (std::size_t r = 0; r < count; ++r) {
        Region& region = regions[r];
        if (!region.singleRamp) {
            continue;
        }
        // The tempi at its sync points: the shared tempo where ramps meet
        // (§5.6), or the neighbouring hold's tempo (§5.7). An earlier
        // single-ramp neighbour is already resolved, in tick order (§5.11).
        const std::optional<Tempo> start =
            r == 0 ? std::nullopt
                   : std::optional<Tempo>{region.fixedStart.has_value()
                                              ? *region.fixedStart
                                              : regions[r - 1].pieces.back().end};
        const std::optional<Tempo> end =
            r + 1 == count ? std::nullopt
                           : std::optional<Tempo>{region.fixedEnd.has_value()
                                                      ? *region.fixedEnd
                                                      : regions[r + 1].pieces.front().start};
        if (start.has_value() && !end.has_value()) {
            resolveEdgeRamp(region, /*atStart=*/false, *start);
        } else if (end.has_value() && !start.has_value()) {
            resolveEdgeRamp(region, /*atStart=*/true, *end);
        } else if (!start.has_value() || !end.has_value()) {
            // A single ramp with both ends free is the uniform stretch of
            // §5.3 and is never marked singleRamp.
            throw TempoMapError{"a single ramp with both ends free reached the single-ramp "
                                "stage, which the design says cannot happen; please report this "
                                "map"};
        } else {
            // The gradient to continue at each end (§5.10): the earlier
            // neighbour's final piece, and the later neighbour's first piece
            // unless that neighbour is a single ramp still to be resolved, in
            // which case only a fixed tempo may be used there (§5.11).
            const double before = gradientOf(regions[r - 1].pieces.back());
            const std::optional<double> after =
                regions[r + 1].singleRamp
                    ? std::nullopt
                    : std::optional<double>{gradientOf(regions[r + 1].pieces.front())};
            resolveInteriorRamp(region, *start, *end, before, after);
        }
    }
}

ResolvedTempoMap Resolver::assemble(const std::vector<Region>& regions) const {
    const auto& points = points_;
    const std::size_t firstSync = syncIndex_.front();
    const std::size_t lastSync = syncIndex_.back();
    std::vector<ResolvedSegment> segments;

    // Before the synced span (§5.4): nominal, except that a ramp into the
    // first sync point arrives at the synced tempo. Times run backwards from
    // the first anchor.
    std::vector<ResolvedSegment> before;
    SecondsTime at = anchor_.front();
    for (std::size_t i = firstSync; i-- > 0;) {
        const Tempo start = points[i].tempo;
        const Tempo end = (i + 1 == firstSync && !points[i].hold)
                              ? regions.front().pieces.front().start
                              : endTempo(i);
        const double length = lengthTicks(i);
        at = at - secondsThrough(start, end, length, length, resolution_, ramp_);
        before.push_back(
            ResolvedSegment{.startTicks = static_cast<double>(points[i].position.value()),
                .lengthTicks = length,
                .startSeconds = at,
                .startTempo = start,
                .endTempo = end});
    }
    segments.assign(before.rbegin(), before.rend());

    // The regions: each starts at its anchor exactly (§7 step 8).
    for (std::size_t r = 0; r < regions.size(); ++r) {
        at = anchor_[r];
        for (const Piece& piece : regions[r].pieces) {
            segments.push_back(ResolvedSegment{.startTicks = piece.startTicks,
                .lengthTicks = piece.lengthTicks,
                .startSeconds = at,
                .startTempo = piece.start,
                .endTempo = piece.end});
            at += durationOf(piece);
        }
    }

    // After the synced span: the mirror image.
    at = anchor_.back();
    for (std::size_t i = lastSync; i < points.size(); ++i) {
        const Tempo start =
            (i == lastSync && !points[i].hold) ? regions.back().pieces.back().end : points[i].tempo;
        const Tempo end = endTempo(i);
        const double length = lengthTicks(i);
        segments.push_back(
            ResolvedSegment{.startTicks = static_cast<double>(points[i].position.value()),
                .lengthTicks = length,
                .startSeconds = at,
                .startTempo = start,
                .endTempo = end});
        if (std::isfinite(length)) {
            at += secondsThrough(start, end, length, length, resolution_, ramp_);
        }
    }

    for (const ResolvedSegment& segment : segments) {
        const std::string where = "the segment from tick " + formatTick(segment.startTicks);
        requireInRange(segment.startTempo.value(), "the tempo at the start of " + where);
        requireInRange(segment.endTempo.value(), "the tempo at the end of " + where);
    }
    return ResolvedTempoMap{resolution_, ramp_, std::move(segments)};
}

ResolvedTempoMap Resolver::resolve() {
    if (syncIndex_.size() == 1) {
        return shift();
    }
    std::vector<Region> regions = makeRegions();
    compensate(regions);
    resolveStretches(regions);
    resolveSingleRamps(regions);
    return assemble(regions);
}

} // namespace

ResolvedTempoMap::ResolvedTempoMap(
    TicksPerBeat resolution, TempoRamp ramp, std::vector<ResolvedSegment> segments)
    : resolution_{resolution}, ramp_{ramp}, segments_{std::move(segments)} {}

ResolvedTempoMap ResolvedTempoMap::nominal(const TempoMap& map) {
    const auto& points = map.points();
    std::vector<ResolvedSegment> segments;
    segments.reserve(points.size());
    // The same running sum as TempoMap's constructor, so that the times agree
    // bit for bit.
    SecondsTime at;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const bool last = i + 1 == points.size();
        const double length =
            last ? std::numeric_limits<double>::infinity()
                 : static_cast<double>((points[i + 1].position - points[i].position).value());
        const Tempo end = (points[i].hold || last) ? points[i].tempo : points[i + 1].tempo;
        segments.push_back(ResolvedSegment{
            .startTicks = static_cast<double>(points[i].position.value()),
            .lengthTicks = length,
            .startSeconds = at,
            .startTempo = points[i].tempo,
            .endTempo = end,
        });
        if (!last) {
            at += secondsThrough(
                points[i].tempo, end, length, length, map.ticksPerBeat(), map.ramp());
        }
    }
    return ResolvedTempoMap{map.ticksPerBeat(), map.ramp(), std::move(segments)};
}

SecondsTime ResolvedTempoMap::toSeconds(MusicalTime at) const {
    if (at < MusicalTime{0}) {
        throw std::invalid_argument{
            "a musical position cannot be negative: " + std::to_string(at.value())};
    }
    const auto ticks = static_cast<double>(at.value());
    const auto after = std::ranges::upper_bound(segments_, ticks, {}, &ResolvedSegment::startTicks);
    const auto index = static_cast<std::size_t>(std::distance(segments_.begin(), after)) - 1;
    const ResolvedSegment& segment = segments_[index];
    return segment.startSeconds + secondsThrough(segment.startTempo, segment.endTempo,
                                      segment.lengthTicks, ticks - segment.startTicks, resolution_,
                                      ramp_);
}

ResolvedTempoMap resolveSynchronised(const TempoMap& nominal, std::span<const SyncAnchor> anchors) {
    return Resolver{nominal, anchors}.resolve();
}

namespace {

/// The synchronisation edges as adjacency sets.
struct Adjacency {
    std::map<std::int64_t, std::set<std::int64_t>> parentsOf;  ///< Each child's distinct parents.
    std::map<std::int64_t, std::set<std::int64_t>> childrenOf; ///< Each parent's distinct children.
};

/// Builds the adjacency of the edges, checking that every map they name
/// exists.
///
/// @param mapIds  Every TempoMap.id.
/// @param edges   Every edge.
/// @return The adjacency.
/// @throws TempoMapError  If an edge names a map not in `mapIds`.
Adjacency adjacencyOf(std::span<const std::int64_t> mapIds, std::span<const SyncEdgeRecord> edges) {
    const std::unordered_set<std::int64_t> known{mapIds.begin(), mapIds.end()};
    Adjacency adjacency;
    for (const SyncEdgeRecord& edge : edges) {
        for (const std::int64_t id : {edge.childId, edge.parentId}) {
            if (!known.contains(id)) {
                throw TempoMapError{"a TEMPO_MAP_SYNCHRONIZATION_POINT edge from TempoMap " +
                                    std::to_string(edge.childId) + " to TempoMap " +
                                    std::to_string(edge.parentId) + " names TempoMap " +
                                    std::to_string(id) + ", which does not exist"};
            }
        }
        adjacency.parentsOf[edge.childId].insert(edge.parentId);
        adjacency.childrenOf[edge.parentId].insert(edge.childId);
    }
    return adjacency;
}

/// Seeds the order with the Scores' roots, in Score order.
///
/// @param roots            Each Score's root map as (TempoMap.id, Score.id).
/// @param adjacency        The edges.
/// @param[out] result      Receives each root's Score.
/// @return The roots, in Score order.
/// @throws TempoMapError  If a root has a parent, or two Scores share one.
std::deque<std::int64_t> seedRoots(std::span<const std::pair<std::int64_t, std::int64_t>> roots,
    const Adjacency& adjacency, TempoMapOrder& result) {
    std::vector<std::pair<std::int64_t, std::int64_t>> sorted{roots.begin(), roots.end()};
    std::ranges::sort(sorted, {}, &std::pair<std::int64_t, std::int64_t>::second);
    std::deque<std::int64_t> ready;
    for (const auto& [mapId, scoreId] : sorted) {
        if (const auto parents = adjacency.parentsOf.find(mapId);
            parents != adjacency.parentsOf.end()) {
            throw TempoMapError{"TempoMap " + std::to_string(mapId) + " is the TempoMap of Score " +
                                std::to_string(scoreId) +
                                " and has a synchronisation edge to TempoMap " +
                                std::to_string(*parents->second.begin()) +
                                "; a Score's map is the root of its graph and plays as written"};
        }
        if (const auto seen = result.scoreOfMap.find(mapId); seen != result.scoreOfMap.end()) {
            throw TempoMapError{"TempoMap " + std::to_string(mapId) +
                                " is the TempoMap of both Score " + std::to_string(seen->second) +
                                " and Score " + std::to_string(scoreId)};
        }
        result.scoreOfMap.emplace(mapId, scoreId);
        ready.push_back(mapId);
    }
    return ready;
}

/// Throws for the lowest-numbered map the order did not reach, if any.
///
/// @param mapIds     Every TempoMap.id.
/// @param order      The maps reached, parents first.
/// @param adjacency  The edges.
/// @throws TempoMapError  Naming an orphan, or a map in or behind a cycle.
void requireAllReached(std::span<const std::int64_t> mapIds, std::span<const std::int64_t> order,
    const Adjacency& adjacency) {
    std::vector<std::int64_t> unreached;
    for (const std::int64_t id : mapIds) {
        if (std::ranges::find(order, id) == order.end()) {
            unreached.push_back(id);
        }
    }
    if (unreached.empty()) {
        return;
    }
    const std::int64_t id = *std::ranges::min_element(unreached);
    if (!adjacency.parentsOf.contains(id)) {
        throw TempoMapError{"TempoMap " + std::to_string(id) +
                            " is not the TempoMap of any Score and has no synchronisation "
                            "edge to a parent; every map must descend from a Score's map"};
    }
    throw TempoMapError{"TempoMap " + std::to_string(id) +
                        " is not reached from any Score: its synchronisation edges form a "
                        "cycle, or lead only to maps that no Score reaches"};
}

} // namespace

TempoMapOrder orderTempoMaps(std::span<const std::int64_t> mapIds,
    std::span<const SyncEdgeRecord> edges,
    std::span<const std::pair<std::int64_t, std::int64_t>> roots) {
    const Adjacency adjacency = adjacencyOf(mapIds, edges);
    TempoMapOrder result;
    std::deque<std::int64_t> ready = seedRoots(roots, adjacency, result);
    std::unordered_map<std::int64_t, std::size_t> remaining;
    for (const auto& [child, parents] : adjacency.parentsOf) {
        remaining[child] = parents.size();
    }
    // Kahn's algorithm: a child is ready once every parent is ordered.
    while (!ready.empty()) {
        const std::int64_t mapId = ready.front();
        ready.pop_front();
        result.order.push_back(mapId);
        const std::int64_t scoreId = result.scoreOfMap.at(mapId);
        const auto children = adjacency.childrenOf.find(mapId);
        if (children == adjacency.childrenOf.end()) {
            continue;
        }
        for (const std::int64_t child : children->second) {
            if (const auto seen = result.scoreOfMap.find(child);
                seen != result.scoreOfMap.end() && seen->second != scoreId) {
                throw TempoMapError{
                    "TempoMap " + std::to_string(child) + " is reached from both Score " +
                    std::to_string(std::min(seen->second, scoreId)) + " and Score " +
                    std::to_string(std::max(seen->second, scoreId)) +
                    "; every map belongs to one Score's graph"};
            }
            result.scoreOfMap.emplace(child, scoreId);
            if (--remaining.at(child) == 0) {
                ready.push_back(child);
            }
        }
    }
    requireAllReached(mapIds, result.order, adjacency);
    return result;
}

std::unordered_map<std::int64_t, ResolvedTempoMap> resolveTempoMapGraph(
    const std::unordered_map<std::int64_t, TempoMap>& maps, std::span<const SyncEdgeRecord> edges,
    const TempoMapOrder& order) {
    std::unordered_map<std::int64_t, ResolvedTempoMap> resolved;
    for (const std::int64_t mapId : order.order) {
        const std::string what = "TempoMap " + std::to_string(mapId);
        const TempoMap& nominal = maps.at(mapId);
        std::vector<SyncAnchor> anchors;
        for (const SyncEdgeRecord& edge : edges) {
            if (edge.childId != mapId) {
                continue;
            }
            const std::string where =
                what + ": its synchronisation edge to TempoMap " + std::to_string(edge.parentId);
            if (!edge.syncPointChild.has_value()) {
                throw TempoMapError{where + " has a NULL sync_point_child"};
            }
            if (!edge.syncPointParent.has_value()) {
                throw TempoMapError{where + " has a NULL sync_point_parent"};
            }
            if (*edge.syncPointParent < 0) {
                throw TempoMapError{where + " has sync_point_parent " +
                                    std::to_string(*edge.syncPointParent) + "; it must be >= 0"};
            }
            // The parent is resolved already: parents come first in `order`.
            anchors.push_back(SyncAnchor{.childTick = MusicalTime{*edge.syncPointChild},
                .anchor =
                    resolved.at(edge.parentId).toSeconds(MusicalTime{*edge.syncPointParent})});
        }
        if (anchors.empty()) {
            resolved.emplace(mapId, ResolvedTempoMap::nominal(nominal));
            continue;
        }
        try {
            resolved.emplace(mapId, resolveSynchronised(nominal, anchors));
        } catch (const TempoMapError& e) {
            throw TempoMapError{what + ": " + e.what()};
        }
    }
    return resolved;
}

} // namespace rainbohz::rendering_engine::transform
