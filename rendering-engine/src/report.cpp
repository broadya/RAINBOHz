#include "rainbohz/rendering_engine/report.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <ios>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// @brief Writes the engine's reports as text.
///
/// Implements the functions of report.hpp. Moved out of main.cpp unchanged
/// apart from the stream, so that batch mode, which writes to standard output,
/// and server mode, which writes each call's narrative into its response, say
/// the same things in the same words.
///
/// @see report.hpp

namespace rainbohz::rendering_engine {

using transform::PartialSetTransformOutcome;
using transform::PartialTransformOutcome;
using transform::SoundSourceTransformOutcome;
using transform::TempoMapSummary;
using transform::TransformReport;

namespace {

/// The width of the longest table name in a census, for aligning the counts.
///
/// @param counts  Table names with their row counts.
/// @return Length in bytes of the longest name; 0 when `counts` is empty.
std::size_t widestName(const std::vector<RowCount>& counts) {
    std::size_t widest = 0;
    for (const RowCount& count : counts) {
        widest = std::max(widest, count.first.size());
    }
    return widest;
}

/// Prints one line per table: its name, left-aligned in a column of `width`,
/// then its row count.
///
/// @param out     Where the report goes.
/// @param counts  Table names with their row counts, printed in the given order.
/// @param width   Column width for the names, normally from widestName() over
///                every list printed together so that all the counts align.
void reportRows(std::ostream& out, const std::vector<RowCount>& counts, std::size_t width) {
    for (const RowCount& count : counts) {
        out << "    " << std::left << std::setw(static_cast<int>(width)) << count.first << "  "
            << count.second << '\n';
    }
}

/// Names a partial for the precomputation report.
///
/// @param outcome  The partial's outcome.
/// @return Its name followed by its id, e.g. "Partial 0 (id 0)"; or
///         "(unnamed) (id 3)" when Partial.name is NULL.
std::string describePartial(const PartialOutcome& outcome) {
    return outcome.partialName.value_or("(unnamed)") + " (id " + std::to_string(outcome.partialId) +
           ")";
}

/// Names a partial for the transform report.
///
/// @param outcome  The partial's outcome.
/// @return Its name followed by its id, as describePartial() formats it.
std::string describeTransformedPartial(const PartialTransformOutcome& outcome) {
    return outcome.partialName.value_or("(unnamed)") + " (id " + std::to_string(outcome.partialId) +
           ")";
}

/// Names a partial set for the transform report.
///
/// @param outcome  The set's outcome.
/// @return Its name followed by its id, e.g. "PartialSet Chord (id 0)".
std::string describeSet(const PartialSetTransformOutcome& outcome) {
    return "PartialSet " + outcome.setName.value_or("(unnamed)") + " (id " +
           std::to_string(outcome.setId) + ")";
}

/// Names a partial that failed rendering's validation.
///
/// @param failure  The failure.
/// @return Its name followed by its id, as describePartial() formats it.
std::string describeFailedPartial(const PartialFailure& failure) {
    return failure.partialName.value_or("(unnamed)") + " (id " + std::to_string(failure.partialId) +
           ")";
}

} // namespace

void narrateStatement(std::ostream& out, bool verbose, const std::filesystem::path& file,
    const rainbohz::rendering_engine::CypherStatement& statement,
    const rainbohz::rendering_engine::ExecuteOutcome& outcome) {
    if (outcome.ok || outcome.alreadyExists) {
        if (verbose) {
            out << "    " << (outcome.ok ? "ok      " : "present ") << ' '
                << file.filename().string() << ", " << statement.lineSpan() << ": "
                << statement.label() << '\n';
        }
        return;
    }

    out << "    FAILED   " << file.filename().string() << ", " << statement.lineSpan() << ": "
        << statement.label() << '\n';
    std::istringstream detail{outcome.message};
    for (std::string line; std::getline(detail, line);) {
        out << "               " << line << '\n';
    }
}

void reportLoad(std::ostream& out, std::string_view what, const LoadReport& report) {
    out << "  " << what << ": applied " << report.applied << ", already present "
        << report.alreadyPresent << ", failed " << report.failures.size() << '\n';
}

void reportSchemaCensus(std::ostream& out, EngineDb& db) {
    const std::vector<TableEntry> tables = db.tables();
    int nodes = 0;
    int rels = 0;
    for (const TableEntry& table : tables) {
        if (table.second == "NODE") {
            ++nodes;
        } else if (table.second == "REL") {
            ++rels;
        }
    }
    out << "  engine-db declares " << nodes << " NODE and " << rels << " REL tables.\n";
}

void reportDataCensus(std::ostream& out, EngineDb& db) {
    const Census census = db.census();

    out << "  engine-db holds " << census.totalNodes << " nodes and " << census.totalRelationships
        << " relationships.\n";
    if (census.empty()) {
        return;
    }

    const std::size_t width = std::max(widestName(census.nodes), widestName(census.relationships));
    out << '\n';
    reportRows(out, census.nodes, width);
    if (!census.nodes.empty() && !census.relationships.empty()) {
        out << '\n';
    }
    reportRows(out, census.relationships, width);
}

void reportProblems(std::ostream& out, const std::filesystem::path& file,
    const std::vector<RequestProblem>& problems) {
    for (const RequestProblem& problem : problems) {
        out << "    FAILED   " << describeProblem(file, problem) << '\n';
    }
}

void reportIntegrity(std::ostream& out, const IntegrityReport& report, bool verbose) {
    std::size_t passed = 0;
    for (const IntegrityCheck& check : report.checks) {
        if (check.ok()) {
            ++passed;
            if (verbose) {
                out << "    ok       " << check.name << '\n';
            }
            continue;
        }
        for (const std::string& violation : check.violations) {
            out << "    FAILED   " << check.name << ": " << violation << '\n';
        }
        if (check.violationCount > check.violations.size()) {
            out << "    FAILED   " << check.name << ": and "
                << check.violationCount - check.violations.size() << " more\n";
        }
    }
    out << "  integrity: " << passed << " of " << report.checks.size() << " check(s) passed, "
        << report.violationCount() << " violation(s)\n";
}

void reportTransform(std::ostream& out, const TransformReport& report, bool verbose) {
    out << "  transform: sample rate " << report.sampleRate.value() << '\n';
    for (const TempoMapSummary& map : report.tempoMaps) {
        out << "    Score " << map.scoreId << " keeps time by TempoMap "
            << map.name.value_or("(unnamed)") << " (id " << map.tempoMapId
            << "): " << map.ticksPerBeat << " ticks per beat, " << map.pointCount << " point(s)\n";
    }
    if (report.sets.empty()) {
        out << "    no PartialSet has a musical timeline; nothing to transform\n";
    }
    for (const PartialSetTransformOutcome& set : report.sets) {
        if (set.error.has_value()) {
            out << "    FAILED   " << describeSet(set) << ": " << *set.error << '\n';
            continue;
        }
        for (const PartialTransformOutcome& partial : set.partials) {
            if (partial.error.has_value()) {
                out << "    FAILED   " << describeTransformedPartial(partial) << " in "
                    << describeSet(set) << ": " << *partial.error << '\n';
            } else if (verbose) {
                out << "    ok       " << describeTransformedPartial(partial) << ": "
                    << partial.frequencyPoints << " frequency point(s), " << partial.amplitudePoints
                    << " amplitude point(s), " << partial.phasePoints << " phase point(s)\n";
            }
        }
    }
    std::size_t failedSets = 0;
    std::size_t failedPartials = 0;
    for (const PartialSetTransformOutcome& set : report.sets) {
        failedSets += set.error.has_value() ? 1 : 0;
        failedPartials += set.failureCount();
    }
    out << "  transformed " << report.partialCount() - failedPartials << " of "
        << report.partialCount() << " partial(s) in " << report.sets.size() - failedSets << " of "
        << report.sets.size() << " partial set(s)\n";

    std::size_t failedSources = 0;
    for (const SoundSourceTransformOutcome& source : report.sources) {
        const std::string name = "SoundSource " + source.sourceName.value_or("(unnamed)") +
                                 " (id " + std::to_string(source.sourceId) + ")";
        if (source.error.has_value()) {
            ++failedSources;
            out << "    FAILED   " << name << ": " << *source.error << '\n';
        } else if (verbose) {
            out << "    ok       " << name << ": " << source.points.at(0) << " azimuth, "
                << source.points.at(1) << " elevation, " << source.points.at(2)
                << " distance point(s)\n";
        }
    }
    out << "  transformed the spatial position of " << report.sources.size() - failedSources
        << " of " << report.sources.size() << " SoundSource(s) with a musical timeline\n";
}

void reportPrecomputation(std::ostream& out, const PrecomputationReport& report, bool verbose) {
    out << "  precomputation: frame size " << report.frameSize.value() << " samples\n";
    for (const PartialOutcome& outcome : report.partials) {
        if (outcome.error.has_value()) {
            out << "    FAILED   " << describePartial(outcome) << ": " << *outcome.error << '\n';
            continue;
        }
        if (verbose && outcome.frames.has_value()) {
            out << "    ok       " << describePartial(outcome) << ": " << outcome.segmentCount
                << " segment(s), " << outcome.compensationCount
                << " phase compensation segment(s), frames " << outcome.frames->first.value()
                << ".." << outcome.frames->last.value() << ", " << outcome.entryCount
                << " frame entries\n";
        }
    }
    out << "  precomputed " << report.partials.size() - report.failureCount() << " of "
        << report.partials.size() << " partial(s)\n";
}

void reportRendering(std::ostream& out, const RenderingReport& report, bool verbose) {
    out << "  rendering: sample rate " << report.sampleRate.value() << ", frame size "
        << report.frameSize.value() << " samples, " << channelModeName(report.channelMode) << " ("
        << channelCount(report.channelMode) << " channel(s))\n";
    for (const FrameOutcome& frame : report.frames) {
        for (const PartialFailure& failure : frame.newFailures) {
            out << "    FAILED   " << describeFailedPartial(failure) << " in frame "
                << failure.frame.value() << ": " << failure.reason << '\n';
        }
        if (const std::optional<std::string>& error = frame.error) {
            out << "    FAILED   frame " << frame.frame.value() << ": " << *error << '\n';
            continue;
        }
        if (!verbose) {
            continue;
        }
        out << "    ok       frame " << frame.frame.value() << ": " << frame.sourceCount
            << " source(s), " << frame.setCount << " set(s), " << frame.partialCount
            << " partial(s), ";
        if (frame.setCount == 0) {
            out << "silent";
        } else {
            out << "peak " << frame.peak;
        }
        out << ", wrote " << frame.file.filename().string() << '\n';
        for (const auto& [setId, file] : frame.setFiles) {
            out << "    ok         set " << setId << ": wrote " << file.filename().string() << '\n';
        }
    }
    out << "  rendered " << report.frames.size() - report.failureCount() << " of "
        << report.frames.size() << " frame(s)";
    if (report.failedPartialCount() > 0) {
        out << "; " << report.failedPartialCount() << " partial(s) failed validation";
    }
    out << '\n';
}

void reportRequest(std::ostream& out, const RenderRequest& request) {
    out << "  request: " << request.frames.size() << " frame(s)\n";
    for (const auto& frame : request.frames) {
        out << "    frame " << std::left << std::setw(10) << frame.frameNumber;
        if (frame.allPartials()) {
            out << "all partials\n";
            continue;
        }
        out << "labels ";
        const char* separator = "";
        for (const std::string& label : frame.labels) {
            out << separator << label;
            separator = ", ";
        }
        out << '\n';
    }
}

} // namespace rainbohz::rendering_engine
