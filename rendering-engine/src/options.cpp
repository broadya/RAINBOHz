#include "rainbohz/rendering_engine/options.hpp"

#include "rainbohz/rendering_engine/engine_db.hpp"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <system_error>

/// @file
/// @brief Parses the rendering-engine command line into Options.
///
/// Implements parseCommandLine() and modeName() with CLI11, which is confined
/// to this file so that no third-party type reaches options.hpp. Mode names
/// match case-insensitively. `distributed` is recognised but not implemented.
/// Each mode's required options, and the options it refuses, are enforced
/// here, so that main() receives a complete Options or an exit code. The default schema file
/// is the in-repo engine-db schema, whose path CMake injects as
/// RE_DEFAULT_SCHEMA_FILE.
///
/// Help text and diagnostics for a bad command line go to stdout and stderr
/// from here; everything else is reported by main().
///
/// @see options.hpp

// The in-repo schema file, injected by CMake. See CMakeLists.txt.
#ifndef RE_DEFAULT_SCHEMA_FILE
#define RE_DEFAULT_SCHEMA_FILE ""
#endif

namespace rainbohz::rendering_engine {

namespace {

constexpr std::string_view kDefaultSchemaFile = RE_DEFAULT_SCHEMA_FILE;

/// Returns the table of command-line mode names, the single source for both
/// the lookup and the list of names shown in a diagnostic.
///
/// A function-local static rather than a namespace-scope map, so it is built
/// on first use and cannot be caught by static initialisation order.
///
/// @return Lower-case mode name to Mode, for every mode. Valid for the whole
///         program.
const std::map<std::string, Mode>& modesByName() {
    static const std::map<std::string, Mode> kModes{
        {"batch", Mode::Batch}, {"server", Mode::Server}, {"distributed", Mode::Distributed}};
    return kModes;
}

/// Returns `text` in lower case, so that mode names match case-insensitively.
///
/// Byte-wise and locale-dependent through `std::tolower`, which is enough for
/// the ASCII mode names; it is not a general Unicode case fold.
///
/// @param text  Text to convert. Taken by value and converted in place.
/// @return `text` with every byte passed through `std::tolower`.
std::string lowerCase(std::string text) {
    std::ranges::transform(text, text.begin(), [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    });
    return text;
}

/// Returns every accepted mode name as a comma-separated list, for the
/// unknown-mode diagnostic.
///
/// @return The names from modesByName() in alphabetical order (the map's
///         order), separated by ", ".
std::string knownModes() {
    std::string names;
    for (const auto& [name, mode] : modesByName()) {
        if (!names.empty()) {
            names += ", ";
        }
        names += name;
    }
    return names;
}

/// Checks that a mode's required option was given, and reports it if not.
///
/// @param option  The option, as CLI11 registered it.
/// @param mode    The mode's name, for the message.
/// @param value   What the option takes, e.g. `<file.cypher>`.
/// @param why     What the option is for, in a few words.
/// @return True when the option was given.
/// @note Side effect: writes the diagnostic to stderr when it returns false.
bool requireOption(const CLI::Option* option, std::string_view mode, std::string_view value,
    std::string_view why) {
    if (option->count() > 0) {
        return true;
    }
    std::cerr << "rendering-engine: " << mode << " mode needs " << option->get_name() << ' '
              << value << "\n"
              << "                  (" << why << ")\n";
    return false;
}

/// Checks that an option a mode does not use was not given, and reports it if
/// it was. An option that is accepted and then ignored misleads whoever
/// passed it.
///
/// @param option  The option, as CLI11 registered it.
/// @param mode    The mode's name, for the message.
/// @param why     Why the mode does not use it, in a few words.
/// @return True when the option was not given.
/// @note Side effect: writes the diagnostic to stderr when it returns false.
bool refuseOption(const CLI::Option* option, std::string_view mode, std::string_view why) {
    if (option->count() == 0) {
        return true;
    }
    std::cerr << "rendering-engine: " << option->get_name() << " is not used in " << mode
              << " mode (" << why << ").\n";
    return false;
}

} // namespace

std::string_view modeName(Mode mode) noexcept {
    switch (mode) {
    case Mode::Batch:
        return "batch";
    case Mode::Server:
        return "server";
    case Mode::Distributed:
        return "distributed";
    }
    return "unknown";
}

ParseResult parseCommandLine(int argc, const char* const* argv) {
    CLI::App app{"RAINBOHz rendering engine: hosts engine-db and computes from it."};
    app.set_version_flag(
        "--version", std::string{"rendering-engine (ladybug "} + EngineDb::ladybugVersion() + ")");
    Options options;
    options.schemaFile = std::filesystem::path{kDefaultSchemaFile};

    // The mode is taken as a string and mapped afterwards rather than through
    // CLI11's Transformer, neither variant of which does what is wanted here:
    // Transformer passes an unmatched value through unchanged, and it then
    // converts to the enum by its underlying integer, so `--mode 1` silently
    // means `distributed`; CheckedTransformer rejects unknown names but
    // accepts those same integers deliberately. Neither should be a command
    // line anyone can write, and both would outlive a renumbering of the enum.
    std::string modeName;
    app.add_option("-m,--mode", modeName, "Run mode [batch]")
        ->option_text("batch|server|distributed");

    // Schema and state are one file each, named separately. The engine does
    // not read a directory: which files a run applies, and in what order, is
    // something the command line says outright rather than something a
    // directory listing decides.
    app.add_option("--schema", options.schemaFile, "Cypher file of schema DDL to apply")
        ->capture_default_str()
        ->check(CLI::ExistingFile);

    const CLI::Option* state = app.add_option("-s,--state", options.stateFile,
                                      "Cypher file holding the initial database state (batch mode)")
                                   ->check(CLI::ExistingFile);

    const CLI::Option* request = app.add_option("-r,--request", options.requestFile,
                                        "YAML file of frames and labels to render (batch mode)")
                                     ->check(CLI::ExistingFile);

    const CLI::Option* outDir = app.add_option("-o,--out-dir", options.outputDir,
        "Directory for rendered WAV files, created if absent "
        "[batch: current directory; server: required]");

    const CLI::Option* dbPath = app.add_option("--db-path", options.dbPath,
        "Open engine-db on disk at this path instead of in memory (batch mode). "
        "A debugging affordance, not the deployed configuration.");

    // The server's options have no defaults, deliberately: which interface a
    // server is exposed on, and which part of the filesystem it may read, are
    // always said outright.
    const CLI::Option* host = app.add_option(
        "--host", options.host, "Address of the interface to listen on (server mode, required)");

    const CLI::Option* port =
        app.add_option("--port", options.port,
               "TCP port to listen on; 0 picks a free one (server mode, required)")
            ->check(CLI::Range(0, 65535));

    const CLI::Option* inputRoot =
        app.add_option("--input-root", options.inputRoot,
               "Directory every score load must read from (server mode, required)")
            ->check(CLI::ExistingDirectory);

    app.add_flag("-v,--verbose", options.verbose, "Report every statement, not just failures");

    // Named for what it validates: the command line and the render request
    // are always validated, so a bare --validate would not say which.
    app.add_flag("--validate-db", options.validateDb,
        "Check engine-db's integrity after loading; stop before precomputation if it fails");

    app.add_flag("--per-set-wav", options.perSetWav,
        "Also write each partial set's contribution to a frame as its own WAV file");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        return ParseResult{.options = std::nullopt, .exitCode = app.exit(e)};
    }

