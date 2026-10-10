// SPDX-License-Identifier: Apache-2.0

#pragma once

// Integrity checks over engine-db: invariants the data model relies on but
// the database does not, or cannot, enforce.
//
// Run on request (--validate-db), not on every run. The upstream components
// are expected to build a sound database; this is a development aid and a
// backstop, answering "is what loaded actually well formed?" before anything
// computes from it. The design is rendering-engine/docs/integrity.md.
//
// Two families of check, each driven by a table so that adding a check is
// adding a row:
//
//   - Linked lists. For every list type in NEXT_POINT: no cycles, and no list
//     merged into another. Ladybug counts NEXT_POINT's ONE_ONE per FROM/TO
//     pair, so neither is ruled out by the schema (AGENTS.md, Linked Lists).
//   - Ranges. Columns the engine reads are checked against the ranges the
//     schema documents. Columns of features not yet implemented are not
//     checked; their rules arrive with the feature.
//
// Plus checks that are neither: EngineConfig is a singleton with a frame
// size; every Partial is in exactly one PartialSet; each partial's
// amplitude and frequency timelines start and end at the same times, since
// they define the lifeline together; and, for the musical-time tables the
// transform reads (docs/transform.md section 9), every TempoMap is well
// shaped, every musical timeline point has a usable POINT_AT to its Score's
// TempoMap and the points are in musical order, and every PartialState is
// linked to one set and one partial of the right PartialSet.
//
// Checks report; they never modify the database.

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace rainbohz::rendering_engine {

/// The most violations a check keeps the text of. The count is always exact;
/// only the messages are capped, so that a badly broken database does not
/// produce millions of lines.
inline constexpr std::size_t kMaxReportedViolations = 20;

/// The outcome of one integrity check.
struct IntegrityCheck {
    /// What the check establishes when it passes, e.g. "FrequencyPhysicalTimeline
    /// lists have no cycles".
    std::string name;

    /// How many violations were found.
    std::size_t violationCount = 0;

    /// The first kMaxReportedViolations violations, each naming the table,
    /// the node id and what is wrong.
    std::vector<std::string> violations;

    /// Records a violation: counted always, its message kept only while
    /// fewer than kMaxReportedViolations have been kept.
    ///
    /// @param message  What is wrong, naming the node involved.
    void add(std::string message);

    /// Whether the check passed.
    ///
    /// @return True when the check found nothing wrong.
    bool ok() const noexcept { return violationCount == 0; }
};

/// The result of checkIntegrity().
struct IntegrityReport {
    /// Every check that ran, in a fixed order.
    std::vector<IntegrityCheck> checks;

    /// How many violations were found, over all checks.
    ///
    /// @return The sum of every check's violationCount.
    std::size_t violationCount() const noexcept;

    /// Whether every check passed.
    ///
    /// @return True when no check found a violation, including when none ran.
    bool ok() const noexcept { return violationCount() == 0; }
};

/// A NEXT_POINT cycle.
struct ListCycle {
    /// The node ids on the cycle, in list order, starting from the smallest
    /// id so that the same cycle is always reported the same way.
    std::vector<std::int64_t> nodes;

    /// Ids of the timelines whose head is on the cycle, in ascending order.
    /// Empty when no timeline reaches it. A timeline can only reach a cycle
    /// through its head: entering it anywhere else needs a node with two
    /// predecessors of the same pair, which the database refuses.
    std::vector<std::int64_t> enteredFrom;
};

/// A list merged into another: a timeline's first node is also the successor
/// of a node, so two lists share a tail.
struct ListMerge {
    /// Id of the timeline whose head is `node`.
    std::int64_t timeline = 0;

    /// Id of the node that is both a head and a successor.
    std::int64_t node = 0;

    /// Id of the node whose successor is `node`.
    std::int64_t predecessor = 0;

    /// Id of the timeline whose list `predecessor` is on; nullopt when that
    /// list is reached from no timeline.
    std::optional<std::int64_t> owner;
};

/// What is wrong with the linked lists of one list type.
struct ListDefects {
    /// Every cycle, ordered by its smallest node id.
    std::vector<ListCycle> cycles;

    /// Every merge, ordered by timeline id. A head on a cycle is reported as
    /// the cycle's entry, not as a merge.
    std::vector<ListMerge> merges;
};

/// Finds the cycles and merges among the linked lists of one list type.
///
/// Pure: works on ids already read from the database.
///
/// @param heads  For each timeline id, the id of its first node. Timelines
///               with no nodes are absent.
/// @param next   For each node id, the id of its successor. Nodes absent
///               from it end their list.
/// @return The defects; both lists empty when the lists are sound.
/// @pre No node is the successor of two nodes, and no node is the head of two
///      timelines. The database enforces both (NEXT_POINT is ONE_ONE per
///      FROM/TO pair); a violation of either is not reported.
/// @note O(n log n) in the number of nodes and timelines: linear, apart from
///       sorting them so that the result is deterministic.
ListDefects findListDefects(const std::unordered_map<std::int64_t, std::int64_t>& heads,
    const std::unordered_map<std::int64_t, std::int64_t>& next);

/// Runs the canonical pass of the integrity checks against `db`: everything
/// the transform reads (docs/integrity.md section 2).
///
/// That is the EngineConfig, the canonical model's columns, set membership,
/// the tempo maps, every musical timeline's POINT_AT edges and order, the
/// PartialStates, the first SpatialPositionState of every spatial timeline,
/// and the canonical lists. It is run before the transform, when the physical
/// timelines of a musical score do not exist yet.
///
/// Reads the whole of every table it checks, so its cost is proportional to
/// the size of the database. It is meant for development and diagnosis, not
/// for every request.
///
/// @param db  Database to check. Not modified.
/// @return One IntegrityCheck per check, in a fixed order.
/// @throws EngineDbError  If a query fails, e.g. because the schema has not
///         been applied. Violations are reported, not thrown.
IntegrityReport checkCanonicalIntegrity(EngineDb& db);

/// Runs the physical pass of the integrity checks against `db`: everything
/// the transform writes and precomputation reads (docs/integrity.md section
/// 2).
///
/// That is the physical timelines' columns, lifeline equality, the coverage
/// of every SoundSource's spatial timelines, and the physical lists. It is
/// run after the transform and before precomputation.
///
/// @param db  Database to check. Not modified.
/// @return One IntegrityCheck per check, in a fixed order.
/// @throws EngineDbError  If a query fails. Violations are reported, not
///         thrown.
IntegrityReport checkPhysicalIntegrity(EngineDb& db);

/// Runs both passes against `db`, canonical first, as one report.
///
/// For a database whose physical timelines are already in place, such as one
/// written in physical time or already transformed.
///
/// @param db  Database to check. Not modified.
/// @return The canonical pass's checks followed by the physical pass's.
/// @throws EngineDbError  If a query fails. Violations are reported, not
///         thrown.
IntegrityReport checkIntegrity(EngineDb& db);

} // namespace rainbohz::rendering_engine
