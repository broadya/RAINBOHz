#include "rainbohz/rendering_engine/integrity.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

/// @file
/// @brief The engine-db integrity checks run on request by `--validate-db`.
///
/// The checks are declared as data: a table of the list types in NEXT_POINT
/// and a table of column range rules, so that adding a check is adding a row.
/// For each list type the heads and links are read into memory and analysed
/// by findListDefects() for cycles and for lists merged into another. Each
/// range rule is checked against its column. EngineConfig is checked for being
/// a singleton that yields a frame size; every Partial for being in exactly
/// one PartialSet; and each partial's two timelines for starting and ending
/// together. The musical-time checks cover what the transform reads: the
/// shape of each TempoMap, every musical point's POINT_AT, the order of each
/// musical timeline, and the links and membership of PartialStates.
/// checkIntegrity() runs them all and gathers the results in an
/// IntegrityReport; message text is capped at kMaxReportedViolations per
/// check, while counts stay exact.
///
/// Read-only: nothing here modifies the database. The design is
/// docs/integrity.md.
///
/// @see integrity.hpp

namespace rainbohz::rendering_engine {

namespace {

// ---------------------------------------------------------------------------
// The checks, as data
// ---------------------------------------------------------------------------

/// One linked-list type of NEXT_POINT: a timeline table whose node points to
/// the first list node, and the node table of the list itself.
struct ListType {
    /// The table holding the list's head, e.g. "FrequencyPhysicalTimeline".
    std::string_view timeline;

    /// The table holding the list's nodes, e.g. "FrequencyPhysicalTimelinePoint".
    std::string_view node;
};

/// Every list type declared in NEXT_POINT, in the order the schema declares
/// them.
///
/// These are checked whether or not the engine uses them yet: a cycle or a
/// merge is a structural defect in any list, and the check is the same for
/// all of them.
constexpr std::array<ListType, 14> kListTypes{{
    {.timeline = "PartialSegmentPhysicalTimeline", .node = "PartialIntegrableSegment"},
    {.timeline = "PartialSegmentPhysicalTimeline", .node = "PartialPhaseCompensationSegment"},
    {.timeline = "SpatialPositionMusicalTimeline", .node = "SpatialPositionMusicalTimelinePoint"},
    {.timeline = "AmplitudePhysicalTimeline", .node = "AmplitudePhysicalTimelinePoint"},
    {.timeline = "FrequencyPhysicalTimeline", .node = "FrequencyPhysicalTimelinePoint"},
    {.timeline = "PhasePhysicalTimeline", .node = "PhasePhysicalTimelinePoint"},
    {.timeline = "FrequencyModulatorPhysicalTimeline",
        .node = "FrequencyModulatorPhysicalTimelinePoint"},
    {.timeline = "AmplitudeModulatorPhysicalTimeline",
        .node = "AmplitudeModulatorPhysicalTimelinePoint"},
    {.timeline = "PhaseModulatorPhysicalTimeline", .node = "PhaseModulatorPhysicalTimelinePoint"},
    {.timeline = "AzimuthModulatorPhysicalTimeline",
        .node = "AzimuthModulatorPhysicalTimelinePoint"},
    {.timeline = "ElevationModulatorPhysicalTimeline",
        .node = "ElevationModulatorPhysicalTimelinePoint"},
    {.timeline = "DistanceModulatorPhysicalTimeline",
        .node = "DistanceModulatorPhysicalTimelinePoint"},
    {.timeline = "TempoMap", .node = "TempoMapPoint"},
    {.timeline = "PartialSetMusicalTimeline", .node = "PartialSetMusicalTimelinePoint"},
}};

/// One end of a permitted range.
struct Bound {
    /// The limit itself.
    double value = 0.0;

    /// Whether the limit is itself permitted.
    bool inclusive = true;
};

/// A range rule for one column: the values the schema documents as valid.
///
/// Every non-NULL value must also be finite. Integer columns are compared as
/// doubles, which is exact for every integer type the schema uses here.
struct RangeRule {
    /// Node table, e.g. "FrequencyPhysicalTimelinePoint".
    std::string_view table;

    /// Column of that table.
    std::string_view column;

    /// Whether NULL is a valid value.
    bool nullable = false;

    /// Lowest valid value; nullopt when unbounded below.
    std::optional<Bound> min;

