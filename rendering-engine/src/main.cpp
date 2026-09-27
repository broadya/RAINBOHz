#include "rainbohz/rendering_engine/engine_db.hpp"
#include "rainbohz/rendering_engine/integrity.hpp"
#include "rainbohz/rendering_engine/options.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/render_request.hpp"
#include "rainbohz/rendering_engine/rendering.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <ios>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

/// @file
/// @brief Entry point of the rendering-engine executable.
///
/// Parses the command line (options.hpp) and dispatches on the run mode. Three
/// run modes are anticipated; only `test` is implemented: read the render
/// request, stand up engine-db by applying the schema from one Cypher file,
/// load an initial database state from a second, optionally check its
/// integrity (`--validate-db`), precompute every partial's integrable segments
/// and frame entries, check the request's labels against what loaded, and
/// render the requested frames to WAV files. Transformation is not
/// implemented, and labels do not yet select partials.
///
/// The library modules return reports; this file turns them into output on
/// stdout and stderr and into the process exit code. The reporting format
/// deliberately follows scripts/load_schema.py. The two tools apply the same
/// DDL to the same database for the same reasons, and a reader should not have
/// to learn two output formats to compare them.

namespace {

using rainbohz::rendering_engine::Census;
using rainbohz::rendering_engine::describeProblem;
using rainbohz::rendering_engine::EngineDb;
using rainbohz::rendering_engine::FrameOutcome;
using rainbohz::rendering_engine::IntegrityCheck;
using rainbohz::rendering_engine::IntegrityReport;
using rainbohz::rendering_engine::loadCypherFile;
using rainbohz::rendering_engine::LoadReport;
using rainbohz::rendering_engine::Mode;
using rainbohz::rendering_engine::modeName;
using rainbohz::rendering_engine::Options;
using rainbohz::rendering_engine::PartialFailure;
using rainbohz::rendering_engine::PartialOutcome;
using rainbohz::rendering_engine::PrecomputationError;
using rainbohz::rendering_engine::PrecomputationReport;
using rainbohz::rendering_engine::RenderingError;
using rainbohz::rendering_engine::RenderingReport;
using rainbohz::rendering_engine::RenderRequest;
using rainbohz::rendering_engine::RenderRequestError;
using rainbohz::rendering_engine::RequestProblem;
using rainbohz::rendering_engine::RowCount;
using rainbohz::rendering_engine::TableEntry;

/// @name Process exit codes
/// @{

/// The run completed: everything loaded and the request checked.
constexpr int kExitOk = 0;

/// Something did not work: a bad request, a statement that did not apply, an
/// integrity violation under --validate-db, a partial that could not be
/// precomputed, an unknown label, a frame that could not be rendered, or an
/// exception that reached main().
constexpr int kExitFailed = 1;

/// The requested mode exists on the command line but has no implementation
/// yet. Distinct from kExitFailed so a caller can tell "not yet" from "broken".
constexpr int kExitNotImplemented = 2;

/// @}

/// Reports statements as the loader applies them, in source order.
///
/// A failure is always reported, with its diagnostic, because a statement that
/// did not run is the thing worth knowing about. Successes are reported only
/// under --verbose. Everything goes to standard output, as it does in
/// scripts/load_schema.py: it is one narrative, and splitting it across two
/// streams only scrambles the order when the output is redirected.
///
/// @param verbose    Whether to report statements that applied or were already
///                   present, as well as those that failed.
/// @param file       Script the statement came from. Only its filename is
///                   printed.
/// @param statement  Statement just executed; supplies the line span and label.
/// @param outcome    What Ladybug made of it. A multi-line diagnostic is printed
///                   one indented line at a time beneath the FAILED line.
void narrateStatement(bool verbose, const std::filesystem::path& file,
    const rainbohz::rendering_engine::CypherStatement& statement,
    const rainbohz::rendering_engine::ExecuteOutcome& outcome) {
    if (outcome.ok || outcome.alreadyExists) {
        if (verbose) {
            std::cout << "    " << (outcome.ok ? "ok      " : "present ") << ' '
                      << file.filename().string() << ", " << statement.lineSpan() << ": "
                      << statement.label() << '\n';
        }
        return;
    }

    std::cout << "    FAILED   " << file.filename().string() << ", " << statement.lineSpan() << ": "
              << statement.label() << '\n';
    std::istringstream detail{outcome.message};
    for (std::string line; std::getline(detail, line);) {
        std::cout << "               " << line << '\n';
    }
}

/// Prints the one-line summary of loading a single Cypher file.
///
/// @param what    Name of the file's role ("schema", "state "), padded by the
///                caller so the summaries line up.
/// @param report  Result of loadCypherFile() for that file.
void reportLoad(std::string_view what, const LoadReport& report) {
    std::cout << "  " << what << ": applied " << report.applied << ", already present "
              << report.alreadyPresent << ", failed " << report.failures.size() << '\n';
}

/// Reports how many NODE and REL tables engine-db declares.
///
/// Other table kinds in the catalog are not counted.
///
/// @param db  Database to inspect. Non-const only because querying it is.
/// @throws EngineDbError  If Ladybug rejects the catalog query.
void reportSchemaCensus(EngineDb& db) {
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
    std::cout << "  engine-db declares " << nodes << " NODE and " << rels << " REL tables.\n";
}

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
/// @param counts  Table names with their row counts, printed in the given order.
/// @param width   Column width for the names, normally from widestName() over
///                every list printed together so that all the counts align.
void reportRows(const std::vector<RowCount>& counts, std::size_t width) {
    for (const RowCount& count : counts) {
        std::cout << "    " << std::left << std::setw(static_cast<int>(width)) << count.first
                  << "  " << count.second << '\n';
    }
}

/// Reports what the database actually holds, table by table.
///
/// This is the only part of the output that distinguishes a loaded engine-db
/// from an empty one. Statement counts say the state file parsed and was
/// accepted; a statement that matched nothing is accepted too, and applies
/// nothing. Row counts are what say the score arrived.
///
/// Prints the totals, then, unless the database is empty, the node tables and
/// the relationship tables as two blocks with their counts in one column.
///
/// @param db  Database to inspect. Non-const only because querying it is.
/// @throws EngineDbError  If Ladybug rejects either scan.
void reportDataCensus(EngineDb& db) {
    const Census census = db.census();

    std::cout << "  engine-db holds " << census.totalNodes << " nodes and "
              << census.totalRelationships << " relationships.\n";
    if (census.empty()) {
        return;
    }

    const std::size_t width = std::max(widestName(census.nodes), widestName(census.relationships));
    std::cout << '\n';
    reportRows(census.nodes, width);
    if (!census.nodes.empty() && !census.relationships.empty()) {
        std::cout << '\n';
    }
    reportRows(census.relationships, width);
}

/// Prints each problem as a FAILED line, in the same layout narrateStatement()
/// uses for a statement that did not apply.
///
/// @param file      File the problems were found in, passed to describeProblem().
/// @param problems  Problems to print, in the given order.
void reportProblems(
    const std::filesystem::path& file, const std::vector<RequestProblem>& problems) {
    for (const RequestProblem& problem : problems) {
        std::cout << "    FAILED   " << describeProblem(file, problem) << '\n';
    }
}

/// Reports the outcome of the integrity checks, one line per violation.
///
/// Every violation a check kept gets a FAILED line, in the layout
/// narrateStatement() uses; a check with more violations than it kept says how
/// many more. A check that passed gets an `ok` line under --verbose only. A
/// closing line gives the totals.
///
/// @param report   Result of checkIntegrity().
/// @param verbose  Whether to list the checks that passed.
void reportIntegrity(const IntegrityReport& report, bool verbose) {
    std::size_t passed = 0;
    for (const IntegrityCheck& check : report.checks) {
        if (check.ok()) {
            ++passed;
            if (verbose) {
                std::cout << "    ok       " << check.name << '\n';
            }
            continue;
        }
        for (const std::string& violation : check.violations) {
            std::cout << "    FAILED   " << check.name << ": " << violation << '\n';
        }
        if (check.violationCount > check.violations.size()) {
            std::cout << "    FAILED   " << check.name << ": and "
                      << check.violationCount - check.violations.size() << " more\n";
        }
    }
    std::cout << "  integrity: " << passed << " of " << report.checks.size() << " check(s) passed, "
              << report.violationCount() << " violation(s)\n";
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

/// Reports the outcome of precomputation, one line per partial.
///
/// A partial that failed always gets a FAILED line with the reason, in the
/// layout narrateStatement() uses. One that succeeded gets an `ok` line with
/// its segment count and frame range under --verbose only, as a statement
/// that applied does. A closing line gives the totals.
///
/// @param report   Result of precomputeAll().
/// @param verbose  Whether to list the partials that succeeded.
void reportPrecomputation(const PrecomputationReport& report, bool verbose) {
    std::cout << "  precomputation: frame size " << report.frameSize.value() << " samples\n";
    for (const PartialOutcome& outcome : report.partials) {
        if (outcome.error.has_value()) {
            std::cout << "    FAILED   " << describePartial(outcome) << ": " << *outcome.error
                      << '\n';
            continue;
        }
        if (verbose && outcome.frames.has_value()) {
            std::cout << "    ok       " << describePartial(outcome) << ": " << outcome.segmentCount
                      << " segment(s), frames " << outcome.frames->first.value() << ".."
                      << outcome.frames->last.value() << ", " << outcome.entryCount
                      << " frame entries\n";
        }
    }
    std::cout << "  precomputed " << report.partials.size() - report.failureCount() << " of "
              << report.partials.size() << " partial(s)\n";
}

/// Names a partial that failed rendering's validation.
///
/// @param failure  The failure.
/// @return Its name followed by its id, as describePartial() formats it.
std::string describeFailedPartial(const PartialFailure& failure) {
    return failure.partialName.value_or("(unnamed)") + " (id " + std::to_string(failure.partialId) +
           ")";
}

/// Reports the outcome of rendering, one line per frame.
///
/// In request order: every partial that first failed validation in a frame
/// gets a FAILED line with its reason, then the frame gets its own line -- an
/// `ok` line with its set and partial counts, peak and file under --verbose
/// only, a FAILED line always. With --per-set-wav, each set's file is an
/// indented `ok` line under its frame's. A closing line gives the totals.
///
/// @param report   Result of renderFrames().
/// @param verbose  Whether to list the frames that rendered.
void reportRendering(const RenderingReport& report, bool verbose) {
    std::cout << "  rendering: sample rate " << report.sampleRate.value() << ", frame size "
              << report.frameSize.value() << " samples, mono\n";
    for (const FrameOutcome& frame : report.frames) {
        for (const PartialFailure& failure : frame.newFailures) {
            std::cout << "    FAILED   " << describeFailedPartial(failure) << " in frame "
                      << failure.frame.value() << ": " << failure.reason << '\n';
        }
        if (const std::optional<std::string>& error = frame.error) {
            std::cout << "    FAILED   frame " << frame.frame.value() << ": " << *error << '\n';
            continue;
        }
        if (!verbose) {
            continue;
        }
        std::cout << "    ok       frame " << frame.frame.value() << ": " << frame.setCount
                  << " set(s), " << frame.partialCount << " partial(s), ";
        if (frame.setCount == 0) {
            std::cout << "silent";
        } else {
            std::cout << "peak " << frame.peak;
        }
        std::cout << ", wrote " << frame.file.filename().string() << '\n';
        for (const auto& [setId, file] : frame.setFiles) {
            std::cout << "    ok         set " << setId << ": wrote " << file.filename().string()
                      << '\n';
        }
    }
    std::cout << "  rendered " << report.frames.size() - report.failureCount() << " of "
              << report.frames.size() << " frame(s)";
    if (report.failedPartialCount() > 0) {
        std::cout << "; " << report.failedPartialCount() << " partial(s) failed validation";
    }
    std::cout << '\n';
}

/// Lists the frames a request asks for, and what selects each one's partials.
///
/// @param request  A request that has already been loaded and validated.
void reportRequest(const RenderRequest& request) {
    std::cout << "  request: " << request.frames.size() << " frame(s)\n";
    for (const auto& frame : request.frames) {
        std::cout << "    frame " << std::left << std::setw(10) << frame.frameNumber;
        if (frame.allPartials()) {
            std::cout << "all partials\n";
            continue;
        }
        std::cout << "labels ";
        const char* separator = "";
        for (const std::string& label : frame.labels) {
            std::cout << separator << label;
            separator = ", ";
        }
        std::cout << '\n';
    }
}

/// Runs the `test` mode, as far as it is implemented.
///
/// In order: loads the render request; applies the schema file, then the state
/// file, to a fresh engine-db; and, only if every statement applied, checks
/// the database's integrity if --validate-db was given, precomputes every
/// partial, checks the request's labels against those in the database, and
/// renders the requested frames into --out-dir, creating it if absent. What
/// the database holds is reported after loading fails, or after
/// precomputation, so that the census includes its output. Progress goes to
/// standard output and the closing verdict on a failure to standard error.
///
/// @param options  Validated command-line options, with `mode` equal to
///                 Mode::Test.
/// @return kExitOk if everything loaded, every partial was precomputed, every
///         requested label exists and every requested frame was rendered;
///         kExitFailed if the request is unusable, any statement did not
///         apply, an integrity check failed under --validate-db,
///         precomputation could not run or any partial failed, any requested
///         label is unknown, the output directory cannot be created,
///         rendering could not run, or any frame failed.
/// @throws EngineDbError    If engine-db cannot be opened or inspected.
/// @throws ScriptLoadError  If the schema or state file cannot be read.
/// @note Side effect: creates `options.outputDir` and writes WAV files into
///       it.
int runTestMode(const Options& options) {
    EngineDb db{options.dbPath};

    std::cout << "rendering-engine, mode " << modeName(options.mode) << '\n'
              << "  engine-db: " << db.path() << '\n'
              << "  schema:    " << options.schemaFile.string() << '\n'
              << "  state:     " << options.stateFile.string() << '\n'
              << "  request:   " << options.requestFile.string() << '\n'
              << "  out-dir:   " << options.outputDir.string() << '\n'
              << '\n';

    // The request is read first: its shape needs nothing from the database,
    // and a malformed request is not worth loading a score for.
    RenderRequest request;
    try {
        request = rainbohz::rendering_engine::loadRenderRequest(options.requestFile);
    } catch (const RenderRequestError& e) {
        reportProblems(e.file(), e.problems());
        std::cerr << "\nrendering-engine: the render request is not usable.\n";
        return kExitFailed;
    }

    const rainbohz::rendering_engine::StatementObserver observe =
        [verbose = options.verbose](const std::filesystem::path& file,
            const rainbohz::rendering_engine::CypherStatement& statement,
            const rainbohz::rendering_engine::ExecuteOutcome& outcome) {
            narrateStatement(verbose, file, statement, outcome);
        };

    LoadReport total;

    const LoadReport schema = loadCypherFile(db, options.schemaFile, observe);
    reportLoad("schema", schema);
    total += schema;

    // The state file is applied even when the schema did not load whole: the
    // errors it then produces say which tables are missing, which is more use
    // than stopping at the first problem.
    const LoadReport state = loadCypherFile(db, options.stateFile, observe);
    reportLoad("state ", state);
    total += state;

    std::cout << '\n';
    reportSchemaCensus(db);

    if (!total.ok()) {
        reportDataCensus(db);
        std::cerr << '\n'
                  << "rendering-engine: " << total.failures.size()
                  << " statement(s) did not apply. engine-db is partially loaded.\n";
        return kExitFailed;
    }

    // Integrity is checked only on a score that loaded whole: on a partial
    // one, the violations say more about the load than about the data. A
    // failure stops the run before anything computes from the data.
    if (options.validateDb) {
        std::cout << '\n';
        const IntegrityReport integrity = rainbohz::rendering_engine::checkIntegrity(db);
        reportIntegrity(integrity, options.verbose);
        if (!integrity.ok()) {
            std::cerr << "\nrendering-engine: " << integrity.violationCount()
                      << " integrity violation(s) in engine-db. Precomputation was not run.\n";
            return kExitFailed;
        }
    }

    // Precomputation runs only on a score that loaded whole, for the same
    // reason the labels are checked only against one.
    std::cout << '\n';
    PrecomputationReport precomputation;
    try {
        precomputation = rainbohz::rendering_engine::precomputeAll(db);
    } catch (const PrecomputationError& e) {
        reportDataCensus(db);
        std::cerr << "\nrendering-engine: precomputation could not run: " << e.what() << '\n';
        return kExitFailed;
    }
    reportPrecomputation(precomputation, options.verbose);
    std::cout << '\n';
    reportDataCensus(db);

    if (!precomputation.ok()) {
        std::cerr << "\nrendering-engine: " << precomputation.failureCount()
                  << " partial(s) could not be precomputed.\n";
        return kExitFailed;
    }

    // Labels are checked only against a score that loaded whole: against a
    // partial one, a missing label says more about the load than the request.
    std::cout << '\n';
    reportRequest(request);
    const std::vector<RequestProblem> unknown =
        rainbohz::rendering_engine::findUnknownLabels(request, db.labelNames());
    if (!unknown.empty()) {
        reportProblems(request.file, unknown);
        std::cerr << "\nrendering-engine: " << unknown.size()
                  << " requested label(s) are not in engine-db.\n";
        return kExitFailed;
    }

    // The output directory is created only now, once there is something to
    // write: a run that fails before this point leaves no empty directory
    // behind.
    std::error_code error;
    std::filesystem::create_directories(options.outputDir, error);
    if (error) {
        std::cerr << "\nrendering-engine: cannot create --out-dir " << options.outputDir.string()
                  << ": " << error.message() << '\n';
        return kExitFailed;
    }

    std::cout << '\n';
    RenderingReport rendering;
    try {
        rendering = rainbohz::rendering_engine::renderFrames(
            db, request, options.outputDir, options.perSetWav);
    } catch (const RenderingError& e) {
        std::cerr << "\nrendering-engine: rendering could not run: " << e.what() << '\n';
        return kExitFailed;
    }
    reportRendering(rendering, options.verbose);

    if (!rendering.ok()) {
        std::cerr << "\nrendering-engine: " << rendering.failureCount()
                  << " frame(s) could not be rendered. The other frames' files remain.\n";
        return kExitFailed;
    }

    std::cout << "\nLoaded, precomputed, and rendered " << rendering.frames.size()
              << " frame(s) to " << options.outputDir.string()
              << ". Transformation is not implemented, and labels do not yet select partials.\n";
    return kExitOk;
}

/// Reports that a run mode has no implementation yet.
///
/// @param mode  The mode that was asked for.
/// @return kExitNotImplemented, always.
int notImplemented(Mode mode) {
    std::cerr << "rendering-engine: mode '" << modeName(mode)
              << "' is not implemented.\n"
                 "                  Only 'test' is available; see "
                 "rendering-engine/docs/design.md section 3.\n";
    return kExitNotImplemented;
}

} // namespace

/// Parses the command line and dispatches to the requested run mode.
///
/// Any exception that escapes a mode is reported on standard error as a
/// single line and turned into kExitFailed, so the process never ends by
/// std::terminate().
///
/// @param argc  Argument count, as passed by the runtime.
/// @param argv  Argument vector, as passed by the runtime.
/// @return The run mode's exit code; or, when the command line did not yield
///         options (--help, --version, a usage error), the code
///         parseCommandLine() chose.
int main(int argc, char** argv) {
    try {
        const auto parsed = rainbohz::rendering_engine::parseCommandLine(argc, argv);
        if (!parsed.options.has_value()) {
            return parsed.exitCode;
        }
        const Options& options = *parsed.options;

        switch (options.mode) {
        case Mode::Test:
            return runTestMode(options);
        case Mode::Distributed:
        case Mode::Standalone:
            return notImplemented(options.mode);
        }
        return kExitFailed;
    } catch (const std::exception& e) {
        std::cerr << "rendering-engine: " << e.what() << '\n';
        return kExitFailed;
    }
}
