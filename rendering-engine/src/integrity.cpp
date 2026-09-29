#include "rainbohz/rendering_engine/integrity.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
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
/// together. checkIntegrity() runs them all and
/// gathers the results in an IntegrityReport; message text is capped at
/// kMaxReportedViolations per check, while counts stay exact.
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
    /// The table holding the list's head, e.g. "FrequencyTimeline".
    std::string_view timeline;

    /// The table holding the list's nodes, e.g. "FrequencyTimelinePoint".
    std::string_view node;
};

/// Every list type declared in NEXT_POINT, in the order the schema declares
/// them.
///
/// These are checked whether or not the engine uses them yet: a cycle or a
/// merge is a structural defect in any list, and the check is the same for
/// all of them.
constexpr std::array<ListType, 10> kListTypes{{
    {.timeline = "PartialSegmentTimeline", .node = "PartialIntegrableSegment"},
    {.timeline = "SpatialPositionTimeline", .node = "SpatialPositionTimelinePoint"},
    {.timeline = "AmplitudeTimeline", .node = "AmplitudeTimelinePoint"},
    {.timeline = "FrequencyTimeline", .node = "FrequencyTimelinePoint"},
    {.timeline = "PhaseTimeline", .node = "PhaseTimelinePoint"},
    {.timeline = "AmplitudeModulatorTimeline", .node = "AmplitudeModulatorTimelinePoint"},
    {.timeline = "PhaseModulatorTimeline", .node = "PhaseModulatorTimelinePoint"},
    {.timeline = "AzimuthModulatorTimeline", .node = "AzimuthModulatorTimelinePoint"},
    {.timeline = "ElevationModulatorTimeline", .node = "ElevationModulatorTimelinePoint"},
    {.timeline = "DistanceModulatorTimeline", .node = "DistanceModulatorTimelinePoint"},
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
    /// Node table, e.g. "FrequencyTimelinePoint".
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
/// @return The rules, in the order their checks are reported. Valid for the
///         whole program.
const std::array<RangeRule, 11>& rangeRules() {
    static const std::array<RangeRule, 11> kRules{{
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
            .max = Bound{.value = 2.0, .inclusive = true}},
        {.table = "FrequencyTimelinePoint",
            .column = "time",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = std::nullopt},
        {.table = "FrequencyTimelinePoint",
            .column = "frequency",
            .nullable = false,
            .min = std::nullopt,
            .max = std::nullopt},
        {.table = "FrequencyTimelinePoint",
            .column = "frequency_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "FrequencyTimelinePoint",
            .column = "frequency_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
        {.table = "AmplitudeTimelinePoint",
            .column = "time",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = std::nullopt},
        {.table = "AmplitudeTimelinePoint",
            .column = "amplitude",
            .nullable = false,
            .min = std::nullopt,
            .max = Bound{.value = 0.0, .inclusive = true}},
        {.table = "AmplitudeTimelinePoint",
            .column = "amplitude_curve_type",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 4.0, .inclusive = true}},
        {.table = "AmplitudeTimelinePoint",
            .column = "amplitude_curve_amount",
            .nullable = false,
            .min = Bound{.value = 0.0, .inclusive = true},
            .max = Bound{.value = 1.0, .inclusive = true}},
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
/// @return e.g. "FrequencyTimelinePoint.time is not NULL, finite and >= 0".
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
/// @param timeline  The timeline table, e.g. "FrequencyTimeline".
/// @param node      Its node table, which has a `time` column.
/// @return For each partial with such a timeline, the `time` of the list's
///         first node and of its last, either of which is nullopt when NULL
///         or when the list is empty.
/// @throws EngineDbError  If a query fails.
std::unordered_map<std::int64_t, std::pair<std::optional<double>, std::optional<double>>> listEnds(
    EngineDb& db, const std::string& timeline, const std::string& node) {
    std::unordered_map<std::int64_t, std::optional<std::int64_t>> headOfPartial;
    std::string headsQuery = "MATCH (p:Partial)-[:HAS_TIMELINE]->(t:";
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
/// AmplitudeTimelinePoint's time equals the first FrequencyTimelinePoint's
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
    IntegrityCheck check{
        .name = "AmplitudeTimeline and FrequencyTimeline lists start and end at the same times",
        .violationCount = 0,
        .violations = {}};
    const auto frequency = listEnds(db, "FrequencyTimeline", "FrequencyTimelinePoint");
    const auto amplitude = listEnds(db, "AmplitudeTimeline", "AmplitudeTimelinePoint");

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
            check.add("Partial " + std::to_string(partial) + ": AmplitudeTimeline starts at " +
                      describeTime(aFirst) + " but FrequencyTimeline starts at " +
                      describeTime(fFirst));
        }
        if (fLast != aLast) {
            check.add("Partial " + std::to_string(partial) + ": AmplitudeTimeline ends at " +
                      describeTime(aLast) + " but FrequencyTimeline ends at " +
                      describeTime(fLast));
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
    cycles.name = timeline + " lists have no cycles";
    merges.name = timeline + " lists do not merge into one another";

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