    /// Highest valid value; nullopt when unbounded above.
    std::optional<Bound> max;
};

/// The range rules: only columns the engine currently reads (§3 of
/// docs/integrity.md). A feature brings its columns' rules with it.
///
/// The limits are those of the schema. `time` ≥ 0 was confirmed by the author
/// (2026-09-23): a partial cannot start before the score does. Curve type and
/// amount are checked against the schema's enumeration and its documented
/// [0, 1], not against what precomputation or rendering supports; that
/// narrower check is theirs (precomputation.md §7, rendering.md §9). An
/// amplitude above 0 dB is an error (author, 2026-09-23): the renderer clips
/// it as a defence, not as permission to store it.
///
/// The canonical-model rows arrived with the transform (docs/transform.md
/// section 9): tempo map resolution and points, the curve fields of musical
/// timeline points, and PartialState values, which may be NULL by design (a
/// NULL property is "no knot here").
///
/// @return The rules, in the order their checks are reported. Valid for the
///         whole program.
const std::array<RangeRule, 20>& rangeRules() {
    static const std::array<RangeRule, 20> kRules{{
        {.table = "EngineConfig",
            .column = "frameSize",
            .nullable = true,
            .min = Bound{.value = 0.0, .inclusive = false},
            .max = std::nullopt},
        {.table = "EngineConfig",
            .column = "sampleRate",
            .nullable = true,
            .min = Bound{.value = 0.0, .inclusive = false},
            .max = std::nullopt},
        {.table = "EngineConfig",
            .column = "channelMode",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "FrequencyPhysicalTimelinePoint",
            .column = "time",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = std::nullopt},
        {.table = "FrequencyPhysicalTimelinePoint",
            .column = "frequency",
            .nullable = false,
            .min = std::nullopt,
            .max = std::nullopt},
        {.table = "FrequencyPhysicalTimelinePoint",
            .column = "frequency_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "FrequencyPhysicalTimelinePoint",
            .column = "frequency_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
        {.table = "AmplitudePhysicalTimelinePoint",
            .column = "time",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = std::nullopt},
        {.table = "AmplitudePhysicalTimelinePoint",
            .column = "amplitude",
            .nullable = false,
            .min = std::nullopt,
            .max = Bound{.value = 0.0, .inclusive = true}},
        {.table = "AmplitudePhysicalTimelinePoint",
            .column = "amplitude_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "AmplitudePhysicalTimelinePoint",
            .column = "amplitude_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
        {.table = "TempoMap",
            .column = "ticks_per_beat",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = false},
            .max = std::nullopt},
        {.table = "TempoMapPoint",
            .column = "position",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = std::nullopt},
        {.table = "TempoMapPoint",
            .column = "start_tempo",
            .nullable = false,
            .min = Bound{.value = 1.0, .inclusive = true},
            .max = Bound{.value = 1000.0, .inclusive = true}},
        {.table = "PartialSetMusicalTimelinePoint",
            .column = "frequency_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "PartialSetMusicalTimelinePoint",
            .column = "frequency_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
        {.table = "PartialSetMusicalTimelinePoint",
            .column = "amplitude_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "PartialSetMusicalTimelinePoint",
            .column = "amplitude_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
        {.table = "PartialState",
            .column = "frequency",
            .nullable = true,
            .min = std::nullopt,
            .max = std::nullopt},
        {.table = "PartialState",
            .column = "amplitude",
            .nullable = true,
            .min = Bound{.value = -144.0, .inclusive = true},
            .max = Bound{.value = 0.0, .inclusive = true}},
    }};
    return kRules;
}

/// Longest run of node ids a cycle message spells out.
constexpr std::size_t kMaxCycleIdsShown = 8;

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

/// Formats a double in the shortest form that reads back as the same value.
///
/// @param value  Any value, including non-finite ones.
/// @return e.g. "48000", "0.1", "-inf", "nan".
std::string formatNumber(double value) {
    std::array<char, 32> buffer{};
    const auto [end, error] = std::to_chars(
        buffer.data(), std::next(buffer.data(), static_cast<std::ptrdiff_t>(buffer.size())), value);
    if (error != std::errc{}) {
        return "?";
    }
    return std::string{buffer.data(), end};
}

/// Describes a rule as the property a passing check establishes.
///
/// @param rule  The rule.
/// @return e.g. "FrequencyPhysicalTimelinePoint.time is not NULL, finite and >= 0".
std::string describe(const RangeRule& rule) {
    std::string text = std::string{rule.table} + "." + std::string{rule.column} + " is ";
    text += rule.nullable ? "NULL or finite" : "not NULL, finite";
    if (rule.min.has_value() && rule.max.has_value()) {
        text += std::string{" and in "} + (rule.min->inclusive ? "[" : "(") +
                formatNumber(rule.min->value) + ", " + formatNumber(rule.max->value) +
                (rule.max->inclusive ? "]" : ")");
    } else if (rule.min.has_value()) {
        text += std::string{" and "} + (rule.min->inclusive ? ">= " : "> ") +
                formatNumber(rule.min->value);
    } else if (rule.max.has_value()) {
        text += std::string{" and "} + (rule.max->inclusive ? "<= " : "< ") +
                formatNumber(rule.max->value);
    }
    return text;
}

/// Spells out the node ids of a cycle, back to its first.
///
/// @param nodes  The cycle's ids in list order. Not empty.
/// @return e.g. "3 -> 4 -> 5 -> 3"; a long cycle is elided after
///         kMaxCycleIdsShown ids, with its length.
std::string describeCycle(const std::vector<std::int64_t>& nodes) {
    std::string text;
    const std::size_t shown = std::min(nodes.size(), kMaxCycleIdsShown);
    for (std::size_t i = 0; i < shown; ++i) {
        text += std::to_string(nodes[i]) + " -> ";
    }
    if (shown < nodes.size()) {
        text += "... (" + std::to_string(nodes.size()) + " nodes) -> ";
    }
    return text + std::to_string(nodes.front());
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// Reads an id-to-id map from a two-column query.
///
/// @param db      Database to read.
/// @param cypher  A query returning (key id, value id) rows.
/// @return The map. A key that appears twice keeps its first value; the
///         queries used here cannot produce one (NEXT_POINT is ONE_ONE).
/// @throws EngineDbError  If the query fails or returns a NULL id.
std::unordered_map<std::int64_t, std::int64_t> readIdMap(EngineDb& db, const std::string& cypher) {
    std::unordered_map<std::int64_t, std::int64_t> map;
    for (const DbRow& row : db.query(cypher)) {
        map.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    return map;
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------

/// Checks that there is exactly one EngineConfig, and that it yields a frame
/// size: `frameSize`, or `sampleRate` when that is NULL.
///
/// Whether the values are positive is a range rule; this checks only what no
/// single-column rule can.
///
/// @param db  Database to check.
/// @return The check's outcome.
/// @throws EngineDbError  If the query fails.
IntegrityCheck checkEngineConfig(EngineDb& db) {
    IntegrityCheck check{.name = "EngineConfig is a singleton with frameSize or sampleRate set",
        .violationCount = 0,
        .violations = {}};
    const std::vector<DbRow> rows =
        db.query("MATCH (c:EngineConfig) RETURN c.id, c.frameSize, c.sampleRate ORDER BY c.id;");
    if (rows.size() != 1) {
        check.add("there are " + std::to_string(rows.size()) +
                  " EngineConfig nodes; exactly one is required");
    }
    for (const DbRow& row : rows) {
        if (!optionalInt64(row.at(1)).has_value() && !optionalInt64(row.at(2)).has_value()) {
            check.add("EngineConfig " + std::to_string(requireInt64(row.at(0))) +
                      ": frameSize and sampleRate are both NULL, so there is no frame size");
        }
    }
    return check;
}

/// Checks one value against a range rule.
///
/// @param rule   The rule.
/// @param value  The column's value for one node.
/// @return What is wrong, to follow the column name in a message (e.g. " is
///         NULL", " is -3; it must be >= 0"); nullopt when the value is valid.
std::optional<std::string> rangeViolation(const RangeRule& rule, const DbValue& value) {
    if (std::holds_alternative<std::monostate>(value)) {
        return rule.nullable ? std::nullopt : std::optional<std::string>{" is NULL"};
    }
    double number = 0.0;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        number = static_cast<double>(*integer);
    } else if (const auto* real = std::get_if<double>(&value)) {
        number = *real;
    } else {
        return " is not a number";
    }

    const std::string is = " is " + formatNumber(number);
    if (!std::isfinite(number)) {
        return is + ", which is not finite";
    }
    if (rule.min.has_value() &&
        (rule.min->inclusive ? number < rule.min->value : number <= rule.min->value)) {
        return is + "; it must be " + (rule.min->inclusive ? ">= " : "> ") +
               formatNumber(rule.min->value);
    }
    if (rule.max.has_value() &&
        (rule.max->inclusive ? number > rule.max->value : number >= rule.max->value)) {
        return is + "; it must be " + (rule.max->inclusive ? "<= " : "< ") +
               formatNumber(rule.max->value);
    }
    return std::nullopt;
}

/// Checks every value of one column against its range rule.
///
/// @param db    Database to check.
/// @param rule  The rule. Its table and column are spliced into the query
///              text; they come from rangeRules(), never from input.
/// @return The check's outcome, one violation per offending node, in id
///         order.
/// @throws EngineDbError  If the query fails, e.g. the column does not exist.
IntegrityCheck checkRange(EngineDb& db, const RangeRule& rule) {
    IntegrityCheck check{.name = describe(rule), .violationCount = 0, .violations = {}};
    const std::string column{rule.column};
    const std::string cypher =
        "MATCH (n:" + std::string{rule.table} + ") RETURN n.id, n." + column + " ORDER BY n.id;";

    for (const DbRow& row : db.query(cypher)) {
        if (const std::optional<std::string> problem = rangeViolation(rule, row.at(1))) {
            std::string message{rule.table};
            message += " " + std::to_string(requireInt64(row.at(0))) + ": " + column;
            message += *problem;
            check.add(std::move(message));
        }
    }
    return check;
}

/// Checks that every Partial is aggregated by exactly one PartialSet.
///
/// Ladybug enforces at most one (HAS_PARTIAL is ONE_MANY); zero is
/// the defect, and rendering needs the set to group by.
///
/// @param db  Database to check.
/// @return The check's outcome, one violation per partial without a set, in
///         id order.
/// @throws EngineDbError  If the query fails.
IntegrityCheck checkSetMembership(EngineDb& db) {
    IntegrityCheck check{.name = "every Partial is aggregated by exactly one PartialSet",
        .violationCount = 0,
        .violations = {}};
    const std::vector<DbRow> rows =
        db.query("MATCH (p:Partial) OPTIONAL MATCH (s:PartialSet)-[:HAS_PARTIAL]->(p) "
                 "RETURN p.id, count(s) ORDER BY p.id;");
    for (const DbRow& row : rows) {
        const std::int64_t sets = requireInt64(row.at(1));
        if (sets != 1) {
            check.add("Partial " + std::to_string(requireInt64(row.at(0))) + ": aggregated by " +
                      std::to_string(sets) + " PartialSet(s)");
        }
    }
    return check;
}

/// The first and last node times of every list of one timeline type, by
/// partial.
///
/// Walks each list in C++ from its head, stopping silently at a revisited
/// node: cycles are the list checks' business, and a list that loops still
/// has a first time and some last time to report.
///
/// @param db        Database to read.
/// @param timeline  The timeline table, e.g. "FrequencyPhysicalTimeline".
/// @param node      Its node table, which has a `time` column.
/// @return For each partial with such a timeline, the `time` of the list's
///         first node and of its last, either of which is nullopt when NULL
///         or when the list is empty.
/// @throws EngineDbError  If a query fails.
std::unordered_map<std::int64_t, std::pair<std::optional<double>, std::optional<double>>> listEnds(
    EngineDb& db, const std::string& timeline, const std::string& node) {
    std::unordered_map<std::int64_t, std::optional<std::int64_t>> headOfPartial;
    std::string headsQuery = "MATCH (p:Partial)-[:HAS_PHYSICAL_TIMELINE]->(t:";
    headsQuery += timeline;
    headsQuery += ") OPTIONAL MATCH (t)-[:NEXT_POINT]->(n:";
    headsQuery += node;
    headsQuery += ") RETURN p.id, n.id;";
    for (const DbRow& row : db.query(headsQuery)) {
        headOfPartial.emplace(requireInt64(row.at(0)), optionalInt64(row.at(1)));
    }
    std::unordered_map<std::int64_t, std::optional<double>> timeOf;
    for (const DbRow& row : db.query("MATCH (n:" + node + ") RETURN n.id, n.time;")) {
        timeOf.emplace(requireInt64(row.at(0)), optionalDouble(row.at(1)));
    }
    const auto next =
        readIdMap(db, "MATCH (a:" + node + ")-[:NEXT_POINT]->(b:" + node + ") RETURN a.id, b.id;");

    std::unordered_map<std::int64_t, std::pair<std::optional<double>, std::optional<double>>> ends;
    for (const auto& [partial, head] : headOfPartial) {
        std::optional<double> first;
        std::optional<double> last;
        std::unordered_set<std::int64_t> seen;
        for (std::optional<std::int64_t> at = head; at.has_value() && seen.insert(*at).second;) {
            const auto time = timeOf.find(*at);
            const std::optional<double> value = time == timeOf.end() ? std::nullopt : time->second;
            if (seen.size() == 1) {
                first = value;
            }
            last = value;
            const auto link = next.find(*at);
            at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
        }
        ends.emplace(partial, std::make_pair(first, last));
    }
    return ends;
}

/// Checks that, for every partial with both timelines, the first
/// AmplitudePhysicalTimelinePoint's time equals the first FrequencyPhysicalTimelinePoint's
/// and the last equals the last, exactly.
///
/// The two timelines define the lifeline together; interior points are
/// independent (author, 2026-09-24). A partial with only one of the
/// timelines is precomputation's failure, not this check's.
///
/// @param db  Database to check.
/// @return The check's outcome, one violation per mismatched end, in
///         partial id order.
/// @throws EngineDbError  If a query fails.
IntegrityCheck checkLifelineEquality(EngineDb& db) {
    IntegrityCheck check{.name = "AmplitudePhysicalTimeline and FrequencyPhysicalTimeline lists "
                                 "start and end at the same times",
        .violationCount = 0,
        .violations = {}};
    const auto frequency =
        listEnds(db, "FrequencyPhysicalTimeline", "FrequencyPhysicalTimelinePoint");
    const auto amplitude =
        listEnds(db, "AmplitudePhysicalTimeline", "AmplitudePhysicalTimelinePoint");

    std::vector<std::int64_t> partials;
    for (const auto& [partial, ends] : frequency) {
        if (amplitude.contains(partial)) {
            partials.push_back(partial);
        }
    }
    std::ranges::sort(partials);

    const auto describeTime = [](const std::optional<double>& time) {
        return time.has_value() ? formatNumber(*time) : std::string{"NULL"};
    };
    for (const std::int64_t partial : partials) {
        const auto& [fFirst, fLast] = frequency.at(partial);
        const auto& [aFirst, aLast] = amplitude.at(partial);
        if (fFirst != aFirst) {
            check.add("Partial " + std::to_string(partial) +
                      ": AmplitudePhysicalTimeline starts at " + describeTime(aFirst) +
                      " but FrequencyPhysicalTimeline starts at " + describeTime(fFirst));
        }
        if (fLast != aLast) {
            check.add("Partial " + std::to_string(partial) +
                      ": AmplitudePhysicalTimeline ends at " + describeTime(aLast) +
                      " but FrequencyPhysicalTimeline ends at " + describeTime(fLast));
        }
    }
    return check;
}

/// Checks the linked lists of one list type for cycles and merges.
///
/// @param db    Database to check.
/// @param type  The list type. Its table names are spliced into the query
///              text; they come from kListTypes, never from input.
/// @param[out] cycles  Receives the cycle check's outcome.
/// @param[out] merges  Receives the merge check's outcome.
/// @throws EngineDbError  If a query fails.
void checkLists(
    EngineDb& db, const ListType& type, IntegrityCheck& cycles, IntegrityCheck& merges) {
    const std::string timeline{type.timeline};
    const std::string node{type.node};
    // A list is named after its timeline, unless the timeline heads more than
    // one list; then after its node table, so that the names stay distinct.
    const bool sharedTimeline =
        std::ranges::count(kListTypes, type.timeline, &ListType::timeline) > 1;
    const std::string name{sharedTimeline ? type.node : type.timeline};
    cycles.name = name + " lists have no cycles";
    merges.name = name + " lists do not merge into one another";

    const auto heads = readIdMap(
        db, "MATCH (t:" + timeline + ")-[:NEXT_POINT]->(n:" + node + ") RETURN t.id, n.id;");
    const auto next =
        readIdMap(db, "MATCH (a:" + node + ")-[:NEXT_POINT]->(b:" + node + ") RETURN a.id, b.id;");

    const ListDefects defects = findListDefects(heads, next);

    for (const ListCycle& cycle : defects.cycles) {
        std::string message = node + " ids " + describeCycle(cycle.nodes) + " form a cycle, ";
        if (cycle.enteredFrom.empty()) {
            message += "reached from no " + timeline;
        } else {
            message += "entered from the head of " + timeline;
            for (std::size_t i = 0; i < cycle.enteredFrom.size(); ++i) {
                message += (i == 0 ? " " : ", ") + std::to_string(cycle.enteredFrom[i]);
            }
        }
        cycles.add(std::move(message));
    }

    for (const ListMerge& merge : defects.merges) {
        std::string message = timeline + " " + std::to_string(merge.timeline);
        message += " starts at " + node + " " + std::to_string(merge.node);
        message += ", which also follows " + node + " " + std::to_string(merge.predecessor);
        message += merge.owner.has_value()
                       ? " in the list of " + timeline + " " + std::to_string(*merge.owner)
                       : " in a list no " + timeline + " reaches";
        merges.add(std::move(message));
    }
}

// ---------------------------------------------------------------------------
// Musical-time checks: the transform's inputs (docs/transform.md section 9)
// ---------------------------------------------------------------------------

/// Follows a NEXT_POINT list from its head, stopping silently at a revisited
/// node: cycles are the list checks' business.
///
/// @param head  Id of the first node, or nullopt for an empty list.
/// @param next  For each node id, the id of its successor.
/// @return The node ids in list order, each at most once.
std::vector<std::int64_t> walkQuietly(
    std::optional<std::int64_t> head, const std::unordered_map<std::int64_t, std::int64_t>& next) {
    std::vector<std::int64_t> chain;
    std::unordered_set<std::int64_t> seen;
    for (std::optional<std::int64_t> at = head; at.has_value() && seen.insert(*at).second;) {
        chain.push_back(*at);
        const auto link = next.find(*at);
        at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
    }
    return chain;
}

/// Checks the shape of every TempoMap's point list: at least one point, the
/// first at position 0, positions strictly increasing, hold set on every
/// point and true on the last (docs/transform.md section 6.1).
///
/// NULL positions are the range rule's business and are skipped here.
///
/// @param db  Database to check.
/// @return The check's outcome, in TempoMap id order.
/// @throws EngineDbError  If a query fails.
IntegrityCheck checkTempoMapShape(EngineDb& db) {
    IntegrityCheck check{.name = "TempoMap lists start at position 0, strictly increase, and "
                                 "end with a point that holds",
        .violationCount = 0,
        .violations = {}};
    std::vector<std::pair<std::int64_t, std::optional<std::int64_t>>> maps;
    for (const DbRow& row : db.query("MATCH (m:TempoMap) OPTIONAL MATCH (m)-[:NEXT_POINT]->"
                                     "(p:TempoMapPoint) RETURN m.id, p.id ORDER BY m.id;")) {
        maps.emplace_back(requireInt64(row.at(0)), optionalInt64(row.at(1)));
    }
    std::unordered_map<std::int64_t, std::pair<std::optional<std::int64_t>, std::optional<bool>>>
        points;
    for (const DbRow& row : db.query("MATCH (p:TempoMapPoint) RETURN p.id, p.position, p.hold;")) {
        std::optional<bool> hold;
        if (const auto* flag = std::get_if<bool>(&row.at(2))) {
            hold = *flag;
        }
        points.emplace(requireInt64(row.at(0)), std::make_pair(optionalInt64(row.at(1)), hold));
    }
    const auto next = readIdMap(
        db, "MATCH (a:TempoMapPoint)-[:NEXT_POINT]->(b:TempoMapPoint) RETURN a.id, b.id;");

    for (const auto& [mapId, head] : maps) {
        const std::vector<std::int64_t> chain = walkQuietly(head, next);
        const std::string map = "TempoMap " + std::to_string(mapId);
        if (chain.empty()) {
            check.add(map + ": has no TempoMapPoint; at least one is required");
            continue;
        }
        std::optional<std::int64_t> previous;
        for (std::size_t i = 0; i < chain.size(); ++i) {
            const auto& [position, hold] = points.at(chain[i]);
            const auto at = [&](const std::string& reason) {
                return "TempoMapPoint " + std::to_string(chain[i]) + ": " + reason;
            };
            if (i == 0 && position.has_value() && *position != 0) {
                check.add(at("position is " + std::to_string(*position) + "; the first point of " +
                             map + " must be at position 0"));
            }
            if (i > 0 && position.has_value() && previous.has_value() && *position <= *previous) {
                check.add(at("position " + std::to_string(*position) +
                             " is not after the previous point's " + std::to_string(*previous) +
                             " in " + map));
            }
            if (position.has_value()) {
                previous = position;
            }
            if (!hold.has_value()) {
                check.add(at("hold is NULL"));
            } else if (i + 1 == chain.size() && !*hold) {
                check.add(
                    at("hold is false on the last point of " + map + "; the last point must hold"));
            }
        }
    }
    return check;
}

/// What a PartialSetMusicalTimelinePoint's POINT_AT says, as read.
struct PointAt {
    std::optional<std::int64_t> tempoMapId;
    std::optional<std::int64_t> syncPoint;
    std::optional<double> offsetSeconds;
};

/// Reads every musical timeline point's POINT_AT, or its absence.
///
/// @param db  Database to read.
/// @return By point id. A point without a POINT_AT has every field nullopt.
/// @throws EngineDbError  If the query fails.
std::map<std::int64_t, PointAt> readPointAts(EngineDb& db) {
    std::map<std::int64_t, PointAt> result;
    for (const DbRow& row :
        db.query("MATCH (p:PartialSetMusicalTimelinePoint) OPTIONAL MATCH "
                 "(p)-[a:POINT_AT]->(m:TempoMap) "
                 "RETURN p.id, m.id, a.sync_point, a.offset_seconds ORDER BY p.id;")) {
        result.emplace(requireInt64(row.at(0)), PointAt{.tempoMapId = optionalInt64(row.at(1)),
                                                    .syncPoint = optionalInt64(row.at(2)),
                                                    .offsetSeconds = optionalDouble(row.at(3))});
    }
    return result;
}

/// Checks that every PartialSetMusicalTimelinePoint has a POINT_AT (the
/// schema enforces at most one) with a non-NULL sync_point of 0 or more and
/// an offset that is NULL or finite.
///
/// @param db  Database to check.
/// @return The check's outcome, in point id order.
/// @throws EngineDbError  If the query fails.
IntegrityCheck checkPointAt(EngineDb& db) {
    IntegrityCheck check{.name = "every PartialSetMusicalTimelinePoint has one POINT_AT with "
                                 "sync_point >= 0 and a NULL or finite offset_seconds",
        .violationCount = 0,
        .violations = {}};
    for (const auto& [pointId, at] : readPointAts(db)) {
        const std::string point = "PartialSetMusicalTimelinePoint " + std::to_string(pointId);
        if (!at.tempoMapId.has_value()) {
            check.add(point + ": has no POINT_AT");
            continue;
        }
        if (!at.syncPoint.has_value()) {
            check.add(point + ": sync_point is NULL");
        } else if (*at.syncPoint < 0) {
            check.add(
                point + ": sync_point is " + std::to_string(*at.syncPoint) + "; it must be >= 0");
        }
        if (at.offsetSeconds.has_value() && !std::isfinite(*at.offsetSeconds)) {
            check.add(point + ": offset_seconds is " + formatNumber(*at.offsetSeconds) +
                      ", which is not finite");
        }
    }
    return check;
}

/// A PartialSet's musical timeline, with the Score it hangs from and that
/// Score's TempoMap, as far as they exist.
struct MusicalList {
    std::int64_t setId = 0;
    std::vector<std::int64_t> points;
    std::optional<std::int64_t> scoreId;
    std::optional<std::int64_t> tempoMapId;
};

/// Reads every PartialSet with a musical timeline, its points in list order,
/// and its Score and TempoMap.
///
/// @param db  Database to read.
/// @return The lists, in PartialSet id order. A set with two timelines
///         appears once per timeline; the database allows at most one.
/// @throws EngineDbError  If a query fails.
std::vector<MusicalList> readMusicalLists(EngineDb& db) {
    std::unordered_map<std::int64_t, std::pair<std::int64_t, std::optional<std::int64_t>>> owner;
    for (const DbRow& row : db.query(
             "MATCH (s:Score)-[:HAS_SOUNDENTITY]->(:SoundEntity)-[:HAS_SOUNDSOURCE]->(:SoundSource)"
             "-[:HAS_PARTIALSET]->(ps:PartialSet) OPTIONAL MATCH (s)-[:NEXT_POINT]->(m:TempoMap) "
             "RETURN ps.id, s.id, m.id;")) {
        owner.emplace(requireInt64(row.at(0)),
            std::make_pair(requireInt64(row.at(1)), optionalInt64(row.at(2))));
    }
    const auto next = readIdMap(db, "MATCH (a:PartialSetMusicalTimelinePoint)-[:NEXT_POINT]->"
                                    "(b:PartialSetMusicalTimelinePoint) RETURN a.id, b.id;");
    std::vector<MusicalList> lists;
    for (const DbRow& row :
        db.query("MATCH (ps:PartialSet)-[:HAS_MUSICAL_TIMELINE]->(t:PartialSetMusicalTimeline) "
                 "OPTIONAL MATCH (t)-[:NEXT_POINT]->(p:PartialSetMusicalTimelinePoint) "
                 "RETURN ps.id, p.id ORDER BY ps.id, t.id;")) {
        MusicalList list{.setId = requireInt64(row.at(0)),
            .points = walkQuietly(optionalInt64(row.at(1)), next),
            .scoreId = std::nullopt,
            .tempoMapId = std::nullopt};
        if (const auto found = owner.find(list.setId); found != owner.end()) {
            list.scoreId = found->second.first;
            list.tempoMapId = found->second.second;
        }
        lists.push_back(std::move(list));
    }
    return lists;
}

/// Checks that the points of every musical timeline are in non-decreasing
/// sync_point order, and that each leads to the TempoMap of the Score the
/// timeline's PartialSet hangs from.
///
/// Points without a POINT_AT, or with a NULL sync_point, are checkPointAt()'s
/// business and are skipped here.
///
/// @param db  Database to check.
/// @return The check's outcome, in PartialSet id order.
/// @throws EngineDbError  If a query fails.
IntegrityCheck checkMusicalOrder(EngineDb& db) {
    IntegrityCheck check{.name = "PartialSetMusicalTimeline points are in non-decreasing "
                                 "sync_point order on the TempoMap of their Score",
        .violationCount = 0,
        .violations = {}};
    const std::map<std::int64_t, PointAt> pointAts = readPointAts(db);
    for (const MusicalList& list : readMusicalLists(db)) {
        const std::string set = "PartialSet " + std::to_string(list.setId);
        if (!list.scoreId.has_value()) {
            check.add(set + ": has a musical timeline but is not reached from any Score");
            continue;
        }
        if (!list.tempoMapId.has_value()) {
            check.add(set + ": has a musical timeline but its Score " +
                      std::to_string(*list.scoreId) + " has no TempoMap");
            continue;
        }
        const std::int64_t scoreId = *list.scoreId;
        const std::int64_t mapId = *list.tempoMapId;
        std::optional<std::int64_t> previous;
        for (const std::int64_t pointId : list.points) {
            const auto found = pointAts.find(pointId);
            if (found == pointAts.end()) {
                continue;
            }
            const PointAt& at = found->second;
            if (!at.tempoMapId.has_value()) {
                continue;
            }
            const std::int64_t target = *at.tempoMapId;
            const auto describe = [pointId](const std::string& reason) {
                return "PartialSetMusicalTimelinePoint " + std::to_string(pointId) + ": " + reason;
            };
            if (target != mapId) {
                check.add(describe("POINT_AT leads to TempoMap " + std::to_string(target) +
                                   ", not TempoMap " + std::to_string(mapId) + " of Score " +
                                   std::to_string(scoreId)));
            }
            if (!at.syncPoint.has_value()) {
                continue;
            }
            const std::int64_t sync = *at.syncPoint;
            if (previous.has_value() && sync < *previous) {
                check.add(describe("sync_point " + std::to_string(sync) +
                                   " is before the previous point's " + std::to_string(*previous) +
                                   " in " + set + "'s timeline"));
            }
            previous = sync;
        }
    }
    return check;
}

/// Checks that every PartialState is in exactly one PartialStateSet and is
/// the state of exactly one Partial. The schema enforces at most one of each;
/// zero is the defect.
///
/// @param db  Database to check.
/// @return The check's outcome, in PartialState id order.
/// @throws EngineDbError  If the query fails.
IntegrityCheck checkPartialStateLinks(EngineDb& db) {
    IntegrityCheck check{
        .name = "every PartialState is in one PartialStateSet and is the state of one Partial",
        .violationCount = 0,
        .violations = {}};
    for (const DbRow& row :
        db.query("MATCH (st:PartialState) OPTIONAL MATCH (st)-[:MEMBER_OF_SET_MANY_ONE]->"
                 "(ss:PartialStateSet) OPTIONAL MATCH (st)-[:STATE_OF]->(p:Partial) "
                 "RETURN st.id, count(DISTINCT ss), count(DISTINCT p) ORDER BY st.id;")) {
        const std::int64_t sets = requireInt64(row.at(1));
        const std::int64_t partials = requireInt64(row.at(2));
        if (sets != 1 || partials != 1) {
            check.add("PartialState " + std::to_string(requireInt64(row.at(0))) + ": in " +
                      std::to_string(sets) + " PartialStateSet(s) and the state of " +
                      std::to_string(partials) + " Partial(s)");
        }
    }
    return check;
}

/// Checks that a PartialStateSet reached from a timeline point holds states
/// only of partials in that point's PartialSet, and at most one per partial.
///
/// @param db  Database to check.
/// @return The check's outcome, in point id order.
/// @throws EngineDbError  If a query fails.
IntegrityCheck checkStateMembership(EngineDb& db) {
    IntegrityCheck check{.name = "a PartialStateSet used by a timeline point holds at most one "
                                 "state per partial, all of that point's PartialSet",
        .violationCount = 0,
        .violations = {}};
    std::unordered_map<std::int64_t, std::int64_t> setOfPartial;
    for (const DbRow& row :
        db.query("MATCH (ps:PartialSet)-[:HAS_PARTIAL]->(p:Partial) RETURN p.id, ps.id;")) {
        setOfPartial.emplace(requireInt64(row.at(0)), requireInt64(row.at(1)));
    }
    std::unordered_map<std::int64_t, std::int64_t> setOfPoint;
    for (const MusicalList& list : readMusicalLists(db)) {
        for (const std::int64_t pointId : list.points) {
            setOfPoint.emplace(pointId, list.setId);
        }
    }
    std::optional<std::int64_t> currentPoint;
    std::unordered_set<std::int64_t> seenPartials;
    for (const DbRow& row :
        db.query("MATCH (p:PartialSetMusicalTimelinePoint)-[:POINT_STATE]->(:PartialStateSet)"
                 "<-[:MEMBER_OF_SET_MANY_ONE]-(st:PartialState)-[:STATE_OF]->(partial:Partial) "
                 "RETURN p.id, st.id, partial.id ORDER BY p.id, st.id;")) {
        const std::int64_t pointId = requireInt64(row.at(0));
        const std::int64_t stateId = requireInt64(row.at(1));
        const std::int64_t partialId = requireInt64(row.at(2));
        if (currentPoint != pointId) {
            currentPoint = pointId;
            seenPartials.clear();
        }
        const std::string where = "PartialSetMusicalTimelinePoint " + std::to_string(pointId) +
                                  ": PartialState " + std::to_string(stateId);
        const auto set = setOfPoint.find(pointId);
        const auto partialSet = setOfPartial.find(partialId);
        if (set != setOfPoint.end() &&
            (partialSet == setOfPartial.end() || partialSet->second != set->second)) {
            check.add(where + " belongs to Partial " + std::to_string(partialId) +
                      ", which is not in PartialSet " + std::to_string(set->second));
        }
        if (!seenPartials.insert(partialId).second) {
            check.add(where + " is a second state of Partial " + std::to_string(partialId) +
                      " at this point");
        }
    }
    return check;
}

} // namespace

// ---------------------------------------------------------------------------
// Reports
// ---------------------------------------------------------------------------

void IntegrityCheck::add(std::string message) {
    ++violationCount;
    if (violations.size() < kMaxReportedViolations) {
        violations.push_back(std::move(message));
    }
}

std::size_t IntegrityReport::violationCount() const noexcept {
    std::size_t total = 0;
    for (const IntegrityCheck& check : checks) {
        total += check.violationCount;
    }
    return total;
}

// ---------------------------------------------------------------------------
// List analysis
// ---------------------------------------------------------------------------

namespace {

/// Node id to node id: a successor, predecessor or head map.
using IdMap = std::unordered_map<std::int64_t, std::int64_t>;

/// The keys of a map, sorted, so that results do not depend on hash order.
///
/// @param map  Any id map.
/// @return Its keys in ascending order.
std::vector<std::int64_t> sortedKeys(const IdMap& map) {
    std::vector<std::int64_t> keys;
    keys.reserve(map.size());
    for (const auto& [key, value] : map) {
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    return keys;
}

/// Finds every cycle in a successor map.
///
/// Every node has at most one successor, so following successors from any
/// node either ends or runs into a cycle. Each node is walked at most once:
/// `done` marks nodes already accounted for, and `onPath` the position of each
/// node on the walk in progress.
///
/// @param next  For each node id, the id of its successor.
/// @return Every cycle, rotated to start at its smallest id and ordered by
///         it. Their `enteredFrom` is empty.
std::vector<ListCycle> findCycles(const IdMap& next) {
    std::vector<ListCycle> cycles;
    std::unordered_set<std::int64_t> done;
    for (const std::int64_t start : sortedKeys(next)) {
        std::vector<std::int64_t> path;
        std::unordered_map<std::int64_t, std::size_t> onPath;
        for (std::optional<std::int64_t> at = start; at.has_value() && !done.contains(*at);) {
            if (const auto seen = onPath.find(*at); seen != onPath.end()) {
                ListCycle cycle;
                cycle.nodes.assign(
                    std::next(path.begin(), static_cast<std::ptrdiff_t>(seen->second)), path.end());
                std::ranges::rotate(cycle.nodes, std::ranges::min_element(cycle.nodes));
                cycles.push_back(std::move(cycle));
                break;
            }
            onPath.emplace(*at, path.size());
            path.push_back(*at);
            const auto link = next.find(*at);
            at = link == next.end() ? std::nullopt : std::optional<std::int64_t>{link->second};
        }
        done.insert(path.begin(), path.end());
    }
    std::ranges::sort(cycles, {}, [](const ListCycle& c) { return c.nodes.front(); });
    return cycles;
}

/// The timeline whose list a node is on, found by walking back to the start
/// of the list.
///
/// @param node      Id of a node not on a cycle.
/// @param previous  For each node id, the id of its predecessor.
/// @param headOf    For each first node, the id of its timeline.
/// @return The timeline's id; nullopt when the list starts at a node no
///         timeline points to. `seen` only guards against a database that
///         breaks findListDefects()'s precondition; a sound one cannot loop
///         here, and nullopt is returned if it does.
std::optional<std::int64_t> listOwner(
    std::int64_t node, const IdMap& previous, const IdMap& headOf) {
    std::unordered_set<std::int64_t> seen{node};
    for (auto back = previous.find(node); back != previous.end(); back = previous.find(node)) {
        node = back->second;
        if (!seen.insert(node).second) {
            return std::nullopt;
        }
    }
    const auto owner = headOf.find(node);
    return owner == headOf.end() ? std::nullopt : std::optional<std::int64_t>{owner->second};
}

} // namespace

ListDefects findListDefects(const IdMap& heads, const IdMap& next) {
    ListDefects defects{.cycles = findCycles(next), .merges = {}};

    // Heads, by node, and predecessors, by node: the reverse of `heads` and
    // `next`. Unique by the precondition.
    IdMap headOf;
    for (const auto& [timeline, first] : heads) {
        headOf.emplace(first, timeline);
    }
    IdMap previous;
    for (const auto& [from, to] : next) {
        previous.emplace(to, from);
    }

    std::unordered_set<std::int64_t> onCycle;
    for (ListCycle& cycle : defects.cycles) {
        onCycle.insert(cycle.nodes.begin(), cycle.nodes.end());
        for (const std::int64_t id : cycle.nodes) {
            if (const auto head = headOf.find(id); head != headOf.end()) {
                cycle.enteredFrom.push_back(head->second);
            }
        }
        std::ranges::sort(cycle.enteredFrom);
    }

    // Merges: a head that is also a successor, and not on a cycle (that case
    // is the cycle's entry).
    for (const std::int64_t timeline : sortedKeys(heads)) {
        const std::int64_t first = heads.at(timeline);
        const auto before = previous.find(first);
        if (before == previous.end() || onCycle.contains(first)) {
            continue;
        }
        defects.merges.push_back(ListMerge{
            .timeline = timeline,
            .node = first,
            .predecessor = before->second,
            .owner = listOwner(before->second, previous, headOf),
        });
    }
    return defects;
}

// ---------------------------------------------------------------------------
// The run
// ---------------------------------------------------------------------------

IntegrityReport checkIntegrity(EngineDb& db) {
    IntegrityReport report;
    report.checks.push_back(checkEngineConfig(db));
    for (const RangeRule& rule : rangeRules()) {
        report.checks.push_back(checkRange(db, rule));
    }
    report.checks.push_back(checkSetMembership(db));
    report.checks.push_back(checkLifelineEquality(db));
    report.checks.push_back(checkTempoMapShape(db));
    report.checks.push_back(checkPointAt(db));
    report.checks.push_back(checkMusicalOrder(db));
    report.checks.push_back(checkPartialStateLinks(db));
    report.checks.push_back(checkStateMembership(db));
    for (const ListType& type : kListTypes) {
        IntegrityCheck cycles;
        IntegrityCheck merges;
        checkLists(db, type, cycles, merges);
        report.checks.push_back(std::move(cycles));
        report.checks.push_back(std::move(merges));
    }
    return report;
}

} // namespace rainbohz::rendering_engine
