// SPDX-License-Identifier: Apache-2.0

#include "rainbohz/rendering_engine/score_pipeline.hpp"

#include "rainbohz/rendering_engine/integrity.hpp"
#include "rainbohz/rendering_engine/ladybug_store.hpp"
#include "rainbohz/rendering_engine/native_table_loader.hpp"
#include "rainbohz/rendering_engine/precomputation.hpp"
#include "rainbohz/rendering_engine/report.hpp"
#include "rainbohz/rendering_engine/script_loader.hpp"
#include "rainbohz/rendering_engine/table_loader.hpp"
#include "rainbohz/rendering_engine/transform/transform.hpp"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

/// @file
/// @brief Runs the load and render operations step by step.
///
/// Implements loadScore() and renderRequestedFrames(). The steps, their order
/// and their narrative are those batch mode has always had; they moved here
/// from main.cpp so that server mode runs exactly the same ones.
///
/// @see score_pipeline.hpp

namespace rainbohz::rendering_engine {

namespace {

/// Called once the transform has succeeded; returns a result to stop the
/// load with, or nullopt to go on.
using AfterTransform = std::function<std::optional<LoadResult>()>;

/// Runs the steps every backend shares once its score has loaded: the
/// transform, then `afterTransform`, then precomputation, reporting what
/// engine-db holds when a step fails and after precomputation.
///
/// @param[in,out] store  engine-db holding the loaded score. Modified by the
///                       transform and precomputation.
/// @param settings       verbose is used.
/// @param log            Where the narrative goes.
/// @param afterTransform Called after a successful transform; may stop the
///                       load. Empty for none.
/// @return The step that stopped the load, with its verdict, or Complete.
/// @throws EngineDbError  If engine-db cannot be inspected.
LoadResult runStages(EngineStore& store, const PipelineSettings& settings, std::ostream& log,
    const AfterTransform& afterTransform) {
    // The transform runs first: it writes the physical timelines that
    // precomputation reads. A set or partial it cannot convert stops the
    // load, as an integrity violation does, before precomputation reports the
    // same partials as failures for want of their timelines.
    log << '\n';
    transform::TransformReport transformed;
    try {
        transformed = transform::transformAll(store.transformStore());
    } catch (const transform::TransformError& e) {
        reportDataCensus(log, store);
        return {.stage = LoadStage::Transform,
            .verdict = std::string{"the transform could not run: "} + e.what()};
    }
    reportTransform(log, transformed, settings.verbose);
    if (!transformed.ok()) {
        log << '\n';
        reportDataCensus(log, store);
        return {.stage = LoadStage::Transform,
            .verdict = std::to_string(transformed.failureCount()) +
                       " partial set(s), partial(s) or SoundSource(s) could not be transformed. "
                       "Precomputation was not run."};
    }

    if (afterTransform) {
        if (std::optional<LoadResult> stop = afterTransform()) {
            return std::move(*stop);
        }
    }

    // Precomputation runs only on a score that loaded whole, for the same
    // reason the labels are checked only against one.
    log << '\n';
    PrecomputationReport precomputation;
    try {
        precomputation = precomputeAll(store.precomputationStore());
    } catch (const PrecomputationError& e) {
        reportDataCensus(log, store);
        return {.stage = LoadStage::Precomputation,
            .verdict = std::string{"precomputation could not run: "} + e.what()};
    }
    reportPrecomputation(log, precomputation, settings.verbose);
    log << '\n';
    reportDataCensus(log, store);

    if (!precomputation.ok()) {
        return {.stage = LoadStage::Precomputation,
            .verdict = std::to_string(precomputation.failureCount()) +
                       " partial(s) could not be precomputed."};
    }
    return {};
}

} // namespace

BackendChoice backendFor(const PipelineSettings& settings, const std::filesystem::path& stateFile) {
    const bool cypher = stateFile.extension() != ".json";
    if (!settings.backend.has_value()) {
        return {.backend = cypher || settings.validateDb ? Backend::Ladybug : Backend::Native,
            .refusal = std::nullopt};
    }
    if (*settings.backend == Backend::Native) {
        if (settings.validateDb) {
            return {.backend = Backend::Native,
                .refusal = "--validate-db needs --backend ladybug: the integrity checks are "
                           "Cypher queries, which the native backend does not run"};
        }
        if (cypher) {
            return {.backend = Backend::Native,
                .refusal = stateFile.string() +
                           " is not a table manifest; the native backend reads hzlc's tables "
                           "only. Load a .cypher score with --backend ladybug"};
        }
    }
    return {.backend = *settings.backend, .refusal = std::nullopt};
}

LoadResult loadScore(EngineDb& db, const std::filesystem::path& schemaFile,
    const std::filesystem::path& stateFile, const PipelineSettings& settings, std::ostream& log) {
    LadybugStore store{db};
    const StatementObserver observe =
        [&log, verbose = settings.verbose](const std::filesystem::path& file,
            const CypherStatement& statement, const ExecuteOutcome& outcome) {
            narrateStatement(log, verbose, file, statement, outcome);
        };

    LoadReport total;

    const LoadReport schema = loadCypherFile(db, schemaFile, observe);
    reportLoad(log, "schema", schema);
    total += schema;

    // The state file is applied even when the schema did not load whole: the
    // errors it then produces say which tables are missing, which is more use
    // than stopping at the first problem.
    const LoadReport state = loadStateFile(db, stateFile, observe);
    reportLoad(log, "state ", state);
    total += state;

    log << '\n';
    reportSchemaCensus(log, db);

    if (!total.ok()) {
        reportDataCensus(log, store);
        return {.stage = LoadStage::Statements,
            .verdict = std::to_string(total.failures.size()) +
                       " statement(s) did not apply. engine-db is partially loaded."};
    }

    // Integrity is checked only on a score that loaded whole: on a partial
    // one, the violations say more about the load than about the data. A
    // failure stops the load before anything computes from the data.
    // The canonical pass checks what the transform reads; the physical pass,
    // after the transform, checks what it wrote (docs/integrity.md section 2).
    if (settings.validateDb) {
        log << '\n';
        const IntegrityReport integrity = checkCanonicalIntegrity(db);
        reportIntegrity(log, integrity, settings.verbose);
        if (!integrity.ok()) {
            return {.stage = LoadStage::Integrity,
                .verdict = std::to_string(integrity.violationCount()) +
                           " integrity violation(s) in engine-db's canonical model. The "
                           "transform was not run."};
        }
    }

    AfterTransform physicalIntegrity;
    if (settings.validateDb) {
        physicalIntegrity = [&]() -> std::optional<LoadResult> {
            log << '\n';
            const IntegrityReport integrity = checkPhysicalIntegrity(db);
            reportIntegrity(log, integrity, settings.verbose);
            if (!integrity.ok()) {
                return LoadResult{.stage = LoadStage::Integrity,
                    .verdict = std::to_string(integrity.violationCount()) +
                               " integrity violation(s) in engine-db's physical timelines. "
                               "Precomputation was not run."};
            }
            return std::nullopt;
        };
    }
    return runStages(store, settings, log, physicalIntegrity);
}

NativeScoreLoad loadNativeScore(
    const std::filesystem::path& stateFile, const PipelineSettings& settings, std::ostream& log) {
    native::NativeLoad load = native::loadNativeScore(stateFile);
    log << "  state : read " << load.nodeTables << " node table(s) and " << load.relationshipPairs
        << " relationship pair(s), " << load.rows << " row(s), into the native engine-db\n";
    NativeScoreLoad result{
        .result = {}, .store = std::make_unique<NativeStore>(std::move(load.model))};
    result.result = runStages(*result.store, settings, log, {});
    return result;
}

RenderResult renderRequestedFrames(EngineStore& store, const RenderRequest& request,
    const std::filesystem::path& outputDir, const PipelineSettings& settings, std::ostream& log) {
    // Labels are checked only against a score that loaded whole: against a
    // partial one, a missing label says more about the load than the request.
    log << '\n';
    reportRequest(log, request);
    const std::vector<RequestProblem> unknown = findUnknownLabels(request, store.labelNames());
    if (!unknown.empty()) {
        reportProblems(log, request.file, unknown);
        return {.stage = RenderStage::UnknownLabels,
            .verdict = std::to_string(unknown.size()) + " requested label(s) are not in engine-db.",
            .report = {}};
    }

    // The output directory is created only now, once there is something to
    // write: a run that fails before this point leaves no empty directory
    // behind.
    std::error_code error;
    std::filesystem::create_directories(outputDir, error);
    if (error) {
        return {.stage = RenderStage::OutputDirectory,
            .verdict = "cannot create --out-dir " + outputDir.string() + ": " + error.message(),
            .report = {}};
    }

    log << '\n';
    RenderResult result;
    try {
        result.report = renderFrames(store.frameSource(), request, outputDir, settings.perSetWav,
            settings.renderThreads, settings.renderKernel);
    } catch (const RenderingError& e) {
        result.stage = RenderStage::Rendering;
        result.verdict = std::string{"rendering could not run: "} + e.what();
        return result;
    }
    reportRendering(log, result.report, settings.verbose);

    if (!result.report.ok()) {
        result.stage = RenderStage::Rendering;
        result.verdict = std::to_string(result.report.failureCount()) +
                         " frame(s) could not be rendered. The other frames' files remain.";
    }
    return result;
}

} // namespace rainbohz::rendering_engine