    if (!modeName.empty()) {
        const auto found = modesByName().find(lowerCase(modeName));
        if (found == modesByName().end()) {
            std::cerr << "rendering-engine: unknown mode '" << modeName
                      << "'. Known modes: " << knownModes() << ".\n";
            return ParseResult{.options = std::nullopt, .exitCode = 1};
        }
        options.mode = found->second;
    }

    // Mode-specific requirements. CLI11 can express these as needs/excludes,
    // but stating them here keeps the message in the engine's own words.
    if (options.mode == Mode::Batch) {
        if (!requireOption(state, "batch", "<file.cypher>", "the initial database state to load") ||
            !requireOption(request, "batch", "<file.yaml>", "the frames and labels to render") ||
            !refuseOption(host, "batch", "it is for server mode") ||
            !refuseOption(port, "batch", "it is for server mode") ||
            !refuseOption(inputRoot, "batch", "it is for server mode")) {
            return ParseResult{.options = std::nullopt, .exitCode = 1};
        }
    }

    if (options.mode == Mode::Server) {
        if (!requireOption(host, "server", "<address>", "the interface to listen on") ||
            !requireOption(port, "server", "<port>", "the TCP port to listen on") ||
            !requireOption(inputRoot, "server", "<dir>", "where PUT /v1/score may read from") ||
            !requireOption(outDir, "server", "<dir>", "where rendered frames are written") ||
            !refuseOption(state, "server", "a score arrives by PUT /v1/score") ||
            !refuseOption(
                request, "server", "frames are asked for by POST /v1/frames/{frame}/renderings") ||
            !refuseOption(
                dbPath, "server", "engine-db is in memory, replaced by every PUT /v1/score")) {
            return ParseResult{.options = std::nullopt, .exitCode = 1};
        }
    }

    // An existing file where the output directory should be is caught now,
    // rather than after the score has loaded.
    std::error_code error;
    if (std::filesystem::exists(options.outputDir, error) &&
        !std::filesystem::is_directory(options.outputDir, error)) {
        std::cerr << "rendering-engine: --out-dir " << options.outputDir.string()
                  << " exists and is not a directory.\n";
        return ParseResult{.options = std::nullopt, .exitCode = 1};
    }

    if (options.schemaFile.empty()) {
        std::cerr << "rendering-engine: no schema file. Pass --schema <file.cypher>.\n";
        return ParseResult{.options = std::nullopt, .exitCode = 1};
    }

    return ParseResult{.options = options, .exitCode = 0};
}

} // namespace rainbohz::rendering_engine
